
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
// cb			- 任务处理函数
iothreads_t iothreads_create( uint8_t nthreads, void (*cb)(uint8_t, evsets_t, int32_t, void *) );

// 网络线程组开始运行
int32_t iothreads_start( iothreads_t self );

// 向网络线程组中指定的线程提交任务
// index	- 指定网络线程的编号
// type		- 提交的任务类型, NOTE:0xff内置的任务类型
// task		- 提交的任务数据
int32_t iothreads_post( iothreads_t self, uint8_t index, int32_t type, void * task );

// 网络线程组停止
void iothreads_stop( iothreads_t self );

// 销毁网络线程组
void iothreads_destroy( iothreads_t self );

#ifdef __cplusplus
}
#endif

#endif

