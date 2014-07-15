

//
// benchmark :
//
// FreeBSD 7.4
// Xeon E5410 * 1, 8G
//
// evsets_add(1000000) : 0 secs, 63969 usecs .
// evsets_del(1000000) : 0 secs, 57534 usecs .
// --------------------------------------------------------
// libevent_add(1000000) : 0 secs, 310279 usecs .
// libevent_del(1000000) : 0 secs, 490534 usecs .
//

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#include "event.h"

#define NTEST   1000000

void ev_callback( int32_t fd, int16_t ev, void * arg )
{
    return;
}

void ev_timer_callback( int32_t fd, int16_t ev, void * arg )
{
    event_t e = (event_t)arg;
    evsets_t sets = event_get_sets( e );

    printf("ev_timer_callback(handler=%p, fd=%d, ev=%d) : %d \n", e, fd, ev, time(NULL) );

    evsets_add( sets, e, 2*1000 );

    return;
}

int32_t test_addtimer( evsets_t sets, event_t * events )
{
    int32_t i = 0;
    struct timeval tv_start, tv_end;

    gettimeofday( &tv_start, NULL );

    for ( i = 0; i < NTEST; ++i )
    {
        evsets_add( sets, events[i], 10000 );
    }

    gettimeofday( &tv_end, NULL );

    printf("evsets_add(%d) : %ld secs, %ld usecs .\n", NTEST,
        tv_end.tv_sec - tv_start.tv_sec, tv_end.tv_usec - tv_start.tv_usec );

    return 0;
}

int32_t test_deltimer( evsets_t sets, event_t * events )
{
    int32_t i = 0;
    struct timeval tv_start, tv_end;

    gettimeofday( &tv_start, NULL );

    for ( i = 0; i < NTEST; ++i )
    {
        evsets_del( sets, events[i] );
    }

    gettimeofday( &tv_end, NULL );

    printf("evsets_del(%d) : %ld secs, %ld usecs .\n", NTEST,
        tv_end.tv_sec - tv_start.tv_sec, tv_end.tv_usec - tv_start.tv_usec );

    return 0;
}

int32_t test_operate_timer()
{
    int32_t i = 0;

    evsets_t sets = NULL;
    event_t * events = NULL;

    sets = evsets_create();
    events = malloc( sizeof(event_t) * NTEST );

    for ( i = 0; i < NTEST; ++i )
    {
        events[i] = event_create();
        event_set_callback( events[i], ev_callback, NULL );
    }

    test_addtimer( sets, events );
    test_deltimer( sets, events );

    for ( i = 0; i < NTEST; ++i )
    {
        event_destroy( events[i] );
    }
    free( events );

    evsets_destroy( sets );

    return 0;
}

int32_t test_evtimer()
{
    evsets_t sets = NULL;
    event_t ev_timer = NULL;

    sets = evsets_create();

    ev_timer = event_create();
    event_set( ev_timer, -1, 0 );
    event_set_callback( ev_timer, ev_timer_callback, ev_timer );

    evsets_add( sets, ev_timer, 2*1000 );

    while( 1 )
    {
        evsets_dispatch( sets );
    }

    event_destroy( ev_timer );
    evsets_destroy( sets );

    return 0;
}

int main()
{
    test_operate_timer();
    test_evtimer();

    return 0;
}
