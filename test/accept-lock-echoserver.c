
/*
 * cc -I/usr/local/include -lpthread -L/usr/local/lib -levent test/accept-lock-echoserver.c -o echoserver-lock-libevent
 */


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

#include "utils.h"

#define __DEBUG         0
#define MAGIC_NUM       9
#define USE_LIBEVENT    1
#define TIMEOUT_MSECS   2000

#if USE_LIBEVENT

    #include <event.h>

    typedef struct event *          event_t;
    typedef struct event_base *     evsets_t;

    #define event_create()          malloc(sizeof(struct event))
    #define event_destroy(self)     free( (self) )

    #define evsets_create           event_base_new
    #define evsets_destroy          event_base_free
    #define evsets_dispatch(sets)   event_base_loop( sets, EVLOOP_ONCE )

#else

    #include "event.h"

#endif

int8_t isstarted;

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

int32_t tcp_listen( const char * host, uint16_t port )
{
    int32_t fd = -1;

    int32_t flags = 1;
    struct linger ling = {1, 0};

    struct sockaddr_in addr;

    fd = socket( AF_INET, SOCK_STREAM, 0 );
    if ( fd < 0 )
    {
        return -1;
    }

    /* TCP Socket Option Settings */
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

    set_fd_nonblock( fd );

    memset( &addr, 0, sizeof(addr) );
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if ( host != NULL )
    {
        addr.sin_addr.s_addr = inet_addr( host );
    }
    else
    {
        addr.sin_addr.s_addr = INADDR_ANY;
    }

    if ( bind( fd, (struct sockaddr *)&addr, sizeof(addr) ) == -1 )
    {
        close( fd );
        return -2;
    }

    if ( listen( fd, 8192 ) == -1 )
    {
        close( fd );
        return -3;
    }

    return fd;
}

int32_t tcp_accept( int32_t fd, char * srchost, char * dsthost, uint16_t *dstport )
{
    int32_t rc = -1;
    struct sockaddr_in in_addr;
    socklen_t len = sizeof( struct sockaddr );

    srchost[0] = 0;
    dsthost[0] = 0;
    *dstport = 0;

    memset( &in_addr, 0, sizeof(in_addr) );

    rc = accept( fd, (struct sockaddr *)&in_addr, &len );
    if ( rc != -1 )
    {
        strncpy( dsthost, inet_ntoa(in_addr.sin_addr), INET_ADDRSTRLEN );
        *dstport = ntohs( in_addr.sin_port );

        memset( &in_addr, 0, sizeof(in_addr) );
        if( getsockname( rc, (struct sockaddr*)&in_addr, &len ) == 0 )
        {
            strncpy( srchost, inet_ntoa(in_addr.sin_addr), INET_ADDRSTRLEN );
        }
    }

    return rc;
}



// --------------------------------------------------------------------------------------------
// --------------------------------------------------------------------------------------------
// --------------------------------------------------------------------------------------------

struct acceptor
{
    int8_t holding;

    int32_t socketfd;
    event_t ev_accept;

    pthread_mutex_t lock;
};

struct session
{
    event_t evread;
//    event_t evwrite;

    int32_t fd;
    uint64_t sid;

    size_t iobytes;
};

struct iothread
{
    int8_t running;
    evsets_t core_sets;

    uint8_t key;
    uint32_t index;

    struct acceptor * core_acceptor;
};

void echoserver_signal_handler( int32_t signo )
{
    isstarted = 0;
}


void echoserver_process_message( int32_t fd, int16_t ev, void * arg )
{
    struct session * s = (struct session *)arg;

    if ( ev & EV_READ )
    {
        char buf[16384];
        ssize_t readn = -1;

        readn = read( fd, buf, 16384 );
        if ( readn <= 0 )
        {
#if __DEBUG
            printf( "Client[%ld, %d] is closed, BYTES:%lu, TIME:%lld .\n",
                s->sid, s->fd, s->iobytes, milliseconds() );
#endif
            //evsets_del( event_get_sets(s->evread), s->evread );
            event_destroy( s->evread );
            close( s->fd );
            free( s );

            goto PROCESS_END;
        }
        else
        {
#if __DEBUG
            printf("echoserver_process_message(ev:%d) : TIME:%lld .\n", ev, milliseconds() );
#endif
            readn = write( fd, buf, readn );
            s->iobytes += readn;
        }

#if USE_LIBEVENT
        {
            struct timeval tv = {TIMEOUT_MSECS/1000, 0};
            event_add( s->evread, &tv );
        }
#else
        evsets_add( event_get_sets(s->evread), s->evread, TIMEOUT_MSECS );
#endif
    }
    else
    {
#if __DEBUG
        printf("echoserver_process_message(ev:%d) : TIME:%lld .\n", ev, milliseconds() );
#endif
    }

PROCESS_END :
}

void accept_new_session( int32_t fd, int16_t ev, void * arg )
{
    struct iothread * thr = (struct iothread *)arg;
    struct acceptor * a = thr->core_acceptor;

    if ( ev & EV_READ )
    {
        //
        // 接收新连接完毕后
        //
        char srchost[20];
        char dsthost[20];
        uint16_t dstport = 0;

        int32_t newfd = tcp_accept( fd, srchost, dsthost, &dstport );
        if ( newfd > 0 )
        {
            uint64_t sid = 0;
            struct session * newsession = NULL;

            set_fd_nonblock( newfd );

            newsession = (struct session *)malloc( sizeof(struct session) );
            if ( newsession == NULL )
            {
                printf("Out of memory, allocate for 'newsession' failed .\n");
                goto ACCEPT_END;
            }

            newsession->evread = event_create();
            if ( newsession->evread == NULL )
            {
                printf("Out of memory, allocate for 'newsession->evread' failed .\n");
                goto ACCEPT_END;
            }

            newsession->iobytes = 0;
            newsession->fd = newfd;

            sid = thr->key;
            sid <<= 32;
            sid += thr->index++;
            newsession->sid = sid;

#if USE_LIBEVENT
            {
                struct timeval tv = {TIMEOUT_MSECS/1000, 0};
                event_set( newsession->evread, newsession->fd, EV_READ, echoserver_process_message, newsession );
                event_base_set( thr->core_sets, newsession->evread );
                event_add( newsession->evread, &tv );
            }
#else
            event_set( newsession->evread, newsession->fd, EV_READ );
            event_set_callback( newsession->evread, echoserver_process_message, newsession );
            evsets_add( thr->core_sets, newsession->evread, TIMEOUT_MSECS );
#endif

#if __DEBUG
            printf( "Thread[%d] accept a new Client[%lld, fd:%d, '%s':%d] .\n",
                thr->key, newsession->sid, newsession->fd, dsthost, dstport );
#endif
        }

        a->holding = 0;
        pthread_mutex_unlock( &(a->lock) );
    }

ACCEPT_END :
}

void trylock_accept_mutex( struct iothread * thr )
{
    struct acceptor * a = thr->core_acceptor;

    if ( pthread_mutex_trylock(&(a->lock)) == 0 )
    {
        if ( a->holding == 0 )
        {
#if USE_LIBEVENT
            event_set( a->ev_accept, a->socketfd, EV_READ, accept_new_session, thr );
            event_base_set( thr->core_sets, a->ev_accept );
            event_add( a->ev_accept, -1 );
#else
            event_set( a->ev_accept, a->socketfd, EV_READ );
            event_set_callback( a->ev_accept, accept_new_session, thr );
            evsets_add(thr->core_sets, a->ev_accept, -1 );
#endif
            a->holding = 1;
        }

        //pthread_mutex_unlock( &(a->lock) );
    }
}

void acceptor_destroy( struct acceptor * self )
{
    pthread_mutex_destroy( &(self->lock) );

    if ( self->socketfd > 0 )
    {
        close( self->socketfd );
    }

    if ( self->ev_accept != NULL )
    {
        event_destroy( self->ev_accept );
    }

    free( self );
}

struct acceptor * acceptor_create( const char * host, uint16_t port )
{
    struct acceptor * a = NULL;

    a = (struct acceptor *)malloc( sizeof(struct acceptor) );
    if ( a )
    {
        a->holding = 0;
        a->socketfd = 0;
        a->ev_accept = NULL;
        pthread_mutex_init( &(a->lock), NULL );

        a->socketfd = tcp_listen( host, port );
        if ( a->socketfd < 0 )
        {
            acceptor_destroy( a );
            return NULL;
        }

        set_fd_nonblock( a->socketfd );

        a->ev_accept = event_create();
        if ( a->ev_accept == NULL )
        {
            acceptor_destroy( a );
            return NULL;
        }
    }

    return a;
}

void * iothread_main( void * arg )
{
    struct iothread * thr = (struct iothread *)arg;

    thr->running = 1;
    while ( thr->running )
    {
        //
        // 尝试加锁
        //
        trylock_accept_mutex( thr );

        //
        // 分发IO事件
        //
        evsets_dispatch( thr->core_sets );
    }

    return (void *)0;
}

struct iothread * iothread_create( uint8_t key, struct acceptor * a )
{
    pthread_t tid;
    struct iothread * thr = NULL;

    thr = (struct iothread *)malloc( sizeof(struct iothread) );
    if ( thr )
    {
        thr->key = key;
        thr->index = 0;
        thr->core_acceptor = a;

        thr->core_sets = evsets_create();
        if ( thr->core_sets == NULL )
        {
#if __DEBUG
            printf( "out of memory, allocate for 'thr->core_sets' failed, Thread[%d] .\n", key );
#endif
            return NULL;
        }

        pthread_create( &tid, NULL, iothread_main, thr );
    }

    return thr;
}

// --------------------------------------------------------------------------------------------
// --------------------------------------------------------------------------------------------
// --------------------------------------------------------------------------------------------

int main( int argc, char ** argv )
{
    uint8_t i = 0;
    uint16_t port = 0;
    uint8_t threadcount = 0;

    struct acceptor * a = NULL;
    struct iothread ** thrgroup = NULL;

    if ( argc != 3 )
    {
        printf("echoserver [port] [threadcount] .\n");
        return 0;
    }

    signal( SIGPIPE, SIG_IGN );
    signal( SIGINT, echoserver_signal_handler );
    signal( SIGTERM, echoserver_signal_handler );

    port = (uint16_t)atoi( argv[1] );
    threadcount = (uint8_t)atoi( argv[2] );

    a = acceptor_create( "127.0.0.1", port );

    thrgroup = (struct iothread **)malloc( threadcount*sizeof(struct iothread *) );
    for ( i = 0; i < threadcount; ++i )
    {
        thrgroup[i] = iothread_create( i+1, a );
    }

    isstarted = 1;
    while ( isstarted )
    {
    #if defined(__FreeBSD__)
        sleep(2);
    #else
        pause();
    #endif
    }

    for ( i = 0; i < threadcount; ++i )
    {
        evsets_destroy( thrgroup[i]->core_sets );
    }

    return 0;
}
