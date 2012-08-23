
#ifndef SRC_SESSION_H
#define SRC_SESSION_H

/*
 * session 会话
 * 一个TCP连接的基本单元
 */

#include <stdint.h>
#include <netinet/in.h>

#define SESSION_READING		0x01
#define SESSION_WRITING		0x02
#define SESSION_EXITING		0x10

enum SessionType
{
	eSessionType_Once 		= 1,	// 临时
	eSessionType_Persist	= 2,	// 永久, 有断线重连的功能
};

struct session_setting
{
	int32_t timeout_secs;
	int32_t keepalive_secs;
	int32_t max_inbuffer_len;
};

struct session
{
	sid_t		id;

	int32_t 	fd;
	
	int8_t 		type;
	int8_t 		status;
	
	uint16_t	port;
	char 		host[INET_ADDRSTR_LEN];

	// 读写以及超时事件
	event_t 	evread;
	event_t 	evwrite;
	event_t		evkeepalive;

	// 事件集
	evsets_t	evsets;
	void *		manager;
	void *		context;
	ioservice_t service;
	
	// 接收缓冲区
	struct buffer		inbuffer;

	// 发送队列以及消息偏移量
	int32_t 			msgoffsets;
	struct arraylist 	outmsglist;

	// 会话的设置
	struct session_setting setting;
};

// 会话开始
int32_t session_start( struct session * self );

// 向session发送数据
int32_t session_send( struct session * self, char * buf, uint32_t nbytes );

// 向session的发送队列中添加一条消息
int32_t session_append( struct session * self, struct message * message );

// 会话添加读写事件
void session_add_event( struct session * self, int16_t ev );

// 会话结束
int32_t session_end( struct session * self );

// 会话开始连接远程服务器

struct session_manager
{


};

struct session * session_manager_alloc( struct session_manager * self, int8_t type, evsets_t sets );
struct session * session_manager_get( struct session_manager * self, sid_t id );

#endif

