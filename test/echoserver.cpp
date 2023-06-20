
#include <stdio.h>
#include <unistd.h>
#include <signal.h>

#include "io.h"

#define DEBUG_OUTPUT    1


//
// 回显服务实例
//

class CEchoSession : public IIOSession
{
public :
    CEchoSession()
    {}

    virtual ~CEchoSession()
    {}

public :

    virtual int32_t onStart()
    {
    #if DEBUG_OUTPUT
        printf("the Session (SID=%ld) Start (%s::%d)  \n", id(), host().c_str(), port() );
    #endif
//        setTimeout( 60 );
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

class CEchoService : public IIOService
{
public :

    CEchoService( uint8_t nthreads, uint32_t nclients )
        : IIOService( nthreads, nclients )
    {
    }

    virtual ~CEchoService()
    {
    }

public :

    IIOSession * onAccept( sid_t id, uint16_t listenport, const char * host, uint16_t port )
    {
        printf( "%lld, %s::%d .\n", id, host, port );
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

int main( int argc, char ** argv )
{
    CEchoService * service = NULL;

    if ( argc != 3 )
    {
        printf("Usage: echoserver [host] [port] \n");
        return -1;
    }

    signal( SIGPIPE, SIG_IGN );
    signal( SIGINT, signal_handle );

    service = new CEchoService( 4, 200000 );
    if ( service == NULL )
    {
        return -1;
    }

    service->start();

    if ( !service->listen( NetType::TCP, argv[1], atoi(argv[2]), nullptr ) )
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
