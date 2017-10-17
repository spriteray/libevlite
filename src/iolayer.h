
#ifndef SRC_IOLAYER_H
#define SRC_IOLAYER_H

#include <stdint.h>

#include "event.h"
#include "threads.h"
#include "network.h"

#include "session.h"

enum
{
    eLayerStatus_Running        = 1,    // 运行
    eLayerStatus_Stopped        = 2,    // 停止
};

struct iolayer
{
    // 网络层状态
    uint8_t     status;
    // 基础配置
    uint8_t     nthreads;
    uint32_t    nclients;

    // 网络线程组
    iothreads_t group;

    // 会话管理器
    void **     managers;

    // 数据改造接口
    void *      context;
    char *      (*transform)( void *, const char *, uint32_t * );
};

// 接收器
struct acceptor
{
    int32_t     fd;
    uint8_t     index;

    // 接收事件
    event_t     event;

    // 绑定的地址以及监听的端口号
    char        host[INET_ADDRSTRLEN];
    uint16_t    port;

    // 逻辑
    void *      context;
    int32_t     (*cb)(void *, void *, sid_t, const char *, uint16_t);

    // 通信层句柄
    struct iolayer * parent;
};

// 连接器
struct connector
{
    int32_t     fd;
    uint8_t     index;

    // 连接事件
    event_t     event;
    evsets_t    evsets;

    // 连接服务器的地址和端口号
    char        host[INET_ADDRSTRLEN];
    uint16_t    port;

    // 逻辑
    int32_t     mseconds;
    void *      context;
    int32_t     (*cb)(void *, void *, int32_t, const char *, uint16_t, sid_t);

    // 通信层句柄
    struct iolayer * parent;
};

// 关联器
struct associater
{
    int32_t     fd;

    // 逻辑
    void *      context;
    int32_t     (*cb)( void *, void *, int32_t, sid_t );

    // 通信句柄
    struct iolayer *    parent;
};

//
// NOTICE: 网络任务的最大长度不超过56
//

// NOTICE: task_assign长度已经达到48bytes
struct task_assign
{
    int32_t     fd;                             // 4bytes

    uint16_t    port;                           // 2bytes
    char        host[INET_ADDRSTRLEN];          // 16bytes + 2bytes

    void *      context;                        // 8bytes
    int32_t     (*cb)(void *, void *, sid_t, const char *, uint16_t);    // 8bytes
};

struct task_send
{
    sid_t       id;             // 8bytes
    char *      buf;            // 8bytes
    uint32_t    nbytes;         // 4bytes
    int32_t     isfree;         // 4bytes
};

struct task_perform
{
    sid_t       id;
    int32_t     type;
    void *      task;
};

struct task_perform2
{
    void *      task;
    void *      (*clone)( void * );
    void        (*perform)( void *, void * );
};

// 看样子内存对齐不需要使用了
#pragma pack(1)
#pragma pack()

// 描述符分发策略
// 分发到IO线程后会分配到唯一的会话ID
#define DISPATCH_POLICY( layer, fd ) ( (fd) % ((layer)->nthreads) )

// socket选项
void iolayer_server_option( int32_t fd );
void iolayer_client_option( int32_t fd );

// 分配一个会话
struct session * iolayer_alloc_session( struct iolayer * self, int32_t key, uint8_t index );

// 重新连接远程服务器
int32_t iolayer_reconnect( struct iolayer * self, struct connector * connector );

// 销毁连接器
int32_t iolayer_free_connector( struct iolayer * self, struct connector * connector );

// 给当前线程分发一个会话
int32_t iolayer_assign_session( struct iolayer * self, uint8_t acceptidx, uint8_t index, struct task_assign * task );

#endif
