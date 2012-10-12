
#ifndef THREADS_H
#define THREADS_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include <pthread.h>

#include "event.h"

// 网络线程组
typedef void * iothreads_t;

// 创建网络线程组
// nthreads		- 网络线程组中的线程数
// method		- 任务处理函数
iothreads_t iothreads_start( uint8_t nthreads, 
					void (*method)(void *, uint8_t, int16_t, void *), void * context );

// 获取网络线程组中指定线程的ID
pthread_t iothreads_get_id( iothreads_t self, uint8_t index );

// 获取网络线程组中指定线程的事件集
evsets_t iothreads_get_sets( iothreads_t self, uint8_t index );

// 向网络线程组中指定的线程提交任务
// index	- 指定网络线程的编号
// type		- 提交的任务类型, NOTE:0xff内置的任务类型
// task		- 提交的任务数据
// size		- 任务数据的长度, 默认设置为0
int32_t iothreads_post( iothreads_t self, uint8_t index, int16_t type, void * task, uint8_t size );

// 网络线程组停止
void iothreads_stop( iothreads_t self );

#ifdef __cplusplus
}
#endif

#endif

