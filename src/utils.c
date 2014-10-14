
#include <assert.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <sys/time.h>
#include <sys/syscall.h>

#include "utils.h"

#if defined(__linux__)

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

int64_t mtime()
{
    int64_t now = -1;
    struct timeval tv;

    if ( gettimeofday( &tv, NULL ) == 0 )
    {
        now = tv.tv_sec * 1000ll + tv.tv_usec / 1000ll;
    }

    return now;
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

int32_t set_non_block( int32_t fd )
{
    int32_t flags;
    int32_t rc = -1;

    flags = fcntl( fd, F_GETFL );
    if ( flags >= 0 )
    {
        flags |= O_NONBLOCK;
        rc = fcntl(fd, F_SETFL, flags)!=-1 ? 0 : -2 ;
    }

    return rc;
}

int32_t tcp_accept( int32_t fd, char * remotehost, uint16_t * remoteport )
{
    int32_t cfd = -1;
    struct sockaddr_in in_addr;
    socklen_t len = sizeof( struct sockaddr );

    *remoteport = 0;
    remotehost[0] = 0;

    memset( &in_addr, 0, sizeof(in_addr) );

    cfd = accept( fd, (struct sockaddr *)&in_addr, &len );
    if ( cfd != -1 )
    {
        *remoteport = ntohs( in_addr.sin_port );
        strncpy( remotehost, inet_ntoa(in_addr.sin_addr), INET_ADDRSTRLEN );
    }

    return cfd;
}

int32_t tcp_listen( char * host, uint16_t port, void (*options)(int32_t) )
{
    int32_t fd = -1;
    struct sockaddr_in addr;

    fd = socket( AF_INET, SOCK_STREAM, 0 );
    if ( fd < 0 )
    {
        return -1;
    }

    // 对描述符的选项操作
    options( fd );

    memset( &addr, 0, sizeof(addr) );
    addr.sin_family = AF_INET;
    addr.sin_port    = htons( port );
    if ( host != NULL && strlen(host) > 0 )
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

int32_t tcp_connect( char * host, uint16_t port, void (*options)(int32_t) )
{
    int32_t fd = -1;
    int32_t rc = -1;
    struct sockaddr_in addr;

    fd = socket( AF_INET, SOCK_STREAM, 0 );
    if ( fd < 0 )
    {
        return -1;
    }

    // 对描述符的选项操作
    options( fd );

    memset( &addr, 0, sizeof(addr) );
    addr.sin_family = AF_INET;
    addr.sin_port    = htons(port);
    inet_pton(AF_INET, host, (void *)&(addr.sin_addr.s_addr));

    rc = connect( fd, (struct sockaddr *)&addr, sizeof(struct sockaddr) );
    if ( rc == -1 && errno != EINPROGRESS )
    {
        // 连接出错
        close( fd );
        fd = -1;
    }

    if ( fd >= 0 )
    {
        // Fix: Linux TCP Self-Connection
        struct sockaddr_in laddr;
        socklen_t llen = sizeof(struct sockaddr);

        rc = getsockname( fd, (struct sockaddr *)&laddr, &llen );
        if ( rc == 0
                && addr.sin_port == laddr.sin_port
                && addr.sin_addr.s_addr == laddr.sin_addr.s_addr )
        {
            close( fd );
            fd = -1;
        }
    }

    return fd;
}

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

uint32_t getpower( uint32_t size )
{
    uint32_t n = 0;

    while ( size >>= 1 )
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
        pthread_mutex_init( &self->lock, 0 );

        if ( QUEUE_INIT(taskqueue)(&self->queue, size) != 0 )
        {
            msgqueue_destroy( self );
            self = NULL;
        }
        else
        {
            int32_t rc = -1;
            int32_t fds[2] = { -1, -1 };

            rc = pipe( fds );
            //rc = socketpair( AF_UNIX, SOCK_STREAM, 0, fds );
            if ( rc == -1 )
            {
                msgqueue_destroy( self );
                self = NULL;
            }
            else
            {
                self->popfd = fds[0];
                self->pushfd = fds[1];

#ifdef O_NOATIME
                // linux在读pipe的时候会更新访问时间, touch_atime(), 这个的开销也不小
                fcntl( self->popfd, F_SETFL, O_NOATIME );
#endif
            }
        }
    }

    return self;
}

int32_t msgqueue_push( struct msgqueue * self, struct task * task, uint8_t isnotify  )
{
    int32_t rc = -1;
    uint32_t isbc = 0;

    pthread_mutex_lock( &self->lock );

    rc = QUEUE_PUSH(taskqueue)(&self->queue, task);
    if ( isnotify != 0 )
    {
        isbc = QUEUE_COUNT(taskqueue)(&self->queue);
    }

    pthread_mutex_unlock( &self->lock );

    if ( rc == 0 && isbc == 1 )
    {
        char buf[1] = {0};

        if ( write( self->pushfd, buf, 1 ) != 1 )
        {
            //
        }
    }

    return rc;
}

int32_t msgqueue_pop( struct msgqueue * self, struct task * task )
{
    int32_t rc = -1;

    pthread_mutex_lock( &self->lock );
    rc = QUEUE_POP(taskqueue)(&self->queue, task);
    pthread_mutex_unlock( &self->lock );

    return rc;
}

int32_t msgqueue_swap( struct msgqueue * self, struct taskqueue * queue )
{
    pthread_mutex_lock( &self->lock );
    QUEUE_SWAP(taskqueue)(&self->queue, queue);
    pthread_mutex_unlock( &self->lock );

    return 0;
}

uint32_t msgqueue_count( struct msgqueue * self )
{
    uint32_t rc = 0;

    pthread_mutex_lock( &self->lock );
    rc = QUEUE_COUNT(taskqueue)(&self->queue);
    pthread_mutex_unlock( &self->lock );

    return rc;
}

int32_t msgqueue_popfd( struct msgqueue * self )
{
    int32_t rc = 0;

    pthread_mutex_lock( &self->lock );
    rc = self->popfd;
    pthread_mutex_unlock( &self->lock );

    return rc;
}

int32_t msgqueue_destroy( struct msgqueue * self )
{
    if ( self->popfd )
    {
        close( self->popfd );
        self->popfd = -1;
    }
    if ( self->pushfd )
    {
        close( self->pushfd );
        self->pushfd = -1;
    }

    QUEUE_CLEAR(taskqueue)(&self->queue);
    pthread_mutex_destroy( &self->lock );
    free( self );

    return 0;
}
