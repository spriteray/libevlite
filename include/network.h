
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

#ifndef NETWORK_H
#define NETWORK_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include <sys/types.h>

//
// 网络层
//
typedef uint64_t    sid_t;
typedef void *      iolayer_t;

// 任务克隆函数
//      参数1: 任务本身
typedef void * (*taskcloner_t)( void * );

// 任务处理函数
//      参数1: iocontext
//      参数2: 任务
typedef void (*taskexecutor_t)( void *, void * );

// 任务回收函数
//      参数1: 类型
//      参数2: 任务
typedef void (*taskrecycler_t)( int32_t, void * );

//  重新关联函数，返回新的描述符
//      参数1: 上次关联的描述符
//      参数2: 描述符相关的私有数据
typedef int32_t (*reattacher_t)( int32_t, void * );

// 数据改造方法(不建议原地改造)
//      参数1: 上下文参数
//      参数2: 欲发送或者广播的消息内容
//      参数3: 指向消息长度的指针, 返回改造后的数据包长度
typedef char * (*transformer_t)( void *, const char * , size_t * );

// 新会话创建成功后的回调
//      参数1: 上下文参数;
//      参数2: 网络线程上下文参数
//      参数3: 新会话ID;
//      参数4: 会话的IP地址;
//      参数5: 会话的端口号
typedef int32_t (*acceptor_t)( void *, void *, sid_t, const char * , uint16_t );

//  连接结果的回调
//      参数1: 上下文参数
//      参数2: 网络线程上下文参数
//      参数3: 连接结果
//      参数4: 连接的远程服务器的地址
//      参数5: 连接的远程服务器的端口
//      参数6: 连接成功后返回的会话ID
typedef int32_t (*connector_t)( void *, void *, int32_t, const char *, uint16_t, sid_t );

//  关联成功后的回调
//      参数1: 上下文参数
//      参数2: 网络线程上下文参数
//      参数3: 关联结果
//      参数3: 描述符
//      参数4: 描述符相关私有数据
//      参数5: 会话ID
typedef int32_t (*associator_t)( void *, void *, int32_t, int32_t, void *, sid_t );

// IO服务
//        start()       - 网络就绪的回调
//        process()     - 收到数据包的回调
//                        返回值为处理掉的数据包, <0: 出错,关闭连接
//        transform()   - 发送数据包前的回调
//                        返回需要发送的数据包
//                        如果改造了数据包，请务必确保1.禁止原地改造; 2.数据包是必须是malloc()分配出来的
//        keepalive()   - 保活定时器超时的回调
//        timeout()     - 超时的回调
//        error()       - 出错的回调
//                        对于accept()出来的客户端, 直接回调shutdown();
//                        对于connect()出去的客户端, ==0, 尝试重连, !=0, 直接回调shutdown() .
//        shutdown()    - 会话终止时的回调, 不论返回值, 直接销毁会话
//                        way - 0, 逻辑层主动终止会话的情况,
//                                 也就是直接调用iolayer_shutdown()或者iolayer_shutdowns();
//                              1, 逻辑层被动终止会话的情况.
//        perform()     - 处理其他模块提交到网络层的任务
//                        type - 任务类型
//                        task - 任务数据
typedef struct
{
    int32_t (*start)( void * context );
    ssize_t (*process)( void * context, const char * buf, size_t nbytes );
    char *  (*transform)( void * context, const char * buf, size_t * nbytes );
    int32_t (*keepalive)( void * context );
    int32_t (*timeout)( void * context );
    int32_t (*error)( void * context, int32_t result );
    int32_t (*perform)( void * context, int32_t type, void * task );
    void    (*shutdown)( void * context, int32_t way );
}ioservice_t;

// 创建网络层
//        nthreads      - 网络线程数
//        nclients      - 网络层服务的连接数
//        immediately   - 是否立刻提交网络层, 0:否; 1:是(用于对网络实时性比较高的场景)
iolayer_t iolayer_create( uint8_t nthreads, uint32_t nclients, uint8_t immediately );

// 网络层设置线程上下文参数(在listen(), connect(), associate()之前调用)
//        self          -
//        contexts      - 上下文参数数组, 每个网络线程设置上下文参数
//        count         - 数组长度
//                        确保长度和网络线程个数相等, 毕竟多个网络线程是对等的
int32_t iolayer_set_iocontext( iolayer_t self, void ** contexts, uint8_t count );

// 网络层设置数据包改造方法, 该网络层的统一的数据包改造方法(在listen(), connect(), associate()之前调用)
//        self          -
//        transform     - 数据包改造方法(不建议原地改造)
int32_t iolayer_set_transform( iolayer_t self, transformer_t transform, void * context );

// 服务器开启
//        host          - 绑定的地址
//        port          - 监听的端口号
//        callback      - 新会话创建成功后的回调,会被多个网络线程调用
//        context       - 上下文参数
int32_t iolayer_listen( iolayer_t self,
        const char * host, uint16_t port, acceptor_t callback, void * context );

// 客户端开启
//        host          - 远程服务器的地址
//        port          - 远程服务器的端口
//        callback      - 连接结果的回调
//        context       - 上下文参数
int32_t iolayer_connect( iolayer_t self,
        const char * host, uint16_t port, connector_t callback, void * context );

// 描述符关联会话ID
//      fd              - 描述符
//      privdata        - 描述符相关的私有数据
//      reattach        - 重新关联函数，返回新的描述符
//      callback        - 关联成功后的回调
//      context         - 上下文参数
int32_t iolayer_associate( iolayer_t self,
        int32_t fd, void * privdata, reattacher_t reattach, associator_t callback, void * context );

// 会话参数的设置, 只能在ioservice_t中使用
int32_t iolayer_set_timeout( iolayer_t self, sid_t id, int32_t seconds );
int32_t iolayer_set_keepalive( iolayer_t self, sid_t id, int32_t seconds );
int32_t iolayer_set_endpoint( iolayer_t self, sid_t id, const char * host, uint16_t port );
int32_t iolayer_set_service( iolayer_t self, sid_t id, ioservice_t * service, void * context );

// 发送数据到会话
//      id              - 会话ID
//      buf             - 要发送的缓冲区
//      nbytes          - 要发送的长度
//      isfree          - 1-由网络层释放缓冲区, 0-网络层需要Copy缓冲区
int32_t iolayer_send( iolayer_t self, sid_t id, const char * buf, size_t nbytes, int32_t isfree );

// 广播数据到指定的会话
int32_t iolayer_broadcast( iolayer_t self, sid_t * ids, uint32_t count, const char * buf, size_t nbytes );

// 广播数据到IO层的所有会话
int32_t iolayer_broadcast2( iolayer_t self, const char * buf, size_t nbytes );

// 终止指定的会话
// 此处需要注意, 主动终止会话的情况下,也会收到shutdown()的回调, 只是way==0
int32_t iolayer_shutdown( iolayer_t self, sid_t id );
int32_t iolayer_shutdowns( iolayer_t self, sid_t * ids, uint32_t count );

// 提交任务到网络层(会话ID所在网络线程)
//      id              - 会话ID
//      type            - 任务类型
//      task            - 任务数据
//      recycle         - 任务回收函数
int32_t iolayer_perform( iolayer_t self, sid_t id, int32_t type, void * task, taskrecycler_t recycle );

// 提交任务到网络层(广播所有网络线程)
//      task            - 任务
//      clone           - 任务复制函数
//      perform         - 任务处理函数
//                          参数1: iocontext
//                          参数2: task
int32_t iolayer_performs( iolayer_t self, void * task, taskcloner_t clone, taskexecutor_t perform );

// 停止网络服务
// 行为定义:
//      1. 停止对外提供接入服务, 不再接受新的连接;
//      2. 停止已经接入连接的接收服务, 不再接收新的数据包(接收但不回调ioservice::process())
//      3. 一切发送行为都正常进行
void iolayer_stop( iolayer_t self );

// 销毁网络层
void iolayer_destroy( iolayer_t self );

#ifdef __cplusplus
}
#endif

#endif
