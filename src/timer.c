
#include <stdio.h>
#include <syslog.h>
#include <stdlib.h>

#include "event-internal.h"

struct evtimer * evtimer_create( int32_t max_precision, int32_t bucket_count )
{
    struct evtimer * t = NULL;

    t = (struct evtimer *)malloc( sizeof(struct evtimer) );
    if ( t )
    {
        t->event_count = 0;
        t->dispatch_refer = 0;
        t->bucket_count = bucket_count;
        t->max_precision = max_precision;

        t->bucket_array = (struct event_list *)malloc( bucket_count * sizeof(struct event_list) );
        if ( t->bucket_array == NULL )
        {
            free( t );
            t = NULL;
        }
        else
        {
            int32_t i = 0;

            for ( i = 0; i < bucket_count; ++i )
            {
                TAILQ_INIT( &(t->bucket_array[i]) );
            }
        }
    }

    return t;
}

int32_t evtimer_append( struct evtimer * self, struct event * ev )
{
    int32_t index = -1;
    int32_t tv = EVENT_TIMEOUT(ev);

    if ( tv <= 0 )
    {
        return -1;
    }

    // 把桶的索引号写入事件句柄中, 便于查找以及删除
    // 如果定时器超时时间过长, 设定其定时器周期数
    index = EVTIMER_INDEX(self, tv/self->max_precision+self->dispatch_refer);

    //
    ev->timer_index = index;
    ev->timer_stepcnt = tv / ( self->max_precision * self->bucket_count );
    if ( tv % (self->max_precision * self->bucket_count) != 0 )
    {
        ev->timer_stepcnt += 1;
    }

    ++self->event_count;
    TAILQ_INSERT_TAIL( &(self->bucket_array[index]), ev, timerlink );

    return 0;
}

int32_t evtimer_remove( struct evtimer * self, struct event * ev )
{
    // 根据句柄中的索引号, 快速定位桶
    int32_t index = EVENT_TIMERINDEX(ev);

    if ( index < 0 || index >= self->bucket_count )
    {
        return -1;
    }

    ev->timer_index = -1;
    ev->timer_stepcnt = -1;

    --self->event_count;
    TAILQ_REMOVE( &(self->bucket_array[index]), ev, timerlink );

    return 0;
}

int32_t evtimer_dispatch( struct evtimer * self )
{
    int32_t index = 0;
    int32_t rc = 0, done = 0;

    struct event * laster = NULL;
    struct event_list * head = NULL;

    index = self->dispatch_refer++;
    head = &( self->bucket_array[EVTIMER_INDEX(self, index)] );

    // 该桶中没有定时的事件
    if ( TAILQ_EMPTY(head) )
    {
        return 0;
    }

    // 遍历超时事件链表
    laster = TAILQ_LAST( head, event_list );
    while ( !done )
    {
        int32_t step = 0;
        struct event * ev = TAILQ_FIRST(head);

        // 由于某些事件的超时时间过长
        // 所以还是需要继续添加到事件链表中的
        // 必须判断当前节点是否是链表的尾部元素
        if ( ev == laster )
        {
            done = 1;
        }

        // 获取步长
        --ev->timer_stepcnt;
        step = EVENT_TIMERSTEP( ev );

        if ( step > 0 )
        {
            // 未超时
            TAILQ_REMOVE( head, ev, timerlink );
            TAILQ_INSERT_TAIL( head, ev, timerlink );
        }
        else
        {
            // 超时 or 出错

            // 删除事件
            evtimer_remove( self, ev );
            ev->status &= ~EVSTATUS_TIMER;

            // 超时了,
            // 从队列中删除, 并添加到激活队列中
            if ( step == 0 )
            {
                // 计数器
                ++rc;
                event_active( ev, EV_TIMEOUT );
            }
            else
            {
                // 出错, 暂且记日志吧
                syslog(LOG_WARNING, "%s() evtimer dispatch error <%p>", __FUNCTION__, ev);
            }
        }
    }

    return rc;
}

int32_t evtimer_count( struct evtimer * self )
{
    return self->event_count;
}

int32_t evtimer_clean( struct evtimer * self )
{
    int32_t i = 0, rc = 0;

    for ( i = 0; i < self->bucket_count; ++i )
    {
        struct event * ev = NULL;
        struct event_list * head = &( self->bucket_array[i] );

        for ( ev = TAILQ_FIRST(head); ev; )
        {
            struct event * next = TAILQ_NEXT( ev, timerlink );

            evsets_del( event_get_sets((event_t)ev), (event_t)ev );

            ++rc;
            ev = next;
            --self->event_count;
        }
    }

    self->dispatch_refer = 0;

    return rc;
}

void evtimer_destroy( struct evtimer * self )
{
    if ( self->bucket_array )
    {
        free( self->bucket_array );
    }

    free( self );
}
