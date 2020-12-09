
#ifndef DRIVER_H
#define DRIVER_H

#include "ikcp.h"
#include "event.h"
#include "message.h"

struct session;

struct driver
{
    ikcpcb *        kcp;    // kcp对象
    event_t         timer;  // 定时器
    int64_t         epoch;  // 开始时间
    struct buffer   buffer; // 接收缓冲区
};

// 创建驱动
struct driver * driver_create( struct session * s, struct buffer * buffer );

// 输入数据(KCP重组)
ssize_t driver_input( struct driver * self, struct buffer * buffer );

// 设置窗口, MTU, MINRTO
void driver_set_mtu( struct driver * self, int32_t mtu );
void driver_set_minrto( struct driver * self, int32_t minrto );
void driver_set_wndsize( struct driver * self, int32_t sndwnd, int32_t rcvwnd );

// 发送/接收数据
ssize_t driver_receive( struct session * self );
ssize_t driver_transmit( struct session * self );
ssize_t driver_send( struct session * self, char * buf, size_t nbytes );

// 销毁
void driver_destroy( struct driver * self );

#endif
