
#include <deque>
#include <vector>
#include <algorithm>

#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "io.h"
#include "chatroom.h"

using namespace Utils;

class CChatRoomService;
class CChatRoomSession : public IIOSession
{
public:
	CChatRoomSession();
	virtual ~CChatRoomSession();

public :
	virtual int32_t onStart();
	virtual int32_t	onProcess( const char * buf, uint32_t nbytes );
	virtual int32_t onShutdown();

public :
	void setService( CChatRoomService * s );

private :

	CChatRoomService * m_Service;
};

class CChatRoomService : public IIOService
{
public :
	CChatRoomService( uint8_t nthreads, uint32_t nclients );
	virtual ~CChatRoomService();

public :
	virtual IIOSession * onConnect( sid_t id, const char * host, uint16_t port );

public :
	bool init( const char * host, uint16_t port, uint32_t nclients );
	bool send2( sid_t id );
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
	setTimeout( 90 );
	setKeepalive( 10 );
	m_Service->send2( id() );
	return 0;
}

int32_t CChatRoomSession::onProcess( const char * buf, uint32_t nbytes )
{
	int32_t nprocess = 0;

	while ( 1 )
	{
		uint32_t nleft = nbytes - nprocess;
		const char * buffer = buf + nprocess;

		if ( nbytes < sizeof(struct CSHead) )
		{
			break;
		}
		
		CSHead * head = (CSHead *)buffer;

		if ( nleft < head->length ) 
		{
			break;
		}
		
		m_Service->send2( id() );	
		nprocess += head->length;
	}
	
	return nprocess;
}

int32_t CChatRoomSession::onShutdown()
{
	return 0;
}

void CChatRoomSession::setService( CChatRoomService * s )
{
	m_Service = s;
}


CChatRoomService::CChatRoomService( uint8_t nthreads, uint32_t nclients )
	: IIOService( nthreads, nclients )
{
}

CChatRoomService::~CChatRoomService()
{
}

IIOSession * CChatRoomService::onConnect( sid_t id, const char * host, uint16_t port )
{
	CChatRoomSession * session = new CChatRoomSession;
	if ( session )
	{
		session->setService( this );
	}

	return session;
}

bool CChatRoomService::init( const char * host, uint16_t port, uint32_t nclients )
{
	bool rc = false;

	rc = start();
	if ( !rc )
	{
		printf( "CChatRoomService::start() failed .\n" );
		return false;
	}

	for ( uint32_t i = 0; i < nclients; ++i )
	{
		rc = connect( host, port, 60 );
		if ( !rc )
		{
			printf("#%d connect(%s::%d) failed .\n", i, host, port);
		}
	}

	return true;
}

bool CChatRoomService::send2( sid_t id )
{
	uint16_t length = 100+sizeof(CSHead);
	std::string msg( length, 1 );

	CSHead * head = (CSHead *)msg.data();
	head->msgid = ( rand()%100 < 50 ) ? 1 : 2;
	head->length = length;
	msg.resize( length );
	
	send( id, msg );	
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
	if ( argc != 4 )
	{
		printf("chatroom_client [host] [port] [clients] \n");
		return -1;
	}

	const char * host = argv[1];
	uint16_t port = atoi(argv[2]);
	uint32_t nclients = atoi(argv[3]);
	uint8_t nthreads = nclients/1000 + 1;

	signal( SIGPIPE, SIG_IGN );
	signal( SIGINT, signal_handle );

	CChatRoomService service( nthreads, nclients );

	if ( !service.init(host, port, 60) )
	{
		return -2;
	}
	
	g_Running = true;
	while ( g_Running )
	{
		usleep(1000*10);
	}

	service.stop();
	return 0;
}


