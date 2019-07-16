
#ifndef __SRC_IO_IO_H__
#define __SRC_IO_IO_H__

#include <vector>
#include <string>
#include <pthread.h>

#include "event.h"
#include "network.h"

typedef std::vector<sid_t> sids_t;

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
    virtual ssize_t onProcess( const char * buffer, size_t nbytes ) { return 0; }
    virtual char *  onTransform( const char * buffer, size_t & nbytes ) { return const_cast<char *>(buffer); }
    virtual int32_t onTimeout() { return 0; }
    virtual int32_t onKeepalive() { return 0; }
    virtual int32_t onError( int32_t result ) { return 0; }
    virtual int32_t onPerform( int32_t type, void * task ) { return 0; }
    virtual void    onShutdown( int32_t way ) {}

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
    void setEndpoint( const std::string & host, uint16_t port );

    // 发送数据
    int32_t send( const std::string & buffer );
    int32_t send( const char * buffer, size_t nbytes, bool isfree = false );

    // 关闭会话
    int32_t shutdown();

protected :
    friend class IIOService;

    // 初始化会话
    void init( sid_t id,
            void * context, iolayer_t layer,
            const std::string & host, uint16_t port );

    // 内部回调函数
    static int32_t  onStartSession( void * context );
    static ssize_t  onProcessSession( void * context, const char * buffer, size_t nbytes );
    static char *   onTransformSession( void * context, const char * buffer, size_t * nbytes );
    static int32_t  onTimeoutSession( void * context );
    static int32_t  onKeepaliveSession( void * context );
    static int32_t  onErrorSession( void * context, int32_t result );
    static int32_t  onPerformSession( void * context, int32_t type, void * task );
    static void     onShutdownSession( void * context, int32_t way );

private :
    sid_t           m_Sid;
    uint16_t        m_Port;
    std::string     m_Host;
    iolayer_t       m_Layer;
    void *          m_IOContext;
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
    virtual char * onTransform( const char * buffer, size_t & nbytes ) { return const_cast<char *>(buffer); }

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

    // 开启/停止服务
    bool start();
    void stop();

    // 暂停对外服务(不可恢复)
    void halt();

    // 获取版本号
    static const char * version();

    // 获取IOLAYER
    iolayer_t iolayer() const { return m_IOLayer; }

    // 监听
    bool listen( const char * host, uint16_t port );

    // 是否正在异步连接
    bool isConnecting( const char * host, uint16_t port );

    // 连接远程服务器
    // 参数:
    //      host    - 主机地址
    //      port    - 主机端口
    //      seconds - 超时时间, <=0 非阻塞的连接; >0 带超时时间的阻塞连接
    //
    // 返回值:
    //      -1      - 连接失败
    //      0       - 正在连接
    //      >0      - 连接成功返回会话ID
    sid_t connect( const char * host, uint16_t port, int32_t seconds = 0 );

    // 关联描述符
    // 参数:
    //      fd      - 关联的描述符
    //      privdata- 关联的私有数据
    //      reattach- 重新绑定新的描述符(相当于重连)
    //      cb      - 关联成功后的回调函数
    //      context - 上下文参数
    //
    // 返回值:
    //      -1      - 关联失败
    //      0       - 正在连接
    int32_t associate( int32_t fd, void * privdata,
            reattacher_t reattach, associator_t cb, void * context );

    // 发送数据
    int32_t send( sid_t id, const std::string & buffer );
    int32_t send( sid_t id, const char * buffer, size_t nbytes, bool isfree = false );

    // 广播数据
    int32_t broadcast( const std::string & buffer );
    int32_t broadcast( const char * buffer, size_t nbytes );
    int32_t broadcast( const sids_t & ids, const std::string & buffer );
    int32_t broadcast( const sids_t & ids, const char * buffer, size_t nbytes );

    // 终止会话
    int32_t shutdown( sid_t id );
    int32_t shutdown( const sids_t & ids );

    // 提交任务到网络层
    int32_t perform( sid_t sid,
            int32_t type, void * task, taskrecycler_t recycle );
    int32_t perform( void * task, taskcloner_t clone, taskexecutor_t perform );

private :
    // 监听上下文
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

    // 连接上下文
    struct ConnectContext
    {
        sid_t           sid;
        uint16_t        port;
        std::string     host;
        IIOService *    service;

        ConnectContext()
            : sid( 0 ),
              port( 0 ),
              service( NULL )
        {}

        ConnectContext( const char * h, uint16_t p, IIOService * s )
            : sid( 0 ),
              port( p ),
              host( h ),
              service( s )
        {}
    };

    typedef std::vector<ListenContext *> ListenContexts;
    typedef std::vector<ConnectContext *> ConnectContexts;

private :
    // 初始化会话
    // 在定制化非常强的场景下使用
    void initSession( sid_t id,
            IIOSession * session, void * iocontext,
            const std::string & host, uint16_t port );

    // 通知连接结果
    void notifyConnectResult(
            ConnectContext * context,
            int32_t result, sid_t id, int32_t ack );

    static char * onTransformService( void * context, const char * buffer, size_t * nbytes );
    static int32_t onAcceptSession( void * context, void * iocontext, sid_t id, const char * host, uint16_t port );
    static int32_t onConnectSession( void * context, void * iocontext, int32_t result, const char * host, uint16_t port, sid_t id );

private :
    iolayer_t           m_IOLayer;
    bool                m_Transform;
    bool                m_Immediately;
    uint8_t             m_ThreadsCount;
    uint32_t            m_SessionsCount;
    void **             m_IOContextGroup;

private :
    pthread_cond_t      m_Cond;
    pthread_mutex_t     m_Lock;
    ListenContexts      m_ListenContexts;       // 正在监听的会话
    ConnectContexts     m_ConnectContexts;      // 正在连接的会话
};

#endif
