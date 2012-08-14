
#include "threads.h"
#include "threads-internal.h"

// 创建网络线程组
// nthreads		- 网络线程组中的线程数
// method		- 任务处理函数
iothreads_t iothreads_create( uint8_t nthreads, void (*method)(uint8_t, evsets_t, int32_t, void *) )
{
	struct iothreads * iothreads = NULL;

	iothreads = calloc( 1, sizeof(struct iothreads) );
	if ( iothreads )
	{
		iothreads->method = method;
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

	if ( index >= iothreads->nthreads )
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
	assert( pthread_attr_setstacksize( &attr, THREAD_DEFAULT_STACK_SIZE ) );
	pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_DETACHED );

	int32_t rc = pthread_create(&(iothread->id), &attr, iothread_main, iothread);
	pthread_attr_destroy( &attr );
	
	if ( rc != 0 )
	{
		iothread_stop(self);
		return -3;
	}

	return 0;
}

int32_t iothread_post( struct iothread * self, int32_t type, void * task )
{
	struct task intask;
	intask.type = type;
	intask.data = task;

	return msgqueue_push( self->queue, &intask );
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
		ntasks = msgqueue_pops( thread->queue, &tasks, POP_TASKS_COUNT );
		for ( i = 0; i < ntasks; ++i )
		{
			if ( tasks[i].type == X_THREAD_STOPCMD )
			{
				continue;
			}
			
			parent->method( thread->index, thread->sets, tasks[i].type, tasks[i].data);
		}
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
	struct iothread * iothread = (struct iothread *)arg;

	if ( ev & EV_READ )
	{	
		char buf[ 512 ];		
		int32_t nread = 0;

		while ( 1 )
		{
			nread = read( fd, buf, sizeof(buf) );
			if ( nread <= 0 )
			{
				break;
			}
		}
	}

	return;
}

