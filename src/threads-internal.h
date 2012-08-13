
#ifndef THREADS_INTERNAL_H
#define THREADS_INTERNAL_H

#include "threads.h"

// Õ£÷π√¸¡Ó
#define X_THREAD_CMD		0xff

struct iothread
{
	uint8_t 	index;
	
	evsets_t 	sets;
	void *		parent;

	struct msgqueue * queue;
};


struct iothreads
{
	
	

};







#endif

