
#include <signal.h>

#include "echoserver.h"


namespace Utils
{

sid_t IIOSession::id() const
{
	return m_Sid;
}

void IIOSession::setSid( sid_t id )
{
	m_Sid = id;
}

void IIOSession::setLayer( iolayer_t layer )
{
	m_Layer = layer;
}

int32_t IIOSession::shutdown()
{
	return iolayer_shutdown( m_Layer, m_Sid );
}

int32_t IIOSession::send( std::string & buffer )
{
	return send( buffer.c_str(), static_cast<uint32_t>(buffer.length()) );
}

int32_t IIOSession::send( const char * buffer, uint32_t nbytes, bool iscopy )
{
	return iolayer_send( m_Layer, m_Sid, buffer, nbytes, static_cast<int32_t>(iscopy) );
}

int32_t IIOSession::onProcessSession( void * context, const char * buf, uint32_t nbytes ) 
{
	IIOSession * session = static_cast<IIOSession *>( context );

	if ( session )
	{
		return session->onProcess( buf, nbytes );
	}

	return -1;
}

int32_t IIOSession::onTimeoutSession( void * context ) 
{
	IIOSession * session = static_cast<IIOSession *>( context );

	if ( session )
	{
		return session->onTimeout();
	}

	return -1;
}

int32_t IIOSession::onKeepaliveSession( void * context )
{
	IIOSession * session = static_cast<IIOSession *>( context );

	if ( session )
	{
		return session->onKeepalive();
	}

	return -1;
} 

int32_t IIOSession::onErrorSession( void * context, int32_t result ) 
{
	IIOSession * session = static_cast<IIOSession *>( context );

	if ( session )
	{
		return session->onError( result );
	}

	return -1;
}

int32_t IIOSession::onShutdownSession( void * context ) 
{
	IIOSession * session = static_cast<IIOSession *>( context );

	if ( session )
	{
		return session->onShutdown();
	}

	return -1;
}

// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------

bool IIOService::start()
{
	m_IOLayer = iolayer_create( m_ThreadsCount, m_SessionsCount );
	return ( m_IOLayer != NULL );
}

void IIOService::stop()
{
	return iolayer_destroy();
}

void IIOService::setTimeoutSeconds( int32_t seconds )
{
	m_TimeoutSeconds = seconds;
}

void IIOService::setKeepaliveSeconds( uint32_t seconds )
{
	m_KeepaliveSeconds = seconds;
}

bool IIOService::listen( const char * host, uint16_t port )
{

}

bool IIOService::connect( const char * host, uint16_t port, int32_t seconds )
{
	iolayer_connect( m_IOLayer, host, port,  );
}

int32_t IIOService::broadcast( std::vector<sid_t> & ids, std::string & buffer )
{
	return broadcast( ids, buffer.c_str(), static_cast<uint32_t>(buffer.length()) );
}

int32_t IIOService::broadcast( std::vector<sid_t> & ids, const char * buffer, uint32_t nbytes, bool iscopy )
{
	std::vector<sid_t>::iterator start = ids.begin();	

	uint32_t count = (uint32_t)ids.size();
	sid_t * idlist = static_cast<sid_t *>( &(*start) );

	return iolayer_broadcast( m_IOLayer, idlist, count, buffer, nbytes, static_cast<int32_t>(iscopy) );
}

void IIOService::attach( sid_t id, IIOSession * session, bool iskeepalive )
{
	ioservice_t ioservice;
	ioservice.process	= IIOSession::onProcessSession;
	ioservice.timeout	= IIOSession::onTimeoutSession;
	ioservice.keepalive	= IIOSession::onKeepaliveSession;
	ioservice.error		= IIOSession::onErrorSession;
	ioservice.shutdown	= IIOSession::onShutdownSession;
	
	// 初始化会话
	session->setSid( id );
	session->setServer( m_Server );

	server_set_service( m_Server, id, &ioservice, session );
	server_set_timeout( m_Server, id, m_TimeoutSeconds );
	if ( iskeepalive )
	{
		iolayer_set_keepalive( m_IOLayer, id, m_KeepaliveSeconds );
	}
}

int32_t IIOService::onAcceptSession( void * context, sid_t id, const char * host, uint16_t port )
{
	IIOSession * session = NULL;
	IIOService * service = static_cast<IIOService*>( context );
	
	session = service->onAccept( id, host, port );
	if ( session )
	{
		service->attach( id, session, false );
		return 0;
	}

	return -1;
}

int32_t IIOService::onConnectSession( void * context, int32_t result, const char * host, uint16_t port, sid_t id )
{
	if ( result != 0 )
	{
		return 0;
	}

	IIOSession * session = NULL;
	IIOService * service = static_cast<IIOService *>( context );

	session = service->onConnect( id, host, port );
	if ( session )
	{
		service->attach( id, session, true );
		return 0;
	}

	return -1;
}

}

int32_t CEchoSession::onProcess( const char * buf, uint32_t nbytes )
{
	send( buf, nbytes );
	return nbytes;
}

int32_t CEchoSession::onTimeout()
{
	printf("the Session (SID=%ld) : timeout \n", getSid() );
	return -1;
}

int32_t CEchoSession::onKeepalive()
{
	return 0;
}

int32_t CEchoSession::onError( int32_t result )
{
	printf("the Session (SID=%ld) : error, code=0x%08x \n", getSid(), result );
	return -1;
}

int32_t CEchoSession::onShutdown()
{
	delete this;
	return 0;
}

Utils::IIOSession * Utils::IIOSession::create()
{
	return new CEchoSession;
}

Utils::IIOSession * CEchoService::onAccept( const char * host, uint16_t port )
{
	return Utils::IIOSession::create();
}

Utils::IIOService * Utils::IIOService::create( const char * host, uint16_t port )
{
	return new CEchoService( host, port );
}

bool g_Running;

void signal_handle( int32_t signo )
{
	g_Running = false;
}

int main()
{
	CEchoService * service = NULL;
	
	signal( SIGPIPE, SIG_IGN );
	signal( SIGINT, signal_handle );

	service = static_cast<CEchoService *>( Utils::IIOService::create( "127.0.0.1", 9029 ) );
	if ( service == NULL )
	{
		return -1;
	}
	
	service->setThreadsCount( 4 );
	service->setSessionsCount( 200000 );
	service->setTimeoutSeconds( 30 );

	if ( !service->start() )
	{
		printf("service start failed \n");
		delete service;

		return -2;
	}

	g_Running = true;	

	while ( g_Running )
	{
		sleep(1);
	}

	printf("EchoServer stoping ...\n");
	service->stop();
	delete service;

	return 0;

}

