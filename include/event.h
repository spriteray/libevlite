
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

#ifndef EVENT_H
#define EVENT_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

//
// 事件库所支持的事件类型
//

#define EV_READ     0x01    // 读事件
#define EV_WRITE    0x02    // 写事件
#define EV_TIMEOUT  0x04    // 超时事件
#define EV_PERSIST  0x08    // 永久模式


//
// 事件的定义, 以及事件集的定义
//

typedef void * event_t;
typedef void * evsets_t;

//
// 事件的方法
//

// 创建事件
event_t event_create();

// 设置事件的一些基础属性
//      fd      - 关注的描述符;
//      ev      - 关注的事件,即以上定义的那四种
void event_set( event_t self, int32_t fd, int16_t ev );

// 设置事件的回调函数
//      self    -
//      cb      - 回调函数
//      arg     - 回调函数的参数
void event_set_callback( event_t self, void (*cb)(int32_t, int16_t, void *), void * arg );

// 获取事件关注的描述符FD
int32_t event_get_fd( event_t self );

// 获取事件所属事件集
evsets_t event_get_sets( event_t self );

// 销毁事件
void event_destroy( event_t self );

//
// 事件集的方法
//

// 创建事件集
evsets_t evsets_create();

// 事件库的版本
const char * evsets_get_version();

// 向事件集中添加事件
//      self    -
//      ev      - 事件
//      tv      - 超时时间(ms)
//      返回值定义:
//          返回<0, 添加事件失败
//          返回 1, 添加IO事件成功
//          返回 2, 添加超时事件成功
//          返回 3, IO事件和超时事件添加成功
int32_t evsets_add( evsets_t self, event_t ev, int32_t tv );

// 从事件集中删除事件
int32_t evsets_del( evsets_t self, event_t ev );

// 分发并处理事件
// 返回激活的事件个数
int32_t evsets_dispatch( evsets_t self );

// 销毁事件集
void evsets_destroy( evsets_t self );

#ifdef __cplusplus
}
#endif

#endif
