
#include "tinyos.h"

#include "ringbuf.c"

#include <assert.h>
#include "kernel_cc.h"
#include "kernel_dev.h"
#include "kernel_sched.h"
#include "kernel_streams.h"
#include "kernel_proc.h"

#include "tinyoslib.h"


#include "util.h"

static file_ops pipe_reader_fops = {
  .Open = pipe_open,    //return NULL
  .Read = pipe_read,
  .Write = pipe_dont_write, //return -1
  .Close = pipe_close_reader
};

static file_ops pipe_writer_fops = {
  .Open = pipe_open,  //return NULL
  .Read = pipe_dont_read,   //return -1
  .Write = pipe_write,
  .Close = pipe_close_writer 
}; 


int Pipe(pipe_t* pipe)
{
	//reserve space for pipe
	//acquire 2 fcbs
	//fcb obj = same for both
	//fcb1 func->reader file_ops reader
	//fcb0 func->writer file_ops writer
	/*
	 * allocate space for pipe struct
	 */

	 pipe_t* pipeptr;
	 pipeptr = &pipe;
	if( pipe->exist_state != ALIVE){
			pipeptr = (pipe_t *)xmalloc(PIPE_SIZE);
			//assert(pipe !=NULL);
			if(pipe == NULL) {
				fprintf(stderr, "%s\n","failed to create pipe! xmalloc " );
				return -1;
			}

			/* initialize pipe values */
			pipe->spinlock = MUTEX_INIT;
			pipe->pipe_has_stuff_to_read = COND_INIT;
		  	pipe->pipe_has_space_to_write = COND_INIT;
		  	pipe->buffer = ringbuf_new(8192); //size of buffer in bytes = 8KB
		  	pipe->exist_state = ALIVE; //the pipe has been created
		  	pipe->read = PIPE_NULL_FID;  
			pipe->write = PIPE_NULL_FID;
		  	//assert(pipe->buffer != 0);
		  	if(& pipe->buffer == 0) {
		  		fprintf(stderr, "%s\n", "failed to create buffer...." );
		  		return -1;
		  	}
  	}
	Fid_t fid[2];
	FCB* fcb[2];
	/*
	 * reserve two FCBs if available
	 */
	/* Since FCB_reserve allocates fids in increasing order,
	   we expect pair[0]==0 and pair[1]==1 */
	if(FCB_reserve(2, fid, fcb)==0)// || fid[0]!=0 || fid[1]!=1)
	{
		printf("Failed to allocate console Fids\n");
		return -1;
		abort();
	}
	//they both point to the same object
	fcb[0]->streamobj = pipe;
	fcb[1]->streamobj = pipe;

	//fcb[0] corresponds to reader
	//fcb[1] corresponds to writer
	fcb[0]->streamfunc = & pipe_reader_fops; 
	fcb[1]->streamfunc = & pipe_writer_fops;

	//fprintf(stderr, "%s %d\n", "writer's streamfunc write == ", fcb[1]->streamfunc->Write );

	pipe->read = fid[0];  
	pipe->write = fid[1];
	return 0;
}



/*
  Read from the pipe (device), sleeping if needed.
 */
int pipe_read(void* dev, char *buf, unsigned int size){
		
  pipe_t* pipe_cb = (pipe_t*)dev;
  //preempt_off;            /* Stop preemption */
  Mutex_Lock(& pipe_cb->spinlock);
  uint count =  0;


  if (ringbuf_is_empty(pipe_cb->buffer)){ 
  	if (pipe_cb->write == PIPE_NULL_FID){ //if writer is dead
  		Mutex_Unlock(& pipe_cb->spinlock);
  		//preempt_on;           /* Restart preemption */
  		return 0;// EOF; //aka -1 ->error
  	} 
  	else //if writer is still alive, wait until there is something to read
  		Cond_Wait(&pipe_cb->spinlock, &pipe_cb->pipe_has_stuff_to_read); 
  }
  while(count<size) { 

    int valid = ringbuf_memcpy_from((buf+count), pipe_cb->buffer, 1); //serial write
	

    if (valid){ //if we successfully read something
      count++;
      //wake up writer
      Cond_Broadcast(&pipe_cb->pipe_has_space_to_write);
    }
    else{ //if the buffer is empty
    	if (pipe_cb->write == PIPE_NULL_FID){ //if writer is dead
  			Mutex_Unlock(& pipe_cb->spinlock);
  			//preempt_on;           /* Restart preemption */
  			return count; //0; //EOF;
  		} 
  		else //if writer is still alive, wait until there is something to read
  			Cond_Wait(&pipe_cb->spinlock, &pipe_cb->pipe_has_stuff_to_read);
  	}	
  }

  Mutex_Unlock(& pipe_cb->spinlock);
  //preempt_on;           /* Restart preemption */
  return count;
}

/* 
  Write call 
  returns -1 when FAIL-> reader has closed
*/
int pipe_write(void* dev, const char* buf, unsigned int size)
{
  pipe_t* pipe_cb = (pipe_t*)dev;
   		//fprintf(stderr, "%s\n", "Entering funcop 'write'" );

  //preempt_off;            /* Stop preemption */
  Mutex_Lock(& pipe_cb->spinlock);
  unsigned int count = 0;
  while(count < size) {
  	if (pipe_cb->read == PIPE_NULL_FID){ //reader has closed, all hope is lost....
  		Mutex_Unlock(& pipe_cb->spinlock);
 		//preempt_on;           /* Restart preemption */
 		//fprintf(stderr, "%s\n", "leaving 'write', read was NULL, return -1" );
  		return -1;
  	}
  	else{
		//printf("Is there space to read? Bytes free: %d - PIPE_WRITE\n", ringbuf_bytes_free(pipe_cb->buffer) );
  		if(ringbuf_bytes_free(pipe_cb->buffer)>0){//&* ???????
  			//there is some space to write
  				//printf("I am about to write buf[count]: %c - PIPE_WRITE\n", buf[count] );
  			int success = ringbuf_memset(pipe_cb->buffer, buf[count],  1); //serial write
  			
  			assert(success >0);
  			count++;
  			//wake up reader  			
     		Cond_Broadcast(&pipe_cb->pipe_has_stuff_to_read);
  		}
  		else{
  			//wait until reader frees us some space
  			Cond_Wait(&pipe_cb->spinlock, &pipe_cb->pipe_has_space_to_write); ///????
  		}

  	}

  	
  }
  Mutex_Unlock(& pipe_cb->spinlock);
  //preempt_on;           /* Restart preemption */

  return count;
  
  
}

void* pipe_open(uint minor)
{
  return NULL;
}

int pipe_close_reader(void* dev) 
{
	pipe_t* pipe_cb = (pipe_t*)dev;
		
	Mutex_Lock(& pipe_cb->spinlock);
		pipe_cb->read = PIPE_NULL_FID;
		//wake up writer
		Cond_Broadcast(&pipe_cb->pipe_has_space_to_write);

	
  	Mutex_Unlock(& pipe_cb->spinlock);
  	//clean up pipe space, destroy everything
  	//if(pipe_cb->read == NULL && pipe_cb->write == NULL ){
	// 		free(pipe_cb);

  	//}


  return 0;
}

int pipe_close_writer(void* dev) 
{
	pipe_t* pipe_cb = (pipe_t*)dev;

	Mutex_Lock(& pipe_cb->spinlock);
		pipe_cb->write = PIPE_NULL_FID;
		//wake up reader
    	Cond_Broadcast(&pipe_cb->pipe_has_stuff_to_read);
  	Mutex_Unlock(& pipe_cb->spinlock);
  	//clean up pipe space, destroy everything
  	//if(pipe_cb->read == NULL && pipe_cb->write == NULL ){
  	//	free(pipe_cb);

  	//}

  return 0;
}

int pipe_dont_write(void* dev, const char* buf, unsigned int size){
	return -1;

}

int pipe_dont_read(void* dev, const char* buf, unsigned int size){
	return -1;
}




