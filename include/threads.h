
/*
 * Copyright (c) 2012, Raymond Zhang <spriteray@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in
 *   the documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

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
// nthreads         - 网络线程组中的线程数
// method           - 任务处理函数
iothreads_t iothreads_start( uint8_t nthreads,
                    void (*method)(void *, uint8_t, int16_t, void *), void * context );

// 获取网络线程组中指定线程的ID
pthread_t iothreads_get_id( iothreads_t self, uint8_t index );

// 获取网络线程组中指定线程的事件集
evsets_t iothreads_get_sets( iothreads_t self, uint8_t index );

// 向网络线程组中指定的线程提交任务
// index            - 指定网络线程的编号
// type             - 提交的任务类型, NOTE:0xff内置的任务类型
// task             - 提交的任务数据
// size             - 任务数据的长度, 默认设置为0
int32_t iothreads_post( iothreads_t self, uint8_t index, int16_t type, void * task, uint8_t size );

// 网络线程组停止
void iothreads_stop( iothreads_t self );

#ifdef __cplusplus
}
#endif

#endif
