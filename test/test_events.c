

#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/queue.h>
#include <unistd.h>
#include <sys/time.h>

#include "event.h"

int32_t done;

void signal_handler( int32_t signo )
{
    done = 1;
}

void fifo_read( int32_t fd, int16_t ev, void * arg )
{
    char buf[255];
    int32_t len = 0;
    int32_t rc = 0;

    event_t evfifo = (event_t)arg;
    evsets_t evsets = event_get_sets( evfifo );

    rc = evsets_add( evsets, evfifo, -1 );

    printf("fifo_read called with fd: %d, event: %d, arg: %p\n", fd, ev, arg);

    len = read(fd, buf, sizeof(buf)-1 );
    if ( len == -1 )
    {
        printf("read() error .\n");
        return;
    }
    else if ( len == 0 )
    {
        printf("this Connection is CLOSED .\n");
        return;
    }

    buf[len] = '\0';

    printf("Read: %s\n", buf);
}

int32_t main()
{
    int32_t socketfd;

    event_t evfifo = NULL;
    evsets_t evsets = NULL;

    struct stat st;
    const char * fifo = "event.fifo";

    done = 0;

    printf("VERSION : %s\n", evsets_get_version() );

    signal( SIGINT, signal_handler );
    signal( SIGTERM, signal_handler );

    if ( lstat( fifo, &st ) == 0 )
    {
        if ( (st.st_mode & S_IFMT) == S_IFREG )
        {
            errno = EEXIST;
            printf("lstat() error .\n");

            exit(1);
        }
    }

    unlink( fifo );
    if ( mkfifo( fifo, 0600 ) == -1 )
    {
        printf("mkfifo() error .\n");
        exit(1);
    }

#ifdef __linux__
    socketfd = open( fifo, O_RDWR|O_NONBLOCK, 0 );
#else
    socketfd = open( fifo, O_RDONLY|O_NONBLOCK, 0 );
#endif

    if ( socketfd == -1 )
    {
        printf("open() error .\n");
        exit(1);
    }

    fprintf( stderr, "Write data to %s\n", fifo );

    evsets = evsets_create( 8 );
    if ( evsets == NULL )
    {
        printf("evsets_create() error .\n");
        exit(1);
    }

    evfifo = event_create();
    if ( evfifo == NULL )
    {
        printf("event_create() error .\n");
        exit(1);
    }

    event_set( evfifo, socketfd, EV_READ );
    event_set_callback( evfifo, fifo_read, evfifo );
    printf("event_set() succeed, %p .\n", evfifo );

    evsets_add( evsets, evfifo, -1 );
    printf("evsets_add() succeed .\n");

    while( !done )
    {
        evsets_dispatch( evsets );
    }

    evsets_destroy( evsets );
    event_destroy( evfifo );

    return 0;
}
