
#include <stdio.h>
#include <syslog.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#include "network.h"

#include "channel.h"
#include "session.h"
#include "network-internal.h"

#include "server.h"

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

static int32_t _new_managers( struct server * self );
static int32_t _new_acceptor( struct server * self, 
							const char * host, uint16_t port, 
							int32_t (*cb)(void *, sid_t, const char *, uint16_t), void * context );

static int32_t _start_direct( evsets_t sets, struct acceptor * acceptor );
static int32_t _assign_direct( struct session_manager * manager, evsets_t sets, struct stask_assign * task );
static int32_t _send_direct( struct session_manager * manager, struct stask_send * task );
static int32_t _broadcast_direct( struct session_manager * manager, struct message * msg );
static int32_t _shutdown_direct( struct session_manager * manager, sid_t id );
static int32_t _shutdowns_direct( struct session_manager * manager, struct sidlist * ids );

static void _socket_option( int32_t fd );
static void _io_methods( void * context, uint8_t index, int16_t type, void * task );

static inline struct session_manager * _get_manager( struct server * self, uint8_t index );
static inline void _dispatch_sidlist( struct server * self, struct sidlist ** listgroup, sid_t * ids, uint32_t count );
static inline int32_t _send_buffer( struct server * self, sid_t id, const char * buf, uint32_t nbytes, int32_t iscopy );

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

int32_t _new_managers( struct server * self )
{
	uint8_t i = 0;
	uint32_t sessions_per_thread = self->nclients/self->nthreads; 

	// 会话管理器, 
	// 采用cacheline对齐以提高访问速度
	self->managers = calloc( (self->nthreads)<<3, sizeof(void *) );
	if ( self->managers == NULL )
	{
		return -1;
	}
	for ( i = 0; i < self->nthreads; ++i )
	{
		uint32_t index = i<<3;

		self->managers[index] = session_manager_create( i, sessions_per_thread );
		if ( self->managers[index] == NULL )
		{
			return -2;
		}
	}

	return 0;
}

int32_t _new_acceptor( struct server * self, 
							const char * host, uint16_t port, 
							int32_t (*cb)(void *, sid_t, const char *, uint16_t), void * context )
{
	struct acceptor * acceptor = (struct acceptor *)self->acceptor;

	// 创建接收事件
	acceptor->event = event_create();
	if ( acceptor->event == NULL )
	{
		return -1;
	}

	// 创建listenfd
	acceptor->fd = tcp_listen( (char *)host, port, _socket_option );
	if ( acceptor->fd <= 0 )
	{
		return -2;
	}

	// 接收器初始化
	acceptor->cb = cb;
	acceptor->context = context;
	acceptor->parent = self;
	acceptor->port = port;
	strncpy( acceptor->host, host, INET_ADDRSTRLEN );

	return 0;
}

// 服务器开启
server_t server_start( const char * host, uint16_t port,
							uint8_t nthreads, uint32_t nclients, 
							int32_t (*cb)(void *, sid_t, const char *, uint16_t), void * context )
{
	int32_t rc = 0;
	uint8_t index = 0;

	struct server * self = NULL;
	self = (struct server *)calloc( 1, sizeof(struct server)+sizeof(struct acceptor) );
	if ( self == NULL )
	{
		return NULL;
	}

	self->nthreads = nthreads;
	self->nclients = nclients;
	self->acceptor = (void *)(self + 1);

	// 初始化会话管理器
	rc = _new_managers( self );
	if ( rc != 0 )
	{
		server_stop( self );
		return NULL;
	}

	// 创建会话接收器
	rc = _new_acceptor( self, host, port, cb, context );
	if ( rc != 0 )
	{
		server_stop( self );
		return NULL;
	}

	// 创建网络线程组
	self->group = iothreads_create( self->nthreads, _io_methods, self );
	if ( self->group == NULL )
	{
		server_stop( self );
		return NULL;
	}

	// 服务器开启
	iothreads_start( self->group );

	// 随机提交一个网络线程负责接收
	index = time(NULL) % (self->nthreads);
	iothreads_post( self->group, index, eIOTaskType_Listen, self->acceptor, 0 );

	return self;
}

void server_stop( server_t self )
{
	struct server * server = (struct server *)self;
	struct acceptor * acceptor = (struct acceptor *)server->acceptor;

	// 停止网络线程组
	if ( server->group )
	{
		iothreads_stop( server->group );
	}

	// 销毁服务器接收器
	if ( acceptor )
	{
		// 销毁接收事件
		// 网络线程组停止时事件集就已经销毁了
		if ( acceptor->event )
		{
			event_destroy( acceptor->event );
			acceptor->event = NULL;
		}
		
		// 关闭描述符
		if ( acceptor->fd > 0 )
		{
			close( acceptor->fd );
			acceptor->fd = 0;
		}
	}

	// 销毁管理器
	if ( server->managers )
	{
		uint8_t i = 0;
		for ( i = 0; i < server->nthreads; ++i )
		{
			struct session_manager * manager = server->managers[i<<3];	
			if ( manager )
			{
				session_manager_destroy( manager );
			}
		}
		free( server->managers );
		server->managers = NULL;
	}

	// 销毁网络线程组
	if ( server->group )
	{
		iothreads_destroy( server->group );
		server->group = NULL;
	}

	free( server );

	return; 
}

int32_t server_set_timeout( server_t self, sid_t id, int32_t seconds )
{
	// NOT Thread-Safe
	uint8_t index = SID_INDEX(id);
	struct server * server = (struct server *)self;

	if ( index >= server->nthreads )
	{
		syslog(LOG_WARNING, "server_set_timeout(SID=%ld) failed, the Session's index[%u] is invalid .", id, index );
		return -1;
	}

	struct session_manager * manager = _get_manager( server, index );
	if ( manager == NULL )
	{
		syslog(LOG_WARNING, "server_set_timeout(SID=%ld) failed, the Session's manager[%u] is invalid .", id, index );
		return -2;
	}

	struct session * session = session_manager_get( manager, id );
	if ( session == NULL )
	{
		syslog(LOG_WARNING, "server_set_timeout(SID=%ld) failed, the Session is invalid .", id );
		return -3;
	}

	session->setting.timeout_msecs = seconds*1000;

	return 0;
}

int32_t server_set_keepalive( server_t self, sid_t id, int32_t seconds )
{
	// NOT Thread-Safe
	uint8_t index = SID_INDEX(id);
	struct server * server = (struct server *)self;

	if ( index >= server->nthreads )
	{
		syslog(LOG_WARNING, "server_set_keepalive(SID=%ld) failed, the Session's index[%u] is invalid .", id, index );
		return -1;
	}

	struct session_manager * manager = _get_manager( server, index );
	if ( manager == NULL )
	{
		syslog(LOG_WARNING, "server_set_keepalive(SID=%ld) failed, the Session's manager[%u] is invalid .", id, index );
		return -2;
	}

	struct session * session = session_manager_get( manager, id );
	if ( session == NULL )
	{
		syslog(LOG_WARNING, "server_set_keepalive(SID=%ld) failed, the Session is invalid .", id );
		return -3;
	}

	session->setting.keepalive_msecs = seconds*1000;

	return 0;
}

int32_t server_set_service( server_t self, sid_t id, ioservice_t * service, void * context )
{
	// NOT Thread-Safe
	uint8_t index = SID_INDEX(id);
	struct server * server = (struct server *)self;

	if ( index >= server->nthreads )
	{
		syslog(LOG_WARNING, "server_set_service(SID=%ld) failed, the Session's index[%u] is invalid .", id, index );
		return -1;
	}

	struct session_manager * manager = _get_manager( server, index );
	if ( manager == NULL )
	{
		syslog(LOG_WARNING, "server_set_service(SID=%ld) failed, the Session's manager[%u] is invalid .", id, index );
		return -2;
	}

	struct session * session = session_manager_get( manager, id );
	if ( session == NULL )
	{
		syslog(LOG_WARNING, "server_set_service(SID=%ld) failed, the Session is invalid .", id );
		return -3;
	}

	session->context = context;
	session->service = *service;

	return 0;
}

int32_t server_send( server_t self, sid_t id, const char * buf, uint32_t nbytes, int32_t iscopy )
{
	return _send_buffer( (struct server *)self, id, buf, nbytes, iscopy );
}

int32_t server_broadcast( server_t self, sid_t * ids, uint32_t count, const char * buf, uint32_t nbytes, int32_t iscopy )
{
	// 需要遍历ids
	uint8_t i = 0;
	int32_t rc = 0;
	pthread_t threadid = pthread_self();

	struct sidlist * listgroup[ 256 ] = {NULL};
	struct server * server = (struct server *)self;

	_dispatch_sidlist( server, listgroup, ids, count );

	for ( i = 0; i < server->nthreads; ++i )
	{
		if ( listgroup[i] == NULL )
		{
			continue;
		}

		if ( sidlist_count( listgroup[i] ) > 1 )
		{
			struct message * msg = message_create();
			if ( msg == NULL )
			{
				continue;
			}

			message_add_buffer( msg, (char *)buf, nbytes );
			message_set_receivers( msg, listgroup[i] );

			if ( threadid == iothreads_get_id( server->group, i ) )
			{
				// 本线程内直接广播
				_broadcast_direct( _get_manager(server, i), msg );
			}
			else
			{
				// 跨线程提交广播任务
				iothreads_post( server->group, i, eIOTaskType_Broadcast, msg, 0 );
			}

			rc += sidlist_count( listgroup[i] );
			// listgroup[i] 会在message中销毁
		}
		else
		{
			sid_t id = sidlist_get( listgroup[i], 0 );

			if ( _send_buffer( server, id, buf, nbytes, iscopy ) == 0 )
			{
				// 发送成功
				rc += 1;
			}

			// 销毁listgroup[i]
			sidlist_destroy( listgroup[i] );
		}
	}

	return rc;
}

int32_t server_shutdown( server_t self, sid_t id )
{
	uint8_t index = SID_INDEX(id);
	struct server * server = (struct server *)self;

	if ( index >= server->nthreads )
	{
		syslog(LOG_WARNING, "server_shutdown(SID=%ld) failed, the Session's index[%u] is invalid .", id, index );
		return -1;
	}
	
	// 避免在回调函数中直接终止会话
	// 这样会引发后续对会话的操作都非法了
#if 0
	if ( pthread_self() == iothreads_get_id( server->group, index ) )
	{
		// 本线程内直接终止
		return _shutdown_direct( _get_manager(server, index), &task );
	}
#endif

	// 跨线程提交终止任务
	return iothreads_post( server->group, index, eIOTaskType_Shutdown, (void *)&id, sizeof(id) );

}

int32_t server_shutdowns( server_t self, sid_t * ids, uint32_t count )
{
	// 需要遍历ids
	uint8_t i = 0;
	int32_t rc = 0;

	struct sidlist * listgroup[ 256 ] = {NULL};
	struct server * server = (struct server *)self;

	_dispatch_sidlist( server, listgroup, ids, count );

	for ( i = 0; i < server->nthreads; ++i )
	{
		if ( listgroup[i] == NULL )
		{
			continue;
		}

		// 参照server_shutdown()

		// 跨线程提交批量终止任务
		rc += sidlist_count( listgroup[i] );
		iothreads_post( server->group, i, eIOTaskType_Shutdowns, listgroup[i], 0 );
	}

	return rc;
}


// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

int32_t server_assign_session( struct server * self, uint8_t index, struct stask_assign * task )
{
	evsets_t sets = iothreads_get_sets( self->group, index );
	pthread_t threadid = iothreads_get_id( self->group, index );

	if ( pthread_self() == threadid )
	{
		// 该会话分发到本线程内了
		return _assign_direct( _get_manager(self, index), sets, task );
	}

	// 跨线程提交发送任务
	return iothreads_post( self->group, index, eIOTaskType_Assign, task, sizeof(struct stask_assign) );
}
 

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

struct session_manager * _get_manager( struct server * self, uint8_t index )
{
	if ( index >= self->nthreads )
	{
		return NULL;
	}

	return (struct session_manager *)( self->managers[index<<3] );
}

void _dispatch_sidlist( struct server * self, struct sidlist ** listgroup, sid_t * ids, uint32_t count )
{
	uint32_t i = 0;

	for ( i = 0; i < count; ++i )
	{
		uint8_t index = SID_INDEX( ids[i] );

		// index非法
		if ( index >= self->nthreads )
		{
			continue;
		}

		// list未创建
		if ( listgroup[index] == NULL )
		{
			listgroup[index] = sidlist_create( count );
		}

		// 添加到list中
		if ( listgroup[index] )
		{
			sidlist_add( listgroup[index], ids[i] );
		}
	}

	return;
}

int32_t _send_buffer( struct server * self, sid_t id, const char * buf, uint32_t nbytes, int32_t iscopy )
{
	uint8_t index = SID_INDEX(id);

	if ( index >= self->nthreads )
	{
		syslog(LOG_WARNING, "_send_buffer(SID=%ld) failed, the Session's index[%u] is invalid .", id, index );
		return -1;
	}

	if ( iscopy )
	{
		char * buf2 = (char *)malloc( nbytes );
		if ( buf2 == NULL )
		{
			syslog(LOG_WARNING, "_send_buffer(SID=%ld) failed, can't allocate the memory for 'buf2' .", id );
			return -2;
		}

		memcpy( buf2, buf, nbytes );
		buf = buf2;
	}

	struct stask_send task;
	task.id		= id;
	task.buf	= (char *)buf;
	task.nbytes	= nbytes;

	if ( pthread_self() == iothreads_get_id( self->group, index ) )
	{
		// 本线程内直接发送
		return _send_direct( _get_manager(self, index), &task );
	}

	// 跨线程提交发送任务
	return iothreads_post( self->group, index, eIOTaskType_Send, (void *)&task, sizeof(task) );
}

void _socket_option( int32_t fd )
{
	int32_t flag = 0;
	struct linger ling;
	
	// Socket非阻塞
	set_non_block( fd );
	
	flag = 1;
	setsockopt( fd, SOL_SOCKET, SO_REUSEADDR, (void *)&flag, sizeof(flag) );
	
	flag = 1;
	setsockopt( fd, IPPROTO_TCP, TCP_NODELAY, (void *)&flag, sizeof(flag) );

#if SAFE_SHUTDOWN
	ling.l_onoff = 1;
	ling.l_linger = MAX_SECONDS_WAIT_FOR_SHUTDOWN };
#else
	ling.l_onoff = 1;
	ling.l_linger = 0; 
#endif
	setsockopt( fd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling) );
	
	// 发送接收缓冲区
#if SEND_BUFFER_SIZE > 0
	int32_t sendbuf_size = SEND_BUFFER_SIZE;
	setsockopt( fd, SOL_SOCKET, SO_SNDBUF, (void *)&sendbuf_size, sizeof(sendbuf_size) );
#endif
#if RECV_BUFFER_SIZE > 0
	int32_t recvbuf_size = RECV_BUFFER_SIZE;
	setsockopt( fd, SOL_SOCKET, SO_RCVBUF, (void *)&recvbuf_size, sizeof(recvbuf_size) );
#endif

	return;
}

int32_t _start_direct( evsets_t sets, struct acceptor * acceptor )
{
	// 开始关注accept事件
	
	event_set( acceptor->event, acceptor->fd, EV_READ|EV_PERSIST );
	event_set_callback( acceptor->event, channel_on_accept, acceptor );
	evsets_add( sets, acceptor->event, 0 );

	return 0;
}

int32_t _assign_direct( struct session_manager * manager, evsets_t sets, struct stask_assign * task )
{
	int32_t rc = 0;
	int32_t key = task->fd;
	
	// 会话管理器分配会话
	struct session * session = session_manager_alloc( manager, key );
	if ( session == NULL )
	{
		syslog(LOG_WARNING, 
			"_assign_direct(fd:%d, host:'%s', port:%d) failed .", task->fd, task->host, task->port );
		close( task->fd );
		return -1;
	}
	
	// 回调逻辑层, 确定是否接收这个会话
	rc = task->cb( task->context, session->id, task->host, task->port );
	if ( rc != 0 )
	{
		// 逻辑层不接受这个会话
		session_manager_remove( manager, session );
		close( task->fd );
		return 1;
	}
	
	// 会话开始
	session_set_endpoint( session, task->host, task->port );
	session_start( session, eSessionType_Once, task->fd, sets );
	
	return 0;
}

int32_t _send_direct( struct session_manager * manager, struct stask_send * task )
{
	struct session * session = session_manager_get( manager, task->id );

	if ( session == NULL )
	{
		syslog(LOG_WARNING, "_send_direct(SID=%ld) failed, the Session is invalid .", task->id );
		return -1;
	}

	return session_send( session, task->buf, task->nbytes );
}

int32_t _broadcast_direct( struct session_manager * manager, struct message * msg )
{
	uint32_t i = 0;
	int32_t rc = 0;

	for ( i = 0; i < sidlist_count(msg->tolist); ++i )
	{
		sid_t id = sidlist_get(msg->tolist, i);
		struct session * session = session_manager_get( manager, id );

		if ( session == NULL )
		{
			// 会话非法
			message_add_failure( msg, id );
			continue;
		}

		// 直接添加到会话的发送列表中
		if ( session_append(session, msg) != 0 )
		{
			// 添加失败
			message_add_failure( msg, id );
			continue;
		}

		++rc;
	}

	// 消息发送失败, 直接销毁
	if ( message_is_complete(msg) == 0 )
	{
		message_destroy( msg );
	}

	return rc;
}

int32_t _shutdown_direct( struct session_manager * manager, sid_t id )
{
	struct session * session = session_manager_get( manager, id );

	if ( session == NULL )
	{
		syslog(LOG_WARNING, "_shutdown_direct(SID=%ld) failed, the Session is invalid .", id );
		return -1;
	}

	return session_shutdown( session );
}

int32_t _shutdowns_direct( struct session_manager * manager, struct sidlist * ids )
{
	uint32_t i = 0;
	int32_t rc = 0;

	for ( i = 0; i < sidlist_count(ids); ++i )
	{
		sid_t id = sidlist_get(ids, i);
		struct session * session = session_manager_get( manager, id );

		if ( session == NULL )
		{
			continue;
		}

		// 直接终止
		++rc;
		session_shutdown( session );
	}

	sidlist_destroy( ids );

	return rc;
}

void _io_methods( void * context, uint8_t index, int16_t type, void * task )
{
	struct server * server = (struct server *)context;

	// 获取事件集以及会话管理器
	evsets_t sets = iothreads_get_sets( server->group, index );
	struct session_manager * manager = _get_manager( server, index );
	
	switch ( type )
	{
	
	case eIOTaskType_Listen :
		{
			// 打开一个服务器
			_start_direct( sets, (struct acceptor *)task );
		}
		break;

	case eIOTaskType_Assign :
		{
			// 分配一个描述符
			_assign_direct( manager, sets, (struct stask_assign *)task );
		}
		break;

	case eIOTaskType_Send :
		{
			// 发送数据
			_send_direct( manager, (struct stask_send *)task );
		}
		break;

	case eIOTaskType_Broadcast :
		{
			// 广播数据
			_broadcast_direct( manager, (struct message *)task );
		}
		break;

	case eIOTaskType_Shutdown :
		{
			// 终止一个会话
			_shutdown_direct( manager, *( (sid_t *)task ) );
		}
		break;

	case eIOTaskType_Shutdowns :
		{
			// 批量终止多个会话
			_shutdowns_direct( manager, (struct sidlist *)task );
		}
		break;
	
	}

	return;
}

