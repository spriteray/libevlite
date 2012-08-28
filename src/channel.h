
#ifndef SRC_CHANNEL_H
#define SRC_CHANNEL_H

/*
 * channel 网络通道
 * 提供基于session的一切网络操作
 */

#include <stdint.h>
#include "session.h"

void channel_on_read( int32_t fd, int16_t ev, void * arg );
void channel_on_write( int32_t fd, int16_t ev, void * arg );
void channel_on_accept( int32_t fd, int16_t ev, void * arg );

// 会话出错
// 丢弃发送队列中的数据
int32_t channel_error( struct session * session, int32_t result );

// 尝试终止会话
// 该API会尽量把发送队列中的数据发送出去
// libevlite安全终止会话的核心模块
int32_t channel_try_shutdown( struct session * session );

#endif

