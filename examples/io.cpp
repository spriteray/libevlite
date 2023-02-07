
#include <cassert>
#include <algorithm>
#include <errno.h>
#include <sys/time.h>

#include "io.h"

IIOSession::IIOSession()
    : m_Sid( 0 ),
      m_Port( 0 ),
      m_Layer( nullptr ),
      m_IOContext( nullptr )
{}

IIOSession::~IIOSession()
{}

void IIOSession::setTimeout( int32_t seconds )
{
    assert( m_Sid != 0 && m_Layer != nullptr );
    iolayer_set_timeout( m_Layer, m_Sid, seconds );
}

void IIOSession::setKeepalive( int32_t seconds )
{
    assert( m_Sid != 0 && m_Layer != nullptr );
    iolayer_set_keepalive( m_Layer, m_Sid, seconds );
}

void IIOSession::enablePersist()
{
    assert( m_Sid != 0 && m_Layer != nullptr );
    iolayer_set_persist( m_Layer, m_Sid, 1 );
}

void IIOSession::disablePersist()
{
    assert( m_Sid != 0 && m_Layer != nullptr );
    iolayer_set_persist( m_Layer, m_Sid, 0 );
}

void IIOSession::setSendqueueLimit( int32_t limit )
{
    assert( m_Sid != 0 && m_Layer != nullptr );
    iolayer_set_sndqlimit( m_Layer, m_Sid, limit );
}

void IIOSession::setMTU( int32_t mtu )
{
    assert( m_Sid != 0 && m_Layer != nullptr );
    iolayer_set_mtu( m_Layer, m_Sid, mtu );
}

void IIOSession::setMinRTO( int32_t minrto )
{
    assert( m_Sid != 0 && m_Layer != nullptr );
    iolayer_set_minrto( m_Layer, m_Sid, minrto );
}

void IIOSession::setWindowSize( int32_t sndwnd, int32_t rcvwnd )
{
    assert( m_Sid != 0 && m_Layer != nullptr );
    iolayer_set_wndsize( m_Layer, m_Sid, sndwnd, rcvwnd );
}

void IIOSession::setEndpoint( const std::string & host, uint16_t port )
{
    assert( m_Sid != 0 && m_Layer != nullptr );
    m_Host = host; m_Port = port;
}

int32_t IIOSession::send( const std::string & buffer )
{
    return send( buffer.c_str(), static_cast<ssize_t>(buffer.length()) );
}

int32_t IIOSession::send( const char * buffer, size_t nbytes, bool isfree )
{
    return iolayer_send( m_Layer, m_Sid, buffer, nbytes, static_cast<int32_t>(isfree) );
}

int32_t IIOSession::shutdown()
{
    return iolayer_shutdown( m_Layer, m_Sid );
}

void IIOSession::init( sid_t id, void * context, iolayer_t layer, const std::string & host, uint16_t port )
{
    m_Sid       = id;
    m_Host      = host;
    m_Port      = port;
    m_Layer     = layer;
    m_IOContext = context;
}

int32_t IIOSession::onStartSession( void * context )
{
    return static_cast<IIOSession *>(context)->onStart();
}

ssize_t IIOSession::onProcessSession( void * context, const char * buffer, size_t nbytes )
{
    return static_cast<IIOSession *>(context)->onProcess( buffer, nbytes );
}

char * IIOSession::onTransformSession( void * context, const char * buffer, size_t * nbytes )
{
    size_t & _nbytes = *nbytes;
    return static_cast<IIOSession *>(context)->onTransform( buffer, _nbytes );
}

int32_t IIOSession::onTimeoutSession( void * context )
{
    return static_cast<IIOSession *>(context)->onTimeout();
}

int32_t IIOSession::onKeepaliveSession( void * context )
{
    return static_cast<IIOSession *>(context)->onKeepalive();
}

int32_t IIOSession::onErrorSession( void * context, int32_t result )
{
    return static_cast<IIOSession *>(context)->onError( result );
}

void IIOSession::onShutdownSession( void * context, int32_t way )
{
    IIOSession * session = static_cast<IIOSession *>( context );
    session->onShutdown( way );
    delete session;
}

int32_t IIOSession::onPerformSession( void * context, int32_t type, void * task, int32_t interval )
{
    return static_cast<IIOSession *>(context)->onPerform( type, task, interval );
}

// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------

IIOService::IIOService( uint8_t nthreads, uint32_t nclients, int32_t precision, bool immediately, bool transform )
    : m_IOLayer( nullptr ),
      m_Transform( transform ),
      m_Immediately( immediately ),
      m_Precision( precision ),
      m_ThreadsCount( nthreads ),
      m_SessionsCount( nclients ),
      m_IOContextGroup( nullptr )
{
    pthread_cond_init( &m_Cond, nullptr );
    pthread_mutex_init( &m_Lock, nullptr );
}

IIOService::~IIOService()
{
    pthread_cond_destroy( &m_Cond );
    pthread_mutex_destroy( &m_Lock );
}

const char * IIOService::version()
{
    return evsets_get_version();
}

bool IIOService::start()
{
    m_IOLayer = iolayer_create(
            m_ThreadsCount,
            m_SessionsCount, m_Precision, m_Immediately ? 1 : 0 );
    if ( m_IOLayer == nullptr )
    {
        return false;
    }

    m_IOContextGroup = new void * [ m_ThreadsCount ];
    if ( m_IOContextGroup != nullptr )
    {
        for ( uint8_t i = 0; i < m_ThreadsCount; ++i )
        {
            m_IOContextGroup[ i ] = initIOContext();
        }
    }

    iolayer_set_transform( m_IOLayer, onTransformService, this );
    iolayer_set_iocontext( m_IOLayer, m_IOContextGroup, m_ThreadsCount );

    return true;
}

void IIOService::stop()
{
    if ( m_IOLayer != nullptr )
    {
        iolayer_destroy( m_IOLayer );
        m_IOLayer = nullptr;
    }

    for ( size_t i = 0; i < m_ListenContexts.size(); ++i )
    {
        delete m_ListenContexts[i];
    }

    for ( size_t i = 0; i < m_ConnectContexts.size(); ++i )
    {
        delete m_ConnectContexts[i];
    }

    if ( m_IOContextGroup != nullptr )
    {
        for ( uint8_t i = 0; i < m_ThreadsCount; ++i )
        {
            if ( m_IOContextGroup != nullptr )
            {
                finalIOContext( m_IOContextGroup[i] );
            }
        }

        delete [] m_IOContextGroup;
    }

    m_ListenContexts.clear();
    m_ConnectContexts.clear();
}

void IIOService::halt()
{
    if ( m_IOLayer != nullptr )
    {
        // 停止网络库
        iolayer_stop( m_IOLayer );
    }
}

bool IIOService::isConnecting( const char * host, uint16_t port )
{
    bool found = false;

    pthread_mutex_lock( &m_Lock );
    for ( size_t i = 0; i < m_ConnectContexts.size(); ++i )
    {
        if ( m_ConnectContexts[i]->port == port
                && m_ConnectContexts[i]->host == std::string(host) )
        {
            found = true;
            break;
        }
    }
    pthread_mutex_unlock( &m_Lock );

    return found;
}

bool IIOService::listen( NetType type, const char * host, uint16_t port, const options_t * options )
{
    ListenContext * context = new ListenContext( port, this );
    if ( context == nullptr )
    {
        return false;
    }

    pthread_mutex_lock( &m_Lock );
    m_ListenContexts.push_back( context );
    pthread_mutex_unlock( &m_Lock );

    return ( iolayer_listen( m_IOLayer, (uint8_t)type, host, port, options, onAcceptSession, context ) == 0 );
}

sid_t IIOService::connect( const char * host, uint16_t port, int32_t seconds )
{
    ConnectContext * context = new ConnectContext( host, port, this );
    if ( context == nullptr )
    {
        return -1;
    }

    if ( iolayer_connect( m_IOLayer, host, port, onConnectSession, context ) != 0 )
    {
        delete context;
        return -1;
    }

    // 异步模式需要加入连接的队列中
    if ( seconds <= 0 )
    {
        pthread_mutex_lock( &m_Lock );
        m_ConnectContexts.push_back( context );
        pthread_mutex_unlock( &m_Lock );

        return 0;
    }

    // 计算超时时间
    struct timeval now;
    gettimeofday( &now, nullptr );

    pthread_mutex_lock( &m_Lock );
    for ( ;; )
    {
        if ( context->sid != 0
                && context->sid != (sid_t)-1 )
        {
            break;
        }

        struct timespec outtime;
        outtime.tv_sec = now.tv_sec + seconds;
        outtime.tv_nsec = now.tv_usec * 1000;
        if ( ETIMEDOUT
                == pthread_cond_timedwait( &m_Cond, &m_Lock, &outtime ) )
        {
            break;
        }
    }
    pthread_mutex_unlock( &m_Lock );

    sid_t connectedsid = context->sid;

    delete context;
    return connectedsid;
}

int32_t IIOService::associate( int32_t fd, void * privdata, reattacher_t reattach, associator_t cb, void * context )
{
    return iolayer_associate( m_IOLayer, fd, privdata, reattach, cb, context );
}

int32_t IIOService::send( sid_t id, const std::string & buffer )
{
    return send( id, static_cast<const char *>(buffer.data()), buffer.size() );
}

int32_t IIOService::send( sid_t id, const char * buffer, size_t nbytes, bool isfree )
{
    return iolayer_send( m_IOLayer, id, buffer, nbytes, isfree );
}

int32_t IIOService::broadcast( const std::string & buffer )
{
    return iolayer_broadcast2( m_IOLayer,
            static_cast<const char *>(buffer.data()), buffer.size() );
}

int32_t IIOService::broadcast( const char * buffer, size_t nbytes )
{
    return iolayer_broadcast2( m_IOLayer, buffer, nbytes );
}

int32_t IIOService::broadcast( const sids_t & ids, const std::string & buffer )
{
    if ( ids.empty() )
    {
        return 0;
    }

    uint32_t count = (uint32_t)ids.size();
    sids_t::const_iterator start = ids.begin();

    return iolayer_broadcast( m_IOLayer,
            const_cast<sid_t *>( &(*start) ), count,
            static_cast<const char *>(buffer.data()), buffer.size() );
}

int32_t IIOService::broadcast( const sids_t & ids, const char * buffer, size_t nbytes )
{
    if ( ids.empty() )
    {
        return 0;
    }

    uint32_t count = (uint32_t)ids.size();
    sids_t::const_iterator start = ids.begin();

    return iolayer_broadcast( m_IOLayer, const_cast<sid_t *>( &(*start) ), count, buffer, nbytes );
}

int32_t IIOService::invoke( void * task, taskcloner_t clone, taskexecutor_t execute )
{
    return iolayer_invoke( m_IOLayer, task, clone, execute );
}

int32_t IIOService::perform( sid_t sid, int32_t type, void * task, taskrecycler_t recycle, int32_t interval )
{
    int32_t rc = iolayer_perform(
            m_IOLayer, sid, type, task, interval, recycle );

    if ( rc != 0 )
    {
        recycle( type, task, interval );
    }

    return rc;
}

int32_t IIOService::shutdown( sid_t id )
{
    return iolayer_shutdown( m_IOLayer, id );
}

int32_t IIOService::shutdown( const sids_t & ids )
{
    if ( ids.empty() )
    {
        return 0;
    }

    sids_t::const_iterator start = ids.begin();

    uint32_t count = (uint32_t)ids.size();
    sid_t * idlist = const_cast<sid_t *>( &(*start) );

    return iolayer_shutdowns( m_IOLayer, idlist, count );
}

void IIOService::notifyConnectResult( ConnectContext * context, int32_t result, sid_t id, int32_t ack )
{
    bool isremove = ( result == 0 || ack != 0 );

    pthread_mutex_lock( &m_Lock );

    // 会话ID的转换
    context->sid = result != 0 ? -1 : id;

    // 连接成功或者不再重连的情况下
    // 需要从连接队列中删除
    if ( isremove )
    {
        ConnectContexts::iterator it = std::find(
                m_ConnectContexts.begin(), m_ConnectContexts.end(), context );
        if ( it != m_ConnectContexts.end() )
        {
            it = m_ConnectContexts.erase( it );
            delete context;
        }
    }

    pthread_cond_signal( &m_Cond );
    pthread_mutex_unlock( &m_Lock );
}

void IIOService::initSession( sid_t id, IIOSession * session, void * iocontext, const std::string & host, uint16_t port )
{
    session->init( id, iocontext, m_IOLayer, host, port );

    ioservice_t ioservice;
    ioservice.start     = IIOSession::onStartSession;
    ioservice.process   = IIOSession::onProcessSession;
    ioservice.transform = m_Transform ? IIOSession::onTransformSession : nullptr;
    ioservice.timeout   = IIOSession::onTimeoutSession;
    ioservice.keepalive = IIOSession::onKeepaliveSession;
    ioservice.error     = IIOSession::onErrorSession;
    ioservice.shutdown  = IIOSession::onShutdownSession;
    ioservice.perform   = IIOSession::onPerformSession;
    iolayer_set_service( m_IOLayer, id, &ioservice, session );
}

char * IIOService::onTransformService( void * context, const char * buffer, size_t * nbytes )
{
    size_t & _nbytes = *nbytes;
    IIOService * service = static_cast<IIOService*>( context );

    return service->onTransform( buffer, _nbytes );
}

int32_t IIOService::onAcceptSession( void * context, void * iocontext, sid_t id, const char * host, uint16_t port )
{
    IIOSession * session = nullptr;
    ListenContext * ctx = static_cast<ListenContext*>( context );

    session = ctx->service->onAccept( id, ctx->port, host, port );
    if ( session == nullptr )
    {
        return -1;
    }
    ctx->service->initSession( id, session, iocontext, host, port );

    return 0;
}

int32_t IIOService::onConnectSession( void * context, void * iocontext, int32_t result, const char * host, uint16_t port, sid_t id )
{
    ConnectContext * ctx = static_cast<ConnectContext *>(context);

    int32_t ack = 0;
    IIOSession * session = nullptr;
    IIOService * service = ctx->service;

    if ( result != 0 )
    {
        ack = service->onConnectFailed( result, host, port ) ? 0 : -2;
    }
    else
    {
        session = service->onConnectSucceed( id, host, port );
        if ( session == nullptr )
        {
            ack = -1;
        }
        else
        {
            service->initSession( id, session, iocontext, host, port );
        }
    }

    // 通知连接结果
    service->notifyConnectResult( ctx, result, id, ack );

    return ack;
}
