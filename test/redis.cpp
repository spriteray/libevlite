
#include <cassert>
#include <algorithm>

#include <errno.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <hiredis/hiredis.h>

#include "redis.h"

IRedisSession::IRedisSession( redisContext * c, int32_t seconds )
    : m_Timeoutseconds( seconds ),
      m_RedisConnection( c )
{}

IRedisSession::~IRedisSession()
{
    if ( m_RedisConnection != NULL )
    {
        redisFree( m_RedisConnection );
        m_RedisConnection = NULL;
    }
}

int32_t IRedisSession::onStart()
{
    // TODO: 验证

    setTimeout( m_Timeoutseconds );
    setKeepalive( m_Timeoutseconds / 3 );

    return 0;
}

ssize_t IRedisSession::onProcess( const char * buffer, size_t nbytes )
{
    if ( 0 != redisReaderFeed(
                m_RedisConnection->reader, buffer, nbytes ) )
    {
        return -1;
    }

    void * data = NULL;
    while ( REDIS_OK
            == redisReaderGetReply( m_RedisConnection->reader, &data ) )
    {
        if ( data == NULL )
        {
            break;
        }

        processReply( static_cast<redisReply *>( data ) );
        freeReplyObject( static_cast<redisReply *>( data ) );
    }

    return nbytes;
}

int32_t IRedisSession::onTimeout()
{
    return 0;
}

int32_t IRedisSession::onKeepalive()
{
    Slice cmd = IRedisClient::ping();
    if ( !cmd.empty() )
    {
        send( cmd.data(), cmd.size(), true );
    }

    return 0;
}

int32_t IRedisSession::onError( int32_t result )
{
    printf( "IRedisSession::onError(SID:%lu, %d) .\n", id(), result );
    return 0;
}

void IRedisSession::onShutdown( int32_t way )
{
    // TODO:
}

void IRedisSession::processReply( redisReply * reply )
{
    switch ( reply->type )
    {
        case REDIS_REPLY_STRING :
            printf( "STRING : %s\n", reply->str );
            break;

        case REDIS_REPLY_ARRAY :
            printf( "ARRAY : %lu\n", reply->elements);
            for ( size_t i = 0; i < reply->elements; ++i )
            {
                processReply( reply->element[i] );
            }
            break;

        case REDIS_REPLY_INTEGER :
            printf( "INTEGER : %llu\n", reply->integer );
            break;

        case REDIS_REPLY_NIL :
        case REDIS_REPLY_STATUS :
        case REDIS_REPLY_ERROR :
            printf( "OTHER : %s\n", reply->str );
            break;
    }
}

IRedisClient::IRedisClient( IIOService * s )
    : m_Service( s )
{
    pthread_cond_init( &m_Cond, NULL );
    pthread_mutex_init( &m_Lock, NULL );
}

IRedisClient::~IRedisClient()
{
    for ( size_t i = 0; i < m_AssociateList.size(); ++i )
    {
        delete m_AssociateList[i];
    }

    pthread_cond_destroy( &m_Cond );
    pthread_mutex_destroy( &m_Lock );
}

sid_t IRedisClient::connect( const std::string & host, uint16_t port,int32_t seconds )
{
    sid_t associatedsid = 0;
    redisContext * redisconn = NULL;

    // 计算超时时间
    struct timeval now;
    gettimeofday( &now, NULL );

    // 分情况连接redis服务器
    if ( seconds <= 0 )
    {
        redisconn = redisConnectNonBlock( host.c_str(), port );
    }
    else
    {
        struct timeval tv = {seconds/1000, (seconds%1000)*1000};
        redisconn = redisConnectWithTimeout( host.c_str(), port, tv );
    }

    // 连接失败
    if ( redisconn == NULL || redisconn->err != 0 )
    {
        return -1;
    }

    // 关联描述符到IIOService中
    AssociateContext * context = new AssociateContext( redisconn->fd, redisconn, this );
    assert( context != NULL && "new AssociateContext failed" );
    if ( 0 != m_Service->associate(
                redisconn->fd, redisconn,
                IRedisClient::reattach,
                IRedisClient::associate, context ) )
    {
        delete context;
        return -2;
    }

    // 非阻塞,直接返回
    // 异步模式需要加入连接的队列中
    if ( seconds <= 0 )
    {
        pthread_mutex_lock( &m_Lock );
        m_AssociateList.push_back( context );
        pthread_mutex_unlock( &m_Lock );
    }
    else
    {
        // 等待关联结果
        associatedsid = waitForAssociate( context, now, seconds );
        delete context;
    }

    return associatedsid;
}

int32_t IRedisClient::send( sid_t id, const Slice & cmd )
{
    if ( cmd.empty() )
    {
        return -1;
    }

    return m_Service->send( id, cmd.data(), cmd.size(), true );
}

Slice IRedisClient::ping()
{
    char * buffer = NULL;
    ssize_t length = redisFormatCommand( &buffer, "PING" );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice IRedisClient::echo( const std::string & text )
{
    char * buffer = NULL;
    ssize_t length = redisFormatCommand(
            &buffer, "ECHO \"%s\"", text.c_str() );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice IRedisClient::select( int32_t index )
{
    char * buffer = NULL;
    ssize_t length = redisFormatCommand( &buffer, "SELECT %d", index );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice IRedisClient::auth( const std::string & password )
{
    char * buffer = NULL;
    ssize_t length = redisFormatCommand(
            &buffer, "AUTH %s", password.c_str() );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice IRedisClient::get( const std::string & key )
{
    char * buffer = NULL;
    ssize_t length = redisFormatCommand(
            &buffer, "GET %s", key.c_str() );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice IRedisClient::mget( const std::vector<std::string> & keys )
{
    size_t ncmds = keys.size() + 1;
    size_t argvlen[ ncmds ];
    const char * argv[ ncmds ];

    argv[0] = "MGET";
    argvlen[0] = 4;

    for ( size_t i = 0; i < keys.size(); ++i )
    {
        argv[i+1] = keys[i].c_str();
        argvlen[i+1] = keys[i].size();
    }

    char * buffer = NULL;
    ssize_t length = redisFormatCommandArgv(
            &buffer,
            keys.size()+1, argv, argvlen );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice IRedisClient::set( const std::string & key, const std::string & value )
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

Slice IRedisClient::incr( const std::string & key )
{
    char * buffer = NULL;
    ssize_t length = redisFormatCommand(
            &buffer, "INCR %s", key.c_str() );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice IRedisClient::incrby( const std::string & key, int32_t value )
{
    char * buffer = NULL;
    ssize_t length = redisFormatCommand(
            &buffer, "INCRBY %s %d", key.c_str(), value );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice IRedisClient::decr( const std::string & key )
{
    char * buffer = NULL;
    ssize_t length = redisFormatCommand(
            &buffer, "DECR %s", key.c_str() );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice IRedisClient::decrby( const std::string & key, int32_t value )
{
    char * buffer = NULL;
    ssize_t length = redisFormatCommand(
            &buffer, "DECRBY %s %d", key.c_str(), value );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice IRedisClient::lpush( const std::string & key, const std::vector<std::string> & values )
{
    size_t ncmds = values.size() + 2;

    size_t argvlen[ ncmds ];
    const char * argv[ ncmds ];

    argv[0] = "LPUSH";
    argvlen[0] = 5;
    argv[1] = key.c_str();
    argvlen[1] = key.size();

    for ( size_t i = 0; i < values.size(); ++i )
    {
        argv[i+2] = values[i].c_str();
        argvlen[i+2] = values[i].size();
    }

    char * buffer = NULL;
    ssize_t length = redisFormatCommandArgv(
            &buffer,
            ncmds, argv, argvlen );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice IRedisClient::rpush( const std::string & key, const std::vector<std::string> & values )
{
    size_t ncmds = values.size() + 2;

    size_t argvlen[ ncmds ];
    const char * argv[ ncmds ];

    argv[0] = "RPUSH";
    argvlen[0] = 5;
    argv[1] = key.c_str();
    argvlen[1] = key.size();

    for ( size_t i = 0; i < values.size(); ++i )
    {
        argv[i+2] = values[i].c_str();
        argvlen[i+2] = values[i].size();
    }

    char * buffer = NULL;
    ssize_t length = redisFormatCommandArgv(
            &buffer,
            ncmds, argv, argvlen );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice IRedisClient::lrange( const std::string & key, int32_t startidx, int32_t stopidx )
{
    char * buffer = NULL;
    ssize_t length = redisFormatCommand(
            &buffer, "LRANGE %s %d %d", key.c_str(), startidx, stopidx );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice IRedisClient::subscribe( const std::string & channel )
{
    char * buffer = NULL;
    ssize_t length = redisFormatCommand(
            &buffer, "SUBSCRIBE %s", channel.c_str() );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice IRedisClient::unsubscribe( const std::string & channel )
{
    char * buffer = NULL;
    ssize_t length = redisFormatCommand(
            &buffer, "UNSUBSCRIBE %s", channel.c_str() );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice IRedisClient::publish( const std::string & channel, const std::string & message )
{
    char * buffer = NULL;
    ssize_t length = redisFormatCommand(
            &buffer, "PUBLISH %s \"%s\"", channel.c_str(), message.c_str() );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice IRedisClient::multi()
{
    char * buffer = NULL;
    ssize_t length = redisFormatCommand( &buffer, "MULTI" );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice IRedisClient::pipeline()
{
    char * buffer = NULL;
    ssize_t length = redisFormatCommand( &buffer, "PIPELINE" );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice IRedisClient::exec()
{
    char * buffer = NULL;
    ssize_t length = redisFormatCommand( &buffer, "EXEC" );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

int32_t IRedisClient::reattach( int32_t fd, void * privdata )
{
    redisContext * redisconn = static_cast<redisContext *>(privdata);

    printf( "IRedisClient::reattach(%d, %s::%d) ...\n", fd, redisconn->tcp.host, redisconn->tcp.port );

    int32_t ret = redisReconnect( redisconn );
    if ( ret != REDIS_OK || redisconn->err != 0 )
    {
        return -1;
    }

    return redisconn->fd;
}

int32_t IRedisClient::associate( void * context, void * iocontext, int32_t result, int32_t fd, void * privdata, sid_t sid )
{
    AssociateContext * ctx = static_cast<AssociateContext *>(context);

    int32_t ack = 0;
    IRedisClient * client = ctx->client;
    redisContext * redisconn = static_cast<redisContext *>(privdata);

    if ( result != 0 )
    {
        ack = client->onConnectFailed( result, fd, redisconn ) ? 0 : -2;
    }
    else
    {
        IRedisSession * session = client->onConnectSucceed( sid, fd, redisconn );
        if ( session == NULL )
        {
            ack = -1;
        }
        else
        {
            // 初始化会话
            session->init( sid,
                    iocontext, client->service()->iolayer(),
                    redisconn->tcp.host, redisconn->tcp.port );

            ioservice_t ioservice;
            ioservice.start     = IRedisSession::onStartSession;
            ioservice.process   = IRedisSession::onProcessSession;
            ioservice.transform = NULL;
            ioservice.timeout   = IRedisSession::onTimeoutSession;
            ioservice.keepalive = IRedisSession::onKeepaliveSession;
            ioservice.error     = IRedisSession::onErrorSession;
            ioservice.shutdown  = IRedisSession::onShutdownSession;
            ioservice.perform   = IRedisSession::onPerformSession;
            iolayer_set_service( client->service()->iolayer(), sid, &ioservice, session );
        }
    }

    // 通知连接结果
    client->wakeupFromAssociating( ctx, result, sid, ack );

    return ack;
}

sid_t IRedisClient::waitForAssociate( AssociateContext * context, struct timeval & now, int32_t seconds )
{
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

    return context->sid;
}

void IRedisClient::wakeupFromAssociating( AssociateContext * context, int32_t result, sid_t id, int32_t ack )
{
    bool isremove = ( result == 0 || ack != 0 );

    pthread_mutex_lock( &m_Lock );

    // 会话ID的转换
    context->sid = result != 0 ? -1 : id;

    // 连接成功或者不再重连的情况下
    // 需要从连接队列中删除
    if ( isremove )
    {
        AssociateContexts::iterator it = std::find(
                m_AssociateList.begin(), m_AssociateList.end(), context );
        if ( it != m_AssociateList.end() )
        {
            it = m_AssociateList.erase( it );
            delete context;
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

    virtual ~CExampleService()
    {}
};

class CRedisClient : public IRedisClient
{
public :
    CRedisClient( IIOService * s ) : IRedisClient(s) {}
    virtual ~CRedisClient() {}

    virtual IRedisSession * onConnectSucceed( sid_t sid, int32_t fd, redisContext * conn )
    {
        return new IRedisSession( conn, 30 );
    }
};

int main()
{
    CExampleService * s = new CExampleService();
    if ( s == NULL )
    {
        return -1;
    }

    s->start();

    CRedisClient * rediscli = new CRedisClient( s );
    if ( rediscli == NULL )
    {
        return -2;
    }

    sid_t sid = rediscli->connect(
            "172.21.161.70", 6379, 10 );
    if ( sid == (sid_t)-1 )
    {
        printf( "connect failed .\n" );
        return -3;
    }

    printf( "connect succeed, %lu\n", sid );

    rediscli->send( sid,
            rediscli->set( "account", "lei.a.zhang" ) );
    rediscli->send( sid,
            rediscli->set( "account1", "lei.a.zhang" ) );

    rediscli->send( sid,
            rediscli->get( "account" ) );

    rediscli->send( sid,
            rediscli->subscribe( "ope_channel" ) );

    while( 1 )
    {
        sleep( 2 );

        std::vector<std::string> cmds;
        cmds.push_back( "account" );
        cmds.push_back( "account1" );
        rediscli->send( sid,
                rediscli->mget( cmds ) );
    }

    s->stop();
    delete s;
}
