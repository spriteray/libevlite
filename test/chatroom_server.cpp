
#include <deque>
#include <vector>
#include <set>
#include <algorithm>

#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <sys/time.h>

#include "io.h"
#include "utils.h"
#include "chatroom.h"

class TASK
{
public:
    static void * clone( void * t ) {
        TASK * task = (TASK *)t;
        TASK * newtask = new TASK();
        newtask->data = task->data;
        return newtask;
    }

    static void perform( void * iocontext, void * t ) {
        TASK * task = (TASK *)t;
        printf( "%p : %d \n", iocontext, task->data );
        delete task;
    }

public:
    int32_t data;
};

class ChatRoomService;
class ChatRoomSession : public IIOSession
{
public:
    ChatRoomSession() = default;
    virtual ~ChatRoomSession() = default;

public:
    virtual int32_t onStart() override;
    virtual ssize_t onProcess( const char * buf, size_t nbytes ) override;
    virtual void onShutdown( int32_t way ) override;
    virtual int32_t onPerform( int32_t type, void * task, int32_t interval ) override;

public:
    void setService( ChatRoomService * s ) { m_Service = s; }

private:
    ChatRoomService * m_Service = nullptr;
};

class ChatRoomService : public IIOService
{
public:
    ChatRoomService( uint8_t nthreads, uint32_t nclients );
    virtual ~ChatRoomService();

public:
    virtual IIOSession * onAccept( sid_t id, NetType type, uint16_t listenport, const char * host, uint16_t port ) override;

public:
    bool init( const char * host, uint16_t port );
    void run( int32_t mseconds );
    bool post( sid_t id, CSHead * header );

private:
    struct Task {
        uint16_t msgid = 0;
        uint16_t length = 0;
        sid_t sid = 0;
        char * message = nullptr;
        Task() = default;
    };

    uint32_t m_UniqueID;
    bool m_Perform;
    pthread_cond_t m_TaskCond;
    pthread_mutex_t m_TaskLock;
    std::deque<Task> m_TaskQueue;
    std::set<sid_t> m_SessionMap;
};

// -------------------------------------------------------------------------------
// -------------------------------------------------------------------------------
// -------------------------------------------------------------------------------

int32_t ChatRoomSession::onStart()
{
    CSHead head;
    head.msgid = 0;
    head.length = sizeof( head );
    m_Service->post( id(), &head );
    enablePersist();
    return 0;
}

ssize_t ChatRoomSession::onProcess( const char * buf, size_t nbytes )
{
    ssize_t nprocess = 0;

    while ( 1 ) {
        size_t nleft = nbytes - nprocess;
        const char * buffer = buf + nprocess;

        if ( nleft < sizeof( struct CSHead ) ) {
            break;
        }

        CSHead * head = (CSHead *)buffer;
        assert( head->length == CHATROOM_MESSAGE_SIZE + sizeof( CSHead ) );
        assert( head->msgid == 1 || head->msgid == 2 );

        if ( nleft < head->length ) {
            break;
        }

        m_Service->post( id(), head );
        nprocess += head->length;
    }

    return nprocess;
}

void ChatRoomSession::onShutdown( int32_t way )
{
    CSHead head;
    head.msgid = 3;
    head.length = sizeof( head );
    m_Service->post( id(), &head );
}

int32_t ChatRoomSession::onPerform( int32_t type, void * task, int32_t interval )
{
    printf( "Session:%lu, ID:%lu\n", id(), (uint64_t)( task ) );
    return 0;
}

ChatRoomService::ChatRoomService( uint8_t nthreads, uint32_t nclients )
    : IIOService( nthreads, nclients ),
      m_UniqueID( 0 ),
      m_Perform( false )
{
    pthread_cond_init( &m_TaskCond, nullptr );
    pthread_mutex_init( &m_TaskLock, nullptr );
}

ChatRoomService::~ChatRoomService()
{
    pthread_cond_destroy( &m_TaskCond );
    pthread_mutex_destroy( &m_TaskLock );
}

IIOSession * ChatRoomService::onAccept( sid_t id, NetType type, uint16_t listenport, const char * host, uint16_t port )
{
    ChatRoomSession * session = new ChatRoomSession;
    if ( session ) {
        session->setService( this );
    }
    return session;
}

bool ChatRoomService::init( const char * host, uint16_t port )
{
    bool rc = false;

    start();

    rc = listen( NetType::TCP, host, port );
    if ( !rc ) {
        printf( "ChatRoomService::listen(%s::%d) failed .\n", host, port );
        return false;
    }

    return true;
}

void ChatRoomService::run( int32_t seconds )
{
    std::deque<Task> swapqueue;
    pthread_mutex_lock( &m_TaskLock );
    std::swap( swapqueue, m_TaskQueue );
    pthread_mutex_unlock( &m_TaskLock );

    for ( auto & task : swapqueue ) {
        switch ( task.msgid ) {
            case 0 : {
                m_SessionMap.insert( task.sid );
            } break;

            case 1 : {
                uint16_t length = task.length + sizeof( CSHead );
                std::string buffer( length, 0 );
                CSHead * head = (CSHead *)buffer.data();
                head->msgid = task.msgid;
                head->length = length;
                memcpy( head+1, task.message, task.length );
                buffer.resize( length );
                send( task.sid, buffer );
            } break;

            case 2 : {
                uint16_t length = task.length + sizeof( CSHead );
                std::string buffer( length, 0 );
                CSHead * head = (CSHead *)buffer.data();
                head->msgid = task.msgid;
                head->length = length;
                memcpy( head + 1, task.message, task.length );
                buffer.resize( length );
                broadcast( buffer );
                free( task.message );
            } break;

            case 3 : {
                m_SessionMap.erase( task.sid );
            } break;
        }
    }

    struct timeval tv;
    gettimeofday( &tv, nullptr );

    struct timespec ts;
    int64_t next_usec = tv.tv_usec + (int64_t)seconds * 1000;
    ts.tv_sec = tv.tv_sec + (next_usec / 1000000);
    ts.tv_nsec = (next_usec % 1000000) * 1000;

    pthread_mutex_lock( &m_TaskLock );
    pthread_cond_timedwait( &m_TaskCond, &m_TaskLock, &ts );
    pthread_mutex_unlock( &m_TaskLock );
}

bool ChatRoomService::post( sid_t id, CSHead * header )
{
    Task task;
    task.sid = id;
    task.msgid = header->msgid;

    if ( header->length - sizeof( CSHead ) > 0 ) {
        task.length = header->length - sizeof( CSHead );

        assert( task.length == CHATROOM_MESSAGE_SIZE );

        task.message = (char *)malloc( task.length + 1 );
        memcpy( task.message, header + 1, task.length );
        task.message[task.length] = 0;
    }

    pthread_mutex_lock( &m_TaskLock );
    m_TaskQueue.push_back( task );
    pthread_mutex_unlock( &m_TaskLock );

    return true;
}

// -------------------------------------------------------------------------------
// -------------------------------------------------------------------------------
// -------------------------------------------------------------------------------

bool g_Running;

void signal_handle( int32_t signo )
{
    g_Running = false;
}

int main( int argc, char ** argv )
{
    if ( argc != 5 ) {
        printf( "chatroom_server [host] [port] [threads] [clients] \n" );
        return -1;
    }

    const char * host = argv[1];
    uint16_t port = atoi( argv[2] );
    uint8_t nthreads = atoi( argv[3] );
    uint32_t nclients = atoi( argv[4] );

    signal( SIGPIPE, SIG_IGN );
    signal( SIGINT, signal_handle );

    ChatRoomService service( nthreads, nclients );

    if ( !service.init( host, port ) ) {
        return -2;
    }

    g_Running = true;
    while ( g_Running ) {
        service.run( 20 );
    }

    service.halt();
    service.stop();
    return 0;
}
