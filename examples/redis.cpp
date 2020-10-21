
#include <cassert>
#include <algorithm>

#include <errno.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <hiredis/hiredis.h>

#include "command.h"
#include "redis.h"

class RedisDBTask
{
public :
    RedisDBTask( const Slice & cmd )
        : m_Counter( -1 ),
          m_RedisCommand( cmd )
    {}

    RedisDBTask( const Slice & cmd, uint32_t ctxid )
        : m_Counter( 1 ),
          m_ContextID( ctxid ),
          m_RedisCommand( cmd )
    {}

    RedisDBTask( size_t count, const Slice & cmd, uint32_t ctxid )
        : m_Counter( count ),
          m_ContextID( ctxid ),
          m_RedisCommand( cmd )
    {}

    ~RedisDBTask()
    {
        if ( !m_RedisCommand.empty() )
        {
            std::free( (void *)m_RedisCommand.data() );
            m_RedisCommand.clear();
        }
    }

    static void perform( void * iocontext, void * task )
    {
        RedisDBTask * t = static_cast<RedisDBTask *>(task);
        RedisConnectionPool * pool = static_cast<RedisConnectionPool *>(iocontext);

        pool->perform( t );
    }

public :
    size_t decrCounter() { return --m_Counter; }

    size_t getCounter() const { return m_Counter; }
    uint32_t getContextID() const { return m_ContextID; }
    const Slice & getCommand() const { return m_RedisCommand; }

private :
    size_t      m_Counter;        // 任务计数器
    uint32_t    m_ContextID;
    Slice       m_RedisCommand;
};

class RedisConnectionPool;
class RedisConnection : public IIOSession
{
public :
    RedisConnection( redisContext * c )
        : m_Task( NULL ),
          m_RedisConnection( c )
    {}

    virtual ~RedisConnection()
    {
        if ( m_RedisConnection != NULL )
        {
            redisFree( m_RedisConnection );
            m_RedisConnection = NULL;
        }
    }

    virtual int32_t onStart()
    {
        // TODO: 验证
        // TODO: 心跳
        // TODO: 保活

        // 添加到连接池
        getConnectionPool()->addConnection( id(), this );

        return 0;
    }

    virtual ssize_t onProcess( const char * buffer, size_t nbytes )
    {
        RedisConnectionPool * pool = getConnectionPool();
        IRedisClient * redisclient = pool->getRedisClient();

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

            redisReply * reply = static_cast<redisReply *>( data );

            assert( m_Task != NULL && "Empty Task" );

            if ( m_Task->getContextID() == 0 )
            {
                debugReply( reply );
                freeReplyObject( reply );
            }
            else
            {
                redisclient->datasets( m_Task->getContextID(), reply );
            }

            if ( m_Task->decrCounter() == 0 )
            {
                delete m_Task; m_Task = NULL;
                pool->addIdleConnection( id(), this );
            }
        }

        return nbytes;
    }

    virtual int32_t onTimeout()
    {
        return 0;
    }

    virtual int32_t onKeepalive()
    {
        // TODO:
        return 0;
    }

    virtual int32_t onError( int32_t result )
    {
        // TODO:
        return 0;
    }

    virtual void    onShutdown( int32_t way )
    {
        // TODO:
    }

public :
    //
    void setTask( RedisDBTask * task ) { m_Task = task; }

    static void debugReply( redisReply * reply )
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
                    debugReply( reply->element[i] );
                }
                break;

            case REDIS_REPLY_INTEGER :
                printf( "INTEGER : %llu\n", reply->integer );
                break;

            case REDIS_REPLY_NIL :
                printf( "NIL : %s\n", reply->str );
                break;

            case REDIS_REPLY_STATUS :
                printf( "STATUS : %s\n", reply->str );
                break;

            case REDIS_REPLY_ERROR :
                printf( "ERROR : %s\n", reply->str );
                break;
        }
    }

private :
    RedisConnectionPool * getConnectionPool() { return static_cast<RedisConnectionPool *>(iocontext()); }

protected :
    friend class IRedisClient;
    RedisDBTask *       m_Task;
    redisContext *      m_RedisConnection;
};

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

int32_t RedisConnectionPool::perform( RedisDBTask * task )
{
    sid_t sid = takeIdleConnection();
    if ( sid == 0 )
    {
        //
        m_TaskQueue.push_back( task );

        //
        if ( m_ConnectionManager.size() < m_MaxConnections )
        {
            m_Client->attach();
        }

        return 0;
    }

    RedisConnection * connection = getConnection( sid );
    if ( connection != NULL )
    {
        connection->setTask( task );
        connection->send( task->getCommand().data(), task->getCommand().size(), false );
    }

    return 0;
}

void RedisConnectionPool::fetchAndPerformTask( RedisConnection * connection )
{
    if ( m_TaskQueue.empty() )
    {
        return;
    }

    RedisDBTask * task = m_TaskQueue.front();
    if ( task != NULL )
    {
        m_TaskQueue.pop_front();

        connection->setTask( task );
        connection->send( task->getCommand().data(), task->getCommand().size(), false );
    }
}

sid_t RedisConnectionPool::takeIdleConnection()
{
    sid_t sid = 0;

    if ( !m_IdleSidlist.empty() )
    {
        sid = m_IdleSidlist.front();
        m_IdleSidlist.pop_front();
    }

    return sid;
}

RedisConnection * RedisConnectionPool::getConnection( sid_t sid ) const
{
    ConnectionManager::const_iterator result = m_ConnectionManager.find( sid );
    if ( result != m_ConnectionManager.end() )
    {
        return result->second;
    }

    return NULL;
}

void RedisConnectionPool::addIdleConnection( sid_t sid, RedisConnection * connection )
{
    if ( m_TaskQueue.empty() )
    {
        m_IdleSidlist.push_back( sid );
    }
    else
    {
        fetchAndPerformTask( connection );
    }
}

void RedisConnectionPool::addConnection( sid_t sid, RedisConnection * connection )
{
    m_ConnectionManager.insert( std::make_pair(sid, connection) );

    if ( m_TaskQueue.empty() )
    {
        m_IdleSidlist.push_back( sid );
    }
    else
    {
        fetchAndPerformTask( connection );
    }
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

IRedisClient::IRedisClient( uint8_t nthreads, uint32_t nconnections )
    : IIOService( nthreads, nconnections, true, false ),
      m_Port( 0 ),
      m_ThreadCount( nthreads ),
      m_Connections( nconnections )
{
    pthread_cond_init( &m_Cond, NULL );
    pthread_mutex_init( &m_Lock, NULL );
}

IRedisClient::~IRedisClient()
{
    pthread_cond_destroy( &m_Cond );
    pthread_mutex_destroy( &m_Lock );
}

bool IRedisClient::initialize(
        const std::string & host, uint16_t port, uint32_t nconnections, const std::string & token )
{
    m_Host = host;
    m_Port = port;
    m_Token = token;

    if ( !start() )
    {
        return false;
    }

    for ( uint32_t i = 0; i < nconnections; ++i )
    {
        attach();
    }

    return true;
}

void IRedisClient::finalize()
{
    halt();
    stop();
}

int32_t IRedisClient::subscribe( const Slice & cmd )
{
    if ( cmd.empty() )
    {
        return -1;
    }

    return perform( new RedisDBTask(cmd), NULL, RedisDBTask::perform );
}

int32_t IRedisClient::submit( const Slice & cmd, uint32_t ctxid )
{
    if ( cmd.empty() )
    {
        return -1;
    }

    return perform( new RedisDBTask(cmd, ctxid), NULL, RedisDBTask::perform );
}

int32_t IRedisClient::submit( const Slices & cmds, uint32_t ctxid )
{
    if ( cmds.empty() )
    {
        return -1;
    }

    char * buffer = NULL;
    size_t len = 0, count = 0;

    for ( size_t i = 0; i < cmds.size(); ++i )
    {
        len += cmds[i].size();
    }

    buffer = (char *)malloc( len );
    assert( buffer != NULL && "allocate this Buffer failed" );

    len = 0;
    count = 0;

    for ( size_t i = 0; i < cmds.size(); ++i )
    {
        if ( cmds[i].empty() )
        {
            continue;
        }

        memcpy( buffer+len, cmds[i].data(), cmds[i].size() );
        ++count;
        len += cmds[i].size();
        std::free( ( void * )( cmds[i].data() ) );
    }

    return perform( new RedisDBTask(count, Slice(buffer, len), ctxid), NULL, RedisDBTask::perform );
}

void * IRedisClient::initIOContext()
{
    return new RedisConnectionPool( this, m_Connections/m_ThreadCount );
}

void IRedisClient::finalIOContext( void * iocontext )
{
    RedisConnectionPool * pool = static_cast<RedisConnectionPool *>( iocontext );
    if ( pool != NULL )
    {
        delete pool;
    }
}

int32_t IRedisClient::attach()
{
    // 连接远程redis
    redisContext * redisconn = redisConnectNonBlock( m_Host.c_str(), m_Port );
    if ( redisconn == NULL || redisconn->err != 0 )
    {
        return -1;
    }

    // 关联描述符到IIOService中
    if ( 0 != associate(
                redisconn->fd, redisconn,
                IRedisClient::reattach,
                IRedisClient::associatecb, this ) )
    {
        return -2;
    }

    return 0;
}

int32_t IRedisClient::reattach( int32_t fd, void * privdata )
{
    redisContext * redisconn = static_cast<redisContext *>(privdata);

    int32_t ret = redisReconnect( redisconn );
    if ( ret != REDIS_OK || redisconn->err != 0 )
    {
        return -1;
    }

    return redisconn->fd;
}

int32_t IRedisClient::associatecb( void * context, void * iocontext, int32_t result, int32_t fd, void * privdata, sid_t sid )
{
    if ( result != 0 )
    {
        return -1;
    }

    IRedisClient * client = static_cast<IRedisClient *>(context);
    redisContext * redisconn = static_cast<redisContext *>(privdata);

    RedisConnection * connection = new RedisConnection( redisconn );
    if ( connection == NULL )
    {
        return -2;
    }
    else
    {
        // 初始化会话
        connection->init( sid,
                iocontext, client->iolayer(),
                redisconn->tcp.host, redisconn->tcp.port );

        ioservice_t ioservice;
        ioservice.start     = RedisConnection::onStartSession;
        ioservice.process   = RedisConnection::onProcessSession;
        ioservice.transform = NULL;
        ioservice.timeout   = RedisConnection::onTimeoutSession;
        ioservice.keepalive = RedisConnection::onKeepaliveSession;
        ioservice.error     = RedisConnection::onErrorSession;
        ioservice.shutdown  = RedisConnection::onShutdownSession;
        ioservice.perform   = RedisConnection::onPerformSession;
        iolayer_set_service( client->iolayer(), sid, &ioservice, connection );
    }

    return 0;
}

////////////////////////////////////////////////////////////////////////////////

class CRedisClient : public IRedisClient
{
public :
    CRedisClient() : IRedisClient(4, 1000) {}
    virtual ~CRedisClient() {}

    virtual void datasets( uint32_t ctxid, redisReply * response )
    {
        // TODO:
        RedisConnection::debugReply( response );
        freeReplyObject( response );
    }
};

int main()
{
    CRedisClient * rediscli = new CRedisClient();
    if ( rediscli == NULL )
    {
        return -2;
    }

    // 初始化
    rediscli->initialize( "172.21.161.70", 6379 );

    // 订阅
    rediscli->subscribe( RedisCommand::subscribe("ope") );

    while( 1 )
    {
        sleep( 2 );

        //std::vector<std::string> cmds;
        //cmds.push_back( "account" );
        //cmds.push_back( "account1" );
        //rediscli->submit( RedisCommand::mget( cmds ) );

        rediscli->submit( RedisCommand::set( "roleid1", "17648020619275" ) );

        //Slices pipeline;
        //pipeline.push_back( RedisCommand::echo( "key_0" ) );
        //pipeline.push_back( RedisCommand::get( "key_0" ) );
        //rediscli->submit( pipeline );
    }

    rediscli->finalize();
    delete rediscli;
}
