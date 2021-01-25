
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <assert.h>

#include "config.h"
#include "utils.h"
#include "event-internal.h"

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

// 版本
#ifndef __EVENT_VERSION__
#define __EVENT_VERSION__ "libevlite-X.X.X"
#endif

// linux和freebsd的兼容性定义
#if defined EVENT_OS_LINUX
extern const struct eventop epollops;
const struct eventop * evsel = &epollops;
#elif defined EVENT_OS_BSD || defined EVENT_OS_MACOS
extern const struct eventop kqueueops;
const struct eventop * evsel = &kqueueops;
#else
const struct eventop * evsel = NULL;
#endif

// event.c中的工具定义

static inline int32_t evsets_process_active( struct eventset * self );
static inline int32_t event_queue_insert( struct eventset * self, struct event * ev, int32_t type );
static inline int32_t event_queue_remove( struct eventset * self, struct event * ev, int32_t type );

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

int32_t event_queue_insert( struct eventset * self, struct event * ev, int32_t type )
{
    if ( ev->status & type )
    {
        // 事件已经处于该列表中
        return 1;
    }

    ev->status |= type;

    switch( type )
    {
        case EVSTATUS_INSERTED :
            TAILQ_INSERT_TAIL( &(self->eventlist), ev, eventlink );
            break;

        case EVSTATUS_ACTIVE :
            TAILQ_INSERT_TAIL( &(self->activelist), ev, activelink );
            break;

        case EVSTATUS_TIMER :
            evtimer_append( self->core_timer, ev );
            break;

        default :
            return -1;
            break;
    }

    return 0;
}

int32_t event_queue_remove( struct eventset * self, struct event * ev, int32_t type )
{
    if ( !(ev->status & type) )
    {
        return -1;
    }

    ev->status &= ~type;

    switch( type )
    {
        case EVSTATUS_INSERTED :
            {
                TAILQ_REMOVE( &(self->eventlist), ev, eventlink );
            }
            break;

        case EVSTATUS_ACTIVE :
            {
                TAILQ_REMOVE( &(self->activelist), ev, activelink );
            }
            break;

        case EVSTATUS_TIMER :
            {
                evtimer_remove( self->core_timer, ev );
            }
            break;

        default :
            {
                return -2;
            }
            break;
    }

    return 0;
}


// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------


event_t event_create()
{
    struct event * self = NULL;

    self = (struct event *)malloc( sizeof(struct event) );
    if ( self )
    {
        self->fd = -1;
        self->events = 0;
        self->evsets = NULL;

        self->cb = NULL;
        self->arg = NULL;

        self->timer_index = -1;
        self->timer_msecs = -1;
        self->timer_stepcnt = -1;

        self->results = 0;
        self->status = EVSTATUS_INIT;
    }

    return (event_t)self;
}

void event_set( event_t self, int32_t fd, int16_t ev )
{
    struct event * e = (struct event *)self;

    e->fd = fd;
    e->events = ev;
}

void event_set_callback( event_t self, eventcb_t cb, void * arg )
{
    struct event * e = (struct event *)self;

    e->cb = cb;
    e->arg = arg;
}

int32_t event_get_fd( event_t self )
{
    return ((struct event *)self)->fd;
}

evsets_t event_get_sets( event_t self )
{
    return ((struct event *)self)->evsets;
}

void event_reset( event_t self )
{
    struct event * ev = (struct event *)self;

    if ( ev->evsets != NULL )
    {
        evsets_del( ev->evsets, self );
        ev->evsets = NULL;
    }

    ev->fd = -1;
    ev->events = 0;
    ev->evsets = NULL;

    ev->cb = NULL;
    ev->arg = NULL;

    ev->timer_index = -1;
    ev->timer_msecs = -1;
    ev->timer_stepcnt = -1;

    ev->results = 0;
    ev->status = EVSTATUS_INIT;
}

void event_destroy( event_t self )
{
    free( self );
}

int32_t event_active( struct event * self, int16_t res )
{
    if ( self->status & EVSTATUS_ACTIVE )
    {
        self->results |= res;
        return 1;
    }

    self->results = res;
    event_queue_insert( self->evsets, self, EVSTATUS_ACTIVE );

    return 0;
}


// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

evsets_t evsets_create()
{
    struct eventset * self = NULL;

    //
    // 兼容性检查
    //
    assert( evsel != NULL );

    self = (struct eventset *)calloc( 1, sizeof(struct eventset) );
    if ( self )
    {
        self->evselect = (struct eventop *)evsel;

        // 初始化
        TAILQ_INIT( &self->eventlist );
        TAILQ_INIT( &self->activelist );

        self->evsets = self->evselect->init();
        if ( self->evsets )
        {
            self->core_timer = evtimer_create( TIMER_MAX_PRECISION, TIMER_BUCKET_COUNT );
            if ( self->core_timer )
            {
                self->timer_precision = TIMER_MAX_PRECISION;
                self->expire_time = milliseconds() + self->timer_precision;
            }
            else
            {
                evsets_destroy( self );
                self = NULL;
            }
        }
        else
        {
            evsets_destroy( self );
            self = NULL;
        }
    }

    return self;
}

const char * evsets_get_version()
{
    return __EVENT_VERSION__;
}

int32_t evsets_add( evsets_t self, event_t ev, int32_t tv )
{
    int32_t rc = 0x00;
    struct event * e = (struct event *)ev;
    struct eventset * sets = (struct eventset *)self;

    assert( e->cb != NULL );

    if ( unlikely(e->status & ~EVSTATUS_ALL) )
    {
        return -1;
    }

    e->evsets = self;

    // 监听fd的网络事件的前提是fd合法
    if ( (e->fd > 0)
            && (e->events & (EV_READ|EV_WRITE))
            && !(e->status & (EVSTATUS_ACTIVE|EVSTATUS_INSERTED)) )
    {
        if ( sets->evselect->add( sets->evsets, e ) == 0 )
        {
            rc |= 0x01;
            event_queue_insert( sets, e, EVSTATUS_INSERTED );
        }
    }

    if ( tv >= 0 )
    {
        // 如果已经在定时器中了,
        // 一定要删除，以当前时间当前索引重新加入定时器中
        // 这样比较准确
        if ( e->status & EVSTATUS_TIMER )
        {
            event_queue_remove( sets, e, EVSTATUS_TIMER );
        }

        // 已经超时了
        // 必须从激活队列中删除，
        // 重新加入定时器中
        if ( (e->results & EV_TIMEOUT)
                && (e->status & EVSTATUS_ACTIVE) )
        {
            event_queue_remove( sets, e, EVSTATUS_ACTIVE );
        }

        rc |= 0x02;
        e->timer_msecs = tv;
        event_queue_insert( sets, e, EVSTATUS_TIMER );
    }

    return rc;
}

int32_t evsets_del( evsets_t self, event_t ev )
{
    struct event * e = (struct event *)ev;
    struct eventset * sets = (struct eventset *)self;

    assert ( e->evsets == sets );

    if ( unlikely( e->status & ~EVSTATUS_ALL ) )
    {
        return -2;
    }

    if ( e->status & EVSTATUS_TIMER )
    {
        event_queue_remove( sets, e, EVSTATUS_TIMER );
    }
    if ( e->status & EVSTATUS_ACTIVE )
    {
        event_queue_remove( sets, e, EVSTATUS_ACTIVE );
    }
    if ( e->status & EVSTATUS_INSERTED )
    {
        event_queue_remove( sets, e, EVSTATUS_INSERTED );
        return sets->evselect->del( sets->evsets, e );
    }

    return 0;
}

int32_t evsets_dispatch( evsets_t self )
{
    int32_t res = 0;
    int32_t seconds4wait = 0;
    struct eventset * sets = (struct eventset *)self;

    // 没有激活事件的情况下等待超时时间
    if ( TAILQ_EMPTY(&sets->activelist) )
    {
        // 根据定时器的超时时间, 确认IO的等待时间
        seconds4wait = (int32_t)( sets->expire_time - milliseconds() );
        if ( seconds4wait < 0 )
        {
            seconds4wait = 0;
        }
        else if ( seconds4wait > sets->timer_precision )
        {
            seconds4wait = sets->timer_precision;
        }
    }

    // 处理IO事件
    res = sets->evselect->dispatch( sets, sets->evsets, seconds4wait );
    if ( res < 0 )
    {
        // IO事件出错
        syslog(LOG_WARNING, "%s() eventsets dispatch error <%d>", __FUNCTION__, res);
    }

    // 事件集的超时时间是要及时更新的
    int64_t now = milliseconds();
    if ( sets->expire_time <= now )
    {
        // 定时器时间到了, 分发事件
        evtimer_dispatch( sets->core_timer );
        sets->expire_time = now + sets->timer_precision;
    }

    // 处理所有事件, 并回调定义好的函数
    return evsets_process_active( sets );
}

void evsets_destroy( evsets_t self )
{
    struct event * ev = NULL;
    struct eventset * sets = (struct eventset *)self;

    // 删除所有事件
    for ( ev = TAILQ_FIRST( &(sets->eventlist) ); ev; )
    {
        struct event * next = TAILQ_NEXT( ev, eventlink );

        if ( !(ev->status & EVSTATUS_INTERNAL) )
        {
            evsets_del( self, (event_t)ev );
        }

        ev = next;
    }

    // 清除定时器
    if ( sets->core_timer )
    {
        evtimer_clean( sets->core_timer );
    }

    // 删除激活事件
    for ( ev = TAILQ_FIRST( &(sets->activelist) ); ev; )
    {
        struct event * next = TAILQ_NEXT( ev, activelink );

        if ( !(ev->status & EVSTATUS_INTERNAL) )
        {
            evsets_del( self, (event_t)ev );
        }

        ev = next;
    }

    // 销毁定时器
    if ( sets->core_timer )
    {
        evtimer_destroy( sets->core_timer );
    }

    // 销毁IO实例
    sets->evselect->final( sets->evsets );
    free( sets );
}

int32_t evsets_process_active( struct eventset * self )
{
    int32_t rc = 0;
    struct event * ev = NULL;
    struct event_list * activelist = &(self->activelist);

    for ( ev = TAILQ_FIRST(activelist); ev; ev = TAILQ_FIRST(activelist) )
    {
        if ( !(ev->events&EV_PERSIST) )
        {
            evsets_del( self, (event_t)ev );
        }
        else
        {
            event_queue_remove( self, ev, EVSTATUS_ACTIVE );

            // Timeouts and persistent events work together
            if ( ev->timer_msecs >= 0 )
            {
                if ( !(ev->results & EV_TIMEOUT) )
                {
                    // IO事件发生后(并非超时), 移除定时器
                    event_queue_remove( self, ev, EVSTATUS_TIMER );
                }
                // 重置定时器
                event_queue_insert( self, ev, EVSTATUS_TIMER );
            }
        }

        // 回调
        ++rc;
        (*ev->cb)( ev->fd, ev->results, ev->arg );
    }

    return rc;
}
