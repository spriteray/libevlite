
#ifndef ECHOSERVER_H 
#define ECHOSERVER_H

#include <vector>
#include <string>

#include "io.h"

//
// 回显服务实例
//

class CEchoSession : public Utils::IIOSession
{
public :
	CEchoSession()
	{
	}

	virtual ~CEchoSession()
	{
	}

public :

	virtual int32_t onProcess( const char * buf, uint32_t nbytes ); 
	virtual int32_t onTimeout(); 
	virtual int32_t onKeepalive(); 
	virtual int32_t onError( int32_t result ); 
	virtual int32_t onShutdown(); 
};

class CEchoService : public Utils::IIOService
{
public :

	CEchoService( uint8_t nthreads, uint32_t nclients )
		: Utils::IIOService( nthreads, nclients )
	{
	}

	virtual ~CEchoService()
	{
	}

public :

	Utils::IIOSession * onAccept( sid_t id, const char * host, uint16_t port );
};

#endif

