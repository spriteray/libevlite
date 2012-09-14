
#include <stdio.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>

#include "utils.h"
#include "threads.h"

uint8_t runflags;

struct iothread_args
{
	uint64_t	taskcount;
	uint64_t 	padding[7];
};

void task_method( void * context, uint8_t index, int16_t type, void * task )
{
	struct iothread_args * arg = (struct iothread_args *)task;

	// TODO: ÈÎÎñ
	++arg->taskcount;
	
	if ( rand()%10 == 0 )
	{
		//usleep(2);
	}

	return;
}

void signal_handler( int32_t signo )
{
	printf("receive a quit signal \n");	
	runflags = 0;

	return ;
}

int32_t main()
{
	uint64_t index = 0;

	uint8_t i = 0;
	uint8_t nthreads = 4;

	iothreads_t threadgroup;
	uint64_t start_time = 0, end_time = 0;	

	struct iothread_args * args = NULL;
	
	srand( (int32_t)time(NULL) );
	signal( SIGINT, signal_handler );

	//
	threadgroup = iothreads_create( nthreads, task_method, NULL );
	if ( threadgroup == NULL )
	{
		printf("iothreads_create() failed \n");
		return -1;
	}

	args = (struct iothread_args *)calloc( nthreads, sizeof(struct iothread_args) );
	if ( args == NULL )
	{
		printf("calloc for 'iothread_args' failed \n");
		return -2;
	}
	
	//	
	runflags = 1;
	iothreads_start( threadgroup );
	
	//
	start_time = mtime();
	while ( runflags )
	{
		uint8_t _index = index&(nthreads-1);
		struct iothread_args * _args = args + _index;
		
		iothreads_post( threadgroup, _index, 0, _args, 0 );
		++index;
	}
	end_time = mtime();

	//
	iothreads_stop( threadgroup );
	iothreads_destroy( threadgroup );
	
	//
	for ( i = 0; i < nthreads; ++i )
	{
		struct iothread_args * _args = args + i;
		float rate = (float)(_args->taskcount)*1000.0f/(float)(end_time-start_time);
		
		printf("iothread[%d] process tasks: %ld, rate: %6.3f\n", i, _args->taskcount, rate );
	}
	
	printf("dispatch tasks: %ld, rate: %6.3f\n", index, (float)(index)*1000.0f/(float)(end_time-start_time) );

	return 0;
}

