
#include "config.h"

#if defined EVENT_OS_LINUX

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <syslog.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include <sys/time.h>
#include <sys/epoll.h>
#include <sys/resource.h>

#include "utils.h"
#include "event-internal.h"

//
// epoll关注的是描述符, 而非读写事件
// 为了模拟读写事件, 所以增加了一个事件对
//
struct eventpair {
    struct event * evread;
    struct event * evwrite;
};

struct epoller {
    // 管理的所有事件对, 这个是基于描述符的
    int32_t npairs;
    struct eventpair * evpairs;

    // 管理固定数目的激活事件
    int32_t nevents;
    struct epoll_event * events;

    // epoll描述符
    int32_t epollfd;
};

void * epoll_init();
int32_t epoll_add( void * arg, struct event * ev );
int32_t epoll_del( void * arg, struct event * ev );
int32_t epoll_dispatch( struct eventset * sets, void * arg, int32_t tv );
void epoll_final( void * arg );

int32_t epoll_expand( struct epoller * self );
int32_t epoll_insert( struct epoller * self, int32_t max );

const struct eventop epollops = {
    epoll_init,
    epoll_add,
    epoll_del,
    epoll_dispatch,
    epoll_final
};

#define MAX_EPOLL_WAIT 35 * 60 * 1000

void * epoll_init()
{
    int32_t epollfd = -1;
    struct epoller * poller = NULL;

#if defined EVENT_HAVE_EPOLLCREATE1
    epollfd = epoll_create1( EPOLL_CLOEXEC );
#endif
    if ( epollfd == -1 ) {
        epollfd = epoll_create( 64000 ); // 该参数在新内核中已经取消
        if ( epollfd == -1 ) {
            return NULL;
        }
        // CLOSEONEXEC
        set_cloexec( epollfd );
    }

    poller = (struct epoller *)malloc( sizeof( struct epoller ) );
    if ( poller == NULL ) {
        close( epollfd );
        return NULL;
    }

    poller->epollfd = epollfd;

    poller->evpairs = (struct eventpair *)calloc( INIT_EVENTS, sizeof( struct eventpair ) );
    poller->events = (struct epoll_event *)calloc( INIT_EVENTS, sizeof( struct epoll_event ) );
    if ( poller->evpairs == NULL || poller->events == NULL ) {
        epoll_final( poller );
        return NULL;
    }

    poller->npairs = INIT_EVENTS;
    poller->nevents = INIT_EVENTS;

    return poller;
}

int32_t epoll_expand( struct epoller * self )
{
    int32_t nevents = self->nevents;
    struct epoll_event * newevents = NULL;

    nevents <<= 1;

    newevents = (struct epoll_event*)realloc(
        self->events, nevents * sizeof( struct epoll_event ) );
    if ( unlikely( newevents == NULL ) ) {
        return -1;
    }

    self->nevents = nevents;
    self->events = newevents;

    return 0;
}

int32_t epoll_insert( struct epoller * self, int32_t max )
{
    int32_t npairs = 0;
    struct eventpair * pairs = NULL;

    npairs = self->npairs;
    for ( ; npairs <= max; ) {
        npairs <<= 1;
    }

    pairs = (struct eventpair *)realloc(
        self->evpairs, npairs * sizeof( struct eventpair ) );
    if ( unlikely( pairs == NULL ) ) {
        return -1;
    }

    self->evpairs = pairs;
    memset( pairs + self->npairs, 0, ( npairs - self->npairs ) * sizeof( struct eventpair ) );
    self->npairs = npairs;

    return 0;
}

int32_t epoll_add( void * arg, struct event * ev )
{
    int32_t fd = 0, rc = -1;
    int32_t op = 0, events = 0;

    struct epoll_event epollevent;
    struct eventpair * eventpair = NULL;
    struct epoller * poller = (struct epoller *)arg;

    fd = event_get_fd( (event_t)ev );
    if ( fd < 0 ) {
        return -3;
    } else if ( fd >= poller->npairs ) {
        if ( epoll_insert( poller, fd ) != 0 ) {
            return -1;
        }
    }

    events = 0;
    op = EPOLL_CTL_ADD;
    eventpair = &( poller->evpairs[fd] );

    if ( eventpair->evread != NULL ) {
        events |= EPOLLIN;
        op = EPOLL_CTL_MOD;
    }
    if ( eventpair->evwrite != NULL ) {
        events |= EPOLLOUT;
        op = EPOLL_CTL_MOD;
    }

    if ( ev->events & EV_READ ) {
        events |= EPOLLIN;
    }
    if ( ev->events & EV_WRITE ) {
        events |= EPOLLOUT;
    }

    epollevent.data.u64 = 0; /* avoid valgrind warnning */
    epollevent.data.fd = fd;
    epollevent.events = events;

    rc = epoll_ctl( poller->epollfd, op, fd, &epollevent );
    if ( rc == -1 ) {
        if ( op == EPOLL_CTL_MOD && errno == ENOENT ) {
            // If a MOD operation fails with ENOENT, the fd was probably closed and re-opened.
            // We should retry the operation as an ADD.
            rc = epoll_ctl( poller->epollfd, EPOLL_CTL_ADD, fd, &epollevent );
        } else if ( op == EPOLL_CTL_ADD && errno == EEXIST ) {
            // If an ADD operation fails with EEXIST,
            // either the operation was redundant (as with a precautionary add),
            // or we ran into a fun kernel bug where using dup*() to duplicate the same file into the same fd gives you the same epitem rather than a fresh one.
            // For the second case, we must retry with MOD.
            rc = epoll_ctl( poller->epollfd, EPOLL_CTL_MOD, fd, &epollevent );
        }
    }

    if ( rc == -1 ) {
        return -2;
    }

    if ( ev->events & EV_READ ) {
        eventpair->evread = ev;
    }
    if ( ev->events & EV_WRITE ) {
        eventpair->evwrite = ev;
    }

    return 0;
}

int32_t epoll_del( void * arg, struct event * ev )
{
    int32_t fd = 0;
    int32_t op = 0, events = 0;
    int32_t delwrite = 1, delread = 1;

    struct epoll_event epollevent;
    struct eventpair * eventpair = NULL;
    struct epoller * poller = (struct epoller *)arg;

    fd = event_get_fd( (event_t)ev );
    if ( fd >= poller->npairs ) {
        return -1;
    }

    // 简单的删除指定的事件
    events = 0;
    op = EPOLL_CTL_DEL;
    eventpair = &( poller->evpairs[fd] );

    if ( ev->events & EV_READ ) {
        events |= EPOLLIN;
    }
    if ( ev->events & EV_WRITE ) {
        events |= EPOLLOUT;
    }

    // 删除的同时查看该描述符是否支持了其他事件
    // 如有, 则不能直接简单的删除
    if ( ( events & ( EPOLLIN | EPOLLOUT ) ) != ( EPOLLIN | EPOLLOUT ) ) {
        if ( ( events & EPOLLIN ) && eventpair->evwrite != NULL ) {
            // 删除的是读事件, 写事件仍然要监听
            // 并且还需要从写事件中查看是否支持了边缘触发模式
            delwrite = 0;
            events = EPOLLOUT;
            op = EPOLL_CTL_MOD;
        } else if ( ( events & EPOLLOUT ) && eventpair->evread != NULL ) {
            // 删除的是写事件, 读事件仍然要监听
            // 并且还需要从读事件中查看是否支持了边缘触发模式
            delread = 0;
            events = EPOLLIN;
            op = EPOLL_CTL_MOD;
        }
    }

    epollevent.data.u64 = 0; /* avoid valgrind warnning */
    epollevent.data.fd = fd;
    epollevent.events = events;

    if ( delread ) {
        eventpair->evread = NULL;
    }
    if ( delwrite ) {
        eventpair->evwrite = NULL;
    }

    return epoll_ctl( poller->epollfd, op, fd, &epollevent ) == -1 ? -2 : 0;
}

int32_t epoll_dispatch( struct eventset * sets, void * arg, int32_t tv )
{
    int32_t i, res = -1;
    struct epoller * poller = (struct epoller *)arg;

    if ( unlikely( tv > MAX_EPOLL_WAIT ) ) {
        tv = MAX_EPOLL_WAIT;
    }

    res = epoll_wait( poller->epollfd, poller->events, poller->nevents, tv );
    if ( res == -1 ) {
        if ( errno != EINTR ) {
            syslog( LOG_WARNING, "%s() epoll_wait() error <%d, %s>", __FUNCTION__, errno, strerror( errno ) );
            return -1;
        }

        return 0;
    }

    for ( i = 0; i < res; ++i ) {
        int32_t fd = poller->events[i].data.fd;
        int32_t what = poller->events[i].events;

        struct eventpair * eventpair = NULL;
        struct event *evread = NULL, *evwrite = NULL;

        if ( fd < 0 || fd >= poller->npairs ) {
            continue;
        }

        eventpair = &( poller->evpairs[fd] );

        if ( what & ( EPOLLHUP | EPOLLERR ) ) {
            evread = eventpair->evread;
            evwrite = eventpair->evwrite;
        } else {
            if ( what & ( EPOLLIN | EPOLLPRI ) ) {
                evread = eventpair->evread;
            }
            if ( what & EPOLLOUT ) {
                evwrite = eventpair->evwrite;
            }
        }

        if ( evread ) {
            event_active( evread, EV_READ );
        }
        if ( evwrite ) {
            event_active( evwrite, EV_WRITE );
        }
    }

    if ( res == poller->nevents ) {
        epoll_expand( poller );
    }

    return res;
}

void epoll_final( void * arg )
{
    struct epoller * poller = (struct epoller *)arg;

    if ( poller->evpairs ) {
        free( poller->evpairs );
    }
    if ( poller->events ) {
        free( poller->events );
    }
    if ( poller->epollfd >= 0 ) {
        close( poller->epollfd );
    }

    free( poller );
}

#endif
