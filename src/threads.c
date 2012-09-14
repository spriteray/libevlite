
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include "utils.h"
#include "threads.h"
#include "threads-internal.h"

// 创建网络线程组
// nthreads		- 网络线程组中的线程数
// method		- 任务处理函数
iothreads_t iothreads_create( uint8_t nthreads, 
					void (*method)(void *, uint8_t, int16_t, void *), void * context )
{
	struct iothreads * iothreads = NULL;

	iothreads = calloc( 1, sizeof(struct iothreads) );
	if ( iothreads )
	{
		iothreads->method 	= method;
		iothreads->context	= context;
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
		iothread_start( iothreads->threadgroup+i, i, self );
	}

	return 0;
}

pthread_t iothreads_get_id( iothreads_t self, uint8_t index )
{
	struct iothreads * iothreads = (struct iothreads *)(self);

	if ( index >= iothreads->nthreads )
	{
		return 0;
	}

	return iothreads->threadgroup[index].id;
}

evsets_t iothreads_get_sets( iothreads_t self, uint8_t index )
{
	struct iothreads * iothreads = (struct iothreads *)(self);

	if ( index >= iothreads->nthreads )
	{
		return 0;
	}

	return iothreads->threadgroup[index].sets;
}

// 向网络线程组中指定的线程提交任务
// index	- 指定网络线程的编号
// type		- 提交的任务类型
// task		- 提交的任务数据
// size		- 任务数据的长度, 默认设置为0
int32_t iothreads_post( iothreads_t self, uint8_t index, int16_t type, void * task, uint8_t size )
{
	int16_t intype = eTaskType_Data;
	struct iothreads * iothreads = (struct iothreads *)(self);

	if ( index >= iothreads->nthreads )
	{
		return -1;
	}

	if ( iothreads->runflags != 1 )
	{
		return -2;
	}

	if ( size > TASK_PADDING_SIZE )
	{
		intype = eTaskType_Null;	
	}
	else if ( size == 0 )
	{
		intype = eTaskType_User;
	}

	if ( intype != eTaskType_Null )
	{
		return iothread_post( iothreads->threadgroup+index, intype, type, task, size );
	}

	return -3;
}

void iothreads_stop( iothreads_t self )
{
	uint8_t i = 0;
	struct iothreads * iothreads = (struct iothreads *)(self);

	// 向所有线程发送停止命令
	iothreads->runflags = 0;
	for ( i = 0; i < iothreads->nthreads; ++i )
	{
		iothread_post( iothreads->threadgroup+i, eTaskType_Null, 0, NULL, 0 );
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

void iothreads_destroy( iothreads_t self )
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

static void * iothread_main( void * arg );
static void iothread_on_command( int32_t fd, int16_t ev, void * arg );

int32_t iothread_start( struct iothread * self, uint8_t index, iothreads_t parent )
{
	self->index = index;
	self->parent = parent;

	self->sets = evsets_create();
	if ( self->sets == NULL )
	{
		iothread_stop(self);
		return -1;
	}

	self->cmdevent = event_create();
	self->queue = msgqueue_create( MSGQUEUE_DEFAULT_SIZE );
	if ( self->queue == NULL || self->cmdevent == NULL )
	{
		iothread_stop(self);
		return -2;
	}

	// 初始化命令事件
	event_set( self->cmdevent, msgqueue_popfd(self->queue), EV_READ|EV_PERSIST );
	event_set_callback( self->cmdevent, iothread_on_command, self );
	evsets_add( self->sets, self->cmdevent, 0 );

	// 启动线程
	pthread_attr_t attr;
	pthread_attr_init( &attr );
//	assert( pthread_attr_setstacksize( &attr, THREAD_DEFAULT_STACK_SIZE ) );
	pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_DETACHED );

	int32_t rc = pthread_create(&(self->id), &attr, iothread_main, self);
	pthread_attr_destroy( &attr );
	
	if ( rc != 0 )
	{
		iothread_stop(self);
		return -3;
	}

	return 0;
}

int32_t iothread_post( struct iothread * self, int16_t type, int16_t utype, void * task, uint8_t size )
{
	struct task intask;

	intask.type		= type;
	intask.utype	= utype;
	if ( size > 0 )
	{
		memcpy( &(intask.data), task, size );
	}
	else
	{
		intask.task = task;
	}

	// 默认: 提交任务不提醒消费者
	return msgqueue_push( self->queue, &intask, POST_IOTASK_AND_NOTIFY );
}

int32_t iothread_stop( struct iothread * self )
{
	if ( self->queue )
	{
		msgqueue_destroy( self->queue );
		self->queue = NULL;
	}

	if ( self->cmdevent )
	{
		evsets_del( self->sets, self->cmdevent );
		event_destroy( self->cmdevent );
		self->cmdevent = NULL;
	}

	if ( self->sets )
	{
		evsets_destroy( self->sets );
		self->sets = NULL;
	}

	return 0;
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
		do
		{
			ntasks = msgqueue_pops( thread->queue, tasks, POP_TASKS_COUNT );
			for ( i = 0; i < ntasks; ++i )
			{
				void * data = NULL;

				switch ( tasks[i].type )
				{
				case eTaskType_Null :
					{
						// 空命令
						continue;
					}
					break;

				case eTaskType_User :
					{
						// 用户命令
						data = tasks[i].task;
					}
					break;

				case eTaskType_Data :
					{
						// 数据命令
						data = (void *)(tasks[i].data);
					}
					break;
				}

				// 回调
				parent->method( parent->context, thread->index, tasks[i].utype, data );
			}
		}
		while ( ntasks == POP_TASKS_COUNT );
	}

	// 向主线程发送终止信号
	pthread_mutex_lock( &(parent->lock) );
	--parent->nrunthreads;
	pthread_cond_signal( &(parent->cond) );
	pthread_mutex_unlock( &(parent->lock) );
	
	return NULL;
}

void iothread_on_command( int32_t fd, int16_t ev, void * arg )
{
	//struct iothread * iothread = (struct iothread *)arg;

	if ( ev & EV_READ )
	{	
		char buf[ 64 ];		
		int32_t nread = 0;
		
		nread = read( fd, buf, sizeof(buf) );
	}

	return;
}

