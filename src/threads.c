
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <signal.h>

#include <sys/types.h>
#include <sys/uio.h>
#include <syslog.h>
#include <unistd.h>

#include "utils.h"
#include "threads.h"
#include "threads-internal.h"

// 创建网络线程组
// nthreads     - 网络线程组中的线程数
// method       - 任务处理函数
iothreads_t iothreads_start( uint8_t nthreads, uint8_t immediately,
        void (*method)(void *, uint8_t, int16_t, void *), void * context )
{
    uint8_t i = 0;
    struct iothreads * iothreads = NULL;

    iothreads = calloc( 1, sizeof(struct iothreads) );
    if ( iothreads == NULL )
    {
        return NULL;
    }

    iothreads->threadgroup = calloc( nthreads, sizeof(struct iothread) );
    if ( iothreads->threadgroup == NULL )
    {
        free( iothreads );
        return NULL;
    }

    iothreads->method   = method;
    iothreads->immediately = immediately;
    iothreads->context  = context;
    iothreads->nthreads = nthreads;
    pthread_cond_init( &iothreads->cond, NULL );
    pthread_mutex_init( &iothreads->lock, NULL );

    // 开启网络线程
    iothreads->runflags = 1;
    iothreads->nrunthreads = nthreads;
    for ( i = 0; i < nthreads; ++i )
    {
        iothread_start( iothreads->threadgroup+i, i, iothreads );
    }

    return iothreads;
}

pthread_t iothreads_get_id( iothreads_t self, uint8_t index )
{
    struct iothreads * iothreads = (struct iothreads *)(self);

    assert( iothreads != NULL );
    assert( index < iothreads->nthreads );
    assert( iothreads->threadgroup != NULL );

    return iothreads->threadgroup[index].id;
}

evsets_t iothreads_get_sets( iothreads_t self, uint8_t index )
{
    struct iothreads * iothreads = (struct iothreads *)(self);

    assert( iothreads != NULL );
    assert( index < iothreads->nthreads );
    assert( iothreads->threadgroup != NULL );

    return iothreads->threadgroup[index].sets;
}

void * iothreads_get_context( iothreads_t self, uint8_t index )
{
    struct iothreads * iothreads = (struct iothreads *)(self);

    assert( iothreads != NULL );
    assert( index < iothreads->nthreads );
    assert( iothreads->threadgroup != NULL );

    return iothreads->threadgroup[index].context;
}

void iothreads_set_context( iothreads_t self, uint8_t index, void * context )
{
    struct iothreads * iothreads = (struct iothreads *)(self);

    assert( iothreads != NULL );
    assert( index < iothreads->nthreads );
    assert( iothreads->threadgroup != NULL );

    iothreads->threadgroup[index].context = context;
}

// 向网络线程组中指定的线程提交任务
// index    - 指定网络线程的编号
// type     - 提交的任务类型
// task     - 提交的任务数据
// size     - 任务数据的长度, 默认设置为0
int32_t iothreads_post( iothreads_t self, uint8_t index, int16_t type, void * task, uint8_t size )
{
    struct iothreads * iothreads = (struct iothreads *)(self);

    assert( size <= TASK_PADDING_SIZE );
    assert( index < iothreads->nthreads );

    if ( unlikely( iothreads->runflags != 1 ) )
    {
        return -1;
    }

    return iothread_post( iothreads->threadgroup+index, (size == 0 ? eTaskType_User : eTaskType_Data), type, task, size );
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
    pthread_mutex_lock( &iothreads->lock );
    while ( iothreads->nrunthreads > 0 )
    {
        pthread_cond_wait( &iothreads->cond, &iothreads->lock );
    }
    pthread_mutex_unlock( &iothreads->lock );

    // 销毁所有网络线程
    for ( i = 0; i < iothreads->nthreads; ++i )
    {
        iothread_stop( iothreads->threadgroup + i );
    }

    pthread_cond_destroy( &iothreads->cond );
    pthread_mutex_destroy( &iothreads->lock );
    if ( iothreads->threadgroup )
    {
        free( iothreads->threadgroup );
        iothreads->threadgroup = NULL;
    }

    free ( iothreads );
}

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

static void * iothread_main( void * arg );
static void iothread_on_command( int32_t fd, int16_t ev, void * arg );
static inline uint32_t _process(
        struct iothreads * parent, struct iothread * thread, struct taskqueue * doqueue );

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
    //    assert( pthread_attr_setstacksize( &attr, THREAD_DEFAULT_STACK_SIZE ) );
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
    struct task inter_task = { .type=type, .utype=utype };
    struct iothreads * threads = (struct iothreads *)self->parent;

    if ( size == 0 )
    {
        inter_task.taskdata = task;
    }
    else
    {
        memcpy( &(inter_task.data), task, size );
    }

    // 默认: 提交任务不提醒消费者
    return msgqueue_push( self->queue, &inter_task, threads->immediately );
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

uint32_t _process( struct iothreads * parent, struct iothread * thread, struct taskqueue * doqueue )
{
    uint32_t nprocess = 0;

    // 交换任务队列
    msgqueue_swap( thread->queue, doqueue );

    // 获取最大任务数
    nprocess = QUEUE_COUNT(taskqueue)(doqueue);

    // 处理任务
    while ( QUEUE_COUNT(taskqueue)(doqueue) > 0 )
    {
        struct task task;
        void * data = NULL;

        QUEUE_POP(taskqueue)( doqueue, &task );
        switch ( task.type )
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
                    data = task.taskdata;
                }
                break;

            case eTaskType_Data :
                {
                    // 数据命令
                    data = (void *)(task.data);
                }
                break;
        }

        // 回调
        parent->method( parent->context, thread->index, task.utype, data );
    }

    return nprocess;
}

void * iothread_main( void * arg )
{
    uint32_t maxtasks = 0;

    struct iothread * thread = (struct iothread *)arg;
    struct iothreads * parent = (struct iothreads *)(thread->parent);

    sigset_t mask;
    sigfillset(&mask);
    pthread_sigmask(SIG_SETMASK, &mask, NULL);

    // 初始化队列
    struct taskqueue doqueue;
    QUEUE_INIT(taskqueue)( &doqueue, MSGQUEUE_DEFAULT_SIZE );

    while ( parent->runflags )
    {
        uint32_t nprocess = 0;

        // 轮询网络事件
        evsets_dispatch( thread->sets );

        // 处理事件
        nprocess = _process( parent, thread, &doqueue );

        // 最大任务数
        maxtasks = MAX( maxtasks, nprocess );
    }

    // 清理队列中剩余数据
    _process( parent, thread, &doqueue );
    // 清空队列
    QUEUE_CLEAR(taskqueue)( &doqueue );

    // 日志
    syslog( LOG_INFO, "%s(INDEX=%d) : the Maximum Number of Requests is %d in EachFrame .",
            __FUNCTION__, thread->index, maxtasks );

    // 向主线程发送终止信号
    pthread_mutex_lock( &parent->lock );
    --parent->nrunthreads;
    pthread_cond_signal( &parent->cond );
    pthread_mutex_unlock( &parent->lock );

    return NULL;
}

void iothread_on_command( int32_t fd, int16_t ev, void * arg )
{
    struct iothread * iothread = (struct iothread *)arg;

    if ( ev & EV_READ )
    {
        uint64_t one = 1;

        if ( sizeof( one )
                != read( fd, &one, sizeof(one) ) )
        {
            // 出错了
            syslog( LOG_WARNING,
                    "%s(INDEX=%d) : read from Pipe(fd:%u) error .", __FUNCTION__, iothread->index, fd );
        }
    }
}
