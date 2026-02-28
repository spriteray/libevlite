
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>

#include "io.h"

#define DEBUG_OUTPUT 0

//
// 回显服务实例
//

class CEchoSession : public IIOSession
{
public:
    CEchoSession() = default;
    virtual ~CEchoSession() override = default;

public:
    virtual int32_t onStart() override {
#if DEBUG_OUTPUT
        printf( "the Session (SID=%ld) Start (%s::%d)  \n", id(), host().c_str(), port() );
#endif
        //setTimeout( 60 );
        return 0;
    }

    virtual ssize_t onProcess( const char * buf, size_t nbytes ) override {
        send( buf, nbytes );
        return nbytes;
    }

    virtual int32_t onTimeout() override {
#if DEBUG_OUTPUT
        printf( "the Session (SID=%ld) : timeout \n", id() );
#endif
        return -1;
    }

    virtual int32_t onError( int32_t result ) override {
#if DEBUG_OUTPUT
        printf( "the Session (SID=%ld) : error, code=0x%08x \n", id(), result );
#endif
        return -1;
    }

    virtual void onShutdown( int32_t way ) override {}
};

class CEchoService : public IIOService
{
public:
    CEchoService( uint8_t nthreads, uint32_t nclients )
        : IIOService( nthreads, nclients )
    {}

    virtual ~CEchoService() override
    {}

public:
    IIOSession * onAccept( sid_t id, NetType type, uint16_t listenport, const char * host, uint16_t port ) override {
#if DEBUG_OUTPUT
        printf( "%lld, %s::%d .\n", id, host, port );
#endif
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

    if ( argc != 3 ) {
        printf( "Usage: echoserver [host] [port] \n" );
        return -1;
    }

    signal( SIGPIPE, SIG_IGN );
    signal( SIGINT, signal_handle );

    service = new CEchoService( 4, 200000 );
    if ( service == NULL ) {
        return -1;
    }

    service->start();

    if ( !service->listen( NetType::TCP, argv[1], atoi( argv[2] ), nullptr ) ) {
        printf( "service start failed \n" );
        delete service;

        return -2;
    }

    g_Running = true;

    while ( g_Running ) {
        sleep( 1 );
    }

    printf( "EchoServer stoping ...\n" );
    service->stop();
    delete service;

    return 0;
}
