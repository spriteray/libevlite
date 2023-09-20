
#include "utils.h"
#include "ephashtable.h"

static inline uint32_t _hash_function( const void * key, int32_t len )
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

struct ephashtable * ephashtable_create( size_t count, size_t size, helper_t helper )
{
    struct ephashtable * q = NULL;

    q = (struct ephashtable *)calloc( 1, sizeof(struct ephashtable) );
    assert( q != NULL && "create struct ephashtable failed" );

    q->size = nextpow2( count );
    q->sizemask = q->size - 1;
    q->count = 0;
    q->objsize = size;
    q->helper = helper;
    q->table = (struct endpoint **)calloc( q->size, sizeof(struct endpoint *) );
    assert( q->table != NULL && "create struct udpentry * failed" );

    return q;
}

void ephashtable_destroy( struct ephashtable * self )
{
    size_t i = 0;

    if ( self->table != NULL )
    {
        for ( i = 0; i < self->size; ++i )
        {
            struct endpoint * next = NULL;
            struct endpoint * head = self->table[i];

            while ( head != NULL )
            {
                next = head->next;
                self->helper( 2, head );
                head = next;
                --self->count;
            }
        }

        free( self->table );
        self->table = NULL;
    }

    self->count = 0;
    self->size = 0;
    self->sizemask = 0;

    free( self );
    self = NULL;
}

void * ephashtable_find( struct ephashtable * self, const char * host, uint16_t port )
{
    if ( self->count == 0 )
    {
        return NULL;
    }

    char key[ 128 ];
    size_t len = 0;
    struct endpoint * head = NULL;

    len = snprintf( key, 127,
            "%s::%d", host, port );
    head = self->table[ _hash_function( key, len ) & self->sizemask ];
    while ( head != NULL )
    {
        if ( port == head->port
                && strcmp( host, head->host ) == 0 )
        {
            return head->value;
        }

        head = head->next;
    }

    return NULL;
}

void ephashtable_remove( struct ephashtable * self, const char * host, uint16_t port )
{
    if ( self->count == 0 )
    {
        return;
    }

    char key[ 128 ];
    size_t len = 0;
    uint32_t index = 0;
    struct endpoint * head = NULL, * prev = NULL;

    len = snprintf( key, 127,
            "%s::%d", host, port );
    index = _hash_function( key, len ) & self->sizemask;
    head = self->table[ index ];
    while ( head != NULL )
    {
        if ( port == head->port
                && strcmp( host, head->host ) == 0 )
        {
            if ( prev != NULL )
            {
                prev->next = head->next;
            }
            else
            {
                self->table[ index ] = head->next;
            }

            self->helper( 2, head );
            free( head );
            --self->count;
            return;
        }

        prev = head;
        head = head->next;
    }
}

void * ephashtable_append( struct ephashtable * self, const char * host, uint16_t port )
{
    char key[ 128 ];
    size_t len = 0, index;
    struct endpoint * entry = NULL;

    len = snprintf( key, 127,
            "%s::%d", host, port );
    index = _hash_function( key, len ) & self->sizemask;

    entry = (struct endpoint *)calloc( 1, sizeof(struct endpoint) + self->objsize );
    assert( entry != NULL && "create struct udpentry failed" );
    strcpy( entry->host, host );
    if ( self->objsize > 0 )
    {
        entry->value = entry + 1;
    }
    entry->port = port;
    entry->next = self->table[ index ];
    ++self->count;
    self->table[ index ] = entry;
    self->helper( 1, entry );

    return entry->value;
}
