
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <syslog.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/uio.h>

#include "config.h"
#include "session.h"
#include "utils.h"
#include "driver.h"
#include "channel.h"
#include "addrtable.h"
#include "threads-internal.h"
#include "network-internal.h"

enum {
#if defined IOV_MAX
    iov_max = IOV_MAX,
#elif defined UIO_MAXIOV
    iov_max = UIO_MAXIOV,
#elif defined MAX_IOVEC
    iov_max = MAX_IOVEC,
#else
    iov_max = 128,
#endif
};

// 发送接收数据
static inline ssize_t _receive( struct session * session );

// 逻辑操作
static inline ssize_t _process( struct session * session );
static inline int32_t _timeout( struct session * session );

static void _reconnected( int32_t fd, int16_t ev, void * arg );
static void _reconnect_direct( int32_t fd, int16_t ev, void * arg );
static void _reassociate_direct( int32_t fd, int16_t ev, void * arg );

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

ssize_t channel_transmit( struct session * session )
{
    ssize_t total = 0;

    while ( session_sendqueue_count( session ) > 0 ) {
        ssize_t writen = 0;
        int32_t iov_size = 0;
        struct iovec iov_array[iov_max];
        size_t offset = session->msgoffset;

        for ( uint32_t i = 0; i < session_sendqueue_count( session ) && iov_size < iov_max; ++i ) {
            struct message * message = NULL;
            QUEUE_GET( sendqueue ) ( &session->sendqueue, i, &message );
            if ( offset >= message_get_length( message ) ) {
                offset -= message_get_length( message );
            } else {
                iov_array[iov_size].iov_len = message_get_length( message ) - offset;
                iov_array[iov_size].iov_base = message_get_buffer( message ) + offset;
                ++iov_size; offset = 0;
            }
        }

        if ( iov_size > 0 ) {
            writen = writev( session->fd, iov_array, iov_size );
            if ( writen <= 0 ) {
                if ( writen < 0 && errno == EINTR ) {
                    continue;
                }
                if ( writen < 0 && ( errno == EAGAIN || errno == EWOULDBLOCK ) ) {
                    break; // 内核缓冲区满，交给 epoll EPOLLOUT 继续
                }
                return writen; // 真正的错误
            }
        }

        // An all-empty batch still needs to complete and leave the queue.
        total += writen;
        offset = session->msgoffset + writen;

        for ( ; session_sendqueue_count( session ) > 0; ) {
            struct message * message = NULL;
            QUEUE_TOP( sendqueue ) ( &session->sendqueue, &message );
            if ( offset < message_get_length( message ) ) break;
            QUEUE_POP( sendqueue ) ( &session->sendqueue, &message );
            offset -= message_get_length( message );
            message_add_success( message );
            if ( message_is_complete( message ) ) message_destroy( message );
        }
        session->msgoffset = offset;
    }

    return total;
}

ssize_t channel_send( struct session * session, char * buf, size_t nbytes )
{
    ssize_t writen = write( session->fd, buf, nbytes );
    if ( writen < 0 ) {
        if ( errno == EINTR
            || errno == EAGAIN
            || errno == EWOULDBLOCK ) {
            writen = 0;
        }
    }

    return writen;
}

void channel_udpprocess( struct session * session, struct buffer * buffer, uint8_t status )
{
    ssize_t rc = driver_input(
        session->driver, buffer, &( session->inbuffer ) );
    if ( rc == -2 ) {
        session_error( session, eIOError_OutMemory, 0 );
        return;
    }

    // kcp状态维护
    if ( status == eIOStatus_Running
        && !( session->status & SESSION_EXITING ) ) {
        if ( _process( session ) < 0 ) {
            // 处理出错，尝试终止会话
            session_shutdown( session );
        }
    } else {
        // Stopped/closing KCP sessions still consume protocol packets and ACKs, not requests.
        buffer_erase( &session->inbuffer, -1 );
    }
}

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

ssize_t _receive( struct session * session )
{
    // 从socket中读取数据
    return buffer_readv( &session->inbuffer, session->fd );
}

ssize_t _process( struct session * session )
{
    ssize_t nprocess = 0;

    if ( buffer_length( &session->inbuffer ) > 0 ) {
        char * buffer = buffer_data( &session->inbuffer );
        size_t nbytes = buffer_length( &session->inbuffer );

        // 回调逻辑层
        nprocess = session->service.process(
            session->context, buffer, nbytes );
        if ( nprocess > 0 ) {
            buffer_erase( &session->inbuffer, nprocess );
        }
    }

    return nprocess;
}

int32_t _timeout( struct session * session )
{
    // oneshot 已由事件库移除, 持久事件需要主动删除
    if ( session->setting.persist_mode == 0 ) {
        session->status &= ~SESSION_READING;
    } else {
        session_del_event( session, EV_READ );
    }

    /*
     * 超时, 会尝试安全的终止会话
     * 根据逻辑层的返回值确定是否终止会话
     */
    int32_t rc = session->service.timeout( session->context );

    if ( rc != 0
        || ( session->status & SESSION_EXITING ) ) {
        // 等待终止的会话
        // 逻辑层需要终止的会话
        // NOTICE: 此处会尝试终止会话
        return session_shutdown( session );
    }

    if ( session_is_reattch( session ) ) {
        // 尝试重连的永久会话
        session_start_reconnect( session );
    } else {
        // 临时会话, 添加读事件
        session_add_event( session, EV_READ );
        // TODO: 是否需要打开keepalive
        session_start_keepalive( session );
    }

    return 0;
}

void _reconnected( int32_t fd, int16_t ev, void * arg )
{
    struct session * session = (struct session *)arg;

    session->status &= ~SESSION_WRITING;

    if ( ev & EV_WRITE ) {
#if defined EVENT_OS_LINUX || defined EVENT_OS_MACOS
        if ( is_connected( fd ) != 0 ) {
            session_error( session, eIOError_ConnectStatus, 0 );
            return;
        }
#endif
        // 总算是连接上了

        // 把缓存的消息提取出来
        struct sendqueue queue;
        session_sendqueue_take( session, &queue );

        set_non_block( fd );
        session->service.start( session->context );

        // 把积累下来的数据全部发送出去
        session_sendqueue_merge( session, &queue );
        // 注册读写事件
        session_add_event( session, EV_READ );
        session_add_event( session, EV_WRITE );
        session_start_keepalive( session );
    } else {
        _timeout( session );
    }
}

void _reconnect_direct( int32_t fd, int16_t ev, void * arg )
{
    struct connector * connector = (struct connector *)arg;

    // 尝试重新连接
    connector->fd = tcp_connect( connector->host, connector->port, iolayer_client_option );
    if ( connector->fd < 0 ) {
        syslog( LOG_WARNING, "%s(host:'%s', port:%d) failed, tcp_connect() failure .", __FUNCTION__, connector->host, connector->port );
    }

    // 检查连接状态
    event_set( connector->event, connector->fd, EV_WRITE );
    event_set_callback( connector->event, channel_on_connected, connector );
    evsets_add( connector->evsets, connector->event, MAX_SECONDS_WAIT_FOR_CONNECT );
}

void _reassociate_direct( int32_t fd, int16_t ev, void * arg )
{
    struct associater * associater = (struct associater *)arg;

    // 这种情况不会出现
    assert( associater->reattach != NULL
        && "Illegal specified Reattach-Function" );

    // 尝试重新关联
    associater->fd = associater->reattach( associater->fd, associater->privdata );
    if ( associater->fd < 0 ) {
        syslog( LOG_WARNING, "%s(fd:'%d', privdata:%p) failed, associater->reattach() failure .", __FUNCTION__, associater->fd, associater->privdata );
    }

    // 检查连接状态
    event_set( associater->event, associater->fd, EV_WRITE );
    event_set_callback( associater->event, channel_on_associated, associater );
    evsets_add( associater->evsets, associater->event, MAX_SECONDS_WAIT_FOR_CONNECT );
}

int32_t channel_shutdown( struct session * session )
{
    sid_t id = session->id;
    struct session_manager * manager = session->manager;
    int32_t way = ( session->status & SESSION_SHUTDOWNING ? 0 : 1 );

    // 会话终止
    session->service.shutdown(
        session->context, way );
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
    if ( session->setting.persist_mode == 0 ) {
        session->status &= ~SESSION_READING;
    }

    if ( ev & EV_READ ) {
        /* >0    - ok
         *  0    - peer shutdown
         * -1    - read() failure
         * -2    - expand() failure
         */
        ssize_t nprocess = 0;
        ssize_t nread = _receive( session );
        int32_t current_errno = errno;

        // 只有iolayer处于运行状态下的时候
        // 才会回调逻辑层处理数据
        if ( likely( iolayer->status == eIOStatus_Running ) ) {
            nprocess = _process( session );
        }

        if ( nprocess < 0 ) {
            // 处理出错, 尝试终止会话

            // 先删除常驻事件库中的读事件
            if ( session->setting.persist_mode != 0 ) {
                session_del_event( session, EV_READ );
            }
            session_shutdown( session );
        } else {
            if ( nread > 0
                || ( nread == -1 && current_errno == EINTR )
                || ( nread == -1 && current_errno == EAGAIN )
                || ( nread == -1 && current_errno == EWOULDBLOCK ) ) {
                // 会话正常

                // 回调可能修改持久模式, 按实际监听状态补注册
                if ( !( session->status & SESSION_READING ) ) {
                    session_add_event( session, EV_READ );
                }
            } else {
                // 出错了

                // 删除常驻事件库中的读事件
                if ( session->setting.persist_mode != 0 ) {
                    session_del_event( session, EV_READ );
                }

                if ( nread == -2 ) {
                    // expand() failure
                    session_error( session, eIOError_OutMemory, 0 );
                } else if ( nread == -1 ) {
                    // read() failure
                    switch ( current_errno ) {
                        case EBADF :
                        case EISDIR :
                            session_error( session, eIOError_SocketInvalid, 0 );
                            break;
                        case EFAULT :
                            session_error( session, eIOError_InBufferInvalid, 0 );
                            break;
                        case EINVAL :
                            session_error( session, eIOError_ReadInvalid, 0 );
                            break;
                        case EIO :
                            session_error( session, eIOError_ReadIOError, 0 );
                            break;
                        default :
                            session_error( session, eIOError_ReadFailure, 0 );
                            break;
                    }
                } else if ( nread == 0 ) {
                    // peer shutdown
                    session_error( session, eIOError_PeerShutdown, 0 );
                }
            }
        }
    } else {
        // 回调超时
        _timeout( session );
    }
}

void channel_on_write( int32_t fd, int16_t ev, void * arg )
{
    struct session * session = (struct session *)arg;

    if ( ev & EV_WRITE ) {
        session->status &= ~SESSION_WRITING;
        session_flush_sendqueue( session );
    } else if ( ev & EV_TIMEOUT ) {
        // 关闭超时直接终止, 普通发送超时重新检查队列。
        session->status &= ~SESSION_WRITING;
        if ( session->status & SESSION_EXITING ) {
            channel_shutdown( session );
        } else {
            // 队列达到上限后补上的timer。重新尝试发送并检查当前积压,
            // session_flush_sendqueue()沿用原有错误处理契约。
            session_flush_sendqueue( session );
        }
    }
}

void channel_on_udptimeout( int32_t fd, int16_t ev, void * arg )
{
    _timeout( (struct session *)arg );
}

void channel_on_udpshutdown( int32_t fd, int16_t ev, void * arg )
{
    struct session * session = arg;
    session->status &= ~SESSION_WRITING;
    assert( session->status & SESSION_EXITING );
    channel_shutdown( session );
}

void channel_on_accept( int32_t fd, int16_t ev, void * arg )
{
    struct acceptor * acceptor = (struct acceptor *)arg;
    struct iolayer * layer = (struct iolayer *)( acceptor->parent );

    if ( likely( ev & EV_READ ) ) {
        int32_t cfd = -1;
        uint16_t port = 0;
        char * host = (char *)malloc( INET6_ADDRSTRLEN );
        assert( host != NULL && "allocate host failed" );

        cfd = tcp_accept( fd, host, &port );
        if ( cfd >= 0 ) {
            if ( layer->status != eIOStatus_Running ) {
                // Drain pending connections without admitting new sessions after stop.
                close( cfd );
                free( host );
                return;
            }
#if !defined EVENT_OS_BSD
            // FreeBSD会继承listenfd的NON Block属性
            set_non_block( cfd );
#endif
            struct task_assign task;
            task.fd = cfd;
            task.acceptor = acceptor;
            task.type = acceptor->type;
            task.transfer = NULL;
            task.host = host;
            task.port = port;
            iolayer_assign_session( layer,
                acceptor->index, DISPATCH_POLICY( layer, cfd ), &task );
        } else {
            if ( errno == EMFILE ) {
                // Read the section named
                // "The special problem of accept()ing when you can't" in libev's doc.
                // By Marc Lehmann, author of libev
                iolayer_accept_fdlimits( acceptor );
            }
            free( host );
        }
    }
}

void channel_on_datagram( int32_t fd, int16_t ev, void * arg )
{
    struct transfer * transfer = (struct transfer *)arg;
    struct acceptor * acceptor = transfer->acceptor;
    struct iolayer * layer = (struct iolayer *)( acceptor->parent );

    if ( likely( ev & EV_READ ) ) {
        // 收包
        struct sockaddr_storage remoteaddr;
        ssize_t n = buffer_receive( &transfer->buffer, transfer->fd, &remoteaddr );
        if ( n < 0 ) {
            syslog( LOG_WARNING, "%s(fd:%d, %s::%d) failed, buffer_receive() %ld .",
                __FUNCTION__, fd, transfer->acceptor->host, transfer->acceptor->port, n );
        } else {
            // A KCP datagram must contain at least its 24-byte header.
            if ( n < 24 ) {
                buffer_erase( &transfer->buffer, -1 );
                return;
            }
            // 查找会话
            struct udpentry * entry = (struct udpentry *)addrtable_find( acceptor->addrtable, &remoteaddr );
            if ( entry != NULL ) {
                // 接收到的数据包发送到会话中
                channel_udpprocess(
                    entry->session, &( transfer->buffer ), layer->status );
            } else if ( layer->status == eIOStatus_Running ) {
                if ( ikcp_getconv( buffer_data( &transfer->buffer ) ) == 0 ) {
                    // 新会话首包conv必须为0, 否则是已关闭会话的残包, 丢弃
                    // 新会话才解析对端地址
                    uint16_t port = 0;
                    char host[INET6_ADDRSTRLEN];
                    parse_endpoint( &remoteaddr, host, &port );
                    // 分配任务(KCP一定在本线程处理)
                    struct task_assign task;
                    task.fd = fd;
                    task.port = port;
                    task.host = strdup(host);
                    task.transfer = transfer;
                    task.acceptor = acceptor;
                    task.type = acceptor->type;
                    iolayer_assign_session( layer, acceptor->index, acceptor->index, &task );
                }
            }
            // 重置BUFF
            buffer_erase( &transfer->buffer, -1 );
        }
    }
}

void channel_on_keepalive( int32_t fd, int16_t ev, void * arg )
{
    struct session * session = (struct session *)arg;
    ioservice_t * service = &session->service;

    session->status &= ~SESSION_KEEPALIVING;

    if ( service->keepalive( session->context ) == 0 ) {
        // 逻辑层需要继续发送保活包
        session_start_keepalive( session );
    }
}

void channel_on_reconnect( int32_t fd, int16_t ev, void * arg )
{
    struct session * session = (struct session *)arg;

    session->status &= ~SESSION_WRITING;

    // 连接远程服务器
    if ( session->type == eSessionType_Connect ) {
        session->fd = tcp_connect(
            session->host, session->port, iolayer_client_option );
    } else if ( session->type == eSessionType_Associate ) {
        // 这种情况不会出现
        assert( session->reattach != NULL );

        // 第三方会话重连
        session->fd = session->reattach( session->fd, session->privdata );
        // 绑定的情况下需要设置非阻塞状态
        if ( session->fd >= 0 ) {
            set_non_block( session->fd );
        }
    }

    if ( session->fd < 0 ) {
        session_error( session, eIOError_ConnectFailure, 0 );
        return;
    }

    // 注册写事件, 以重新激活会话
    event_set( session->evwrite, session->fd, EV_WRITE );
    event_set_callback( session->evwrite, _reconnected, session );
    evsets_add( session->evsets, session->evwrite, MAX_SECONDS_WAIT_FOR_CONNECT );

    session->status |= SESSION_WRITING;
}

void channel_on_connected( int32_t fd, int16_t ev, void * arg )
{
    int32_t ack = 0, result = 0;
    struct connector * connector = (struct connector *)arg;

    sid_t id = 0;
    struct session * session = NULL;
    struct iolayer * layer = (struct iolayer *)( connector->parent );
    struct iothread * thread = iothreads_get( layer->threads, connector->index );

    if ( ev & EV_WRITE ) {
#if defined EVENT_OS_LINUX || defined EVENT_OS_MACOS
        // linux需要进一步检查连接是否成功
        if ( is_connected( fd ) != 0 ) {
            result = eIOError_ConnectStatus;
        }
#endif
    } else {
        result = eIOError_Timeout;
    }

    if ( result == 0 ) {
        // 连接成功的情况下, 创建会话
        session = iolayer_alloc_session( layer, connector->fd, connector->index );
        if ( session != NULL ) {
            id = session->id;
        } else {
            result = eIOError_OutMemory;
        }
    }

    // 把连接结果回调给逻辑层
    ack = connector->cb(
        connector->context, thread->context, result, connector->host, connector->port, id );
    if ( ack != 0 ) {
        // 逻辑层确认需要关闭该会话
        if ( session ) {
            session_manager_remove( session->manager, session );
            session_end( session, id, 0 );
        }

        iolayer_free_connector( connector );
    } else {
        if ( result != 0 ) {
            // 逻辑层需要继续连接
            // 首先必须先关闭以前的描述符
            if ( connector->fd >= 0 ) {
                close( connector->fd );
                connector->fd = -1;
            }
            // 200毫秒后尝试重连, 避免进入重连死循环
            event_set( connector->event, -1, 0 );
            event_set_callback( connector->event, _reconnect_direct, connector );
            evsets_add( connector->evsets, connector->event, TRY_RECONNECT_INTERVAL );
        } else {
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

void channel_on_associated( int32_t fd, int16_t ev, void * arg )
{
    int32_t ack = 0, result = 0;
    struct associater * associater = (struct associater *)arg;

    sid_t id = 0;
    struct session * session = NULL;
    struct iolayer * layer = (struct iolayer *)( associater->parent );
    struct iothread * thread = iothreads_get( layer->threads, associater->index );

    if ( ev & EV_WRITE ) {
#if defined EVENT_OS_LINUX || defined EVENT_OS_MACOS
        // linux需要进一步检查连接是否成功
        if ( is_connected( fd ) != 0 ) {
            result = eIOError_ConnectStatus;
        }
#endif
    } else {
        result = eIOError_Timeout;
    }

    if ( result == 0 ) {
        // 连接成功的情况下, 创建会话
        session = iolayer_alloc_session( layer, associater->fd, associater->index );
        if ( session != NULL ) {
            id = session->id;
        } else {
            result = eIOError_OutMemory;
        }
    }

    // 把连接结果回调给逻辑层
    ack = associater->cb(
        associater->context, thread->context, result, associater->fd, associater->privdata, id );
    if ( ack != 0 ) {
        // 逻辑层确认需要关闭该会话
        if ( session ) {
            session_manager_remove( session->manager, session );
            session_end( session, id, 0 );
        }

        iolayer_free_associater( associater );
    } else {
        if ( result != 0 ) {
            // 逻辑层需要重新绑定
            if ( unlikely( associater->reattach == NULL ) ) {
                // 没有设置重新关联函数
                iolayer_free_associater( associater );
            } else {
                // 启动200ms的定时器尝试重新绑定(避免进入reattach死循环)
                event_set( associater->event, -1, 0 );
                event_set_callback( associater->event, _reassociate_direct, associater );
                evsets_add( associater->evsets, associater->event, TRY_RECONNECT_INTERVAL );
            }
        } else {
            // 关联成功, 该会话可以进行IO操作
            set_non_block( associater->fd );
            session_set_iolayer( session, layer );
            session_set_reattach( session, associater->reattach, associater->privdata );
            session_start( session, eSessionType_Associate, associater->fd, associater->evsets );

            iolayer_free_associater( associater );
        }
    }
}

void channel_on_schedule( int32_t fd, int16_t ev, void * arg )
{
    struct scheduler * task = (struct scheduler *)arg;
    struct iothread * thread = task->thread;

    // 立刻执行
    int32_t rc = task->perform(
        thread->context, task->task, task->interval );
    if ( rc != 0 ) {
        iothread_cancel( thread, task, 0 );
    } else {
        // 继续定时
        evsets_add( thread->sets, task->event, task->interval );
    }
}
