
#ifndef NETWORK_H
#define NETWORK_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

//
// IO服务
//
typedef struct
{
	int32_t (*process)( void * context, const char * buf, uint32_t nbytes );
	int32_t (*timeout)( void * context );
	int32_t (*keepalive)( void * context );
	int32_t (*error)( void * context, int32_t result );
	int32_t (*shutdown)( void * context );
}ioservice_t;

//
// 服务器
//
typedef uint64_t sid_t;
typedef void * server_t;

server_t server_create( const char * host, uint16_t port, uint32_t nthreads, uint32_t nclients );
void server_destroy( server_t self );

int32_t server_start( server_t self, int32_t (*cb)(void *, sid_t, const char *, uint16_t), void * context );
int32_t server_stop( server_t self );

int32_t server_set_timeout( server_t self, sid_t id, int32_t seconds );
int32_t server_set_keepalive( server_t self, sid_t id, int32_t seconds );
int32_t server_set_service( server_t self, sid_t id, ioservice_t * service, void * context );

int32_t server_send( server_t self, sid_t id, const char * buf, uint32_t nbytes, int32_t iscopy );
int32_t server_broadcast( server_t self, sid_t * ids, uint32_t count, const char * buf, uint32_t nbytes, int32_t iscopy );
int32_t server_shutdown( server_t self, sid_t id );
int32_t server_shutdowns( server_t self, sid_t * ids, uint32_t count );

//
// 客户端
//
typedef void * client_t;

client_t client_create(	const char * host, uint16_t port );
void client_destroy( client_t self );

int32_t client_start( client_t self, int32_t seconds, int32_t (*cb)(void *, int32_t), void * context );
int32_t client_send( client_t self, const char * buf, uint32_t nbytes, int32_t iscopy );
int32_t client_stop( client_t self );

int32_t client_set_keepalive( client_t self, int32_t seconds );
int32_t client_set_service( client_t self, ioservice_t * service, void * context );


#ifdef __cplusplus
}
#endif

#endif

