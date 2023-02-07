
#ifndef THREADS_INTERNAL_H
#define THREADS_INTERNAL_H

#include "threads.h"
#include "network-internal.h"

// 队列默认大小
// 这个选项需要测试期间不断调整以适应场景的需要
#define MSGQUEUE_DEFAULT_SIZE       4096

// 线程默认栈大小
#define THREAD_DEFAULT_STACK_SIZE   (8*1024)

//
// 网络线程
//

STAILQ_HEAD( acceptorlist, acceptor );
STAILQ_HEAD( connectorlist, connector );
STAILQ_HEAD( associaterlist, associater );

struct iothread
{
    uint8_t             index;
    pthread_t           id;

    evsets_t            sets;
    void *              parent;
    void *              context;    // 上下文

    event_t             cmdevent;
    struct msgqueue *   queue;
    char                padding[ 8 ];

    // 回收列表
    struct acceptorlist acceptorlist;
    struct connectorlist connectorlist;
    struct associaterlist associaterlist;
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
    struct iothread * threads;

    // 任务处理器
    void * context;
    processor_t processor;

    uint8_t nthreads;
    uint8_t runflags;
    int32_t precision;      // 时间精度
    uint8_t immediately;    // 是否立刻通知IO线程

    uint8_t nrunthreads;
    pthread_cond_t cond;
    pthread_mutex_t lock;
};

int8_t iothreads_get_index( iothreads_t self );
struct acceptorlist * iothreads_get_acceptlist( iothreads_t self, uint8_t index );
struct connectorlist * iothreads_get_connectlist( iothreads_t self, uint8_t index );
struct associaterlist * iothreads_get_associatelist( iothreads_t self, uint8_t index );

#endif
