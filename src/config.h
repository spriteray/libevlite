
#ifndef SRC_CONFIG_H
#define SRC_CONFIG_H

//
// OS Feature(s)
//

#include <sys/types.h>
#include <sys/socket.h>

//
// OS
// EVENT_OS_LINUX,
// EVENT_OS_BSD, EVENT_OS_MACOS
// EVENT_OS_WIN
// EVENT_OS_UNKNOWN
//
#if defined __linux__
    #define EVENT_OS_LINUX
    #include <linux/version.h>
#elif defined __FreeBSD__ || defined __OpenBSD__
    #define EVENT_OS_BSD
#elif defined __APPLE__ || defined __darwin__
    #define EVENT_OS_MACOS
#else
    #define EVENT_OS_UNKNOWN
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

// EVENT_HAVE_EVENTFD

#endif
