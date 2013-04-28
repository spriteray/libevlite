
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <stdint.h>

#include "network.h"

#define METHOD		1

struct session
{
	sid_t id;
	iolayer_t layer;
};

struct PacketHead
{
	uint16_t cmd;
	uint16_t ext;
	uint32_t len;
};

int32_t onStart( void * context )
{
	return 0;
}

int32_t onProcess( void * context, const char * buf, uint32_t nbytes ) 
{
	int32_t nprocess = 0;
	struct session * s = (struct session *)context;

#if METHOD

	iolayer_send( s->layer, s->id, buf, nbytes, 0 );
	nprocess = nbytes;

#else	

	while ( 1 )
	{
		uint32_t nleft = nbytes - nprocess;
		const char * buffer = buf + nprocess;

		if ( nleft < sizeof(struct PacketHead) )
		{
			break;
		}
		
		struct PacketHead * head = (struct PacketHead *)buffer;
		uint32_t size = head->len+sizeof(struct PacketHead);

		if ( nleft < size ) 
		{
			break;
		}

		iolayer_send( s->layer, s->id, buffer, size, 0 );
		nprocess += size; 
	}

#endif

	return nprocess;
}

char * onTransform( void * context, const char * buf, uint32_t * nbytes )
{
	return (char *)buf;
}

int32_t onTimeout( void * context ) 
{
	return 0;
}

int32_t onKeepalive( void * context )
{
	return 0;
} 

int32_t onError( void * context, int32_t result ) 
{
	return 0;
}

void onShutdown( void * context, int32_t way ) 
{
	struct session * s = (struct session *)context;
	free( s );
}

char * onLayerTransform( void * context, const char * buf, uint32_t * nbytes )
{
	return (char *)buf;
}

int32_t onLayerAccept( void * context, sid_t id, const char * host, uint16_t port )
{
	iolayer_t layer = (iolayer_t)context;
	struct session * session = malloc( sizeof(struct session) );
	
	if ( session )
	{
		session->id = id;
		session->layer = layer;
		
		ioservice_t ioservice;
		ioservice.start		= onStart;
		ioservice.process	= onProcess;
		ioservice.transform = onTransform;
		ioservice.timeout	= onTimeout;
		ioservice.keepalive	= onKeepalive;
		ioservice.error		= onError;
		ioservice.shutdown	= onShutdown;
		iolayer_set_service( layer, id, &ioservice, session );
	}

	return 0;
}


int32_t g_Running;

void signal_handle( int32_t signo )
{
	g_Running = 0;
}

int main( int32_t argc, char ** argv )
{
	if ( argc != 4 )
	{
		printf("pingpong [host] [port] [threads] \n");
		return -1;
	}

	char * host = argv[1];
	uint16_t port = atoi(argv[2]);
	uint8_t nthreads = atoi(argv[3]);

	signal( SIGPIPE, SIG_IGN );
	signal( SIGINT, signal_handle );

	iolayer_t layer = iolayer_create( nthreads, 20000 );
	if ( layer == NULL )
	{
		return -2;
	}

	iolayer_set_transform( layer, onLayerTransform, layer );
	iolayer_listen( layer, host, port, onLayerAccept, layer) ;
	
	g_Running = 1;	

	while ( g_Running )
	{
		sleep(10);
	}
	
	iolayer_destroy( layer );

	return 0;
}

