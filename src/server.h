
#ifndef SRC_SERVER_H
#define SRC_SERVER_H

#include "session.h"

struct server
{
	uint8_t		nthreads;
	uint32_t	nclients;

	char *		host;
	uint16_t	port;

	//
	//
	//
	
	iothreads_t group;

	void **		managers;
};

//
// NOTICE: 网络任务的最大长度不超过56
//

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

#endif

