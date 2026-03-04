
#include <assert.h>
#include <strings.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>

#include <sys/un.h>
#include <sys/time.h>
#include <sys/syscall.h>

#include <netdb.h>

#include "utils.h"
#include "config.h"

#if defined EVENT_OS_LINUX

__thread pid_t t_cached_threadid = 0;

pid_t threadid()
{
    if ( t_cached_threadid == 0 ) {
        t_cached_threadid = syscall( SYS_gettid );
    }

    return t_cached_threadid;
}

#endif

uint32_t getpower( uint32_t size )
{
    uint32_t n = 0;

    for ( ; size >>= 1; ) {
        ++n;
    }

    return n;
}

uint32_t nextpow2( uint32_t size )
{
    --size;
    size |= size >> 1;
    size |= size >> 2;
    size |= size >> 4;
    size |= size >> 8;
    size |= size >> 16;
    return ++size;
}

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

void msleep( int32_t mseconds )
{
    struct timeval tv;

    tv.tv_sec = mseconds / 1000;
    tv.tv_usec = ( mseconds % 1000 ) * 1000;

    select( 0, NULL, NULL, NULL, &tv );
}

int64_t milliseconds()
{
    int64_t now = -1;
    struct timeval tv;

    if ( gettimeofday( &tv, NULL ) == 0 ) {
        now = tv.tv_sec * 1000ll + tv.tv_usec / 1000ll;
    }

    return now;
}

int64_t microseconds()
{
    int64_t now = -1;
    struct timeval tv;

    if ( gettimeofday( &tv, NULL ) == 0 ) {
        now = tv.tv_sec * 1000000ll + tv.tv_usec;
    }

    return now;
}

int32_t is_ipv6only( int32_t fd )
{
    int yes = 1;
    socklen_t length = sizeof( yes );

    if ( setsockopt( fd, IPPROTO_IPV6,
        IPV6_V6ONLY, &yes, length ) == -1 ) {
        return -1;
    }

    return 0;
}

int32_t is_connected( int32_t fd )
{
    int32_t value = -1;
    socklen_t length = sizeof( int32_t );

    if ( getsockopt( fd, SOL_SOCKET,
        SO_ERROR, (void *)&value, &length ) == 0 ) {
        return value;
    }

    return value;
}

int32_t set_cloexec( int32_t fd )
{
    int32_t flags;
    int32_t rc = -1;

    flags = fcntl( fd, F_GETFD );
    if ( flags >= 0 ) {
        flags |= FD_CLOEXEC;
        rc = fcntl( fd, F_SETFD, flags ) != -1 ? 0 : -2;
    }

    return rc;
}

int32_t set_non_block( int32_t fd )
{
    int32_t flags;
    int32_t rc = -1;

    flags = fcntl( fd, F_GETFL );
    if ( flags >= 0 ) {
        flags |= O_NONBLOCK;
        rc = fcntl( fd, F_SETFL, flags ) != -1 ? 0 : -2;
    }

    return rc;
}

int32_t unix_connect( const char * path, int32_t ( *options )( int32_t ) )
{
    int32_t fd = socket( AF_UNIX, SOCK_STREAM, 0 );
    if ( fd < 0 ) {
        return -4;
    }

    // 对描述符的选项操作
    if ( options( fd ) != 0 ) {
        close( fd );
        return -5;
    }

    struct sockaddr_un addr;
    memset( &addr, 0, sizeof( addr ) );
    addr.sun_family = AF_LOCAL;
    strncpy( addr.sun_path, path, sizeof( addr.sun_path ) - 1 );

    // bind
    if ( connect( fd,
        (struct sockaddr *)&addr, sizeof( addr ) ) == -1 ) {
        close( fd );
        return -6;
    }

    return fd;
}

int32_t unix_listen( const char * path, int32_t ( *options )( int32_t ) )
{
    // 删除文件
    if ( unlink( path ) != 0 && errno != ENOENT ) {
        return -3;
    }

    int32_t fd = socket( AF_UNIX, SOCK_STREAM, 0 );
    if ( fd < 0 ) {
        return -4;
    }

    // 对描述符的选项操作
    if ( options( fd ) != 0 ) {
        close( fd );
        return -5;
    }

    struct sockaddr_un addr;
    memset( &addr, 0, sizeof( addr ) );
    addr.sun_family = AF_LOCAL;
    strncpy( addr.sun_path, path, sizeof( addr.sun_path ) - 1 );

    // bind
    if ( bind( fd,
        (struct sockaddr *)&addr, sizeof( addr ) ) == -1 ) {
        close( fd );
        return -6;
    }

    // listen
    if ( listen( fd, SOMAXCONN ) == -1 ) {
        close( fd );
        return -7;
    }

    // 修改文件的权限
    chmod( path, (mode_t)0700 );

    return fd;
}

int32_t tcp_accept( int32_t fd, char * remotehost, uint16_t * remoteport )
{
    int32_t cfd = -1;
    struct sockaddr_storage cli_addr;
    socklen_t len = sizeof( cli_addr );

    *remoteport = 0;
    remotehost[0] = 0;
    bzero( &cli_addr, len );

    cfd = accept( fd, (struct sockaddr *)&cli_addr, &len );
    if ( cfd != -1 ) {
        // 解析获得目标ip和端口
        parse_endpoint( &cli_addr, remotehost, remoteport );
    }

    return cfd;
}

int32_t tcp_listen( const char * host, uint16_t port, int32_t ( *options )( int32_t ) )
{
    int32_t fd = -1;
    char strport[6];
    struct addrinfo hints, *res, *p;

    bzero( &hints, sizeof( hints ) );
    snprintf( strport, sizeof( strport ), "%d", port );
    // 设置参数
    hints.ai_flags = AI_PASSIVE;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    // 获取地址信息
    if ( getaddrinfo( host, strport, &hints, &res ) != 0 ) {
        return port == 0 ? unix_listen( host, options ) : -1;
    }

    for ( p = res; p != NULL; p = p->ai_next ) {
        fd = socket( p->ai_family, p->ai_socktype, p->ai_protocol );
        if ( fd < 0 ) {
            continue;
        }

        // 对描述符的选项操作
        if ( options( fd ) != 0 ) {
            close( fd );
            continue;
        }

        // bind
        if ( bind( fd, p->ai_addr, p->ai_addrlen ) == -1 ) {
            close( fd );
            continue;
        }

        // listen
        if ( listen( fd, SOMAXCONN ) == -1 ) {
            close( fd );
            continue;
        }

        // 绑定成功后退出循环
        break;
    }

    freeaddrinfo( res );
    if ( p == NULL ) {
        return port == 0 ? unix_listen( host, options ) : -2;
    }

    return fd;
}

int32_t tcp_connect( const char * host, uint16_t port, int32_t ( *options )( int32_t ) )
{
    if ( port == 0 ) {
        return unix_connect( host, options );
    }

    int32_t fd = -1;
    char strport[6];
    struct addrinfo hints, *res, *p;

    bzero( &hints, sizeof( hints ) );
    snprintf( strport, sizeof( strport ), "%d", port );
    // 设置参数
    hints.ai_flags = AI_PASSIVE;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    // 获取地址信息
    if ( getaddrinfo( host, strport, &hints, &res ) != 0 ) {
        return -1;
    }

    for ( p = res; p != NULL; p = p->ai_next ) {
        int32_t rc = -1;

        fd = socket( p->ai_family, p->ai_socktype, p->ai_protocol );
        if ( fd < 0 ) {
            continue;
        }

        // 对描述符的选项操作
        if ( options( fd ) != 0 ) {
            close( fd );
            continue;
        }

        // 连接
        rc = connect( fd, p->ai_addr, p->ai_addrlen );
        // 出错的情况下, 忽略EINPROGRESS, EINTR
        if ( rc == -1
            && errno != EINTR
            && errno != EINPROGRESS ) {
            close( fd );
            continue;
        }

        // 连接成功后退出
        // TODO: FIX Linux TCP Self_Connecttion
        break;
    }

    freeaddrinfo( res );
    if ( p == NULL ) {
        return -2;
    }

    return fd;
}

int32_t udp_bind( const char * host, uint16_t port, int32_t ( *options )( int32_t ), struct sockaddr_storage * addr )
{
    int32_t fd = -1;
    char strport[6];
    struct addrinfo hints, *res, *p;

    bzero( &hints, sizeof( hints ) );
    snprintf( strport, sizeof( strport ), "%d", port );
    // 设置参数
    hints.ai_flags = AI_PASSIVE;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    // 获取地址信息
    if ( getaddrinfo( host, strport, &hints, &res ) != 0 ) {
        return -1;
    }

    for ( p = res; p != NULL; p = p->ai_next ) {
        fd = socket( p->ai_family, p->ai_socktype, p->ai_protocol );
        if ( fd < 0 ) {
            continue;
        }

        // 对描述符的选项操作
        if ( options( fd ) != 0 ) {
            close( fd );
            continue;
        }

        // bind
        if ( bind( fd, p->ai_addr, p->ai_addrlen ) == -1 ) {
            close( fd );
            continue;
        }

        // 绑定成功后退出循环
        memcpy( addr, p->ai_addr, p->ai_addrlen );
        break;
    }

    freeaddrinfo( res );
    if ( p == NULL ) {
        return -2;
    }

    return fd;
}

int32_t udp_connect( struct sockaddr_storage * localaddr, struct sockaddr_storage * remoteaddr, int32_t ( *options )( int32_t ) )
{
    int32_t newfd = socket(
        ( (struct sockaddr *)localaddr )->sa_family, SOCK_DGRAM, IPPROTO_UDP );
    if ( newfd <= 0 ) {
        return -1;
    }

    options( newfd );

    // 绑定
    if ( bind( newfd, (struct sockaddr *)localaddr, sizeof( struct sockaddr ) ) < 0 ) {
        close( newfd );
        return -2;
    }

    // 连接
    int32_t rc = connect( newfd,
        (struct sockaddr *)remoteaddr, sizeof( struct sockaddr ) );
    // 出错的情况下, 忽略EINPROGRESS, EINTR
    if ( rc == -1
        && errno != EINTR
        && errno != EINPROGRESS ) {
        close( newfd );
        return -3;
    }

    return newfd;
}

void convert_endpoint( struct sockaddr_storage * addr, int32_t type, const char * host, uint16_t port )
{
    switch ( type ) {
        case AF_INET : {
            struct sockaddr_in * addr4 = (struct sockaddr_in *)addr;
            addr4->sin_family = type;
            addr4->sin_addr.s_addr = inet_addr( host );
            addr4->sin_port = htons( port );
        } break;

        case AF_INET6 : {
            struct sockaddr_in6 * addr6 = (struct sockaddr_in6 *)addr;
            addr6->sin6_family = type;
            addr6->sin6_port = htons( port );
            inet_pton( AF_INET6, host, &( addr6->sin6_addr ) );
        } break;
    }
}

int32_t parse_endpoint( struct sockaddr_storage * addr, char * host, uint16_t * port )
{
    switch ( addr->ss_family ) {
        case AF_INET : {
            struct sockaddr_in * saddr = (struct sockaddr_in *)addr;
            *port = ntohs( saddr->sin_port );
            inet_ntop( saddr->sin_family, &saddr->sin_addr, host, INET_ADDRSTRLEN );
        } break;

        case AF_UNIX : {
            struct sockaddr_un * saddr = (struct sockaddr_un *)addr;
            *port = 0;
            inet_ntop( saddr->sun_family, &saddr->sun_path, host, INET_ADDRSTRLEN );
        } break;

        case AF_INET6 : {
            struct sockaddr_in6 * saddr = (struct sockaddr_in6 *)addr;
            *port = ntohs( ( (struct sockaddr_in6 *)saddr )->sin6_port );
            inet_ntop( saddr->sin6_family, &saddr->sin6_addr, host, INET6_ADDRSTRLEN );
        } break;
    }

    return addr->ss_family;
}

