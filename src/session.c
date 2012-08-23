
#include "server.h"
#include "channel.h"

#include "session.h"

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
	int32_t count = arraylist_count( &self->outmsglist );

	// 清空发送队列
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

	//
}

