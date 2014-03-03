
#include <time.h>
#include <stdint.h>
#include <stdlib.h>

#include "queue.h"

struct task
{
    int16_t type;
    int16_t utype;

    union
    {
        void * taskdata;
        char data[56];
    };
};

QUEUE_PADDING_HEAD(taskqueue, struct task);
QUEUE_GENERATE(taskqueue, struct task)

int main()
{
    uint32_t i = 0;
    struct taskqueue queue;

    srand( time(NULL) );
    QUEUE_INIT(taskqueue)( &queue, 8192 );

    for ( i = 0; i < 10000000; ++i )
    {
        struct task * task = (struct task *)malloc( sizeof(struct task) );
        QUEUE_PUSH( taskqueue )( &queue, task );

        if ( rand()%10 == 0 )
        {
            QUEUE_POP( taskqueue )( &queue, task );
        }
    }

    QUEUE_CLEAR( taskqueue )( &queue );

    return 0;
}
