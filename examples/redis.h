
#ifndef __SRC_REDIS_REDIS_H__
#define __SRC_REDIS_REDIS_H__

#include <deque>
#include <vector>
#include <unordered_map>

#include "io.h"
#include "slice.h"

struct redisReply;
struct redisContext;
class RedisDBTask;
class RedisConnection;
class IRedisClient;

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
    void fetchAndPerformTask( RedisConnection * c );

    IRedisClient * getRedisClient() const { return m_Client; }

    sid_t takeIdleConnection();
    RedisConnection * getConnection( sid_t sid ) const;
    void addConnection( sid_t sid, RedisConnection * c );
    void addIdleConnection( sid_t sid, RedisConnection * c );

private :
    typedef std::deque<RedisDBTask *> TaskQueue;
    typedef std::unordered_map<sid_t, RedisConnection *> ConnectionManager;

    IRedisClient *          m_Client;
    uint32_t                m_MaxConnections;
    TaskQueue               m_TaskQueue;
    std::deque<sid_t>       m_IdleSidlist;
    ConnectionManager       m_ConnectionManager;
};

class IRedisClient : public IIOService
{
public :
    // nthreads - 线程数
    // nconnections - 最大连接数
    IRedisClient( uint8_t nthreads, uint32_t nconnections );
    virtual ~IRedisClient();

    virtual void datasets( uint32_t ctxid, redisReply * response ) = 0;

public :
    // 初始化
    // nconnections - 默认初始化连接数
    bool initialize(
            const std::string & host, uint16_t port,
            uint32_t connections = 4, const std::string & token = "" );
    // 销毁
    void finalize();

    // 绑定
    int32_t attach();

    // 订阅
    int32_t subscribe( const Slice & cmd );
    // 提交
    int32_t submit( const Slice & cmd, uint32_t ctxid = 0 );
    // 提交(支持pipeline)
    int32_t submit( const Slices & cmds, uint32_t ctxid = 0 );

private :
    // 初始化/销毁IO上下文
    virtual void * initIOContext();
    virtual void finalIOContext( void * context );

    static int32_t reattach( int32_t fd, void * privdata );
    static int32_t associatecb( void * context,
            void * iocontext, int32_t result, int32_t fd, void * privdata, sid_t sid );

private :
    pthread_cond_t      m_Cond;
    pthread_mutex_t     m_Lock;
    std::string         m_Host;
    uint16_t            m_Port;
    std::string         m_Token;

    uint8_t             m_ThreadCount;
    uint32_t            m_Connections;
};

#endif
