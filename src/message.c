
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include "utils.h"
#include "network-internal.h"
#include "message.h"

#define MIN_BUFFER_LENGTH       128

static inline void _align( struct buffer * self );
static inline uint32_t _offset( struct buffer * self );
static inline int32_t _expand( struct buffer * self, uint32_t length );
static inline int32_t _read_withvector( struct buffer * self, int32_t fd  );
static inline int32_t _read_withsize( struct buffer * self, int32_t fd, int32_t nbytes );

void _align( struct buffer * self )
{
    memmove( self->orignbuffer, self->buffer, self->length );
    self->buffer = self->orignbuffer;
}

uint32_t _offset( struct buffer * self )
{
    return (uint32_t)( self->buffer - self->orignbuffer );
}

uint32_t _left( struct buffer * self )
{
    return (uint32_t)( self->capacity - _offset(self) - self->length );
}

int32_t _expand( struct buffer * self, uint32_t length )
{
    int32_t offset = _offset( self );
    uint32_t needlength = offset + self->length + length;

    if ( needlength <= self->capacity )
    {
        return 0;
    }

    if ( self->capacity - self->length >= length )
    {
        _align( self );
    }
    else
    {
        void * newbuffer = NULL;
        uint32_t newcapacity = self->capacity;

        if ( newcapacity < MIN_BUFFER_LENGTH )
        {
            newcapacity = MIN_BUFFER_LENGTH;
        }
        for ( ; newcapacity < needlength; )
        {
            newcapacity <<= 1;
        }

        if ( self->orignbuffer != self->buffer )
        {
            _align( self );
        }

        newbuffer = (char *)realloc( self->orignbuffer, newcapacity );
        if ( newbuffer == NULL )
        {
            return -1;
        }

        self->capacity = newcapacity;
        self->orignbuffer = self->buffer = newbuffer;
    }

    return 0;
}

int32_t _read_withvector( struct buffer * self, int32_t fd  )
{
    struct iovec vec[ 2 ];
    char extra[ RECV_BUFFER_SIZE ];
    int32_t nread = 0, left = _left(self);

    vec[0].iov_base = self->buffer + self->length;
    vec[0].iov_len = left;
    vec[1].iov_base = extra;
    vec[1].iov_len = sizeof( extra );

    nread = (int32_t)readv( fd, vec, left < sizeof(extra) ? 2 : 1 );
    if ( nread > left )
    {
        self->length += left;
        buffer_append( self, extra, (uint32_t)(nread-left) );
    }
    else if ( nread > 0 )
    {
        self->length += nread;
    }

    return nread;
}

int32_t _read_withsize( struct buffer * self, int32_t fd, int32_t nbytes )
{
    int32_t nread = -1;

    if ( nbytes == -1 )
    {
        int32_t rc = ioctl( fd, FIONREAD, &nread );
        if ( rc == -1 || nread == 0 )
        {
            nbytes = RECV_BUFFER_SIZE;
        }
        else
        {
            nbytes = nread;
        }
    }

    if ( _expand( self, nbytes ) != 0 )
    {
        return -2;
    }

    nread = (int32_t)read( fd, self->buffer+self->length, nbytes );
    if ( nread > 0 )
    {
        self->length += nread;
    }

    return nread;
}

int32_t buffer_init( struct buffer * self )
{
    self->length = 0;
    self->capacity = 0;
    self->buffer = NULL;
    self->orignbuffer = NULL;
    return 0;
}

int32_t buffer_set( struct buffer * self, char * buf, uint32_t length )
{
    if ( self->orignbuffer )
    {
        free( self->orignbuffer );
    }

    self->buffer = self->orignbuffer = buf;
    self->length = self->capacity = length;

    return 0;
}

int32_t buffer_erase( struct buffer * self, uint32_t length )
{
    if ( self->length <= length )
    {
        self->length = 0;
        self->buffer = self->orignbuffer;
    }
    else
    {
        self->buffer += length;
        self->length -= length;
    }

    return 0;
}

int32_t buffer_append( struct buffer * self, char * buf, uint32_t length )
{
    uint32_t offset = _offset(self);
    uint32_t needlength = offset + self->length + length;

    if ( needlength > self->capacity )
    {
        if ( _expand(self, length) == -1 )
        {
            return -1;
        }
    }

    memcpy( self->buffer+self->length, buf, length );
    self->length += length;

    return 0;
}

uint32_t buffer_take( struct buffer * self, char * buf, uint32_t length )
{
    length = ( length > self->length ? self->length : length );

    memcpy( buf, self->buffer, length );
    buffer_erase( self, length );

    return length;
}

void buffer_swap( struct buffer * buf1, struct buffer * buf2 )
{
    struct buffer tmpbuf = *buf1;

    *buf1 = *buf2;
    *buf2 = tmpbuf;
}

int32_t buffer_read( struct buffer * self, int32_t fd, int32_t nbytes )
{
    // 尽量读取SOCKET中的数据
    if ( likely( nbytes == 0 ) )
    {
        return _read_withvector( self, fd );
    }

    // -1和>0 都是读取定长的数据到BUFF中
    return _read_withsize( self, fd, nbytes );
}

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

struct message * message_create()
{
    struct message * self = (struct message *)malloc( sizeof(struct message) );
    if ( self != NULL )
    {
        self->nfailure = 0;
        self->nsuccess = 0;
        self->tolist = NULL;
        buffer_init( &self->buffer );
    }

    return self;
}

void message_destroy( struct message * self )
{
    if ( self->tolist )
    {
        sidlist_destroy( self->tolist );
        self->tolist = NULL;
    }

//     if ( self->failurelist )
//     {
//         sidlist_destroy( self->failurelist );
//         self->failurelist = NULL;
//     }

    buffer_clear( &self->buffer );
    free( self );
}

int32_t message_add_receiver( struct message * self, sid_t id )
{
    if ( self->tolist == NULL )
    {
        self->tolist = sidlist_create(8);
        if ( unlikely(self->tolist == NULL) )
        {
            return -1;
        }
    }

    return sidlist_add( self->tolist, id );
}

int32_t message_add_receivers( struct message * self, sid_t * ids, uint32_t count )
{
    if ( self->tolist == NULL )
    {
        self->tolist = sidlist_create(count);
        if ( unlikely(self->tolist == NULL) )
        {
            return -1;
        }
    }

    return sidlist_adds( self->tolist, ids, count );
}

int32_t message_set_receivers( struct message * self, struct sidlist * ids )
{
    if ( self->tolist )
    {
        sidlist_destroy( self->tolist );
    }

    self->tolist = ids;

    return 0;
}

// int32_t message_add_failure( struct message * self, sid_t id )
// {
//     if ( self->failurelist == NULL )
//     {
//         self->failurelist = sidlist_create(8);
//         if ( self->failurelist == NULL )
//         {
//             return -1;
//         }
//     }
//
//     return sidlist_add( self->failurelist, id );
// }
//
// int32_t message_is_complete( struct message * self )
// {
//     int32_t totalcount = self->tolist ? sidlist_count(self->tolist) : 0;
//     int32_t failurecount = self->failurelist ? sidlist_count( self->failurelist ) : 0;
//
//     return (totalcount == self->nsuccess - failurecount);
// }

int32_t message_is_complete( struct message * self )
{
    int32_t totalcount = self->tolist ? sidlist_count(self->tolist) : 0;
    return ( totalcount == self->nsuccess + self->nfailure );
}
