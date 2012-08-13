
#include "threads.h"
#include "threads-internal.h"

// 创建网络线程组
// nthreads		- 网络线程组中的线程数
// cb			- 任务处理函数
iothreads_t iothreads_create( uint8_t nthreads, void (*cb)(uint8_t, evsets_t, int32_t, void *) )
{
	struct iothreads * iothreads = NULL;

	iothreads = calloc( 1, sizeof(struct iothreads) );
	if ( iothreads )
	{
		iothreads->onTask = cb;
		iothreads->nrunthreads = 0;
		iothreads->nthreads = nthreads;

		iothreads->threadgroup = calloc( nthreads, sizeof(struct iothread) );
		if ( iothreads->threadgroup )
		{
			pthread_cond_init( &(iothreads->cond), NULL );
			pthread_mutex_init( &(iothreads->lock), NULL );
		}
		else
		{
			free( iothreads );
			iothreads = NULL;
		}
	}

	return iothreads;
}

// 网络线程组开始运行
int32_t iothreads_start( iothreads_t self )
{
	uint8_t i = 0;
	struct iothreads * iothreads = (struct iothreads *)(self);

	iothreads->runflags = 1;
	iothreads->nrunthreads = iothreads->nthreads;

	// 开启网络线程
	for ( i = 0; i < iothreads->nthreads; ++i )
	{
		iothread_start( iothreads->threadgroup + i );
	}

	return 0;
}

// 向网络线程组中指定的线程提交任务
// index	- 指定网络线程的编号
// type		- 提交的任务类型, NOTE:0xff内置的任务类型
// task		- 提交的任务数据
int32_t iothreads_post( iothreads_t self, uint8_t index, int32_t type, void * task )
{
	struct iothreads * iothreads = (struct iothreads *)(self);

	if ( index > iothreads->nthreads )
	{
		return -1;
	}

	if ( type == X_THREAD_STOPCMD )
	{
		return -2;
	}

	return iothread_post( iothreads->threadgroup+index, type, task );
}

void iothreads_stop( iothreads_t self )
{
	uint8_t i = 0;
	struct iothreads * iothreads = (struct iothreads *)(self);

	// 向所有线程发送停止命令
	iothreads->runflags = 0;
	for ( i = 0; i < iothreads->nthreads; ++i )
	{
		iothread_post( iothreads->threadgroup+i, X_THREAD_STOPCMD, NULL );
	}

	// 等待线程退出
	pthread_mutex_lock( &(iothreads->lock) );
	while ( iothreads->nrunthreads > 0 )
	{
		pthread_cond_wait( &(iothreads->cond), &(iothreads->lock) );
	}
	pthread_mutex_unlock( &(iothreads->lock) );

	// 销毁所有网络线程
	for ( i = 0; i < iothreads->nthreads; ++i )
	{
		iothread_stop( iothreads->threadgroup + i );
	}

	return;
}

void iothread_destroy( iothreads_t self )
{
	struct iothreads * iothreads = (struct iothreads *)(self);

	pthread_cond_destroy( &(iothreads->cond) );
	pthread_mutex_destroy( &(iothreads->lock) );

	if ( iothreads->threadgroup )
	{
		free( iothreads->threadgroup );
		iothreads->threadgroup = NULL;
	}

	free ( iothreads );
	return ;
}

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

int32_t iothread_start( struct iothread * self )
{

}

int32_t iothread_post( struct iothread * self, int32_t type, void * task )
{

}

int32_t iothread_stop( struct iothread * self )
{

}

void * iothread_main( void * arg )
{
	struct iothread * thread = (struct iothread *)arg;
	struct iothreads * parent = (struct iothreads *)(thread->parent);

	int32_t i = 0;
	int32_t ntasks = 0;
	struct task tasks[ POP_TASKS_COUNT ];

	while ( parent->runflags )
	{
		// 轮询网络事件
		evsets_dispatch( thread->sets );

		// 处理任务
		ntasks = msgqueue_pops( thread->queue, &tasks, POP_TASKS_COUNT );
		for ( i = 0; i < ntasks; ++i )
		{
			if ( tasks[i].type == X_THREAD_STOPCMD )
			{
				continue;
			}
			parent->onTask( thread->index, thread->sets, tasks[i].type, tasks[i].data);
		}
	}

	// 向主线程发送终止信号
	pthread_mutex_lock( &(parent->lock) );
	--parent->nrunthreads;
	pthread_cond_signal( &(parent->cond) );
	pthread_mutex_unlock( &(parent->lock) );
	
	return NULL;
}

