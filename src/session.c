
#include <stdio.h>
#include <assert.h>
#include <syslog.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

#include "driver.h"
#include "channel.h"
#include "session.h"
#include "network-internal.h"

static inline struct session * _new_session();
static inline int32_t _del_session( struct session * self );
static inline int32_t _reset_session( struct session * self );
static inline void _stop( struct session * self );
static inline void _init_settings( struct session_setting * self );

// 发送数据
// _send_only()仅发送,
// _send_message()发送消息, 未发送成功的添加到发送队列中
// _send_buffer()发送数据, 未发送成功的创建消息, 添加到发送队列中
static inline ssize_t _send_only( struct session * self, char * buf, size_t nbytes );
static inline ssize_t _send_message( struct session * self, struct message * message );
static inline ssize_t _send_buffer( struct session * self, char * buf, size_t nbytes );

//
QUEUE_GENERATE( sendqueue, struct message * )

//
struct session * _new_session()
{
    struct session * self = (struct session *)calloc( 1, sizeof( struct session ) );
    assert( self != NULL && "allocate struct session failed" );

    // 初始化任务队列
    SLIST_INIT( &self->tasklist );
    // 初始化接收缓冲区
    buffer_init( &self->inbuffer );
    // 初始化设置
    _init_settings( &self->setting );

    // 初始化网络事件
    self->evread = event_create();
    self->evwrite = event_create();
    self->evkeepalive = event_create();
    assert( self->evread != NULL
        && self->evwrite != NULL && self->evkeepalive != NULL );

    // 初始化发送队列
    QUEUE_INIT( sendqueue )( &self->sendqueue, DEFAULT_SENDQUEUE_SIZE );

    return self;
}

int32_t _reset_session( struct session * self )
{
    // 重置
    self->id = 0;
    self->status = 0;
    self->msgoffset = 0;

    // 初始化设置
    _init_settings( &self->setting );

    if ( likely( self->host != NULL ) ) {
        free( self->host );
        self->host = NULL;
    }
    // 销毁网络事件
    if ( likely( self->evread != NULL ) ) {
        event_reset( self->evread );
    }
    if ( likely( self->evwrite != NULL ) ) {
        event_reset( self->evwrite );
    }
    if ( likely( self->evkeepalive != NULL ) ) {
        event_reset( self->evkeepalive );
    }

    buffer_erase( &self->inbuffer,
        buffer_length( &self->inbuffer ) );
    // 收缩发送队列
    QUEUE_RESET( sendqueue ) ( &self->sendqueue );
    QUEUE_SHRINK( sendqueue ) ( &self->sendqueue, DEFAULT_SENDQUEUE_SIZE );

    return 0;
}

int32_t _del_session( struct session * self )
{
    // 重置
    self->id = 0;
    self->status = 0;
    self->msgoffset = 0;

    // 销毁host
    if ( likely( self->host != NULL ) ) {
        free( self->host );
        self->host = NULL;
    }

    // 销毁网络事件
    if ( likely( self->evread != NULL ) ) {
        event_destroy( self->evread );
        self->evread = NULL;
    }
    if ( likely( self->evwrite != NULL ) ) {
        event_destroy( self->evwrite );
        self->evwrite = NULL;
    }
    if ( likely( self->evkeepalive != NULL ) ) {
        event_destroy( self->evkeepalive );
        self->evkeepalive = NULL;
    }

    buffer_clear( &self->inbuffer );
    QUEUE_CLEAR( sendqueue ) ( &self->sendqueue );
    free( self );

    return 0;
}

// 会话停止(删除网络事件以及关闭描述符)
void _stop( struct session * self )
{
    struct schedule_task * loop = NULL;
    SLIST_FOREACH( loop, &self->tasklist, tasklink )
    {
        if ( likely( loop->evschedule != NULL ) ) {
            evsets_del( self->evsets, loop->evschedule );
            event_destroy( loop->evschedule );
            loop->evschedule = NULL;
        }
        loop->recycle( loop->type, loop->task, loop->interval );
        free( loop );
    }
    SLIST_INIT( &self->tasklist );

    // 删除网络事件
    if ( self->status & SESSION_READING ) {
        evsets_del( self->evsets, self->evread );
        self->status &= ~SESSION_READING;
    }
    if ( self->status & SESSION_WRITING ) {
        evsets_del( self->evsets, self->evwrite );
        self->status &= ~SESSION_WRITING;
    }
    if ( self->status & SESSION_KEEPALIVING ) {
        evsets_del( self->evsets, self->evkeepalive );
        self->status &= ~SESSION_KEEPALIVING;
    }

    // 清空接收缓冲区
    buffer_erase( &self->inbuffer, -1 );

    // 销毁UDP驱动
    if ( likely( self->driver != NULL ) ) {
        driver_destroy( self->driver );
    }

    // 关闭描述符
    if ( self->fd > 0
        && self->type != eSessionType_Shared ) {
        close( self->fd );
    }

    self->fd = -1;
    self->driver = NULL;
}

void _init_settings( struct session_setting * self )
{
    self->persist_mode = 0;
    self->timeout_msecs = -1;
    self->keepalive_msecs = -1;
    self->max_inbuffer_len = 0;
    self->sendqueue_limit = 0;
    self->send = NULL;
    self->transmit = NULL;
}

//
ssize_t _send_only( struct session * self, char * buf, size_t nbytes )
{
    ssize_t ntry = 0;

    if ( unlikely( self->status & SESSION_EXITING ) ) {
        // 等待关闭的连接
        return -1;
    }

    // 判断session是否繁忙
    if ( !( self->status & SESSION_WRITING )
        && session_sendqueue_count( self ) == 0 ) {
        assert( self->msgoffset == 0 && "SendQueue Offset Invalid" );

        // 直接发送
        ntry = self->setting.send( self, buf, nbytes );
        if ( ntry < 0 ) {
            // 发送出错的情况下
            // 将整包添加到发送队列中, 发送长度置0
            ntry = 0;
        }

        // 为什么发送错误没有直接终止会话呢？
        // 该接口有可能在ioservice_t中调用, 直接终止会话后, 会引发后续对会话的操作崩溃
        // 发送出错的情况下, 添加写事件, 在channel.c:_transmit()出错后, 会尝试关闭连接
    }

    return ntry;
}

ssize_t _send_message( struct session * self, struct message * message )
{
    ssize_t ntry = 0;
    char * buf = message_get_buffer( message );
    size_t nbytes = message_get_length( message );

    // 发送
    ntry = _send_only( self, buf, nbytes );
    if ( ntry >= 0 && ntry < (ssize_t)nbytes ) {
        // 未全部发送成功的情况下

        // 添加到发送队列中
        // ntry == 0有两种情况:1. 繁忙; 2. 发送出错
        self->msgoffset += ntry;
        QUEUE_PUSH( sendqueue ) ( &self->sendqueue, &message );
        // 增加写事件
        session_add_event( self, EV_WRITE );
    }

    return ntry;
}

ssize_t _send_buffer( struct session * self, char * buf, size_t nbytes )
{
    // 发送
    ssize_t ntry = _send_only( self, buf, nbytes );
    if ( ntry >= 0 && ntry < (ssize_t)nbytes ) {
        // 未全部发送成功的情况下

        // 创建message, 添加到发送队列中
        struct message * message = message_create();
        if ( message == NULL ) {
            return -2;
        }
        message_add_buffer( message, buf + ntry, nbytes - ntry );
        message_add_receiver( message, self->id );
        QUEUE_PUSH( sendqueue ) ( &self->sendqueue, &message );
        session_add_event( self, EV_WRITE );
    }

    return ntry;
}

int32_t session_start( struct session * self, int8_t type, int32_t fd, evsets_t sets )
{
    assert( self->service.start != NULL );
    assert( self->service.process != NULL );
    assert( self->service.keepalive != NULL );
    assert( self->service.timeout != NULL );
    assert( self->service.error != NULL );
    assert( self->service.shutdown != NULL );
    assert( self->service.perform != NULL );

    // 默认参数
    self->fd = fd;
    self->type = type;
    self->evsets = sets;

    // TODO: 设置默认的最大接收缓冲区

    // Session收发函数定义
    // NOTICE: 因为会话start()回调会发送数据，所以必须在start()之前设置
    if ( self->driver == NULL ) {
        self->setting.send = channel_send;
        self->setting.transmit = channel_transmit;
    } else {
        self->setting.send = driver_send;
        self->setting.transmit = driver_transmit;
    }

    // 不需要每次开始会话的时候初始化
    // 只需要在manager创建会话的时候初始化一次，即可
    // NOTICE: 目前可以知道的是linux kernel >= 4.18, 能感知到对端断开
    self->service.start( self->context );

    // 关注读事件, 按需开启保活心跳
    session_add_event( self, EV_READ );
    session_start_keepalive( self );

    return 0;
}

int8_t session_is_reattch( struct session * self )
{
    if ( self->type == eSessionType_Accept ) {
        // Accept会话是临时的
        return 0;
    } else if ( self->type == eSessionType_Shared ) {
        // Shared会话是临时的
        return 0;
    } else if ( self->type == eSessionType_Connect ) {
        // Connect会话是永久的
        return 1;
    }

    // 设置了第三方重连函数的Associate会话
    return self->reattach != NULL ? 1 : 0;
}

void session_set_iolayer( struct session * self, void * iolayer )
{
    self->iolayer = iolayer;
}

void session_set_driver( struct session * self, struct driver * driver )
{
    self->driver = driver;
}

void session_set_endpoint( struct session * self, char * host, uint16_t port )
{
    assert( self->host == NULL );
    self->port = port;
    self->host = host;
}

void session_copy_endpoint( struct session * self, const char * host, uint16_t port )
{
    assert( self->host == NULL );
    self->port = port;
    self->host = strdup( host );
}

void session_set_reattach( struct session * self, reattacher_t reattach, void * data )
{
    self->privdata = data;
    self->reattach = reattach;
}

void session_sendqueue_take( struct session * self, struct sendqueue * q )
{
    // 当前的消息需要重发
    QUEUE_INIT( sendqueue ) ( q, DEFAULT_SENDQUEUE_SIZE );

    self->msgoffset = 0;
    QUEUE_SWAP( sendqueue ) ( q, &self->sendqueue );
}

void session_sendqueue_merge( struct session * self, struct sendqueue * q )
{
    for ( ; QUEUE_COUNT( sendqueue )( q ) > 0; ) {
        struct message * msg = NULL;
        QUEUE_POP( sendqueue ) ( q, &msg );
        session_sendqueue_append( self, msg );
    }

    QUEUE_CLEAR( sendqueue ) ( q );
}

ssize_t session_send( struct session * self, char * buf, size_t nbytes )
{
    ssize_t rc = -1;
    char * _buf = buf;
    size_t _nbytes = nbytes;

    // 数据改造(加密 or 压缩)
    if ( self->service.transform != NULL ) {
        _buf = self->service.transform(
            self->context, (const char *)buf, &_nbytes );
    }

    if ( likely( _buf != NULL ) ) {
        // 发送数据
        // TODO: _send_buffer()可以根据具体情况决定是否copy内存
        rc = _send_buffer( self, _buf, _nbytes );

        if ( _buf != buf ) {
            // 销毁改造的消息
            free( _buf );
        }
    }

    return rc;
}

//
ssize_t session_sendmessage( struct session * self, struct message * message )
{
    char * buf = message_get_buffer( message );
    size_t nbytes = message_get_length( message );

    ssize_t rc = -1;
    char * buffer = buf;

    // 数据改造(加密 or 压缩)
    if ( self->service.transform != NULL ) {
        buffer = self->service.transform(
            self->context, (const char *)buf, &nbytes );
    }

    if ( buffer == buf ) {
        // 消息未进行改造

        // 添加到会话的发送列表中
        rc = QUEUE_PUSH( sendqueue )( &self->sendqueue, &message );
        if ( rc == 0 ) {
            // 注册写事件
            session_add_event( self, EV_WRITE );
        }
    } else if ( buffer != NULL ) {
        // 消息改造成功

        rc = _send_buffer( self, buffer, nbytes );
        if ( rc >= 0 ) {
            // 改造后的消息已经单独发送
            message_add_success( message );
        }

        free( buffer );
    }

    if ( rc < 0 ) {
        message_add_failure( message, self->id );
    }

    return rc;
}

// 注册网络事件
void session_add_event( struct session * self, int16_t ev )
{
    int8_t status = self->status;

    // 注册读事件
    // 不在等待读事件的正常会话
    if ( !( status & SESSION_EXITING )
        && ( ev & EV_READ ) && !( status & SESSION_READING ) ) {
        int32_t fd = -1;
        int16_t event = 0;
        if ( self->type != eSessionType_Shared ) {
            fd = self->fd;
            event = ev | self->setting.persist_mode;
        }
        event_set( self->evread, fd, event );
        event_set_callback( self->evread, channel_on_read, self );
        evsets_add( self->evsets, self->evread, self->setting.timeout_msecs );
        // 修改读状态
        self->status |= SESSION_READING;
    }

    // 注册写事件
    if ( ( ev & EV_WRITE ) && !( status & SESSION_WRITING ) ) {
        int32_t wait_for_shutdown = -1;

        // 在等待退出的会话上总是会添加10s的定时器
        if ( status & SESSION_EXITING ) {
            // 对端主机崩溃 + Socket缓冲区满的情况下
            // 会一直等不到EV_WRITE发生, 这时libevlite就出现了会话泄漏
            wait_for_shutdown = MAX_SECONDS_WAIT_FOR_SHUTDOWN;
        }

        event_set( self->evwrite, self->fd, ev );
        event_set_callback( self->evwrite, channel_on_write, self );
        evsets_add( self->evsets, self->evwrite, wait_for_shutdown );
        // 修改写状态
        self->status |= SESSION_WRITING;
    }
}

// 反注册网络事件
void session_del_event( struct session * self, int16_t ev )
{
    int8_t status = self->status;
    evsets_t sets = self->evsets;

    if ( ( ev & EV_READ ) && ( status & SESSION_READING ) ) {
        evsets_del( sets, self->evread );
        self->status &= ~SESSION_READING;
    }

    if ( ( ev & EV_WRITE ) && ( status & SESSION_WRITING ) ) {
        evsets_del( sets, self->evwrite );
        self->status &= ~SESSION_WRITING;
    }
}

void session_readd_event( struct session * self, int16_t ev )
{
    if ( ev & EV_READ ) {
        self->status &= ~SESSION_READING;
    }
    if ( ev & EV_WRITE ) {
        self->status &= ~SESSION_WRITING;
    }

    session_add_event( self, ev );
}

int32_t session_start_keepalive( struct session * self )
{
    int8_t status = self->status;
    evsets_t sets = self->evsets;

    if ( self->setting.keepalive_msecs >= 0 && !( status & SESSION_KEEPALIVING ) ) {
        event_set( self->evkeepalive, -1, 0 );
        event_set_callback( self->evkeepalive, channel_on_keepalive, self );
        evsets_add( sets, self->evkeepalive, self->setting.keepalive_msecs );

        self->status |= SESSION_KEEPALIVING;
    }

    return 0;
}

int32_t session_start_reconnect( struct session * self )
{
    evsets_t sets = self->evsets;

    if ( self->status & SESSION_EXITING ) {
        // 会话等待退出
        return -1;
    }

    if ( self->status & SESSION_WRITING ) {
        // 正在等待写事件的情况下
        return -2;
    }

    // NOTICE: 关联的会话, 网络层是不能关闭的, 必须交由逻辑层处理
    if ( self->type == eSessionType_Associate ) {
        self->fd = -1;
    }

    // 停止会话
    _stop( self );

    // 200毫秒后尝试重连, 避免进入重连死循环
    event_set( self->evwrite, -1, 0 );
    event_set_callback( self->evwrite, channel_on_reconnect, self );
    evsets_add( sets, self->evwrite, TRY_RECONNECT_INTERVAL );
    self->status |= SESSION_WRITING; // 让session忙起来

    return 0;
}

int32_t session_cancel_task( struct session * self, struct schedule_task * task )
{
    SLIST_REMOVE(
        &self->tasklist, task, schedule_task, tasklink );

    if ( task->evschedule != NULL ) {
        evsets_del( self->evsets, task->evschedule );
        event_destroy( task->evschedule );
        task->evschedule = NULL;
    }

    task->recycle( task->type, task->task, task->interval );
    free( task );
    return 0;
}

int32_t session_schedule_task( struct session * self, int32_t type, void * t, int32_t interval, taskrecycler_t recycle )
{
    if ( self->status & SESSION_EXITING ) {
        return -1;
    }

    struct schedule_task * task = (struct schedule_task *)calloc( 1, sizeof( struct schedule_task ) );
    if ( task == NULL ) {
        return -2;
    }

    task->task = task;
    task->type = type;
    task->recycle = recycle;
    task->interval = interval;
    task->session = self;
    task->evschedule = event_create();
    if ( task->evschedule == NULL ) {
        return -3;
    }

    // 压入链表
    SLIST_INSERT_HEAD( &self->tasklist, task, tasklink );
    // 定时
    event_set( task->evschedule, -1, 0 );
    event_set_callback( task->evschedule, channel_on_schedule, task );
    evsets_add( self->evsets, task->evschedule, task->interval );

    return 0;
}

int32_t session_shutdown( struct session * self )
{
    if ( !( self->status & SESSION_EXITING )
        && session_sendqueue_count( self ) > 0 ) {
        // 会话状态正常, 并且发送队列不为空
        // 尝试继续把未发送的数据发送出去, 在终止会话
        self->status |= SESSION_EXITING;

        // 优先把数据发出去
        session_del_event( self, EV_READ );
        session_add_event( self, EV_WRITE );

        return 1;
    }

    // 主动关闭连接
    return channel_shutdown( self );
}

int32_t session_end( struct session * self, sid_t id, int8_t recycle )
{
    // 由于会话已经从管理器中删除了
    // 会话中的ID已经非法

    // 清空发送队列
    uint32_t count = session_sendqueue_count( self );
    if ( count > 0 ) {
        syslog( LOG_WARNING,
            "%s(SID=%ld)'s Out-Message-List (%d) is not empty .", __FUNCTION__, id, count );

        for ( ; count > 0; --count ) {
            struct message * msg = NULL;
            QUEUE_POP( sendqueue ) ( &self->sendqueue, &msg );

            assert( msg != NULL && "QUEUE_POP() NULL Message" );

            // 检查消息是否可以销毁了
            message_add_failure( msg, id );
            if ( message_is_complete( msg ) ) {
                message_destroy( msg );
            }
        }
    }

    // 停止会话
    _stop( self );

    // 是否回收会话
    if ( recycle == 0 ) {
        _del_session( self );
    } else {
        _reset_session( self );
    }

    return 0;
}

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

struct slot {
    union {
        uint32_t next_free;
        struct session * session;
    };
    uint16_t version;
    uint8_t is_active;
};

inline int32_t _session_manager_expand( struct session_manager * self )
{
    if ( self->capacity >= MAX_SLOT_CAPACITY ) {
        syslog( LOG_ERR, "%s: Reached absolute maximum limit of 20-bit SID index (%u).", __FUNCTION__, MAX_SLOT_CAPACITY );
        return -1;
    }

    // 倍增扩容策略
    uint32_t old_capacity = self->capacity;
    uint32_t new_capacity = old_capacity * 2;
    if ( new_capacity > MAX_SLOT_CAPACITY ) {
        new_capacity = MAX_SLOT_CAPACITY;
    }

    // 核心：内存重新分配。原有数据会被完美保留。
    struct slot * new_pool = (struct slot *)realloc( self->table, new_capacity * sizeof( struct slot ) );
    if ( new_pool == NULL ) {
        syslog( LOG_ERR, "%s: realloc failed, Out Of Memory.", __FUNCTION__ );
        return -2;
    }
    self->table = new_pool;

    // 初始化新扩容出来的尾部槽位
    for ( uint32_t i = old_capacity; i < new_capacity; ++i ) {
        self->table[i].version = 0;
        self->table[i].is_active = 0;
        self->table[i].next_free = i + 1;
    }
    self->table[new_capacity - 1].next_free = INVALID_SLOT_INDEX;

    // 将扩容出来的第一个空位接管为新的 free_head
    self->capacity = new_capacity;
    self->free_head = old_capacity;

    syslog( LOG_INFO, "%s(Index=%u): Session pool dynamically expanded from %u to %u.", __FUNCTION__, self->index, old_capacity, self->capacity );

    return 0;
}

struct session_manager * session_manager_create( uint8_t index, uint32_t size )
{
    struct session_manager * self = NULL;

    if ( size == 0 ) size = 1024;
    if ( size > MAX_SLOT_CAPACITY ) size = MAX_SLOT_CAPACITY;

    self = (struct session_manager *)calloc( 1, sizeof(struct session_manager) );
    assert( self != NULL && "allocate session_manager failed" );

    self->table = (struct slot *)calloc( size, sizeof(struct slot) );
    assert( self->table != NULL && "allocate struct slot failed" );

    self->count = 0;
    self->free_head = 0;
    self->index = index;
    self->capacity = size;

    // 初始化空闲链表
    for ( uint32_t i = 0; i < size; ++i ) {
        self->table[i].version = 0;
        self->table[i].is_active = 0;
        self->table[i].next_free = i + 1;
    }
    self->table[size - 1].next_free = INVALID_SLOT_INDEX;

    self->recyclesize = 0;
    STAILQ_INIT( &self->recyclelist );

    return self;
}

struct session * session_manager_alloc( struct session_manager * self )
{
    if ( unlikely( self->free_head == INVALID_SLOT_INDEX ) ) {
        if ( _session_manager_expand( self ) != 0 ) {
            return NULL;
        }
    }

    struct session * session = NULL;

#ifndef USE_REUSESESSION
    session = _new_session();
#else
    // 优先到回收队列中取
    session = STAILQ_FIRST( &self->recyclelist );
    if ( session == NULL ) {
        session = _new_session();
    } else {
        --self->recyclesize;
        STAILQ_REMOVE_HEAD( &self->recyclelist, recyclelink );
    }
#endif

    if ( unlikely( session == NULL ) ) return NULL;

    // 获取空槽位
    uint32_t seq = self->free_head;
    struct slot * slot = &self->table[seq];

    // 推进空闲链表
    self->free_head = slot->next_free;

    // 激活
    ++self->count;
    slot->is_active = 1;
    slot->session = session;

    // 生成sid
    sid_t sid = MAKE_SID( self->index, slot->version, seq );

    // 绑定
    session->id = sid;
    session->manager = self;

    return session;
}

struct session * session_manager_get( struct session_manager * self, sid_t id )
{
    uint32_t seq = SID_SEQ( id );
    uint16_t ver = SID_VERSION( id );

    if ( unlikely( seq >= self->capacity ) ) {
        return NULL;
    }

    struct slot * slot = &self->table[seq];

    if ( slot->is_active
        && slot->version == ver
        && slot->session != NULL ) {
        return slot->session;
    }

    return NULL;
}

int32_t session_manager_foreach( struct session_manager * self, int32_t ( *func )( void *, struct session * ), void * context )
{
    int32_t count = 0;

    for ( uint32_t i = 0; i < self->capacity; ++i ) {
        struct slot * slot = &self->table[i];
        if ( slot->is_active && slot->session != NULL ) {
            if ( func( context, slot->session ) != 0 ) {
                return count;
            }
            ++count;
        }
    }

    return count;
}

int32_t session_manager_remove( struct session_manager * self, struct session * session )
{
    if ( unlikely( session == NULL ) ) return -1;

    uint32_t seq = SID_SEQ( session->id );
    if ( unlikely( seq >= self->capacity ) ) return -1;

    struct slot * slot = &self->table[ seq ];
    if ( unlikely( !slot->is_active || slot->session != session ) ) {
        return -1;
    }

    // 关闭
    slot->is_active = 0;
    // 版本号增加
    ++slot->version;
    // 归还
    slot->next_free = self->free_head;
    --self->count;
    self->free_head = seq;
    // 清空会话
    session->id = 0;
    session->manager = NULL;

    return 0;
}

void session_manager_recycle( struct session_manager * self, struct session * session )
{
    if ( session->manager == self && session->id != 0 ) {
        session_manager_remove( self, session );
    } else {
        ++self->recyclesize;
        STAILQ_INSERT_TAIL( &self->recyclelist, session, recyclelink );
    }
}

void session_manager_destroy( struct session_manager * self )
{
    if ( self->count > 0 ) {
        syslog( LOG_WARNING,
            "%s(Index=%u): the number of residual active session is %u .",
            __FUNCTION__, self->index, self->count );
    }

    // 安全关闭所有会话
    for ( uint32_t i = 0; i < self->capacity; ++i ) {
        struct slot * slot = &self->table[i];
        if ( slot->is_active && slot->session != NULL ) {
            struct session * s = slot->session;
            session_call_shutdown( s, 0 );
            session_end( s, s->id, 0 );
            slot->is_active = 0;
            slot->session = NULL;
        }
    }

    // 排空并释放 session 对象的回收队列
    struct session *session;
    while ( (session = STAILQ_FIRST( &self->recyclelist )) != NULL ) {
        STAILQ_REMOVE_HEAD( &self->recyclelist, recyclelink );
        _del_session( session );
    }

    // 3. 释放 SlotMap 核心物理数组
    if ( self->table != NULL ) {
        free( self->table );
        self->table = NULL;
    }

    self->capacity = 0;
    self->count = 0;
    self->recyclesize = 0;

    free( self );
}
