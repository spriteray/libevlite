
#include <stdio.h>
#include <syslog.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <fcntl.h>
#include <sys/socket.h>

#include "config.h"
#include "driver.h"
#include "network.h"
#include "channel.h"
#include "session.h"
#include "message.h"
#include "acceptq.h"
#include "threads-internal.h"
#include "network-internal.h"

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

static inline int32_t _new_managers( struct iolayer * self );
static inline struct session_manager * _get_manager( struct iolayer * self, uint8_t index );
static inline int32_t _send_buffer( struct iolayer * self, sid_t id, const char * buf, size_t nbytes, int32_t isfree );
static inline int32_t _broadcast2_loop( void * context, struct session * s );
static inline void _free_task_assign( struct task_assign * task );
static int32_t _server_listen( struct iolayer * layer, uint8_t type, uint8_t index, const char * host, uint16_t port, acceptor_t callback, void * context );

static int32_t _listen_direct( struct acceptorlist * acceptorlist, evsets_t sets, struct acceptor * acceptor );
static int32_t _connect_direct( evsets_t sets, struct connector * connector );
static int32_t _associate_direct( evsets_t sets, struct associater * associater );
static int32_t _assign_direct( struct iolayer * self, uint8_t index, evsets_t sets, struct task_assign * task );

static ssize_t _send_direct( struct iolayer * self, struct session_manager * manager, struct task_send * task );
static int32_t _broadcast_direct( struct iolayer * self, uint8_t index, struct session_manager * manager, struct message * msg );
static int32_t _broadcast2_direct( struct iolayer * self, struct session_manager * manager, struct message * msg );
static void _invoke_direct( struct iolayer * self, uint8_t index, struct task_invoke * task );
static int32_t _perform_direct( struct iolayer * self, struct session_manager * manager, struct task_perform * task );
static int32_t _shutdown_direct( struct session_manager * manager, sid_t id );
static int32_t _shutdowns_direct( uint8_t index, struct session_manager * manager, struct sidlist * ids );

static void _concrete_processor( void * context, uint8_t index, int16_t type, void * task );

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
    self->roundrobin= 0;
    self->status    = eIOStatus_Running;
    self->threads   = NULL;
    self->managers  = NULL;

    // 初始化会话管理器
    if ( _new_managers( self ) != 0 )
    {
        iolayer_destroy( self );
        return NULL;
    }

    // 创建网络线程组
    self->threads = iothreads_start( self->nthreads, immediately );
    if ( self->threads == NULL )
    {
        iolayer_destroy( self );
        return NULL;
    }
    iothreads_set_processor( self->threads, _concrete_processor, self );

    return self;
}

// 停止网络服务
void iolayer_stop( iolayer_t self )
{
    ( (struct iolayer *)self )->status = eIOStatus_Stopped;
}

// 销毁网络通信层
void iolayer_destroy( iolayer_t self )
{
    uint8_t i = 0;
    struct iolayer * layer = (struct iolayer *)self;

    // 设置停止状态
    layer->status = eIOStatus_Stopped;

    // 停止网络线程组
    if ( layer->threads )
    {
        iothreads_stop( layer->threads );
        layer->threads = NULL;
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
//      type        - 网络类型: NETWORK_TCP or NETWORK_KCP
//      host        - 绑定的地址
//      port        - 监听的端口号
//      callback    - 新会话创建成功后的回调,会被多个网络线程调用
//      context     - 上下文参数
int32_t iolayer_listen( iolayer_t self, uint8_t type, const char * host, uint16_t port, acceptor_t callback, void * context )
{
    struct iolayer * layer = (struct iolayer *)self;

    // 参数检查
    assert( self != NULL && "Illegal IOLayer" );
    assert( callback != NULL && "Illegal specified Callback-Function" );
    assert( (type == NETWORK_TCP || type == NETWORK_KCP) && "Illegal type" );

    if ( type == NETWORK_KCP )
    {
        // UDP服务端
        return _server_listen( layer, type, 0, host, port, callback, context );
    }
    else if ( type == NETWORK_TCP )
    {
        // TCP服务端
#ifndef EVENT_HAVE_REUSEPORT
        // normal
        return _server_listen( layer, type, 0, host, port, callback, context );
#else
        // reuseport
        uint8_t i = 0;
        syslog(LOG_INFO,
                "%s(host:'%s', port:%d) use SO_REUSEPORT .", __FUNCTION__, host == NULL ? "" : host, port);
        for ( i = 0; i < layer->nthreads; ++i )
        {
            int32_t rc = _server_listen( layer, type, i, host, port, callback, context );
            if ( rc < 0 )
            {
                return rc;
            }
        }
        return 0;
#endif
    }

    return -1;
}

// 客户端开启
//      host        - 远程服务器的地址
//      port        - 远程服务器的端口
//      callback    - 连接结果的回调
//      context     - 上下文参数
int32_t iolayer_connect( iolayer_t self, const char * host, uint16_t port, connector_t callback, void * context )
{
    struct iolayer * layer = (struct iolayer *)self;

    assert( self != NULL && "Illegal IOLayer" );
    assert( host != NULL && "Illegal specified Host" );
    assert( callback != NULL && "Illegal specified Callback-Function" );

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

    connector->parent   = layer;
    connector->port     = port;
    connector->context  = context;
    connector->cb       = callback;
    connector->host     = strdup( host );

    // 就地投递给本网络线程
    int8_t index = iothreads_get_index( layer->threads );
    if ( index >= 0 )
    {
        connector->index = index;
        _connect_direct( iothreads_get_sets(layer->threads, index), connector );
    }
    else
    {
        connector->index = DISPATCH_POLICY(
                layer, __sync_fetch_and_add(&layer->roundrobin, 1) );
        iothreads_post( layer->threads, connector->index, eIOTaskType_Connect, connector, 0 );
    }

    return 0;
}

// 描述符关联会话ID
//      fd              - 描述符
//      privdata        - 描述符相关的私有数据
//      reattach        - 重新关联函数，返回新的描述符
//      callback        - 关联成功后的回调
//      context         - 上下文参数
int32_t iolayer_associate( iolayer_t self, int32_t fd, void * privdata, reattacher_t reattach, associator_t callback, void * context )
{
    struct iolayer * layer = (struct iolayer *)self;

    assert( self != NULL && "Illegal IOLayer" );
    //assert( fd > 2 && "Illegal Descriptor" );
    assert( callback != NULL && "Illegal specified Callback-Function" );
    //assert( reattach != NULL && "Illegal specified Reattach-Function" );

    struct associater * associater = (struct associater *)calloc( 1, sizeof(struct associater) );
    if ( associater == NULL )
    {
        syslog( LOG_WARNING, "%s(fd:%u) failed, Out-Of-Memory .", __FUNCTION__, fd );
        return -1;
    }

    associater->event = event_create();
    if ( associater->event == NULL )
    {
        syslog(LOG_WARNING, "%s(fd:'%d', privdata:%p) failed, can't create AssoicateEvent.", __FUNCTION__, fd, privdata);
        free( associater );
        return -2;
    }

    associater->fd          = fd;
    associater->cb          = callback;
    associater->reattach    = reattach;
    associater->context     = context;
    associater->parent      = layer;
    associater->privdata    = privdata;

    // 就地投递给本网络线程
    int8_t index = iothreads_get_index( layer->threads );
    if ( index >= 0 )
    {
        associater->index = index;
        _associate_direct( iothreads_get_sets(layer->threads, index), associater );
    }
    else
    {
        // 随机找一个io线程
        associater->index = DISPATCH_POLICY(
                layer, __sync_fetch_and_add(&layer->roundrobin, 1) );
        // 提交到网络层
        iothreads_post( layer->threads, associater->index, eIOTaskType_Associate, associater, 0 );
    }

    return 0;
}

int32_t iolayer_set_iocontext( iolayer_t self, void ** contexts, uint8_t count )
{
    uint8_t i = 0;
    struct iolayer * layer = (struct iolayer *)self;

    // 参数检查
    assert( self != NULL && "Illegal IOLayer" );
    assert( layer->threads != NULL && "Illegal IOThreadGroup" );
    assert( layer->nthreads == count && "IOThread Number Invalid" );

    for ( i = 0; i < count; ++i )
    {
        iothreads_set_context( layer->threads, i, contexts[i] );
    }

    return 0;
}

int32_t iolayer_set_transform( iolayer_t self, transformer_t transform, void * context )
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
    assert( seconds >= 0 && "Invalid Timeout" );
    assert( layer != NULL && "Illegal IOLayer" );
    assert( layer->threads != NULL && "Illegal IOThreadGroup" );
    assert( "iolayer_set_timeout() must be in the specified thread"
            && pthread_equal(iothreads_get_id(layer->threads, index), pthread_self()) != 0 );

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

    // 设置超时时间后，重新添加超时事件
    session->setting.timeout_msecs = seconds*1000;
    //
    session_readd_event( session, EV_READ );

    return 0;
}

int32_t iolayer_set_persist( iolayer_t self, sid_t id, int32_t onoff )
{
    // NOT Thread-Safe
    uint8_t index = SID_INDEX(id);
    struct iolayer * layer = (struct iolayer *)self;

    // 参数检查
    assert( layer != NULL && "Illegal IOLayer" );
    assert( layer->threads != NULL && "Illegal IOThreadGroup" );
    assert( "iolayer_set_persist() must be in the specified thread"
            && pthread_equal(iothreads_get_id(layer->threads, index), pthread_self()) != 0 );

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

    session->setting.persist_mode = onoff == 0 ? 0 : EV_PERSIST;

    return 0;
}

int32_t iolayer_set_sndqlimit( iolayer_t self, sid_t id, int32_t queuelimit )
{
    // NOT Thread-Safe
    uint8_t index = SID_INDEX(id);
    struct iolayer * layer = (struct iolayer *)self;

    // 参数检查
    assert( layer != NULL && "Illegal IOLayer" );
    assert( layer->threads != NULL && "Illegal IOThreadGroup" );
    assert( "iolayer_set_sndqlimit() must be in the specified thread"
            && pthread_equal(iothreads_get_id(layer->threads, index), pthread_self()) != 0 );

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

    session->setting.sendqueue_limit = queuelimit;

    return 0;
}

int32_t iolayer_set_mtu( iolayer_t self, sid_t id, int32_t mtu )
{
    // NOT Thread-Safe
    uint8_t index = SID_INDEX(id);
    struct iolayer * layer = (struct iolayer *)self;

    // 参数检查
    assert( layer != NULL && "Illegal IOLayer" );
    assert( layer->threads != NULL && "Illegal IOThreadGroup" );
    assert( "iolayer_set_mtu() must be in the specified thread"
            && pthread_equal(iothreads_get_id(layer->threads, index), pthread_self()) != 0 );

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

    if ( session->driver == NULL )
    {
        syslog(LOG_WARNING, "%s(SID=%ld) failed, the Session does not support setting the MTU .", __FUNCTION__, id );
        return -3;
    }

    driver_set_mtu( session->driver, mtu );

    return 0;
}

int32_t iolayer_set_minrto( iolayer_t self, sid_t id, int32_t minrto )
{
    // NOT Thread-Safe
    uint8_t index = SID_INDEX(id);
    struct iolayer * layer = (struct iolayer *)self;

    // 参数检查
    assert( layer != NULL && "Illegal IOLayer" );
    assert( layer->threads != NULL && "Illegal IOThreadGroup" );
    assert( "iolayer_set_minrto() must be in the specified thread"
            && pthread_equal(iothreads_get_id(layer->threads, index), pthread_self()) != 0 );

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

    if ( session->driver == NULL )
    {
        syslog(LOG_WARNING, "%s(SID=%ld) failed, the Session does not support setting the MinRTO.", __FUNCTION__, id );
        return -3;
    }

    driver_set_minrto( session->driver, minrto );

    return 0;
}

int32_t iolayer_set_wndsize( iolayer_t self, sid_t id, int32_t sndwnd, int32_t rcvwnd )
{
    // NOT Thread-Safe
    uint8_t index = SID_INDEX(id);
    struct iolayer * layer = (struct iolayer *)self;

    // 参数检查
    assert( layer != NULL && "Illegal IOLayer" );
    assert( layer->threads != NULL && "Illegal IOThreadGroup" );
    assert( "iolayer_set_sndqlimit() must be in the specified thread"
            && pthread_equal(iothreads_get_id(layer->threads, index), pthread_self()) != 0 );

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

    if ( session->driver == NULL )
    {
        syslog(LOG_WARNING, "%s(SID=%ld) failed, the Session does not support setting the WindowSize .", __FUNCTION__, id );
        return -3;
    }

    driver_set_wndsize( session->driver, sndwnd, rcvwnd );

    return 0;
}

int32_t iolayer_set_keepalive( iolayer_t self, sid_t id, int32_t seconds )
{
    // NOT Thread-Safe
    uint8_t index = SID_INDEX(id);
    struct iolayer * layer = (struct iolayer *)self;

    // 参数检查
    assert( seconds >= 0 && "Invalid Timeout" );
    assert( layer != NULL && "Illegal IOLayer" );
    assert( layer->threads != NULL && "Illegal IOThreadGroup" );
    assert( "iolayer_set_keepalive() must be in the specified thread"
            && pthread_equal(iothreads_get_id(layer->threads, index), pthread_self()) != 0 );

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

    // 设置保活时间后，重新添加事件
    session->setting.keepalive_msecs = seconds*1000;
    session_start_keepalive( session );

    return 0;
}

int32_t iolayer_set_service( iolayer_t self, sid_t id, ioservice_t * service, void * context )
{
    // NOT Thread-Safe
    uint8_t index = SID_INDEX(id);
    struct iolayer * layer = (struct iolayer *)self;

    // 参数检查
    assert( layer != NULL && "Illegal IOLayer" );
    assert( layer->threads != NULL && "Illegal IOThreadGroup" );
    assert( "iolayer_set_service() must be in the specified thread"
            && pthread_equal(iothreads_get_id(layer->threads, index), pthread_self()) != 0 );

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
int32_t iolayer_send( iolayer_t self, sid_t id, const char * buf, size_t nbytes, int32_t isfree )
{
    return _send_buffer( (struct iolayer *)self, id, buf, nbytes, isfree );
}

int32_t iolayer_broadcast( iolayer_t self, sid_t * ids, uint32_t count, const char * buf, size_t nbytes )
{
    if ( unlikely( ids == NULL || count == 0 ) )
    {
        return 0;
    }

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
        message_add_buffer( msg, buf, nbytes );
        message_add_receivers( msg, ids, count );

        if ( threadid == iothreads_get_id( layer->threads, i ) )
        {
            // 本线程内直接广播
            _broadcast_direct( layer, i, _get_manager(layer, i), msg );
        }
        else
        {
            // 跨线程提交广播任务
            int32_t result = iothreads_post( layer->threads, i, eIOTaskType_Broadcast, msg, 0 );
            if ( unlikely(result != 0) )
            {
                message_destroy( msg );
                continue;
            }
        }
    }

    return 0;
}

int32_t iolayer_broadcast2( iolayer_t self, const char * buf, size_t nbytes )
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
        message_add_buffer( msg, buf, nbytes );

        if ( threadid == iothreads_get_id( layer->threads, i ) )
        {
            // 本线程内直接广播
            _broadcast2_direct( layer, _get_manager(layer, i), msg );
        }
        else
        {
            // 跨线程提交广播任务
            int32_t result = iothreads_post( layer->threads, i, eIOTaskType_Broadcast2, msg, 0 );
            if ( unlikely(result != 0) )
            {
                message_destroy( msg );
                continue;
            }
        }
    }

    return 0;
}

int32_t iolayer_invoke( iolayer_t self, void * task, taskcloner_t clone, taskexecutor_t execute )
{
    assert( self != NULL && "Illegal IOLayer" );
    assert( execute != NULL && "Illegal specified Execute-Function" );

    uint8_t i = 0;
    pthread_t threadid = pthread_self();
    struct task_invoke tasklist[ 256 ];   // 栈中分配更快
    struct iolayer * layer = (struct iolayer *)self;

    if ( clone == NULL )
    {
        uint8_t index = milliseconds() % layer->nthreads;
        struct task_invoke inner_task = { task, execute };

        if ( threadid == iothreads_get_id( layer->threads, index ) )
        {
            // 本线程内直接执行
            _invoke_direct( layer, index, &inner_task );
        }
        else
        {
            // 跨线程提交执行任务
            iothreads_post( layer->threads, index, eIOTaskType_Invoke, &inner_task, sizeof(struct task_invoke) );
        }
    }
    else
    {
        for ( i = 0; i < layer->nthreads; ++i )
        {
            tasklist[i].perform = execute;
            tasklist[i].task = i == 0 ? task : clone( task );
        }

        for ( i = 0; i < layer->nthreads; ++i )
        {
            if ( threadid == iothreads_get_id( layer->threads, i ) )
            {
                // 本线程内直接广播
                _invoke_direct( layer, i, &(tasklist[i]) );
            }
            else
            {
                // 跨线程提交广播任务
                iothreads_post( layer->threads, i, eIOTaskType_Invoke, &(tasklist[i]), sizeof(struct task_invoke) );
            }
        }
    }

    return 0;
}

int32_t iolayer_perform( iolayer_t self, sid_t id, int32_t type, void * task, taskrecycler_t recycle )
{
    uint8_t index = SID_INDEX(id);
    struct iolayer * layer = (struct iolayer *)self;

    if ( unlikely(index >= layer->nthreads) )
    {
        syslog(LOG_WARNING, "%s(SID=%ld) failed, the Session's index[%u] is invalid .", __FUNCTION__, id, index );
        return -1;
    }

    struct task_perform ptask = { id, type, task, recycle };

    if ( pthread_self() == iothreads_get_id( layer->threads, index ) )
    {
        return _perform_direct( layer, _get_manager(layer, index), &ptask );
    }

    // 跨线程提交发送任务
    return iothreads_post( layer->threads, index, eIOTaskType_Perform, (void *)&ptask, sizeof(ptask) );
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
    if ( pthread_self() == iothreads_get_id( layer->threads, index ) )
    {
        // 本线程内直接终止
        return _shutdown_direct( _get_manager(layer, index), &task );
    }
#endif

    // 跨线程提交终止任务
    return iothreads_post( layer->threads, index, eIOTaskType_Shutdown, (void *)&id, sizeof(id) );
}

int32_t iolayer_shutdowns( iolayer_t self, sid_t * ids, uint32_t count )
{
    if ( unlikely( ids == NULL || count == 0 ) )
    {
        return 0;
    }

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
        int32_t result = iothreads_post( layer->threads, i, eIOTaskType_Shutdowns, list, 0 );
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

int32_t iolayer_server_option( int32_t fd )
{
    int32_t flag = 0;

    // 是否是IPV6-Only
    // is_ipv6only( fd );

    // Socket非阻塞
    set_non_block( fd );

    flag = 1;
    setsockopt( fd, SOL_SOCKET, SO_REUSEADDR, (void *)&flag, sizeof(flag) );

    flag = 1;
    setsockopt( fd, IPPROTO_TCP, TCP_NODELAY, (void *)&flag, sizeof(flag) );

#ifdef EVENT_HAVE_REUSEPORT
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
    // NOTICE: 内核可以动态调整发送和接受缓冲区的, 所以不建议设置该选项
#if SEND_BUFFER_SIZE > 0
    size_t sendbuf_size = SEND_BUFFER_SIZE;
    setsockopt( fd, SOL_SOCKET, SO_SNDBUF, (void *)&sendbuf_size, sizeof(sendbuf_size) );
#endif
#if RECV_BUFFER_SIZE > 0
    size_t recvbuf_size = RECV_BUFFER_SIZE;
    setsockopt( fd, SOL_SOCKET, SO_RCVBUF, (void *)&recvbuf_size, sizeof(recvbuf_size) );
#endif

    return 0;
}

int32_t iolayer_client_option( int32_t fd )
{
    int32_t flag = 0;

    // Socket非阻塞
    set_non_block( fd );

    flag = 1;
    setsockopt( fd, IPPROTO_TCP, TCP_NODELAY, (void *)&flag, sizeof(flag) );

    // 发送接收缓冲区
    // NOTICE: 内核可以动态调整发送和接受缓冲区的, 所以不建议设置该选项
#if SEND_BUFFER_SIZE > 0
    size_t sendbuf_size = SEND_BUFFER_SIZE;
    setsockopt( fd, SOL_SOCKET, SO_SNDBUF, (void *)&sendbuf_size, sizeof(sendbuf_size) );
#endif
#if RECV_BUFFER_SIZE > 0
    size_t recvbuf_size = RECV_BUFFER_SIZE;
    setsockopt( fd, SOL_SOCKET, SO_RCVBUF, (void *)&recvbuf_size, sizeof(recvbuf_size) );
#endif

    return 0;
}

int32_t iolayer_udp_option( int32_t fd )
{
    int32_t flag = 0;

    // 是否是IPV6-Only
    // is_ipv6only( fd );

    //
    set_cloexec( fd );

    // Socket非阻塞
    set_non_block( fd );

    flag = 1;
    setsockopt( fd, SOL_SOCKET, SO_REUSEADDR, (void *)&flag, sizeof(flag) );
    flag = 1;
    setsockopt( fd, SOL_SOCKET, SO_REUSEPORT, (void *)&flag, sizeof(flag) );

#if SAFE_SHUTDOWN == 0
    {
        struct linger ling;
        ling.l_onoff = 1;
        ling.l_linger = 0;
        setsockopt( fd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling) );
    }
#endif

    // 发送接收缓冲区
    // NOTICE: 内核可以动态调整发送和接受缓冲区的, 所以不建议设置该选项
#if SEND_BUFFER_SIZE > 0
    size_t sendbuf_size = SEND_BUFFER_SIZE;
    setsockopt( fd, SOL_SOCKET, SO_SNDBUF, (void *)&sendbuf_size, sizeof(sendbuf_size) );
#endif
#if RECV_BUFFER_SIZE > 0
    size_t recvbuf_size = RECV_BUFFER_SIZE;
    setsockopt( fd, SOL_SOCKET, SO_RCVBUF, (void *)&recvbuf_size, sizeof(recvbuf_size) );
#endif

    return 0;
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

void iolayer_free_acceptor( struct acceptor * acceptor )
{
    if ( acceptor->event )
    {
        evsets_del( acceptor->evsets, acceptor->event );
        event_destroy( acceptor->event );
        acceptor->event = NULL;
    }

    if ( acceptor->host != NULL )
    {
        free( acceptor->host );
        acceptor->host = NULL;
    }

    if ( acceptor->fd > 0 )
    {
        close( acceptor->fd );
        acceptor->fd = -1;
    }
    if ( acceptor->idlefd > 0 )
    {
        close( acceptor->idlefd );
        acceptor->idlefd = -1;
    }

    if ( acceptor->acceptq != NULL )
    {
        acceptqueue_destroy( acceptor->acceptq );
        acceptor->acceptq = NULL;
    }
    buffer_clear( &acceptor->buffer );

    free( acceptor );
}

void iolayer_free_connector( struct connector * connector )
{
    if ( connector->event )
    {
        evsets_del( connector->evsets, connector->event );
        event_destroy( connector->event );
        connector->event = NULL;
    }

    if ( connector->host != NULL )
    {
        free( connector->host );
        connector->host = NULL;
    }

    if ( connector->fd > 0 )
    {
        close( connector->fd );
        connector->fd = -1;
    }

    free( connector );
}

void iolayer_free_associater( struct associater * associater )
{
    if ( associater->event )
    {
        evsets_del( associater->evsets, associater->event );
        event_destroy( associater->event );
        associater->event = NULL;
    }

    // NOTICE: 释放关联器的时候，不会关闭描述符

    free( associater );
}

void iolayer_accept_fdlimits( struct acceptor * acceptor )
{
    close( acceptor->idlefd );

    acceptor->idlefd = accept( acceptor->fd, NULL, NULL );
    if ( acceptor->idlefd > 0 )
    {
        close( acceptor->idlefd );
    }

    acceptor->idlefd = open( "/dev/null", O_RDONLY|O_CLOEXEC );
}

int32_t iolayer_assign_session( struct iolayer * self, uint8_t index, struct task_assign * task )
{
    evsets_t sets = iothreads_get_sets( self->threads, index );
    pthread_t threadid = iothreads_get_id( self->threads, index );

    if ( pthread_self() == threadid )
    {
        return _assign_direct( self, index, sets, task );
    }

    // 跨线程提交发送任务
    return iothreads_post( self->threads, index, eIOTaskType_Assign, task, sizeof(struct task_assign) );
}

int32_t iolayer_assign_session_direct( struct iolayer * self, uint8_t acceptidx, uint8_t index, struct task_assign * task )
{
#ifndef EVENT_HAVE_REUSEPORT
    return iolayer_assign_session( self, index, task );
#else
    return _assign_direct( self, acceptidx,
            iothreads_get_sets( self->threads, acceptidx ), task );
#endif
}

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

int32_t _new_managers( struct iolayer * self )
{
    uint8_t i = 0;
    uint32_t sessions_per_thread = self->nclients/self->nthreads;

    // 采用cacheline对齐避免False Sharing
    // NOTICE: 根据MESI协议来看，只读的内存块都应该在Shared状态，不会出现伪共享的现象
    // NOTICE: 但实际测试结果却相反
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

int32_t _server_listen( struct iolayer * layer, uint8_t type, uint8_t index, const char * host, uint16_t port, acceptor_t callback, void * context )
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
                "%s(host:'%s', port:%d) failed, can't create AcceptEvent .",
                __FUNCTION__, host == NULL ? "" : host, port);
        iolayer_free_acceptor( acceptor );
        return -2;
    }

    acceptor->type      = type;
    acceptor->parent    = layer;
    acceptor->port      = port;
    acceptor->context   = context;
    acceptor->cb        = callback;
    if ( host != NULL )
    {
        acceptor->host  = strdup( host );
    }
    acceptor->index     = DISPATCH_POLICY( layer,
            __sync_fetch_and_add(&layer->roundrobin, 1) );

    if ( type == NETWORK_TCP )
    {
#ifdef EVENT_HAVE_REUSEPORT
        acceptor->index     = index;
#endif
        acceptor->idlefd    = open( "/dev/null", O_RDONLY|O_CLOEXEC );

        acceptor->fd = tcp_listen( host, port, iolayer_server_option );
        if ( acceptor->fd <= 0 )
        {
            syslog(LOG_WARNING,
                    "%s(host:'%s', port:%d) failed, tcp_listen() failure .",
                    __FUNCTION__, host == NULL ? "" : host, port);
            iolayer_free_acceptor( acceptor );
            return -3;
        }
    }
    else if ( type == NETWORK_KCP )
    {
#ifndef SO_REUSEPORT
        assert( 0 && "Not Support REUSEPORT" );
        syslog(LOG_WARNING,
                "%s(host:'%s', port:%d) failed, Not support REUSEPORT .",
                __FUNCTION__, host == NULL ? "" : host, port);
        return -3;
#endif
        // 初始化BUFF
        buffer_init( &acceptor->buffer );

        // 接收队列
        acceptor->acceptq = acceptqueue_create( 4096 );
        if ( acceptor->acceptq == NULL )
        {
            syslog(LOG_WARNING,
                    "%s(host:'%s', port:%d) failed, can't create AcceptQueue .",
                    __FUNCTION__, host == NULL ? "" : host, port);
            iolayer_free_acceptor( acceptor );
            return -4;
        }

        acceptor->fd = udp_bind( host, port, iolayer_udp_option, &(acceptor->addr) );
        if ( acceptor->fd <= 0 )
        {
            syslog(LOG_WARNING,
                    "%s(host:'%s', port:%d) failed, udp_bind() failure .",
                    __FUNCTION__, host == NULL ? "" : host, port);
            iolayer_free_acceptor( acceptor );
            return -5;
        }
    }

    // 提交
    iothreads_post( layer->threads, acceptor->index, eIOTaskType_Listen, acceptor, 0 );
    return 0;
}

int32_t _send_buffer( struct iolayer * self, sid_t id, const char * buf, size_t nbytes, int32_t isfree )
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

    if ( pthread_self() == iothreads_get_id( self->threads, index ) )
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

    result = iothreads_post( self->threads, index, eIOTaskType_Send, (void *)&task, sizeof(task) );
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

void _free_task_assign( struct task_assign * task )
{
    if ( task->fd > 0 )
    {
        close( task->fd );
        task->fd = 0;
    }

    if ( task->host != NULL )
    {
        free( task->host );
        task->host = NULL;
    }

    if ( task->buffer != NULL )
    {
        buffer_clear( task->buffer );
        free( task->buffer );
        task->buffer = NULL;
    }
}

int32_t _listen_direct( struct acceptorlist * acceptlist, evsets_t sets, struct acceptor * acceptor )
{
    // 开始关注accept事件
    acceptor->evsets = sets;
    STAILQ_INSERT_TAIL( acceptlist, acceptor, linker );

    event_set( acceptor->event, acceptor->fd, EV_READ|EV_PERSIST );
    if ( acceptor->type == NETWORK_TCP )
        event_set_callback( acceptor->event, channel_on_accept, acceptor );
    else if ( acceptor->type == NETWORK_KCP )
        event_set_callback( acceptor->event, channel_on_udpaccept, acceptor );
    evsets_add( sets, acceptor->event, -1 );

    return 0;
}

int32_t _connect_direct( evsets_t sets, struct connector * connector )
{
    // 设置事件集
    connector->evsets = sets;

    // 检查描述符连接状态
    event_set( connector->event, connector->fd, EV_WRITE );
    event_set_callback( connector->event, channel_on_connected, connector );
    evsets_add( sets, connector->event, TRY_RECONNECT_INTERVAL );

    return 0;
}

int32_t _associate_direct( evsets_t sets, struct associater * associater )
{
    // 设置事件集
    associater->evsets = sets;

    // 检查描述符连接状态
    event_set( associater->event, associater->fd, EV_WRITE );
    event_set_callback( associater->event, channel_on_associated, associater );
    evsets_add( sets, associater->event, TRY_RECONNECT_INTERVAL );

    return 0;
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
        _free_task_assign( task );
        return -1;
    }

    // 回调逻辑层, 确定是否接收这个会话
    rc = task->cb( task->context,
            iothreads_get_context( layer->threads, index ), session->id, task->host, task->port );
    if ( rc != 0 )
    {
        // 逻辑层不接受这个会话
        session_manager_remove( manager, session );
        _free_task_assign( task );
        return 1;
    }

    if ( task->type == NETWORK_TCP )
    {
        session_set_iolayer( session, layer );
        session_set_endpoint( session, task->host, task->port );
        session_start( session, eSessionType_Accept, task->fd, sets );
    }
    else if ( task->type == NETWORK_KCP )
    {
        rc = session_init_driver( session, task->buffer );
        if ( rc != 0 )
        {
            syslog(LOG_WARNING,
                    "%s(fd:%d, host:'%s', port:%d) failed, initialize driver .", __FUNCTION__, task->fd, task->host, task->port );
            session_manager_remove( manager, session );
            _free_task_assign( task );
            return -2;
        }

        //
        session_set_iolayer( session, layer );
        session_set_endpoint( session, task->host, task->port );
        session_start( session, eSessionType_Accept, task->fd, sets );

        // UDP需要回调连接过程中的数据包
        channel_udpprocess( session );
        // 清空任务buffer
        buffer_clear( task->buffer ); free( task->buffer );
    }

    return 0;
}

ssize_t _send_direct( struct iolayer * self, struct session_manager * manager, struct task_send * task )
{
    ssize_t writen = 0;
    struct session * session = session_manager_get( manager, task->id );

    if ( likely(session != NULL) )
    {
        // 数据统一改造
        char * buffer = task->buf;
        size_t nbytes = task->nbytes;

        if ( self->transform != NULL )
        {
            buffer = self->transform( self->context, task->buf, &nbytes );
        }

        if ( buffer != NULL )
        {
            writen = session_send( session, buffer, nbytes );
            if ( writen < 0 )
            {
                syslog( LOG_WARNING, "%s(SID=%ld) failed, the Session drop this message(LENGTH=%lu) .\n",
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
        syslog( LOG_WARNING, "%s(SID=%ld) failed, the Session is invalid .", __FUNCTION__, task->id );
    }

    if ( task->isfree != 0 )
    {
        // 指定底层释放
        free( task->buf );
    }

    return writen;
}

int32_t _broadcast_direct( struct iolayer * self, uint8_t index, struct session_manager * manager, struct message * msg )
{
    uint32_t i = 0;
    int32_t count = 0;

    // 数据改造
    if ( self->transform != NULL )
    {
        // 数据需要改造
        size_t nbytes = message_get_length( msg );
        char * buffer = self->transform( self->context, message_get_buffer(msg), &nbytes );

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

int32_t _broadcast2_direct( struct iolayer * self, struct session_manager * manager, struct message * msg )
{
    int32_t count = 0;

    // 数据改造
    if ( self->transform != NULL )
    {
        // 数据需要改造
        size_t nbytes = message_get_length( msg );
        char * buffer = self->transform( self->context, message_get_buffer(msg), &nbytes );

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

    // 优化接受者列表初始化
    message_reserve_receivers(
            msg, session_manager_count( manager ) );

    // 遍历在线会话
    count = session_manager_foreach( manager, _broadcast2_loop, msg );

    // 消息发送完毕, 直接销毁
    if ( message_is_complete(msg) )
    {
        message_destroy( msg );
    }

    return count;
}

void _invoke_direct( struct iolayer * self, uint8_t index, struct task_invoke * task )
{
    task->perform( iothreads_get_context( self->threads, index ), task->task );
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
        syslog(LOG_WARNING, "%s(SID=%ld, TASK:%u) failed, the Session is invalid .", __FUNCTION__, task->id, task->type );
    }

    // 回收任务
    task->recycle( task->type, task->task );

    return rc;
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

void _concrete_processor( void * context, uint8_t index, int16_t type, void * task )
{
    struct iolayer * layer = (struct iolayer *)context;

    // 获取事件集以及会话管理器
    evsets_t sets = iothreads_get_sets( layer->threads, index );
    struct session_manager * manager = _get_manager( layer, index );

    switch ( type )
    {
            // 打开一个服务器
        case eIOTaskType_Listen :
            _listen_direct(
                    iothreads_get_acceptlist(layer->threads, index), sets, (struct acceptor *)task );
            break;

            // 连接远程服务器
        case eIOTaskType_Connect :
            _connect_direct( sets, (struct connector *)task );
            break;

            // 关联描述符和会话ID
        case eIOTaskType_Associate :
            _associate_direct( sets, (struct associater *)task );
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
            _broadcast2_direct( layer, manager, (struct message *)task );
            break;

            // 逻辑任务
        case eIOTaskType_Invoke :
            _invoke_direct( layer, index, (struct task_invoke *)task );
            break;

            // 逻辑任务
        case eIOTaskType_Perform :
            _perform_direct( layer, manager, (struct task_perform *)task );
            break;
    }
}
