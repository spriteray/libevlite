
#include <unistd.h>
#include <string.h>
#include <hiredis/hiredis.h>
#include "redis.h"

CRedisSession::CRedisSession( redisContext * c, int32_t seconds )
    : m_Timeoutseconds( seconds ),
      m_RedisConnection( c )
{}

CRedisSession::~CRedisSession()
{
    if ( m_RedisConnection != NULL )
    {
        redisFree( m_RedisConnection );
        m_RedisConnection = NULL;
    }
}

int32_t CRedisSession::onStart()
{
    // TODO: 验证

    setTimeout( m_Timeoutseconds );
    setKeepalive( m_Timeoutseconds / 3 );
    setEndpoint( m_RedisConnection->tcp.host, m_RedisConnection->tcp.port );

    return 0;
}

ssize_t CRedisSession::onProcess( const char * buffer, size_t nbytes )
{
    if ( 0 != redisReaderFeed(
                m_RedisConnection->reader, buffer, nbytes ) )
    {
        return -1;
    }

    void * data = NULL;
    redisReply * reply = NULL;

    while ( REDIS_OK
            == redisReaderGetReply( m_RedisConnection->reader, &data ) )
    {
        reply = static_cast<redisReply *>( data );
        if ( reply == NULL )
        {
            break;
        }

        switch ( reply->type )
        {
            case REDIS_REPLY_STRING :
                printf( "STRING : %s\n", reply->str );
                break;

            case REDIS_REPLY_ARRAY :
                printf( "ARRAY : %s\n", reply->str );
                break;

            case REDIS_REPLY_INTEGER :
                printf( "INTEGER : %s\n", reply->str );
                break;

            case REDIS_REPLY_NIL :
            case REDIS_REPLY_STATUS :
            case REDIS_REPLY_ERROR :
                printf( "OTHER : %s\n", reply->str );
                break;
        }

        freeReplyObject( reply );
    }

    return nbytes;
}

int32_t CRedisSession::onTimeout()
{
    return 0;
}

int32_t CRedisSession::onKeepalive()
{
    Slice cmd = RedisClient::ping();
    if ( !cmd.empty() )
    {
        send( cmd.data(), cmd.size(), true );
    }

    return 0;
}

int32_t CRedisSession::onError( int32_t result )
{
    // TODO:
    return 0;
}

void CRedisSession::onShutdown( int32_t way )
{
    // TODO:
}

////////////////////////////////////////////////////////////////////////////////

RedisClient::RedisClient( IIOService * s )
    : m_Service( s )
{
    pthread_cond_init( &m_Cond, NULL );
    pthread_mutex_init( &m_Lock, NULL );
}

RedisClient::~RedisClient()
{
    for ( size_t i = 0; i < m_AssociateList.size(); ++i )
    {
        delete m_AssociateList[i];
    }

    pthread_cond_destroy( &m_Cond );
    pthread_mutex_destroy( &m_Lock );
}

sid_t RedisClient::connect( const std::string & host, uint16_t port，int32_t seconds )
{
    redisContext * redisconn = NULL;
    AssociateContext * context = new AssociateContext();

    if ( seconds > 0 )
    {
        redisconn = redisConnectWithTimeout( host.c_str(), port, seconds );
        if ( redisconn == NULL )
        {
            delete context;
            return -1;
        }
    }
    else
    {
        redisconn = redisConnectNonBlock( host.c_str(), port );
        if ( redisconn == NULL )
        {
            delete context;
            return -1;
        }

        pthread_mutex_lock( &m_Lock );
        m_AssociateList.push_back( context );
        pthread_mutex_unlock( &m_Lock );
    }

    if ( 0 != m_Service->associate(
                redisconn->fd, redisconn,
                RedisClient::associate, RedisClient::reattach, context ) )
    {
        return -1;
    }

    // 计算超时时间
    struct timeval now;
    gettimeofday( &now, NULL );

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

    sid_t associatedsid = context->sid;

    delete context;
    return associatedsid;
}

int32_t RedisClient::send( sid_t id, const Slice & cmd )
{
    if ( cmd.empty() )
    {
        return -1;
    }

    return m_Service->send( id, cmd.data(), cmd.size(), true );
}

Slice RedisClient::ping()
{
    char * buffer = NULL;
    ssize_t length = redisFormatCommand( &buffer, "PING" );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice RedisClient::auth( const std::string & password )
{
    char * buffer = NULL;
    ssize_t length = redisFormatCommand( &buffer, "AUTH %s", password.c_str() );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice RedisClient::get( const std::string & key )
{
    char * buffer = NULL;
    ssize_t length = redisFormatCommand( &buffer, "GET %s", key.c_str() );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice RedisClient::get( const std::vector<std::string> & keys )
{
    char * buffer = NULL;
    //ssize_t length = redisFormatCommand( &buffer, "GET %s", key.c_str() );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice RedisClient::set( const std::string & key, const std::string & value )
{
    char * buffer = NULL;
    ssize_t length = redisFormatCommand(
            &buffer, "SET %s %b",
            key.c_str(), value.data(), value.size() );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

int32_t RedisClient::reattach( void * context, int32_t fd, void * privdata )
{
    redisContext * rc = static_cast<redisContext *>(privdata);

    redisReconnect( rc );
    return rc->fd;
}

int32_t RedisClient::associate( <#void *context#>, <#void *iocontext#>, <#int32_t result#>, <#int32_t fd#>, <#int sid#> )
{
    AssociateContext * ctx = static_cast<AssociateContext *>(context);

    int32_t ack = 0;
    IIOSession * session = NULL;
    IIOService * service = ctx->service;
    redisContext * redisconn = static_cast<redisContext *>(privdata);

    if ( result != 0 )
    {
        // ack = service->onConnectFailed( result, host, port ) ? 0 : -2;
        // TODO: 绑定失败
    }
    else
    {
        // TODO: 绑定成功
        CRedisSession * session = new CRedisSession( redisconn, 30 );
        if ( session != NULL )
        {
            // 初始化会话
            service->initSession( sid, session, iocontext );
        }
    }

    // 通知连接结果
    service->notifyAssociateResult( ctx, result, id, ack );

    return ack;
}

void RedisClient::notifyAssociateResult( AssociateContext * context, int32_t result, sid_t id, int32_t ack )
{
    bool isremove = ( result == 0 || ack != 0 );

    pthread_mutex_lock( &m_Lock );

    // 会话ID的转换
    context->sid = result != 0 ? -1 : id;

    // 连接成功或者不再重连的情况下
    // 需要从连接队列中删除
    if ( isremove )
    {
        AssociateContexts::iterator it = m_AssociateList.begin();
        for ( ; it != m_AssociateList.end(); )
        {
            if ( (*it) != context )
            {
                ++it;
            }
            else
            {
                it = m_AssociateList.erase( it );
                delete context;
                break;
            }
        }
    }

    pthread_cond_signal( &m_Cond );
    pthread_mutex_unlock( &m_Lock );
}

////////////////////////////////////////////////////////////////////////////////

class CExampleService : public IIOService
{
public :
    CExampleService()
        : IIOService( 1, 10, true, false )
    {}

    virtual CExampleService()
    {}
};

int main()
{
    CExampleService * s = new CExampleService();
    if ( s == NULL )
    {
        return -1;
    }

    s->start();

    RedisClient * rediscli = new RedisClient( s );
    if ( rediscli == NULL )
    {
        return -2;
    }

    sid_t sid = rediscli->connect( "172.21.161.70", 6379 );
    printf( "connect succeed, %lu\n", sid );

    rediscli->send( sid,
            rediscli->set( "account", "lei.a.zhang" ) );

    rediscli->send( sid,
            rediscli->get( "account" ) );

    while( 1 )
    {
        sleep( 2 );
    }

    s->stop();
    delete s;
}
