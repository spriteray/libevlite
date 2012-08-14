
#include <stdio.h>
#include <stdint.h>
#include <signal.h>

#include "threads.h"

uint8_t runflags;

void task_method( uint8_t index, evsets_t sets, int16_t type, void * task )
{
	printf("Thread[%d]: recv a task \n", index);	
}

void signal_handler( int32_t signo )
{
	printf("receive a quit signal \n");	
	runflags = 0;

	return ;
}

int32_t main()
{
	uint32_t index = 0;
	uint8_t nthreads = 4;

	iothreads_t threadgroup;

	signal( SIGINT, signal_handler );
	threadgroup = iothreads_create( nthreads, task_method );
	if ( threadgroup == NULL )
	{
		printf("iothreads_create() failed \n");
		return -1;
	}
	
	runflags = 1;
	iothreads_start( threadgroup );

	while ( runflags )
	{
		uint8_t _index = index&(nthreads-1);
		iothreads_post( threadgroup, _index, 0, NULL, 0 );
		++index;
	}

	iothreads_stop( threadgroup );
	iothreads_destroy( threadgroup );

	return 0;
}

