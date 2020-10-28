
#ifndef ACCEPTQUEUE_H
#define ACCEPTQUEUE_H

#include "event.h"
#include "message.h"

struct udpentry
{
    uint16_t                port;
    char                    host[64];
    event_t                 event;
    struct buffer           buffer;
    struct acceptor *       acceptor;
    struct udpentry *       next;
};

// 接收队列
struct acceptqueue
{
    struct udpentry **      table;
    size_t                  size;
    size_t                  sizemask;
    size_t                  count;
};

// 创建接受队列
struct acceptqueue * acceptqueue_create( size_t count );
void acceptqueue_destroy( struct acceptqueue * self );

// 查找
struct udpentry * acceptqueue_find(
        struct acceptqueue * self, const char * host, uint16_t port );

// 删除
void acceptqueue_remove(
        struct acceptqueue * self, const char * host, uint16_t port );

// 添加
struct udpentry * acceptqueue_append(
        struct acceptqueue * self, const char * host, uint16_t port );

#endif
