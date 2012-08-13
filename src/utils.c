
#include <sys/time.h>

int64_t mtime()
{
	int64_t now = -1;
	struct timeval tv;
	
	if ( gettimeofday( &tv, NULL ) == 0 )
	{
		now = tv.tv_sec * 1000ll + tv.tv_usec / 1000ll;
	}
	
	return now;
}

