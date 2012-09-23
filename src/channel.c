
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "utils.h"
#include "client.h"
#include "server.h"
#include "network-internal.h"

#include "channel.h"

// 发送接收数据
static int32_t _receive( struct session * session );
static int32_t _transmit( struct session * session );

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

#if defined (TCP_CORK)
	int32_t corked = 0;
#endif

	uint32_t i = 0;
	int32_t writen = 0;
	int32_t offsets = session->msgoffsets;

	int32_t iov_size = 0;
	struct iovec iov_array[iov_max];
	memset( iov_array, 0, sizeof(iov_array) );

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

int32_t channel_send( struct session * session, char * buf, uint32_t nbytes )
{
	int32_t writen = -1;

	writen = write( session->fd, buf, nbytes );
	if ( writen < 0 )
	{
		if ( errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK )
		{
			writen = 0;
		}
	}

	if ( writen < nbytes )
	{
		// 没有完全发送或者发送错误
		// 为什么发送错误没有直接终止会话呢？
		// 该接口有可能在ioservice_t中调用, 直接终止会话后, 会引发后续对会话的操作崩溃
		struct message * message = message_create();
		if ( message )
		{
			message_add_receiver( message, session->id );
			message_set_buffer( message,  buf, nbytes );

			if ( writen > 0 )
			{
				buffer_erase( &message->buffer, writen );
			}

			arraylist_append( &session->outmsglist, message );
			session_add_event( session, EV_WRITE|EV_ET );

			return writen;
		}
	}

	// 全部发送完毕
	free ( buf );
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
		session_add_event( session, EV_READ|EV_ET );
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
	ioservice_t * service = &session->service;
	struct session_manager * manager = session->manager;

	// 回调逻辑层
	service->shutdown( session->context );
	
	// 从会话管理器中移除会话
	if ( manager != NULL
			&& session->type == eSessionType_Once )
	{
		// 临时会话, 需要从manager中移除会话
		session_manager_remove( manager, session );
	}

	// 会话终止
	session_end( session );

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
		while ( 1 )
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
				// 处理出错, 安全的终止会话
				// NOTICE: 此处会尝试终止会话
				session_shutdown( session );
				break;
			}

			if ( nread == -2 )
			{
				// InBuffer 扩展失败 
				channel_error( session, eIOError_OutMemory );
				break;
			}
			else if ( nread == -1 )
			{
				if ( nread == -1 && errno == EAGAIN )
				{
					// 正常,socket中无数据可读
					session_add_event( session, EV_READ|EV_ET );
				}
				else
				{
					// 出错, 
					channel_error( session, eIOError_ReadFailure );
				}
				
				break;
			}
			else if ( nread == 0 )
			{
				// 对端关闭连接
				channel_error( session, eIOError_PeerShutdown );
				break;
			}
		}
	}
	else
	{
		// 超时
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
		int32_t writen = 0;

		if ( arraylist_count(&session->outmsglist) > 0 )
		{
			// 发送数据
			writen = _transmit( session );

			// 处理结果
			if ( writen < 0 && errno != EAGAIN )
			{
				// 出错了
				channel_error( session, eIOError_WriteFailure );
			}
			else
			{
				// 正常发送
				// socket缓冲区已满

				if ( arraylist_count(&session->outmsglist) > 0 )
				{
					// 还有数据需要发送
					// NOTICE: 为什么不判断会话是否正在终止呢?
					// 为了尽量把数据发送完全, 所以只要不出错的情况下, 会一直发送
					// 直到发送队列为空
					session_add_event( session, EV_WRITE|EV_ET );
				}
				else
				{
					// 数据已经全部发送

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
			set_non_block( cfd );
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

void channel_on_connect( int32_t fd, int16_t ev, void * arg )
{
	int32_t rc = 0;
	int32_t result = 0;
	struct connector * connector = (struct connector *)arg;

	if ( ev & EV_WRITE )
	{
#if defined (linux)
		// linux需要进一步检查连接是否成功
		if ( is_connected( fd ) != 0 )
		{
			// 连接失败
			result = eIOError_ConnectStatus;
		}
#endif
	}
	else
	{
		// 连接超时了
		result = eIOError_Timeout;
	}
	
	// 把连接结果回调给逻辑层
	rc = connector->cb( connector->context, result );

	if ( rc == 0 )
	{
		if ( result != 0 )
		{
			// 逻辑层需要继续连接
			if ( connector->fd > 0 )
			{
				close( connector->fd );
			}
			
			// 重新连接
			// 不管是否连接上, 都定时监听写事件
			connector->fd = tcp_connect( connector->host, connector->port, 0 );
			client_connect_direct( connector->sets, connector );
		}
		else
		{
			// 连接成功
			struct session * session = connector->session;

			session_set_endpoint( session, connector->host, connector->port );
			session_start( session, 
					eSessionType_Persist, connector->fd, connector->sets );
			
			connector->fd = -1;
		}
	}

	return;
}

void channel_on_reconnect( int32_t fd, int16_t ev, void * arg )
{
	struct session * session = (struct session *)arg;

	session->status &= ~SESSION_WRITING;

	if ( ev & EV_WRITE )
	{
#if defined (linux)
		if ( is_connected(fd) != 0 )
		{
			// 重连失败
			channel_error( session, eIOError_ConnectStatus );
			return;
		}
#endif
		// 总算是连接上了

		// 重连需要重置描述符吗? 
		//session->fd = fd;

		// 注册读写事件
		// 把积累下来的数据全部发送出去
		session_add_event( session, EV_READ|EV_ET );
		session_add_event( session, EV_WRITE|EV_ET );

		// 启动keepalive服务
		session_start_keepalive( session );
	}
	else
	{
		// 超时
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

