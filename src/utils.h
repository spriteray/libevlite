
#ifndef SRC_UTILS_H
#define SRC_UTILS_H

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * 工具算法模块
 */

#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/uio.h>

#include "queue.h"
#include "network.h"

//
// 系统相关的操作
//

// 时间函数, 返回毫秒数
int64_t mtime();

// socket基本操作
int32_t is_connected( int32_t fd );
int32_t set_non_block( int32_t fd );
int32_t tcp_connect( char * host, uint16_t port, int8_t isasyn );
int32_t tcp_accept( int32_t fd, char * remotehost, uint16_t * remoteport );
int32_t tcp_listen( char * host, uint16_t port, void (*options)(int32_t) );

//
// 基础算法类
//
uint32_t getpower( uint32_t size );
uint32_t nextpow2( uint32_t size );

/*
 * sidlist 
 */
struct sidlist
{
	uint32_t	count;
	uint32_t	size;

	sid_t *		entries;
};

struct sidlist * sidlist_create( uint32_t size );
#define sidlist_count( self )	( (self)->count )
sid_t sidlist_get( struct sidlist * self, int32_t index );
int32_t sidlist_add( struct sidlist * self, sid_t id );
sid_t sidlist_del( struct sidlist * self, int32_t index );
void sidlist_destroy( struct sidlist * self );

// 任务类型
enum
{
	eTaskType_Null		= 0,	// 空任务
	eTaskType_User		= 1,	// 用户任务
	eTaskType_Data		= 2,	// 数据任务
};

// 任务填充长度
#define TASK_PADDING_SIZE		56	

// 任务数据
struct task
{
	int16_t type;				// 2bytes
	int16_t utype;				// 2bytes
	union
	{
		void *	taskdata;			 
		char	data[TASK_PADDING_SIZE];
	};
};

QUEUE_PADDING_HEAD(taskqueue, struct task);
QUEUE_PROTOTYPE(taskqueue, struct task)

/* 
 * 消息队列
 * 线程安全的消息队列, 有通知的功能
 */
struct msgqueue
{
	struct taskqueue queue;
	int32_t popfd;
	int32_t pushfd;

	pthread_mutex_t lock; 
};

// 创建消息队列
struct msgqueue * msgqueue_create( uint32_t size );

// 生产者发送任务
// isnotify - 是否需要通知消费者
int32_t msgqueue_push( struct msgqueue * self, struct task * task, uint8_t isnotify );

// 消费者从消息队列中取一定量的任务
int32_t msgqueue_pop( struct msgqueue * self, struct task * task );

// 交换
int32_t msgqueue_swap( struct msgqueue * self, struct taskqueue * queue );

// 消息队列的长度
uint32_t msgqueue_count( struct msgqueue * self );

// 消费者管道fd
int32_t msgqueue_popfd( struct msgqueue * self );

// 销毁消息队列
int32_t msgqueue_destroy( struct msgqueue * self );

#ifdef __cplusplus
}
#endif

#endif

