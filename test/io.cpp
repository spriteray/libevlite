
#include <stdlib.h>
#include <string.h>

#include "io.h"

namespace Utils
{

sid_t IIOSession::id() const
{
	return m_Sid;
}

void IIOSession::setTimeout( int32_t seconds )
{
	m_TimeoutSeconds = seconds;
}

void IIOSession::setKeepalive( int32_t seconds )
{
	m_KeepaliveSeconds = seconds;
}

int32_t IIOSession::send( const std::string & buffer )
{
	return send( buffer.c_str(), static_cast<uint32_t>(buffer.length()) );
}

int32_t IIOSession::send( const char * buffer, uint32_t nbytes, bool isfree )
{
	return iolayer_send( m_Layer, m_Sid, buffer, nbytes, static_cast<int32_t>(isfree) );
}

int32_t IIOSession::shutdown()
{
	return iolayer_shutdown( m_Layer, m_Sid );
}

void IIOSession::init( sid_t id, iolayer_t layer )
{
	m_Sid = id;
	m_Layer = layer;

	if ( m_TimeoutSeconds > 0 )
	{
		iolayer_set_timeout( m_Layer, m_Sid, m_TimeoutSeconds );
	}
	if ( m_KeepaliveSeconds > 0 )
	{
		iolayer_set_keepalive( m_Layer, m_Sid, m_KeepaliveSeconds );
	}
}

int32_t IIOSession::onProcessSession( void * context, const char * buf, uint32_t nbytes ) 
{
	IIOSession * session = static_cast<IIOSession *>( context );

	if ( session )
	{
		return session->onProcess( buf, nbytes );
	}

	return -1;
}

char * IIOSession::onTransformSession( void * context, const char * buf, uint32_t * nbytes )
{
	uint32_t & _nbytes = *nbytes;
	IIOSession * session = static_cast<IIOSession *>( context );

	if ( session )
	{
		return session->onTransform( buf, _nbytes );
	}

	return NULL;
}

int32_t IIOSession::onTimeoutSession( void * context ) 
{
	IIOSession * session = static_cast<IIOSession *>( context );

	if ( session )
	{
		return session->onTimeout();
	}

	return -1;
}

int32_t IIOSession::onKeepaliveSession( void * context )
{
	IIOSession * session = static_cast<IIOSession *>( context );

	if ( session )
	{
		return session->onKeepalive();
	}

	return -1;
} 

int32_t IIOSession::onErrorSession( void * context, int32_t result ) 
{
	IIOSession * session = static_cast<IIOSession *>( context );

	if ( session )
	{
		return session->onError( result );
	}

	return -1;
}

int32_t IIOSession::onShutdownSession( void * context ) 
{
	IIOSession * session = static_cast<IIOSession *>( context );

	if ( session )
	{
		int32_t rc = session->onShutdown();
		delete session;
		return rc;
	}

	return -1;
}

// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------

bool IIOService::start()
{
	m_IOLayer = iolayer_create( m_ThreadsCount, m_SessionsCount );
	return ( m_IOLayer != NULL );
}

void IIOService::stop()
{
	return iolayer_destroy( m_IOLayer );
}

bool IIOService::listen( const char * host, uint16_t port )
{
	return ( iolayer_listen( m_IOLayer, host, port, onAcceptSession, this ) == 0 );
}

bool IIOService::connect( const char * host, uint16_t port, int32_t seconds )
{
	return ( iolayer_connect( m_IOLayer, host, port, seconds, onConnectSession, this ) == 0 );
}

int32_t IIOService::send( sid_t id, const std::string & buffer )
{
	return send( id, buffer.c_str(), static_cast<uint32_t>(buffer.length()) );
}

int32_t IIOService::send( sid_t id, const char * buffer, uint32_t nbytes, bool isfree )
{
	return iolayer_send( m_IOLayer, id, buffer, nbytes, isfree );
}

int32_t IIOService::broadcast( const std::vector<sid_t> & ids, const std::string & buffer )
{
	return broadcast( ids, buffer.c_str(), static_cast<uint32_t>(buffer.length()) );
}

int32_t IIOService::broadcast( const std::vector<sid_t> & ids, const char * buffer, uint32_t nbytes )
{
	int32_t rc = 0;
	std::vector<sid_t>::const_iterator start = ids.begin();	

	uint32_t count = (uint32_t)ids.size();
	sid_t * idlist = const_cast<sid_t *>( &(*start) );

	rc = iolayer_broadcast( m_IOLayer, idlist, count, buffer, nbytes );
	if ( rc != 0 )
	{
		free( const_cast<char*>(buffer) );
	}

	return rc;
}

int32_t IIOService::shutdown( sid_t id )
{
	return iolayer_shutdown( m_IOLayer, id );
}

int32_t IIOService::shutdown( const std::vector<sid_t> & ids )
{
	std::vector<sid_t>::const_iterator start = ids.begin();

	uint32_t count = (uint32_t)ids.size();
	sid_t * idlist = const_cast<sid_t *>( &(*start) );

	return iolayer_shutdowns( m_IOLayer, idlist, count );
}

void IIOService::attach( sid_t id, IIOSession * session )
{
	session->init( id, m_IOLayer );

	ioservice_t ioservice;
	ioservice.process	= IIOSession::onProcessSession;
	ioservice.transform = IIOSession::onTransformSession;
	ioservice.timeout	= IIOSession::onTimeoutSession;
	ioservice.keepalive	= IIOSession::onKeepaliveSession;
	ioservice.error		= IIOSession::onErrorSession;
	ioservice.shutdown	= IIOSession::onShutdownSession;
	iolayer_set_service( m_IOLayer, id, &ioservice, session );
}

int32_t IIOService::onAcceptSession( void * context, sid_t id, const char * host, uint16_t port )
{
	IIOSession * session = NULL;
	IIOService * service = static_cast<IIOService*>( context );

	session = service->onAccept( id, host, port );
	if ( session == NULL )
	{
		return -1;
	}

	service->attach( id, session );
	return 0;
}

int32_t IIOService::onConnectSession( void * context, int32_t result, const char * host, uint16_t port, sid_t id )
{
	if ( result != 0 )
	{
		return 0;
	}
	
	IIOSession * session = NULL;
	IIOService * service = static_cast<IIOService *>( context );

	session = service->onConnect( id, host, port );
	if ( session == NULL )
	{
		return -1;
	}

	service->attach( id, session );
	return 0;
}

}


