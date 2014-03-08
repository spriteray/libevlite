
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
    struct linger ling;

    /* TCP Socket Option Settings */
    flags = 1;
    setsockopt( fd, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags) );

    flags = 1;
    setsockopt( fd, SOL_SOCKET, SO_KEEPALIVE, (void *)&flags, sizeof(flags) );

    flags = 1;
    setsockopt( fd, IPPROTO_TCP, TCP_NODELAY, (void *)&flags, sizeof(flags) );
    setsockopt( fd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling) );

#if defined (__linux__)
	flags = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flags, sizeof(flags) );
#endif

    set_non_block( fd );
}

void echoserver_signal_handler( int signo )
{
    isrunning = 0;
}

void process_message( int32_t fd, int16_t ev, void * arg )
{
    event_t event = arg;
    static int32_t capacity = 16384 * 2;

    if ( ev & EV_READ )
    {
        char buf[ capacity ];
        int32_t readn = -1;

        readn = read( fd, buf, capacity );
        if ( readn <= 0 )
        {
            event_destroy( event );
            close( fd );
        }
        else
        {
            write( fd, buf, readn );
//            evsets_add( event_get_sets(event), event, 0 );
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
    if ( argc != 2 )
    {
        printf("echoserver [port] .\n");
        return 0;
    }

    evsets_t coreset = evsets_create();
    if ( coreset == NULL )
    {
        printf( "create core event sets failed .\n" );
        goto FINAL;
    }

    signal( SIGPIPE, SIG_IGN );
    signal( SIGINT, echoserver_signal_handler );
    signal( SIGTERM, echoserver_signal_handler );

    // listen port
    uint16_t port = (uint16_t)atoi( argv[1] );
    int32_t fd = tcp_listen( "127.0.0.1", port, listenfd_options );
    if ( fd < 0 )
    {
        printf( "listen failed %d .\n", port );
        goto FINAL;
    }
    set_non_block( fd );
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
