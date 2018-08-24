
#ifndef SRC_CONFIG_H
#define SRC_CONFIG_H

// EVENT_OS_XXXXXXX
#if defined __linux__
    #define EVENT_OS_LINUX
    #include <linux/version.h>
#elif defined __FreeBSD__ || defined __OpenBSD__
    #define EVENT_OS_BSD
#elif defined __APPLE__ || defined __darwin__
    #define EVENT_OS_MACOS
#endif

// EVENT_HAVE_REUSEPORT
#ifdef SO_REUSEPORT
    #ifdef EVENT_OS_LINUX
        #if LINUX_VERSION_CODE >= KERNEL_VERSION(4,6,0)
            #define EVENT_HAVE_REUSEPORT
        #endif
    #else
        #define EVENT_HAVE_REUSEPORT
    #endif
#endif

// EVENT_HAVE_EVENTFD

#endif
