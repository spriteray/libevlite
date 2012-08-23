
#ifndef SRC_CHANNEL_H
#define SRC_CHANNEL_H

/*
 * channel 网络通道
 * 提供基于session的一切网络操作
 */

#include <stdint.h>

void channel_on_read( int32_t fd, int16_t ev, void * arg );
void channel_on_write( int32_t fd, int16_t ev, void * arg );
void channel_on_accept( int32_t fd, int16_t ev, void * arg );


int32_t channel_shutdown( struct session * session );

#endif

