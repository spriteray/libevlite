
#ifndef LOCK_H
#define LOCK_H

#include <stdint.h>

#ifdef USE_ATOMIC

struct evlock {
    int32_t lock;
};

static inline void evlock_init( struct evlock * lock )
{
    lock->lock = 0;
}

static inline void evlock_lock( struct evlock * lock )
{
    for ( ; __sync_lock_test_and_set( &lock->lock, 1 ); ) usleep( 0 );
}

static inline void evlock_unlock( struct evlock * lock )
{
    __sync_lock_release( &lock->lock );
}

static inline void evlock_destroy( struct evlock * lock )
{}

#else

#include <pthread.h>

struct evlock {
    pthread_mutex_t lock;
};

static inline void evlock_init( struct evlock * lock )
{
    pthread_mutex_init( &lock->lock, NULL );
}

static inline void evlock_lock( struct evlock * lock )
{
    pthread_mutex_lock( &lock->lock );
}

static inline void evlock_unlock( struct evlock * lock )
{
    pthread_mutex_unlock( &lock->lock );
}

static inline void evlock_destroy( struct evlock * lock )
{
    pthread_mutex_destroy( &lock->lock );
}

#endif

#endif
