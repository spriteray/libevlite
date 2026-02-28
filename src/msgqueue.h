
#ifndef MSGQUEUE_H
#define MSGQUEUE_H

#include <stdint.h>

#include "lock.h"
#include "queue.h"

//
// 消息队列
//

// 任务类型
enum {
    eTaskType_Null = 0, // 空任务
    eTaskType_User = 1, // 用户任务
    eTaskType_Data = 2, // 数据任务
};

// 任务填充长度
#if __SIZEOF_POINTER__ == 4
    #define TASK_PADDING_SIZE 60
#elif __SIZEOF_POINTER__ == 8
    #define TASK_PADDING_SIZE 56
#else
    #error "No way to define bits"
#endif

// 任务数据
struct task {
    int16_t type;  // 2bytes
    int16_t utype; // 2bytes
    union {
        void * taskdata;
        char data[TASK_PADDING_SIZE];
    };
};

QUEUE_PADDING_HEAD( taskqueue, struct task );
QUEUE_PROTOTYPE( taskqueue, struct task )

//
// 消息队列
// 线程安全的消息队列, 有通知的功能
//
struct msgqueue {
    struct taskqueue queue;
    int32_t popfd;
    int32_t pushfd;

    struct evlock lock;
};

// 创建消息队列
struct msgqueue * msgqueue_create( uint32_t size );

// 生产者发送任务
// isnotify - 是否需要通知消费者
int32_t msgqueue_push( struct msgqueue * self, struct task * task );

// 消费者从消息队列中取一定量的任务
int32_t msgqueue_pop( struct msgqueue * self, struct task * task );
int32_t msgqueue_pops( struct msgqueue * self, struct task * tasks, uint32_t count );

// 交换
int32_t msgqueue_swap( struct msgqueue * self, struct taskqueue * queue );

// 消息队列的长度
uint32_t msgqueue_count( struct msgqueue * self );

// 消费者管道fd
int32_t msgqueue_popfd( struct msgqueue * self );

// 销毁消息队列
int32_t msgqueue_destroy( struct msgqueue * self );

#endif
