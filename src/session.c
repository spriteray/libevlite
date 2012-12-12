
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
static inline int32_t _send( struct session * self, char * buf, uint32_t nbytes );

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

//
int32_t _send( struct session * self, char * buf, uint32_t nbytes )
{
	int32_t rc = 0;

	// 等待关闭的连接
	if ( self->status&SESSION_EXITING )
	{
		return -1;
	}

	// 判断session是否繁忙
	if ( !(self->status&SESSION_WRITING) 
			&& arraylist_count(&self->outmsglist) == 0 )
	{
		// 直接发送
		rc = channel_send( self, buf, nbytes ); 
		if ( rc == nbytes )
		{
			// 全部发送出去
			return rc;
		}

		// 为什么发送错误没有直接终止会话呢？
		// 该接口有可能在ioservice_t中调用, 直接终止会话后, 会引发后续对会话的操作崩溃
	}

	// 创建message, 添加到发送队列中
	struct message * message = message_create();
	if ( message == NULL )
	{
		return -2;
	}

	message_add_buffer( message, buf+rc, nbytes-rc );
	message_add_receiver( message, self->id );
	arraylist_append( &self->outmsglist, message );
	session_add_event( self, EV_WRITE );

	return rc;
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
	session_add_event( self, EV_READ );
	session_start_keepalive( self );

	return 0;
}

void session_set_endpoint( struct session * self, char * host, uint16_t port )
{
	self->port = port;
	strncpy( self->host, host, INET_ADDRSTRLEN );
}

int32_t session_send( struct session * self, char * buf, uint32_t nbytes )
{
	int32_t rc = -1;
	ioservice_t * service = &self->service;
	
	// 数据改造(加密 or 压缩)
	uint32_t _nbytes = nbytes;
	char * _buf = service->transform( self->context, (const char *)buf, &_nbytes );

	if ( _buf != NULL )
	{
		// 发送数据
		// TODO: _send()可以根据具体情况决定是否copy内存
		rc = _send( self, _buf, _nbytes );

		if ( _buf != buf )
		{
			// 消息已经成功改造
			free( _buf );
		}
	}

	return rc;
}

//
int32_t session_append( struct session * self, struct message * message )
{
	int32_t rc = -1;
	ioservice_t * service = &self->service;

	char * buf = message_get_buffer( message );
	uint32_t nbytes = message_get_length( message );
	
	// 数据改造
	char * buffer = service->transform( self->context, (const char *)buf, &nbytes );

	if ( buffer == buf )
	{
		// 消息未进行改造

		// 添加到会话的发送列表中
		rc = arraylist_append( &self->outmsglist, message );
		if ( rc == 0 )
		{
			// 注册写事件, 处理发送队列
			session_add_event( self, EV_WRITE );
		}
	}
	else if ( buffer != NULL )
	{
		// 消息改造成功

		rc = _send( self, buffer, nbytes );
		if ( rc >= 0 )
		{
			// 改造后的消息已经单独发送
			rc = 1;
		}

		free( buffer );
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

	event_set( self->evwrite, self->fd, EV_WRITE );
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
		session_add_event( self, EV_WRITE );

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
		syslog(LOG_WARNING, "%s(SID=%ld)'s Out-Message-List (%d) is not empty .", __FUNCTION__, id, count );

		for ( ; count >= 0; --count )
		{
			struct message * msg = arraylist_take( &self->outmsglist, -1 );
			if ( msg )
			{
				// 消息发送失败一次
				message_add_failure( msg, id );

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

	// NOTICE: 最后一步终止描述符
	// 系统会回收描述符, libevlite会重用以描述符为key的会话
	if ( self->fd > 0 )
	{
		close( self->fd );
		self->fd = -1;
	}

	_del_session( self );

	return 0;
}

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

// 填装因子
#define ENLARGE_FACTOR	0.5f

enum
{
	eBucketStatus_Free		= 0,
	eBucketStatus_Deleted	= 1,
	eBucketStatus_Occupied	= 2,
};

struct hashbucket
{
	int8_t status;
	struct session * session;
};

struct hashtable
{
	uint32_t size;
	uint32_t count;
	uint32_t enlarge_size;				// = size*ENLARGE_FACTOR
	
	struct hashbucket * buckets;
};

static inline int32_t _init_table( struct hashtable * table, uint32_t size );
static inline int32_t _enlarge_table( struct hashtable * table );
static inline int32_t _find_position( struct hashtable * table, sid_t id );
static inline int32_t _append_session( struct hashtable * table, struct session * s );
static inline int32_t _remove_session( struct hashtable * table, struct session * s );
static inline struct session * _find_session( struct hashtable * table, sid_t id );

int32_t _init_table( struct hashtable * table, uint32_t size )
{
	table->count = 0;
	table->size = size;
	table->enlarge_size = (uint32_t)( (float)size * ENLARGE_FACTOR );
	
	table->buckets = calloc( size, sizeof(struct hashbucket) );
	if ( table->buckets == NULL )
	{
		return -1;
	}

	return 0;
}

int32_t _enlarge_table( struct hashtable * table )
{
	uint32_t i = 0;
	struct hashtable newtable;	

	if ( _init_table(&newtable, table->size<<1) != 0 )
	{
		return -1;
	}

	for ( i = 0; i < table->size; ++i )
	{
		struct hashbucket * bucket = table->buckets + i;

		if ( bucket->session != NULL 
				&& bucket->status == eBucketStatus_Occupied )
		{
			_append_session( &newtable, bucket->session );
		}
	}

	assert( newtable.count == table->count );

	free ( table->buckets );
	table->buckets = newtable.buckets;
	table->size = newtable.size;
	table->enlarge_size = newtable.enlarge_size;

	return 0;
}

int32_t _find_position( struct hashtable * table, sid_t id )
{
	int32_t pos = -1; 
	int32_t probes = 0;
	int32_t bucknum = SID_SEQ(id) & (table->size-1);

	while ( 1 )
	{
		struct hashbucket * bucket = table->buckets + bucknum;

		if ( bucket->status == eBucketStatus_Free
				|| bucket->status == eBucketStatus_Deleted )
		{
			if ( pos == -1 )
			{
				pos = bucknum;
			}

			if ( bucket->status == eBucketStatus_Free )
			{
				return pos;
			}
		}	
		else if ( bucket->session->id == id )
		{
			return bucknum;
		}

		++probes;
		bucknum = (bucknum + probes) & (table->size-1);
	}

	return -1;
}

int32_t _append_session( struct hashtable * table, struct session * s )
{
	if ( table->count >= table->enlarge_size )
	{
		// 扩展
		if ( _enlarge_table( table ) != 0 )
		{
			return -1;
		}
	}

	int32_t pos = _find_position( table, s->id );
	if ( pos == -1 )
	{
		return -1;
	}

	struct hashbucket * bucket = table->buckets + pos;
	if ( bucket->session != NULL && bucket->session->id == s->id )
	{
		syslog(LOG_WARNING, "%s(Index=%d): the SID (Seq=%u, Sid=%ld) conflict !", __FUNCTION__, SID_INDEX(s->id), SID_SEQ(s->id), s->id );
		return -2;
	}
	
	++table->count;
	bucket->session = s;
	bucket->status = eBucketStatus_Occupied;

	return 0;
}

int32_t _remove_session( struct hashtable * table, struct session * s )
{
	int32_t pos = _find_position( table, s->id );
	if ( pos == -1 )
	{
		return -1;
	}

	if ( table->buckets[pos].status == eBucketStatus_Free
	  || table->buckets[pos].status == eBucketStatus_Deleted )
	{
		return -2;
	}

	struct hashbucket * bucket = table->buckets + pos;
	assert( bucket->session == s );
	
	--table->count;
	bucket->session = NULL;
	bucket->status = eBucketStatus_Deleted;

	return 0;	
}

struct session * _find_session( struct hashtable * table, sid_t id )
{
	int32_t pos = _find_position( table, id );
	if ( pos == -1 )
	{
		return NULL;
	}

	if ( table->buckets[pos].status == eBucketStatus_Free
	  || table->buckets[pos].status == eBucketStatus_Deleted )
	{
		return NULL;
	}

	assert( table->buckets[pos].session != NULL );
	assert( table->buckets[pos].session->id == id );

	return table->buckets[pos].session;
}

struct session_manager * session_manager_create( uint8_t index, uint32_t size )
{
	struct session_manager * self = NULL;

	self = calloc( 1, sizeof(struct session_manager)+sizeof(struct hashtable) );
	if ( self == NULL )
	{
		return NULL;
	}

	size = nextpow2( size );

	self->seq = 0;
	self->index = index;
	self->table = (struct hashtable *)( self + 1 );

	if ( _init_table(self->table, size) != 0 )
	{
		free( self );
		self = NULL;
	}		

	return self;
}

struct session * session_manager_alloc( struct session_manager * self )
{
	sid_t id = 0;
	struct session * session = NULL;

	// 生成sid
	id = self->index+1;
	id <<= 32;
	id |= self->seq++;
	id &= SID_MASK;

	session = _new_session( OUTMSGLIST_DEFAULT_SIZE );
	if ( session != NULL )
	{
		session->id = id;
		session->manager = self;

		// 添加会话
		if ( _append_session(self->table, session) != 0 )
		{
			_del_session( session );
			session = NULL;
		}
	}	

	return session;
}

struct session * session_manager_get( struct session_manager * self, sid_t id )
{
	return _find_session( self->table, id );
}

int32_t session_manager_remove( struct session_manager * self, struct session * session )
{
	if ( _remove_session(self->table, session) != 0 )
	{
		return -1;
	}

	// 会话数据清空
	session->id = 0;
	session->manager = NULL;

	return 0;
}

void session_manager_destroy( struct session_manager * self )
{
	int32_t i = 0;

	if ( self->table->count > 0 )
	{
		syslog( LOG_WARNING, "%s(Index=%u): the number of residual active session is %d .", __FUNCTION__, self->index, self->table->count );
	}

	for ( i = 0; i < self->table->size; ++i )
	{
		struct hashbucket * bucket = self->table->buckets + i;

		if ( bucket->session != NULL 
				&& bucket->status == eBucketStatus_Occupied )
		{
			struct session * session = bucket->session; 

			if ( session )
			{
				session_end( session, session->id );
			}
		}
	}

	if ( self->table->buckets != NULL )
	{
		free( self->table->buckets );
		self->table->buckets = NULL;
	}

	free( self );
	return;
}

