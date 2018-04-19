
#include <stdio.h>
#include <syslog.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/socket.h>

#include "network.h"
#include "channel.h"
#include "session.h"
#include "network-internal.h"

#include "iolayer.h"

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

static inline int32_t _new_managers( struct iolayer * self );
static inline struct session_manager * _get_manager( struct iolayer * self, uint8_t index );
static inline int32_t _send_buffer( struct iolayer * self, sid_t id, const char * buf, uint32_t nbytes, int32_t isfree );
static inline int32_t _broadcast2_loop( void * context, struct session * s );

static int32_t _listen_direct( evsets_t sets, struct acceptor * acceptor );
static int32_t _connect_direct( evsets_t sets, struct connector * connector );
static void _reconnect_direct( int32_t fd, int16_t ev, void * arg );
static int32_t _assign_direct( struct iolayer * self, uint8_t index, evsets_t sets, struct task_assign * task );
static int32_t _send_direct( struct iolayer * self, struct session_manager * manager, struct task_send * task );
static int32_t _broadcast_direct( struct iolayer * self, uint8_t index, struct session_manager * manager, struct message * msg );
static int32_t _broadcast2_direct( struct iolayer * self, uint8_t index, struct session_manager * manager, struct message * msg );
static int32_t _perform_direct( struct iolayer * self, struct session_manager * manager, struct task_perform * task );
static void _perform2_direct( struct iolayer * self, uint8_t index, struct task_perform2 * task );
static int32_t _shutdown_direct( struct session_manager * manager, sid_t id );
static int32_t _shutdowns_direct( uint8_t index, struct session_manager * manager, struct sidlist * ids );
static int32_t _associate_direct( struct iolayer * self, uint8_t index, evsets_t sets, struct associater * associater );

static void _io_methods( void * context, uint8_t index, int16_t type, void * task );

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

// 创建网络通信层
iolayer_t iolayer_create( uint8_t nthreads, uint32_t nclients, uint8_t immediately )
{
    struct iolayer * self = (struct iolayer *)malloc( sizeof(struct iolayer) );
    if ( self == NULL )
    {
        return NULL;
    }

    self->context   = NULL;
    self->transform = NULL;
    self->nthreads  = nthreads;
    self->nclients  = nclients;
    self->status    = eLayerStatus_Running;

    // 初始化会话管理器
    if ( _new_managers( self ) != 0 )
    {
        iolayer_destroy( self );
        return NULL;
    }

    // 创建网络线程组
    self->group = iothreads_start( self->nthreads, immediately, _io_methods, self );
    if ( self->group == NULL )
    {
        iolayer_destroy( self );
        return NULL;
    }

    return self;
}

// 停止网络服务
void iolayer_stop( iolayer_t self )
{
    ( (struct iolayer *)self )->status = eLayerStatus_Stopped;
}

// 销毁网络通信层
void iolayer_destroy( iolayer_t self )
{
    uint8_t i = 0;
    struct iolayer * layer = (struct iolayer *)self;

    // 设置停止状态
    layer->status = eLayerStatus_Stopped;

    // 停止网络线程组
    if ( layer->group )
    {
        iothreads_stop( layer->group );
        layer->group = NULL;
    }

    // 销毁管理器
    if ( layer->managers )
    {
        for ( i = 0; i < layer->nthreads; ++i )
        {
            struct session_manager * manager = (struct session_manager *)layer->managers[i<<3];
            if ( manager )
            {
                session_manager_destroy( manager );
            }
        }
        free( layer->managers );
        layer->managers = NULL;
    }

    free( layer );
}

// 服务器开启
//      host        - 绑定的地址
//      port        - 监听的端口号
//      cb          - 新会话创建成功后的回调,会被多个网络线程调用
//                          参数1: 上下文参数;
//                          参数2: 网络线程本地数据(线程安全)
//                          参数3: 新会话ID;
//                          参数4: 会话的IP地址;
//                          参数5: 会话的端口号
//      context     - 上下文参数
int32_t iolayer_listen( iolayer_t self,
        const char * host, uint16_t port,
        int32_t (*cb)( void *, void *, sid_t, const char * , uint16_t ), void * context )
{
    uint8_t i = 0, nthreads = 1;
    struct iolayer * layer = (struct iolayer *)self;

    // 参数检查
    assert( self != NULL && "Illegal IOLayer" );
    assert( cb != NULL && "Illegal specified Callback-Function" );

#ifdef USE_REUSEPORT
    nthreads = layer->nthreads;
#endif

    for ( i = 0; i < nthreads; ++i )
    {
        struct acceptor * acceptor = (struct acceptor *)calloc( 1, sizeof(struct acceptor) );
        if ( acceptor == NULL )
        {
            syslog(LOG_WARNING,
                    "%s(host:'%s', port:%d) failed, Out-Of-Memory .",
                    __FUNCTION__, host == NULL ? "" : host, port);
            return -1;
        }

        acceptor->event = event_create();
        if ( acceptor->event == NULL )
        {
            syslog(LOG_WARNING,
                    "%s(host:'%s', port:%d) failed, can't create AcceptEvent.",
                    __FUNCTION__, host == NULL ? "" : host, port);
            free( acceptor );
            return -2;
        }

        acceptor->fd = tcp_listen( host, port, iolayer_server_option );
        if ( acceptor->fd <= 0 )
        {
            syslog(LOG_WARNING,
                    "%s(host:'%s', port:%d) failed, tcp_listen() failure .",
                    __FUNCTION__, host == NULL ? "" : host, port);
            free( acceptor );
            return -3;
        }

        acceptor->cb        = cb;
        acceptor->context   = context;
        acceptor->parent    = layer;
        acceptor->port      = port;
        acceptor->host[0]   = 0;
#ifdef USE_REUSEPORT
        acceptor->index     = i;
#else
        acceptor->index     = DISPATCH_POLICY(layer, acceptor->fd);
#endif
        if ( host != NULL )
        {
            strncpy( acceptor->host, host, INET_ADDRSTRLEN );
        }

        iothreads_post( layer->group, acceptor->index, eIOTaskType_Listen, acceptor, 0 );
    }

    return 0;
}

// 客户端开启
//      host        - 远程服务器的地址
//      port        - 远程服务器的端口
//      seconds     - 连接超时时间
//      cb          - 连接结果的回调
//                          参数1: 上下文参数
//                          参数2: 网络线程本地数据(线程安全)
//                          参数3: 连接结果
//                          参数4: 连接的远程服务器的地址
//                          参数5: 连接的远程服务器的端口
//                          参数6: 连接成功后返回的会话ID
//      context     - 上下文参数
int32_t iolayer_connect( iolayer_t self,
        const char * host, uint16_t port, int32_t seconds,
        int32_t (*cb)( void *, void *, int32_t, const char *, uint16_t , sid_t), void * context )
{
    struct iolayer * layer = (struct iolayer *)self;

    assert( self != NULL && "Illegal IOLayer" );
    assert( host != NULL && "Illegal specified Host" );
    assert( cb != NULL && "Illegal specified Callback-Function" );

    struct connector * connector = (struct connector *)calloc( 1, sizeof(struct connector) );
    if ( connector == NULL )
    {
        syslog(LOG_WARNING, "%s(host:'%s', port:%d) failed, Out-Of-Memory .", __FUNCTION__, host, port);
        return -1;
    }

    connector->event = event_create();
    if ( connector->event == NULL )
    {
        syslog(LOG_WARNING, "%s(host:'%s', port:%d) failed, can't create ConnectEvent.", __FUNCTION__, host, port);
        free( connector );
        return -2;
    }

    connector->fd = tcp_connect( host, port, iolayer_client_option );
    if ( connector->fd <= 0 )
    {
        syslog(LOG_WARNING, "%s(host:'%s', port:%d) failed, tcp_connect() failure .", __FUNCTION__, host, port);
        free( connector );
        return -3;
    }

    connector->cb       = cb;
    connector->context  = context;
    connector->mseconds = seconds*1000;
    connector->parent   = layer;
    connector->port     = port;
    strncpy( connector->host, host, INET_ADDRSTRLEN );
    connector->index    = DISPATCH_POLICY( layer, connector->fd );

    iothreads_post( layer->group, connector->index, eIOTaskType_Connect, connector, 0 );

    return 0;
}

// 描述符关联会话ID
//      fd              - 描述符
//      cb              - 关联成功后的回调
//                            参数1: 上下文参数
//                            参数2: 网络线程上下文参数
//                            参数3: 描述符
//                            参数4: 关联的会话ID
//      context         - 上下文参数
int32_t iolayer_associate( iolayer_t self, int32_t fd,
        int32_t (*cb)(void *, void *, int32_t, sid_t), void * context )
{
    struct iolayer * layer = (struct iolayer *)self;

    assert( self != NULL && "Illegal IOLayer" );
    assert( fd > 2 && "Illegal Descriptor" );
    assert( cb != NULL && "Illegal specified Callback-Function" );

    struct associater * associater = (struct associater *)calloc( 1, sizeof(struct associater) );
    if ( associater == NULL )
    {
        syslog( LOG_WARNING, "%s(fd:%u) failed, Out-Of-Memory .", __FUNCTION__, fd );
        return -1;
    }

    associater->fd = fd;
    associater->cb = cb;
    associater->context = context;
    associater->parent = layer;

    iothreads_post( layer->group, DISPATCH_POLICY(layer, fd), eIOTaskType_Associate, associater, 0 );

    return 0;
}

int32_t iolayer_set_iocontext( iolayer_t self, void ** contexts, uint8_t count )
{
    uint8_t i = 0;
    struct iolayer * layer = (struct iolayer *)self;

    // 参数检查
    assert( self != NULL && "Illegal IOLayer" );
    assert( layer->group != NULL && "Illegal IOThreadGroup" );
    assert( layer->nthreads == count && "IOThread Number Invalid" );

    for ( i = 0; i < count; ++i )
    {
        iothreads_set_context( layer->group, i, contexts[i] );
    }

    return 0;
}

int32_t iolayer_set_transform( iolayer_t self,
        char * (*transform)(void *, const char *, uint32_t *), void * context )
{
    struct iolayer * layer = (struct iolayer *)self;

    assert( self != NULL && "Illegal IOLayer" );

    layer->context = context;
    layer->transform = transform;
    return 0;
}

int32_t iolayer_set_timeout( iolayer_t self, sid_t id, int32_t seconds )
{
    // NOT Thread-Safe
    uint8_t index = SID_INDEX(id);
    struct iolayer * layer = (struct iolayer *)self;

    // 参数检查
    assert( layer != NULL && "Illegal IOLayer" );
    assert( layer->group != NULL && "Illegal IOThreadGroup" );
    assert( "iolayer_set_timeout() must be in the specified thread"
            && pthread_equal(iothreads_get_id(layer->group, index), pthread_self()) != 0 );

    if ( index >= layer->nthreads )
    {
        syslog(LOG_WARNING, "%s(SID=%ld) failed, the Session's index[%u] is invalid .", __FUNCTION__, id, index );
        return -1;
    }

    struct session_manager * manager = _get_manager( layer, index );
    if ( manager == NULL )
    {
        syslog(LOG_WARNING, "%s(SID=%ld) failed, the Session's manager[%u] is invalid .", __FUNCTION__, id, index );
        return -2;
    }

    struct session * session = session_manager_get( manager, id );
    if ( session == NULL )
    {
        syslog(LOG_WARNING, "%s(SID=%ld) failed, the Session is invalid .", __FUNCTION__, id );
        return -3;
    }

    session->setting.timeout_msecs = seconds*1000;

    return 0;
}

int32_t iolayer_set_keepalive( iolayer_t self, sid_t id, int32_t seconds )
{
    // NOT Thread-Safe
    uint8_t index = SID_INDEX(id);
    struct iolayer * layer = (struct iolayer *)self;

    // 参数检查
    assert( layer != NULL && "Illegal IOLayer" );
    assert( layer->group != NULL && "Illegal IOThreadGroup" );
    assert( "iolayer_set_keepalive() must be in the specified thread"
            && pthread_equal(iothreads_get_id(layer->group, index), pthread_self()) != 0 );

    if ( index >= layer->nthreads )
    {
        syslog(LOG_WARNING, "%s(SID=%ld) failed, the Session's index[%u] is invalid .", __FUNCTION__, id, index );
        return -1;
    }

    struct session_manager * manager = _get_manager( layer, index );
    if ( manager == NULL )
    {
        syslog(LOG_WARNING, "%s(SID=%ld) failed, the Session's manager[%u] is invalid .", __FUNCTION__, id, index );
        return -2;
    }

    struct session * session = session_manager_get( manager, id );
    if ( session == NULL )
    {
        syslog(LOG_WARNING, "%s(SID=%ld) failed, the Session is invalid .", __FUNCTION__, id );
        return -3;
    }

    session->setting.keepalive_msecs = seconds*1000;

    return 0;
}

int32_t iolayer_set_service( iolayer_t self, sid_t id, ioservice_t * service, void * context )
{
    // NOT Thread-Safe
    uint8_t index = SID_INDEX(id);
    struct iolayer * layer = (struct iolayer *)self;

    // 参数检查
    assert( layer != NULL && "Illegal IOLayer" );
    assert( layer->group != NULL && "Illegal IOThreadGroup" );
    assert( "iolayer_set_service() must be in the specified thread"
            && pthread_equal(iothreads_get_id(layer->group, index), pthread_self()) != 0 );

    if ( index >= layer->nthreads )
    {
        syslog(LOG_WARNING, "%s(SID=%ld) failed, the Session's index[%u] is invalid .", __FUNCTION__, id, index );
        return -1;
    }

    struct session_manager * manager = _get_manager( layer, index );
    if ( manager == NULL )
    {
        syslog(LOG_WARNING, "%s(SID=%ld) failed, the Session's manager[%u] is invalid .", __FUNCTION__, id, index );
        return -2;
    }

    struct session * session = session_manager_get( manager, id );
    if ( session == NULL )
    {
        syslog(LOG_WARNING, "%s(SID=%ld) failed, the Session is invalid .", __FUNCTION__, id );
        return -3;
    }

    session->context = context;
    session->service = *service;

    return 0;
}

// 发送数据到会话
//      id              - 会话ID
//      buf             - 要发送的缓冲区
//      nbytes          - 要发送的长度
//      isfree          - 1-由网络层释放缓冲区, 0-网络层需要Copy缓冲区
int32_t iolayer_send( iolayer_t self, sid_t id, const char * buf, uint32_t nbytes, int32_t isfree )
{
    return _send_buffer( (struct iolayer *)self, id, buf, nbytes, isfree );
}

int32_t iolayer_broadcast( iolayer_t self, sid_t * ids, uint32_t count, const char * buf, uint32_t nbytes )
{
    uint8_t i = 0;

    pthread_t threadid = pthread_self();
    struct iolayer * layer = (struct iolayer *)self;

    for ( i = 0; i < layer->nthreads; ++i )
    {
        struct message * msg = message_create();
        if ( unlikely(msg == NULL) )
        {
            continue;
        }
        message_add_receivers( msg, ids, count );
        message_add_buffer( msg, (char *)buf, nbytes );

        if ( threadid == iothreads_get_id( layer->group, i ) )
        {
            // 本线程内直接广播
            _broadcast_direct( layer, i, _get_manager(layer, i), msg );
        }
        else
        {
            // 跨线程提交广播任务
            int32_t result = iothreads_post( layer->group, i, eIOTaskType_Broadcast, msg, 0 );
            if ( unlikely(result != 0) )
            {
                message_destroy( msg );
                continue;
            }
        }
    }

    return 0;
}

int32_t iolayer_broadcast2( iolayer_t self, const char * buf, uint32_t nbytes )
{
    uint8_t i = 0;

    pthread_t threadid = pthread_self();
    struct iolayer * layer = (struct iolayer *)self;

    for ( i = 0; i < layer->nthreads; ++i )
    {
        struct message * msg = message_create();
        if ( unlikely(msg == NULL) )
        {
            continue;
        }
        message_add_buffer( msg, (char *)buf, nbytes );

        if ( threadid == iothreads_get_id( layer->group, i ) )
        {
            // 本线程内直接广播
            _broadcast2_direct( layer, i, _get_manager(layer, i), msg );
        }
        else
        {
            // 跨线程提交广播任务
            int32_t result = iothreads_post( layer->group, i, eIOTaskType_Broadcast2, msg, 0 );
            if ( unlikely(result != 0) )
            {
                message_destroy( msg );
                continue;
            }
        }
    }

    return 0;
}

int32_t iolayer_perform( iolayer_t self, sid_t id, int32_t type, void * task, void (*recycle)(int32_t, void *) )
{
    uint8_t index = SID_INDEX(id);
    struct iolayer * layer = (struct iolayer *)self;

    if ( unlikely(index >= layer->nthreads) )
    {
        syslog(LOG_WARNING, "%s(SID=%ld) failed, the Session's index[%u] is invalid .", __FUNCTION__, id, index );
        return -1;
    }

    struct task_perform posttask = { id, type, task, recycle };

    if ( pthread_self() == iothreads_get_id( layer->group, index ) )
    {
        return _perform_direct( layer, _get_manager(layer, index), &posttask );
    }

    // 跨线程提交发送任务
    return iothreads_post( layer->group, index, eIOTaskType_Perform, (void *)&posttask, sizeof(posttask) );
}

int32_t iolayer_perform2( iolayer_t self, void * task, void * (*clone)( void * ), void (*perform)( void *, void * ) )
{
    uint8_t i = 0;

    pthread_t threadid = pthread_self();
    struct iolayer * layer = (struct iolayer *)self;
    struct task_perform2 * tasklist = (struct task_perform2 *)calloc( layer->nthreads, sizeof(struct task_perform2) );

    // clone task
    assert( tasklist != NULL && "create struct task_perform2 failed" );
    for ( i = 0; i < layer->nthreads; ++i )
    {
        tasklist[i].clone = clone;
        tasklist[i].perform = perform;
        tasklist[i].task = i == 0 ? task : clone( task );
    }

    for ( i = 0; i < layer->nthreads; ++i )
    {
        if ( threadid == iothreads_get_id( layer->group, i ) )
        {
            // 本线程内直接广播
            _perform2_direct( layer, i, &( tasklist[i] ) );
        }
        else
        {
            // 跨线程提交广播任务
            iothreads_post( layer->group, i, eIOTaskType_Perform2, &( tasklist[i] ), sizeof(struct task_perform2) );
        }
    }

    free( tasklist );

    return 0;
}

int32_t iolayer_shutdown( iolayer_t self, sid_t id )
{
    uint8_t index = SID_INDEX(id);
    struct iolayer * layer = (struct iolayer *)self;

    if ( index >= layer->nthreads )
    {
        syslog(LOG_WARNING, "%s(SID=%ld) failed, the Session's index[%u] is invalid .", __FUNCTION__, id, index );
        return -1;
    }

    // 避免在回调函数中直接终止会话
    // 这样会引发后续对会话的操作都非法了
#if 0
    if ( pthread_self() == iothreads_get_id( layer->group, index ) )
    {
        // 本线程内直接终止
        return _shutdown_direct( _get_manager(layer, index), &task );
    }
#endif

    // 跨线程提交终止任务
    return iothreads_post( layer->group, index, eIOTaskType_Shutdown, (void *)&id, sizeof(id) );
}

int32_t iolayer_shutdowns( iolayer_t self, sid_t * ids, uint32_t count )
{
    uint8_t i = 0;
    struct iolayer * layer = (struct iolayer *)self;

    for ( i = 0; i < layer->nthreads; ++i )
    {
        struct sidlist * list = sidlist_create( count );
        if ( list == NULL )
        {
            continue;
        }
        sidlist_adds( list, ids, count );

        // 参照iolayer_shutdown()

        // 跨线程提交批量终止任务
        int32_t result = iothreads_post( layer->group, i, eIOTaskType_Shutdowns, list, 0 );
        if ( unlikely(result != 0) )
        {
            sidlist_destroy( list );
            continue;
        }
    }

    return 0;
}


// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

void iolayer_server_option( int32_t fd )
{
    int32_t flag = 0;

    // Socket非阻塞
    set_non_block( fd );

    flag = 1;
    setsockopt( fd, SOL_SOCKET, SO_REUSEADDR, (void *)&flag, sizeof(flag) );

    flag = 1;
    setsockopt( fd, IPPROTO_TCP, TCP_NODELAY, (void *)&flag, sizeof(flag) );

#ifdef USE_REUSEPORT
    flag = 1;
    setsockopt( fd, SOL_SOCKET, SO_REUSEPORT, (void *)&flag, sizeof(flag) );
#endif

#if SAFE_SHUTDOWN == 0
    {
        struct linger ling;
        ling.l_onoff = 1;
        ling.l_linger = 0;
        setsockopt( fd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling) );
    }
#endif

    // 发送接收缓冲区
#if SEND_BUFFER_SIZE > 0
    //    int32_t sendbuf_size = SEND_BUFFER_SIZE;
    //    setsockopt( fd, SOL_SOCKET, SO_SNDBUF, (void *)&sendbuf_size, sizeof(sendbuf_size) );
#endif
#if RECV_BUFFER_SIZE > 0
    //    int32_t recvbuf_size = RECV_BUFFER_SIZE;
    //    setsockopt( fd, SOL_SOCKET, SO_RCVBUF, (void *)&recvbuf_size, sizeof(recvbuf_size) );
#endif
}

void iolayer_client_option( int32_t fd )
{
    int32_t flag = 0;

    // Socket非阻塞
    set_non_block( fd );

    flag = 1;
    setsockopt( fd, IPPROTO_TCP, TCP_NODELAY, (void *)&flag, sizeof(flag) );

    // 发送接收缓冲区
#if SEND_BUFFER_SIZE > 0
    //    int32_t sendbuf_size = SEND_BUFFER_SIZE;
    //    setsockopt( fd, SOL_SOCKET, SO_SNDBUF, (void *)&sendbuf_size, sizeof(sendbuf_size) );
#endif
#if RECV_BUFFER_SIZE > 0
    //    int32_t recvbuf_size = RECV_BUFFER_SIZE;
    //    setsockopt( fd, SOL_SOCKET, SO_RCVBUF, (void *)&recvbuf_size, sizeof(recvbuf_size) );
#endif
}

struct session * iolayer_alloc_session( struct iolayer * self, int32_t key, uint8_t index )
{
    struct session * session = NULL;
    struct session_manager * manager = _get_manager( self, index );

    if ( manager )
    {
        session = session_manager_alloc( manager );
    }
    else
    {
        syslog(LOG_WARNING, "%s(fd=%d) failed, the SessionManager's index[%d] is invalid .", __FUNCTION__, key, index );
    }

    return session;
}

int32_t iolayer_reconnect( struct iolayer * self, struct connector * connector )
{
    // 首先必须先关闭以前的描述符
    if ( connector->fd > 0 )
    {
        close( connector->fd );
        connector->fd = -1;
    }

    // 2秒后尝试重连, 避免忙等
    event_set( connector->event, -1, 0 );
    event_set_callback( connector->event, _reconnect_direct, connector );
    evsets_add( connector->evsets, connector->event, TRY_RECONNECT_INTERVAL );

    return 0;
}

int32_t iolayer_free_connector( struct iolayer * self, struct connector * connector )
{
    if ( connector->event )
    {
        evsets_del( connector->evsets, connector->event );
        event_destroy( connector->event );
        connector->event = NULL;
    }

    if ( connector->fd > 0 )
    {
        close( connector->fd );
        connector->fd = -1;
    }

    free( connector );
    return 0;
}

int32_t iolayer_assign_session( struct iolayer * self, uint8_t acceptidx, uint8_t index, struct task_assign * task )
{
#ifdef USE_REUSEPORT
    return _assign_direct( self, acceptidx,
            iothreads_get_sets( self->group, acceptidx ), task );
#else
    evsets_t sets = iothreads_get_sets( self->group, index );
    pthread_t threadid = iothreads_get_id( self->group, index );

    if ( pthread_self() == threadid )
    {
        return _assign_direct( self, index, sets, task );
    }
    // 跨线程提交发送任务
    return iothreads_post( self->group, index, eIOTaskType_Assign, task, sizeof(struct task_assign) );
#endif
}

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

int32_t _new_managers( struct iolayer * self )
{
    uint8_t i = 0;
    uint32_t sessions_per_thread = self->nclients/self->nthreads;

    // 会话管理器,
    // 采用cacheline对齐以提高访问速度
    self->managers = (void **)calloc( (self->nthreads)<<3, sizeof(void *) );
    if ( self->managers == NULL )
    {
        return -1;
    }
    for ( i = 0; i < self->nthreads; ++i )
    {
        uint32_t index = i<<3;

        self->managers[index] = session_manager_create( i, sessions_per_thread );
        if ( self->managers[index] == NULL )
        {
            return -2;
        }
    }

    return 0;
}

struct session_manager * _get_manager( struct iolayer * self, uint8_t index )
{
    if ( unlikely(index >= self->nthreads) )
    {
        return NULL;
    }

    return (struct session_manager *)( self->managers[index<<3] );
}

int32_t _send_buffer( struct iolayer * self, sid_t id, const char * buf, uint32_t nbytes, int32_t isfree )
{
    int32_t result = 0;
    uint8_t index = SID_INDEX(id);

    if ( unlikely(index >= self->nthreads) )
    {
        syslog(LOG_WARNING, "%s(SID=%ld) failed, the Session's index[%u] is invalid .", __FUNCTION__, id, index );

        if ( isfree != 0 )
        {
            free( (void *)buf );
        }

        return -1;
    }

    struct task_send task = { id, (char *)buf, nbytes, isfree };

    if ( pthread_self() == iothreads_get_id( self->group, index ) )
    {
        return _send_direct( self, _get_manager(self, index), &task ) > 0 ? 0 : -3;
    }

    // 跨线程提交发送任务

    if ( isfree == 0 )
    {
        task.buf = (char *)malloc( nbytes );
        if ( unlikely( task.buf == NULL ) )
        {
            syslog(LOG_WARNING, "%s(SID=%ld) failed, can't allocate the memory for 'task.buf' .", __FUNCTION__, id );
            return -2;
        }

        task.isfree = 1;
        memcpy( task.buf, buf, nbytes );
    }

    result = iothreads_post( self->group, index, eIOTaskType_Send, (void *)&task, sizeof(task) );
    if ( unlikely( result != 0 ) )
    {
        free( task.buf );
    }

    return result;
}

int32_t _broadcast2_loop( void * context, struct session * s )
{
    struct message * msg = ( struct message * )context;

    // 添加接收者
    message_add_receiver( msg, s->id );

    // 尝试发送消息
    session_sendmessage( s, msg );

    return 0;
}

int32_t _listen_direct( evsets_t sets, struct acceptor * acceptor )
{
    // 开始关注accept事件

    event_set( acceptor->event, acceptor->fd, EV_READ|EV_PERSIST );
    event_set_callback( acceptor->event, channel_on_accept, acceptor );
    evsets_add( sets, acceptor->event, 0 );

    return 0;
}

int32_t _connect_direct( evsets_t sets, struct connector * connector )
{
    // 开始关注连接事件
    connector->evsets = sets;

    event_set( connector->event, connector->fd, EV_WRITE );
    event_set_callback( connector->event, channel_on_connected, connector );
    evsets_add( sets, connector->event, connector->mseconds );

    return 0;
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

    _connect_direct( connector->evsets, connector );
}

int32_t _assign_direct( struct iolayer * layer, uint8_t index, evsets_t sets, struct task_assign * task )
{
    int32_t rc = 0;
    struct session_manager * manager = _get_manager( layer, index );

    // 会话管理器分配会话
    struct session * session = session_manager_alloc( manager );
    if ( unlikely( session == NULL ) )
    {
        syslog(LOG_WARNING,
                "%s(fd:%d, host:'%s', port:%d) failed .", __FUNCTION__, task->fd, task->host, task->port );
        close( task->fd );
        return -1;
    }

    // 回调逻辑层, 确定是否接收这个会话
    rc = task->cb( task->context,
            iothreads_get_context( layer->group, index ), session->id, task->host, task->port );
    if ( rc != 0 )
    {
        // 逻辑层不接受这个会话
        session_manager_remove( manager, session );
        close( task->fd );
        return 1;
    }

    session_set_iolayer( session, layer );
    session_set_endpoint( session, task->host, task->port );
    session_start( session, eSessionType_Once, task->fd, sets );

    return 0;
}

int32_t _send_direct( struct iolayer * self, struct session_manager * manager, struct task_send * task )
{
    int32_t rc = -1;
    struct session * session = session_manager_get( manager, task->id );

    if ( likely(session != NULL) )
    {
        // 数据统一改造
        char * buffer = task->buf;
        uint32_t nbytes = task->nbytes;

        if ( self->transform != NULL )
        {
            buffer = self->transform( self->context, task->buf, &nbytes );
        }

        if ( buffer != NULL )
        {
            rc = session_send( session, buffer, nbytes );
            if ( rc < 0 )
            {
                syslog( LOG_WARNING, "%s(SID=%ld) failed, the Session drop this message(LENGTH=%d) .\n",
                        __FUNCTION__, task->id, nbytes );
            }

            // 销毁改造后的数据
            if ( buffer != task->buf )
            {
                free( buffer );
            }
        }
    }
    else
    {
        syslog(LOG_WARNING, "%s(SID=%ld) failed, the Session is invalid .", __FUNCTION__, task->id );
    }

    if ( task->isfree != 0 )
    {
        // 指定底层释放
        free( task->buf );
    }

    return rc;
}

int32_t _broadcast_direct( struct iolayer * self, uint8_t index, struct session_manager * manager, struct message * msg )
{
    uint32_t i = 0;
    int32_t count = 0;

    // 数据改造
    if ( self->transform != NULL )
    {
        // 数据需要改造
        char * buffer = NULL;
        uint32_t nbytes = message_get_length( msg );
        buffer = self->transform( self->context, message_get_buffer(msg), &nbytes );
        if ( buffer == NULL )
        {
            // 数据改造失败
            message_destroy( msg );
            return -1;
        }
        if ( buffer != message_get_buffer(msg) )
        {
            // 数据改造成功
            message_set_buffer( msg, buffer, nbytes );
        }
    }

    for ( i = 0; i < sidlist_count(msg->tolist); ++i )
    {
        sid_t id = sidlist_get(msg->tolist, i);

        if ( SID_INDEX(id) != index )
        {
            message_add_failure( msg, id );
            continue;
        }

        struct session * session = session_manager_get( manager, id );
        if ( unlikely(session == NULL) )
        {
            message_add_failure( msg, id );
            continue;
        }

        if ( session_sendmessage(session, msg) >= 0 )
        {
            // 尝试单独发送
            ++count;
        }
    }

    // 消息发送完毕, 直接销毁
    if ( message_is_complete(msg) )
    {
        message_destroy( msg );
    }

    return count;
}

int32_t _broadcast2_direct( struct iolayer * self, uint8_t index, struct session_manager * manager, struct message * msg )
{
    int32_t count = 0;

    // 数据改造
    if ( self->transform != NULL )
    {
        // 数据需要改造
        char * buffer = NULL;
        uint32_t nbytes = message_get_length( msg );
        buffer = self->transform( self->context, message_get_buffer(msg), &nbytes );
        if ( buffer == NULL )
        {
            // 数据改造失败
            message_destroy( msg );
            return -1;
        }
        if ( buffer != message_get_buffer(msg) )
        {
            // 数据改造成功
            message_set_buffer( msg, buffer, nbytes );
        }
    }

    // 遍历在线会话
    count = session_manager_foreach( manager, _broadcast2_loop, msg );

    // 消息发送完毕, 直接销毁
    if ( message_is_complete(msg) )
    {
        message_destroy( msg );
    }

    return count;
}

int32_t _perform_direct( struct iolayer * self, struct session_manager * manager, struct task_perform * task  )
{
    int32_t rc = 0;
    struct session * session = session_manager_get( manager, task->id );

    if ( likely( session != NULL ) )
    {
        if ( session->service.perform(
                    session->context, task->type, task->task ) < 0 )
        {
            session_close( session );
            session_shutdown( session );
        }
    }
    else
    {
        rc = -1;
        task->recycle( task->type, task->task );
        syslog(LOG_WARNING, "%s(SID=%ld, TASK:%u) failed, the Session is invalid .", __FUNCTION__, task->id, task->type );
    }

    return rc;
}

void _perform2_direct( struct iolayer * self, uint8_t index, struct task_perform2 * task )
{
    task->perform( iothreads_get_context( self->group, index ), task->task );
}

int32_t _shutdown_direct( struct session_manager * manager, sid_t id )
{
    struct session * session = session_manager_get( manager, id );

    if ( session == NULL )
    {
        //syslog(LOG_WARNING, "%s(SID=%ld) failed, the Session is invalid .", __FUNCTION__,id );
        return -1;
    }

    // 设置会话被逻辑层终止了
    session_close( session );
    return session_shutdown( session );
}

int32_t _shutdowns_direct( uint8_t index, struct session_manager * manager, struct sidlist * ids )
{
    uint32_t i = 0;
    int32_t count = 0;

    for ( i = 0; i < sidlist_count(ids); ++i )
    {
        sid_t id = sidlist_get(ids, i);

        if ( SID_INDEX(id) != index )
        {
            continue;
        }

        struct session * session = session_manager_get( manager, id );
        if ( session == NULL )
        {
            continue;
        }

        // 直接终止
        ++count;
        session_close( session );
        session_shutdown( session );
    }

    sidlist_destroy( ids );

    return count;
}

int32_t _associate_direct( struct iolayer * self, uint8_t index, evsets_t sets, struct associater * associater )
{
    int32_t rc = 0;
    struct session_manager * manager = _get_manager( self, index );

    // 会话管理器分配会话
    struct session * session = session_manager_alloc( manager );
    if ( unlikely( session == NULL ) )
    {
        syslog( LOG_WARNING, "%s(fd:%u) failed .", __FUNCTION__, associater->fd );
    }

    rc = associater->cb( associater->context,
            iothreads_get_context( self->group, index ), associater->fd, session == NULL ? 0 : session->id );
    if ( rc != 0 )
    {
        if ( session != NULL )
        {
            session_manager_remove( manager, session );
        }

        free( associater );
        return 1;
    }

    session_set_iolayer( session, self );
    session_start( session, eSessionType_Once, associater->fd, sets );
    // 释放
    free( associater );

    return 0;
}

void _io_methods( void * context, uint8_t index, int16_t type, void * task )
{
    struct iolayer * layer = (struct iolayer *)context;

    // 获取事件集以及会话管理器
    evsets_t sets = iothreads_get_sets( layer->group, index );
    struct session_manager * manager = _get_manager( layer, index );

    switch ( type )
    {
            // 打开一个服务器
        case eIOTaskType_Listen :
            _listen_direct( sets, (struct acceptor *)task );
            break;

            // 连接远程服务器
        case eIOTaskType_Connect :
            _connect_direct( sets, (struct connector *)task );
            break;

            // 分配一个描述符
        case eIOTaskType_Assign :
            _assign_direct( layer, index, sets, (struct task_assign *)task );
            break;

            // 发送数据
        case eIOTaskType_Send :
            _send_direct( layer, manager, (struct task_send *)task );
            break;

            // 广播数据
        case eIOTaskType_Broadcast :
            _broadcast_direct( layer, index, manager, (struct message *)task );
            break;

            // 终止一个会话
        case eIOTaskType_Shutdown :
            _shutdown_direct( manager, *( (sid_t *)task ) );
            break;

            // 批量终止多个会话
        case eIOTaskType_Shutdowns :
            _shutdowns_direct( index, manager, (struct sidlist *)task );
            break;

            // 广播数据
        case eIOTaskType_Broadcast2 :
            _broadcast2_direct( layer, index, manager, (struct message *)task );
            break;

            // 关联描述符和会话ID
        case eIOTaskType_Associate :
            _associate_direct( layer, index, sets, (struct associater *)task );
            break;

            // 逻辑任务
        case eIOTaskType_Perform :
            _perform_direct( layer, manager, (struct task_perform *)task );
            break;

            // 逻辑任务
        case eIOTaskType_Perform2 :
            _perform2_direct( layer, index, (struct task_perform2 *)task );
            break;
    }
}
