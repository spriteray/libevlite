
#include <stdio.h>

#include "network.h"

#include "server.h"

static void _io_method( uint8_t index, evsets_t sets, int16_t type, void * task );

static int32_t _send_direct( evsets_t sets, struct stask_send * task );
static int32_t _shutdown_direct( evsets_t sets, struct stask_shutdown * task );

static inline struct session_manager * _get_manager( struct server * self, uint8_t index );

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

}

int32_t server_stop( server_t self )
{

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
		// TODO: 记录日志, server_set_timeout() failed, session[%ld] is invalid
		return -1;
	}


	return 0;
}

int32_t server_set_keepalive( server_t self, sid_t id, int32_t seconds )
{
	// NOT Thread-Safe
}

int32_t server_set_service( server_t self, sid_t id, ioservice_t * service, void * context )
{
	// NOT Thread-Safe
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
		evsets_t sets = iothreads_get_sets( server->group, index );
		return _send_direct( sets, &task );
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

	if ( pthread_self() == iothreads_get_id( server->group, index ) )
	{
		// 本线程内直接关闭
		evsets_t sets = iothreads_get_sets( server->group, index );
		return _shutdown_direct( sets, &task );
	}

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

int32_t _send_direct( evsets_t sets, struct stask_send * task )
{
	struct session * session = session_manager_get( task->manager, task->id );
	if ( session == NULL )
	{
		// TODO: 记录日志, server_send_direct() failed, session[%ld] is invalid
		return -1;
	}

	return session_send( session, sets, task->buf, task->nbytes );
}

void _io_method( uint8_t index, evsets_t sets, int16_t type, void * task )
{
	switch ( type )
	{
	
	case eIOTaskType_Listen :
		{
			// 打开一个服务器
		}
		break;

	case eIOTaskType_Send :
		{
			// 发送数据
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
			_shutdown_direct( sets, (struct stask_shutdown *)task );
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


