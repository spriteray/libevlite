
#ifndef UTILS_H
#define UTILS_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 工具算法模块
 */

#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/uio.h>

#include "config.h"

//
// 基础算法
//
uint32_t getpower( uint32_t size );
uint32_t nextpow2( uint32_t size );

//
// 系统相关的操作
//

// 睡眠
void msleep( int32_t mseconds );

// 时间函数, 返回毫秒数/微妙数
int64_t milliseconds();
int64_t microseconds();

// 获取线程ID
#if defined EVENT_OS_LINUX
pid_t threadid();
#endif

// socket基本操作
int32_t is_ipv6only( int32_t fd );
int32_t is_connected( int32_t fd );
int32_t set_cloexec( int32_t fd );
int32_t set_non_block( int32_t fd );
int32_t unix_connect( const char * path, int32_t ( *options )( int32_t ) );
int32_t unix_listen( const char * path, int32_t ( *options )( int32_t ) );
int32_t tcp_accept( int32_t fd, char * remotehost, uint16_t * remoteport );
int32_t tcp_listen( const char * host, uint16_t port, int32_t ( *options )( int32_t ) );
int32_t tcp_connect( const char * host, uint16_t port, int32_t ( *options )( int32_t ) );
int32_t udp_bind( const char * host, uint16_t port, int32_t ( *options )( int32_t ), struct sockaddr_storage * addr );
int32_t udp_connect( struct sockaddr_storage * localaddr,
    struct sockaddr_storage * remoteaddr, int32_t ( *options )( int32_t ) );
int32_t parse_endpoint( struct sockaddr_storage * addr, char * host, uint16_t * port );
void convert_endpoint( struct sockaddr_storage * addr, int32_t type, const char * host, uint16_t port );

#ifdef __cplusplus
}
#endif

#endif
