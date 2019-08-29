
#include <vector>

#include "io.h"
#include "slice.h"

struct redisReply;
struct redisContext;

class IRedisSession : public IIOSession
{
public :
    IRedisSession( redisContext * c, int32_t seconds );
    virtual ~IRedisSession();

    virtual int32_t onStart();
    virtual ssize_t onProcess( const char * buffer, size_t nbytes );
    virtual int32_t onTimeout();
    virtual int32_t onKeepalive();
    virtual int32_t onError( int32_t result );
    virtual void    onShutdown( int32_t way );

private :
    void processReply( redisReply * reply );

protected :
    friend class IRedisClient;
    int32_t             m_Timeoutseconds;
    redisContext *      m_RedisConnection;
};

class IRedisClient
{
public :
    IRedisClient( IIOService * s );
    virtual ~IRedisClient();

    virtual bool onConnectFailed( int32_t result, int32_t fd, redisContext * conn ) { return false; }
    virtual IRedisSession * onConnectSucceed( sid_t sid, int32_t fd, redisContext * conn ) { return NULL; }

public :
    // 服务
    IIOService * service() const { return m_Service; }

    // 连接
    sid_t connect(
            const std::string & host,
            uint16_t port, int32_t seconds = 0 );

    // 发送
    int32_t send( sid_t id, const Slice & cmd );

public :
    // PING
    static Slice ping();
    // ECHO
    static Slice echo( const std::string & text );
    // 切换数据库
    static Slice select( int32_t index );
    // 验证命令
    static Slice auth( const std::string & password );
    // 获取数据
    static Slice get( const std::string & key );
    static Slice mget( const std::vector<std::string> & keys );
    // 更新命令
    static Slice set(
            const std::string & key, const std::string & value );
    // 自增
    static Slice incr( const std::string & key );
    static Slice incrby( const std::string & key, int32_t value );
    // 自减
    static Slice decr( const std::string & key );
    static Slice decrby( const std::string & key, int32_t value );
    // list
    static Slice lpush( const std::string & key, const std::vector<std::string> & values );
    static Slice rpush( const std::string & key, const std::vector<std::string> & values );
    static Slice lrange( const std::string & key, int32_t startidx, int32_t stopidx );
    // 订阅命令
    static Slice subscribe( const std::string & channel );
    // 取消订阅命令
    static Slice unsubscribe( const std::string & channel );
    // 发布命令
    static Slice publish( const std::string & channel, const std::string & message );
    // 事务开始
    static Slice multi();
    // pipeline
    static Slice pipeline();
    // 提交事务
    static Slice exec();

private :
    struct AssociateContext
    {
        int32_t         fd;
        void *          privdata;
        sid_t           sid;
        IRedisClient *  client;

        AssociateContext( int32_t fd_, void * data_, IRedisClient * c )
            : fd( fd_ ),
              privdata( data_ ),
              sid( 0 ),
              client( c )
        {}

        ~AssociateContext()
        {}
    };
    typedef std::vector<AssociateContext *>     AssociateContexts;

    sid_t waitForAssociate( AssociateContext * context, struct timeval & now, int32_t seconds );
    void wakeupFromAssociating( AssociateContext * context, int32_t result, sid_t id, int32_t ack );

    static int32_t reattach( int32_t fd, void * privdata );
    static int32_t associate( void * context, void * iocontext, int32_t result, int32_t fd, void * privdata, sid_t sid );

private :
    pthread_cond_t      m_Cond;
    pthread_mutex_t     m_Lock;
    IIOService *        m_Service;
    AssociateContexts   m_AssociateList;
};
