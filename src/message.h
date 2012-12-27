
#ifndef SRC_MESSAGE_H
#define SRC_MESSAGE_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

#include "utils.h"

//
// 缓冲区
//
struct buffer
{
	uint32_t offset;		// 有效数据段相对于原始数据段的偏移量
	uint32_t length;		// 有效数据段的长度
	uint32_t totallen;		// 内存块的总长度

	char * buffer;			// 有效数据段
	char * orignbuffer;		// 原始数据段
};

// 设置缓冲区
// 速度快, 不存在内存copy, buf一定是malloc()出来的内存地址
int32_t buffer_set( struct buffer * self, char * buf, uint32_t length );

// 获取网络缓冲区得大小和数据
#define buffer_data( self )			(self)->buffer
#define buffer_length( self )		(self)->length

//
int32_t buffer_erase( struct buffer * self, uint32_t length );
int32_t buffer_append( struct buffer * self, char * buf, uint32_t length );
uint32_t buffer_take( struct buffer * self, char * buf, uint32_t length );

// 两个缓冲区相互交换
void buffer_exchange( struct buffer * buf1, struct buffer * buf2 );

// -1, 系统调用read()返回出错; -2, 返回expand()失败
int32_t buffer_read( struct buffer * self, int32_t fd, int32_t nbytes );


//
// 缓冲区池
//

// TODO: 是否有必要写一个对象池来管理所有缓冲区的分配与释放

//
// 消息
//
struct message
{
    int32_t nsuccess;

    struct sidlist * tolist;
    struct sidlist * failurelist;
    
    struct buffer 	buffer;
};

// 创建/销毁 消息
struct message * message_create();
void message_destroy( struct message * self );

// 增加消息的接收者
// 设置消息的接收列表
int32_t message_add_receiver( struct message * self, sid_t id );
int32_t message_set_receivers( struct message * self, struct sidlist * ids );

//
int32_t message_add_failure( struct message * self, sid_t id );
#define message_add_success( self )					++((self)->nsuccess)

// 添加/设置 消息的数据
#define message_set_buffer( self, buf, nbytes ) 	buffer_set( &((self)->buffer), (buf), (nbytes) )
#define message_add_buffer( self, buf, nbytes ) 	buffer_append( &((self)->buffer), (buf), (nbytes) )

// 消息是否完全发送
int32_t message_left_count( struct message * self );
#define message_is_complete( self ) 				( message_left_count( (self) ) == 0 )

// 获取消息数据的长度以及内
#define message_get_buffer( self )					buffer_data( &((self)->buffer) )
#define message_get_length( self ) 					buffer_length( &((self)->buffer) )

#ifdef __cplusplus
}
#endif

#endif

