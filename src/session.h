
#ifndef SESSION_H
#define SESSION_H

/*
 * session 会话
 * 一个TCP连接的基本单元
 */

#include <stdint.h>
#include <netinet/in.h>

#include "event.h"
#include "network.h"
#include "queue.h"
#include "message.h"

// 64位SID的构成
// |RESERVED  |INDEX   |VERSION    |SEQ      |
// |16        |8       |16         |24       |
// (逻辑层用)  (路由节点) (防串号)     (底层槽位)

#define SID_RES_BITS        16
#define SID_INDEX_BITS      8
#define SID_VERSION_BITS    16
#define SID_SEQ_BITS        24
#define INVALID_SLOT_INDEX  0xFFFFFFFF
#define MAX_SLOT_CAPACITY   ( 1 << SID_SEQ_BITS )

#define SID_RES_MASK        ((1ULL << SID_RES_BITS) - 1)
#define SID_INDEX_MASK      ((1ULL << SID_INDEX_BITS) - 1)
#define SID_VERSION_MASK    ((1ULL << SID_VERSION_BITS) - 1)
#define SID_SEQ_MASK        ((1ULL << SID_SEQ_BITS) - 1)

// 提取宏：利用 Mask 天然屏蔽高位干扰
#define SID_SEQ(sid)        ((uint32_t)((sid) & SID_SEQ_MASK))
#define SID_VERSION(sid)    ((uint16_t)(((sid) >> SID_SEQ_BITS) & SID_VERSION_MASK))
#define SID_INDEX(sid)      ((uint8_t)(((sid) >> (SID_SEQ_BITS + SID_VERSION_BITS)) & SID_INDEX_MASK) - 1)
#define SID_RES(sid)        ((uint16_t)(((sid) >> (SID_SEQ_BITS + SID_VERSION_BITS + SID_INDEX_BITS)) & SID_RES_MASK))

// 底层通信库生成 SID 的宏（保留位默认填 0）
#define MAKE_SID( index, ver, seq ) \
    ( ( ( (uint64_t)(index+1)&SID_INDEX_MASK ) << ( SID_SEQ_BITS + SID_VERSION_BITS ) ) | \
      ( ( (uint64_t)(ver)&SID_VERSION_MASK ) << SID_SEQ_BITS ) | \
      ( ( (uint64_t)(seq)&SID_SEQ_MASK ) ) )

// 逻辑层专用的组合宏（将业务标识缝合进底层 SID）
#define BIND_LOGIC_RES( sid, res ) \
    ( (sid) | ( ( (uint64_t)(res)&SID_RES_MASK ) << ( SID_SEQ_BITS + SID_VERSION_BITS + SID_INDEX_BITS ) ) )

#define SESSION_READING 0x01     // 等待读事件
#define SESSION_WRITING 0x02     // 等待写事件, [连接, 重连, 发送]
#define SESSION_KEEPALIVING 0x04 // 等待保活事件
#define SESSION_SHUTDOWNING 0x08 // 正在终止中..., 被逻辑层终止的会话
#define SESSION_EXITING 0x10     // 等待退出, 数据全部发送完毕后, 即可终止
#define SESSION_SCHEDULING 0x20  // UDP会话正在调度

enum SessionType {
    eSessionType_Accept = 1,    // Accept会话
    eSessionType_Connect = 2,   // Connect会话
    eSessionType_Associate = 3, // Associate会话
    eSessionType_Shared = 4,    // 共享会话
};

struct session;
struct session_setting {
    int32_t persist_mode;
    int32_t timeout_msecs;
    int32_t keepalive_msecs;
    int32_t max_inbuffer_len;
    int32_t sendqueue_limit;
    ssize_t ( *transmit )( struct session * s );
    ssize_t ( *send )( struct session * s, char * buf, size_t nbytes );
};

// 定时任务
struct schedule_task {
    int32_t type;
    void * task;
    int32_t interval;
    taskrecycler_t recycle;
    event_t evschedule;
    struct session * session;
    SLIST_ENTRY( schedule_task ) tasklink;
};
SLIST_HEAD( tasklist, schedule_task );

struct driver;
QUEUE_HEAD( sendqueue, struct message * );
QUEUE_PROTOTYPE( sendqueue, struct message * )

struct session {
    sid_t id;

    int32_t fd;
    int8_t type;
    int8_t status;
    char * host;
    uint16_t port;

    // 读写以及超时事件
    event_t evread;
    event_t evwrite;
    event_t evkeepalive;

    // 事件集和管理器
    evsets_t evsets;
    void * iolayer;
    void * context;
    ioservice_t service;
    struct session_manager * manager;

    // udp驱动
    struct driver * driver;

    // 关联的第三方会话
    void * privdata;       // 私有数据
    reattacher_t reattach; //

    // 接收缓冲区
    struct buffer inbuffer;

    // 定时任务列表
    struct tasklist tasklist;

    // 发送队列以及消息偏移量
    size_t msgoffset;
    struct sendqueue sendqueue;

    // 会话的设置
    struct session_setting setting;

    // 回收链表
    STAILQ_ENTRY( session ) recyclelink;
};

// 会话开始
int32_t session_start( struct session * self, int8_t type, int32_t fd, evsets_t sets );

// 会话是否支持重连
int8_t session_is_reattch( struct session * self );

//
void session_set_iolayer( struct session * self, void * iolayer );
void session_set_driver( struct session * self, struct driver * driver );
void session_set_endpoint( struct session * self, char * host, uint16_t port );
void session_copy_endpoint( struct session * self, const char * host, uint16_t port );
// 设置第三方的重连函数
void session_set_reattach( struct session * self, reattacher_t reattach, void * data );

// 发送队列的长度
#define session_sendqueue_count( self ) QUEUE_COUNT( sendqueue )( &( ( self )->sendqueue ) )
#define session_sendqueue_append( self, msg ) QUEUE_PUSH( sendqueue )( &( ( self )->sendqueue ), &( msg ) )
#define session_sendqueue_shrink( self, size ) QUEUE_SHRINK( sendqueue )( &( ( self )->sendqueue ), ( size ) )

// 发送队列交换
void session_sendqueue_take( struct session * self, struct sendqueue * q );
// 发送队列合并
void session_sendqueue_merge( struct session * self, struct sendqueue * q );

// 发送数据
ssize_t session_send( struct session * self, char * buf, size_t nbytes );
// 发送消息
ssize_t session_sendmessage( struct session * self, struct message * message );

// 会话注册/反注册网络事件
void session_add_event( struct session * self, int16_t ev );
void session_del_event( struct session * self, int16_t ev );
// 重新注册网络事件
void session_readd_event( struct session * self, int16_t ev );

// 开始发送心跳
int32_t session_start_keepalive( struct session * self );

// 重连远程服务器
int32_t session_start_reconnect( struct session * self );

// 启动定时任务
int32_t session_cancel_task( struct session * self, struct schedule_task * task );
int32_t session_schedule_task( struct session * self, int32_t type, void * t, int32_t interval, taskrecycler_t recycle );

// 设置被终止的标志
#define session_close( self ) ( ( self )->status |= SESSION_SHUTDOWNING )
#define session_call_shutdown( self, way ) ( ( self )->service.shutdown( ( self )->context, ( way ) ) )

// 尝试终止会话
// 该API会尽量把发送队列中的数据发送出去
// libevlite安全终止会话的核心模块
int32_t session_shutdown( struct session * self );

// 会话结束
int32_t session_end( struct session * self, sid_t id, int8_t recycle );

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

struct slot;
STAILQ_HEAD( sessionlist, session );

struct session_manager {
    uint8_t index;
    uint32_t count;
    uint32_t capacity;              // 当前容量

    struct slot * table;
    uint32_t free_head;

    uint32_t recyclesize;           // 回收个数
    struct sessionlist recyclelist; // 回收队列
};

// 创建会话管理器
// index    - 会话管理器索引号
// count    - 会话管理器中管理多少个会话
struct session_manager * session_manager_create( uint8_t index, uint32_t size );

// 获取会话个数
#define session_manager_count( self ) ( (self)->count )

// 分配一个会话
struct session * session_manager_alloc( struct session_manager * self );

// 从会话管理器中取出一个会话
struct session * session_manager_get( struct session_manager * self, sid_t id );

// 遍历
int32_t session_manager_foreach( struct session_manager * self,
    int32_t ( *func )( void *, struct session * ), void * context );

// 从会话管理器中移出会话
int32_t session_manager_remove( struct session_manager * self, struct session * session );

// 回收会话
void session_manager_recycle( struct session_manager * self, struct session * session );

// 销毁会话管理器
void session_manager_destroy( struct session_manager * self );

#endif
