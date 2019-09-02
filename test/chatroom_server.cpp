
#include <deque>
#include <vector>
#include <algorithm>

#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>

#include "io.h"
#include "chatroom.h"

class TASK
{
public :
    TASK() {}
    ~TASK() {}

    static void * clone( void * t )
    {
        TASK * task = (TASK *)t;
        TASK * newtask = new TASK();
        newtask->data = task->data;

        return newtask;
    }

    static void perform( void * iocontext, void * t )
    {
        TASK * task = (TASK *)t;
        printf( "%p : %d \n", iocontext, task->data );
        delete task;
    }

public :
    int32_t data;
};

class CChatRoomService;
class CChatRoomSession : public IIOSession
{
public:
    CChatRoomSession();
    virtual ~CChatRoomSession();

public :
    virtual int32_t onStart();
    virtual ssize_t onProcess( const char * buf, size_t nbytes );
    virtual void onShutdown( int32_t way );
    virtual int32_t onPerform( int32_t type, void * task );

public :
    void setService( CChatRoomService * s );

private :
    CChatRoomService *    m_Service;
};

class CChatRoomService : public IIOService
{
public :
    CChatRoomService( uint8_t nthreads, uint32_t nclients );
    virtual ~CChatRoomService();

public :
    virtual IIOSession * onAccept( sid_t id, uint16_t listenport, const char * host, uint16_t port );

public :
    bool init( const char * host, uint16_t port );
    void run();
    bool post( sid_t id, CSHead * header );

private :
    struct Task
    {
        uint16_t msgid;
        uint16_t length;
        sid_t sid;
        char * message;

        Task()
        {
            msgid = 0;
            length = 0;
            sid = 0;
            message = NULL;
        }
    };

    uint32_t            m_UniqueID;
    bool                m_Perform;
    pthread_mutex_t     m_TaskLock;
    std::deque<Task>    m_TaskQueue;
    std::vector<sid_t>  m_SessionMap;
};

// -------------------------------------------------------------------------------
// -------------------------------------------------------------------------------
// -------------------------------------------------------------------------------

CChatRoomSession::CChatRoomSession()
{
}

CChatRoomSession::~CChatRoomSession()
{
}

int32_t CChatRoomSession::onStart()
{
    CSHead head;
    head.msgid = 0;
    head.length = sizeof(head);
    m_Service->post( id(), &head );

    enablePersist();

    return 0;
}

ssize_t CChatRoomSession::onProcess( const char * buf, size_t nbytes )
{
    ssize_t nprocess = 0;

    while ( 1 )
    {
        size_t nleft = nbytes - nprocess;
        const char * buffer = buf + nprocess;

        if ( nleft < sizeof(struct CSHead) )
        {
            break;
        }

        CSHead * head = (CSHead *)buffer;

        assert( head->length == CHATROOM_MESSAGE_SIZE+sizeof(CSHead));
        assert( head->msgid == 1 || head->msgid == 2 );

        if ( nleft < head->length )
        {
            break;
        }

        m_Service->post( id(), head );
        nprocess += head->length;
    }

    return nprocess;
}

void CChatRoomSession::onShutdown( int32_t way )
{
    CSHead head;
    head.msgid = 3;
    head.length = sizeof(head);
    m_Service->post( id(), &head );
}

int32_t CChatRoomSession::onPerform( int32_t type, void * task )
{
    printf( "Session:%lu, ID:%lu\n", id(), (uint64_t)(task) );
    return 0;
}

void CChatRoomSession::setService( CChatRoomService * s )
{
    m_Service = s;
}


CChatRoomService::CChatRoomService( uint8_t nthreads, uint32_t nclients )
    : IIOService( nthreads, nclients ),
      m_UniqueID( 0 ),
      m_Perform( false )
{
    m_SessionMap.reserve( nclients );
    pthread_mutex_init( &m_TaskLock, NULL );
}

CChatRoomService::~CChatRoomService()
{
    pthread_mutex_destroy( &m_TaskLock );
}

IIOSession * CChatRoomService::onAccept( sid_t id, uint16_t listenport, const char * host, uint16_t port )
{
    CChatRoomSession * session = new CChatRoomSession;
    if ( session )
    {
        session->setService( this );
    }

    return session;
}

bool CChatRoomService::init( const char * host, uint16_t port )
{
    bool rc = false;

    start();

    rc = listen( host, port );
    if ( !rc )
    {
        printf( "CChatRoomService::listen(%s::%d) failed .\n", host, port );
        return false;
    }

    return true;
}

void CChatRoomService::run()
{
    std::deque<Task> swapqueue;
    pthread_mutex_lock( &m_TaskLock );
    std::swap( swapqueue, m_TaskQueue );
    pthread_mutex_unlock( &m_TaskLock );

    for ( std::deque<Task>::iterator it = swapqueue.begin(); it != swapqueue.end(); ++it )
    {
        Task * task = &(*it);

        switch ( task->msgid )
        {
            case 0 :
                {
                    m_SessionMap.push_back( task->sid );
                }
                break;

            case 1 :
                {
                    uint16_t length = task->length+sizeof(CSHead);
#if 0
                    std::string buffer( length, 0 );
                    CSHead * head = (CSHead *)buffer.data();
                    head->msgid = task->msgid;
                    head->length = length;
                    memcpy( head+1, task->message, task->length );
                    buffer.resize( length );

                    send( task->sid, buffer );
#endif
                    char * buffer = (char *)malloc( length );
                    CSHead * head = (CSHead *)buffer;
                    head->msgid = task->msgid;
                    head->length = length;
                    memcpy( head+1, task->message, task->length );
                    send( task->sid, buffer, length, true );

                    free( task->message );
                }
                break;

            case 2 :
                {
                    uint16_t length = task->length+sizeof(CSHead);
                    std::string buffer( length, 0 );

                    CSHead * head = (CSHead *)buffer.data();
                    head->msgid = task->msgid;
                    head->length = length;
                    memcpy( head+1, task->message, task->length );
                    buffer.resize( length );

                    broadcast( /*m_SessionMap, */buffer );
                    free( task->message );
                }
                break;

            case 3 :
                {
                    std::vector<sid_t>::iterator it;

                    it = std::find( m_SessionMap.begin(), m_SessionMap.end(), task->sid );
                    if ( it != m_SessionMap.end() )
                    {
                        m_SessionMap.erase( it );
                    }
                }
                break;
        }
    }
}

bool CChatRoomService::post( sid_t id, CSHead * header )
{
    Task task;
    task.sid = id;
    task.msgid = header->msgid;

    if ( header->length - sizeof(CSHead) > 0 )
    {
        task.length = header->length - sizeof(CSHead);

        assert( task.length == CHATROOM_MESSAGE_SIZE );

        task.message = (char *)malloc( task.length+1 );
        memcpy( task.message, header+1, task.length );
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
    if ( argc != 5 )
    {
        printf("chatroom_server [host] [port] [threads] [clients] \n");
        return -1;
    }

    const char * host = argv[1];
    uint16_t port = atoi(argv[2]);
    uint8_t nthreads = atoi(argv[3]);
    uint32_t nclients = atoi(argv[4]);

    signal( SIGPIPE, SIG_IGN );
    signal( SIGINT, signal_handle );

    CChatRoomService service( nthreads, nclients );

    if ( !service.init(host, port ) )
    {
        return -2;
    }

    g_Running = true;
    while ( g_Running )
    {
        service.run();
        usleep(1000);
    }

    service.halt();
    service.stop();
    return 0;
}
