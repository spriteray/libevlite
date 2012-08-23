
#ifndef SRC_NETWORK_INTERNAL_H
#define SRC_NETWORK_INTERNAL_H

#include <stdint.h>

//
// 任务类型
//

enum
{
	eIOTaskType_Invalid		= 0,
	eIOTaskType_Listen		= 1,
	eIOTaskType_Connect		= 2,
	eIOTaskType_Send		= 3,
	eIOTaskType_Broadcast	= 4,
	eIOTaskType_Shutdown	= 5,
	eIOTaskType_Shutdowns	= 6,	
};

#define INET_ADDRSTR_LEN	16


#endif

