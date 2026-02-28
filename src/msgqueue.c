
#include "msgqueue.h"

#include <syslog.h>

QUEUE_GENERATE( taskqueue, struct task )

struct msgqueue * msgqueue_create( uint32_t size )
{
    struct msgqueue * self = NULL;

    self = (struct msgqueue *)malloc( sizeof( struct msgqueue ) );
    if ( self ) {
        self->popfd = -1;
        self->pushfd = -1;
        evlock_init( &self->lock );

        if ( QUEUE_INIT( taskqueue )( &self->queue, size ) != 0 ) {
            msgqueue_destroy( self );
            self = NULL;
        } else {
            int32_t rc = -1;

#if !defined EVENT_HAVE_EVENTFD
            int32_t fds[2] = { -1, -1 };
            rc = pipe( fds );
            // rc = socketpair( AF_UNIX, SOCK_STREAM, 0, fds );
            if ( rc != -1 ) {
                self->popfd = fds[0];
                self->pushfd = fds[1];
#ifdef O_NOATIME
                // linux在读pipe的时候会更新访问时间, touch_atime(), 这个的开销也不小
                fcntl( self->popfd, F_SETFL, O_NOATIME );
#endif
            }
#else
            syslog( LOG_INFO, "%s() use eventfd() .", __FUNCTION__ );
            rc = eventfd( 0, EFD_NONBLOCK | EFD_CLOEXEC );
            if ( rc >= 0 ) {
                self->popfd = rc;
                self->pushfd = rc;
            }
#endif
            if ( rc == -1 ) {
                msgqueue_destroy( self );
                self = NULL;
            }
        }
    }

    return self;
}

int32_t msgqueue_push( struct msgqueue * self, struct task * task )
{
    int32_t rc = -1;
    uint32_t isbc = 0;

    evlock_lock( &self->lock );

    rc = QUEUE_PUSH( taskqueue )( &self->queue, task );
    isbc = QUEUE_COUNT( taskqueue )( &self->queue );

    evlock_unlock( &self->lock );

    if ( rc == 0 && isbc == 1 ) {
        uint64_t one = 1;

        if ( sizeof( one )
            != write( self->pushfd, &one, sizeof( one ) ) ) {
            // 写出错了
            syslog( LOG_WARNING, "%s() : write to Pipe(fd:%u) error .", __FUNCTION__, self->pushfd );
        }
    }

    return rc;
}

int32_t msgqueue_pop( struct msgqueue * self, struct task * task )
{
    int32_t rc = -1;

    evlock_lock( &self->lock );
    rc = QUEUE_POP( taskqueue )( &self->queue, task );
    evlock_unlock( &self->lock );

    return rc;
}

int32_t msgqueue_pops( struct msgqueue * self, struct task * tasks, uint32_t count )
{
    uint32_t i = 0;

    evlock_lock( &self->lock );
    for ( i = 0; i < count; ++i ) {
        if ( QUEUE_POP( taskqueue )( &self->queue, &tasks[i] )
            == 0 ) {
            break;
        }
    }
    evlock_unlock( &self->lock );

    return i;
}

int32_t msgqueue_swap( struct msgqueue * self, struct taskqueue * queue )
{
    evlock_lock( &self->lock );
    QUEUE_SWAP( taskqueue ) ( &self->queue, queue );
    evlock_unlock( &self->lock );

    return 0;
}

uint32_t msgqueue_count( struct msgqueue * self )
{
    uint32_t rc = 0;

    evlock_lock( &self->lock );
    rc = QUEUE_COUNT( taskqueue )( &self->queue );
    evlock_unlock( &self->lock );

    return rc;
}

int32_t msgqueue_popfd( struct msgqueue * self )
{
    int32_t rc = 0;

    evlock_lock( &self->lock );
    rc = self->popfd;
    evlock_unlock( &self->lock );

    return rc;
}

int32_t msgqueue_destroy( struct msgqueue * self )
{
    if ( self->popfd ) {
        close( self->popfd );
    }
    if ( self->pushfd
        && self->pushfd != self->popfd ) {
        close( self->pushfd );
    }

    self->popfd = -1;
    self->pushfd = -1;

    QUEUE_CLEAR( taskqueue ) ( &self->queue );
    evlock_destroy( &self->lock );
    free( self );

    return 0;
}

