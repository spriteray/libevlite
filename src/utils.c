
#include <assert.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <sys/time.h>

#include "utils.h"

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

int32_t tcp_connect( char * host, uint16_t port, int8_t isasyn )
{
	int32_t fd = -1;
	int32_t rc = -1;

	struct sockaddr_in addr;

	fd = socket( AF_INET, SOCK_STREAM, 0 );
	if ( fd < 0 )
	{
		return -1;
	}

	if ( isasyn != 0 )
	{
		// 指定了异步连接
		set_non_block( fd );
	}

	memset( &addr, 0, sizeof(addr) );
	addr.sin_family = AF_INET;
	addr.sin_port	= htons(port);
	inet_pton(AF_INET, host, (void *)&(addr.sin_addr.s_addr));

	rc = connect( fd, (struct sockaddr *)&addr, sizeof(struct sockaddr) );
	if ( rc == -1 && errno != EINPROGRESS )
	{
		// 连接出错
		close( fd );
		fd = -1;
	}

	return fd;
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
	addr.sin_port	= htons( port );
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

struct arraylist * arraylist_create( uint32_t size )
{
	struct arraylist * self = NULL;

	self = (struct arraylist *)malloc( sizeof(struct arraylist) );
	if ( self )
	{
		if ( arraylist_init( self, size ) != 0 )
		{
			free( self );
			self = NULL;
		}
	}

	return self;
}

int32_t arraylist_init( struct arraylist * self, uint32_t size )
{
	size = size ? size : 8;

    self->count = 0;
    self->size = size;
    self->entries = (void **)calloc( self->size, sizeof(void *) );
    if ( self->entries == NULL )
    {
        free( self );
        return -1;
    }

    return 0;
}

uint32_t arraylist_count( struct arraylist * self )
{
    return self->count;
}

void arraylist_reset( struct arraylist * self )
{
    memset( self->entries, 0, sizeof(void *)*self->count );
    self->count = 0;

    return;
}

void arraylist_final( struct arraylist * self )
{
	if ( self->entries )
	{
		free( self->entries );
		self->entries = NULL;
	}

	self->size = 0;
	self->count = 0;
	return;
}

int32_t arraylist_append( struct arraylist * self, void * data )
{
    if ( self->count >= self->size )
    {
        self->size <<= 1;
        self->entries = (void **)realloc( self->entries, sizeof(void *)*self->size );

        assert( self->entries != NULL );
        memset( self->entries+self->count, 0, (self->size-self->count)*sizeof(void *) );
    }

    self->entries[self->count++] = data;

    return 0;
}

void * arraylist_get( struct arraylist * self, int32_t index )
{
    uint32_t id = 0;
    void * data = NULL;

    id = index == -1 ? self->count-1 : index;
    if ( id < self->count )
    {
        data = self->entries[id];
    }

    return data;
}

void * arraylist_take( struct arraylist * self, int32_t index )
{
    uint32_t id = 0;
    void * data = NULL;

    id = index == -1 ? self->count-1 : index;
    if ( id < self->count )
    {
        data = self->entries[id];
        --self->count;
#if 0
        if ( id != self->count )
        {
            self->entries[id] = self->entries[self->count];
        }

        self->entries[self->count] = NULL;
#endif

		if ( index+1 < self->size )
		{
			memmove( self->entries+id, self->entries+id+1, (self->size-id-1)*sizeof(void *) );
		}
		else
		{
			self->entries[index] = NULL;
		}
    }

    return data;
}

int32_t arraylist_destroy( struct arraylist * self )
{
    if ( self->entries )
    {
        free( self->entries );
        self->entries = NULL;
    }

    free(self);

    return 0;
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
		self->entries = (sid_t *)calloc( self->size, sizeof(sid_t) );
		if ( self->entries == NULL )
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
	if ( self->count >= self->size )
	{
		self->size <<= 1;
		self->entries = (sid_t *)realloc( self->entries, sizeof(sid_t)*self->size );

		assert( self->entries != NULL );
		memset( self->entries+self->count, 0, (self->size-self->count)*sizeof(sid_t) );
	}

	self->entries[self->count++] = id;

	return 0;
}

sid_t sidlist_del( struct sidlist * self, int32_t index )
{
	sid_t rc = 0;
	uint32_t id = 0;

	id = index == -1 ? self->count-1 : index;
	if ( id < self->count )
	{
		rc = self->entries[id];
		--self->count;

		if ( id != self->count )
		{
			self->entries[id] = self->entries[self->count];
		}

		self->entries[self->count] = 0;
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
	return;
}

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

static int32_t _queue_expand( struct queue * self );

int32_t _queue_expand( struct queue * self )
{
	// 需要扩展
	uint32_t headlen = 0, taillen = 0;
	uint32_t new_size = self->size << 1;

	struct task * new_entries = NULL;

	new_entries = (struct task * )malloc( sizeof(struct task) * new_size );
	if ( new_entries == NULL )
	{
		// 扩展内存块失败
		return -1;
	}

	uint32_t headidx = self->head&(self->size-1);
	uint32_t tailidx = self->tail&(self->size-1);

	if ( headidx < tailidx )
	{
		headlen = tailidx - headidx; 
	}
	else
	{
		headlen = self->size - headidx;
		taillen = tailidx;
	}

	memcpy( new_entries, self->entries+headidx, sizeof(struct task)*headlen );
	memcpy( new_entries+headlen, self->entries, sizeof(struct task)*taillen );

	self->head = 0;
	self->tail = headlen+taillen; 
	self->size = new_size;

	free ( self->entries );
	self->entries = new_entries;

	return 0;	
}

struct queue * queue_create( uint32_t size )
{
	struct queue * self = NULL;

	size = size ? size : 8;
	size = nextpow2( size );

	self = (struct queue *)malloc( sizeof(struct queue) );
	if ( self )
	{
		self->entries = (struct task *)calloc( size, sizeof(struct task) );
		if ( self->entries )
		{
			self->size 	= size;
			self->head 	= self->tail = 0;
		}
		else
		{
			free( self );
			self = NULL;
		}
	}

	return self;
}

int32_t queue_push( struct queue * self, struct task * task )
{
	if ( self->size + self->head - self->tail <= 0 ) 
	{
		// 需要扩展
		if ( _queue_expand(self) != 0 )
		{
			return -1; 
		}
	}	
	
	self->entries[self->tail&(self->size-1)] = *task;
	++self->tail;

	return 0;
}

int32_t queue_pop( struct queue * self, struct task * task )
{
	uint32_t count = self->tail - self->head;

	if ( count > 0 )
	{
		*task = self->entries[self->head&(self->size-1)];
		++self->head;

		return 1;
	}

	return 0;
}

int32_t queue_pops( struct queue * self, struct task * tasks, uint32_t count )
{
	int32_t i = 0;

	for ( i = 0; i < count; ++i )
	{
		if ( queue_pop( self, &(tasks[i]) ) == 0 )
		{
			break;
		}
	}

	return i;
}

uint32_t queue_count( struct queue * self )
{
	return self->tail - self->head;
}

int32_t queue_destroy( struct queue * self )
{
    if ( self->entries )
    {
        free( self->entries );
        self->entries = NULL;
    }

    self->size = 0;
    self->head = self->tail = 0;

    free(self);

    return 0;
}

// -----------------------------------------------------------------------------------------------------------------

struct msgqueue * msgqueue_create( uint32_t size )
{
    struct msgqueue * self = NULL;

    self = (struct msgqueue *)malloc( sizeof(struct msgqueue) );
    if ( self )
    {
        self->popfd = -1;
        self->pushfd = -1;
        pthread_mutex_init( &self->lock, 0 );
    
        self->queue = queue_create( size );
        if ( self->queue == NULL )
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

    rc = queue_push( self->queue, task );
    if ( isnotify != 0 )
    {
		isbc = queue_count( self->queue );
    }
    
    pthread_mutex_unlock( &self->lock );

    if ( rc == 0 && isbc == 1 )
    {
        char buf[1] = {0};
        int32_t nwrite = 0;
		
		nwrite = write( self->pushfd, buf, 1 );
		if ( nwrite != 1 )
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
    rc = queue_pop( self->queue, task );
    pthread_mutex_unlock( &self->lock );

    return rc;
}

int32_t msgqueue_pops( struct msgqueue * self, struct task * tasks, uint32_t count )
{
	int32_t rc = -1;

    pthread_mutex_lock( &self->lock );
    rc = queue_pops( self->queue, tasks, count );
    pthread_mutex_unlock( &self->lock );

    return rc;
}

uint32_t msgqueue_count( struct msgqueue * self )
{
	uint32_t rc = 0;
	
	pthread_mutex_lock( &self->lock );
	rc = queue_count( self->queue );
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
	if ( self->queue )
	{
		queue_destroy( self->queue );
		self->queue = NULL;
	}
	
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
	
	pthread_mutex_destroy( &self->lock );
	free( self );
	
	return 0;
}

