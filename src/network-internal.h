
#ifndef SRC_NETWORK_INTERNAL_H
#define SRC_NETWORK_INTERNAL_H

#include <stdint.h>

// 是否安全的终止会话
#define SAFE_SHUTDOWN                   1

// 发送队列的默认大小
#define DEFAULT_SENDQUEUE_SIZE          128

// 关闭前最大等待时间,默认10s
#define MAX_SECONDS_WAIT_FOR_SHUTDOWN   (10*1000)

// 尝试重连的间隔时间,默认为200ms
#define TRY_RECONNECT_INTERVAL          200

// 发送接收缓冲区设置
// 默认为0, 由内核动态控制
#define SEND_BUFFER_SIZE                0
#define RECV_BUFFER_SIZE                0

// 任务类型
enum
{
    eIOTaskType_Invalid         = 0,
    eIOTaskType_Listen          = 1,
    eIOTaskType_Assign          = 2,
    eIOTaskType_Connect         = 3,
    eIOTaskType_Associate       = 4,
    eIOTaskType_Send            = 5,
    eIOTaskType_Broadcast       = 6,
    eIOTaskType_Shutdown        = 7,
    eIOTaskType_Shutdowns       = 8,
    eIOTaskType_Broadcast2      = 9,
    eIOTaskType_Invoke          = 10,
    eIOTaskType_Perform         = 11,
};

// 网络服务错误码定义
enum
{
    eIOError_OutMemory          = 0x00010001,
    eIOError_ConnectStatus      = 0x00010002,   // 非法的连接状态
    eIOError_ConflictSid        = 0x00010003,   // 冲突的SID
    eIOError_InBufferFull       = 0x00010004,   // 缓冲区满了
    eIOError_ReadFailure        = 0x00010005,   // read()失败
    eIOError_PeerShutdown       = 0x00010006,   // 对端关闭了连接
    eIOError_WriteFailure       = 0x00010007,   // write()失败
    eIOError_ConnectFailure     = 0x00010008,   // 连接失败
    eIOError_Timeout            = 0x00010009,   // 连接超时了
    eIOError_SocketInvalid      = 0x0001000A,   // read()失败, Socket非法
    eIOError_InBufferInvalid    = 0x0001000B,   // read()失败, 接收缓冲区非法
    eIOError_ReadIOError        = 0x0001000C,   // read()失败, IO错误
    eIOError_ReadInvalid        = 0x0001000D,   // read()失败, EINVAL
    eIOError_SendQueueLimit     = 0x0001000E,   // 发送队列过大
};

#endif
