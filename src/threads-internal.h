
#ifndef THREADS_INTERNAL_H
#define THREADS_INTERNAL_H

#include "threads.h"

// 停止命令
#define X_THREAD_STOPCMD	0xff

// 一次从队列中批量获取任务
#define POP_TASKS_COUNT		128


//
// 网络线程
// 

struct iothread
{
	uint8_t 	index;
	
	evsets_t 	sets;
	void *		parent;

	struct msgqueue * queue;
};

int32_t iothread_start( struct iothread * self );
int32_t iothread_post( struct iothread * self, int32_t type, void * task );
int32_t iothread_stop( struct iothread * self );

//
// 网络线程组
//

struct iothreads
{
	struct iothread * threadgroup;
	void (*onTask)( uint8_t, evsets_t, int32_t, void *);	
	
	uint8_t nthreads;
	uint8_t runflags;

	uint8_t nrunthreads;
	pthread_cond_t cond;
	pthread_mutex_t lock;
};


#endif

