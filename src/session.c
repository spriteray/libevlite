
#include <assert.h>
#include <syslog.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

#include "iolayer.h"
#include "channel.h"
#include "network-internal.h"

#include "session.h"

static struct session * _new_session( uint32_t size );
static int32_t _del_session( struct session * self );

struct session * _new_session( uint32_t size )
{
	struct session * self = NULL;

	self = calloc( 1, sizeof(struct session) );
	if ( self == NULL )
	{
		return NULL;
	}

	// 初始化网络事件
	self->evread = event_create();
	self->evwrite = event_create();
	self->evkeepalive = event_create();
	
	if ( self->evkeepalive == NULL 
			|| self->evread == NULL || self->evwrite == NULL )
	{
		_del_session( self );
		return NULL;
	}
	
	// 初始化发送队列	
	if ( arraylist_init(&self->outmsglist, size) != 0 )
	{
		_del_session( self );
		return NULL;
	}

	return self;
}

int32_t _del_session( struct session * self )
{
	// 销毁网络事件
	if ( self->evread )
	{
		event_destroy( self->evread );
		self->evread = NULL;
	}
	if ( self->evwrite )
	{
		event_destroy( self->evwrite );
		self->evwrite = NULL;
	}
	if ( self->evkeepalive )
	{
		event_destroy( self->evkeepalive );
		self->evkeepalive = NULL;
	}
	
	// 销毁发送队列
	arraylist_final( &self->outmsglist );

	// 销毁接收缓冲区
	buffer_set( &self->inbuffer, NULL, 0 );

	free( self );

	return 0;
}

int32_t session_start( struct session * self, int8_t type, int32_t fd, evsets_t sets )
{
	self->fd		= fd;
	self->type		= type;
	self->evsets	= sets;

	// TODO: 设置默认的最大接收缓冲区
	
	// 不需要每次开始会话的时候初始化
	// 只需要在manager创建会话的时候初始化一次，即可	

	// 关注读事件, 按需开启保活心跳
	session_add_event( self, EV_READ|EV_ET );
	session_start_keepalive( self );

	return 0;
}

void session_set_endpoint( struct session * self, char * host, uint16_t port )
{
	self->port = port;
	strncpy( self->host, host, INET_ADDRSTRLEN );
}

//
int32_t session_send( struct session * self, char * buf, uint32_t nbytes )
{
	// 等待关闭的连接

	if ( self->status&SESSION_EXITING )
	{
		free( buf );
		return -1;
	}

	// 判断session是否繁忙
	if ( (self->status&SESSION_WRITING) 
		|| arraylist_count(&self->outmsglist) > 0 )
	{
		// 创建message, 添加到发送队列中
		struct message * message = message_create();
		if ( message == NULL )
		{
			free( buf );
			return -2;
		}

		message_add_receiver( message, self->id );
		message_set_buffer( message, buf, nbytes );
		arraylist_append( &self->outmsglist, message );
		session_add_event( self, EV_WRITE|EV_ET );
		return 0;
	}

	// 直接发送
	return channel_send( self, buf, nbytes ); 
}

//
int32_t session_append( struct session * self, struct message * message )
{
	int32_t rc = 0;

	// 添加到会话的发送列表中
	rc = arraylist_append( &self->outmsglist, message );
	if ( rc == 0 )
	{
		// 注册写事件, 处理发送队列
		session_add_event( self, EV_WRITE|EV_ET );
	}

	return rc;
}

// 注册网络事件 
void session_add_event( struct session * self, int16_t ev )
{
	int8_t status = self->status;
	evsets_t sets = self->evsets;

	// 注册读事件
	// 不在等待读事件的正常会话
	if ( !(status&SESSION_EXITING)
		&& (ev&EV_READ) && !(status&SESSION_READING) )
	{
		event_set( self->evread, self->fd, ev );
		event_set_callback( self->evread, channel_on_read, self );
		evsets_add( sets, self->evread, self->setting.timeout_msecs );

		self->status |= SESSION_READING;
	}

	// 注册写事件
	if ( (ev&EV_WRITE) && !(status&SESSION_WRITING) )
	{
		int32_t wait_for_shutdown = 0;
		
		// 在等待退出的会话上总是会添加10s的定时器
		if ( status&SESSION_EXITING )
		{
			// 对端主机崩溃 + Socket缓冲区满的情况下
			// 会一直等不到EV_WRITE发生, 这时libevlite就出现了会话泄漏
			wait_for_shutdown = MAX_SECONDS_WAIT_FOR_SHUTDOWN;
		}

		event_set( self->evwrite, self->fd, ev );
		event_set_callback( self->evwrite, channel_on_write, self );
		evsets_add( sets, self->evwrite, wait_for_shutdown );

		self->status |= SESSION_WRITING;
	}

	return;
}

// 反注册网络事件
void session_del_event( struct session * self, int16_t ev )
{
	int8_t status = self->status;
	evsets_t sets = self->evsets;

	if ( (ev&EV_READ) && (status&SESSION_READING) )
	{
		evsets_del( sets, self->evread );
		self->status &= ~SESSION_READING;
	}

	if ( (ev&EV_WRITE) && (status&SESSION_WRITING) )
	{
		evsets_del( sets, self->evread );
		self->status &= ~SESSION_WRITING;
	}

	return;
}

int32_t session_start_keepalive( struct session * self )
{
	int8_t status = self->status;
	evsets_t sets = self->evsets;

	if ( self->setting.keepalive_msecs > 0 && !(status&SESSION_KEEPALIVING) )
	{
		event_set( self->evkeepalive, -1, 0 );
		event_set_callback( self->evkeepalive, channel_on_keepalive, self );
		evsets_add( sets, self->evkeepalive, self->setting.keepalive_msecs );

		self->status |= SESSION_KEEPALIVING;
	}	

	return 0;
}

int32_t session_start_reconnect( struct session * self )
{
	evsets_t sets = self->evsets;

	if ( self->status&SESSION_EXITING )
	{
		// 会话等待退出,
		return -1;
	}

	if ( self->status&SESSION_WRITING )
	{
		// 正在等待写事件的情况下
		return -2;
	}

	// 删除网络事件
	if ( self->status&SESSION_READING )
	{
		evsets_del( sets, self->evread );
		self->status &= ~SESSION_READING;
	}
	if ( self->status&SESSION_WRITING )
	{
		evsets_del( sets, self->evwrite );
		self->status &= ~SESSION_WRITING;
	}
	if ( self->status&SESSION_KEEPALIVING )
	{
		evsets_del( sets, self->evkeepalive );
		self->status &= ~SESSION_KEEPALIVING;
	}

	// 终止描述符
	if ( self->fd > 0 )
	{
		close( self->fd );
	}

	// 连接远程服务器
	self->fd = tcp_connect( self->host, self->port, 1 ); 
	if ( self->fd < 0 )
	{
		return channel_error( self, eIOError_ConnectFailure );
	}

	event_set( self->evwrite, self->fd, EV_WRITE|EV_ET );
	event_set_callback( self->evwrite, channel_on_reconnect, self );
	evsets_add( sets, self->evwrite, 0 );
	self->status |= SESSION_WRITING;

	return 0;
}

int32_t session_shutdown( struct session * self )
{
	if ( !(self->status&SESSION_EXITING)
		&& arraylist_count(&self->outmsglist) > 0 )
	{
		// 会话状态正常, 并且发送队列不为空
		// 尝试继续把未发送的数据发送出去, 在终止会话
		self->status |= SESSION_EXITING;

		// 删除读事件
		session_del_event( self, EV_READ );
		// 注册写事件
		session_add_event( self, EV_WRITE|EV_ET );

		return 1;
	}

	return channel_shutdown( self );
}

int32_t session_end( struct session * self, sid_t id )
{
	// 由于会话已经从管理器中删除了
	// 会话中的ID已经非法

	// 清空发送队列
	int32_t count = arraylist_count( &self->outmsglist );
	if ( count > 0 )
	{
		// 会话终止时发送队列不为空
		syslog(LOG_WARNING, "session_end(SID=%ld)'s Out-Message-List (%d) is not empty .", id, count );

		for ( ; count > 0; --count )
		{
			struct message * msg = arraylist_take( &self->outmsglist, -1 );
			if ( msg )
			{
				// 消息发送失败一次
				message_add_failure( msg, self->id );

				// 检查消息是否可以销毁了
				if ( message_is_complete(msg) == 0 )
				{
					message_destroy( msg );
				}
			}
		}
	}
	
	// 清空接收缓冲区
	buffer_erase( &self->inbuffer, buffer_length(&self->inbuffer) );

	// 删除事件
	if ( self->evread && (self->status&SESSION_READING) )
	{
		evsets_del( self->evsets, self->evread );
		self->status &= ~SESSION_READING;
	}
	if ( self->evwrite && (self->status&SESSION_WRITING) )
	{
		evsets_del( self->evsets, self->evwrite );
		self->status &= ~SESSION_WRITING;
	}
	if ( self->evkeepalive && (self->status&SESSION_KEEPALIVING) )
	{
		evsets_del( self->evsets, self->evkeepalive );
		self->status &= ~SESSION_KEEPALIVING;
	}
	
	// 重置部分数据
	self->id = 0;
	self->type = 0;
	self->status = 0;
	self->port = 0;
	self->host[0] = 0;
	self->evsets = NULL;
	self->context = NULL;
	self->service.error = NULL;
	self->service.timeout = NULL;
	self->service.process = NULL;
	self->service.shutdown = NULL;
	self->service.keepalive = NULL;
	self->msgoffsets = 0;
	self->setting.timeout_msecs = 0;
	self->setting.keepalive_msecs = 0;
	self->setting.max_inbuffer_len = 0;

	// NOTICE: 最后一步终止描述符
	// 系统会回收描述符, libevlite会重用以描述符为key的会话
	if ( self->fd > 0 )
	{
		close( self->fd );
		self->fd = -1;
	}

	return 0;
}

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

static struct session * _get_entry( struct session_manager * self, sid_t id );

struct session * _get_entry( struct session_manager * self, sid_t id )
{
	uint32_t i = 0;
	uint32_t seq = SID_SEQ( id );
	struct arraylist * bucket = self->entries + ( seq&(self->size-1) );

	assert( self->index == SID_INDEX(id) );

	for ( i = 0; i < arraylist_count(bucket); ++i )
	{
		struct session * session = arraylist_get( bucket, i );
		if ( session->id == id )
		{
			// 会话id合法但与entry中记录的key和seq不相符
			// 这种错误对于libevlite来说是相当致命的

			return session;
		}
	}

	return NULL;
}

struct session_manager * session_manager_create( uint8_t index, uint32_t size )
{
	uint32_t i = 0;
	struct session_manager * self = NULL;

	self = calloc( 1, sizeof(struct session_manager) );
	if ( self == NULL )
	{
		return NULL;
	}

	size = nextpow2( size );

	self->seq = 0;
	self->count = 0;
	self->size = size;
	self->index = index;

	self->entries = calloc( size, sizeof(struct arraylist) );
	if ( self->entries == NULL )
	{
		free( self );
		return NULL;
	}

	for ( i = 0; i < size; ++i )
	{
		int32_t rc = arraylist_init( self->entries+i, 8 );
		if ( rc != 0 )
		{
			uint32_t j = 0;
			for ( j = 0; j < i; ++j )
			{
				arraylist_final( self->entries+j );
			}

			free( self->entries );
			free( self );
			return NULL;
		}
	}

	return self;
}

struct session * session_manager_alloc( struct session_manager * self )
{
	sid_t id = 0;
	uint32_t i = 0;

	struct session * session = NULL;
	struct arraylist * bucket = self->entries + ( (self->seq)&(self->size-1) );
	
	// 生成sid
	id = self->index+1;
	id <<= 32;
	id |= self->seq++;
	id &= SID_MASK;

	// 查找合适的会话并交验sid是否合法
	for ( i = 0; i < arraylist_count(bucket); ++i )
	{
		struct session * s = arraylist_get( bucket, i );
		if ( s->id == id )
		{
			// 会话ID冲突, 记录日志
			syslog(LOG_WARNING, "session_manager_alloc(Index=%u): the SID (Seq=%u, Sid=%ld) conflict !", self->index, self->seq-1, id );
			return NULL;
		}
		else if ( s->id == 0 )
		{
			// 找到可用的会话句柄
			session = s;
		}
	}

	if ( session == NULL )
	{
		// 没有找到可用的会话句柄
		session = _new_session( OUTMSGLIST_DEFAULT_SIZE );
		if ( session == NULL )
		{
			return NULL;
		}

		// 添加到列表中
		arraylist_append( bucket, session );
	}

	++self->count;

	// 会话初始化操作
	session->id = id;
	session->manager = self;

	return session;
}

struct session * session_manager_get( struct session_manager * self, sid_t id )
{
	return _get_entry( self, id );
}

int32_t session_manager_remove( struct session_manager * self, struct session * session )
{
	if ( _get_entry( self, session->id ) == NULL )
	{
		return -1;
	}

	// 会话数据清空
	session->id = 0;
	session->manager = NULL;

	--self->count;

	return 0;
}

void session_manager_destroy( struct session_manager * self )
{
	int32_t i = 0;

	if ( self->count > 0 )
	{
		syslog(LOG_WARNING, "session_manager_destroy(Index=%u): the number of residual active session is %d .", self->index, self->count );
	}

	for ( i = 0; i < self->size; ++i )
	{
		struct arraylist * bucket = self->entries+i;
		
		while ( arraylist_count(bucket) > 0 )
		{
			struct session * session = arraylist_take( bucket, -1 );

			if ( session )
			{
				if ( session->id != 0 ) 
				{
					// 会话处于激活状态
					session_end( session, session->id );
				}
				
				// 销毁	
				_del_session( session );
			}
		}

		arraylist_final( bucket );
	}

	if ( self->entries )
	{
		free( self->entries );
		self->entries = NULL;
	}

	free( self );
	return;
}

