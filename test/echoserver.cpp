
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>

#include "io.h"

#define DEBUG_OUTPUT 0

//
// 回显服务实例
//

class EchoSession : public IIOSession
{
public:
    EchoSession() = default;
    virtual ~EchoSession() override = default;

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

class EchoService : public IIOService
{
public:
    EchoService( uint8_t nthreads, uint32_t nclients )
        : IIOService( nthreads, nclients ) {}
    virtual ~EchoService() override {}

public :
    virtual IIOSession * onAccept( sid_t id, NetType type, uint16_t listenport, const char * host, uint16_t port ) override {
#if DEBUG_OUTPUT
        printf( "%lld, %s::%d .\n", id, host, port );
#endif
        return new EchoSession;
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
    EchoService * service = NULL;

    if ( argc != 4 ) {
        printf( "Usage: echoserver [host] [port] [number] \n" );
        return -1;
    }

    signal( SIGPIPE, SIG_IGN );
    signal( SIGINT, signal_handle );

    service = new EchoService( 4, atoi(argv[3]) );
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
