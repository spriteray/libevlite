
#ifndef THREADS_INTERNAL_H
#define THREADS_INTERNAL_H

#include "threads.h"

// 一次从队列中批量获取任务
#define POP_TASKS_COUNT				128

// 队列默认大小
#define MSGQUEUE_DEFAULT_SIZE		1024

// 线程默认栈大小
#define THREAD_DEFAULT_STACK_SIZE	(8*1024)

//
// 网络线程
// 

struct iothread
{
	uint8_t 	index;
	pthread_t	id;
	
	evsets_t 	sets;
	void *		parent;

	event_t	 	cmdevent;
	struct msgqueue * queue;
};

int32_t iothread_start( struct iothread * self, uint8_t index, iothreads_t parent );
int32_t iothread_post( struct iothread * self, 
						int16_t type, int16_t utype, void * task, uint8_t size );
int32_t iothread_stop( struct iothread * self );

//
// 网络线程组
//

struct iothreads
{
	struct iothread * threadgroup;

	void * context;
	void (*method)( void *, uint8_t, int16_t, void *);	
	
	uint8_t nthreads;
	uint8_t runflags;

	uint8_t nrunthreads;
	pthread_cond_t cond;
	pthread_mutex_t lock;
};


#endif

