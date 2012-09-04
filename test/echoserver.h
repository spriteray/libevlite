
#ifndef ECHOSERVER_H
#define ECHOSERVER_H

#include <vector>

#include "network.h"

typedef std::vector<sid_t> sidlist_t;

class CEchoService;
class CEchoSession
{

public :

	CEchoSession(sid_t id, server_t server) 
		: m_Sid(id), 
		  m_Server(server)
	{}

	~CEchoSession()
	{}

public :

	static int32_t onProcess( void * context, const char * buf, uint32_t nbytes );
	static int32_t onTimeout( void * context );
	static int32_t onKeepalive( void * context );
	static int32_t onError( void * context, int32_t result );
	static int32_t onShutdown( void * context );

public :
	
	sid_t getSid() const
	{
		return m_Sid;
	}

	server_t getServer() const
	{
		return m_Server;
	}

	int32_t shutdown();
	int32_t send( const char * buf, uint32_t nbytes, bool iscopy = true );

private :

	sid_t			m_Sid;
	server_t		m_Server;
};

class CEchoService
{

public :

	CEchoService();
	~CEchoService();

public :
	
	// 接收新的会话
	static int32_t onAcceptSession( void * context, sid_t id, 
										const char * host, uint16_t port );

public :

	bool start();
	void stop();

	int32_t broadcast( sidlist_t & sidlist, const char * buf, uint32_t nbytes, bool iscopy = true );

public :

	int32_t getTimeoutSeconds() const
	{
		return m_TimeoutSeconds;
	}

	void setTimeoutSeconds( int32_t seconds )
	{
		m_TimeoutSeconds = seconds;
	}

	void setThreadsCount( uint8_t count )
	{
		m_ThreadsCount = count;
	}

	void setSessionsCount( uint32_t count )
	{
		m_SessionsCount = count;
	}

	void setEndPoint( const char * host, uint16_t port )
	{
		if ( m_BindHost )
		{
			free( m_BindHost );
			m_BindHost = NULL;
		}

		if ( host )
		{
			m_BindHost = ::strdup( host );
		}

		m_ListenPort = port;
	}

	server_t getCoreServer() const
	{
		return m_CoreServer;
	}

private :

	uint8_t		m_ThreadsCount;
	uint32_t	m_SessionsCount;
	int32_t		m_TimeoutSeconds;

	char *		m_BindHost;
	uint16_t	m_ListenPort;

	server_t	m_CoreServer;
};



#endif

