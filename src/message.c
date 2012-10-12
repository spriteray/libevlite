

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include "network-internal.h"

#include "message.h"

static void _align( struct buffer * self );
static int32_t _expand( struct buffer * self, uint32_t length );

void _align( struct buffer * self )
{
	memmove( self->orignbuffer, self->buffer, self->length );

	self->buffer = self->orignbuffer;
	self->length = 0;
}

int32_t _expand( struct buffer * self, uint32_t length )
{
	uint32_t needlength = self->offset + self->length + length;

	if ( needlength <= self->totallen )
	{
		return 0;
	}

	if ( self->totallen - self->length >= length )
	{
		_align( self );
	}
	else
	{
		void * newbuffer = NULL;
		uint32_t newlength = self->totallen;

		if ( newlength < 64 )
		{
			newlength = 64;
		}
		for ( ; newlength < needlength; )
		{
			newlength <<= 1;
		}

		if ( self->orignbuffer != self->buffer )
		{
			_align( self );
		}

		newbuffer = (char *)realloc( self->orignbuffer, newlength );
		if ( newbuffer == NULL )
		{
			return -1;
		}

		self->totallen = newlength;
		self->orignbuffer = self->buffer = newbuffer;
	}

	return 0;
}

struct buffer * buffer_create()
{
	struct buffer * buffer = (struct buffer *)malloc(sizeof(struct buffer));
	if ( buffer )
	{
		buffer->buffer		= NULL;
		buffer->orignbuffer	= NULL;
		buffer->offset		= 0;
		buffer->length		= 0;
		buffer->totallen	= 0;
	}
	
	return buffer;
}

void buffer_destroy( struct buffer * self )
{
	if ( self->orignbuffer )
	{
		free( self->orignbuffer );
		self->orignbuffer = NULL;
	}
	
	self->buffer = NULL;
	self->offset = self->length = self->totallen = 0;
	
	free( self );

	return;
}

int32_t buffer_set( struct buffer * self, char * buf, uint32_t length )
{
	if ( self->orignbuffer )
	{
		free( self->orignbuffer );
	}
	
	self->offset = 0;
	self->buffer = self->orignbuffer = buf;
	self->length = self->totallen = length;
	
	return 0;
}

uint32_t buffer_length( struct buffer * self )
{
	return self->length;
}

char * buffer_data( struct buffer * self )
{
	return self->buffer;
}

int32_t buffer_erase( struct buffer * self, uint32_t length )
{
	if ( self->length <= length )
	{
		self->length = 0;
		self->offset = 0;
		self->buffer = self->orignbuffer;
	}
	else
	{
		self->buffer += length;
		self->offset += length;
		self->length -= length;
	}
	
	return 0;
}

int32_t buffer_append( struct buffer * self, char * buf, uint32_t length )
{
	uint32_t needlength = self->offset + self->length + length;
	
	if ( needlength >= self->totallen )
	{
		if ( _expand(self,  length) == -1 )
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

void buffer_exchange( struct buffer * buf1, struct buffer * buf2 )
{
	struct buffer tmpbuf;
	
	memcpy( &tmpbuf, buf1, sizeof(struct buffer) );
	memcpy( buf1, buf2, sizeof(struct buffer) );
	memcpy( &buf2, &tmpbuf, sizeof(struct buffer) );
	
	return;
}

int32_t buffer_read( struct buffer * self, int32_t fd, int32_t nbytes )
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

	nread = read( fd, self->buffer+self->length, nbytes );
	if ( nread > 0 )
	{
		self->length += nread;
	}

	return nread;
}

// =============================================================================
// =============================================================================
// =============================================================================

struct message * message_create()
{
	return (struct message *)calloc( 1, sizeof(struct message) );
}

void message_destroy( struct message * self )
{
	if ( self->tolist )
	{
		sidlist_destroy( self->tolist );
		self->tolist = NULL;
	}

	if ( self->failurelist )
	{
		sidlist_destroy( self->failurelist );
		self->failurelist = NULL;
	}

	buffer_set( &self->buffer, NULL, 0 );

	free( self );
	return;
}

int32_t message_add_receiver( struct message * self, sid_t id )
{
	if ( self->tolist == NULL )
	{
		self->tolist = sidlist_create(8);
		if ( self->tolist == NULL )
		{
			return -1;
		}
	}

	return sidlist_add( self->tolist, id );
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

int32_t message_add_failure( struct message * self, sid_t id )
{
	if ( self->failurelist == NULL )
	{
		self->failurelist = sidlist_create(8);
		if ( self->failurelist == NULL )
		{
			return -1;
		}
	}

	return sidlist_add( self->failurelist, id );
}

int32_t message_left_count( struct message * self )
{
	int32_t totalcount = 0;
	int32_t failurecount = 0;

	if ( self->tolist )
	{
		totalcount = sidlist_count( self->tolist );
	}

	if ( self->failurelist )
	{
		failurecount = sidlist_count( self->failurelist );
	}

	return totalcount-self->nsuccess-failurecount;
}

