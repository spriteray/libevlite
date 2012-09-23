
#ifndef SRC_CLIENT_H
#define SRC_CLIENT_H

#include "threads.h"
#include "session.h"

struct client
{
	// 网络线程组, 只有1个线程
	iothreads_t		group;

	struct session	session;
	
	// 连接器
	void *			connector;	
};

// 连接器
struct connector
{
	int32_t		fd;

	// 连接事件
	event_t		event;

	//
	evsets_t			sets;
	struct session *	session;

	// 连接服务器的地址和端口号
	char		host[INET_ADDRSTRLEN];
	uint16_t	port;

	// 逻辑
	int32_t		seconds;
	void *		context;
	int32_t		(*cb)( void *, int32_t );
};

//
// NOTICE: 网络任务的最大长度不超过56
//

struct ctask_send
{
	char *				buf;		// 8bytes
	uint32_t			nbytes;		// 4bytes+4bytes
	struct session *	session;	// 8bytes
};

// 看样子内存对齐不需要使用了
#pragma pack(1)
#pragma pack()

int32_t client_reconnect( struct connector * connector );

#endif


