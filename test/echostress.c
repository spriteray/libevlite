
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <event.h>

// --------------------------------------------------------------------------------------------
// --------------------------------------------------------------------------------------------
// --------------------------------------------------------------------------------------------

#define SENDBUFFER_SIZE 16384
#define INBUFFER_SIZE   16384

struct stress_client
{
    int32_t fd;

    struct event evread;
    struct event evwrite;

    char inbuffer[INBUFFER_SIZE];

    size_t send_nbytes;
    size_t recv_nbytes;
};

static  int8_t g_running;

static  char * g_host;
static  uint16_t g_port;
static  int32_t g_clients_count;

static struct stress_client * g_clients;

static struct timeval g_end;
static struct timeval g_start;


// --------------------------------------------------------------------------------------------
// --------------------------------------------------------------------------------------------
// --------------------------------------------------------------------------------------------

void stress_signal( int32_t signo )
{
    g_running = 0;
}

int32_t set_fd_nonblock( int32_t fd )
{
    int32_t flags ;
    int32_t retval = -1;

    flags = fcntl( fd, F_GETFL );
    if ( flags >= 0 )
    {
        flags |= O_NONBLOCK;

        if ( fcntl( fd, F_SETFL, flags ) >= 0 )
        {
            retval = 0;
        }
    }

    return retval;
}

void stress_client_close( struct stress_client * client )
{
    event_del( &client->evread );
    event_del( &client->evwrite );

    if ( client->fd > 0 )
    {
        close( client->fd );
        client->fd = -1;
    }
}

void client_on_read( int32_t fd, int16_t ev, void * arg )
{
    struct stress_client * client = (struct stress_client *)arg;

    if ( ev & EV_READ )
    {
        ssize_t nread = read( fd, client->inbuffer, INBUFFER_SIZE );
        if ( nread <= 0 )
        {
            if( nread < 0 && EAGAIN != errno )
            {
                printf( "#%d client_on_read error, recvbytes %d, writebytes %d, errno %d, %s\n",
                    fd, client->recv_nbytes, client->send_nbytes, errno, strerror( errno ) );
            }

            stress_client_close( client );
            return;
        }

        client->recv_nbytes += nread;

        if ( g_running == 0
            && client->recv_nbytes == client->send_nbytes )
        {
             stress_client_close( client );
        }
    }
}

void client_on_write( int32_t fd, int16_t ev, void * arg )
{
    struct stress_client * client = (struct stress_client *)arg;

    if ( ev & EV_WRITE )
    {
        char buf[SENDBUFFER_SIZE] = {1};

        ssize_t nwrite = write( client->fd, buf, SENDBUFFER_SIZE );
        if ( nwrite == -1 )
        {
            printf( "#%d client_on_write error, writebytes %lu, errno %d, %s\n",
                fd, client->send_nbytes, errno, strerror( errno ) );
            return ;
        }

        client->send_nbytes += nwrite;

        if ( g_running == 0 )
        {
            event_del( &client->evwrite );
        }
    }
}

void start_clients( struct event_base * base )
{
    int32_t i = 0;
    struct sockaddr_in addr;

    double mseconds = 0;
    struct timeval connect_start;
    struct timeval connect_end;

    //
    memset( &addr, 0, sizeof(addr) );
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr( g_host );
    addr.sin_port = htons( g_port );

    gettimeofday(&connect_start, NULL);

    //
    for ( i = 0; i < g_clients_count; ++i )
    {
        struct stress_client * client = g_clients + i;

        client->fd = socket( AF_INET, SOCK_STREAM, 0 );
        if( client->fd < 0 )
        {
            printf("#%d, socket failed, errno %d, %s\n", i, errno, strerror( errno ) );
            return ;
        }

        if( connect( client->fd, (struct sockaddr *)&addr, sizeof(addr) ) != 0)
        {
            printf("#%d, connect failed, errno %d, %s\n", i, errno, strerror( errno ) );
            return ;
        }

        //set_fd_nonblock( client->fd );

        event_set( &client->evread, client->fd, EV_READ | EV_PERSIST, client_on_read, client );
        event_base_set( base, &client->evread );
        event_add( &client->evread, NULL );

        event_set( &client->evwrite, client->fd, EV_WRITE | EV_PERSIST, client_on_write, client );
        event_base_set( base, &client->evwrite );
        event_add( &client->evwrite, NULL );

        if( 0 == ( i % 10 ) ) write( fileno( stdout ), ".", 1 );
    }
    printf("\n");

    gettimeofday(&connect_end, NULL);

    mseconds = (double) (
    (connect_end.tv_sec-connect_start.tv_sec)*1000 + (connect_end.tv_usec-connect_start.tv_usec)/1000 );
    printf("start_clients(CLIENTS=%d) ExecTimes: %.6f mseconds\n", g_clients_count, mseconds );
}

int main( int argc, char ** argv )
{
    int32_t c, i ;
    extern char *optarg ;

    double total_time = 0;
    struct event_base * base = NULL;
    uint64_t total_sendlen = 0, total_recvlen = 0;

    while( ( c = getopt ( argc, argv, "h:p:c:v" )) != EOF )
    {
        switch ( c )
        {
            case 'h' :
                g_host = optarg;
            break;

            case 'p':
                g_port = (uint16_t)atoi( optarg );
            break;

            case 'c' :
                g_clients_count = atoi ( optarg );
            break;

            case 'v' :
            case '?' :
            return -1;
        }
    }

    //
    // 注册信号
    //
    signal( SIGPIPE, SIG_IGN );
    signal( SIGINT, stress_signal );
    signal( SIGINT, stress_signal );

    //
    // 初始化事件集
    //
    base = event_base_new();
    if ( base == NULL )
    {
        printf("out of memory, allocate for 'base' failed .\n");
        return -2;
    }

    //
    // 为所有的客户端分配内存
    //
    g_clients = calloc( g_clients_count, sizeof(struct stress_client) );
    if ( g_clients == NULL )
    {
        printf("out of memory, allocate for 'g_clients' failed .\n");
        return -2;
    }

    //
    // 打开所有客户端
    //
    g_running = 1;
    start_clients( base );
    printf("IO Test Begin, you can press Ctrl-C to break, and see the IOTestReport ... \n");

    gettimeofday( &g_start, NULL );
    while ( g_running )
    {
        event_base_loop( base, EVLOOP_ONCE );
    }
    gettimeofday( &g_end, NULL );

    total_time = (double) ( 1000000*(g_end.tv_sec-g_start.tv_sec) + g_end.tv_usec-g_start.tv_usec ) / 1000000;

    // show result
    printf( "\n\nTest result :\n" );
    printf( "Host %s, Port %d, Clients %d\n", g_host, g_port, g_clients_count );
    printf( "ExecTimes: %.6f seconds\n\n", total_time );

    printf( "client\t\t\tSend\t\t\tRecv\n" );
    for( i = 0; i < g_clients_count; i++ )
    {
        struct stress_client * client = g_clients + i;

        //printf( "client#%d : %d\t%d\n", i, client->mSendMsgs, client->mRecvMsgs );

        total_sendlen += client->send_nbytes;
        total_recvlen += client->recv_nbytes;

        stress_client_close( client );
    }

    printf( "total : \t\t%lld\t\t%lld\n", total_sendlen, total_recvlen );
    printf( "average[KBytes/sec] : \t%.0f\t\t\t%.0f\n", total_sendlen / total_time / 1000.0f, total_recvlen / total_time / 1000.0f );

    free( g_clients );

    return 0;
}
