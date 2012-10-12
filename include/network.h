
#ifndef NETWORK_H
#define NETWORK_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

//
// 网络层 
//
typedef uint64_t	sid_t;
typedef void *		iolayer_t;

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

// 创建网络层 
iolayer_t iolayer_create( uint8_t nthreads, uint32_t nclients );

// 服务器开启
//		host		- 绑定的地址
//		port		- 监听的端口号
//		cb			- 新会话创建成功后的回调,会被多个网络线程调用 
//							参数1: 上下文参数; 
//							参数2: 新会话ID; 
//							参数3: 会话的IP地址; 
//							参数4: 会话的端口号
//		context		- 上下文参数
int32_t iolayer_listen( iolayer_t self,
		const char * host, uint16_t port, 
		int32_t (*cb)( void *, sid_t, const char * , uint16_t ), void * context );

// 客户端开启
//		host		- 远程服务器的地址
//		port		- 远程服务器的端口
//		seconds		- 连接超时时间
//		cb			- 连接结果的回调
//							参数1: 上下文参数
//							参数2: 连接结果
//							参数3: 连接的远程服务器的地址
//							参数4: 连接的远程服务器的端口
//							参数5: 连接成功后返回的会话ID
//		context		- 上下文参数
int32_t iolayer_connect( iolayer_t self, 
		const char * host, uint16_t port, int32_t seconds, 
		int32_t (*cb)( void *, int32_t, const char *, uint16_t, sid_t), void * context );

// 会话参数的设置, 只能在ioservice_t中使用
int32_t iolayer_set_timeout( iolayer_t self, sid_t id, int32_t seconds );
int32_t iolayer_set_keepalive( iolayer_t self, sid_t id, int32_t seconds );
int32_t iolayer_set_service( iolayer_t self, sid_t id, ioservice_t * service, void * context );

// 发送数据到会话
// 指定不需要拷贝buf, 在发送失败的情况下, 需要手动释放buf
int32_t iolayer_send( iolayer_t self, sid_t id, const char * buf, uint32_t nbytes, int32_t iscopy );

// 广播数据到指定的会话
int32_t iolayer_broadcast( iolayer_t self, 
							sid_t * ids, uint32_t count, 
							const char * buf, uint32_t nbytes, int32_t iscopy );

// 终止指定的会话
int32_t iolayer_shutdown( iolayer_t self, sid_t id );
int32_t iolayer_shutdowns( iolayer_t self, sid_t * ids, uint32_t count );

// 销毁网络层
void iolayer_destroy( iolayer_t self );

#ifdef __cplusplus
}
#endif

#endif

