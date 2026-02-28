
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include "utils.h"
#include "sidlist.h"

struct sidlist * sidlist_create( uint32_t size )
{
    struct sidlist * self = NULL;

    size = size ? size : 8;
    self = (struct sidlist *)malloc( sizeof( struct sidlist ) );
    if ( self ) {
        self->count = 0;
        self->size = size;
        self->entries = (sid_t *)malloc( self->size * sizeof( sid_t ) );
        if ( unlikely( self->entries == NULL ) ) {
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

    id = index == -1 ? self->count - 1 : index;
    if ( id < self->count ) {
        sid = self->entries[id];
    }

    return sid;
}

int32_t sidlist_add( struct sidlist * self, sid_t id )
{
    if ( self->count + 1 > self->size ) {
        self->size <<= 1;

        self->entries = (sid_t *)realloc(
            self->entries, sizeof( sid_t ) * self->size );
        assert( self->entries != NULL );
    }

    self->entries[self->count++] = id;

    return 0;
}

int32_t sidlist_adds( struct sidlist * self, sid_t * ids, uint32_t count )
{
    uint32_t totalcount = self->count + count;

    if ( totalcount > self->size ) {
        self->size = totalcount;

        self->entries = (sid_t *)realloc(
            self->entries, sizeof( sid_t ) * self->size );
        assert( self->entries != NULL );
    }

    memcpy( self->entries + self->count, ids, count * sizeof( sid_t ) );
    self->count = totalcount;

    return 0;
}

sid_t sidlist_del( struct sidlist * self, int32_t index )
{
    sid_t rc = 0;
    uint32_t id = 0;

    id = index == -1 ? self->count - 1 : index;
    if ( id < self->count ) {
        --self->count;
        rc = self->entries[id];
        if ( id != self->count ) {
            self->entries[id] = self->entries[self->count];
        }
    }

    return rc;
}

void sidlist_destroy( struct sidlist * self )
{
    if ( self->entries ) {
        free( self->entries );
        self->entries = NULL;
    }

    free( self );
}

