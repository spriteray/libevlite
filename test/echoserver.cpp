
#include <signal.h>

#include "echoserver.h"

#define DEBUG		0
#define BIG_PACKET	32

int32_t CEchoSession::onProcess( void * context, const char * buf, uint32_t nbytes )
{
	CEchoSession * session = static_cast<CEchoSession*>(context);

	if ( session )
	{
		session->send( buf, nbytes );
#if DEBUG
		if ( nbytes >= BIG_PACKET )
		{
			return -1;
		}
#endif
		return nbytes;
	}

	return -1;
}

int32_t CEchoSession::onTimeout( void * context )
{
	CEchoSession * session = static_cast<CEchoSession*>(context);

	if ( session )
	{
#if DEBUG
		printf("the Session (SID=%ld) timeout .\n", session->getSid() );
#endif
	}

	return -1; 
}

int32_t CEchoSession::onKeepalive( void * context )
{
	// 不需要服务器给客户端发消息
	return 0;
}

int32_t CEchoSession::onError( void * context, int32_t result )
{
	CEchoSession * session = static_cast<CEchoSession*>(context);

	if ( session )
	{
#if DEBUG
		printf("the Session (SID=%ld) error (result=0x%08x) .\n", session->getSid(), result );
#endif
	}

	return -1;
}

int32_t CEchoSession::onShutdown( void * context )
{
	CEchoSession * session = static_cast<CEchoSession*>(context);

	if ( session )
	{
#if DEBUG
		printf("the Session (SID=%ld) shutdown .\n", session->getSid() );
#endif
		delete session;
	}

	return 0;
}

int32_t CEchoSession::shutdown()
{
	return server_shutdown( m_Server, m_Sid );
}

int32_t CEchoSession::send( const char * buf, uint32_t nbytes, bool iscopy /*= true*/ )
{
	return server_send( m_Server, m_Sid, buf, nbytes, static_cast<int32_t>(iscopy) );
}

// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------

CEchoService::CEchoService()
	: m_ThreadsCount(0),
	  m_SessionsCount(0),
	  m_TimeoutSeconds(0),
	  m_BindHost(NULL),
	  m_ListenPort(0),
	  m_CoreServer(NULL)
{
}

CEchoService::~CEchoService()
{
	if ( m_BindHost )
	{
		free( m_BindHost );
		m_BindHost = NULL;
	}
}

int32_t CEchoService::onAcceptSession( void * context, sid_t id, const char * host, uint16_t port )
{
	CEchoService * service = static_cast<CEchoService*>(context);

	CEchoSession * session = new CEchoSession( id, service->getCoreServer() );
	if ( session == NULL )
	{
		return -1;
	}
#if DEBUG
	printf("CEchoService::onAcceptSession(SID=%ld) .\n", id);
#endif
	ioservice_t ioservice;
	ioservice.process	= CEchoSession::onProcess;
	ioservice.timeout	= CEchoSession::onTimeout;
	ioservice.keepalive	= CEchoSession::onKeepalive;
	ioservice.error		= CEchoSession::onError;
	ioservice.shutdown	= CEchoSession::onShutdown;

	server_set_service( service->getCoreServer(), id, &ioservice, session );
	server_set_timeout( service->getCoreServer(), id, service->getTimeoutSeconds() );

	return 0;
}

bool CEchoService::start()
{
	m_CoreServer = server_start(
				const_cast<const char *>(m_BindHost), m_ListenPort,
				m_ThreadsCount, m_SessionsCount,
				onAcceptSession, this );

	if ( m_CoreServer == NULL )
	{
		return false;
	}

	return true;
}

void CEchoService::stop()
{
	server_stop( m_CoreServer );
}

int32_t CEchoService::broadcast( sidlist_t & sidlist, const char * buf, uint32_t nbytes, bool iscopy )
{
	sidlist_t::iterator start = sidlist.begin();

	uint32_t count = (uint32_t)sidlist.size();
	sid_t * _sidlist = static_cast<sid_t *>( &(*start) );

	return server_broadcast( m_CoreServer, _sidlist, count, buf, nbytes, static_cast<int32_t>(iscopy) );
}

// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------

bool g_Running;

void signal_handle( int signo )
{
	g_Running = false;
}

int32_t main( int32_t argc, char ** argv )
{
	signal( SIGPIPE, SIG_IGN );
	signal( SIGINT, signal_handle );

	g_Running = true;

	CEchoService * service = new CEchoService;
	if ( service == NULL )
	{
		return -1;
	}

	service->setThreadsCount(4);
	service->setSessionsCount(200000);
	service->setTimeoutSeconds(60);
	service->setEndPoint( "127.0.0.1", 9029 );

	if ( !service->start() )
	{
		delete service;
		return -2;
	}
	
	while ( g_Running )
	{
		sleep(1);
	}

	printf("EchoServer stoping ...\n");
	service->stop();
	delete service;

	return 0;
}

