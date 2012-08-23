
#include "channel.h"

// 发送接收数据
static int32_t _receive( struct session * session );
static int32_t _transmit( struct session * session );

// 
static int32_t _process( struct session * session );

int32_t _receive( struct session * session )
{
	int32_t nleft = session->setting.max_inbuffer_len;

	nleft -= buffer_length( &session->inbuffer );
	if ( nleft <= 0 )
	{
		return -3;
	}

	return buffer_read( &session->inbuffer, session->fd, nleft );
}

int32_t _transmit( struct session * session )
{

#if defined (IOV_MAX)
	const int32_t iov_max = IOV_MAX;
#elif defined (UIO_MAXIOV)
	const int32_t iov_max = UIO_MAXIOV;
#elif defined (MAX_IOVEC)
	const int32_t iov_max = MAX_IOVEC;
#else
	const int32_t iov_max = 8;
#endif

#if defined (TCP_CORK)
	int32_t corked = 0;
#endif

	uint32_t i = 0;
	int32_t writen = 0;

	int32_t iov_size = 0;
	struct iovec iov_array[iov_max];
	memset( iov_array, 0, sizeof(iov_array) );

	int32_t offsets = session->msgoffsets;

	for ( i = 0; i < arraylist_count( &session->outmsglist ); ++i )
	{
		struct message * message = arraylist_get( &session->outmsglist, i );

		if ( offsets >= message_get_length(message) )
		{
			offsets -= message_get_length(message);
		}
		else
		{
			iov_array[iov_size].iov_len = message_get_length(message) - offsets;
			iov_array[iov_size].iov_base = message_get_buffer(message) + offsets;

			++iov_size;
			offsets = 0;
		}
	}

#if defined (TCP_CORK)
	corked = 1;
	setsockopt( session->fd, SOL_TCP, TCP_CORK, (const char *)&corked, sizeof(corked) );
#endif

	writen = writev( session->fd, iov_array, iov_size );

#if defined (TCP_CORK)
	corked = 0;
	setsockopt( session->fd, SOL_TCP, TCP_CORK, (const char *)&corked, sizeof(corked) );
#endif

	if ( writen > 0 )
	{
		offsets = session->msgoffsets + writen;

		for ( ; arraylist_count( &session->outmsglist ); )
		{
			struct message * message = arraylist_get( &session->outmsglist, 0 );

			if ( offsets < message_get_length(message) )
			{
				break;
			}

			arraylist_take( &session->outmsglist, 0 );

			offsets -= message_get_length(message);
			++message->nsuccess;
			if ( message_is_complete(message) == 0 )
			{
				message_destroy( message );
			}
		}

		session->msgoffsets = offsets;
	}

	if ( writen > 0 && arraylist_count( &session->outmsglist ) > 0 )
	{
		int32_t againlen = _transmit( session );
		if ( againlen > 0 )
		{
			writen += againlen;
		}
	}

	return writen;
}

int32_t _process( struct session * session )
{
	int32_t nprocess = 0;
	ioservice_t * service = &session->service;

	if ( buffer_length( &session->inbuffer ) > 0 )
	{
		char * buffer = buffer_data( &session->inbuffer );
		uint32_t nbytes = buffer_length( &session->inbuffer );

		nprocess = service->process(
						session->context, buffer, nbytes);
		if ( nprocess > 0 )
		{
			buffer_erase( &session->inbuffer, nprocess );
		}
	}

	return nprocess;
}

void channel_on_read( int32_t fd, int16_t ev, void * arg )
{
	struct session * session = (struct session *)arg;

	if ( ev & EV_READ )
	{
		uint8_t done = 0;

		while ( done )
		{
			int32_t nread = 0;
			int32_t nprocess = 0;

			nread = _receive( session );
			if ( nread > 0 )
			{
				continue;
			}

			nprocess = _process( session );
			if ( nprocess > 0 && nread == -3 )
			{
				continue;
			}
		}
	}
}

void channel_on_write( int32_t fd, int16_t ev, void * arg )
{

}

void channel_on_accept( int32_t fd, int16_t ev, void * arg )
{
	struct acceptor * acceptor = (struct acceptor *)arg;
	struct server * server = (struct server *)(acceptor->parent);

	if ( ev & EV_READ )
	{
		int32_t cfd = -1;
		struct stask_assign task;

		cfd = tcp_accept( fd, task.host, &(task.port) );
		if ( cfd > 0 )
		{
			uint8_t index = 0;

#if !defined(__FreeBSD__)
			// FreeBSD会继承listenfd的NON Block属性
			set_fd_nonblock( cfd );
#endif

			task.fd = cfd;
			task.cb = acceptor->cb;
			task.context = acceptor->context;

			// 分发策略
			index = cfd % (server->nthreads);

			// 把这个会话分发给指定的网络线程
			server_assign_session( server, index, &(task) );
		}
	}

	return;
}

int32_t channel_start( struct session * session )
{
	
}

int32_t channel_shutdown( struct session * session )
{
	if ( !(session->status&SESSION_EXITING)
		&& arraylist_count(&session->outmsglist) > 0 )
	{
		// 会话状态正常, 并且发送队列不为空
		// 尝试继续把未发送的数据发送出去, 在关闭会话
		session->status |= SESSION_EXITING;
		session_add_event( session, EV_WRITE|EV_ET );
		return 1;
	}

	evsets_t sets = session->evsets;
	void * context = session->context;
	ioservice_t * service = &session->service;
	struct session_manager * manager = session->manager;

	// 从会话管理器中移除
	session_manager_remove( manager, session );

	// 会话终止
	session_end( session );

	// 回调逻辑层
	service->shutdown( context );
}


