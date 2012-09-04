
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

#define OUTMSGLIST_DEFAULT_SIZE		8			// 发送队列的默认大小
#define INBUFFER_DEFAULT_SIZE		4096


/*
 * 网络服务错误原因定义
 */
enum
{
	eIOError_OutMemory 			= 0x00010001,
	eIOError_ConnectStatus		= 0x00010002,	// 非法的连接状态
	eIOError_ConflictSid		= 0x00010003,	// 冲突的SID
	eIOError_InBufferFull		= 0x00010004,	// 缓冲区满了
	eIOError_ReadFailure		= 0x00010005,	// read()失败
	eIOError_PeerShutdown		= 0x00010006,	// 对端关闭了连接
	eIOError_WriteFailure		= 0x00010007, 	// write()失败
	eIOError_ConnectFailure		= 0x00010008,	// 连接失败
};


#endif

