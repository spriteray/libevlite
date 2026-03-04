
#ifndef LOCK_H
#define LOCK_H

#include <stdint.h>
#include <unistd.h>
#include <sched.h>
#include <stdatomic.h>

#ifdef USE_ATOMIC

#include "config.h"

struct evlock {
    atomic_int lock;
} __attribute__((aligned(64))) ;

// 指数级退避方案
static inline void _evlock_backoff( int * step, int max_step ) {
    if ( *step < max_step ) {
        // 阶段 1：极速自旋，降低功耗，不触发上下文切换
        int count = 1 << *step;
        for ( int i = 0; i < count; ++i ) {
            cpu_pause();
        }
        ++( *step );
    } else {
        // 阶段 2：发生严重拥堵，主动放弃当前 CPU 时间片，让操作系统调度其他线程
#if defined( _POSIX_PRIORITY_SCHEDULING ) || defined( __APPLE__ ) || defined( __linux__ )
        sched_yield();  // 会有上下文切换(微秒级)
#else
        usleep( 0 );    // 在不支持sched_yield()的情况下，使用usleep(), 延迟不可控, 取决于HZ
#endif
        // 注意：yield 之后最好把 spin_count 清零，让下一次唤醒时重新从 pause 开始退避
        *step = 0;
    }
}

static inline void evlock_init( struct evlock * lock ) {
    // atomic_int 明确清空状态为 0 (0: 未锁, 1: 已锁)
    // 初始化时没有数据竞争，使用 relaxed 即可
    atomic_store_explicit( &lock->lock, 0, memory_order_relaxed );
}

static inline void evlock_lock( struct evlock * lock ) {
    int step = 0;

    // 外层循环(TAS), 尝试抢锁
    // atomic_exchange 会把 1 强行塞进去，并返回原来的旧值。
    // 如果返回 0，说明原来是没锁的，我们抢占成功！跳出循环。
    while ( atomic_exchange_explicit( &lock->lock, 1, memory_order_acquire ) == 1 ) {

        // 内层循环(TTAS), 不锁总线
        while ( atomic_load_explicit( &lock->lock, memory_order_relaxed ) == 1 ) {
            // 别人拿着锁，我们在纯用户态安静地退避
            _evlock_backoff( &step, 6 );
        }

        // 锁被释放，重置退避状态，迅速发起抢锁
        step = 0;
    }
}

static inline void evlock_unlock( struct evlock * lock ) {
    // 【解锁：写回 0】
    // Release 屏障：极其关键！确保解锁前（临界区内）的数据修改全部刷入内存，
    // 对下一个拿到锁（执行 acquire）的线程绝对可见。
    atomic_store_explicit( &lock->lock, 0, memory_order_release );
}

static inline void evlock_destroy( struct evlock * lock ) {}

#else

#include <pthread.h>

struct evlock {
    pthread_mutex_t lock;
};

static inline void evlock_init( struct evlock * lock ) {
    pthread_mutex_init( &lock->lock, NULL );
}

static inline void evlock_lock( struct evlock * lock ) {
    pthread_mutex_lock( &lock->lock );
}

static inline void evlock_unlock( struct evlock * lock ) {
    pthread_mutex_unlock( &lock->lock );
}

static inline void evlock_destroy( struct evlock * lock ) {
    pthread_mutex_destroy( &lock->lock );
}

#endif

#endif
