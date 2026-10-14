
#ifndef DRIVER_H
#define DRIVER_H

#include "ikcp.h"
#include "event.h"
#include "message.h"
#include "network.h"

struct session;
struct udpentry;
struct ephashtable;

struct driver
{
    ikcpcb * kcp;                 // kcp对象
    int32_t mtu;                  // 待生效的MTU, 0表示无请求
    event_t timer;                // 定时器
    uint32_t deadline;            // 定时器到期时间(kcp时钟)
    int64_t epoch;                // 开始时间
    struct udpentry * entry;      // UDP条目
    struct sockaddr_storage addr; // 对端地址
};

// 创建驱动
struct driver * driver_create(
    struct session * s, struct buffer * buffer, const options_t * option );

int32_t driver_set_endpoint( struct driver * self,
    struct ephashtable * table, int32_t family, const char * host, uint16_t port );

// 输入数据(KCP重组), 返回接收字节数; -2: 接收缓冲区扩容失败
ssize_t driver_input( struct driver * self, struct buffer * in, struct buffer * out );

// 设置窗口, MTU, MINRTO
// 0: 已接受(可能延后生效); -1: 非法MTU; -2: 立即应用时分配失败
int32_t driver_set_mtu( struct driver * self, int32_t mtu );
void driver_set_minrto( struct driver * self, int32_t minrto );
void driver_set_wndsize( struct driver * self, int32_t sndwnd, int32_t rcvwnd );
void driver_set_conv( struct driver * self, uint32_t conv );

// 发送/接收数据
ssize_t driver_transmit( struct session * self );
ssize_t driver_send( struct session * self, char * buf, size_t nbytes );
// Number of queued or unacknowledged KCP segments.
int32_t driver_pending( struct driver * self );

// 销毁
void driver_destroy( struct driver * self );

#endif
