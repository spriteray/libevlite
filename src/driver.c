
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <syslog.h>

#include "utils.h"
#include "message.h"
#include "session.h"
#include "ephashtable.h"
#include "network-internal.h"
#include "event-internal.h"

#include "driver.h"

static inline size_t _kcp_max_send( struct driver * self );
static inline int32_t _kcp_can_send( struct driver * self );
static inline uint32_t _kcp_milliseconds( struct driver * self );
static inline struct session * _get_session( struct driver * self );
static inline void _kcp_schedule( struct driver * self, int32_t timeout );

static void _kcp_refresh_state( struct driver * self );
static void _kcp_timer( int32_t fd, int16_t ev, void * arg );
static int32_t _kcp_output( const char * buf, int32_t len, ikcpcb * kcp, void * user );

struct driver * driver_create( struct session * s, struct buffer * buffer, const options_t * options )
{
    struct driver * self = (struct driver *)calloc( 1, sizeof( struct driver ) );
    if ( self != NULL ) {
        uint32_t conv = ikcp_getconv( buffer_data( buffer ) );

        // 创建定时器
        self->timer = event_create();
        if ( self->timer == NULL ) {
            driver_destroy( self );
            return NULL;
        }

        // 创建kcp对象
        self->kcp = ikcp_create( conv, s );
        if ( self->kcp == NULL ) {
            driver_destroy( self );
            return NULL;
        }

        // 设置时间戳
        self->epoch = event_get_current( self->timer );
        // 设置kcp的极速模式
        // 1 - 启动nodelay模式
        // 1 - 关闭流量控制
        ikcp_nodelay( self->kcp,
            1, options->interval, options->resend, 1 );              // KCP极速模式
        ikcp_setmtu( self->kcp, options->mtu );                      // 最大传输单元
        ikcp_wndsize( self->kcp, options->sndwnd, options->rcvwnd ); // 窗口大小
        self->kcp->stream = options->stream;                         // 流模式
        self->kcp->rx_minrto = options->minrto;                      // 最小重传超时时间
        self->kcp->dead_link = options->deadlink;                    // 最大重传次数
        ikcp_setoutput( self->kcp, _kcp_output );
    }

    return self;
}

void driver_destroy( struct driver * self )
{
    struct session * session = _get_session( self );

    if ( self->timer != NULL ) {
        if ( session != NULL
            && ( session->status & SESSION_SCHEDULING ) ) {
            evsets_del( session->evsets, self->timer );
            session->status &= ~SESSION_SCHEDULING;
        }

        event_destroy( self->timer );
        self->timer = NULL;
    }

    if ( self->entry != NULL ) {
        ephashtable_remove(
            self->entry->hashtable, session->host, session->port );
        self->entry = NULL;
    }

    if ( self->kcp != NULL ) {
        ikcp_release( self->kcp );
        self->kcp = NULL;
    }

    free( self );
    self = NULL;
}

int32_t driver_set_endpoint( struct driver * self, struct ephashtable * table, int32_t family, const char * host, uint16_t port )
{
    // 添加到ep管理器中
    self->entry = (struct udpentry *)ephashtable_append( table, host, port );
    if ( self->entry == NULL ) {
        return -1;
    }

    self->entry->hashtable = table;
    self->entry->session = _get_session( self );
    convert_endpoint( &( self->addr ), family, host, port );

    return 0;
}

ssize_t driver_input( struct driver * self, struct buffer * in, struct buffer * out )
{
    size_t len = 0;

    // 输入kcp
    ikcp_input( self->kcp, buffer_data( in ), buffer_length( in ) );

    // 接收逻辑包到inbuffer中
    while ( 1 ) {
        ssize_t peeksize = ikcp_peeksize( self->kcp );
        if ( peeksize < 0 ) {
            break;
        }
        // 空消息也需要消费, 否则会挡住后续消息
        if ( peeksize == 0 ) {
            if ( ikcp_recv( self->kcp, NULL, 0 ) < 0 ) break;
            continue;
        }

        if ( buffer_reserve( out, peeksize ) != 0 ) {
            return -2;
        }
        peeksize = ikcp_recv( self->kcp,
            out->buffer + out->length, (int32_t)peeksize );
        if ( peeksize <= 0 ) {
            break;
        }

        len += peeksize;
        out->length += peeksize;
        // 检验self->buffer中的conv是否变化
        // 变化了直接替换kcp句柄中的conv
        // driver_set_conv( self, ikcp_getconv( buffer_data( in ) ) );
    }

    // 读事件, 立刻update()不需要等下一轮
    _kcp_refresh_state( self );

    return len;
}

ssize_t driver_transmit( struct session * session )
{
    ssize_t writen = 0;
    struct driver * driver = session->driver;

    for ( ; session_sendqueue_count( session ) > 0; ) {
        if ( !_kcp_can_send( driver ) ) {
            break;
        }

        // 取出第一条消息
        int32_t rc = 0;
        struct message * message = NULL;
        QUEUE_TOP( sendqueue ) ( &session->sendqueue, &message );
        size_t length = message_get_length( message ) - session->msgoffset;
        if ( length > _kcp_max_send( driver ) ) {
            if ( !driver->kcp->stream ) {
                // 消息模式超过KCP分片上限, 丢弃该消息
                session->msgoffset = 0;
                QUEUE_POP( sendqueue ) ( &session->sendqueue, &message );
                message_add_failure( message, session->id );
                if ( message_is_complete( message ) ) {
                    message_destroy( message );
                }
                syslog( LOG_WARNING, "%s(SID=%ld) failed, the message(LENGTH=%lu) is too large .", __FUNCTION__, session->id, length );
                continue;
            }
            // 流模式分段发送
            length = _kcp_max_send( driver );
        }
        // 发送数据给KCP
        if ( length > 0 ) {
            rc = ikcp_send( driver->kcp,
                message_get_buffer( message ) + session->msgoffset, (int32_t)length );
        }
        if ( rc < 0 ) {
            return -1;
        }
        if ( rc == 0 && length > 0 ) {
            break;
        }

        writen += rc;
        session->msgoffset += rc;
        if ( session->msgoffset < message_get_length( message ) ) {
            continue;
        }
        session->msgoffset = 0;
        QUEUE_POP( sendqueue ) ( &session->sendqueue, &message );
        message_add_success( message );
        if ( message_is_complete( message ) ) {
            message_destroy( message );
        }
    }

    // 写事件, 立刻update()不需要等下一轮
    _kcp_refresh_state( driver );

    return writen;
}

ssize_t driver_send( struct session * session, char * buf, size_t nbytes )
{
    int32_t rc = 0;
    struct driver * d = session->driver;

    if ( nbytes > _kcp_max_send( d ) ) {
        // 消息模式超过KCP分片上限
        if ( !d->kcp->stream ) {
            return -1;
        }
        nbytes = _kcp_max_send( d );
    }
    if ( _kcp_can_send( d ) ) {
        // 发送
        rc = ikcp_send( d->kcp, buf, (int32_t)nbytes );
    }

    // 写事件, 立刻update()不需要等下一轮
    _kcp_refresh_state( d );
    return rc < 0 ? 0 : rc;
}

int32_t driver_pending( struct driver * self )
{
    return ikcp_waitsnd( self->kcp );
}

int32_t driver_set_mtu( struct driver * self, int32_t mtu )
{
    // KCP's minimum and the maximum portable UDP payload size.
    if ( mtu < 50 || mtu > 65507 ) return -1;

    // Setting the active value also cancels an older deferred request.
    if ( mtu == self->kcp->mtu ) {
        self->mtu = 0;
        return 0;
    }
    if ( ikcp_waitsnd( self->kcp ) > 0 ) {
        self->mtu = mtu;
        return 0;
    }

    int32_t rc = ikcp_setmtu( self->kcp, mtu );
    if ( rc == 0 ) {
        self->mtu = 0;
    }

    return rc;
}

void driver_set_minrto( struct driver * self, int32_t minrto )
{
    self->kcp->rx_minrto = minrto;
}

void driver_set_wndsize( struct driver * self, int32_t sndwnd, int32_t rcvwnd )
{
    ikcp_wndsize( self->kcp, sndwnd, rcvwnd );
}

void driver_set_conv( struct driver * self, uint32_t conv )
{
    self->kcp->conv = conv;
}

void _kcp_refresh_state( struct driver * self )
{
    // Old segments retain their original size until acknowledged. Only resize
    // after both KCP send queues drain; allocation failure retries next time.
    if ( self->mtu != 0
        && ikcp_waitsnd( self->kcp ) == 0 ) {
        if ( ikcp_setmtu( self->kcp, self->mtu ) == 0 ) {
            self->mtu = 0;
        }
    }

    // 刷新kcp的状态
    uint32_t current = _kcp_milliseconds( self );
    ikcp_update( self->kcp, current );
    ikcp_flush( self->kcp );
    _kcp_schedule( self, ikcp_check( self->kcp, current ) - current );
}

uint32_t _kcp_milliseconds( struct driver * self )
{
    return (uint32_t)( event_get_current( self->timer ) - self->epoch );
}

void _kcp_timer( int32_t fd, int16_t ev, void * arg )
{
    struct session * session = (struct session *)arg;

    // 状态
    session->status &= ~SESSION_SCHEDULING;

    // 刷新kcp的状态
    _kcp_refresh_state( session->driver );

    // Only the timer reports dead links: send/input callers may still use the session.
    if ( session->driver->kcp->state == (IUINT32)-1 ) {
        session_error( session, eIOError_KcpDeadLink, 0 );
        return;
    }

    // 消费会话层的发送队列
    if ( session->status & SESSION_WRITING ) {
        session_flush_sendqueue( session );
    }
}

struct session * _get_session( struct driver * self )
{
    if ( self->kcp != NULL ) {
        return (struct session *)self->kcp->user;
    }
    return NULL;
}

void _kcp_schedule( struct driver * self, int32_t timeout )
{
    struct session * session = _get_session( self );

    if ( timeout < 0 ) {
        timeout = EVENTSET_PRECISION( session->evsets );
    }

    // 已有更早的定时器, 不必重排
    uint32_t deadline = _kcp_milliseconds( self ) + timeout;
    if ( session->status & SESSION_SCHEDULING ) {
        if ( (int32_t)( deadline - self->deadline ) >= 0 ) {
            return;
        }
        evsets_del( session->evsets, self->timer );
        session->status &= ~SESSION_SCHEDULING;
    }

    // 加入定时器
    event_set( self->timer, -1, 0 );
    event_set_callback( self->timer, _kcp_timer, session );
    evsets_add( session->evsets, self->timer, timeout );
    self->deadline = deadline;
    session->status |= SESSION_SCHEDULING;
}

int32_t _kcp_output( const char * buf, int32_t len, ikcpcb * kcp, void * user )
{
    struct session * session = (struct session *)user;
    struct driver * driver = session->driver;

    // kcp出错的情况下，就重传呗, 没啥大不了的
    ssize_t writen = sendto( session->fd, buf, len,
        MSG_DONTWAIT, (struct sockaddr *)&driver->addr, endpoint_length( &driver->addr ) );
    if ( writen < 0 ) {
        if ( errno == EINTR
            || errno == EAGAIN || errno == EWOULDBLOCK ) {
            writen = 0;
        }
    }

    return (int32_t)writen;
}

size_t _kcp_max_send( struct driver * self )
{
    // ikcp_send()单次最多分片数(IKCP_WND_RCV - 1)
    return (size_t)127 * self->kcp->mss;
}

int32_t _kcp_can_send( struct driver * self )
{
    return (uint32_t)ikcp_waitsnd( self->kcp ) < 4 * self->kcp->snd_wnd;
}

