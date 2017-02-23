
#include <cassert>
#include <sys/time.h>

#include "io.h"

IIOSession::IIOSession()
    : m_Sid( 0 ),
      m_Port( 0 ),
      m_Layer( NULL ),
      m_IOContext( NULL )
{}

IIOSession::~IIOSession()
{}

void IIOSession::setTimeout( int32_t seconds )
{
    assert( m_Sid != 0 && m_Layer != NULL );
    iolayer_set_timeout( m_Layer, m_Sid, seconds );
}

void IIOSession::setKeepalive( int32_t seconds )
{
    assert( m_Sid != 0 && m_Layer != NULL );
    iolayer_set_keepalive( m_Layer, m_Sid, seconds );
}

int32_t IIOSession::send( const std::string & buffer )
{
    return send( buffer.c_str(), static_cast<uint32_t>(buffer.length()) );
}

int32_t IIOSession::send( const char * buffer, uint32_t nbytes, bool isfree )
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

int32_t IIOSession::onProcessSession( void * context, const char * buffer, uint32_t nbytes )
{
    return static_cast<IIOSession *>(context)->onProcess( buffer, nbytes );
}

char * IIOSession::onTransformSession( void * context, const char * buffer, uint32_t * nbytes )
{
    uint32_t & _nbytes = *nbytes;
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

// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------

IIOService::IIOService( uint8_t nthreads, uint32_t nclients, bool immediately )
    : m_IOLayer( NULL ),
      m_ThreadsCount( nthreads ),
      m_SessionsCount( nclients ),
      m_IOContextGroup( NULL )
{
    pthread_cond_init( &m_Cond, NULL );
    pthread_mutex_init( &m_Lock, NULL );

    m_IOLayer = iolayer_create( m_ThreadsCount, m_SessionsCount, immediately ? 1 : 0 );

    if ( m_IOLayer != NULL )
    {
        m_IOContextGroup = new void * [ m_ThreadsCount ];
        if ( m_IOContextGroup != NULL )
        {
            for ( uint8_t i = 0; i < m_ThreadsCount; ++i )
            {
                m_IOContextGroup[ i ] = initIOContext();
            }
        }

        iolayer_set_transform( m_IOLayer, onTransformService, this );
        iolayer_set_iocontext( m_IOLayer, m_IOContextGroup, m_ThreadsCount );
    }
}

IIOService::~IIOService()
{
    if ( m_IOLayer != NULL )
    {
        iolayer_destroy( m_IOLayer );
        m_IOLayer = NULL;
    }

    if ( m_IOContextGroup != NULL )
    {
        for ( uint8_t i = 0; i < m_ThreadsCount; ++i )
        {
            if ( m_IOContextGroup != NULL )
            {
                finalIOContext( m_IOContextGroup[i] );
            }
        }

        delete [] m_IOContextGroup;
    }

    pthread_cond_destroy( &m_Cond );
    pthread_mutex_destroy( &m_Lock );
}

sid_t IIOService::id( const char * host, uint16_t port )
{
    sid_t sid = 0;

    pthread_mutex_lock( &m_Lock );
    sid = this->getConnectedSid( host, port );
    pthread_mutex_unlock( &m_Lock );

    return sid == (sid_t)-1 ? 0 : sid;
}

bool IIOService::listen( const char * host, uint16_t port )
{
    return ( iolayer_listen( m_IOLayer, host, port, onAcceptSession, this ) == 0 );
}

bool IIOService::connect( const char * host, uint16_t port, int32_t seconds, bool isblock )
{
    if ( iolayer_connect( m_IOLayer, host, port, seconds, onConnectSession, this ) != 0 )
    {
        return false;
    }

    if ( !isblock )
    {
        return true;
    }

    sid_t connectedsid = 0;

    // 计算超时时间
    struct timeval now;
    gettimeofday( &now, NULL );

    pthread_mutex_lock( &m_Lock );
    for ( ;; )
    {
        connectedsid = getConnectedSid( host, port );
        if ( connectedsid != 0 )
        {
            break;
        }

        struct timespec outtime;
        outtime.tv_sec = now.tv_sec + seconds;
        outtime.tv_nsec = now.tv_usec * 1000;
        pthread_cond_timedwait( &m_Cond, &m_Lock, &outtime );
    }
    pthread_mutex_unlock( &m_Lock );

    return connectedsid > 0 && connectedsid != (sid_t)-1 ;
}

void IIOService::stop()
{
    if ( m_IOLayer != NULL )
    {
        iolayer_stop( m_IOLayer );
    }
}

int32_t IIOService::send( sid_t id, const std::string & buffer )
{
    return send( id, static_cast<const char *>(buffer.data()), static_cast<uint32_t>(buffer.size()) );
}

int32_t IIOService::send( sid_t id, const char * buffer, uint32_t nbytes, bool isfree )
{
    return iolayer_send( m_IOLayer, id, buffer, nbytes, isfree );
}

int32_t IIOService::broadcast( const std::string & buffer )
{
    uint32_t nbytes = static_cast<uint32_t>(buffer.size());
    const char * buf = static_cast<const char *>( buffer.data() );

    return iolayer_broadcast2( m_IOLayer, buf, nbytes );
}

int32_t IIOService::broadcast( const std::vector<sid_t> & ids, const std::string & buffer )
{
    uint32_t nbytes = static_cast<uint32_t>(buffer.size());
    const char * buf = static_cast<const char *>( buffer.data() );

    uint32_t count = (uint32_t)ids.size();
    std::vector<sid_t>::const_iterator start = ids.begin();

    return iolayer_broadcast( m_IOLayer, const_cast<sid_t *>( &(*start) ), count, buf, nbytes );
}

int32_t IIOService::shutdown( sid_t id )
{
    return iolayer_shutdown( m_IOLayer, id );
}

int32_t IIOService::shutdown( const std::vector<sid_t> & ids )
{
    std::vector<sid_t>::const_iterator start = ids.begin();

    uint32_t count = (uint32_t)ids.size();
    sid_t * idlist = const_cast<sid_t *>( &(*start) );

    return iolayer_shutdowns( m_IOLayer, idlist, count );
}

void IIOService::attach( sid_t id, IIOSession * session, void * iocontext, const std::string & host, uint16_t port )
{
    session->init( id, iocontext, m_IOLayer, host, port );

    ioservice_t ioservice;
    ioservice.start     = IIOSession::onStartSession;
    ioservice.process   = IIOSession::onProcessSession;
    ioservice.transform = IIOSession::onTransformSession;
    ioservice.timeout   = IIOSession::onTimeoutSession;
    ioservice.keepalive = IIOSession::onKeepaliveSession;
    ioservice.error     = IIOSession::onErrorSession;
    ioservice.shutdown  = IIOSession::onShutdownSession;
    iolayer_set_service( m_IOLayer, id, &ioservice, session );
}

sid_t IIOService::getConnectedSid( const char * host, uint16_t port ) const
{
    for ( size_t i = 0; i < m_RemoteHosts.size(); ++i )
    {
        if ( m_RemoteHosts[i].port == port
                && m_RemoteHosts[i].host == std::string( host ) )
        {
            return m_RemoteHosts[i].sid;
        }
    }

    return 0;
}

void IIOService::setConnectedSid( const char * host, uint16_t port, sid_t sid )
{
    pthread_mutex_lock( &m_Lock );

    if ( sid != 0 )
    {
        bool is_add = true;

        for ( size_t i = 0; i < m_RemoteHosts.size(); ++i )
        {
            if ( m_RemoteHosts[i].port == port
                    && m_RemoteHosts[i].host == std::string( host ) )
            {
                is_add = false;
                m_RemoteHosts[i].sid = sid;
            }
        }

        if ( is_add )
        {
            m_RemoteHosts.push_back( RemoteHost( host, port, sid ) );
        }
    }
    pthread_cond_signal( &m_Cond );

    pthread_mutex_unlock( &m_Lock );
}

char * IIOService::onTransformService( void * context, const char * buffer, uint32_t * nbytes )
{
    uint32_t & _nbytes = *nbytes;
    IIOService * service = static_cast<IIOService*>( context );

    return service->onTransform( buffer, _nbytes );
}

int32_t IIOService::onAcceptSession( void * context, void * iocontext, sid_t id, const char * host, uint16_t port )
{
    IIOSession * session = NULL;
    IIOService * service = static_cast<IIOService*>( context );

    session = service->onAccept( id, host, port );
    if ( session == NULL )
    {
        return -1;
    }
    service->attach( id, session, iocontext, host, port );

    return 0;
}

int32_t IIOService::onConnectSession( void * context, void * iocontext, int32_t result, const char * host, uint16_t port, sid_t id )
{
    IIOSession * session = NULL;
    IIOService * service = static_cast<IIOService *>( context );

    // 通知
    service->setConnectedSid( host, port, result != 0 ? -1 : id );

    if ( result != 0 )
    {
        // 失败
        return service->onConnectFailed( result, host, port ) ? 0 : -2;
    }

    // 成功
    session = service->onConnectSucceed( id, host, port );
    if ( session == NULL )
    {
        return -1;
    }
    service->attach( id, session, iocontext, host, port );

    return 0;
}
