
#ifndef SRC_SESSION_H
#define SRC_SESSION_H

/*
 * session 会话
 * 一个TCP连接的基本单元
 */

#include <stdint.h>

struct session
{
	int8_t reading;
	int8_t writing;
};




// 向session发送数据
int32_t session_send( struct session * self, evsets_t sets, char * buf, uint32_t nbytes );

// 向session的发送队列中添加一条消息到
int32_t session_append( struct session * self, evsets_t sets, struct message * message );

struct session_manager
{


};

struct session * session_manager_get( struct session_manager * self, sid_t id );

#endif

