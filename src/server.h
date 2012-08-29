
#ifndef SRC_SERVER_H
#define SRC_SERVER_H

#include <stdint.h>

#include "event.h"
#include "threads.h"
#include "network.h"

#include "session.h"

struct server
{
	uint8_t		nthreads;
	uint32_t	nclients;

	char *		host;
	uint16_t	port;

	int32_t		listenfd;

	iothreads_t group;
	void **		managers;
	
	void * 		acceptor;
};

struct acceptor
{
	int32_t 	fd;

	// 逻辑
	void * 		context;
	int32_t 	(*cb)(void *, sid_t, const char *, uint16_t);

	// 事件库
	event_t		evaccept;

	// 服务器句柄
	struct server * parent;
};


//
// NOTICE: 网络任务的最大长度不超过56
//

// NOTICE: stask_assign长度已经达到48bytes
struct stask_assign
{
	int32_t		fd;							// 4bytes

	uint16_t	port;						// 2bytes
	char 		host[INET_ADDRSTRLEN];		// 16bytes + 2bytes

	void *		context;					// 8bytes
	int32_t		(*cb)(void *, sid_t, const char *, uint16_t);	// 8bytes

	struct session_manager * manager;		// 8bytes
};

struct stask_send
{
	sid_t						id;			// 8bytes
	char *						buf;		// 8bytes
	uint32_t					nbytes;		// 4bytes+4bytes
	struct session_manager *	manager;	// 8bytes
};

struct stask_broadcast
{
	struct message *			msg;		// 8bytes
	struct session_manager *	manager;	// 8bytes	
};

struct stask_shutdown
{
	sid_t						id;			// 8bytes
	struct session_manager *	manager;	// 8bytes
};

struct stask_shutdowns
{
	struct sidlist *			ids;		// 8bytes
	struct session_manager *	manager;	// 8bytes	
};

// 看样子内存对齐不需要使用了
#pragma pack(1)
#pragma pack()

// 给当前线程分发一个会话
int32_t server_assign_session( struct server * self, uint8_t index, struct stask_assign * task );


#endif

