
#include <stdio.h>
#include <assert.h>
#include <syslog.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

#include "iolayer.h"
#include "channel.h"
#include "network-internal.h"
#include "session.h"

static struct session * _new_session();
static int32_t _reset_session( struct session * self );
static int32_t _del_session( struct session * self );
static inline void _stop( struct session * self );

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
    struct session * self = NULL;

    self = calloc( 1, sizeof(struct session) );
    if ( self == NULL )
    {
        return NULL;
    }

    // 初始化网络事件
    self->evread = event_create();
    self->evwrite = event_create();
    self->evkeepalive = event_create();
    if ( self->evkeepalive == NULL
            || self->evread == NULL || self->evwrite == NULL )
    {
        _del_session( self );
        return NULL;
    }

    // 初始化发送队列
    if ( QUEUE_INIT(sendqueue)(&self->sendqueue, DEFAULT_SENDQUEUE_SIZE) != 0 )
    {
        _del_session( self );
        return NULL;
    }

    return self;
}

int32_t _reset_session( struct session * self )
{
    // 销毁网络事件
    if ( self->evread )
    {
        event_reset( self->evread );
    }
    if ( self->evwrite )
    {
        event_reset( self->evwrite );
    }
    if ( self->evkeepalive )
    {
        event_reset( self->evkeepalive );
    }

    buffer_erase( &self->inbuffer,
            buffer_length(&self->inbuffer) );
    QUEUE_RESET(sendqueue)(&self->sendqueue);

    return 0;
}

int32_t _del_session( struct session * self )
{
    // 销毁网络事件
    if ( self->evread )
    {
        event_destroy( self->evread );
        self->evread = NULL;
    }
    if ( self->evwrite )
    {
        event_destroy( self->evwrite );
        self->evwrite = NULL;
    }
    if ( self->evkeepalive )
    {
        event_destroy( self->evkeepalive );
        self->evkeepalive = NULL;
    }

    buffer_clear( &self->inbuffer );
    QUEUE_CLEAR(sendqueue)(&self->sendqueue);
    free( self );

    return 0;
}

// 会话停止(删除网络事件以及关闭描述符)
void _stop( struct session * self )
{
    evsets_t sets = self->evsets;

    // 删除网络事件
    if ( self->status&SESSION_READING )
    {
        evsets_del( sets, self->evread );
        self->status &= ~SESSION_READING;
    }
    if ( self->status&SESSION_WRITING )
    {
        evsets_del( sets, self->evwrite );
        self->status &= ~SESSION_WRITING;
    }
    if ( self->status&SESSION_KEEPALIVING )
    {
        evsets_del( sets, self->evkeepalive );
        self->status &= ~SESSION_KEEPALIVING;
    }

    // 清空接收缓冲区
    buffer_erase( &self->inbuffer, buffer_length(&self->inbuffer) );

    // 关闭描述符
    if ( self->fd > 0 )
    {
        close( self->fd );
        self->fd = -1;
    }
}

//
ssize_t _send_only( struct session * self, char * buf, size_t nbytes )
{
    ssize_t ntry = 0;

    if ( unlikely( self->status&SESSION_EXITING ) )
    {
        // 等待关闭的连接
        return -1;
    }

    // 判断session是否繁忙
    if ( !(self->status&SESSION_WRITING)
            && session_sendqueue_count(self) == 0 )
    {
        assert( self->msgoffset == 0 && "SendQueue Offset Invalid" );

        // 直接发送
        ntry = channel_send( self, buf, nbytes );
        if ( ntry < 0 )
        {
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
    if ( ntry >= 0 && ntry < (ssize_t)nbytes )
    {
        // 未全部发送成功的情况下

        // 添加到发送队列中
        // ntry == 0有两种情况:1. 繁忙; 2. 发送出错
        self->msgoffset += ntry;
        QUEUE_PUSH(sendqueue)(&self->sendqueue, &message);
        // 增加写事件
        session_add_event( self, EV_WRITE );
    }

    return ntry;
}

ssize_t _send_buffer( struct session * self, char * buf, size_t nbytes )
{
    // 发送
    ssize_t ntry = _send_only( self, buf, nbytes );
    if ( ntry >= 0 && ntry < (ssize_t)nbytes )
    {
        // 未全部发送成功的情况下

        // 创建message, 添加到发送队列中
        struct message * message = message_create();
        if ( message == NULL )
        {
            return -2;
        }
        message_add_buffer( message, buf+ntry, nbytes-ntry );
        message_add_receiver( message, self->id );
        QUEUE_PUSH(sendqueue)(&self->sendqueue, &message);
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

    self->fd        = fd;
    self->type      = type;
    self->evsets    = sets;

    // TODO: 设置默认的最大接收缓冲区

    // 不需要每次开始会话的时候初始化
    // 只需要在manager创建会话的时候初始化一次，即可

    self->service.start( self->context );

    // 关注读事件, 按需开启保活心跳
    session_add_event( self, EV_READ );
    session_start_keepalive( self );

    return 0;
}

void session_set_iolayer( struct session * self, void * iolayer )
{
    self->iolayer = iolayer;
}

void session_set_endpoint( struct session * self, char * host, uint16_t port )
{
    self->port = port;
    strncpy( self->host, host, INET_ADDRSTRLEN );
}

ssize_t session_send( struct session * self, char * buf, size_t nbytes )
{
    ssize_t rc = -1;
    char * _buf = buf;
    size_t _nbytes = nbytes;

    // 数据改造(加密 or 压缩)
    if ( unlikely( self->service.transform != NULL ) )
    {
        _buf = self->service.transform(
                self->context, (const char *)buf, &_nbytes );
    }

    if ( likely( _buf != NULL ) )
    {
        // 发送数据
        // TODO: _send_buffer()可以根据具体情况决定是否copy内存
        rc = _send_buffer( self, _buf, _nbytes );

        if ( _buf != buf )
        {
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
    if ( unlikely( self->service.transform != NULL ) )
    {
        buffer = self->service.transform(
                self->context, (const char *)buf, &nbytes );
    }

    if ( buffer == buf )
    {
        // 消息未进行改造

        // 添加到会话的发送列表中
        rc = QUEUE_PUSH(sendqueue)(&self->sendqueue, &message);
        if ( rc == 0 )
        {
            // 注册写事件
            session_add_event( self, EV_WRITE );
        }
    }
    else if ( buffer != NULL )
    {
        // 消息改造成功

        rc = _send_buffer( self, buffer, nbytes );
        if ( rc >= 0 )
        {
            // 改造后的消息已经单独发送
            message_add_success( message );
        }

        free( buffer );
    }

    if ( rc < 0 )
    {
        message_add_failure( message, self->id );
    }

    return rc;
}

// 注册网络事件
void session_add_event( struct session * self, int16_t ev )
{
    int8_t status = self->status;
    evsets_t sets = self->evsets;

    // 注册读事件
    // 不在等待读事件的正常会话
    if ( !(status&SESSION_EXITING)
            && (ev&EV_READ) && !(status&SESSION_READING) )
    {
        event_set( self->evread, self->fd, ev );
        event_set_callback( self->evread, channel_on_read, self );
        evsets_add( sets, self->evread, self->setting.timeout_msecs );

        self->status |= SESSION_READING;
    }

    // 注册写事件
    if ( (ev&EV_WRITE) && !(status&SESSION_WRITING) )
    {
        int32_t wait_for_shutdown = 0;

        // 在等待退出的会话上总是会添加10s的定时器
        if ( status&SESSION_EXITING )
        {
            // 对端主机崩溃 + Socket缓冲区满的情况下
            // 会一直等不到EV_WRITE发生, 这时libevlite就出现了会话泄漏
            wait_for_shutdown = MAX_SECONDS_WAIT_FOR_SHUTDOWN;
        }

        event_set( self->evwrite, self->fd, ev );
        event_set_callback( self->evwrite, channel_on_write, self );
        evsets_add( sets, self->evwrite, wait_for_shutdown );

        self->status |= SESSION_WRITING;
    }
}

// 反注册网络事件
void session_del_event( struct session * self, int16_t ev )
{
    int8_t status = self->status;
    evsets_t sets = self->evsets;

    if ( (ev&EV_READ) && (status&SESSION_READING) )
    {
        evsets_del( sets, self->evread );
        self->status &= ~SESSION_READING;
    }

    if ( (ev&EV_WRITE) && (status&SESSION_WRITING) )
    {
        evsets_del( sets, self->evwrite );
        self->status &= ~SESSION_WRITING;
    }
}

int32_t session_start_keepalive( struct session * self )
{
    int8_t status = self->status;
    evsets_t sets = self->evsets;

    if ( self->setting.keepalive_msecs > 0 && !(status&SESSION_KEEPALIVING) )
    {
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

    if ( self->status&SESSION_EXITING )
    {
        // 会话等待退出,
        return -1;
    }

    if ( self->status&SESSION_WRITING )
    {
        // 正在等待写事件的情况下
        return -2;
    }

    // 停止会话
    _stop( self );

    // 2秒后尝试重连, 避免忙等
    event_set( self->evwrite, -1, 0 );
    event_set_callback( self->evwrite, channel_on_reconnect, self );
    evsets_add( sets, self->evwrite, TRY_RECONNECT_INTERVAL );
    self->status |= SESSION_WRITING;            // 让session忙起来

    return 0;
}

int32_t session_shutdown( struct session * self )
{
    if ( !(self->status&SESSION_EXITING)
            && session_sendqueue_count(self) > 0 )
    {
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
    uint32_t count = session_sendqueue_count(self);
    if ( count > 0 )
    {
        syslog(LOG_WARNING,
                "%s(SID=%ld)'s Out-Message-List (%d) is not empty .", __FUNCTION__, id, count );

        for ( ; count > 0; --count )
        {
            struct message * msg = NULL;
            QUEUE_POP(sendqueue)( &self->sendqueue, &msg );

            assert( msg != NULL && "QUEUE_POP() NULL Message" );

            // 检查消息是否可以销毁了
            message_add_failure( msg, id );
            if ( message_is_complete(msg) )
            {
                message_destroy( msg );
            }
        }
    }

    // 停止会话
    _stop( self );

    // 是否回收会话
    if ( recycle == 0 )
    {
        _del_session( self );
    }
    else
    {
        _reset_session( self );
    }

    return 0;
}

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

struct hashnode
{
    struct hashnode * next;
    struct session * session;
};

struct hashtable
{
    uint32_t size;
    uint32_t count;
    struct hashnode * entries;
};

static inline int32_t _init_table( struct hashtable * table, uint32_t size );
static inline struct hashnode * _find_table( struct hashtable * table, sid_t id, int32_t flag );
static inline int32_t _append_session( struct hashtable * table, struct session * s );
static inline struct session * _find_session( struct hashtable * table, sid_t id );
static inline int32_t _remove_session( struct hashtable * table, struct session * s );

int32_t _init_table( struct hashtable * table, uint32_t size )
{
    assert( (size&(size-1)) == 0 );

    table->count = 0;
    table->size = size;

    table->entries = calloc( size, sizeof(struct hashnode) );
    if ( table->entries == NULL )
    {
        return -1;
    }

    return 0;
}

struct hashnode * _find_table( struct hashtable * table, sid_t id, int32_t flag )
{
    int32_t bucket = SID_SEQ(id) & (table->size-1);

    struct hashnode * node = NULL;
    struct hashnode * entries = table->entries + bucket;

    for ( ; entries != NULL; entries = entries->next )
    {
        if ( entries->session != NULL )
        {
            if ( entries->session->id == id )
            {
                node = entries;
                break;
            }
        }
        else if ( node == NULL )
        {
            node = entries;
        }
    }

    if ( node == NULL && flag != 0 )
    {
        node = malloc( sizeof(struct hashnode) );

        if ( node != NULL )
        {
            node->next = NULL;
            node->session = NULL;
            entries->next = node;
        }
    }

    return node;
}

int32_t _append_session( struct hashtable * table, struct session * s )
{
    struct hashnode * node = _find_table( table, s->id, 1 );

    if ( unlikely(node == NULL) )
    {
        return -1;
    }

    if ( unlikely( node->session != NULL && node->session->id == s->id ) )
    {
        syslog(LOG_WARNING, "%s(Index=%d): the SID (Seq=%u, Sid=%ld) conflict !",
                __FUNCTION__, (int32_t)SID_INDEX(s->id), (uint32_t)SID_SEQ(s->id), s->id );
        return -2;
    }

    ++table->count;
    node->session = s;

    return 0;
}

struct session * _find_session( struct hashtable * table, sid_t id )
{
    struct hashnode * node = _find_table( table, id, 0 );

    if ( node == NULL )
    {
        return NULL;
    }
    else if ( node->session == NULL )
    {
        return NULL;
    }

    assert( node->session->id == id );
    return node->session;
}

int32_t _remove_session( struct hashtable * table, struct session * s )
{
    struct hashnode * node = _find_table( table, s->id, 0 );

    if ( node == NULL )
    {
        return -1;
    }
    else if ( node->session == NULL )
    {
        return -2;
    }

    assert( node->session == s );

    --table->count;
    node->session = NULL;

    return 0;
}

struct session_manager * session_manager_create( uint8_t index, uint32_t size )
{
    struct session_manager * self = NULL;

    self = calloc( 1, sizeof(struct session_manager)+sizeof(struct hashtable) );
    if ( self == NULL )
    {
        return NULL;
    }

    size = nextpow2( size );

    self->autoseq = 0;
    self->index = index;
    self->table = (struct hashtable *)( self + 1 );
    // 初始化回收队列
    STAILQ_INIT( &self->recyclelist );

    if ( _init_table(self->table, size) != 0 )
    {
        free( self );
        self = NULL;
    }

    return self;
}

uint32_t session_manager_count( struct session_manager * self )
{
    return self->table->count;
}

struct session * session_manager_alloc( struct session_manager * self )
{
    sid_t id = 0;
    struct session * session = NULL;

    // 生成sid
    id = self->index+1;
    id <<= 32;
    id |= self->autoseq++;
    id &= SID_MASK;

#ifndef USE_REUSESESSION
    session = _new_session();
#else
    // 优先到回收队列中取
    session = STAILQ_FIRST( &self->recyclelist );
    if ( session == NULL )
    {
        session = _new_session();
    }
    else
    {
        STAILQ_REMOVE_HEAD( &self->recyclelist, recyclelink );
    }
#endif

    if ( session != NULL )
    {
        session->id = id;
        session->manager = self;

        // 添加会话
        // TODO: 是否可以拆分, generate() and session_start()
        if ( _append_session(self->table, session) != 0 )
        {
            _del_session( session );
            session = NULL;
        }
    }

    return session;
}

struct session * session_manager_get( struct session_manager * self, sid_t id )
{
    return _find_session( self->table, id );
}

int32_t session_manager_foreach( struct session_manager * self, int32_t (*func)(void *, struct session *), void * context )
{
    int32_t i = 0, rc = 0;

    for ( i = 0; i < self->table->size; ++i )
    {
        struct hashnode * node = self->table->entries + i;

        for ( ; node;  )
        {
            struct session * s = node->session;
            if ( s != NULL )
            {
                if ( func( context, s ) != 0 )
                {
                    return rc;
                }

                ++rc;
            }

            node = node->next;
        }
    }

    return rc;
}

int32_t session_manager_remove( struct session_manager * self, struct session * session )
{
    if ( _remove_session(self->table, session) != 0 )
    {
        return -1;
    }

    // 会话数据清空
    session->id = 0;
    session->manager = NULL;

    return 0;
}

void session_manager_recycle( struct session_manager * self, struct session * session )
{
    STAILQ_INSERT_TAIL( &self->recyclelist, session, recyclelink );
}

void session_manager_destroy( struct session_manager * self )
{
    uint32_t i = 0;
    struct session * session = NULL;

    if ( self->table->count > 0 )
    {
        syslog( LOG_WARNING,
                "%s(Index=%u): the number of residual active session is %d .", __FUNCTION__, self->index, self->table->count );
    }

    for ( i = 0; i < self->table->size; ++i )
    {
        struct hashnode * head = self->table->entries + i;
        struct hashnode * node = self->table->entries + i;

        for ( ; node; )
        {
            struct hashnode * n = node;
            struct session * s = n->session;

            // 下个节点
            node = n->next;

            // 销毁会话
            if ( s != NULL )
            {
                // 避免触发逻辑层的回调
                session_call_shutdown( s, 0 );
                session_end( s, s->id, 0 );
                n->session = NULL;
            }

            // head是table创建的, 所以不需要销毁
            if ( head != n ) free( n );
        }
    }

    // 销毁回收队列
    for ( session = STAILQ_FIRST( &self->recyclelist ); session != NULL; )
    {
        struct session * next = STAILQ_NEXT( session, recyclelink );

        _del_session( session );
        session = next;
    }

    if ( self->table->entries != NULL )
    {
        free( self->table->entries );
        self->table->entries = NULL;
    }

    free( self );
}
