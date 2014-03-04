
#include "utils.h"

int main()
{
    sid_t ids[3] = { 1234567, 8901234, 5678901 };
    struct sidlist * list = sidlist_create( 8 );

    sidlist_adds( list, &ids, 3 );
    sidlist_adds( list, &ids, 3 );

    sidlist_destroy( list );

    return 0;
}
