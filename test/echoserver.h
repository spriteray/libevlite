
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

	IIOService( const char * host, uint16_t port )
		: m_Host( host ),
		  m_Port( port ),
		  m_ThreadsCount( 0 ),
		  m_SessionsCount( 0 ),
		  m_TimeoutSeconds( 0 ),
		  m_Server( NULL )
	{
	}

	virtual ~IIOService()
	{
	}
	
	// 创建服务器
	static IIOService * create( const char * host, uint16_t port );

public :
	
	// 接受事件
	// 需要调用者自己实现
	virtual IIOSession * onAccept( const char * host, uint16_t port ) = 0;
	
	// 设置服务器属性
	void setThreadsCount( uint8_t count );
	void setSessionsCount( uint32_t count );
	void setTimeoutSeconds( int32_t seconds );

	// 开启服务
	bool start();

	// 停止服务
	void stop();

	// 广播数据	
	int32_t broadcast( std::vector<sid_t> & ids, std::string & buffer );
	int32_t broadcast( std::vector<sid_t> & ids, const char * buffer, uint32_t nbytes, bool iscopy = true );	

public :

	void attach( sid_t id, IIOSession * session );
	
private :
	
	// 内部函数
	static int32_t onAcceptSession( void * context, sid_t id, const char * host, uint16_t port );

private :

	std::string	m_Host;
	uint16_t	m_Port;

	uint8_t		m_ThreadsCount;
	uint32_t	m_SessionsCount;
	int32_t		m_TimeoutSeconds;

	server_t	m_Server;
};

//
// 客户端
//

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

