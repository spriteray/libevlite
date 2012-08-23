
#include <stdio.h>
#include <syslog.h>
#include <stdarg.h>

#include "network.h"

#include "server.h"

static void _socket_option( int32_t fd );
static void _io_method( uint8_t index, evsets_t sets, int16_t type, void * task );

static int32_t _start_direct( uint8_t index, evsets_t sets, struct acceptor * acceptor );
static int32_t _assign_direct( evsets_t sets, struct stask_assign * task );
static int32_t _send_direct( struct stask_send * task );
static int32_t _shutdown_direct( struct stask_shutdown * task );

static inline struct session_manager * _get_manager( struct server * self, uint8_t index );

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

server_t server_create( const char * host, uint16_t port, uint8_t nthreads, uint32_t nclients )
{
	uint8_t i = 0;
	struct server * self = NULL;

	self = (struct server *)calloc( 1, sizeof(struct server) );
	if ( self == NULL )
	{
		return NULL;
	}

	self->port = port;
	self->nthreads = nthreads;
	self->nclients = nclients;

	if ( host )
	{
		self->host = strdup(host);
	}

	// 会话管理器, 
	// 采用cacheline对齐以提高访问速度
	self->managers = calloc( nthreads<<3, sizeof(void *) );
	if ( self->managers == NULL )
	{
		server_destroy( self );
		return NULL;
	}
	for ( i = 0; i < nthreads; ++i )
	{
		uint32_t index = i<<3;

		self->managers[index] = session_manager_create( index, nclients/nthreads );
		if ( self->managers[index] == NULL )
		{
			server_destroy( self );
			return NULL;
		}
	}

	// 创建网络线程组
	self->group = iothreads_create( nthreads, _io_method );
	if ( self->group == NULL )
	{
		server_destroy( self );
		return NULL;
	}

	return self;
}

void server_destroy( server_t self )
{

}

int32_t server_start( server_t self, int32_t (*cb)(void *, sid_t, const char *, uint16_t), void * context )
{
	int32_t fd = -1;
	struct server * server = (struct server *)self;

	// 创建listenfd
	fd = tcp_listen( server->host, server->port, _socket_option );
	if ( fd <= 0 )
	{
		return -1;
	}
	
	// 创建服务器接收器
	event_t event = event_create();
	struct acceptor * acceptor = malloc( sizeof(struct acceptor) );
	if ( event == NULL || acceptor == NULL )
	{
		server_stop( self );
		return -2;
	}
	acceptor->cb = cb;
	acceptor->context = context;
	acceptor->parent = server;
	acceptor->evaccept = event;

	server->acceptor = acceptor;
	
	// 线程组开始运行
	iothreads_start( server->group );

	return fd;
}

int32_t server_stop( server_t self )
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
		if ( acceptor->evaccept )
		{
			event_destroy( acceptor->evaccept );
			acceptor->evaccept = NULL;
		}

		free( acceptor );
		server->acceptor = NULL;
	}

	// 关闭listenfd
	if ( server->fd > 0 )
	{
		close( server->fd );
		server->fd = 0;
	}

	return 0;
}

int32_t server_set_timeout( server_t self, sid_t id, int32_t seconds )
{
	// NOT Thread-Safe
	uint8_t index = SID_INDEX(id);
	struct server * server = (struct server *)self;
	struct session_manager * manager = _get_manager( server, index );

	struct session * session = session_manager_get( manager, task->id );
	if ( session == NULL )
	{
		syslog(LOG_WARNING, "server_set_timeout() failed, session[%ld] is invalid", id );
		return -1;
	}

	session->setting.timeout_secs = seconds;

	return 0;
}

int32_t server_set_keepalive( server_t self, sid_t id, int32_t seconds )
{
	// NOT Thread-Safe
	uint8_t index = SID_INDEX(id);
	struct server * server = (struct server *)self;
	struct session_manager * manager = _get_manager( server, index );

	struct session * session = session_manager_get( manager, task->id );
	if ( session == NULL )
	{
		syslog(LOG_WARNING, "server_set_keepalive() failed, session[%ld] is invalid", id );
		return -1;
	}

	session->setting.keepalive_secs = seconds;

	return 0;
}

int32_t server_set_service( server_t self, sid_t id, ioservice_t * service, void * context )
{
	// NOT Thread-Safe
	uint8_t index = SID_INDEX(id);
	struct server * server = (struct server *)self;
	struct session_manager * manager = _get_manager( server, index );

	struct session * session = session_manager_get( manager, task->id );
	if ( session == NULL )
	{
		syslog(LOG_WARNING, "server_set_service() failed, session[%ld] is invalid", id );
		return -1;
	}

	session->context = context;
	session->service = *service;

	return 0;
}

int32_t server_send( server_t self, sid_t id, const char * buf, uint32_t nbytes, int32_t iscopy )
{
	uint8_t index = SID_INDEX(id);
	struct server * server = (struct server *)self;
	
	struct stask_send task;
	task.id		= id;
	task.buf	= buf;
	task.nbytes	= nbytes;
	task.manager= _get_manager( server, index );

	if ( pthread_self() == iothreads_get_id( server->group, index ) )
	{
		// 本线程内直接发送
		return _send_direct( &task );
	}

	// 跨线程提交发送任务
	return iothreads_post( server->group, index, eIOTaskType_Send, (void *)&task, sizeof(task) );
}

int32_t server_broadcast( server_t self, sid_t * ids, uint32_t count, const char * buf, uint32_t nbytes, int32_t iscopy )
{
	// 需要遍历ids
}

int32_t server_shutdown( server_t self, sid_t id )
{
	uint8_t index = SID_INDEX(id);
	struct server * server = (struct server *)self;

	struct stask_shutdown task;
	task.id		= id;
	task.manager= _get_manager( server, index );
	
#if 0
	if ( pthread_self() == iothreads_get_id( server->group, index ) )
	{
		// 本线程内直接关闭
		return _shutdown_direct( &task );
	}
#endif

	// 跨线程提交发送任务
	return iothreads_post( server->group, index, eIOTaskType_Shutdown, (void *)&task, sizeof(task) );

}

int32_t server_shutdowns( server_t self, sid_t * ids, uint32_t count )
{
	// 需要遍历ids
}


// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

int32_t server_assign_session( struct server * self, uint8_t index, struct stask_assign * task )
{
	evsets_t sets = iothreads_get_sets( self->group, index );
	pthread_t threadid = iothreads_get_id( self->group, index );

	// 取会话管理器
	task->manager = _get_manager( self, index );

	if ( pthread_self() == threadid )
	{
		// 该会话分发到本线程内了
		return _assign_direct( sets, task );
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

void _socket_option( int32_t fd )
{

}

int32_t _start_direct( evsets_t sets, struct acceptor * acceptor )
{
	event_set( acceptor->evaccept, acceptor->fd, EV_READ|EV_PERSIST );
	event_set_callback( acceptor->evaccept, channel_on_accept, acceptor );
	evsets_add( sets, acceptor->evaccept, 0 );

	return 0;
}

int32_t _assign_direct( evsets_t sets, struct stask_assign * task )
{
	int32_t rc = 0;
	int32_t key = task->fd;
	struct session_manager * manager = task->manager;

	struct session * session = session_manager_alloc( manager, key );
	if ( session == NULL )
	{
		syslog(LOG_WARNING, 
			"session_manager_alloc(fd:%d, host:'%s', port:%d) failed", task->fd, task->host, task->port );
		return -1;
	}

	rc = task->cb( task->context, session->id, task->host, task->port );
	if ( rc != 0 )
	{
		// 逻辑层不接受这个连接
		session_manager_remove( manager, session );
		return 1;
	}

	session_start( session, eSessionType_Once, task->fd, sets );
	return 0;
}

int32_t _send_direct( struct stask_send * task )
{
	struct session_manager * manager = task->manager;
	
	struct session * session = session_manager_get( manager, task->id );
	if ( session == NULL )
	{
		syslog(LOG_WARNING, "server_send_direct() failed, session[%ld] is invalid", id );
		return -1;
	}

	return session_send( session, task->buf, task->nbytes );
}

int32_t _shutdown_direct( struct stask_shutdown * task )
{
	struct session_manager * manager = task->manager;

	struct session * session = session_manager_get( manager, task->id );
	if ( session == NULL )
	{
		syslog(LOG_WARNING, "server_shutdown_direct() failed, session[%ld] is invalid", id );
		return -1;
	}

	return channel_shutdown( session );
}

void _io_method( uint8_t index, evsets_t sets, int16_t type, void * task )
{
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
			_assign_direct( sets, (struct stask_assign *)task );
		}
		break;

	case eIOTaskType_Send :
		{
			// 发送数据
			_send_direct( (struct stask_send *)task );
		}
		break;

	case eIOTaskType_Broadcast :
		{
			// 广播数据
		}
		break;

	case eIOTaskType_Shutdown :
		{
			// 关闭一个连接
			_shutdown_direct( (struct stask_shutdown *)task );
		}
		break;

	case eIOTaskType_Shutdowns :
		{
			// 批量关闭多个连接
		}
		break;
	
	}

	return;
}


