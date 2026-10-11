
#ifndef THREADS_INTERNAL_H
#define THREADS_INTERNAL_H

#include "network.h"
#include "threads.h"
#include "msgqueue.h"
#include "network-internal.h"

// 队列默认大小
// 这个选项需要测试期间不断调整以适应场景的需要
#define MSGQUEUE_DEFAULT_SIZE 4096

// 线程默认栈大小
#define THREAD_DEFAULT_STACK_SIZE ( 8 * 1024 )

//
// 网络线程
//

struct iothread;
struct scheduler {
    int32_t interval;
    void * task;
    event_t event;
    taskscheduler_t perform;
    struct iothread * thread;
    LIST_ENTRY( scheduler ) linker;
};

struct session_manager;
LIST_HEAD( schedulerlist, scheduler );
LIST_HEAD( acceptorlist, acceptor );
LIST_HEAD( connectorlist, connector );
LIST_HEAD( associaterlist, associater );

// 64字节对齐，消除伪共享
struct iothread {
    uint8_t index;
    pthread_t id;

    evsets_t sets;
    void * parent;
    void * context; // 上下文
    struct session_manager * manager;

    event_t cmdevent;
    struct msgqueue * queue;

    // 定时调度
    struct schedulerlist schedulerlist;

    // 回收列表
    struct acceptorlist acceptorlist;
    struct connectorlist connectorlist;
    struct associaterlist associaterlist;
}__attribute__((aligned(64)));

int32_t iothread_start( struct iothread * self,
    uint8_t index, uint32_t nclients, iothreads_t parent );

int32_t iothread_post( struct iothread * self,
    int16_t type, int16_t utype, void * task, uint8_t size );
int32_t iothread_schedule( struct iothread * self,
    void * task, taskscheduler_t execute, int32_t interval );
void iothread_cancel( struct iothread * self, struct scheduler * s, uint8_t flag );

int32_t iothread_stop( struct iothread * self );

//
// 网络线程组
//

struct iothreads {
    struct iothread * threads;

    // 任务处理器
    void * context;
    processor_t processor;

    uint8_t nthreads;
    uint8_t runflags;
    int32_t precision;   // 时间精度

    uint8_t nrunthreads;
    pthread_cond_t cond;
    pthread_mutex_t lock;
};

int8_t iothreads_get_index( iothreads_t self );
struct iothread * iothreads_get( iothreads_t self, uint8_t index );
struct acceptorlist * iothreads_get_acceptlist( iothreads_t self, uint8_t index );
struct connectorlist * iothreads_get_connectlist( iothreads_t self, uint8_t index );
struct associaterlist * iothreads_get_associatelist( iothreads_t self, uint8_t index );

#endif
