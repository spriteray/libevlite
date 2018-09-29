
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>

#include "io.h"

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

    virtual ssize_t onProcess( const char * buf, size_t nbytes )
    {
        std::string line( buf, buf+nbytes );
        printf("%s", line.c_str() );
        fflush(stdout);

        return nbytes;
    }

    virtual int32_t onError( int32_t result )
    {
        printf("the Session (SID=%ld) : error, code=0x%08x \n", id(), result );
        return 0;
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

    Utils::IIOSession * onConnect( sid_t id, const char * host, uint16_t port )
    {
        m_ClientSid = id;
        return new CEchoSession();
    }

public :

    int32_t send2( const std::string & buffer )
    {
        return send( m_ClientSid, buffer );
    }

private :

    sid_t    m_ClientSid;
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
    std::string line;
    CEchoService * service = NULL;

    if ( argc != 3 )
    {
        printf("Usage: echoclient [host] [port] \n");
        return -1;
    }

    signal( SIGPIPE, SIG_IGN );
    signal( SIGINT, signal_handle );

    service = new CEchoService( 1, 200 );
    if ( service == NULL )
    {
        return -1;
    }

    service->start();

    if ( !service->connect( argv[1], atoi(argv[2]), 10 ) )
    {
        printf("service start failed \n");
        delete service;

        return -2;
    }

    g_Running = true;

    while ( g_Running )
    {
        int ch = getc(stdin);
        line.push_back( (char)ch );

        if ( ch == '\n' )
        {
            if ( strcmp( line.c_str(), "quit\n" ) == 0 )
            {
                g_Running = false;
                continue;
            }

            service->send2( line );
            line.clear();
        }
        fflush(stdin);
    }

    printf("EchoClient stoping ...\n");
    service->stop();
    delete service;

    return 0;
}
