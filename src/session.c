
#include "session.h"


int32_t session_send( struct session * self, evsets_t sets, char * buf, uint32_t nbytes )
{
	// 判断session是否繁忙
	if ( self->writing == 1 
		|| arraylist_count(&(self->outmsglist)) > 0 )
	{
		// 创建message, 添加到发送队列中

		return 0;
	}

	return channel_send( self, sets, buf, nbytes ); 
}


