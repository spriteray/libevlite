
#include "io.h"
#include "slice.h"

struct redisContext;

class CRedisSession : public IIOSession
{
public :
    CRedisSession( redisContext * c );
    virtual ~CRedisSession();

    virtual int32_t onStart();
    virtual ssize_t onProcess( const char * buffer, size_t nbytes );
    virtual int32_t onTimeout();
    virtual int32_t onKeepalive();
    virtual int32_t onError( int32_t result );
    virtual void    onShutdown( int32_t way );

protected :
    redisContext *      m_RedisConnection;
};

class CRedisService : public IIOService
{
public :
    CRedisService();
    virtual ~CRedisService();

    virtual IIOSession * onAssociate( sid_t id, int32_t type, int32_t fd, void * privdata );
};

class RedisClient
{
public :
    RedisClient( IIOService * s );
    ~RedisClient();

public :
    // 链接
    sid_t connect(
            const std::string & host, uint16_t port );

    // 发送
    int32_t send( sid_t id, const Slice & cmd );

public :
    // 验证命令
    static Slice auth( const std::string & password );
    // PING
    static Slice ping();
    // 获取数据
    static Slice get( const std::string & key );
    // 更新命令
    static Slice set(
            const std::string & key, const std::string & value );
    // 订阅命令
    // 发布命令

private :
    IIOService *        m_Service;
};
