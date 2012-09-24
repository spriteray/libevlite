
# -----------------------------------------------------------

CC		= gcc
CXX		= g++
CFLAGS	= -Wall -Iinclude/ -Isrc/ -Itest/ -ggdb -fPIC -O2 -D__EVENT_VERSION__=\"$(REALNAME)\"
LFLAGS	= -ggdb -lpthread 
SOFLAGS	= -shared -Wl

LIBNAME	= libevlite.so
SONAME	= $(LIBNAME).2
REALNAME= $(LIBNAME).2.2.2

OS		= $(shell uname)

#
# 利用git tag发布软件版本
#
#APPNAME=`git describe | awk -F- '{print $$1}'`
#VERSION=`git describe | awk -F- '{print $$2}'`
#MAJORVER=`git describe | awk -F- '{print $$2}' | awk -F. '{print $$1}'`
#
#LIBNAME=$(APPNAME).so
#SONAME=$(APPNAME).so.$(MAJORVER)
#REALNAME=$(APPNAME).so.$(VERSION)
#

OBJS 	= utils.o timer.o event.o \
			threads.o \
			message.o channel.o session.o \
			client.o server.o

ifeq ($(OS),Linux)
	LFLAGS += -lrt -L/usr/local/lib -ltcmalloc_minimal
#	LFLAGS += -lrt
	OBJS += epoll.o
else
	OBJS += kqueue.o
endif

# Release, open it
# CFLAGS += DNDEBUG

# -----------------------------------------------------------

install : all

all : $(REALNAME)

$(REALNAME) : $(OBJS)

	$(CC) $(SOFLAGS) $(LFLAGS) $^ -o $@
	rm -rf $(SONAME); ln -s $@ $(SONAME)
	rm -rf $(LIBNAME); ln -s $@ $(LIBNAME)

test : test_events test_addtimer echoserver-lock echoserver iothreads_dispatcher

test_events : test_events.o $(OBJS)

	$(CC) $(LFLAGS) $^ -o $@

test_addtimer : test_addtimer.o $(OBJS)

	$(CC) $(LFLAGS) $^ -o $@

echoserver-lock : accept-lock-echoserver.o $(OBJS)

	$(CC) $(LFLAGS) $^ -o $@

echoserver : echoserver.o $(OBJS)

	$(CXX) $(LFLAGS) $^ -o $@

echostress :

	$(CC) -I/usr/local/include -L/usr/local/lib -levent test/echostress.c -o $@

iothreads_dispatcher : test_iothreads.o $(OBJS)

	$(CC) $(LFLAGS) $^ -o $@

clean :

	rm -rf *.o
	rm -rf *.log
	rm -rf *.core
	rm -rf core.*
	rm -rf vgcore.*

	rm -rf $(SONAME)
	rm -rf $(LIBNAME)
	rm -rf $(REALNAME)

	rm -rf test_events event.fifo
	rm -rf test_addtimer echostress echoserver echoserver-lock iothreads_dispatcher
	
# --------------------------------------------------------
#
# gmake的规则
#
%.o : %.c
	$(CC) $(CFLAGS) -c $^ -o $@

%.o : %.cpp
	$(CXX) $(CFLAGS) -c $^ -o $@

VPATH = src:include:test
	
