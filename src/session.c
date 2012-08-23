
#include <assert.h>

#include "server.h"
#include "channel.h"

#include "session.h"


// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

int32_t session_start( struct session * self, int8_t type, int32_t fd, evsets_t sets )
{
	self->fd		= fd;
	self->type		= type;
	self->evsets	= sets;


}

int32_t session_send( struct session * self, char * buf, uint32_t nbytes )
{
	evsets_t sets = self->evsets;
	
	// 判断session是否繁忙
	if ( self->writing == 1 
		|| arraylist_count(&self->outmsglist) > 0 )
	{
		// 创建message, 添加到发送队列中

		return 0;
	}

	// 直接发送
	return channel_send( self, sets, buf, nbytes ); 
}


void session_add_event( struct session * self, int16_t ev )
{
	int8_t status = self->status;
	evsets_t sets = self->evsets;
	
	if ( (ev&EV_READ) && !(status&SESSION_READING) ) 
	{
		int32_t nsecs = self->setting.timeout_secs * 1000;
		
		event_set( self->evread, self->fd, ev );
		event_set_callback( self->evread, channel_on_read, self );
		evsets_add( sets, self->evread, 0 );

		self->status |= SESSION_READING;
	}
	
	if ( (ev&EV_WRITE) && !(status&SESSION_WRITING) )
	{
		event_set( self->evwrite, self->fd, ev );
		event_set_callback( self->evwrite, channel_on_write, self );
		evsets_add( sets, self->evwrite, 0 );

		self->status |= SESSION_WRITING;
	}

	return;
}

int32_t session_end( struct session * self )
{
	// 清空发送队列
	int32_t count = arraylist_count( &self->outmsglist );
	if ( count > 0 )
	{
		// 会话终止时发送队列不为空
		syslog(LOG_WARNING, "session_end(%ld)'s Out-Message-List (%d) is not empty", self->id, count );
	}
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

	// 删除事件
	if ( self->evread )
	{
		evsets_del( self->evsets, self->evread );
		self->status &= ~SESSION_READING;
	}
	if ( self->evwrite )
	{
		evsets_del( self->evsets, self->evwrite );
		self->status &= ~SESSION_WRITING;
	}
	if ( self->evkeepalive )
	{
		evsets_del( self->evsets, self->evkeepalive );
	}

	self->type = 0;
	self->evsets = NULL;

	// NOTICE: 最后一步关闭描述符
	// 系统会回收描述符, libevlite会重用以描述符为key的会话
	if ( self->fd )
	{
		close( self->fd );
		self->fd = 0;
	}
}


// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

static struct session_entry * _get_entry( struct session_manager * self, sid_t id );



struct session_entry * _get_entry( struct session_manager * self, sid_t id )
{
	uint32_t i = 0;
	int32_t key = SID_KEY( id );
	
	struct arraylist * bucket = self->entries + ( key&(self->size-1) );
	for ( i = 0; i < arraylist_count(bucket); ++i )
	{
		struct session_entry * entry = arraylist_get( bucket, i );
		if ( entry->data.id == id )
		{
			return entry;
		}
	}

	return NULL;
}

struct session * session_manager_alloc( struct session_manager * self, int32_t fd )
{
	sid_t id = 0;
	uint32_t i = 0;

	struct session * session = NULL;
	struct session_entry * entry = NULL; 
	struct arraylist * bucket = self->entries + ( fd&(self->size-1) );

	for ( i = 0; i < arraylist_count(bucket); ++i )
	{
		entry = arraylist_get( bucket, i );
		if ( entry->key == fd )
		{
			// 会话冲突了, 很严重的错误
			assert( entry->data.sid == 0 );
			break;
		}
	}

	if ( entry == NULL )
	{
		entry = malloc( sizeof(struct session_entry) );
		if ( entry == NULL )
		{
			return NULL;
		}
		
		entry->key = fd;
		entry->seq = 0;
		arraylist_append( bucket, entry );
	}
	
	// 生成sid
	id = self->index+1;
	id <<= 32;
	id |= entry->key;
	id <<= 16;
	id |= entry->seq;
	id &= 0x00ffffffffffffff;
	
	session = &(entry->data);

	// 会话初始化操作
	session->id = id;
	session->manager = self;

	return session;
}

struct session * session_manager_get( struct session_manager * self, sid_t id )
{
	struct session_entry * entry = _get_entry( self, id );
	if ( entry == NULL )
	{
		return NULL;
	}

	assert( SID_KEY(id) == entry->key && SID_SEQ(id) == entry->seq );

	return &( entry->data );
}

int32_t session_manager_remove( struct session_manager * self, struct session * session )
{
	struct session_entry * entry = _get_entry( self, session->id );	

	if ( entry == NULL )
	{
		return -1;
	}

	assert( SID_KEY(id) == entry->key && SID_SEQ(id) == entry->seq );

	// 序号自增
	++entry->seq;
	
	// 会话数据清空
	session->id = 0;
	session->manager = NULL;

	return 0;
}


