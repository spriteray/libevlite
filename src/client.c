
#include <client.h>


static void _io_method( uint8_t index, evsets_t sets, int16_t type, void * task );

client_t client_start( const char * host, uint16_t port, 
						int32_t seconds, int32_t (*cb)(void *, int32_t), void * context )
{
	struct client * self = NULL;

	self = malloc( sizeof(struct client) );	
	if ( self == NULL )
	{
		return NULL;
	}
}

int32_t client_send( client_t self, const char * buf, uint32_t nbytes, int32_t iscopy )
{

}

int32_t client_set_keepalive( client_t self, int32_t seconds )
{
	// NOT Thread-Safe
	struct client * client = ( struct client * )self;

	if ( client->session == NULL )
	{
		return -1;
	}

	client->session->settings.keepalive_msecs = seconds*1000;
	return 0;

}

int32_t client_set_service( client_t self, ioservice_t * service, void * context )
{
	// NOT Thread-Safe
	struct client * client = ( struct client * )self;

	if ( client->session == NULL )
	{
		return -1;
	}

	client->session->context = context;
	client->session->service = *service;

	return 0;
}

// 客户端停止
int32_t client_stop( client_t self )
{

}

void _io_method( uint8_t index, evsets_t sets, int16_t type, void * task )
{
	switch ( type )
	{
	
	case eIOTaskType_Connect:
		{
			//
			_connect_direct( sets, (struct stask_connect *)task );
		}
		break;

	case eIOTaskType_Send :
		{
			// 发送数据
			_send_direct( (struct stask_send *)task );
		}
		break;

	}

	return;
}

