#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "network.h"
#include "channel.h"
#include "session.h"
#include "event-internal.h"
#include "threads-internal.h"
#include "ephashtable.h"

static void test_buffer_compact_growth( void )
{
    struct buffer buffer;
    char original[4000], appended[5000];
    memset( original, 'a', sizeof( original ) );
    memset( appended, 'b', sizeof( appended ) );
    buffer_init( &buffer );
    assert( buffer_append( &buffer, original, sizeof( original ) ) == 0 );
    assert( buffer.capacity == 4096 );
    buffer_erase( &buffer, 3000 );
    assert( buffer_append( &buffer, appended, sizeof( appended ) ) == 0 );
    assert( buffer.capacity == 8192 && buffer.length == 6000 );
    assert( buffer.buffer == buffer.orignbuffer );
    assert( memcmp( buffer.buffer, original + 3000, 1000 ) == 0 );
    assert( memcmp( buffer.buffer + 1000, appended, sizeof( appended ) ) == 0 );
    buffer_clear( &buffer );
}

static void test_endpoint_keys( void )
{
    const char * hosts[] = { "127.0.0.1", "::1", "2001:db8::1234" };
    uint16_t ports[] = { 0, 1, 8080, 65535 };
    size_t sizes[] = { 1, 8192 };
    for ( size_t s = 0; s < 2; ++s ) {
        // A single bucket forces collisions through insert/find/remove.
        struct ephashtable * table = ephashtable_create( sizes[s], sizeof( int ), NULL );
        assert( table != NULL );
        for ( size_t h = 0; h < 3; ++h ) {
            for ( size_t p = 0; p < 4; ++p ) {
                int * value = ephashtable_append( table, hosts[h], ports[p] );
                assert( value != NULL );
                *value = (int)( h * 4 + p );
            }
        }
        for ( size_t h = 0; h < 3; ++h ) {
            for ( size_t p = 0; p < 4; ++p ) {
                char key[65];
                strcpy( key + 1, hosts[h] );
                int * value = ephashtable_find( table, key + 1, ports[p] );
                assert( value != NULL && *value == (int)( h * 4 + p ) );
                ephashtable_remove( table, key + 1, ports[p] );
                assert( ephashtable_find( table, key + 1, ports[p] ) == NULL );
            }
        }
        assert( table->count == 0 );
        ephashtable_destroy( table );
    }
}

#ifdef TEST_WRITEV_INTERRUPT
static int interrupted_fd = -1;
static int write_calls;
ssize_t __real_writev( int fd, const struct iovec * iov, int count );

ssize_t __wrap_writev( int fd, const struct iovec * iov, int count )
{
    if ( fd == interrupted_fd ) {
        ++write_calls;
        if ( write_calls == 1 ) {
            assert( count > 0 && iov[0].iov_len > 2 );
            return write( fd, iov[0].iov_base, 2 );
        }
        if ( write_calls == 2 || write_calls == 3 ) {
            errno = EINTR;
            return -1;
        }
    }
    return __real_writev( fd, iov, count );
}
#endif

struct task_state {
    atomic_int calls;
    atomic_int cancelled;
};

static int32_t execute_task( void * context, void * task, int32_t interval )
{
    struct task_state * state = task;
    assert( context == state );
    if ( interval < 0 ) {
        atomic_fetch_add( &state->cancelled, 1 );
    } else {
        assert( interval == 1 );
        atomic_fetch_add( &state->calls, 1 );
    }
    return 1;
}

static void test_schedule( void )
{
    struct task_state state = { 0 };
    struct iothread thread = { 0 };
    thread.context = &state;
    thread.sets = evsets_create( 1 );
    assert( thread.sets != NULL );
    assert( iothread_schedule( &thread, &state, execute_task, 1 ) == 0 );
    for ( int i = 0; i < 1000 && atomic_load( &state.calls ) == 0; ++i ) {
        evsets_dispatch( thread.sets );
    }
    assert( atomic_load( &state.calls ) == 1 );
    assert( LIST_EMPTY( &thread.schedulerlist ) );

    assert( iothread_schedule( &thread, &state, execute_task, 60000 ) == 0 );
    iothread_stop( &thread );
    assert( atomic_load( &state.cancelled ) == 1 );
}

static void test_cross_thread_schedule( void )
{
    struct task_state state = { 0 };
    void * contexts[] = { &state };
    iolayer_t layer = iolayer_create( 1, 16, 1 );
    assert( layer != NULL );
    assert( iolayer_set_iocontext( layer, contexts, 1 ) == 0 );
    assert( iolayer_schedule( layer, &state, execute_task, 1 ) == 0 );
    for ( int i = 0; i < 5000 && atomic_load( &state.calls ) == 0; ++i ) {
        usleep( 1000 );
    }
    assert( atomic_load( &state.calls ) == 1 );
    iolayer_destroy( layer );
    assert( atomic_load( &state.cancelled ) == 0 );
}

static void on_shutdown( void * context, int32_t way )
{
    int * count = context;
    assert( way == 0 );
    ++*count;
}

static void test_session_shutdown( void )
{
    int closed = 0;
    int sockets[2];
    evsets_t sets = evsets_create( 1 );
    struct session_manager * manager = session_manager_create( 0, 1 );
    assert( sets != NULL && manager != NULL );
    assert( socketpair( AF_UNIX, SOCK_STREAM, 0, sockets ) == 0 );
    struct session * s = session_manager_alloc( manager );
    assert( s != NULL );
    sid_t id = s->id;
    s->fd = sockets[0];
    s->type = eSessionType_Accept;
    s->setting.read_callback = channel_on_read;
    s->setting.write_callback = channel_on_write;
    s->evsets = sets;
    s->context = &closed;
    s->service.shutdown = on_shutdown;

    struct message * msg = message_create();
    assert( msg != NULL );
    assert( message_add_buffer( msg, "pending", 7 ) == 0 );
    message_add_receiver( msg, id );
    assert( session_sendqueue_append( s, msg ) == 0 );
    session_add_event( s, EV_WRITE );
    assert( !( ( (struct event *)s->evwrite )->status & EVSTATUS_TIMER ) );
    session_close( s );
    assert( session_shutdown( s ) == 1 );
    assert( ( (struct event *)s->evwrite )->status & EVSTATUS_TIMER );
    assert( EVENT_TIMEOUT( (struct event *)s->evwrite ) == MAX_SECONDS_WAIT_FOR_SHUTDOWN );

    // Deliver the timeout as the event loop would, without waiting ten seconds.
    evsets_del( sets, s->evwrite );
    channel_on_write( sockets[0], EV_TIMEOUT, s );
    assert( closed == 1 );
    assert( session_manager_count( manager ) == 0 );
    assert( session_manager_get( manager, id ) == NULL );
#ifdef USE_REUSESESSION
    assert( manager->recyclesize == 1 );
#endif

    s = session_manager_alloc( manager );
    assert( s != NULL && s->id != id );
    s->fd = -1;
    s->type = eSessionType_Accept;
    s->context = &closed;
    s->service.shutdown = on_shutdown;
    session_close( s );
    assert( session_shutdown( s ) == 0 );
    assert( closed == 2 );
    session_manager_destroy( manager );
    evsets_destroy( sets );
    close( sockets[1] );
}

static ssize_t send_would_block( struct session * s, char * buf, size_t size )
{
    (void)s;
    (void)buf;
    (void)size;
    return 0;
}

static char * reject_transform( void * context, const char * buf, size_t * size )
{
    (void)context;
    (void)buf;
    (void)size;
    return NULL;
}

static void test_send_result( void )
{
    int closed = 0;
    int sockets[2];
    struct iothread thread = { 0 };
    struct iothreads threads = { 0 };
    struct iolayer layer = { 0 };
    thread.id = pthread_self();
    thread.manager = session_manager_create( 0, 1 );
    thread.sets = evsets_create( 1 );
    assert( thread.manager != NULL && thread.sets != NULL );
    threads.nthreads = 1;
    threads.threads = &thread;
    layer.nthreads = 1;
    layer.threads = &threads;
    assert( socketpair( AF_UNIX, SOCK_STREAM, 0, sockets ) == 0 );
    struct session * s = session_manager_alloc( thread.manager );
    assert( s != NULL );
    s->fd = sockets[0];
    s->type = eSessionType_Accept;
    s->evsets = thread.sets;
    s->context = &closed;
    s->service.shutdown = on_shutdown;
    s->setting.send = send_would_block;
    s->setting.read_callback = channel_on_read;
    s->setting.write_callback = channel_on_write;

    assert( iolayer_send( &layer, s->id, "first", 5, 0 ) == 0 );
    assert( iolayer_send( &layer, s->id, "second", 6, 0 ) == 0 );
    assert( session_sendqueue_count( s ) == 2 );
    assert( iolayer_send( &layer, MAKE_SID( 0, 1, 0 ), "invalid", 7, 0 ) == -3 );
    layer.transform = reject_transform;
    assert( iolayer_send( &layer, s->id, "rejected", 8, 0 ) == -3 );
    assert( session_sendqueue_count( s ) == 2 );

    session_close( s );
    channel_shutdown( s );
    assert( closed == 1 );
    session_manager_destroy( thread.manager );
    evsets_destroy( thread.sets );
    close( sockets[1] );
}

static void test_tcp_flush( int interrupt )
{
    int sockets[2], closed = 0;
    evsets_t sets = evsets_create( 1 );
    struct session_manager * manager = session_manager_create( 0, 1 );
    assert( sets != NULL && manager != NULL );
    assert( socketpair( AF_UNIX, SOCK_STREAM, 0, sockets ) == 0 );
    struct session * s = session_manager_alloc( manager );
    assert( s != NULL );
    sid_t id = s->id;
    s->fd = sockets[0];
    s->type = eSessionType_Accept;
    s->evsets = sets;
    s->context = &closed;
    s->service.shutdown = on_shutdown;
    s->setting.send = send_would_block;
    s->setting.read_callback = channel_on_read;
    s->setting.write_callback = channel_on_write;
    s->setting.transmit = channel_transmit;
#ifdef TEST_WRITEV_INTERRUPT
    if ( interrupt ) {
        interrupted_fd = sockets[0];
        write_calls = 0;
    }
#else
    assert( !interrupt );
#endif
    assert( session_send( s, "first", 5 ) == 0 );
    for ( int i = 0; i < 100 && session_sendqueue_count( s ) > 0; ++i ) {
        evsets_dispatch( sets );
    }
    assert( session_sendqueue_count( s ) == 0 );
    assert( !( s->status & SESSION_WRITING ) );
#ifdef TEST_WRITEV_INTERRUPT
    if ( interrupt ) {
        assert( write_calls == 4 );
        interrupted_fd = -1;
    }
#endif
    char data[5];
    assert( recv( sockets[1], data, sizeof( data ), MSG_DONTWAIT ) == sizeof( data ) );
    assert( memcmp( data, "first", sizeof( data ) ) == 0 );
    assert( recv( sockets[1], data, sizeof( data ), MSG_DONTWAIT ) == -1 );
    assert( errno == EAGAIN || errno == EWOULDBLOCK );

    assert( session_send( s, "close", 5 ) == 0 );
    session_close( s );
    assert( session_shutdown( s ) == 1 );
    for ( int i = 0; i < 100 && session_manager_get( manager, id ) != NULL; ++i ) {
        evsets_dispatch( sets );
    }
    assert( closed == 1 && session_manager_count( manager ) == 0 );
    assert( recv( sockets[1], data, sizeof( data ), MSG_DONTWAIT ) == sizeof( data ) );
    assert( memcmp( data, "close", sizeof( data ) ) == 0 );
    session_manager_destroy( manager );
    evsets_destroy( sets );
    close( sockets[1] );
}

int main( void )
{
    test_buffer_compact_growth();
    test_endpoint_keys();
    test_schedule();
    test_cross_thread_schedule();
    test_session_shutdown();
    test_send_result();
    test_tcp_flush( 0 );
#ifdef TEST_WRITEV_INTERRUPT
    test_tcp_flush( 1 );
#endif
    puts( "regression tests passed" );
    return 0;
}
