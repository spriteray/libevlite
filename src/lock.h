
#ifndef LOCK_H
#define LOCK_H

#include <stdint.h>
#include <unistd.h>
#include <sched.h>
#include <stdatomic.h>

#ifdef USE_ATOMIC

#include "config.h"

struct evlock {
    atomic_flag lock;
};

// 通用的自适应退避策略 (Adaptive Backoff)
// 传入自旋次数的指针，函数内部会自动递增并决定是“空转”还是“让出 CPU”
static inline void _evlock_backoff( int * spin_count, int max_spin ) {
    if ( *spin_count < max_spin ) {
        // 阶段 1：极速自旋，降低功耗，不触发上下文切换
        cpu_pause();
        ++( *spin_count );
    } else {
        // 阶段 2：发生严重拥堵，主动放弃当前 CPU 时间片，让操作系统调度其他线程
#if defined( _POSIX_PRIORITY_SCHEDULING ) || defined( __APPLE__ ) || defined( __linux__ )
        sched_yield();  // 会有上下文切换(微秒级)
#else
        usleep( 0 );    // 在不支持sched_yield()的情况下，使用usleep(), 延迟不可控, 取决于HZ
#endif
    }
}

static inline void evlock_init( struct evlock * lock ) {
    // atomic_flag 必须明确清空状态。使用 relaxed 因为初始化时没有数据竞争
    atomic_flag_clear_explicit( &lock->lock, memory_order_relaxed );
}

static inline void evlock_lock( struct evlock * lock ) {
    int spin_count = 0;
    // Acquire 屏障：确保拿到锁之后，后续的读写操作不会被 CPU 乱序执行提前到拿锁之前
    while ( atomic_flag_test_and_set_explicit( &lock->lock, memory_order_acquire ) ) {
        // 根据 CPU 频率调整，通常 1000-5000 之间
        _evlock_backoff( &spin_count, 4000 );
    }
}

static inline void evlock_unlock( struct evlock * lock ) {
    // Release 屏障：确保解锁前的数据修改全部刷入内存
    atomic_flag_clear_explicit( &lock->lock, memory_order_release );
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
