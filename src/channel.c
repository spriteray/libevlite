
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "utils.h"
#include "iolayer.h"
#include "network-internal.h"

#include "channel.h"

// 发送接收数据
static int32_t _receive( struct session * session );
static int32_t _transmit( struct session * session );
static inline int32_t _write_vec( int32_t fd, struct iovec * array, int32_t count );

// 逻辑操作  
static int32_t _process( struct session * session );
static int32_t _timeout( struct session * session );

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

int32_t _receive( struct session * session )
{
	// 尽量读数据
	return buffer_read( &session->inbuffer, session->fd, -1 );
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

	uint32_t i = 0;
	int32_t writen = 0, offsets = session->msgoffsets;

	int32_t iov_size = 0;
	struct iovec iov_array[iov_max];
	memset( iov_array, 0, sizeof(iov_array) );

	for ( i = 0; i < QUEUE_COUNT(sendqueue)(&session->sendqueue) && iov_size < iov_max; ++i )
	{
		struct message * message = NULL;

		QUEUE_GET(sendqueue)( &session->sendqueue, i, &message );
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

	writen = _write_vec( session->fd, iov_array, iov_size );

	if ( writen > 0 )
	{
		offsets = session->msgoffsets + writen;

		for ( ; QUEUE_COUNT(sendqueue)(&session->sendqueue) > 0; )
		{
			struct message * message = NULL;
			
			QUEUE_TOP(sendqueue)( &session->sendqueue, &message );
			if ( offsets < message_get_length(message) )
			{
				break;
			}

			QUEUE_POP(sendqueue)( &session->sendqueue, &message );
			offsets -= message_get_length(message);
			
			message_add_success( message );
			if ( message_is_complete(message) )
			{
				message_destroy( message );
			}
		}

		session->msgoffsets = offsets;
	}

	if ( writen > 0 && QUEUE_COUNT(sendqueue)(&session->sendqueue) > 0 )
	{
		int32_t againlen = _transmit( session );
		if ( againlen > 0 )
		{
			writen += againlen;
		}
	}

	return writen;
}

int32_t _write_vec( int32_t fd, struct iovec * array, int32_t count )
{
	int32_t writen = -1;

#if defined (TCP_CORK)
	int32_t corked = 1;
	setsockopt( fd, SOL_TCP, TCP_CORK, (const char *)&corked, sizeof(corked) );
#endif

	writen = writev( fd, array, count );

#if defined (TCP_CORK)
	corked = 0;
	setsockopt( fd, SOL_TCP, TCP_CORK, (const char *)&corked, sizeof(corked) );
#endif

	return writen;
}

int32_t channel_send( struct session * session, char * buf, uint32_t nbytes )
{
	int32_t writen = 0;

	writen = write( session->fd, buf, nbytes );
	if ( writen < 0 )
	{
		if ( errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK )
		{
			writen = 0;
		}
	}

	return writen;
}

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

int32_t _process( struct session * session )
{
	int32_t nprocess = 0;
	ioservice_t * service = &session->service;

	if ( buffer_length( &session->inbuffer ) > 0 )
	{
		char * buffer = buffer_data( &session->inbuffer );
		uint32_t nbytes = buffer_length( &session->inbuffer );
		
		// 回调逻辑层
		nprocess = service->process(
						session->context, buffer, nbytes );
		if ( nprocess > 0 )
		{
			buffer_erase( &session->inbuffer, nprocess );
		}
	}

	return nprocess;
}

int32_t _timeout( struct session * session )
{
	/*
	 * 超时, 会尝试安全的终止会话
	 * 根据逻辑层的返回值确定是否终止会话
	 */
	int32_t rc = 0;
	ioservice_t * service = &session->service;

	rc = service->timeout( session->context );

	if ( rc != 0
		|| ( session->status&SESSION_EXITING ) )
	{
		// 等待终止的会话
		// 逻辑层需要终止的会话
		// NOTICE: 此处会尝试终止会话
		return session_shutdown( session );
	}

	if ( session->type == eSessionType_Persist )
	{
		// 尝试重连的永久会话
		session_start_reconnect( session );
	}
	else
	{
		// 临时会话, 添加读事件
		session_add_event( session, EV_READ );
		// TODO: 是否需要打开keepalive
		session_start_keepalive( session );
	}

	return 0;
}

int32_t channel_error( struct session * session, int32_t result )
{
	/* 出错
	 * 出错时, libevlite会直接终止会话, 丢弃发送队列中的数据 
	 *
	 * 根据会话的类型
	 *		1. 临时会话, 直接终止会话
	 *		2. 永久会话, 终止socket, 尝试重新连接
	 */
	int32_t rc = 0;
	ioservice_t * service = &session->service;

	rc = service->error( session->context, result );

	if ( session->type == eSessionType_Once
			|| ( session->status&SESSION_EXITING )
			|| ( session->type == eSessionType_Persist && rc != 0 ) )
	{
		// 临时会话
		// 等待终止的会话
		// 逻辑层需要终止的永久会话
		// 直接终止会话, 导致发送队列中的数据丢失
		return channel_shutdown( session );
	}

	// 尝试重连的永久会话
	session_start_reconnect( session );

	return 0;
}

int32_t channel_shutdown( struct session * session )
{
	sid_t id = session->id;
	ioservice_t * service = &session->service;
	struct session_manager * manager = session->manager;

	// 会话终止
	service->shutdown( session->context );
	session_manager_remove( manager, session );
	session_end( session, id );

	return 0;
}

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

void channel_on_read( int32_t fd, int16_t ev, void * arg )
{
	struct session * session = (struct session *)arg;

	session->status &= ~SESSION_READING;

	if ( ev & EV_READ )
	{
		/* >0	- ok
		 *  0	- peer shutdown
		 * -1	- read() failure
		 * -2	- expand() failure
		 */
		int32_t nread = _receive( session );
		int32_t nprocess = _process( session );

		if ( nprocess < 0 )
		{
			// 处理出错, 尝试终止会话
			session_shutdown( session );
			return;
		}

		if ( nread > 0 || (nread == -1 && errno == EAGAIN) )
		{
			// 会话正常
			session_add_event( session, EV_READ );
			return;
		}

		if ( nread == -2 )
		{
			// expand() failure
			channel_error( session, eIOError_OutMemory );
		}
		else if ( nread == -1 )
		{
			// read() failure
			channel_error( session, eIOError_ReadFailure );
		}
		else if ( nread == 0 )
		{
			// peer shutdown
			channel_error( session, eIOError_PeerShutdown );
		}
	}
	else
	{
		_timeout( session );
	}

	return;
}

void channel_on_write( int32_t fd, int16_t ev, void * arg )
{
	struct session * session = (struct session *)arg;

	session->status &= ~SESSION_WRITING;

	if ( ev & EV_WRITE )
	{
		if ( QUEUE_COUNT(sendqueue)(&session->sendqueue) > 0 )
		{
			// 发送数据
			int32_t writen = _transmit( session );
			if ( writen < 0 && errno != EAGAIN )
			{
				channel_error( session, eIOError_WriteFailure );
			}
			else
			{
				// 正常发送 或者 socket缓冲区已满

				if ( QUEUE_COUNT(sendqueue)(&session->sendqueue) > 0 )
				{
					// NOTICE: 为什么不判断会话是否正在终止呢?
					// 为了尽量把数据发送完全, 所以只要不出错的情况下, 会一直发送
					// 直到发送队列为空
					session_add_event( session, EV_WRITE );
				}
				else
				{
					if ( session->status&SESSION_EXITING )
					{
						// 等待关闭的会话, 直接终止会话
						// 后续的行为由SO_LINGER决定
						channel_shutdown( session );
					}
				}
			}
		}
		else
		{
			// TODO: 队列为空的情况
		}
	}
	else
	{
		// 等待关闭的会话写事件超时的情况下
		// 不管发送队列如何, 直接终止会话
		
		assert( session->status&SESSION_EXITING );
		
		channel_shutdown( session );
	}

	return;
}

void channel_on_accept( int32_t fd, int16_t ev, void * arg )
{
	struct acceptor * acceptor = (struct acceptor *)arg;
	struct iolayer * layer = (struct iolayer *)(acceptor->parent);

	if ( ev & EV_READ )
	{
		int32_t cfd = -1;
		struct task_assign task;

		cfd = tcp_accept( fd, task.host, &(task.port) );
		if ( cfd > 0 )
		{
			uint8_t index = 0;

#if !defined(__FreeBSD__)
			// FreeBSD会继承listenfd的NON Block属性
			set_non_block( cfd );
#endif

			task.fd = cfd;
			task.cb = acceptor->cb;
			task.context = acceptor->context;

			// 分发策略
			index = cfd % (layer->nthreads);
			iolayer_assign_session( layer, index, &(task) );
		}
	}

	return;
}

void channel_on_connect( int32_t fd, int16_t ev, void * arg )
{
	int32_t rc = 0, result = 0;
	struct connector * connector = (struct connector *)arg;

	sid_t id = 0;
	struct session * session = NULL;
	struct iolayer * layer = (struct iolayer *)( connector->parent );

	if ( ev & EV_WRITE )
	{
#if defined (__linux__)
		// linux需要进一步检查连接是否成功
		if ( is_connected( fd ) != 0 )
		{
			result = eIOError_ConnectStatus;
		}
#endif
	}
	else
	{
		result = eIOError_Timeout;
	}
	
	if ( result == 0 )
	{
		session = iolayer_alloc_session( layer, connector->fd );
		if ( session == NULL )
		{
			result = eIOError_OutMemory;
		}
		else
		{
			id = session->id;
		}
	}

	// 把连接结果回调给逻辑层
	rc = connector->cb( connector->context, result, connector->host, connector->port, id );

	if ( rc == 0 )
	{
		if ( result != 0 )
		{
			// 逻辑层需要继续连接
			iolayer_reconnect( layer, connector );
		}
		else
		{
			// 连接成功
			evsets_t sets = event_get_sets( connector->event );

			set_non_block( connector->fd );
			session_set_endpoint( session, connector->host, connector->port );
			session_start( session, eSessionType_Persist, connector->fd, sets );
			connector->fd = -1;
			iolayer_free_connector( layer, connector );
		}
	}
	else
	{
		if ( session )
		{
			session_manager_remove( session->manager, session );
			session_end( session, id );
		}
		iolayer_free_connector( layer, connector );
	}

	return;
}

void channel_on_reconnect( int32_t fd, int16_t ev, void * arg )
{
	struct session * session = (struct session *)arg;

	session->status &= ~SESSION_WRITING;

	if ( ev & EV_WRITE )
	{
#if defined (__linux__)
		if ( is_connected(fd) != 0 )
		{
			channel_error( session, eIOError_ConnectStatus );
			return;
		}
#endif
		// 总算是连接上了

		set_non_block( fd );
		session->service.start( session->context );
		
		// 注册读写事件
		// 把积累下来的数据全部发送出去
		session_add_event( session, EV_READ );
		session_add_event( session, EV_WRITE );
		session_start_keepalive( session );
	}
	else
	{
		_timeout( session );
	}
	
	return;
}

void channel_on_keepalive( int32_t fd, int16_t ev, void * arg )
{
	struct session * session = (struct session *)arg;
	ioservice_t * service = &session->service;

	session->status &= ~SESSION_KEEPALIVING;
	
	if ( service->keepalive( session->context ) == 0 )
	{
		// 逻辑层需要继续发送保活包
		session_start_keepalive( session );
	}	

	return;
}

void channel_on_tryreconnect( int32_t fd, int16_t ev, void * arg )
{
	struct session * session = (struct session *)arg;

	session->status &= ~SESSION_WRITING;
	
	// 连接远程服务器
	session->fd = tcp_connect( session->host, session->port, 1 ); 
	if ( session->fd < 0 )
	{
		channel_error( session, eIOError_ConnectFailure );
		return;
	}
	
	// 注册写事件, 以重新激活会话
	event_set( session->evwrite, session->fd, EV_WRITE );
	event_set_callback( session->evwrite, channel_on_reconnect, session );
	evsets_add( session->evsets, session->evwrite, 0 );

	session->status |= SESSION_WRITING;

	return;
}

