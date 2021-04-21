
#ifndef EVENT_INTERNAL_H
#define EVENT_INTERNAL_H

#include "queue.h"
#include "event.h"

// 初始化的事件数, 该框架同样适用于内网服务器
#define INIT_EVENTS         1024

//
// 事件的状态
// 0000 0000 0000 0000
// 保留 私有 系统 链表
//
// 链表:    1 - 在全局链表中
//          2 - 在定时器中
//          3 - 激活链表中
//          4 - 保留
//

#define EVSTATUS_INSERTED   0x01
#define EVSTATUS_TIMER      0x02
#define EVSTATUS_ACTIVE     0x04
#define EVSTATUS_INTERNAL   0x10
#define EVSTATUS_INIT       0x20

#define EVSTATUS_ALL        (0x0f00|0x37)

//
//
//
struct event;
struct eventset;
struct eventop
{
    void * (*init)();
    int32_t (*add)(void *, struct event *);
    int32_t (*del)(void *, struct event *);
    int32_t (*dispatch)(struct eventset *, void *, int32_t);
    void (*final)(void *);
};

//
// 事件
//
struct event
{
    int32_t fd;
    int16_t events;

    int32_t status;
    int32_t results;

    // cb 一定要合法
    void * arg;
    eventcb_t cb;

    void * evsets;

    // 定时器的超时时间
    int32_t timer_msecs;

    // 事件在定时器数组中的索引
    // 删除时, 快速定位到某一个桶
    int32_t timer_index;

    // 事件的周期数
    int32_t timer_stepcnt;

    TAILQ_ENTRY(event) timerlink;
    TAILQ_ENTRY(event) eventlink;
    TAILQ_ENTRY(event) activelink;
};

TAILQ_HEAD( event_list, event );

#define EVENT_TIMEOUT(ev)       (int32_t)( (ev)->timer_msecs )
#define EVENT_TIMERINDEX(ev)    (int32_t)( (ev)->timer_index )
#define EVENT_TIMERSTEP(ev)     (int32_t)( (ev)->timer_stepcnt )

int32_t event_active( struct event * self, int16_t res );

//
// event定时器模块
//
#define TIMER_MAX_PRECISION 8           // 定时器最大精度为8ms
#define TIMER_BUCKET_COUNT  8192        // 必须是2的N次方

struct evtimer
{
    int32_t event_count;                // 管理的事件个数
    int32_t bucket_count;               // 桶的个数
    int32_t max_precision;              // 最大精度, 精确到1毫秒

    int32_t dispatch_refer;             //
    struct event_list * bucket_array;   // 桶的数组
};

#define EVTIMER_INDEX(t,c)      ( (c) & ((t)->bucket_count-1) )

struct evtimer * evtimer_create( int32_t max_precision, int32_t bucket_count );
int32_t evtimer_append( struct evtimer * self, struct event * ev );
int32_t evtimer_remove( struct evtimer * self, struct event * ev );
int32_t evtimer_dispatch( struct evtimer * self );
int32_t evtimer_count( struct evtimer * self );
int32_t evtimer_clean( struct evtimer * self );
void evtimer_destroy( struct evtimer * self );

//
// 事件集模块
//

struct eventset
{
    int32_t timer_precision;

    int64_t expire_time;
    struct evtimer * core_timer;

    void * evsets;
    struct eventop * evselect;

    struct event_list eventlist;
    struct event_list activelist;
};

#define EVENTSET_PRECISION(sets)       (((struct eventset *)(sets))->timer_precision)

#endif
