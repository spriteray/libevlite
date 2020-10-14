
#ifndef SRC_CONFIG_H
#define SRC_CONFIG_H

//
// OS Feature(s)
//

//
// OS
// EVENT_OS_WIN32
// EVENT_OS_UNKNOWN
// EVENT_OS_LINUX,
// EVENT_OS_BSD, EVENT_OS_MACOS
//
#if defined _WIN32
    #define EVENT_OS_WIN32
#else
    // 除了windows平台外
    // 几乎都支持的头文件, 用于判断以下特性
    #include <sys/types.h>
    #include <sys/socket.h>
    //
    #if defined __linux__
        #define EVENT_OS_LINUX
        #include <features.h>
        #include <linux/version.h>
    #elif defined __FreeBSD__ || defined __OpenBSD__
        #define EVENT_OS_BSD
    #elif defined __APPLE__ || defined __darwin__
        #define EVENT_OS_MACOS
    #else
        #define EVENT_OS_UNKNOWN
    #endif
#endif

// EVENT_HAVE_REUSEPORT
#ifdef SO_REUSEPORT
    #ifdef EVENT_OS_LINUX
        // Faster SO_REUSEPORT for TCP ( since Linux 4.6.0 )
        // faster lookup of a target socket when choosing a socket from the group of sockets,
        // and also expose the ability to use a BPF program when selecting a socket from a reuseport group
        // https://kernelnewbies.org/Linux_4.6#Networking
        #if LINUX_VERSION_CODE >= KERNEL_VERSION(4,6,0)
            #define EVENT_HAVE_REUSEPORT
        #endif
    #else
        #define EVENT_HAVE_REUSEPORT
    #endif
#endif

// EVENT_USE_LOCALHOST
#if defined EVENT_OS_WIN32
    #define EVENT_USE_LOCALHOST
#elif defined EVENT_OS_MACOS
    #define EVENT_USE_LOCALHOST
#elif defined EVENT_OS_LINUX
    #if LINUX_VERSION_CODE > KERNEL_VERSION(4,4,0)
        #define EVENT_USE_LOCALHOST
    #endif
#endif

// EVENT_HAVE_EVENTFD
#if defined EVENT_OS_LINUX
    // eventfd() is available on Linux since kernel 2.6.22.
    // Working support is provided in glibc since version 2.8
    #if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,22)
        #if defined __GLIBC__ && __GLIBC_PREREQ(2,8)
            #define EVENT_HAVE_EVENTFD
            #include <sys/eventfd.h>
        #endif
    #endif
#endif

// EVENT_HAVE_EPOLLCREATE1
#if defined EVENT_OS_LINUX
    // epoll_create1() was added to the kernel in version 2.6.27.
    // Library support is provided in glibc starting with version 2.9.
    #if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,27)
        #if defined __GLIBC__ && __GLIBC_PREREQ(2,9)
            #define EVENT_HAVE_EPOLLCREATE1
        #endif
    #endif
#endif

#endif
