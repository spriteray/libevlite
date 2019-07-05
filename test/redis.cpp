
#include <unistd.h>
#include <string.h>
#include <hiredis/hiredis.h>
#include "redis.h"

CRedisSession::CRedisSession( redisContext * c )
    : m_RedisConnection( c )
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

    setTimeout( 30 );
    setKeepalive( 10 );
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

CRedisService::CRedisService()
    : IIOService( 1, 10, true, false )
{}

CRedisService::~CRedisService()
{}

IIOSession * CRedisService::onAssociateSucceed( sid_t id, int32_t type, int32_t fd, void * privdata )
{
    redisContext * context = static_cast<redisContext *>(privdata);
    CRedisSession * session = new CRedisSession( context );
    if ( session != NULL )
    {
        // TODO:
    }

    return session;
}

////////////////////////////////////////////////////////////////////////////////

RedisClient::RedisClient( IIOService * s )
    : m_Service( s )
{}

RedisClient::~RedisClient()
{}

sid_t RedisClient::connect( const std::string & host, uint16_t port )
{
    redisContext * c = redisConnectNonBlock( host.c_str(), port );
    if ( c == NULL )
    {
        return 0;
    }

    return m_Service->associate( 0, c->fd, c, 10, RedisClient::reattach, true );
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

////////////////////////////////////////////////////////////////////////////////

int main()
{
    CRedisService * s = new CRedisService();
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
        sleep( 1 );
    }

    s->stop();
    delete s;
}
