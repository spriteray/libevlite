
#ifndef SRC_UTILS_H
#define SRC_UTILS_H

#include <stdint.h>

//
// 
//
inline int64_t mtime();


//
// 基础算法类
//
uint32_t nextpow2( uint32_t size );

struct task
{
	int32_t type;
	void * 	data;
};

// 队列
struct queue
{
	uint32_t size;
	struct task * entries;

	uint32_t padding1[13];
	uint32_t head;
	
	uint32_t padding2[16];
	uint32_t tail;
};

struct queue * queue_create( uint32_t size );
int32_t queue_push( struct queue * self, struct task * task );
int32_t queue_pop( struct queue * self, struct task * task );
int32_t queue_pops( struct queue * self, struct task * tasks, uint32_t count );
inline uint32_t queue_count( struct queue * self );
int32_t queue_destroy( struct queue * self );

// 消息队列
struct msgqueue
{
	struct queue * queue;

	int32_t popfd;
	int32_t pushfd;

	pthread_mutex_t lock; 
};

struct msgqueue * msgqueue_create( uint32_t size );
int32_t msgqueue_push( struct msgqueue * self, struct task * task );
int32_t msgqueue_pop( struct msgqueue * self, struct task * task );
int32_t msgqueue_pops( struct msgqueue * self, struct task * tasks, uint32_t count );
uint32_t msgqueue_count( struct msgqueue * self );
int32_t msgqueue_popfd( struct msgqueue * self );
int32_t msgqueue_destroy( struct msgqueue * self );

#endif

