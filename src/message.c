
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

static inline void _align( struct buffer * self );
static inline size_t _offset( struct buffer * self );
static inline size_t _left( struct buffer * self );
static inline int32_t _expand( struct buffer * self, size_t length );
static inline ssize_t _read_withvector( struct buffer * self, int32_t fd );
static inline ssize_t _read_withsize( struct buffer * self, int32_t fd, ssize_t nbytes );

void _align( struct buffer * self )
{
    memmove( self->orignbuffer, self->buffer, self->length );
    self->buffer = self->orignbuffer;
}

size_t _offset( struct buffer * self )
{
    return (size_t)( self->buffer - self->orignbuffer );
}

size_t _left( struct buffer * self )
{
    return self->capacity - _offset( self ) - self->length;
}

int32_t _expand( struct buffer * self, size_t length )
{
    size_t offset = _offset( self );
    size_t needlength = offset + self->length + length;

    if ( needlength <= self->capacity ) {
        return 0;
    }

    if ( self->capacity - self->length >= length ) {
        _align( self );
    } else {
        void * newbuffer = NULL;
        size_t newcapacity = self->capacity;

        if ( newcapacity < MIN_BUFFER_LENGTH ) {
            newcapacity = MIN_BUFFER_LENGTH;
        }
        for ( ; newcapacity < needlength; ) {
            newcapacity <<= 1;
        }

        if ( self->orignbuffer != self->buffer ) {
            _align( self );
        }

        newbuffer = (char *)realloc( self->orignbuffer, newcapacity );
        if ( newbuffer == NULL ) {
            return -1;
        }

        self->capacity = newcapacity;
        self->orignbuffer = self->buffer = (char *)newbuffer;
    }

    return 0;
}

ssize_t _read_withvector( struct buffer * self, int32_t fd )
{
    struct iovec vec[2];
    char extra[MAX_BUFFER_LENGTH];

    ssize_t nread = 0;
    size_t left = _left( self );

    vec[0].iov_base = self->buffer + self->length;
    vec[0].iov_len = left;
    vec[1].iov_base = extra;
    vec[1].iov_len = sizeof( extra );

    nread = readv( fd, vec, left < sizeof( extra ) ? 2 : 1 );
    if ( nread > (ssize_t)left ) {
        self->length += left;
        int32_t rc = buffer_append( self, extra, (size_t)( nread - left ) );
        if ( rc != 0 ) {
            return -2;
        }
    } else if ( nread > 0 ) {
        self->length += nread;
    }

    return nread;
}

ssize_t _read_withsize( struct buffer * self, int32_t fd, ssize_t nbytes )
{
    ssize_t nread = -1;

    if ( nbytes == -1 ) {
        int32_t rc = ioctl( fd, FIONREAD, &nread );
        if ( rc == 0 && nread > 0 ) {
            nbytes = nread;
        } else {
            nbytes = MAX_BUFFER_LENGTH;
        }
    }

    if ( _expand( self, nbytes ) != 0 ) {
        return -2;
    }

    nread = read( fd, self->buffer + self->length, nbytes );
    if ( nread > 0 ) {
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

int32_t buffer_set( struct buffer * self, char * buf, size_t length )
{
    if ( self->orignbuffer ) {
        free( self->orignbuffer );
    }

    self->buffer = self->orignbuffer = buf;
    self->length = self->capacity = length;

    return 0;
}

int32_t buffer_erase( struct buffer * self, size_t length )
{
    if ( self->length <= length ) {
        self->length = 0;
        self->buffer = self->orignbuffer;
    } else {
        self->buffer += length;
        self->length -= length;
    }

    return 0;
}

int32_t buffer_append( struct buffer * self, const char * buf, size_t length )
{
    size_t offset = _offset( self );
    size_t needlength = offset + self->length + length;

    if ( needlength > self->capacity ) {
        if ( _expand( self, length ) == -1 ) {
            return -1;
        }
    }

    memcpy( self->buffer + self->length, buf, length );
    self->length += length;

    return 0;
}

int32_t buffer_append2( struct buffer * self, struct buffer * buffer )
{
    return buffer_append( self, buffer_data( buffer ), buffer_length( buffer ) );
}

size_t buffer_take( struct buffer * self, char * buf, size_t length )
{
    length = ( length > self->length ? self->length : length );

    memcpy( buf, self->buffer, length );
    buffer_erase( self, length );

    return length;
}

int32_t buffer_reserve( struct buffer * self, size_t length )
{
    return _expand( self, length );
}

void buffer_swap( struct buffer * buf1, struct buffer * buf2 )
{
    struct buffer tmpbuf = *buf1;

    *buf1 = *buf2;
    *buf2 = tmpbuf;
}

ssize_t buffer_read( struct buffer * self, int32_t fd, ssize_t nbytes )
{
    // 尽量读取SOCKET中的数据
    if ( likely( nbytes == 0 ) ) {
        return _read_withvector( self, fd );
    }

    // -1和>0 都是读取定长的数据到BUFF中
    return _read_withsize( self, fd, nbytes );
}

ssize_t buffer_receive( struct buffer * self, int32_t fd, struct sockaddr_storage * addr )
{
    ssize_t nread = MAX_BUFFER_LENGTH;
    socklen_t addrlen = sizeof( *addr );

    // Using FIONREAD, it is impossible to distinguish the case
    // where no datagram is pending from the case where the next pending datagram contains zero bytes of data

    if ( _expand( self, nread ) != 0 ) {
        return -2;
    }

    bzero( addr, addrlen );
    nread = recvfrom( fd,
        self->buffer + self->length,
        nread, MSG_DONTWAIT, (struct sockaddr *)addr, &addrlen );
    if ( nread > 0 ) {
        self->length += nread;
    }

    return nread;
}

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

struct message * message_create()
{
    struct message * self = (struct message *)malloc( sizeof( struct message ) );
    if ( self != NULL ) {
        self->nfailure = 0;
        self->nsuccess = 0;
        self->tolist = NULL;
        buffer_init( &self->buffer );
    }

    return self;
}

void message_destroy( struct message * self )
{
    if ( self->tolist ) {
        sidlist_destroy( self->tolist );
        self->tolist = NULL;
    }

    // if ( self->failurelist )
    // {
    //     sidlist_destroy( self->failurelist );
    //     self->failurelist = NULL;
    // }

    buffer_clear( &self->buffer );
    free( self );
}

int32_t message_add_receiver( struct message * self, sid_t id )
{
    if ( self->tolist == NULL ) {
        self->tolist = sidlist_create( 8 );
        if ( unlikely( self->tolist == NULL ) ) {
            return -1;
        }
    }

    return sidlist_add( self->tolist, id );
}

int32_t message_add_receivers( struct message * self, sid_t * ids, uint32_t count )
{
    if ( self->tolist == NULL ) {
        self->tolist = sidlist_create( count );
        if ( unlikely( self->tolist == NULL ) ) {
            return -1;
        }
    }

    return sidlist_adds( self->tolist, ids, count );
}

int32_t message_set_receivers( struct message * self, struct sidlist * ids )
{
    if ( self->tolist ) {
        sidlist_destroy( self->tolist );
    }

    self->tolist = ids;

    return 0;
}

int32_t message_reserve_receivers( struct message * self, uint32_t count )
{
    if ( self->tolist != NULL ) {
        count += sidlist_count( self->tolist );
    }

    struct sidlist * tolist = sidlist_create( count );
    if ( tolist == NULL ) {
        return -1;
    }

    if ( self->tolist != NULL ) {
        sidlist_append( tolist, self->tolist );
        sidlist_destroy( self->tolist );
    }

    self->tolist = tolist;
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
    int32_t totalcount = self->tolist ? sidlist_count( self->tolist ) : 0;
    return ( totalcount == self->nsuccess + self->nfailure );
}
