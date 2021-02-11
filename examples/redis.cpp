
#include <cassert>
#include <algorithm>

#include <errno.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <hiredis/hiredis.h>

#include "helper.h"
#include "redis.h"

struct redisContext;
class RedisDBTask;
class RedisConnection;

class RedisConnectionPool
{
public :
    RedisConnectionPool( IRedisClient * c, uint32_t maxconnections )
        : m_Client( c ),
          m_MaxConnections( maxconnections )
    {}

    ~RedisConnectionPool()
    {}

public :
    int32_t perform( RedisDBTask * t );
    int32_t perform( const std::string & channel, RedisDBTask * t );
    void fetchAndPerformTask( RedisConnection * c );

    IRedisClient * getRedisClient() const { return m_Client; }

    sid_t takeIdleConnection();
    RedisConnection * getConnection( sid_t sid ) const;
    void removeConnection( sid_t sid );
    void addConnection( sid_t sid, RedisConnection * c );
    void addIdleConnection( sid_t sid, RedisConnection * c );

    void addChannel( const std::string & channel, sid_t sid );
    void removeChannel( const std::string & channel );

private :
    typedef std::deque<RedisDBTask *> TaskQueue;
    typedef std::unordered_map<std::string, sid_t> ChannelManager;
    typedef std::unordered_map<sid_t, RedisConnection *> ConnectionManager;

    IRedisClient *          m_Client;
    uint32_t                m_MaxConnections;
    TaskQueue               m_TaskQueue;
    std::deque<sid_t>       m_IdleSidlist;
    ChannelManager          m_ChannelManager;
    ConnectionManager       m_ConnectionManager;
};

class RedisDBTask
{
public :
    RedisDBTask( int32_t count, const Slice & cmd, uint32_t ctxid )
        : m_Counter( count ),
          m_ContextID( ctxid ),
          m_RedisCommand( cmd )
    {}

    virtual ~RedisDBTask()
    {
        if ( !m_RedisCommand.empty() )
        {
            std::free( (void *)m_RedisCommand.data() );
            m_RedisCommand.clear();
        }
    }

    virtual void * clone()
    {
        return new RedisDBTask( m_Counter, m_RedisCommand, m_ContextID );
    }

    virtual void perform( RedisConnectionPool * pool )
    {
        pool->perform( this );
    }

public :
    uint32_t decrCounter()
    {
        if ( m_Counter == (uint32_t)-1 )
        {
            return -1;
        }

        return --m_Counter;
    }

    void setCounter( uint32_t c ) { m_Counter = c; }
    uint32_t getCounter() const { return m_Counter; }
    uint32_t getContextID() const { return m_ContextID; }
    const Slice & getCommand() const { return m_RedisCommand; }

protected :
    uint32_t    m_Counter;        // 任务计数器
    uint32_t    m_ContextID;
    Slice       m_RedisCommand;
};

class UnsubscribeTask : public RedisDBTask
{
public :
    UnsubscribeTask( const std::string & channel, const Slice & cmd )
        : RedisDBTask( 1, cmd, 0 ),
          m_Channel( channel )
    {}

    virtual ~UnsubscribeTask()
    {}

    virtual void * clone()
    {
        char * buf = (char *)malloc( m_RedisCommand.size() );
        if ( buf == NULL )
        {
            return NULL;
        }

        memcpy( buf, m_RedisCommand.data(), m_RedisCommand.size() );
        return new UnsubscribeTask( m_Channel, Slice(buf, m_RedisCommand.size()) );
    }

    virtual void perform( RedisConnectionPool * pool )
    {
        pool->perform( m_Channel, this );
    }

protected :
    std::string m_Channel;
};

class RedisConnection : public IIOSession
{
public :
    RedisConnection( redisContext * c, const std::string & token )
        : m_Task( NULL ),
          m_Token( token ),
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
        RedisConnectionPool * pool = getConnectionPool();

        // 添加到连接池
        pool->addConnection( id(), this );

        // 验证
        if ( !m_Token.empty() )
        {
            Slice cmd = RedisCommand::auth(m_Token);
            if ( !cmd.empty() )
            {
                m_Task = new RedisDBTask( 1, cmd, 0 );
                send( cmd.data(), cmd.size(), false );
            }
        }
        else
        {
            pool->addIdleConnection( id(), this );
        }

        return 0;
    }

    virtual ssize_t onProcess( const char * buffer, size_t nbytes )
    {
        void * data = NULL;
        RedisConnectionPool * pool = getConnectionPool();
        IRedisClient * redisclient = pool->getRedisClient();

        if ( 0 != redisReaderFeed(
                    m_RedisConnection->reader, buffer, nbytes ) )
        {
            return -1;
        }

        while ( REDIS_OK
                == redisReaderGetReply( m_RedisConnection->reader, &data ) )
        {
            if ( data == NULL )
            {
                break;
            }

            if ( m_Task->getContextID() == 0 )
            {
                int32_t rc = general_processor(
                        redisclient,
                        static_cast<redisReply *>(data) );
                if ( rc == -1 )
                {
                    return -1;
                }
            }
            else
            {
                redisclient->datasets(
                        m_Task->getContextID(), static_cast<redisReply *>(data) );
            }

            if ( m_Task->decrCounter() == 0 )
            {
                delete m_Task; m_Task = NULL;
                pool->addIdleConnection( id(), this );
            }
        }

        return nbytes;
    }

    virtual int32_t onError( int32_t result )
    {
        IRedisClient * redisclient = getConnectionPool()->getRedisClient();
        if ( m_Task != NULL )
        {
            char errstring[ 1024 ];
            snprintf( errstring, 1023,
                    "Connection(Sid:%lu) is Error : 0x%08x", id(), result );
            redisclient->error(
                    m_Task->getCommand(), m_Task->getContextID(), errstring );
            delete m_Task; m_Task = NULL;
        }

        return 0;
    }

    virtual void    onShutdown( int32_t way )
    {
        RedisConnectionPool * pool = getConnectionPool();
        IRedisClient * redisclient = pool->getRedisClient();

        if ( m_Task != NULL )
        {
            char errstring[ 1024 ];
            snprintf( errstring, 1023,
                    "Connection(Sid:%lu) is Closed : %d", id(), way );
            redisclient->error(
                    m_Task->getCommand(), m_Task->getContextID(), errstring );
            delete m_Task; m_Task = NULL;
        }

        // 回收连接
        pool->removeConnection( id() );
    }

public :
    //
    void setTask( RedisDBTask * task ) { m_Task = task; }

    // 通用处理器
    // 屏蔽错误, 状态, 空数据等等
    int32_t general_processor( IRedisClient * redisclient, redisReply * reply )
    {
        switch ( reply->type )
        {
            case REDIS_REPLY_ERROR :
                redisclient->error(
                        m_Task->getCommand(), 0,
                        std::string(reply->str, reply->len) );
                freeReplyObject( reply );
                break;

            case REDIS_REPLY_NIL :
            case REDIS_REPLY_STATUS :
                freeReplyObject( reply );
                break;

            case REDIS_REPLY_ARRAY :
                {
                    switch ( isAboutSubscribe(reply) )
                    {
                        case 0 :
                            redisclient->datasets( 0, reply );
                            break;

                        case 1 :
                            getConnectionPool()->addChannel(
                                    std::string(reply->element[1]->str, reply->element[1]->len), id() );
                            freeReplyObject( reply );
                            break;

                        case 2 :
                            m_Task->setCounter( 1 );
                            getConnectionPool()->removeChannel(
                                    std::string(reply->element[1]->str, reply->element[1]->len) );
                            freeReplyObject( reply );
                            break;
                    }
                }
                break;

            default :
                redisclient->datasets( 0, reply );
                break;
        }

        return 0;
    }

private :
    RedisConnectionPool * getConnectionPool()
    {
        return static_cast<RedisConnectionPool *>(iocontext());
    }

    int32_t isAboutSubscribe( redisReply * reply ) const
    {
        if ( reply->elements == 3
            && reply->element[0]->type == REDIS_REPLY_STRING )
        {
            if ( strncmp( reply->element[0]->str, "subscribe", reply->element[0]->len ) == 0 )
            {
                return 1;
            }
            else if ( strncmp( reply->element[0]->str, "unsubscribe", reply->element[0]->len ) == 0 )
            {
                return 2;
            }
        }

        return 0;
    }

protected :
    friend class IRedisClient;
    RedisDBTask *       m_Task;
    std::string         m_Token;
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

int32_t RedisConnectionPool::perform( const std::string & channel, RedisDBTask * task )
{
    ChannelManager::iterator result = m_ChannelManager.find( channel );
    if ( result != m_ChannelManager.end() )
    {
        RedisConnection * connection = getConnection( result->second );
        if ( connection != NULL )
        {
            connection->send( task->getCommand().data(), task->getCommand().size(), false );
        }
    }

    delete task; return 0;
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
}

void RedisConnectionPool::removeConnection( sid_t sid )
{
    m_ConnectionManager.erase( sid );

    std::deque<sid_t>::iterator result;
    result = std::find( m_IdleSidlist.begin(), m_IdleSidlist.end(), sid );
    if ( result != m_IdleSidlist.end() )
    {
        m_IdleSidlist.erase( result );
    }
}

void RedisConnectionPool::addChannel( const std::string & channel, sid_t sid )
{
    m_ChannelManager.insert( std::make_pair( channel, sid ) );
}

void RedisConnectionPool::removeChannel( const std::string & channel )
{
    m_ChannelManager.erase( channel );
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

IRedisClient::IRedisClient( uint8_t nthreads, uint32_t nconnections )
    : IIOService( nthreads, nconnections, true, false ),
      m_Port( 0 ),
      m_Connections( nconnections/nthreads )
{}

IRedisClient::~IRedisClient()
{}

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

int32_t IRedisClient::subscribe( const std::string & channel )
{
    Slice cmd = RedisCommand::subscribe( channel );
    if ( cmd.empty() )
    {
        return -1;
    }

    return invoke( new RedisDBTask(-1, cmd, 0), NULL, performTask );
}

int32_t IRedisClient::unsubscribe( const std::string & channel )
{
    Slice cmd = RedisCommand::unsubscribe( channel );
    if ( cmd.empty() )
    {
        return -1;
    }

    return invoke( new UnsubscribeTask(channel, cmd), cloneTask, performTask );
}

int32_t IRedisClient::submit( const Slice & cmd, uint32_t ctxid )
{
    if ( cmd.empty() )
    {
        return -1;
    }

    return invoke( new RedisDBTask(1, cmd, ctxid), NULL, performTask );
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

    return invoke( new RedisDBTask(count, Slice(buffer, len), ctxid), NULL, performTask );
}

void * IRedisClient::initIOContext()
{
    return new RedisConnectionPool( this, m_Connections );
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

void * IRedisClient::cloneTask( void * task )
{
    return static_cast<RedisDBTask *>(task)->clone();
}

void IRedisClient::performTask( void * iocontext, void * task )
{
    static_cast<RedisDBTask *>(task)->perform( static_cast<RedisConnectionPool *>(iocontext) );
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

    RedisConnection * connection = new RedisConnection( redisconn, client->getToken() );
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
        debugReply( response );
        freeReplyObject( response );
    }

    virtual void error( const Slice & request, uint32_t ctxid, const std::string & errstr )
    {
        printf( "[ERROR] : %s\n", errstr.c_str() );
    }

public :
    void debugReply( redisReply * reply )
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
};

int main()
{
    CRedisClient * rediscli = new CRedisClient();
    if ( rediscli == NULL )
    {
        return -2;
    }

    srand( time(NULL) );

    // 初始化
    rediscli->initialize(
            "172.21.161.70", 6379, 4, "123456" );

    // 订阅
    rediscli->subscribe( "ope" );
    rediscli->unsubscribe( "ope" );

    while( 1 )
    {
        sleep( 2 );

        //std::vector<std::string> cmds;
        //cmds.push_back( "account" );
        //cmds.push_back( "account1" );
        //rediscli->submit( RedisCommand::mget( cmds ) );

        //rediscli->submit( RedisCommand::set( "roleid1", "17648020619275" ) );

        //Slices pipeline;
        //pipeline.push_back( RedisCommand::echo( "key_0" ) );
        //pipeline.push_back( RedisCommand::get( "key_0" ) );
        //rediscli->submit( pipeline );
        rediscli->unsubscribe( "ope" );
    }

    rediscli->finalize();
    delete rediscli;
}
