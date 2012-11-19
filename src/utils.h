
#ifndef SRC_UTILS_H
#define SRC_UTILS_H

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

#include "network.h"

//
// 系统相关的操作
//

// 时间函数, 返回毫秒数
inline int64_t mtime();

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
 *
 */ 
struct arraylist
{
    uint32_t count;
    uint32_t size;

    void ** entries;
};

struct arraylist * arraylist_create( uint32_t size );
int32_t arraylist_init( struct arraylist * self, uint32_t size );
uint32_t arraylist_count( struct arraylist * self );
void arraylist_reset( struct arraylist * self );
void arraylist_final( struct arraylist * self );
int32_t arraylist_append( struct arraylist * self, void * data );
void * arraylist_get( struct arraylist * self, int32_t index );
void * arraylist_take( struct arraylist * self, int32_t index );
int32_t arraylist_destroy( struct arraylist * self );

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
		void *	task;			 
		char	data[TASK_PADDING_SIZE];
	};
};

/*
 * 队列
 * 简单的队列算法, 需要扩展时, 有一定量的数据拷贝
 * 但是, 在长度可控的情况下, 足够高效和简单
 */
struct queue
{
	uint32_t size;					// (4+4)bytes
	struct task * entries;			// 8bytes
	uint32_t padding1[12];			// 48,padding

	uint32_t head;					// 4bytes, cacheline padding
	uint32_t padding2[15];			// 60bytes 

	uint32_t tail;					// 4bytes, cacheline padding
	uint32_t padding3[15];			// 60bytes 
};

// TODO: 分段存储，扩展时节省了内存拷贝

// 创建队列
// size - 性能方面的考虑, 确保size足够大
struct queue * queue_create( uint32_t size );

// 向队列中提交任务
int32_t queue_push( struct queue * self, struct task * task );

// 从队列中取出一定量的任务
int32_t queue_pop( struct queue * self, struct task * task );
int32_t queue_pops( struct queue * self, struct task * tasks, uint32_t count );

// 队列长度
inline uint32_t queue_count( struct queue * self );

// 销毁队列
int32_t queue_destroy( struct queue * self );

/* 
 * 消息队列
 * 线程安全的消息队列, 有通知的功能
 */
struct msgqueue
{
	struct queue * queue;

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
int32_t msgqueue_pops( struct msgqueue * self, struct task * tasks, uint32_t count );

// 消息队列的长度
uint32_t msgqueue_count( struct msgqueue * self );

// 消费者管道fd
int32_t msgqueue_popfd( struct msgqueue * self );

// 销毁消息队列
int32_t msgqueue_destroy( struct msgqueue * self );

#endif

