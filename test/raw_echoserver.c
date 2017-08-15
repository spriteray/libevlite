
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>

#include "event.h"
#include "utils.h"

int32_t isrunning;

void listenfd_options( int32_t fd )
{
    int32_t flags = 0;

    set_non_block( fd );

    /* TCP Socket Option Settings */
    flags = 1;
    setsockopt( fd, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags) );

    flags = 1;
    setsockopt( fd, SOL_SOCKET, SO_KEEPALIVE, (void *)&flags, sizeof(flags) );

    flags = 1;
    setsockopt( fd, IPPROTO_TCP, TCP_NODELAY, (void *)&flags, sizeof(flags) );
}

void echoserver_signal_handler( int signo )
{
    isrunning = 0;
}

void process_message( int32_t fd, int16_t ev, void * arg )
{
    static int32_t capacity = 16384;

    event_t event = arg;
    evsets_t evsets = event_get_sets( event );

    if ( ev & EV_READ )
    {
        int32_t readn = -1;
        char buf[ capacity ];

        readn = read( fd, buf, capacity );
        if ( readn > 0 )
        {
            write( fd, buf, readn );
        }
        else if ( readn <= 0
                && (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) )
        {
            evsets_del( evsets, event );
            event_destroy( event );
            close( fd );
        }
    }
}

void accept_new_session( int32_t fd, int16_t ev, void * arg )
{
    evsets_t coreset = ( evsets_t )arg;

    if ( ev & EV_READ )
    {
        char dsthost[20];
        uint16_t dstport = 0;

        int32_t newfd = tcp_accept( fd, dsthost, &dstport );
        if ( newfd > 0 )
        {
            set_non_block( newfd );

            event_t event = event_create();
            if ( event == NULL )
            {
                printf( "accept new fd failed .\n" );
                return;
            }

            event_set( event, newfd, EV_READ|EV_PERSIST );
            event_set_callback( event, process_message, event );
            evsets_add( coreset, event, 0 );
        }
    }
}

int main( int argc, char ** argv )
{
    char host[32];
    uint16_t port = 0;

    if ( argc != 3 )
    {
        printf("echoserver [host] [port] .\n");
        return 0;
    }

    signal( SIGPIPE, SIG_IGN );
    signal( SIGINT, echoserver_signal_handler );
    signal( SIGTERM, echoserver_signal_handler );

    // host, listenport
    strcpy( host, argv[1] );
    port = (uint16_t)atoi( argv[2] );

    // core eventsets
    evsets_t coreset = evsets_create();
    if ( coreset == NULL )
    {
        printf( "create core event sets failed .\n" );
        goto FINAL;
    }

    // listen port
    int32_t fd = tcp_listen( host, port, listenfd_options );
    if ( fd < 0 )
    {
        printf( "listen failed %s::%d .\n", host, port );
        goto FINAL;
    }

    event_t evaccept = event_create();
    if ( evaccept == NULL )
    {
        printf( "create accept event failed .\n" );
        goto FINAL;
    }
    event_set( evaccept, fd, EV_READ|EV_PERSIST );
    event_set_callback( evaccept, accept_new_session, coreset );
    evsets_add( coreset, evaccept, 0 );

    // running ...
    isrunning = 1;

    // loop
    while ( isrunning == 1 )
    {
        evsets_dispatch( coreset );
    }

FINAL :
    if ( evaccept != NULL )
    {
        event_destroy( evaccept );
    }

    if ( fd > 0 )
    {
        close( fd );
    }

    if ( coreset != NULL )
    {
        evsets_destroy( coreset );
    }

    return 0;
}
