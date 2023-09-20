
#ifndef EPHASHTABLE_H
#define EPHASHTABLE_H

#include <stdio.h>
#include <stdint.h>

struct endpoint
{
    uint16_t                port;
    char                    host[64];
    void *                  value;
    struct endpoint *       next;
};

typedef void (*helper_t)( int, struct endpoint * );

// ep散列表
struct ephashtable
{
    struct endpoint **      table;
    size_t                  size;
    size_t                  sizemask;
    size_t                  count;
    size_t                  objsize;
    helper_t                helper;
};

// 创建散列表
struct ephashtable * ephashtable_create( size_t count, size_t size, helper_t helper );
void ephashtable_destroy( struct ephashtable * self );

// 查找
void * ephashtable_find(
        struct ephashtable * self, const char * host, uint16_t port );

// 删除
void ephashtable_remove(
        struct ephashtable * self, const char * host, uint16_t port );

// 添加
void * ephashtable_append(
        struct ephashtable * self, const char * host, uint16_t port );

#endif
