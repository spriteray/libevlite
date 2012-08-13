
#include "threads.h"



void * iothread_main( void * arg )
{
	struct iothread * thread = (struct iothread *)arg;
	struct iothreads * parent = (struct iothreads *)(thread->parent);

	while ( parent->flags )
	{
		// 轮询网络事件
		evsets_dispatch( thread->sets );

		// 处理任务

	}

	return NULL;
}

