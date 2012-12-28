
#ifndef SRC_CHANNEL_H
#define SRC_CHANNEL_H

/*
 * channel 网络通道
 * 提供基于session的一切网络操作
 */

#include <stdint.h>
#include "session.h"

//
int32_t channel_send( struct session * session, char * buf, uint32_t nbytes );

// 会话出错
// 丢弃发送队列中的数据
int32_t channel_error( struct session * session, int32_t result );

// 会话关闭
// 丢弃发送队列中的数据
int32_t channel_shutdown( struct session * session );

// 事件的回调函数集合
void channel_on_read( int32_t fd, int16_t ev, void * arg );
void channel_on_write( int32_t fd, int16_t ev, void * arg );
void channel_on_accept( int32_t fd, int16_t ev, void * arg );
void channel_on_connect( int32_t fd, int16_t ev, void * arg );
void channel_on_reconnect( int32_t fd, int16_t ev, void * arg );
void channel_on_keepalive( int32_t fd, int16_t ev, void * arg );

// 尝试重连
void channel_on_tryreconnect( int32_t fd, int16_t ev, void * arg );

#endif

