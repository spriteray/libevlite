
#ifndef EVENT_H
#define EVENT_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

//
// 事件库所支持的事件类型
//

#define EV_READ		0x01    // 读事件
#define EV_WRITE	0x02    // 写事件
#define EV_TIMEOUT	0x04    // 超时事件
#define EV_PERSIST	0x08    // 永久模式
#define EV_ET		0x10    // Edge Triggered模式


//
// 事件的定义, 以及事件集的定义
//

typedef void * event_t;
typedef void * evsets_t;

//
// 事件的方法
//

// 创建事件
event_t event_create();

// 设置事件的一些基础属性
// fd - 关注的描述符; ev - 关注的事件,即以上定义的那五种
void event_set( event_t self, int32_t fd, int16_t ev );

// 设置事件的回调函数
// 设置发生事件后的回调函数
void event_set_callback( event_t self, void (*cb)(int32_t, int16_t, void *), void * arg );

// 获取事件关注的描述符FD
int32_t event_get_fd( event_t self );

// 获取事件所属事件集
evsets_t event_get_sets( event_t self );

// 销毁事件
void event_destroy( event_t self );

// 
// 事件集的方法
//

// 创建事件集
evsets_t evsets_create();

// 事件库的版本
const char * evsets_get_version();

// 向事件集中添加事件
// 返回0, 参数指定的事件成功的添加到事件集中
// 返回1, 参数指定的事件非法, 没有添加到事件集中
// 返回<0, 添加事件失败
int32_t evsets_add( evsets_t self, event_t ev, int32_t tv );

// 从事件集中删除事件
int32_t evsets_del( evsets_t self, event_t ev );

// 分发并处理事件
// 返回激活的事件个数
int32_t evsets_dispatch( evsets_t self );

// 销毁事件集
void evsets_destroy( evsets_t self );

#ifdef __cplusplus
}
#endif

#endif

