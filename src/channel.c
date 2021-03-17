
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <syslog.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/uio.h>

#include "config.h"
#include "utils.h"
#include "driver.h"
#include "channel.h"
#include "acceptq.h"
#include "event-internal.h"
#include "network-internal.h"

// iov_max
#if defined IOV_MAX
const int32_t iov_max = IOV_MAX;
#elif defined UIO_MAXIOV
const int32_t iov_max = UIO_MAXIOV;
#elif defined MAX_IOVEC
const int32_t iov_max = MAX_IOVEC;
#else
const int32_t iov_max = 128;
#endif

// 发送接收数据
static inline ssize_t _write_vec( int32_t fd, struct iovec * array, int32_t count );

// 逻辑操作
static ssize_t _process( struct session * session );
static int32_t _timeout( struct session * session );

static void _on_backconnected( int32_t fd, int16_t ev, void * arg );
static void _assign_direct( int32_t fd, int16_t ev, void * arg );
static void _reconnect_direct( int32_t fd, int16_t ev, void * arg );
static void _reassociate_direct( int32_t fd, int16_t ev, void * arg );

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

ssize_t _write_vec( int32_t fd, struct iovec * array, int32_t count )
{
    ssize_t writen = -1;

#if defined (TCP_CORK)
    int32_t corked = 1;
    setsockopt( fd, IPPROTO_TCP, TCP_CORK, (const char *)&corked, sizeof(corked) );
#endif

    writen = writev( fd, array, count );

#if defined (TCP_CORK)
    corked = 0;
    setsockopt( fd, IPPROTO_TCP, TCP_CORK, (const char *)&corked, sizeof(corked) );
#endif

    return writen;
}

ssize_t channel_receive( struct session * session )
{
    // 从socket中读取数据
    return buffer_read( &session->inbuffer, session->fd, 0 );
}

ssize_t channel_transmit( struct session * session )
{
    uint32_t i = 0;
    ssize_t writen = 0;
    size_t offset = session->msgoffset;

    int32_t iov_size = 0;
    struct iovec iov_array[iov_max];

    for ( i = 0; i < session_sendqueue_count(session) && iov_size < iov_max; ++i )
    {
        struct message * message = NULL;

        QUEUE_GET(sendqueue)( &session->sendqueue, i, &message );
        if ( offset >= message_get_length(message) )
        {
            offset -= message_get_length(message);
        }
        else
        {
            iov_array[iov_size].iov_len = message_get_length(message) - offset;
            iov_array[iov_size].iov_base = message_get_buffer(message) + offset;

            ++iov_size;
            offset = 0;
        }
    }

    writen = _write_vec( session->fd, iov_array, iov_size );

    if ( writen > 0 )
    {
        offset = session->msgoffset + writen;

        for ( ; session_sendqueue_count(session) > 0; )
        {
            struct message * message = NULL;

            QUEUE_TOP(sendqueue)( &session->sendqueue, &message );
            if ( offset < message_get_length(message) )
            {
                break;
            }

            QUEUE_POP(sendqueue)( &session->sendqueue, &message );
            offset -= message_get_length(message);

            message_add_success( message );
            if ( message_is_complete(message) )
            {
                message_destroy( message );
            }
        }

        session->msgoffset = offset;
    }

    if ( writen > 0 && session_sendqueue_count(session) > 0 )
    {
        ssize_t againlen = channel_transmit( session );
        if ( againlen > 0 )
        {
            writen += againlen;
        }
    }

    return writen;
}

ssize_t channel_send( struct session * session, char * buf, size_t nbytes )
{
    ssize_t writen = write( session->fd, buf, nbytes );
    if ( writen < 0 )
    {
        if ( errno == EINTR
            || errno == EAGAIN
            || errno == EWOULDBLOCK )
        {
            writen = 0;
        }
    }

    return writen;
}

void channel_udpprocess( struct session * session )
{
    // 重组
    if ( session->driver != NULL )
    {
        // udp驱动处理输入的数据
        driver_input( session->driver, &session->inbuffer );
    }

    if ( _process( session ) < 0 )
    {
        if ( session->setting.persist_mode != 0 )
        {
            session_del_event( session, EV_READ );
        }

        session_shutdown( session );
    }
    else if ( session->setting.persist_mode != 0 )
    {
        // 常驻事件库的情况下, 重新注册
        session_readd_event( session, EV_READ );
    }
}

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

ssize_t _process( struct session * session )
{
    ssize_t nprocess = 0;
    ioservice_t * service = &session->service;

    if ( buffer_length( &session->inbuffer ) > 0 )
    {
        char * buffer = buffer_data( &session->inbuffer );
        size_t nbytes = buffer_length( &session->inbuffer );

        // 回调逻辑层
        nprocess = service->process(
                session->context, buffer, nbytes );
        if ( nprocess > 0 )
        {
            buffer_erase( &session->inbuffer, nprocess );
        }
    }

    return nprocess;
}

int32_t _timeout( struct session * session )
{
    /*
     * 超时, 会尝试安全的终止会话
     * 根据逻辑层的返回值确定是否终止会话
     */
    int32_t rc = 0;
    ioservice_t * service = &session->service;

    rc = service->timeout( session->context );

    if ( rc != 0
            || ( session->status&SESSION_EXITING ) )
    {
        // 等待终止的会话
        // 逻辑层需要终止的会话
        // NOTICE: 此处会尝试终止会话
        return session_shutdown( session );
    }

    if ( session_is_reattch( session ) )
    {
        // 尝试重连的永久会话
        session_start_reconnect( session );
    }
    else
    {
        // 临时会话, 添加读事件
        session_add_event( session, EV_READ );
        // TODO: 是否需要打开keepalive
        session_start_keepalive( session );
    }

    return 0;
}

void _assign_direct( int32_t fd, int16_t ev, void * arg )
{
    struct udpentry * entry = (struct udpentry *)arg;
    struct iolayer * layer = entry->acceptor->parent;

    // 分配任务
    struct task_assign task;
    task.fd = fd;
    task.port = entry->port;
    task.host = strdup( entry->host );
    task.cb = entry->acceptor->cb;
    task.type = entry->acceptor->type;
    task.context = entry->acceptor->context;
    // 交换buffer
    task.buffer = (struct buffer *)malloc(sizeof(struct buffer));
    assert( task.buffer != NULL && "create struct buffer failed" );
    buffer_init( task.buffer );
    buffer_swap( task.buffer, &(entry->buffer) );
    iolayer_assign_session( layer, DISPATCH_POLICY(layer, fd), &task );

    // 移除接受队列
    acceptqueue_remove( entry->acceptor->acceptq, entry->host, entry->port );
}

void _reconnect_direct( int32_t fd, int16_t ev, void * arg )
{
    struct connector * connector = (struct connector *)arg;

    // 尝试重新连接
    connector->fd = tcp_connect( connector->host, connector->port, iolayer_client_option );
    if ( connector->fd < 0 )
    {
        syslog(LOG_WARNING, "%s(host:'%s', port:%d) failed, tcp_connect() failure .", __FUNCTION__, connector->host, connector->port);
    }

    // 检查连接状态
    event_set( connector->event, connector->fd, EV_WRITE );
    event_set_callback( connector->event, channel_on_connected, connector );
    evsets_add( connector->evsets, connector->event, TRY_RECONNECT_INTERVAL );
}

void _reassociate_direct( int32_t fd, int16_t ev, void * arg )
{
    struct associater * associater = (struct associater *)arg;

    // 这种情况不会出现
    assert( associater->reattach != NULL
            && "Illegal specified Reattach-Function" );

    // 尝试重新关联
    associater->fd = associater->reattach( associater->fd, associater->privdata );
    if ( associater->fd < 0 )
    {
        syslog(LOG_WARNING, "%s(fd:'%d', privdata:%p) failed, associater->reattach() failure .", __FUNCTION__, associater->fd, associater->privdata);
    }

    // 检查连接状态
    event_set( associater->event, associater->fd, EV_WRITE );
    event_set_callback( associater->event, channel_on_associated, associater );
    evsets_add( associater->evsets, associater->event, TRY_RECONNECT_INTERVAL );
}

int32_t channel_error( struct session * session, int32_t result )
{
    /* 出错
     * 出错时, libevlite会直接终止会话, 丢弃发送队列中的数据
     *
     * 根据会话的类型
     *        1. 临时会话, 直接终止会话
     *        2. 永久会话, 终止socket, 尝试重新连接
     */
    int32_t rc = 0;
    ioservice_t * service = &session->service;
    int8_t isattach = session_is_reattch( session );

    rc = service->error( session->context, result );

    if ( isattach == 0
            || ( isattach == 1 && rc != 0 )
            || ( session->status&SESSION_EXITING ) )
    {
        // 临时会话
        // 等待终止的会话
        // 逻辑层需要终止的永久会话
        // 直接终止会话, 导致发送队列中的数据丢失
        return channel_shutdown( session );
    }

    // 尝试重连的永久会话
    session_start_reconnect( session );

    return 0;
}

int32_t channel_shutdown( struct session * session )
{
    sid_t id = session->id;
    ioservice_t * service = &session->service;
    struct session_manager * manager = session->manager;
    int32_t way = (session->status&SESSION_SHUTDOWNING ? 0 : 1);

    // 会话终止
    service->shutdown( session->context, way );
    session_manager_remove( manager, session );
#ifndef USE_REUSESESSION
    session_end( session, id, 0 );
#else
    // 回收会话
    session_end( session, id, 1 );
    session_manager_recycle( manager, session );
#endif

    return 0;
}

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

void channel_on_read( int32_t fd, int16_t ev, void * arg )
{
    struct session * session = (struct session *)arg;
    struct iolayer * iolayer = (struct iolayer *)session->iolayer;

    // 不常驻事件库的情况下, 取消READING标志
    if ( session->setting.persist_mode == 0 )
    {
        session->status &= ~SESSION_READING;
    }

    if ( ev & EV_READ )
    {
        /* >0    - ok
         *  0    - peer shutdown
         * -1    - read() failure
         * -2    - expand() failure
         */
        ssize_t nprocess = 0;
        int32_t mseconds = session->setting.timeout_msecs;
        ssize_t nread = session->setting.receive( session );

        // 只有iolayer处于运行状态下的时候
        // 才会回调逻辑层处理数据
        if ( likely(iolayer->status == eIOStatus_Running) )
        {
            nprocess = _process( session );
        }

        if ( nprocess < 0 )
        {
            // 处理出错, 尝试终止会话

            // 先删除常驻事件库中的读事件
            if ( session->setting.persist_mode != 0 )
            {
                session_del_event( session, EV_READ );
            }

            session_shutdown( session );
        }
        else
        {
            if ( nread > 0
                    || (nread == -1 && errno == EINTR)
                    || (nread == -1 && errno == EAGAIN)
                    || (nread == -1 && errno == EWOULDBLOCK) )
            {
                // 会话正常

                if ( session->setting.persist_mode == 0 )
                {
                    // 不常驻事件库的情况下, 注册读事件
                    session_add_event( session, EV_READ );
                }
                else if ( session->setting.timeout_msecs != mseconds )
                {
                    // 常驻事件库的情况下, 重新注册
                    // 避免出现超时时间无法被修改的情况
                    session_readd_event( session, EV_READ );
                }
            }
            else
            {
                // 出错了

                // 删除常驻事件库中的读事件
                if ( session->setting.persist_mode != 0 )
                {
                    session_del_event( session, EV_READ );
                }

                if ( nread == -2 )
                {
                    // expand() failure
                    channel_error( session, eIOError_OutMemory );
                }
                else if ( nread == -1 )
                {
                    // read() failure
                    switch ( errno )
                    {
                        case EBADF :
                        case EISDIR :
                            channel_error( session, eIOError_SocketInvalid );
                            break;
                        case EFAULT :
                            channel_error( session, eIOError_InBufferInvalid );
                            break;
                        case EINVAL :
                            channel_error( session, eIOError_ReadInvalid );
                            break;
                        case EIO :
                            channel_error( session, eIOError_ReadIOError );
                            break;
                        default :
                            channel_error( session, eIOError_ReadFailure );
                            break;
                    }
                }
                else if ( nread == 0 )
                {
                    // peer shutdown
                    channel_error( session, eIOError_PeerShutdown );
                }
            }
        }
    }
    else
    {
        // 删除常驻事件库的读事件
        if ( session->setting.persist_mode != 0 )
        {
            session_del_event( session, EV_READ );
        }

        // 回调超时
        _timeout( session );
    }
}

void channel_on_write( int32_t fd, int16_t ev, void * arg )
{
    struct session * session = (struct session *)arg;

    session->status &= ~SESSION_WRITING;

    if ( ev & EV_WRITE )
    {
        if ( session_sendqueue_count(session) > 0 )
        {
            // 发送数据
            ssize_t writen = session->setting.transmit( session );
            if ( writen < 0 && errno != EAGAIN )
            {
                channel_error( session, eIOError_WriteFailure );
            }
            else
            {
                // 正常发送 或者 socket缓冲区已满
                uint32_t queuesize = session_sendqueue_count( session );

                if ( queuesize > 0 )
                {
                    if ( session->setting.sendqueue_limit <= 0
                            || queuesize < (uint32_t)session->setting.sendqueue_limit )
                    {
                        // NOTICE: 为什么不判断会话是否正在终止呢?
                        // 为了尽量把数据发送完全, 所以只要不出错的情况下, 会一直发送
                        // 直到发送队列为空
                        session_add_event( session, EV_WRITE );
                    }
                    else
                    {
                        // 客户端无法正常接收数据，导致发送队列过度增长
                        // 避免服务器内存耗尽，必须将该会话关闭
                        channel_error( session, eIOError_SendQueueLimit );
                    }
                }
                else
                {
                    // 数据全部发送完成

                    // 尝试收缩发送队列
                    //session_sendqueue_shrink(
                    //        session, DEFAULT_SENDQUEUE_SIZE );

                    // 关闭会话
                    if ( session->status&SESSION_EXITING )
                    {
                        // 等待关闭的会话, 直接终止会话
                        // 后续的行为由SO_LINGER决定
                        channel_shutdown( session );
                    }
                }
            }
        }
        else
        {
            // 队列为空的情况

            // TODO: 其他需要处理的逻辑

            // 尝试收缩发送队列
            //session_sendqueue_shrink( session, DEFAULT_SENDQUEUE_SIZE );
        }
    }
    else
    {
        // 等待关闭的会话写事件超时的情况下
        // 不管发送队列如何, 直接终止会话

        assert( session->status&SESSION_EXITING );
        channel_shutdown( session );
    }
}

void channel_on_accept( int32_t fd, int16_t ev, void * arg )
{
    struct acceptor * acceptor = (struct acceptor *)arg;
    struct iolayer * layer = (struct iolayer *)(acceptor->parent);

    if ( likely( (ev & EV_READ)
                && layer->status == eIOStatus_Running) )
    {
        int32_t cfd = -1;
        struct task_assign task;

        task.host = (char *)malloc( INET6_ADDRSTRLEN );
        assert( task.host != NULL && "allocate task.host failed" );
        cfd = tcp_accept( fd, task.host, &(task.port) );
        if ( cfd > 0 )
        {
#if !defined EVENT_OS_BSD
            // FreeBSD会继承listenfd的NON Block属性
            set_non_block( cfd );
#endif

            task.fd = cfd;
            task.cb = acceptor->cb;
            task.type = acceptor->type;
            task.context = acceptor->context;

            // 分发策略
            iolayer_assign_session_direct( layer,
                    acceptor->index, DISPATCH_POLICY(layer, cfd), &(task) );
        }
        else if ( errno == EMFILE )
        {
            // Read the section named
            // "The special problem of accept()ing when you can't" in libev's doc.
            // By Marc Lehmann, author of libev
            iolayer_accept_fdlimits( acceptor );
            free( task.host );
        }
    }
}

void channel_on_udpaccept( int32_t fd, int16_t ev, void * arg )
{
    struct acceptor * acceptor = (struct acceptor *)arg;
    struct iolayer * layer = (struct iolayer *)(acceptor->parent);

    if ( likely( (ev & EV_READ)
                && layer->status == eIOStatus_Running) )
    {
        char host[ 64 ];
        uint16_t port = 0;
        struct sockaddr_storage remoteaddr;

        // 收包
        ssize_t n = buffer_receive( &acceptor->buffer, acceptor->fd, &remoteaddr );
        if ( n <= 0 )
        {
            syslog(LOG_WARNING, "%s(fd:%d, %s::%d) failed, buffer_receive() %ld .",
                    __FUNCTION__, fd, acceptor->host, acceptor->port, n );
            buffer_erase( &acceptor->buffer, -1 );
            return;
        }

        // 解析对端地址
        parse_endpoint( &remoteaddr, host, &port );

#ifndef EVENT_USE_LOCALHOST
        // 内核4.4.0之前的版本, 客户端和服务器无法同一地址
        if ( strcmp( host, acceptor->host ) == 0 )
        {
            syslog(LOG_WARNING, "%s(fd:%d, %s::%d) failed, this Connection come from LOCALHOST(%s::%d) .",
                    __FUNCTION__, fd, acceptor->host, acceptor->port, host, port );
            buffer_erase( &acceptor->buffer, -1 );
            return;
        }
#endif

        struct udpentry * entry = acceptqueue_find( acceptor->acceptq, host, port );
        if ( entry == NULL )
        {
            // 反向连接
            int32_t newfd = udp_connect( &acceptor->addr, &remoteaddr, iolayer_udp_option );
            if ( newfd < 0 )
            {
                syslog(LOG_WARNING, "%s(fd:%d, %s::%d) failed, udp_connect(%s::%d) failed .",
                        __FUNCTION__, fd, acceptor->host, acceptor->port, host, port );
                buffer_erase( &acceptor->buffer, -1 );
                return;
            }

            // 新的连接
            entry = acceptqueue_append( acceptor->acceptq, host, port );
            assert( entry != NULL && "acceptqueue_append() failed" );
            entry->acceptor = acceptor;
            entry->event = event_create();
            assert( entry->event != NULL && "event_create() failed" );
            // 查看反向连接是否成功
            event_set( entry->event, newfd, EV_WRITE );
            event_set_callback( entry->event, _on_backconnected, entry );
            evsets_add( entry->acceptor->evsets, entry->event, TRY_RECONNECT_INTERVAL );
        }

        // 缓存收到的数据包
        buffer_append2( &entry->buffer, &acceptor->buffer );
        buffer_erase( &acceptor->buffer, -1 );
    }
}

void channel_on_keepalive( int32_t fd, int16_t ev, void * arg )
{
    struct session * session = (struct session *)arg;
    ioservice_t * service = &session->service;

    session->status &= ~SESSION_KEEPALIVING;

    if ( service->keepalive( session->context ) == 0 )
    {
        // 逻辑层需要继续发送保活包
        session_start_keepalive( session );
    }
}

void channel_on_reconnect( int32_t fd, int16_t ev, void * arg )
{
    struct session * session = (struct session *)arg;

    session->status &= ~SESSION_WRITING;

    // 连接远程服务器
    if ( session->type == eSessionType_Connect )
    {
        session->fd = tcp_connect(
                session->host, session->port, iolayer_client_option );
    }
    else if ( session->type == eSessionType_Associate )
    {
        // 这种情况不会出现
        assert( session->reattach != NULL );

        // 第三方会话重连
        session->fd = session->reattach( session->fd, session->privdata );
        // 绑定的情况下需要设置非阻塞状态
        if ( session->fd >= 0 )
        {
            set_non_block( session->fd );
        }
    }

    if ( session->fd < 0 )
    {
        channel_error( session, eIOError_ConnectFailure );
        return;
    }

    // 注册写事件, 以重新激活会话
    event_set( session->evwrite, session->fd, EV_WRITE );
    event_set_callback( session->evwrite, channel_on_reconnected, session );
    evsets_add( session->evsets, session->evwrite, -1 );

    session->status |= SESSION_WRITING;
}

void channel_on_connected( int32_t fd, int16_t ev, void * arg )
{
    int32_t ack = 0, result = 0;
    struct connector * connector = (struct connector *)arg;

    sid_t id = 0;
    struct session * session = NULL;
    struct iolayer * layer = (struct iolayer *)( connector->parent );
    void * iocontext = iothreads_get_context( layer->threads, connector->index );

    if ( ev & EV_WRITE )
    {
#if defined EVENT_OS_LINUX || defined EVENT_OS_MACOS
        // linux需要进一步检查连接是否成功
        if ( is_connected( fd ) != 0 )
        {
            result = eIOError_ConnectStatus;
        }
#endif
    }
    else
    {
        result = eIOError_Timeout;
    }

    if ( result == 0 )
    {
        // 连接成功的情况下, 创建会话
        session = iolayer_alloc_session( layer, connector->fd, connector->index );
        if ( session != NULL )
        {
            id = session->id;
        }
        else
        {
            result = eIOError_OutMemory;
        }
    }

    // 把连接结果回调给逻辑层
    ack = connector->cb(
            connector->context, iocontext,
            result, connector->host, connector->port, id );
    if ( ack != 0 )
    {
        // 逻辑层确认需要关闭该会话
        if ( session )
        {
            session_manager_remove( session->manager, session );
            session_end( session, id, 0 );
        }

        iolayer_free_connector( connector );
    }
    else
    {
        if ( result != 0 )
        {
            // 逻辑层需要继续连接
            // 首先必须先关闭以前的描述符
            if ( connector->fd > 0 )
            {
                close( connector->fd );
                connector->fd = -1;
            }

            // 200毫秒后尝试重连, 避免进入重连死循环
            event_set( connector->event, -1, 0 );
            event_set_callback( connector->event, _reconnect_direct, connector );
            evsets_add( connector->evsets, connector->event, TRY_RECONNECT_INTERVAL );
        }
        else
        {
            // 连接成功, 可以进行IO操作
            set_non_block( connector->fd );
            session_set_iolayer( session, layer );
            session_copy_endpoint( session, connector->host, connector->port );
            session_start( session, eSessionType_Connect, connector->fd, connector->evsets );

            connector->fd = -1;
            iolayer_free_connector( connector );
        }
    }
}

void channel_on_reconnected( int32_t fd, int16_t ev, void * arg )
{
    struct session * session = (struct session *)arg;

    session->status &= ~SESSION_WRITING;

    if ( ev & EV_WRITE )
    {
#if defined EVENT_OS_LINUX || defined EVENT_OS_MACOS
        if ( is_connected(fd) != 0 )
        {
            channel_error( session, eIOError_ConnectStatus );
            return;
        }
#endif
        // 总算是连接上了

        set_non_block( fd );
        session->service.start( session->context );

        // 注册读写事件
        // 把积累下来的数据全部发送出去
        session_add_event( session, EV_READ );
        session_add_event( session, EV_WRITE );
        session_start_keepalive( session );
    }
    else
    {
        _timeout( session );
    }
}

void channel_on_associated( int32_t fd, int16_t ev, void * arg )
{
    int32_t ack = 0, result = 0;
    struct associater * associater = (struct associater *)arg;

    sid_t id = 0;
    struct session * session = NULL;
    struct iolayer * layer = (struct iolayer *)( associater->parent );
    void * iocontext = iothreads_get_context( layer->threads, associater->index );

    if ( ev & EV_WRITE )
    {
#if defined EVENT_OS_LINUX || defined EVENT_OS_MACOS
        // linux需要进一步检查连接是否成功
        if ( is_connected( fd ) != 0 )
        {
            result = eIOError_ConnectStatus;
        }
#endif
    }
    else
    {
        result = eIOError_Timeout;
    }

    if ( result == 0 )
    {
        // 连接成功的情况下, 创建会话
        session = iolayer_alloc_session( layer, associater->fd, associater->index );
        if ( session != NULL )
        {
            id = session->id;
        }
        else
        {
            result = eIOError_OutMemory;
        }
    }

    // 把连接结果回调给逻辑层
    ack = associater->cb(
            associater->context, iocontext,
            result, associater->fd, associater->privdata, id );
    if ( ack != 0 )
    {
        // 逻辑层确认需要关闭该会话
        if ( session )
        {
            session_manager_remove( session->manager, session );
            session_end( session, id, 0 );
        }

        iolayer_free_associater( associater );
    }
    else
    {
        if ( result != 0 )
        {
            // 逻辑层需要重新绑定
            if ( unlikely( associater->reattach == NULL ) )
            {
                // 没有设置重新关联函数
                iolayer_free_associater( associater );
            }
            else
            {
                // 启动200ms的定时器尝试重新绑定(避免进入reattach死循环)
                event_set( associater->event, -1, 0 );
                event_set_callback( associater->event, _reassociate_direct, associater );
                evsets_add( associater->evsets, associater->event, TRY_RECONNECT_INTERVAL );
            }
        }
        else
        {
            // 关联成功, 该会话可以进行IO操作
            set_non_block( associater->fd );
            session_set_iolayer( session, layer );
            session_set_reattach( session, associater->reattach, associater->privdata );
            session_start( session, eSessionType_Associate, associater->fd, associater->evsets );

            iolayer_free_associater( associater );
        }
    }
}

void _on_backconnected( int32_t fd, int16_t ev, void * arg )
{
    int32_t result = 0;
    struct udpentry * entry = (struct udpentry *)arg;

    if ( ev & EV_WRITE )
    {
#if defined EVENT_OS_LINUX || defined EVENT_OS_MACOS
        // linux需要进一步检查连接是否成功
        if ( is_connected( fd ) != 0 )
        {
            result = eIOError_ConnectStatus;
        }
#endif
    }
    else
    {
        result = eIOError_Timeout;
    }

    if ( result == 0 )
    {
        // NOTICE: 防止竞态条件发生, 下一帧分配会话
        // 反向连接成功+listenfd收到的包在同一帧中出现, 所以最好的做法就是下一帧再分配会话，这就解决所有问题了
        event_set( entry->event, fd, 0 );
        event_set_callback( entry->event, _assign_direct, entry );
        evsets_add( entry->acceptor->evsets, entry->event, TIMER_MAX_PRECISION );
    }
    else
    {
        syslog(LOG_WARNING, "%s(fd:%d, %s::%d) failed, this Connection(fd:%d, %s::%d)'s status(result:%d) is INVALID .",
                __FUNCTION__, entry->acceptor->fd, entry->acceptor->host, entry->acceptor->port, fd, entry->host, entry->port, result );
        close( fd );
        acceptqueue_remove( entry->acceptor->acceptq, entry->host, entry->port );
    }
}
