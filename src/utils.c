
#include <assert.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pthread.h>
#include <sys/socket.h>
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
	uint32_t npower = 0;
	struct queue * self = NULL;

	size = size ? size : 8;
	size = nextpow2( size );
	npower = getpower( size );

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
        pthread_spin_init( &self->lock, 0 );
    
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

            rc = socketpair( AF_UNIX, SOCK_STREAM, 0, fds );
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

    pthread_spin_lock( &self->lock );

    rc = queue_push( self->queue, task );
    if ( isnotify != 0 )
    {
		isbc = queue_count( self->queue );
    }
    
    pthread_spin_unlock( &self->lock );

    if ( rc == 0 && isbc == 1 )
    {
        char buf[1] = {0};
        write( self->pushfd, buf, 1 );
    }

    return rc;
}

int32_t msgqueue_pop( struct msgqueue * self, struct task * task )
{
    int32_t rc = -1;

    pthread_spin_lock( &self->lock );
    rc = queue_pop( self->queue, task );
    pthread_spin_unlock( &self->lock );

    return rc;
}

int32_t msgqueue_pops( struct msgqueue * self, struct task * tasks, uint32_t count )
{
	int32_t rc = -1;

    pthread_spin_lock( &self->lock );
    rc = queue_pops( self->queue, tasks, count );
    pthread_spin_unlock( &self->lock );

    return rc;
}

uint32_t msgqueue_count( struct msgqueue * self )
{
	uint32_t rc = 0;
	
	pthread_spin_lock( &self->lock );
	rc = queue_count( self->queue );
	pthread_spin_unlock( &self->lock );
	
	return rc;
}

int32_t msgqueue_popfd( struct msgqueue * self )
{
	int32_t rc = 0;
	
	pthread_spin_lock( &self->lock );
	rc = self->popfd;
	pthread_spin_unlock( &self->lock );
	
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
	
	pthread_spin_destroy( &self->lock );
	free( self );
	
	return 0;
}

