
#include <string>

#include <signal.h>
#include <stdio.h>
#include <errno.h>

#include "utils.h"
#include "event.h"
#include "message.h"

#include "chatroom.h"

bool g_Running;
bool g_Waiting;

char * g_Host;
uint16_t g_Port;
int32_t g_ClientsCount;

class ChatRoomClient
{
public :
    ChatRoomClient()
        : m_Fd(0),
          m_EventSets(NULL),
          m_ReadEvent(NULL),
          m_TimerEvent(NULL),
          m_SendInterval(0),
          m_SendBytes(0),
          m_RecvBytes(0)
    {
        buffer_init( &m_InBuffer );
    }

    ~ChatRoomClient()
    {
        buffer_clear( &m_InBuffer );
    }

    enum
    {
        e_SendIntervalMicroSeconds    = 500,        // 每个100ms发送一个请求
    };

public :
    bool connect( const char * host, uint16_t port )
    {
        struct sockaddr_in addr;

        memset( &addr, 0, sizeof(addr) );
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr( host );
        addr.sin_port = htons( port );

        m_Fd = socket( AF_INET, SOCK_STREAM, 0 );
        if ( m_Fd < 0 )
        {
            return false;
        }
        if ( ::connect(m_Fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 )
        {
            close( m_Fd );
            m_Fd = -1;
            return false;
        }
        set_non_block( m_Fd );
        return true;
    }

    void start( evsets_t sets )
    {
        m_EventSets = sets;

        m_ReadEvent = event_create();
        m_TimerEvent = event_create();

        event_set( m_ReadEvent, m_Fd, EV_READ|EV_PERSIST );
        event_set_callback( m_ReadEvent, ChatRoomClient::onRead, this );
        evsets_add( sets, m_ReadEvent, 0 );

        addTimer();
    }

    int32_t recv()
    {
        return buffer_read( &m_InBuffer, m_Fd, -1 );
    }

    int32_t send()
    {
        uint16_t length = CHATROOM_MESSAGE_SIZE+sizeof(CSHead);
        std::string msg( length, 0 );

        CSHead * head = (CSHead *)msg.data();
        head->msgid = 1;// (rand()%1000) > 50 ? 1 : 2;
        head->length = length;
        msg.resize( length );

        m_SendBytes += length;

        return write( m_Fd, msg.data(), msg.size() );
    }

    void shutdown()
    {
        if ( m_Fd )
        {
            close( m_Fd );
            m_Fd = -1;
        }

        if ( m_ReadEvent )
        {
            evsets_del( event_get_sets(m_ReadEvent), m_ReadEvent );
            event_destroy( m_ReadEvent );
        }

        if ( m_TimerEvent )
        {
            evsets_del( event_get_sets(m_TimerEvent), m_TimerEvent );
            event_destroy( m_TimerEvent );
        }
    }

    void onProcess()
    {
        int32_t nprocess = 0;

        const char * buf = buffer_data( &m_InBuffer );
        uint32_t nbytes = buffer_length( &m_InBuffer );

        while ( 1 )
        {
            uint32_t nleft = nbytes - nprocess;
            const char * buffer = buf + nprocess;

            if ( nleft < sizeof(struct CSHead) )
            {
                break;
            }

            CSHead * head = (CSHead *)buffer;

            if ( nleft < head->length )
            {
                break;
            }

            assert( head->length == CHATROOM_MESSAGE_SIZE+sizeof(CSHead) );
            assert( head->msgid == 1 || head->msgid == 2 );

            m_RecvBytes += head->length;
            nprocess += head->length;
        }

        buffer_erase( &m_InBuffer, nprocess );
    }

    void addTimer()
    {
        uint32_t seconds = e_SendIntervalMicroSeconds;

        event_set( m_TimerEvent, -1, 0 );
        event_set_callback( m_TimerEvent, ChatRoomClient::onTimer, this );
        evsets_add( m_EventSets,  m_TimerEvent, seconds );
    }

    uint64_t getSendBytes() const { return m_SendBytes; }
    uint64_t getRecvBytes() const { return m_RecvBytes; }

public :
    static void onRead( int32_t fd, int16_t ev, void * arg );
    static void onTimer( int32_t fd, int16_t ev, void * arg );

private :
    int32_t         m_Fd;
    evsets_t        m_EventSets;

    event_t         m_ReadEvent;
    event_t         m_TimerEvent;
    struct buffer   m_InBuffer;

    uint32_t        m_SendInterval;

    uint64_t        m_SendBytes;
    uint64_t        m_RecvBytes;
};

void ChatRoomClient::onRead( int32_t fd, int16_t ev, void * arg )
{
    ChatRoomClient * client = (ChatRoomClient *)arg;

    if ( ev & EV_READ )
    {
        int32_t nread = client->recv();
        if ( nread <= 0 )
        {
            if ( nread == 0
                    || ( nread < 0 && errno == EAGAIN ) )
            {
                client->shutdown();
            }
            return;
        }

        client->onProcess();
    }
}

void ChatRoomClient::onTimer( int32_t fd, int16_t ev, void * arg )
{
    ChatRoomClient * client = (ChatRoomClient *)arg;
    client->send();
    client->addTimer();
}

// -------------------------------------------------------------------------------
// -------------------------------------------------------------------------------
// -------------------------------------------------------------------------------

void signal_handle( int32_t signo )
{
    g_Running = false;
    g_Waiting = false;
}

void start_clients( ChatRoomClient ** clients, evsets_t sets )
{
    for ( int32_t i = 0; i < g_ClientsCount; ++i )
    {
        ChatRoomClient * client = new ChatRoomClient;
        clients[ i ] = client;

        if ( client )
        {
            if ( !client->connect(g_Host, g_Port) )
            {
                delete client;
            }

            client->start( sets );
        }
    }
}

void statics( ChatRoomClient ** clients, uint64_t mseconds )
{
    uint64_t total_recvbytes = 0;
    uint64_t total_sendbytes = 0;

    for ( int32_t i = 0; i < g_ClientsCount; ++i )
    {
        total_recvbytes += clients[i]->getRecvBytes();
        total_sendbytes += clients[i]->getSendBytes();
    }

    printf("TotalRecvBytes: %lld, TotalSendBytes: %lld\n", total_recvbytes, total_sendbytes);
    printf("MSeconds: %lld, RecvSpeed: %6.3fKBytes/s, SendSpeed: %6.3fKBytes/s \n", mseconds, (double)total_recvbytes/(double)mseconds, (double)total_sendbytes/(double)mseconds );
}

int main( int argc, char ** argv )
{
    if ( argc != 4 )
    {
        printf("chatroom_client [host] [port] [clients] .\n");
        return -1;
    }

    g_Host = strdup( argv[1] );
    g_Port = atoi( argv[2] );
    g_ClientsCount = atoi( argv[3] );

    signal( SIGINT, signal_handle );
    signal( SIGPIPE, SIG_IGN );
    srand( time(NULL) );

    //
    ChatRoomClient ** clients = (ChatRoomClient **)malloc( sizeof(ChatRoomClient *) * g_ClientsCount );

    assert( clients != NULL && "malloc() failed " );

    evsets_t sets = evsets_create();
    assert( sets != NULL && "evsets_create() failed" );

    // 连接远程服务器
    start_clients( clients, sets );

    // 运行
    g_Running = true;
    g_Waiting = true;
    uint64_t nStartTime, nEndTime;

    nStartTime = mtime();
    while ( g_Running )
    {
        evsets_dispatch( sets );
    }
    nEndTime = mtime();

    printf("chatroom_client stopped .\n");
    g_Waiting = true;
    while ( g_Waiting )
    {
        pause();
    }

    // 统计
    statics( clients, nEndTime-nStartTime );

    free( clients );
    return 0;
}
