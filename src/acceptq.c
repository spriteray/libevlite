
#include <stdio.h>

#include "iolayer.h"
#include "acceptq.h"

uint32_t _hash_function( const void * key, int32_t len )
{
    /* 'm' and 'r' are mixing constants generated offline.
     *      They're not really 'magic', they just happen to work well.  */
    const int r = 24;
    static uint32_t seed = 5381;
    const uint32_t m = 0x5bd1e995;

    /* Initialize the hash to a 'random' value */
    uint32_t h = seed ^ len;

    /* Mix 4 bytes at a time into the hash */
    const unsigned char *data = (const unsigned char *)key;

    while ( len >= 4 )
    {
        uint32_t k = *(uint32_t*)data;

        k *= m;
        k ^= k >> r;
        k *= m;

        h *= m;
        h ^= k;

        data += 4;
        len -= 4;
    }

    /* Handle the last few bytes of the input array  */
    switch ( len )
    {
        case 3: h ^= data[2] << 16;
        case 2: h ^= data[1] << 8;
        case 1: h ^= data[0]; h *= m;
    };

    /* Do a few final mixes of the hash to ensure the last few
     *      * bytes are well-incorporated. */
    h ^= h >> 13;
    h *= m;
    h ^= h >> 15;

    return h;
}

void _udpentry_destroy( struct udpentry * entry )
{
    buffer_clear( &entry->buffer );

    if ( entry->event != NULL )
    {
        evsets_del( entry->acceptor->evsets, entry->event );
        event_destroy( entry->event );
        entry->event = NULL;
    }

    free( entry );
}

struct acceptqueue * acceptqueue_create( size_t count )
{
    struct acceptqueue * q = NULL;

    q = (struct acceptqueue *)calloc( 1, sizeof(struct acceptqueue) );
    assert( q != NULL && "create struct acceptqueue failed" );

    q->size = nextpow2( count );
    q->sizemask = q->size - 1;
    q->count = 0;
    q->table = (struct udpentry **)calloc( q->size, sizeof(struct udpentry *) );
    assert( q->table != NULL && "create struct udpentry * failed" );

    return q;
}

void acceptqueue_destroy( struct acceptqueue * self )
{
    size_t i = 0;

    if ( self->table != NULL )
    {
        for ( i = 0; i < self->size; ++i )
        {
            struct udpentry * next = NULL;
            struct udpentry * head = self->table[i];

            while ( head != NULL )
            {
                next = head->next;
                _udpentry_destroy( head );
                head = next;
                --self->count;
            }
        }

        self->table = NULL;
    }

    self->count = 0;
    self->size = 0;
    self->sizemask = 0;

    free( self );
    self = NULL;
}

struct udpentry * acceptqueue_find( struct acceptqueue * self, const char * host, uint16_t port )
{
    if ( self->count == 0 )
    {
        return NULL;
    }

    char key[ 64 ];
    size_t len = 0;
    struct udpentry * head = NULL;

    len = snprintf( key, 63,
            "%s::%d", host, port );
    head = self->table[ _hash_function( key, len ) & self->sizemask ];
    while ( head != NULL )
    {
        if ( port == head->port
                && strncmp( host, head->host, 64 ) == 0 )
        {
            return head;
        }

        head = head->next;
    }

    return NULL;
}

void acceptqueue_remove( struct acceptqueue * self, const char * host, uint16_t port )
{
    if ( self->count == 0 )
    {
        return;
    }

    char key[ 64 ];
    size_t len = 0;
    uint32_t index = 0;
    struct udpentry * head = NULL, * prev = NULL;

    len = snprintf( key, 63,
            "%s::%d", host, port );
    index = _hash_function( key, len ) & self->sizemask;
    head = self->table[ index ];
    while ( head != NULL )
    {
        if ( port == head->port
                && strncmp( host, head->host, 64 ) == 0 )
        {
            if ( prev != NULL )
                prev->next = head->next;
            else
                self->table[ index ] = head->next;

            _udpentry_destroy( head );
            --self->count;
            return;
        }

        prev = head;
        head = head->next;
    }
}

struct udpentry * acceptqueue_append( struct acceptqueue * self, const char * host, uint16_t port )
{
    char key[ 64 ];
    size_t len = 0, index;
    struct udpentry * entry = NULL;

    len = snprintf( key, 63,
            "%s::%d", host, port );
    index = _hash_function( key, len ) & self->sizemask;

    entry = (struct udpentry *)calloc( 1, sizeof(*entry) );
    assert( entry != NULL && "create struct udpentry failed" );
    strcpy( entry->host, host );
    buffer_init( &entry->buffer );
    entry->port = port;
    entry->next = self->table[ index ];
    ++self->count;
    self->table[ index ] = entry;

    return entry;
}
