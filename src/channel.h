
#ifndef SRC_CHANNEL_H
#define SRC_CHANNEL_H

/*
 * channel 网络通道
 * 提供基于session的一切网络操作
 */

#include <stdint.h>


int32_t channel_shutdown( struct session * session );

#endif

