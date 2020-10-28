
#ifndef __SRC_REDIS_REDIS_H__
#define __SRC_REDIS_REDIS_H__

#include <deque>
#include <vector>
#include <unordered_map>

#include "io.h"
#include "slice.h"

class IRedisClient : public IIOService
{
public :
    // nthreads - 线程数
    // nconnections - 最大连接数
    IRedisClient( uint8_t nthreads, uint32_t nconnections );
    virtual ~IRedisClient();

    virtual void datasets( uint32_t ctxid, redisReply * response ) = 0;
    virtual void error( const Slice & request, uint32_t ctxid, const std::string & errstr ) = 0;

public :
    // 初始化
    // nconnections - 默认初始化连接数
    bool initialize(
            const std::string & host, uint16_t port,
            uint32_t connections = 4, const std::string & token = "" );
    // 销毁
    void finalize();

    // 订阅
    int32_t subscribe( const std::string & channel );
    int32_t unsubscribe( const std::string & channel );
    // 提交
    int32_t submit( const Slice & cmd, uint32_t ctxid = 0 );
    // 提交(支持pipeline)
    int32_t submit( const Slices & cmds, uint32_t ctxid = 0 );

public :
    // 绑定
    int32_t attach();

    // 获取token
    const std::string & getToken() const { return m_Token; }

private :
    // 初始化/销毁IO上下文
    virtual void * initIOContext();
    virtual void finalIOContext( void * context );

    static void * cloneTask( void * task );
    static void performTask( void * iocontext, void * task );
    static int32_t reattach( int32_t fd, void * privdata );
    static int32_t associatecb( void * context,
            void * iocontext, int32_t result, int32_t fd, void * privdata, sid_t sid );

private :
    std::string         m_Host;
    uint16_t            m_Port;
    std::string         m_Token;
    uint32_t            m_Connections;
};

#endif
