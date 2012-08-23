
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
	eIOTaskType_Assign		= 2,
	eIOTaskType_Connect		= 3,
	eIOTaskType_Send		= 4,
	eIOTaskType_Broadcast	= 5,
	eIOTaskType_Shutdown	= 6,
	eIOTaskType_Shutdowns	= 7,	
};


#endif

