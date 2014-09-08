
#if defined(__FreeBSD__) || defined(__APPLE__) || defined(__darwin__) || defined(__OpenBSD__)

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

#include "utils.h"
#include "event-internal.h"

struct kqueuer
{
    int32_t nchanges;
    int32_t changessize;
    struct kevent * changes;

    int32_t nevents;
    struct kevent * events;

    int32_t kqueuefd;
};

void * kqueue_init();
int32_t kqueue_add( void * arg, struct event * ev );
int32_t kqueue_del( void * arg, struct event * ev );
int32_t kqueue_dispatch( struct eventset * sets, void * arg, int32_t tv );
void kqueue_final( void * arg );

int32_t kqueue_expand( struct kqueuer * self );
int32_t kqueue_insert( struct kqueuer * self, struct kevent * ev );

const struct eventop kqueueops = {
    kqueue_init,
    kqueue_add,
    kqueue_del,
    kqueue_dispatch,
    kqueue_final
};

#define EVSTATUS_X_KQINKERNEL   0x0100

void * kqueue_init()
{
    struct kqueuer * poller = NULL;

    poller = (struct kqueuer *)malloc( sizeof(struct kqueuer) );
    if ( poller == NULL )
    {
        return NULL;
    }

    poller->kqueuefd = kqueue();
    if ( poller->kqueuefd == -1 )
    {
        kqueue_final( poller );
        return NULL;
    }

    poller->events = (struct kevent *)calloc( INIT_EVENTS, sizeof(struct kevent) );
    poller->changes = (struct kevent *)calloc( INIT_EVENTS, sizeof(struct kevent) );
    if ( poller->events == NULL || poller->changes == NULL )
    {
        kqueue_final( poller );
        return NULL;
    }

    poller->nchanges = 0;
    poller->changessize = INIT_EVENTS;

    poller->nevents = INIT_EVENTS;

    return poller;
}

int32_t kqueue_expand( struct kqueuer * self )
{
    int32_t nevents = self->nevents;
    struct kevent * newevents = NULL;

    nevents <<= 1;

    newevents = realloc( self->events, nevents*sizeof(struct kevent) );
    if ( unlikely(newevents == NULL) )
    {
        return -1;
    }

    self->nevents = nevents;
    self->events = newevents;

    return 0;
}

int32_t kqueue_insert( struct kqueuer * self, struct kevent * ev )
{
    int32_t changessize = self->changessize;

    if ( self->nchanges == changessize )
    {
        struct kevent * newchanges = NULL;

        changessize <<= 1;

        newchanges = realloc( self->changes, changessize*sizeof(struct kevent) );
        if ( unlikely(newchanges == NULL) )
        {
            return -1;
        }

        self->changes = newchanges;
        self->changessize = changessize;
    }

    memcpy( &(self->changes[self->nchanges++]), ev, sizeof(struct kevent) );

    return 0;
}

int32_t kqueue_add( void * arg, struct event * ev )
{
    struct kevent kev;
    struct kqueuer * poller = (struct kqueuer *)arg;

    memset( &kev, 0, sizeof(kev) );

    if ( ev->events & EV_READ )
    {
        kev.flags = EV_ADD;
        kev.udata = (void *)ev;
        kev.filter = EVFILT_READ;
        kev.ident = event_get_fd((event_t)ev);
#ifdef NOTE_EOF
        kev.fflags = NOTE_EOF;
#endif
        if ( !(ev->events & EV_PERSIST) )
        {
            kev.flags |= EV_ONESHOT;
        }

        if ( kqueue_insert( poller, &kev ) != 0 )
        {
            return -1;
        }

        ev->status |= EVSTATUS_X_KQINKERNEL;
    }

    if ( ev->events & EV_WRITE )
    {
        kev.flags = EV_ADD;
        kev.udata = (void *)ev;
        kev.filter = EVFILT_WRITE;
        kev.ident = event_get_fd((event_t)ev);
        if ( !(ev->events & EV_PERSIST) )
        {
            kev.flags |= EV_ONESHOT;
        }

        if ( kqueue_insert( poller, &kev ) != 0 )
        {
            return -2;
        }

        ev->status |= EVSTATUS_X_KQINKERNEL;
    }

    return 0;
}

int32_t kqueue_del( void * arg, struct event * ev )
{
    struct kevent kev;
    struct kqueuer * poller = (struct kqueuer *)arg;

    if ( !(ev->status & EVSTATUS_X_KQINKERNEL) )
    {
        //
        // 该事件已经不在KQUEUE中了, 直接返回成功
        //
        return 0;
    }

    memset( &kev, 0, sizeof(kev) );

    if ( ev->events & EV_READ )
    {
        kev.flags = EV_DELETE;
        kev.filter = EVFILT_READ;
        kev.ident = event_get_fd((event_t)ev);

        if ( kqueue_insert( poller, &kev ) != 0 )
        {
            return -2;
        }

        ev->status &= ~EVSTATUS_X_KQINKERNEL;
    }

    if ( ev->events & EV_WRITE )
    {
        kev.flags = EV_DELETE;
        kev.filter = EVFILT_WRITE;
        kev.ident = event_get_fd((event_t)ev);

        if ( kqueue_insert( poller, &kev ) != 0 )
        {
            return -3;
        }

        ev->status &= ~EVSTATUS_X_KQINKERNEL;
    }

    return 0;
}

int32_t kqueue_dispatch( struct eventset * sets, void * arg, int32_t tv )
{
    struct timespec tsp, * ptsp = NULL ;

    int32_t res = -1, i = 0;
    struct kqueuer * poller = (struct kqueuer *)arg;

    if ( tv >= 0 )
    {
        // tv - 单位为毫秒
        // 必须换算成秒，以及纳秒
        tsp.tv_sec = tv/1000;
        tsp.tv_nsec = (tv%1000) * 1000000;
        ptsp = &tsp;
    }

    res = kevent( poller->kqueuefd, poller->changes,
            poller->nchanges, poller->events, poller->nevents, ptsp );
    poller->nchanges = 0;

    if ( res == -1 )
    {
        if ( errno != EINTR )
        {
            return -1;
        }

        return 0;
    }

    for ( i = 0; i < res; ++i )
    {
        int32_t which = 0;
        struct event * ev = NULL;

        if ( poller->events[i].flags & EV_ERROR )
        {
            if ( poller->events[i].data == EBADF
                    || poller->events[i].data == EINVAL
                    || poller->events[i].data == ENOENT )
            {
                continue;
            }

            errno = poller->events[i].data;
            return -2;
        }

        if ( poller->events[i].filter == EVFILT_READ )
        {
            which |= EV_READ;
        }
        else if ( poller->events[i].filter == EVFILT_WRITE )
        {
            which |= EV_WRITE;
        }

        if ( !which )
        {
            continue;
        }

        ev = (struct event *)( poller->events[i].udata );
        if ( !(ev->events & EV_PERSIST) )
        {
            ev->status &= ~EVSTATUS_X_KQINKERNEL;
        }

        event_active( ev, which );
    }

    if ( res == poller->nevents )
    {
        kqueue_expand( poller );
    }

    return res;
}

void kqueue_final( void * arg )
{
    struct kqueuer * poller = (struct kqueuer *)arg;

    if ( poller->kqueuefd >= 0 )
    {
        close( poller->kqueuefd );
    }
    if ( poller->changes )
    {
        free( poller->changes );
    }
    if ( poller->events )
    {
        free( poller->events );
    }

    free( poller );
    return ;
}

#endif
