
#include <vector>

#include "io.h"
#include "slice.h"

struct redisReply;
struct redisContext;

class CRedisSession : public IIOSession
{
public :
    CRedisSession( redisContext * c, int32_t seconds );
    virtual ~CRedisSession();

    virtual int32_t onStart();
    virtual ssize_t onProcess( const char * buffer, size_t nbytes );
    virtual int32_t onTimeout();
    virtual int32_t onKeepalive();
    virtual int32_t onError( int32_t result );
    virtual void    onShutdown( int32_t way );

private :
    void processReply( redisReply * reply );

protected :
    friend class RedisClient;
    int32_t             m_Timeoutseconds;
    redisContext *      m_RedisConnection;
};

class RedisClient
{
public :
    RedisClient( IIOService * s );
    ~RedisClient();

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
    // 验证命令
    static Slice auth( const std::string & password );
    // PING
    static Slice ping();
    // 获取数据
    static Slice get( const std::string & key );
    static Slice mget( const std::vector<std::string> & keys );
    // 更新命令
    static Slice set(
            const std::string & key, const std::string & value );
    // 订阅命令
    static Slice subscribe( const std::string & channel );
    // 取消订阅命令
    static Slice unsubscribe( const std::string & channel );
    // 发布命令
    static Slice publish( const std::string & channel, const std::string & message );

private :
    struct AssociateContext
    {
        int32_t         fd;
        void *          privdata;
        sid_t           sid;
        RedisClient *   client;

        AssociateContext( int32_t fd_, void * data_, RedisClient * c )
            : fd( fd_ ),
              privdata( data_ ),
              sid( 0 ),
              client( c )
        {}

        ~AssociateContext()
        {}
    };
    typedef std::vector<AssociateContext *>     AssociateContexts;

    void notifyAssociateResult( AssociateContext * context, int32_t result, sid_t id, int32_t ack );
    static int32_t reattach( void * context, int32_t fd, void * privdata );
    static int32_t associate( void * context, void * iocontext, int32_t result, int32_t fd, sid_t sid );

private :
    pthread_cond_t      m_Cond;
    pthread_mutex_t     m_Lock;
    IIOService *        m_Service;
    AssociateContexts   m_AssociateList;
};
