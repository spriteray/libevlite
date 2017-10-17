
#ifndef __SRC_IO_IO_H__
#define __SRC_IO_IO_H__

#include <vector>
#include <string>
#include <pthread.h>

#include "event.h"
#include "network.h"

typedef std::vector<sid_t> SidList;

//
// 会话, 非线程安全的
//

class IIOService;

class IIOSession
{
public :
    IIOSession();
    virtual ~IIOSession();

public :
    //
    // 网络事件
    // 多个网络线程中被触发
    //

    virtual int32_t onStart() { return 0; }
    virtual int32_t onProcess( const char * buffer, uint32_t nbytes ) { return 0; }
    virtual char *  onTransform( const char * buffer, uint32_t & nbytes ) { return const_cast<char *>(buffer); }
    virtual int32_t onTimeout() { return 0; }
    virtual int32_t onKeepalive() { return 0; }
    virtual int32_t onError( int32_t result ) { return 0; }
    virtual void    onShutdown( int32_t way ) {}
    virtual void    onPerform( int32_t type, void * task ) {}

public :
    //
    // 在网络线程中对会话的操作
    //

    // 获取会话ID
    sid_t id() const { return m_Sid; }

    // 获取主机端口
    uint16_t port() const { return m_Port; }
    // 获取主机地址
    const std::string & host() const { return m_Host; }

    // 获取线程上下文参数
    void * iocontext() const { return m_IOContext; }

    // 设置超时/保活时间
    void setTimeout( int32_t seconds );
    void setKeepalive( int32_t seconds );

    // 发送数据
    int32_t send( const std::string & buffer );
    int32_t send( const char * buffer, uint32_t nbytes, bool isfree = false );

    // 关闭会话
    int32_t shutdown();

private :
    friend class IIOService;

    // 初始化会话
    void init( sid_t id,
            void * context, iolayer_t layer,
            const std::string & host, uint16_t port );

    // 内部回调函数
    static int32_t  onStartSession( void * context );
    static int32_t  onProcessSession( void * context, const char * buffer, uint32_t nbytes );
    static char *   onTransformSession( void * context, const char * buffer, uint32_t * nbytes );
    static int32_t  onTimeoutSession( void * context );
    static int32_t  onKeepaliveSession( void * context );
    static int32_t  onErrorSession( void * context, int32_t result );
    static void     onShutdownSession( void * context, int32_t way );
    static void     onPerformSession( void * context, int32_t type, void * task );

private :
    sid_t       m_Sid;
    uint16_t    m_Port;
    std::string m_Host;
    iolayer_t   m_Layer;
    void *      m_IOContext;
};

//
// 网络通信层
//

class IIOService
{
public :
    IIOService( uint8_t nthreads,
            uint32_t nclients, bool immediately = false, bool transform = false );
    virtual ~IIOService();

public :
    // 初始化/销毁IO上下文
    virtual void * initIOContext() { return NULL; }
    virtual void finalIOContext( void * context ) { return; }

    // 数据改造
    virtual char * onTransform( const char * buffer, uint32_t & nbytes ) { return const_cast<char *>(buffer); }

    // 回调事件
    // 需要调用者自己实现
    // 有可能在IIOService的多个网络线程中被触发

    // 连接事件
    virtual bool onConnectFailed( int32_t result, const char * host, uint16_t port ) { return false; }
    virtual IIOSession * onConnectSucceed( sid_t id, const char * host, uint16_t port ) { return NULL; }
    // 接受事件
    virtual IIOSession * onAccept( sid_t id, uint16_t listenport, const char * host, uint16_t port ) { return NULL; }

public :
    //
    // 线程安全的API
    //

    // 获取版本号
    static const char * version();

    // 开启服务
    bool start();

    // 获取连接成功的会话ID
    sid_t id( const char * host, uint16_t port );

    // 连接/监听
    bool listen( const char * host, uint16_t port );
    bool connect( const char * host, uint16_t port, int32_t seconds, bool isblock = false );

    // 停止服务
    void stop();

    // 发送数据
    int32_t send( sid_t id, const std::string & buffer );
    int32_t send( sid_t id, const char * buffer, uint32_t nbytes, bool isfree = false );

    // 广播数据
    int32_t broadcast( const std::string & buffer );
    int32_t broadcast( const std::vector<sid_t> & ids, const std::string & buffer );

    // perform
    int32_t perform( sid_t sid, int32_t type, void * task );
    int32_t perform( void * task, void * (*clone)( void * ), void (*perform)( void *, void * ) );

    // 终止会话
    int32_t shutdown( sid_t id );
    int32_t shutdown( const std::vector<sid_t> & ids );

private :
    struct RemoteHost
    {
        sid_t           sid;
        uint16_t        port;
        std::string     host;

        RemoteHost()
            : sid( 0 ),
              port( 0 )
        {}

        RemoteHost( const char * host, uint16_t port, sid_t sid )
        {
            this->sid = sid;
            this->host = host;
            this->port = port;
        }
    };

    struct ListenContext
    {
        uint16_t        port;
        IIOService *    service;

        ListenContext()
            : port( 0 ),
              service( NULL )
        {}

        ListenContext( uint16_t p, IIOService * s )
            : port( p ),
              service( s )
        {}
    };

    typedef std::vector<RemoteHost> RemoteHosts;
    typedef std::vector<ListenContext *> ListenContexts;

    void attach( sid_t id,
            IIOSession * session, void * iocontext,
            const std::string & host, uint16_t port );

    sid_t getRemoteSid( const char * host, uint16_t port ) const;
    void setRemoteSid( const char * host, uint16_t port, sid_t sid );

    static char * onTransformService( void * context, const char * buffer, uint32_t * nbytes );

    static int32_t onAcceptSession( void * context, void * iocontext, sid_t id, const char * host, uint16_t port );
    static int32_t onConnectSession( void * context, void * iocontext, int32_t result, const char * host, uint16_t port, sid_t id );

private :
    iolayer_t           m_IOLayer;
    bool                m_Transform;
    uint8_t             m_ThreadsCount;
    uint32_t            m_SessionsCount;
    void **             m_IOContextGroup;

private :
    pthread_cond_t      m_Cond;
    pthread_mutex_t     m_Lock;
    RemoteHosts         m_RemoteHosts;
    ListenContexts      m_ListenContexts;
};

#endif
