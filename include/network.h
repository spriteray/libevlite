
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
//		process()	- 收到数据包的回调
//						返回值为处理掉的数据包, <0: 处理出错
//
//		timeout()	- 超时的回调
//		keepalive()	- 保活定时器超时的回调
//		error()		- 出错的回调
//		shutdown()	- 会话终止时的回调
//						返回非0, libevlite会终止会话
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

// 服务器开启
//		host		- 绑定的地址
//		port		- 监听的端口号
//		nthreads	- 开启多少个线程
//		nclients	- 最大支持多少个客户端
//		cb			- 新会话创建成功后的回调,会被多个网络线程调用 
//							参数1: 上下文参数; 
//							参数2: 新会话ID; 
//							参数3: 会话的IP地址; 
//							参数4: 会话的端口号
//		context		- 上下文参数
server_t server_start( const char * host, uint16_t port,
							uint8_t nthreads, uint32_t nclients, 
							int32_t (*cb)(void *, sid_t, const char *, uint16_t), void * context );

// 会话参数的设置, 只能在ioservice_t中使用
int32_t server_set_timeout( server_t self, sid_t id, int32_t seconds );
int32_t server_set_keepalive( server_t self, sid_t id, int32_t seconds );
int32_t server_set_service( server_t self, sid_t id, ioservice_t * service, void * context );

// 服务器发送数据到会话
int32_t server_send( server_t self, sid_t id, const char * buf, uint32_t nbytes, int32_t iscopy );

// 服务器广播数据到指定的会话
int32_t server_broadcast( server_t self, 
							sid_t * ids, uint32_t count, 
							const char * buf, uint32_t nbytes, int32_t iscopy );

// 服务器终止指定的会话
int32_t server_shutdown( server_t self, sid_t id );
int32_t server_shutdowns( server_t self, sid_t * ids, uint32_t count );

// 服务器停止
void server_stop( server_t self );

//
// 客户端
//
typedef void * client_t;

// 客户端开启
//		host		- 远程服务器
//		port		- 远程端口号
//		seconds		- 连接远程服务器超时时间
//		cb			- 连接成功后的回调
//							参数1: 上下文参数
//							参数2: 连接结果, 0: 成功, !=0: 失败
//		context		- 上下文参数
client_t client_start( const char * host, uint16_t port, 
						int32_t seconds, int32_t (*cb)(void *, int32_t), void * context );

// 向服务器发送数据
int32_t client_send( client_t self, const char * buf, uint32_t nbytes, int32_t iscopy );

// 客户端参数的设置
int32_t client_set_keepalive( client_t self, int32_t seconds );
int32_t client_set_service( client_t self, ioservice_t * service, void * context );

// 客户端停止
int32_t client_stop( client_t self );

#ifdef __cplusplus
}
#endif

#endif

