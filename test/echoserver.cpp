
#include <stdio.h>
#include <unistd.h>
#include <signal.h>

#include "io.h"

#define DEBUG_OUTPUT	0


//
// 回显服务实例
//

class CEchoSession : public Utils::IIOSession
{
public :
	CEchoSession()
	{}

	virtual ~CEchoSession()
	{}

public :

	virtual int32_t onStart()
	{
		setTimeout( 60 );
		return 0;
	}

	virtual int32_t onProcess( const char * buf, uint32_t nbytes )
	{
		send( buf, nbytes );
		return nbytes;
	}
	
	virtual int32_t onTimeout()
	{
	#if DEBUG_OUTPUT
		printf("the Session (SID=%ld) : timeout \n", id() );
	#endif
		return -1;
	}
	
	virtual int32_t onError( int32_t result ) 
	{
	#if DEBUG_OUTPUT
		printf("the Session (SID=%ld) : error, code=0x%08x \n", id(), result );
	#endif
		return -1;
	}

	virtual int32_t onShutdown()
	{
		return 0;
	}
};

class CEchoService : public Utils::IIOService
{
public :

	CEchoService( uint8_t nthreads, uint32_t nclients )
		: Utils::IIOService( nthreads, nclients )
	{
	}

	virtual ~CEchoService()
	{
	}

public :

	Utils::IIOSession * onAccept( sid_t id, const char * host, uint16_t port )
	{
		return new CEchoSession;
	}
};

// -------------------------------------------------------------------------------
// -------------------------------------------------------------------------------
// -------------------------------------------------------------------------------

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

	service = new CEchoService( 1, 200000 ); 
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

