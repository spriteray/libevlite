
#ifndef ECHOSERVER_H 
#define ECHOSERVER_H

#include <vector>
#include <string>

#include "network.h"

namespace Utils
{

//
// 服务器
//

class IIOService;
class IIOSession
{
public :

	IIOSession() 
		: m_Sid( 0 ),
		  m_Server( NULL )
	{
	}
	virtual ~IIOSession() {}
	
	// 创建会话
	static IIOSession * create();

public :
	
	//
	virtual int32_t onProcess( const char * buf, uint32_t nbytes ) { return 0; } 
	virtual int32_t onTimeout() { return 0; }
	virtual int32_t onKeepalive() { return 0; }
	virtual int32_t onError( int32_t result ) { return 0; }
	virtual int32_t onShutdown() { return 0; }

public :
	
	// 获取会话ID	
	sid_t getSid() const;
	
	// 关闭会话	
	int32_t shutdown();

	// 发送数据
	int32_t send( std::string & buffer );
	int32_t send( const char * buffer, uint32_t nbytes, bool iscopy = true );

private :

	friend class IIOService;

	// 设置会话ID, 以及服务器句柄
	void setSid( sid_t id );
	void setServer( server_t server );
	
	// 内部回调函数
	static int32_t onProcessSession( void * context, const char * buf, uint32_t nbytes ); 
	static int32_t onTimeoutSession( void * context ); 
	static int32_t onKeepaliveSession( void * context ); 
	static int32_t onErrorSession( void * context, int32_t result ); 
	static int32_t onShutdownSession( void * context ); 

private :

	sid_t		m_Sid;
	server_t	m_Server;
};

class IIOService
{

public :

	IIOService( uint8_t nthreads, uint32_t nclients )
		: m_IOLayer(NULL),
		  m_ThreadsCount( 0 ),
		  m_SessionsCount( 0 ),
		  m_TimeoutSeconds( 0 ),
		  m_KeepaliveSeconds( 0 )
	{
	}

	virtual ~IIOService()
	{
	}

public :

	// 创建服务器
	static IIOService * create( uint8_t nthreads, uint32_t nclients );
	
	// 接受/连接事件
	// 需要调用者自己实现
	
	virtual IIOSession * onAccept( sid_t id, const char * host, uint16_t port ) = 0;
	virtual IIOSession * onConnect( sid_t id, const char * host, uint16_t port ) = 0;

public :
	
	// 开启服务
	bool start();

	// 停止服务
	void stop();
	
	// 设置超时/心跳时间
	void setTimeoutSeconds( int32_t seconds );
	void setKeepaliveSeconds( int32_t seconds );

	// 连接/监听
	bool listen( const char * host, uint16_t port );
	bool connect( const char * host, uint16_t port, int32_t seconds );

	// 发送数据
	int32_t send( sid_t id, std::string & buffer );
	int32_t send( sid_t id, const char * buffer, uint32_t nbytes, bool iscopy = true );

	// 广播数据	
	int32_t broadcast( std::vector<sid_t> & ids, std::string & buffer );
	int32_t broadcast( std::vector<sid_t> & ids, const char * buffer, uint32_t nbytes, bool iscopy = true );	

public :

	void attach( sid_t id, IIOSession * session, bool iskeepalive );
	
private :
	
	// 内部函数
	static int32_t onAcceptSession( void * context, sid_t id, const char * host, uint16_t port );
	static int32_t onConnectSession( void * context, int32_t result, const char * host, uint16_t port, sid_t id );

private :

	iolayer_t	m_IOLayer;

	uint8_t		m_ThreadsCount;
	uint32_t	m_SessionsCount;

	int32_t		m_TimeoutSeconds;
	int32_t		m_KeepaliveSeconds;
};

}

//
// 回显服务实例
//

class CEchoSession : public Utils::IIOSession
{
public :
	CEchoSession()
	{
	}

	virtual ~CEchoSession()
	{
	}

public :

	virtual int32_t onProcess( const char * buf, uint32_t nbytes ); 
	virtual int32_t onTimeout(); 
	virtual int32_t onKeepalive(); 
	virtual int32_t onError( int32_t result ); 
	virtual int32_t onShutdown(); 
};

class CEchoService : public Utils::IIOService
{
public :

	CEchoService( const char * host, uint16_t port )
		: Utils::IIOService( host, port )
	{
	}

	virtual ~CEchoService()
	{
	}

public :

	Utils::IIOSession * onAccept( const char * host, uint16_t port );
};

#endif

