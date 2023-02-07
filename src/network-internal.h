
#ifndef NETWORK_INTERNAL_H
#define NETWORK_INTERNAL_H

#include <stdint.h>
#include "queue.h"
#include "message.h"
#include "threads.h"

// 是否安全的终止会话
#define SAFE_SHUTDOWN           1

// 发送队列的默认大小
#define DEFAULT_SENDQUEUE_SIZE  128

// 关闭前最大等待时间,默认10s
#define MAX_SECONDS_WAIT_FOR_SHUTDOWN   (10*1000)

// 尝试重连的间隔时间,默认为200ms
#define TRY_RECONNECT_INTERVAL  200

// 发送接收缓冲区设置
// 默认为0, 由内核动态控制
#define SEND_BUFFER_SIZE        0
#define RECV_BUFFER_SIZE        0

enum
{
    eIOStatus_Running           = 1,        // 运行
    eIOStatus_Stopped           = 2,        // 停止
};

// 任务类型
enum
{
    eIOTaskType_Invalid         = 0,
    eIOTaskType_Listen          = 1,
    eIOTaskType_Assign          = 2,
    eIOTaskType_Connect         = 3,
    eIOTaskType_Associate       = 4,
    eIOTaskType_Send            = 5,
    eIOTaskType_Broadcast       = 6,
    eIOTaskType_Shutdown        = 7,
    eIOTaskType_Shutdowns       = 8,
    eIOTaskType_Broadcast2      = 9,
    eIOTaskType_Invoke          = 10,
    eIOTaskType_Perform         = 11,
};

// 网络服务错误码定义
enum
{
    eIOError_OutMemory          = 0x00010001,
    eIOError_ConnectStatus      = 0x00010002,   // 非法的连接状态
    eIOError_ConflictSid        = 0x00010003,   // 冲突的SID
    eIOError_InBufferFull       = 0x00010004,   // 缓冲区满了
    eIOError_ReadFailure        = 0x00010005,   // read()失败
    eIOError_PeerShutdown       = 0x00010006,   // 对端关闭了连接
    eIOError_WriteFailure       = 0x00010007,   // write()失败
    eIOError_ConnectFailure     = 0x00010008,   // 连接失败
    eIOError_Timeout            = 0x00010009,   // 连接超时了
    eIOError_SocketInvalid      = 0x0001000A,   // read()失败, Socket非法
    eIOError_InBufferInvalid    = 0x0001000B,   // read()失败, 接收缓冲区非法
    eIOError_ReadIOError        = 0x0001000C,   // read()失败, IO错误
    eIOError_ReadInvalid        = 0x0001000D,   // read()失败, EINVAL
    eIOError_SendQueueLimit     = 0x0001000E,   // 发送队列过大
};

// 网络层
struct iolayer
{
    // 网络层状态
    uint8_t                 status;
    // 基础配置
    uint8_t                 nthreads;
    uint32_t                nclients;
    uint32_t                roundrobin;         // 轮询负载均衡

    // 网络线程组
    iothreads_t             threads;
    // 会话管理器
    void **                 managers;

    // 数据改造接口
    void *                  context;
    transformer_t           transform;
};

// 接收器
struct acceptqueue;
struct acceptor
{
    int32_t                 fd;
    uint8_t                 type;
    uint8_t                 index;
    options_t               options;

    // 接收事件
    event_t                 event;
    evsets_t                evsets;

    // 绑定的地址以及监听的端口号
    char *                  host;
    uint16_t                port;
    struct sockaddr_storage addr;

    // 逻辑
    acceptor_t              cb;
    void *                  context;

    // 空闲描述符
    int32_t                 idlefd;
    // 通信层句柄
    struct iolayer *        parent;
    //
    struct buffer           buffer;
    struct acceptqueue *    acceptq;

    // 便于回收资源
    STAILQ_ENTRY(acceptor)  linker;
};

// 连接器
struct connector
{
    int32_t                 fd;
    uint8_t                 index;

    // 连接事件
    event_t                 event;
    evsets_t                evsets;

    // 连接服务器的地址和端口号
    char *                  host;
    uint16_t                port;

    // 逻辑
    connector_t             cb;
    void *                  context;

    // 通信层句柄
    struct iolayer *        parent;

    // 便于回收资源
    uint8_t                 state;
    STAILQ_ENTRY(connector) linker;
};

// 关联器
struct associater
{
    int32_t                     fd;
    uint8_t                     index;
    void *                      privdata;

    // 连接事件
    event_t                     event;
    evsets_t                    evsets;

    // 逻辑
    void *                      context;
    reattacher_t                reattach;
    associator_t                cb;

    // 通信句柄
    struct iolayer *            parent;

    // 便于回收资源
    uint8_t                     state;
    STAILQ_ENTRY(associater)    linker;
};

//
// NOTICE: 网络任务的最大长度不超过56
//

// NOTICE: task_assign长度已经达到40bytes
struct task_assign
{
    int32_t                 fd;
    uint8_t                 type;
    uint16_t                port;

    char *                  host;
    struct buffer *         buffer;

    acceptor_t              cb;
    void *                  context;
};

struct task_send
{
    sid_t                   id;             // 8bytes
    char *                  buf;            // 8bytes
    size_t                  nbytes;         // 8bytes
    int32_t                 isfree;         // 4bytes
};

struct task_invoke
{
    void *                  task;
    taskexecutor_t          perform;
};

struct task_perform
{
    sid_t                   id;
    int32_t                 type;
    void *                  task;
    int32_t                 interval;
    taskrecycler_t          recycle;
};

// 描述符分发策略
// 分发到IO线程后会分配到唯一的会话ID
#define DISPATCH_POLICY( layer, seq ) ( (seq) % ((layer)->nthreads) )

// socket选项
int32_t iolayer_udp_option( int32_t fd );
int32_t iolayer_server_option( int32_t fd );
int32_t iolayer_client_option( int32_t fd );

// 分配一个会话
struct session * iolayer_alloc_session( struct iolayer * self, int32_t key, uint8_t index );

// 销毁接收器
void iolayer_free_acceptor( struct acceptor * acceptor );
// 销毁连接器
void iolayer_free_connector( struct connector * connector );
// 销毁关联器
void iolayer_free_associater( struct associater * associater );

// 处理EMFILE
void iolayer_accept_fdlimits( struct acceptor * acceptor );

// 给当前线程分发一个会话
int32_t iolayer_assign_session( struct iolayer * self, uint8_t index, struct task_assign * task );
int32_t iolayer_assign_session_direct( struct iolayer * self, uint8_t acceptidx, uint8_t index, struct task_assign * task );

#endif
