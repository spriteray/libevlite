
#include <stdio.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>

#include "utils.h"
#include "threads.h"

uint8_t runflags;

void task_method( uint8_t index, evsets_t sets, int16_t type, void * task )
{
	if ( rand()%10 == 0 )
	{
		usleep(2);
	}
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
	uint8_t nthreads = 4;

	iothreads_t threadgroup;
	uint64_t start_time = 0, end_time = 0;	
	
	srand( (int32_t)time(NULL) );
	signal( SIGINT, signal_handler );

	//
	threadgroup = iothreads_create( nthreads, task_method );
	if ( threadgroup == NULL )
	{
		printf("iothreads_create() failed \n");
		return -1;
	}
	
	//	
	runflags = 1;
	iothreads_start( threadgroup );
	
	//
	start_time = mtime();
	while ( runflags )
	{
		uint8_t _index = index&(nthreads-1);
		iothreads_post( threadgroup, _index, 0, NULL, 0 );
		++index;
	}
	end_time = mtime();

	//
	iothreads_stop( threadgroup );
	iothreads_destroy( threadgroup );
	
	//
	printf("dispatch tasks: %ld, rate: %6.3f\n", index, (float)index*1000.0f/(float)(end_time-start_time));

	return 0;
}

