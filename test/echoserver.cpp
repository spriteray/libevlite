
#include <signal.h>

#include "echoserver.h"

#define DEBUG_OUTPUT	0

bool g_Running;

int32_t CEchoSession::onProcess( const char * buf, uint32_t nbytes )
{
	send( buf, nbytes );
	return nbytes;
}

int32_t CEchoSession::onTimeout()
{
#if DEBUG_OUTPUT
	printf("the Session (SID=%ld) : timeout \n", id() );
#endif
	return -1;
}

int32_t CEchoSession::onKeepalive()
{
	return 0;
}

int32_t CEchoSession::onError( int32_t result )
{
#if DEBUG_OUTPUT
	printf("the Session (SID=%ld) : error, code=0x%08x \n", id(), result );
#endif
	return -1;
}

int32_t CEchoSession::onShutdown()
{
	delete this;
	return 0;
}

Utils::IIOSession * Utils::IIOSession::create()
{
	return new CEchoSession();
}

Utils::IIOSession * CEchoService::onAccept( sid_t id, const char * host, uint16_t port )
{
	Utils::IIOSession * session = Utils::IIOSession::create();
	if ( session )
	{
		session->setTimeout( 60 );
	}
	return session;
}

Utils::IIOService * Utils::IIOService::create( uint8_t nthreads, uint32_t nclients )
{
	return new CEchoService( nthreads, nclients );
}


void signal_handle( int32_t signo )
{
	g_Running = false;
}

int main()
{
	CEchoService * service = NULL;
	
	signal( SIGPIPE, SIG_IGN );
	signal( SIGINT, signal_handle );

	service = static_cast<CEchoService *>( Utils::IIOService::create(4, 200000) );
	if ( service == NULL )
	{
		return -1;
	}

	service->start();

	if ( !service->listen( "127.0.0.1", 9029 ) )
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

