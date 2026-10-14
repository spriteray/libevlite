
#ifndef ADDRTABLE_H
#define ADDRTABLE_H

#include <stdio.h>
#include <stdint.h>
#include <sys/socket.h>

// 二进制地址(族 + 端口 + 地址)
struct epkey {
    uint16_t family;
    uint16_t port;
    uint8_t addr[16];
};

struct endpoint {
    struct epkey key;
    void * value;
    struct endpoint * next;
};

// 1 - 创建
// 2 - 移除
// 3 - 销毁
typedef void ( *helper_t )( int, struct endpoint * );

// 地址散列表
struct addresstable {
    struct endpoint ** table;
    size_t size;
    size_t sizemask;
    size_t count;
    size_t objsize;
    helper_t helper;
};

// 创建散列表
struct addresstable * addrtable_create(
    size_t count, size_t size, helper_t helper );

void addrtable_destroy( struct addresstable * self );

// 查找
void * addrtable_find(
    struct addresstable * self, const struct sockaddr_storage * addr );

// 删除
void addrtable_remove(
    struct addresstable * self, const struct sockaddr_storage * addr );

// 添加
void * addrtable_append(
    struct addresstable * self, const struct sockaddr_storage * addr );

#endif
