
#ifndef SRC_UTILS_H
#define SRC_UTILS_H

#include <stdint.h>
#include <pthread.h>

//
// 
//
inline int64_t mtime();

//
// 基础算法类
//
uint32_t nextpow2( uint32_t size );

// 任务填充长度
# define TASK_PADDING_SIZE		56	

// 任务类型
enum
{
	eTaskType_Null		= 0,	// 空任务
	eTaskType_User		= 1,	// 用户任务
	eTaskType_Data		= 2,	// 数据任务
};

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

// 队列
struct queue
{
	uint32_t size;					// (4+4)bytes
	struct task * entries;			// 8bytes
	uint32_t padding1[12];			// 48,padding

	uint32_t head;					// 4bytes
	uint32_t padding2[15];			// 60bytes 

	uint32_t tail;					// 4bytes
	uint32_t padding3[15];			// 60bytes 
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

	uint8_t isnotify;
	
	int32_t popfd;
	int32_t pushfd;

	pthread_spinlock_t lock; 
};

struct msgqueue * msgqueue_create( uint32_t size, uint8_t is_notify );
int32_t msgqueue_push( struct msgqueue * self, struct task * task );
int32_t msgqueue_pop( struct msgqueue * self, struct task * task );
int32_t msgqueue_pops( struct msgqueue * self, struct task * tasks, uint32_t count );
uint32_t msgqueue_count( struct msgqueue * self );
int32_t msgqueue_popfd( struct msgqueue * self );
int32_t msgqueue_destroy( struct msgqueue * self );

#endif

