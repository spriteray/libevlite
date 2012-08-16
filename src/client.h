
#ifndef SRC_CLIENT_H
#define SRC_CLIENT_H

#include "session.h"

struct client
{
	char *		host;
	uint16_t	port;

	//
	//
	//
	
	iothreads_t layer;

	
};

//
// NOTICE: 网络任务的最大长度不超过56
//

struct ctask_send
{
	sid_t				id;			// 8bytes
	char *				buf;		// 8bytes
	uint32_t			nbytes;		// 4bytes+4bytes
	struct session *	manager;	// 8bytes
};

// 看样子内存对齐不需要使用了
#pragma pack(1)
#pragma pack()

#endif


