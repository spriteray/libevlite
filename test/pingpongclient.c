#include <sys/socket.h>
#include <sys/errno.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <stdint.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <memory.h>

#include "ikcp.h"
#include "event.h"
#include "utils.h"
#include "message.h"

int32_t g_IsInput;
int32_t g_IsRunning;
#define INBUFFER_LEN        65536

typedef struct
{
    int32_t fd;
    int32_t conv;
    uint8_t type;
    ikcpcb * kcp;
    event_t event;
    evsets_t evsets;
    struct sockaddr_in addr;
}PingpongClient;

#if ( 1>>1 == 0 )
    #define bswap64(_x)                     \
        (((_x) >> 56) |                     \
         (((_x) >> 40) & (0xffUL << 8)) |   \
         (((_x) >> 24) & (0xffUL << 16)) |  \
         (((_x) >> 8) & (0xffUL << 24)) |   \
         (((_x) << 8) & (0xffUL << 32)) |   \
         (((_x) << 24) & (0xffUL << 40)) |  \
         (((_x) << 40) & (0xffUL << 48)) |  \
         ((_x) << 56))
    #ifndef htobe64
    #define htobe64(x)    bswap64((uint64_t)(x))
    #endif
    #ifndef be64toh
    #define be64toh(x)    bswap64((uint64_t)(x))
    #endif
#else
    #ifndef htobe64
    #define htobe64(x)    ((uint64_t)(x))
    #endif
    #ifndef be64toh
    #define be64toh(x)    ((uint64_t)(x))
    #endif
#endif

void signal_handler( int signo )
{
    g_IsRunning = 0;
}

int32_t kcp_output( const char * buf, int32_t len, ikcpcb * kcp, void * user )
{
    PingpongClient * cli = (PingpongClient *)user;

    if ( write(cli->fd, buf, len) < 0 )
    {
        printf( "sendto(%s) error.\n", strerror(errno) );
    }

    return 0;
}

PingpongClient * pingpongclient_init( uint8_t type )
{
    PingpongClient * client = (PingpongClient *)malloc( sizeof(PingpongClient) );
    if ( client == NULL )
    {
        return NULL;
    }

    client->type = type;
    client->conv = rand() % 0x7fffffff;
    client->kcp = ikcp_create( client->conv, client );
    if ( client->kcp == NULL )
    {
        return NULL;
    }

    client->evsets = evsets_create();
    if ( client->evsets == NULL )
    {
        return NULL;
    }

    client->event = event_create();
    if ( client->event == NULL )
    {
        return NULL;
    }

    client->kcp->stream = 1;
    ikcp_nodelay( client->kcp, 1, 10, 2, 1 );
    ikcp_setoutput( client->kcp, kcp_output );

    if ( type == 1 )
    {
        client->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    }
    else
    {
        client->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    }
    if ( client->fd < 0 )
    {
        return NULL;
    }

    set_non_block( client->fd );

    struct sockaddr_in clientaddr;
    memset(&clientaddr, 0, sizeof(clientaddr));
    clientaddr.sin_family = AF_INET;
    clientaddr.sin_port = 0;
    clientaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    if ( bind( client->fd, (struct sockaddr *)&clientaddr, sizeof(clientaddr) ) < 0 )
    {
        return NULL;
    }

    return client;
}

void pingpongclient_final( PingpongClient * cli )
{
    // TODO:
}

int32_t pingpongclient_connect( PingpongClient * client, const char * host, uint16_t port  )
{
    // 设置远程服务器地址
    memset( &client->addr, 0, sizeof(struct sockaddr_in) );
    client->addr.sin_family = AF_INET;
    client->addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &client->addr.sin_addr.s_addr);

    if ( connect( client->fd, (struct sockaddr*)&client->addr, sizeof(struct sockaddr) ) < 0)
    {
        if ( errno != EINPROGRESS )
        {
            printf( "pingpongclient_connect(%s::%d) : %s .\n", host, port, strerror(errno) );
            return -1;
        }
    }

    return 0;
}

void pingpongclient_on_read( int32_t fd, int16_t ev, void * arg )
{
    PingpongClient * cli = (PingpongClient *)arg;

    if ( ev & EV_READ )
    {
        char buffer[ 65536 ];
        ssize_t len = read( cli->fd, buffer, sizeof(buffer) );

        if ( len > 0 )
        {
            struct buffer inbuf;

            if ( cli->type == 1 )
            {
                buffer_append( &inbuf, buffer, len );
            }
            else
            {
                ikcp_input( cli->kcp, buffer, len );
                while ( 1 )
                {
                    ssize_t peeksize = ikcp_peeksize( cli->kcp );
                    if ( peeksize <= 0 )
                    {
                        break;
                    }

                    buffer_reserve( &inbuf, peeksize );
                    peeksize = ikcp_recv( cli->kcp, inbuf.buffer+inbuf.length, peeksize );
                    if ( peeksize <= 0 )
                    {
                        break;
                    }
                    inbuf.length += peeksize;
                }
            }
            if ( buffer_length(&inbuf) > 0 )
            {
                char * xx = buffer_data( &inbuf );
                size_t length = buffer_length( &inbuf );
                int64_t timestamp = *(int64_t *)( xx - 8 + length );

                xx[ length-8 ] = '\0';
                timestamp = be64toh( timestamp );
                printf( "%d> %s [-- %lu --]\n", cli->conv, xx, milliseconds()-timestamp );
                g_IsInput = 1; fflush( stdin );
            }

            buffer_clear( &inbuf );
        }
    }
}

int main(int argc, char* argv[])
{
    if (argc < 4)
    {
        printf("usage: %s type ip port\n", argv[0]);
        exit(1);
    }

    int64_t epoch = milliseconds();
    srand( time(NULL) );
    signal( SIGPIPE, SIG_IGN );
    signal( SIGINT, signal_handler );
    signal( SIGTERM, signal_handler );

    PingpongClient * cli = pingpongclient_init( atoi(argv[1]) );
    if ( cli == NULL )
    {
        printf( "kcp_init() failed\n" );
        exit( -1 );
    }

    if ( pingpongclient_connect( cli, argv[2], atoi(argv[3]) ) < 0 )
    {
        printf( "kcp_connect() failed\n" );
        exit( -2 );
    }

    event_set( cli->event, cli->fd, EV_READ|EV_PERSIST );
    event_set_callback( cli->event, pingpongclient_on_read, cli );
    evsets_add( cli->evsets, cli->event, -1 );

    // 初始状态
    g_IsInput = 1; g_IsRunning = 1;

    while ( g_IsRunning )
    {
        if ( g_IsInput == 1 )
        {
            size_t length = 0;
            char buffer[ INBUFFER_LEN ] = {0};

            printf( "%d< ", cli->conv );
            fflush( stdin );
            fgets( buffer, INBUFFER_LEN, stdin );

            length = strlen( buffer );
            if ( length != 0 )
            {
                g_IsInput = 0;
                *(int64_t *)(buffer+length-1) = htobe64( milliseconds() );

                if ( cli->type == 1 )
                {
                    write( cli->fd, buffer, length+7 );
                }
                else
                {
                    ikcp_send( cli->kcp, buffer, length+7 );
                    ikcp_flush( cli->kcp );
                }
            }
        }

        evsets_dispatch( cli->evsets );
        if ( cli->type == 2 )
            ikcp_update( cli->kcp, milliseconds()-epoch );
    }

    pingpongclient_final( cli );

    return 0;
}
