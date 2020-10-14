
#include <assert.h>
#include <strings.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>

#include <sys/time.h>
#include <sys/syscall.h>

#include <netdb.h>

#include "utils.h"
#include "config.h"

#if defined EVENT_OS_LINUX

__thread pid_t t_cached_threadid = 0;

pid_t threadid()
{
    if ( t_cached_threadid == 0 )
    {
        t_cached_threadid = syscall( SYS_gettid );
    }

    return t_cached_threadid;
}

#endif

int64_t milliseconds()
{
    int64_t now = -1;
    struct timeval tv;

    if ( gettimeofday( &tv, NULL ) == 0 )
    {
        now = tv.tv_sec * 1000ll + tv.tv_usec / 1000ll;
    }

    return now;
}

int64_t microseconds()
{
    int64_t now = -1;
    struct timeval tv;

    if ( gettimeofday( &tv, NULL ) == 0 )
    {
        now = tv.tv_sec * 1000000ll + tv.tv_usec;
    }

    return now;
}

int32_t is_ipv6only( int32_t fd )
{
    int yes = 1;
    socklen_t length = sizeof(yes);

    if ( setsockopt( fd,
                IPPROTO_IPV6, IPV6_V6ONLY, &yes, length ) == -1 )
    {
        return -1;
    }

    return 0;
}

int32_t is_connected( int32_t fd )
{
    int32_t value = -1;
    socklen_t length = sizeof(int32_t);

    if ( getsockopt( fd, SOL_SOCKET, SO_ERROR, (void *)&value, &length ) == 0 )
    {
        return value;
    }

    return value;
}

int32_t set_cloexec( int32_t fd )
{
    int32_t flags;
    int32_t rc = -1;

    flags = fcntl( fd, F_GETFD );
    if ( flags >= 0 )
    {
        flags |= FD_CLOEXEC;
        rc = fcntl(fd, F_SETFD, flags) != -1 ? 0 : -2 ;
    }

    return rc;
}

int32_t set_non_block( int32_t fd )
{
    int32_t flags;
    int32_t rc = -1;

    flags = fcntl( fd, F_GETFL );
    if ( flags >= 0 )
    {
        flags |= O_NONBLOCK;
        rc = fcntl(fd, F_SETFL, flags) != -1 ? 0 : -2 ;
    }

    return rc;
}

int32_t tcp_accept( int32_t fd, char * remotehost, uint16_t * remoteport )
{
    int32_t cfd = -1;
    struct sockaddr_storage cli_addr;
    socklen_t len = sizeof( cli_addr );

    *remoteport = 0;
    remotehost[0] = 0;
    bzero( &cli_addr, len );

    cfd = accept( fd, (struct sockaddr *)&cli_addr, &len );
    if ( cfd != -1 )
    {
        // 解析获得目标ip和端口
        parse_endpoint( &cli_addr, remotehost, remoteport );
    }

    return cfd;
}

int32_t tcp_listen( const char * host, uint16_t port, int32_t (*options)(int32_t) )
{
    int32_t fd = -1;
    char strport[ 6 ];
    struct addrinfo hints, *res, *p;

    bzero( &hints, sizeof(hints) );
    snprintf( strport, sizeof(strport), "%d", port );
    // 设置参数
    hints.ai_flags = AI_PASSIVE;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    // 获取地址信息
    if ( getaddrinfo( host, strport, &hints, &res ) != 0 )
    {
        return -1;
    }

    for ( p = res; p != NULL; p = p->ai_next )
    {
        fd = socket( p->ai_family, p->ai_socktype, p->ai_protocol );
        if ( fd < 0 )
        {
            continue;
        }

        // 对描述符的选项操作
        if ( options( fd ) != 0 )
        {
            close( fd );
            continue;
        }

        // bind
        if ( bind( fd, p->ai_addr, p->ai_addrlen ) == -1 )
        {
            close( fd );
            continue;
        }

        // listen
        if ( listen( fd, SOMAXCONN ) == -1 )
        {
            close( fd );
            continue;
        }

        // 绑定成功后退出循环
        break;
    }

    freeaddrinfo( res );
    if ( p == NULL )
    {
        return -2;
    }

    return fd;
}

int32_t tcp_connect( const char * host, uint16_t port, int32_t (*options)(int32_t) )
{
    int32_t fd = -1;
    char strport[ 6 ];
    struct addrinfo hints, *res, *p;

    bzero( &hints, sizeof(hints) );
    snprintf( strport, sizeof(strport), "%d", port );
    // 设置参数
    hints.ai_flags = AI_PASSIVE;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    // 获取地址信息
    if ( getaddrinfo( host, strport, &hints, &res ) != 0 )
    {
        return -1;
    }

    for ( p = res; p != NULL; p = p->ai_next )
    {
        int32_t rc = -1;

        fd = socket( p->ai_family, p->ai_socktype, p->ai_protocol );
        if ( fd < 0 )
        {
            continue;
        }

        // 对描述符的选项操作
        if ( options( fd ) != 0 )
        {
            close( fd );
            continue;
        }

        // 连接
        rc = connect( fd, p->ai_addr, p->ai_addrlen );
        if ( rc == -1 && errno != EINPROGRESS )
        {
            close( fd );
            continue;
        }

        // 连接成功后退出
        // TODO: FIX Linux TCP Self_Connecttion
        break;
    }

    freeaddrinfo( res );
    if ( p == NULL )
    {
        return -2;
    }

    return fd;
}

int32_t udp_bind( const char * host, uint16_t port, int32_t (*options)(int32_t), struct sockaddr_storage * addr )
{
    int32_t fd = -1;
    char strport[ 6 ];
    struct addrinfo hints, *res, *p;

    bzero( &hints, sizeof(hints) );
    snprintf( strport, sizeof(strport), "%d", port );
    // 设置参数
    hints.ai_flags = AI_PASSIVE;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    // 获取地址信息
    if ( getaddrinfo( host, strport, &hints, &res ) != 0 )
    {
        return -1;
    }

    for ( p = res; p != NULL; p = p->ai_next )
    {
#ifndef EVENT_USE_LOCALHOST
        char bindhost[ 64 ];
        switch ( p->ai_addr->sa_family )
        {
            case AF_INET :
                inet_ntop( p->ai_addr->sa_family,
                        &((struct sockaddr_in *)p->ai_addr)->sin_addr, bindhost, INET_ADDRSTRLEN );
                break;

            case AF_INET6 :
                inet_ntop( p->ai_addr->sa_family,
                        &((struct sockaddr_in6 *)p->ai_addr)->sin6_addr, bindhost, INET6_ADDRSTRLEN );
                break;
        }
        // 内核4.4.0之前的版本无法绑定127.0.0.1
        if ( strcmp(bindhost, "::1") == 0
                || strcmp(bindhost, "127.0.0.1") == 0 )
        {
            continue;
        }
#endif

        fd = socket( p->ai_family, p->ai_socktype, p->ai_protocol );
        if ( fd < 0 )
        {
            continue;
        }

        // 对描述符的选项操作
        if ( options( fd ) != 0 )
        {
            close( fd );
            continue;
        }

        // bind
        if ( bind( fd, p->ai_addr, p->ai_addrlen ) == -1 )
        {
            close( fd );
            continue;
        }

        // 绑定成功后退出循环
        memcpy( addr, p->ai_addr, p->ai_addrlen );
        break;
    }

    freeaddrinfo( res );
    if ( p == NULL )
    {
        return -2;
    }

    return fd;
}

int32_t udp_connect( struct sockaddr_storage * localaddr, struct sockaddr_storage * remoteaddr, int32_t (*options)(int32_t) )
{
    int32_t newfd = socket(
            ((struct sockaddr *)localaddr)->sa_family, SOCK_DGRAM, IPPROTO_UDP );
    if ( newfd <= 0 )
    {
        return -1;
    }

    options( newfd );

    // 绑定
    if ( bind( newfd, (struct sockaddr *)localaddr, sizeof(struct sockaddr) ) < 0 )
    {
        close( newfd );
        return -2;
    }

    // 连接
    int32_t rc = connect( newfd, (struct sockaddr *)remoteaddr, sizeof(struct sockaddr) );
    if ( rc == -1 && errno != EINPROGRESS )
    {
        close( newfd );
        return -3;
    }

    return newfd;
}

void parse_endpoint( struct sockaddr_storage * addr, char * host, uint16_t * port )
{
    struct sockaddr * saddr = (struct sockaddr *)addr;
    switch ( saddr->sa_family )
    {
        case AF_INET :
            *port = ntohs( ((struct sockaddr_in *)saddr)->sin_port );
            inet_ntop( saddr->sa_family,
                    &((struct sockaddr_in *)saddr)->sin_addr, host, INET_ADDRSTRLEN );
            break;

        case AF_INET6 :
            *port = ntohs( ((struct sockaddr_in6 *)saddr)->sin6_port );
            inet_ntop( saddr->sa_family,
                    &((struct sockaddr_in6 *)saddr)->sin6_addr, host, INET6_ADDRSTRLEN );
            break;
    }
}

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

uint32_t getpower( uint32_t size )
{
    uint32_t n = 0;

    for ( ; size >>= 1; )
    {
        ++n;
    }

    return n;
}

uint32_t nextpow2( uint32_t size )
{
    --size;
    size |= size >> 1;
    size |= size >> 2;
    size |= size >> 4;
    size |= size >> 8;
    size |= size >> 16;
    return ++size;
}

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

struct sidlist * sidlist_create( uint32_t size )
{
    struct sidlist * self = NULL;

    size = size ? size : 8;
    self = (struct sidlist *)malloc( sizeof(struct sidlist) );
    if ( self )
    {
        self->count = 0;
        self->size = size;
        self->entries = (sid_t *)malloc( self->size*sizeof(sid_t) );
        if ( unlikely(self->entries == NULL) )
        {
            free( self );
            self = NULL;
        }
    }

    return self;
}

sid_t sidlist_get( struct sidlist * self, int32_t index )
{
    sid_t sid = 0;
    uint32_t id = 0;

    id = index == -1 ? self->count-1 : index;
    if ( id < self->count )
    {
        sid = self->entries[id];
    }

    return sid;
}

int32_t sidlist_add( struct sidlist * self, sid_t id )
{
    if ( self->count+1 > self->size )
    {
        self->size <<= 1;

        self->entries = (sid_t *)realloc( self->entries, sizeof(sid_t)*self->size );
        assert( self->entries != NULL );
    }

    self->entries[self->count++] = id;

    return 0;
}

int32_t sidlist_adds( struct sidlist * self, sid_t * ids, uint32_t count )
{
    uint32_t totalcount = self->count + count;

    if ( totalcount > self->size )
    {
        self->size = totalcount;

        self->entries = (sid_t *)realloc( self->entries, sizeof(sid_t)*self->size );
        assert( self->entries != NULL );
    }

    memcpy( self->entries+self->count, ids, count*sizeof(sid_t) );
    self->count = totalcount;

    return 0;
}

sid_t sidlist_del( struct sidlist * self, int32_t index )
{
    sid_t rc = 0;
    uint32_t id = 0;

    id = index == -1 ? self->count-1 : index;
    if ( id < self->count )
    {
        --self->count;
        rc = self->entries[id];

        if ( id != self->count )
        {
            self->entries[id] = self->entries[self->count];
        }
    }

    return rc;
}

void sidlist_destroy( struct sidlist * self )
{
    if ( self->entries )
    {
        free( self->entries );
        self->entries = NULL;
    }

    free(self);
}

// -----------------------------------------------------------------------------------------------------------------

QUEUE_GENERATE( taskqueue, struct task )

struct msgqueue * msgqueue_create( uint32_t size )
{
    struct msgqueue * self = NULL;

    self = (struct msgqueue *)malloc( sizeof(struct msgqueue) );
    if ( self )
    {
        self->popfd = -1;
        self->pushfd = -1;
        evlock_init( &self->lock );

        if ( QUEUE_INIT(taskqueue)(&self->queue, size) != 0 )
        {
            msgqueue_destroy( self );
            self = NULL;
        }
        else
        {
            int32_t rc = -1;

#if !defined EVENT_HAVE_EVENTFD
            int32_t fds[2] = { -1, -1 };
            rc = pipe( fds );
            //rc = socketpair( AF_UNIX, SOCK_STREAM, 0, fds );
            if ( rc != -1 )
            {
                self->popfd = fds[0];
                self->pushfd = fds[1];
#ifdef O_NOATIME
                // linux在读pipe的时候会更新访问时间, touch_atime(), 这个的开销也不小
                fcntl( self->popfd, F_SETFL, O_NOATIME );
#endif
            }
#else
            syslog(LOG_INFO, "%s() use eventfd() .", __FUNCTION__ );
            rc = eventfd( 0, EFD_NONBLOCK | EFD_CLOEXEC );
            if ( rc >= 0 )
            {
                self->popfd = rc;
                self->pushfd = rc;
            }
#endif
            if ( rc == -1 )
            {
                msgqueue_destroy( self );
                self = NULL;
            }
        }
    }

    return self;
}

int32_t msgqueue_push( struct msgqueue * self, struct task * task, uint8_t isnotify )
{
    int32_t rc = -1;
    uint32_t isbc = 0;

    evlock_lock( &self->lock );

    rc = QUEUE_PUSH(taskqueue)(&self->queue, task);
    if ( isnotify != 0 )
    {
        isbc = QUEUE_COUNT(taskqueue)(&self->queue);
    }

    evlock_unlock( &self->lock );

    if ( rc == 0 && isbc == 1 )
    {
        uint64_t one = 1;

        if ( sizeof( one )
                != write( self->pushfd, &one, sizeof(one) ) )
        {
            // 写出错了
            syslog( LOG_WARNING, "%s() : write to Pipe(fd:%u) error .", __FUNCTION__, self->pushfd );
        }
    }

    return rc;
}

int32_t msgqueue_pop( struct msgqueue * self, struct task * task )
{
    int32_t rc = -1;

    evlock_lock( &self->lock );
    rc = QUEUE_POP(taskqueue)(&self->queue, task);
    evlock_unlock( &self->lock );

    return rc;
}

int32_t msgqueue_pops( struct msgqueue * self, struct task * tasks, uint32_t count )
{
    uint32_t i = 0;

    evlock_lock( &self->lock );
    for ( i = 0; i < count; ++i )
    {
        int32_t rc = QUEUE_POP(taskqueue)(&self->queue, &tasks[i]);
        if ( rc == 0 )
        {
            break;
        }
    }
    evlock_unlock( &self->lock );

    return i;
}

int32_t msgqueue_swap( struct msgqueue * self, struct taskqueue * queue )
{
    evlock_lock( &self->lock );
    QUEUE_SWAP(taskqueue)(&self->queue, queue);
    evlock_unlock( &self->lock );

    return 0;
}

uint32_t msgqueue_count( struct msgqueue * self )
{
    uint32_t rc = 0;

    evlock_lock( &self->lock );
    rc = QUEUE_COUNT(taskqueue)(&self->queue);
    evlock_unlock( &self->lock );

    return rc;
}

int32_t msgqueue_popfd( struct msgqueue * self )
{
    int32_t rc = 0;

    evlock_lock( &self->lock );
    rc = self->popfd;
    evlock_unlock( &self->lock );

    return rc;
}

int32_t msgqueue_destroy( struct msgqueue * self )
{
    if ( self->popfd )
    {
        close( self->popfd );
    }
    if ( self->pushfd
            && self->pushfd != self->popfd )
    {
        close( self->pushfd );
    }

    self->popfd = -1;
    self->pushfd = -1;

    QUEUE_CLEAR(taskqueue)(&self->queue);
    evlock_destroy( &self->lock );
    free( self );

    return 0;
}
