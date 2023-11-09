
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
#include "ephashtable.h"
#include "event-internal.h"
#include "threads-internal.h"
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
static inline ssize_t _receive( struct session * session );
static inline ssize_t _write_vec( int32_t fd, struct iovec * array, int32_t count );

// 逻辑操作
static ssize_t _process( struct session * session );
static int32_t _timeout( struct session * session );

static void _reconnect_direct( int32_t fd, int16_t ev, void * arg );
static void _reassociate_direct( int32_t fd, int16_t ev, void * arg );

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

ssize_t _write_vec( int32_t fd, struct iovec * array, int32_t count )
{
    ssize_t writen = -1;

#if defined( TCP_CORK )
    int32_t corked = 1;
    setsockopt( fd, IPPROTO_TCP, TCP_CORK, (const char *)&corked, sizeof( corked ) );
#endif

    writen = writev( fd, array, count );

#if defined( TCP_CORK )
    corked = 0;
    setsockopt( fd, IPPROTO_TCP, TCP_CORK, (const char *)&corked, sizeof( corked ) );
#endif

    return writen;
}

ssize_t channel_transmit( struct session * session )
{
    ssize_t writen = 0;
    size_t offset = session->msgoffset;

    int32_t iov_size = 0;
    struct iovec iov_array[iov_max];

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

    writen = _write_vec( session->fd, iov_array, iov_size );

    if ( writen > 0 ) {
        offset = session->msgoffset + writen;

        for ( ; session_sendqueue_count( session ) > 0; ) {
            struct message * message = NULL;

            QUEUE_TOP( sendqueue ) ( &session->sendqueue, &message );
            if ( offset < message_get_length( message ) ) {
                break;
            }

            QUEUE_POP( sendqueue ) ( &session->sendqueue, &message );
            offset -= message_get_length( message );

            message_add_success( message );
            if ( message_is_complete( message ) ) {
                message_destroy( message );
            }
        }

        session->msgoffset = offset;
    }

    if ( writen > 0 && session_sendqueue_count( session ) > 0 ) {
        ssize_t againlen = channel_transmit( session );
        if ( againlen > 0 ) {
            writen += againlen;
        }
    }

    return writen;
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

void channel_udpprocess( struct session * session, struct buffer * buffer )
{
    driver_input(
        session->driver, buffer, &( session->inbuffer ) );

    if ( _process( session ) < 0 ) {
        // 处理出错，尝试终止会话
        session_shutdown( session );
    }
}

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

ssize_t _receive( struct session * session )
{
    // 从socket中读取数据
    return buffer_read( &session->inbuffer, session->fd, 0 );
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
    if ( associater->fd < 0 ) {
        syslog( LOG_WARNING, "%s(fd:'%d', privdata:%p) failed, associater->reattach() failure .", __FUNCTION__, associater->fd, associater->privdata );
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
    int8_t isattach = session_is_reattch( session );

    rc = session->service.error( session->context, result );

    if ( isattach == 0
        || ( isattach == 1 && rc != 0 )
        || ( session->status & SESSION_EXITING ) ) {
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
    int32_t way = ( session->status & SESSION_SHUTDOWNING ? 0 : 1 );

    // 会话终止
    session->service.shutdown(
        session->context, way );
    session_manager_remove( session->manager, session );
#ifndef USE_REUSESESSION
    session_end( session, session->id, 0 );
#else
    // 回收会话
    session_end( session, session->id, 1 );
    session_manager_recycle( session->manager, session );
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
                || ( nread == -1 && errno == EINTR )
                || ( nread == -1 && errno == EAGAIN )
                || ( nread == -1 && errno == EWOULDBLOCK ) ) {
                // 会话正常

                if ( session->setting.persist_mode == 0 ) {
                    // 不常驻事件库的情况下, 注册读事件
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
                    channel_error( session, eIOError_OutMemory );
                } else if ( nread == -1 ) {
                    // read() failure
                    switch ( errno ) {
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
                } else if ( nread == 0 ) {
                    // peer shutdown
                    channel_error( session, eIOError_PeerShutdown );
                }
            }
        }
    } else {
        // 删除常驻事件库的读事件
        if ( session->setting.persist_mode != 0 ) {
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

    if ( ev & EV_WRITE ) {
        if ( session_sendqueue_count( session ) > 0 ) {
            // 发送数据
            ssize_t writen = session->setting.transmit( session );
            if ( writen < 0 && errno != EAGAIN ) {
                channel_error( session, eIOError_WriteFailure );
            } else {
                // 正常发送 或者 socket缓冲区已满
                uint32_t queuesize = session_sendqueue_count( session );

                if ( queuesize > 0 ) {
                    if ( session->setting.sendqueue_limit <= 0
                        || queuesize < (uint32_t)session->setting.sendqueue_limit ) {
                        // NOTICE: 为什么不判断会话是否正在终止呢?
                        // 为了尽量把数据发送完全, 所以只要不出错的情况下, 会一直发送
                        // 直到发送队列为空
                        session_add_event( session, EV_WRITE );
                    } else {
                        // 客户端无法正常接收数据，导致发送队列过度增长
                        // 避免服务器内存耗尽，必须将该会话关闭
                        channel_error( session, eIOError_SendQueueLimit );
                    }
                } else {
                    // 数据全部发送完成

                    // 尝试收缩发送队列
                    // session_sendqueue_shrink(
                    //        session, DEFAULT_SENDQUEUE_SIZE );

                    // 关闭会话
                    if ( session->status & SESSION_EXITING ) {
                        // 等待关闭的会话, 直接终止会话
                        // 后续的行为由SO_LINGER决定
                        channel_shutdown( session );
                    }
                }
            }
        } else {
            // 队列为空的情况

            // TODO: 其他需要处理的逻辑

            // 尝试收缩发送队列
            // session_sendqueue_shrink( session, DEFAULT_SENDQUEUE_SIZE );
        }
    } else {
        // 等待关闭的会话写事件超时的情况下
        // 不管发送队列如何, 直接终止会话

        assert( session->status & SESSION_EXITING );
        channel_shutdown( session );
    }
}

void channel_on_accept( int32_t fd, int16_t ev, void * arg )
{
    struct acceptor * acceptor = (struct acceptor *)arg;
    struct iolayer * layer = (struct iolayer *)( acceptor->parent );

    if ( likely( ( ev & EV_READ )
             && layer->status == eIOStatus_Running ) ) {
        int32_t cfd = -1;
        uint16_t port = 0;
        char * host = (char *)malloc( INET6_ADDRSTRLEN );
        assert( host != NULL && "allocate host failed" );

        cfd = tcp_accept( fd, host, &port );
        if ( cfd > 0 ) {
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
                acceptor->index, DISPATCH_POLICY( layer, fd ), &task );
        } else if ( errno == EMFILE ) {
            // Read the section named
            // "The special problem of accept()ing when you can't" in libev's doc.
            // By Marc Lehmann, author of libev
            iolayer_accept_fdlimits( acceptor ); free( host );
        }
    }
}

void channel_on_udpread( int32_t fd, int16_t ev, void * arg )
{
    struct transfer * transfer = (struct transfer *)arg;
    struct acceptor * acceptor = transfer->acceptor;
    struct iolayer * layer = (struct iolayer *)( acceptor->parent );

    if ( likely( ( ev & EV_READ )
        && layer->status == eIOStatus_Running ) ) {
        // 收包
        struct sockaddr_storage remoteaddr;
        ssize_t n = buffer_receive( &transfer->buffer, transfer->fd, &remoteaddr );
        if ( n < 0 ) {
            syslog( LOG_WARNING, "%s(fd:%d, %s::%d) failed, buffer_receive() %ld .",
                __FUNCTION__, fd, transfer->acceptor->host, transfer->acceptor->port, n );
        } else {
            uint16_t port = 0;
            char host[INET6_ADDRSTRLEN];
            // 解析对端地址
            parse_endpoint( &remoteaddr, host, &port );
            // 查找会话
            struct udpentry * entry = (struct udpentry *)ephashtable_find( acceptor->eptable, host, port );
            if ( entry != NULL ) {
                // 重新添加READ事件
                session_readd_event( entry->session, EV_READ );
                // 接收到的数据包发送到会话中
                channel_udpprocess( entry->session, &( transfer->buffer ) );
            } else {
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
    struct connectorlist * list = iothreads_get_connectlist( layer->threads, connector->index );

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
        connector->context, iocontext, result, connector->host, connector->port, id );
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
            if ( connector->fd > 0 ) {
                close( connector->fd );
                connector->fd = -1;
            }
            if ( connector->state == 0 ) {
                connector->state = 1;
                STAILQ_INSERT_TAIL( list, connector, linker );
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

void channel_on_reconnected( int32_t fd, int16_t ev, void * arg )
{
    struct session * session = (struct session *)arg;

    session->status &= ~SESSION_WRITING;

    if ( ev & EV_WRITE ) {
#if defined EVENT_OS_LINUX || defined EVENT_OS_MACOS
        if ( is_connected( fd ) != 0 ) {
            channel_error( session, eIOError_ConnectStatus );
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

void channel_on_associated( int32_t fd, int16_t ev, void * arg )
{
    int32_t ack = 0, result = 0;
    struct associater * associater = (struct associater *)arg;

    sid_t id = 0;
    struct session * session = NULL;
    struct iolayer * layer = (struct iolayer *)( associater->parent );
    void * iocontext = iothreads_get_context( layer->threads, associater->index );
    struct associaterlist * list = iothreads_get_associatelist( layer->threads, associater->index );

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
        associater->context, iocontext, result, associater->fd, associater->privdata, id );
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
                if ( associater->state == 0 ) {
                    associater->state = 1;
                    STAILQ_INSERT_TAIL( list, associater, linker );
                }
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
    struct schedule_task * task = (struct schedule_task *)arg;
    struct session * session = task->session;

    // 立刻执行
    int32_t rc = session->service.perform(
        session->context, task->type, task->task, task->interval );

    // 取消
    session_cancel_task( session, task );

    // 执行失败
    if ( rc < 0 ) {
        session_close( session );
        session_shutdown( session );
    }
}
