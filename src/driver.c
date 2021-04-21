
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>

#include "utils.h"
#include "message.h"
#include "session.h"
#include "event-internal.h"

#include "driver.h"

static inline int32_t _kcp_can_send( struct driver * self );
static inline uint32_t _kcp_milliseconds( struct driver * self );
static inline struct session * _get_session( struct driver * self );
static inline void _kcp_schedule( struct driver * self, int32_t timeout );

static void _kcp_refresh_state( struct driver * self );
static void _kcp_timer( int32_t fd, int16_t ev, void * arg );
static int32_t _kcp_output( const char * buf, int32_t len, ikcpcb * kcp, void * user );

struct driver * driver_create( struct session * s, struct buffer * buffer )
{
    struct driver * self = (struct driver *)calloc( 1, sizeof( struct driver ) );
    if ( self != NULL )
    {
        uint32_t conv = ikcp_getconv( buffer_data(buffer) );

        // 创建定时器
        self->timer = event_create();
        if ( self->timer == NULL )
        {
            driver_destroy( self );
            return NULL;
        }

        // 创建kcp对象
        self->kcp = ikcp_create( conv, s );
        if ( self->kcp == NULL )
        {
            driver_destroy( self );
            return NULL;
        }

        self->epoch = milliseconds();
        buffer_swap( &self->buffer, buffer );

        // 设置kcp的属性
        // 1 - 启动nodelay模式
        // 40 - 内部的interval(8ms的倍数)
        // 2 - 2次ACK将会被重传
        // 1 - 关闭流量控制
        ikcp_nodelay( self->kcp, 1, 40, 2, 1 );
        // 定制属性(逻辑层无法修改)
        self->kcp->stream = 1;          // 流模式
        self->kcp->dead_link = 50;      // 最大重传次数
        ikcp_setoutput( self->kcp, _kcp_output );
    }

    return self;
}

void driver_destroy( struct driver * self )
{
    if ( self->timer != NULL )
    {
        struct session * session = _get_session(self);

        if ( session != NULL
                && (session->status&SESSION_SCHEDULING) )
        {
            evsets_del( session->evsets, self->timer );
            session->status &= ~SESSION_SCHEDULING;
        }

        event_destroy( self->timer );
        self->timer = NULL;
    }

    if ( self->kcp != NULL )
    {
        ikcp_release( self->kcp );
        self->kcp = NULL;
    }

    buffer_clear( &self->buffer );

    free( self );
    self = NULL;
}

ssize_t driver_input( struct driver * self, struct buffer * buffer )
{
    size_t len = 0;

    // 输入kcp
    ikcp_input( self->kcp,
            buffer_data(&self->buffer), buffer_length(&self->buffer) );

    // 接收逻辑包到inbuffer中
    while ( 1 )
    {
        ssize_t peeksize = ikcp_peeksize( self->kcp );
        if ( peeksize <= 0 )
        {
            break;
        }

        buffer_reserve( buffer, peeksize );
        peeksize = ikcp_recv( self->kcp,
                buffer->buffer + buffer->length, (int32_t)peeksize );
        if ( peeksize <= 0 )
        {
            break;
        }

        len += peeksize;
        buffer->length += peeksize;
    }

    // 清空buffer
    buffer_erase( &self->buffer, -1 );

    // 读事件, 立刻update()不需要等下一轮
    _kcp_refresh_state( self );

    return len;
}

ssize_t driver_receive( struct session * session )
{
    // 从socket中读取数据到udp缓存中
    ssize_t nread = buffer_read(
            &(session->driver->buffer), session->fd, 0 );

    // 重组
    // udp驱动处理输入的数据
    driver_input( session->driver, &session->inbuffer );

    //
    return nread;
}

ssize_t driver_transmit( struct session * session )
{
    ssize_t writen = 0;

    for ( ; session_sendqueue_count(session) > 0; )
    {
        struct message * message = NULL;

        // 取出第一条消息
        QUEUE_TOP(sendqueue)( &session->sendqueue, &message );
        // 发送数据给KCP
        ssize_t len = ikcp_send(
                session->driver->kcp,
                message_get_buffer(message),
                (int32_t)message_get_length(message) );
        if ( len < 0 )
        {
            break;
        }
        writen += len;
        QUEUE_POP(sendqueue)( &session->sendqueue, &message );

        message_add_success( message );
        if ( message_is_complete(message) )
        {
            message_destroy( message );
        }

        if ( !_kcp_can_send( session->driver ) )
        {
            break;
        }
    }

    // 更新kcp状态
    ikcp_flush( session->driver->kcp );

    if ( writen > 0 && session_sendqueue_count(session) > 0 )
    {
        ssize_t againlen = driver_transmit( session );
        if ( againlen > 0 )
        {
            writen += againlen;
        }
    }

    return writen;
}

ssize_t driver_send( struct session * session, char * buf, size_t nbytes )
{
    struct driver * d = session->driver;

    if ( _kcp_can_send( d ) )
    {
        // 发送
        if ( ikcp_send( d->kcp, buf, (int32_t)nbytes ) < 0 )
        {
            return -1;
        }

        // 强制刷新发送队列
        ikcp_flush( d->kcp );
        _kcp_schedule( d, EVENTSET_PRECISION( session->evsets ) );

        return nbytes;
    }

    return 0;
}

void driver_set_mtu( struct driver * self, int32_t mtu )
{
    ikcp_setmtu( self->kcp, mtu );
}

void driver_set_minrto( struct driver * self, int32_t minrto )
{
    self->kcp->rx_minrto = minrto;
}

void driver_set_wndsize( struct driver * self, int32_t sndwnd, int32_t rcvwnd )
{
    ikcp_wndsize( self->kcp, sndwnd, rcvwnd );
}

void _kcp_refresh_state( struct driver * self )
{
    int32_t timeout = -1;
    struct session * session = _get_session( self );

    // 调度
    ikcp_update( self->kcp, _kcp_milliseconds(self) );

    // 检查发送窗口
    if ( _kcp_can_send( self )
            && session_sendqueue_count(session) > 0 )
    {
        // 调用过ikcp_send()下轮直接更新状态
        driver_transmit( session );
        timeout = EVENTSET_PRECISION( session->evsets );
    }

    // 计算超时时间
    // 因为调用过ikcp_send(), 所以不用计算超时时间了
    if ( timeout == -1 )
    {
        uint32_t current = _kcp_milliseconds(self);
        timeout = ikcp_check( self->kcp, current ) - current;
    }

    // 加入定时器
    _kcp_schedule( self, timeout );
}

uint32_t _kcp_milliseconds( struct driver * self )
{
    return (uint32_t)(milliseconds() - self->epoch);
}

void _kcp_timer( int32_t fd, int16_t ev, void * arg )
{
    struct session * session = (struct session *)arg;

    // 状态
    session->status &= ~SESSION_SCHEDULING;

    // 刷新kcp状态
    _kcp_refresh_state( session->driver );
}

struct session * _get_session( struct driver * self )
{
    if ( self->kcp == NULL )
    {
        return NULL;
    }

    return (struct session *)self->kcp->user;
}

void _kcp_schedule( struct driver * self, int32_t timeout )
{
    struct session * session = _get_session( self );

    // 还在定时器中, 移除
    if ( session->status & SESSION_SCHEDULING )
    {
        evsets_del( session->evsets, self->timer );
        session->status &= ~SESSION_SCHEDULING;
    }

    // 加入定时器
    event_set( self->timer, -1, 0 );
    event_set_callback( self->timer, _kcp_timer, session );
    evsets_add( session->evsets, self->timer, timeout );
    session->status |= SESSION_SCHEDULING;
}

int32_t _kcp_output( const char * buf, int32_t len, ikcpcb * kcp, void * user )
{
    struct session * session = (struct session *)user;

    // kcp出错的情况下，就重传呗, 没啥大不了的
    ssize_t writen = write( session->fd, buf, len );
    if ( writen < 0 )
    {
        if ( errno == EINTR
                || errno == EAGAIN || errno == EWOULDBLOCK )
        {
            writen = 0;
        }
    }

    return (int32_t)writen;
}

int32_t _kcp_can_send( struct driver * self )
{
    return (uint32_t)ikcp_waitsnd( self->kcp ) < 6 * self->kcp->snd_wnd;
}
