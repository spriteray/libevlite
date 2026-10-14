
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>

#include "utils.h"
#include "addrtable.h"

#define INVOKE( self, method, entry ) \
    if ( ( self )->helper != NULL ) { ( self )->helper( ( method ), ( entry ) ); }

static inline uint32_t _hash_function( const void * key, int32_t len, uint32_t seed )
{
    /* 'm' and 'r' are mixing constants generated offline.
     *      They're not really 'magic', they just happen to work well.  */
    const int r = 24;
    const uint32_t m = 0x5bd1e995;

    /* Initialize the hash to a 'random' value */
    uint32_t h = seed ^ len;

    /* Mix 4 bytes at a time into the hash */
    const unsigned char * data = (const unsigned char *)key;

    while ( len >= 4 ) {
        uint32_t k;
        memcpy( &k, data, sizeof( k ) );

        k *= m;
        k ^= k >> r;
        k *= m;

        h *= m;
        h ^= k;

        data += 4;
        len -= 4;
    }

    /* Handle the last few bytes of the input array  */
    switch ( len ) {
        case 3 : h ^= data[2] << 16;
        case 2 : h ^= data[1] << 8;
        case 1 : h ^= data[0]; h *= m;
    };

    /* Do a few final mixes of the hash to ensure the last few
     *      * bytes are well-incorporated. */
    h ^= h >> 13;
    h *= m;
    h ^= h >> 15;

    return h;
}

static inline void _make_key( const struct sockaddr_storage * addr, struct epkey * key )
{
    memset( key, 0, sizeof( struct epkey ) );
    key->family = addr->ss_family;
    if ( addr->ss_family == AF_INET ) {
        const struct sockaddr_in * a4 = (const struct sockaddr_in *)addr;
        key->port = a4->sin_port;
        memcpy( key->addr, &a4->sin_addr, 4 );
    } else if ( addr->ss_family == AF_INET6 ) {
        const struct sockaddr_in6 * a6 = (const struct sockaddr_in6 *)addr;
        key->port = a6->sin6_port;
        memcpy( key->addr, &a6->sin6_addr, 16 );
    }
}

static uint32_t _get_hashkey( const struct epkey * key )
{
    return _hash_function( key, sizeof( struct epkey ), 5381U );
}

struct addresstable * addrtable_create( size_t count, size_t size, helper_t helper )
{
    struct addresstable * q = NULL;

    q = (struct addresstable *)calloc( 1, sizeof( struct addresstable ) );
    assert( q != NULL && "create struct addresstable failed" );

    q->size = nextpow2( count );
    q->sizemask = q->size - 1;
    q->count = 0;
    q->objsize = size;
    q->helper = helper;
    q->table = (struct endpoint **)calloc( q->size, sizeof( struct endpoint * ) );
    assert( q->table != NULL && "create struct udpentry * failed" );

    return q;
}

void addrtable_destroy( struct addresstable * self )
{
    if ( self->table != NULL ) {
        for ( size_t i = 0; i < self->size; ++i ) {
            struct endpoint * next = NULL;
            struct endpoint * head = self->table[i];

            while ( head != NULL ) {
                INVOKE( self, 3, head );
                next = head->next;
                free( head ); head = next;
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

void * addrtable_find( struct addresstable * self, const struct sockaddr_storage * addr )
{
    if ( self->count == 0 ) {
        return NULL;
    }

    struct epkey key;
    _make_key( addr, &key );
    struct endpoint * head = self->table[_get_hashkey( &key ) & self->sizemask];
    while ( head != NULL ) {
        if ( memcmp( &key, &head->key, sizeof( key ) ) == 0 ) {
            return head->value;
        }

        head = head->next;
    }

    return NULL;
}

void addrtable_remove( struct addresstable * self, const struct sockaddr_storage * addr )
{
    if ( self->count == 0 ) {
        return;
    }

    struct epkey key;
    _make_key( addr, &key );
    uint32_t index = _get_hashkey( &key ) & self->sizemask;

    struct endpoint * prev = NULL;
    struct endpoint * head = self->table[index];
    while ( head != NULL ) {
        if ( memcmp( &key, &head->key, sizeof( key ) ) == 0 ) {
            if ( prev != NULL ) {
                prev->next = head->next;
            } else {
                self->table[index] = head->next;
            }

            INVOKE( self, 2, head );
            free( head ); --self->count; return;
        }

        prev = head;
        head = head->next;
    }
}

void * addrtable_append( struct addresstable * self, const struct sockaddr_storage * addr )
{
    struct epkey key;
    _make_key( addr, &key );
    uint32_t index = _get_hashkey( &key ) & self->sizemask;

    struct endpoint * entry = (struct endpoint *)calloc(
        1, sizeof( struct endpoint ) + self->objsize );
    assert( entry != NULL && "create struct udpentry failed" );

    entry->key = key;
    if ( self->objsize > 0 ) {
        entry->value = entry + 1;
    }
    entry->next = self->table[index];
    ++self->count;
    self->table[index] = entry;
    INVOKE( self, 1, entry );

    return entry->value;
}

