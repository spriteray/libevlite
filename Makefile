
# ------------------------------------------------------------------------------
OS			= $(shell uname)

APP 		= libevlite
VERSION 	= 9.8.4
PREFIX		= /usr/local

# 主版本号
MAJORVER 	= $(firstword $(subst ., ,$(VERSION)))

# ------------------------------------------------------------------------------
# FreeBSD采用clang做为编译器
ifeq ($(OS),FreeBSD)
	CC 		= clang
	CXX 	= clang++
else
	CC		= gcc
	CXX		= g++
endif

#
# 编译选项
#
# USE_ATOMIC		- 使用原子操作
# USE_REUSESESSION	- 重用会话(提高效率)
#

# 默认选项
LFLAGS		= -ggdb -lpthread
CFLAGS		= -Wall -Wformat=0 -Iinclude/ -Isrc/ -Itest/ -ggdb -fPIC -O2 -DNDEBUG -D__EVENT_VERSION__=\"$(REALNAME)\" -DUSE_ATOMIC #-DUSE_REUSESESSION
CXXFLAGS	= -Wall -Wformat=0 -Iinclude/ -Isrc/ -Itest/ -ggdb -fPIC -O2 -DNDEBUG -D__EVENT_VERSION__=\"$(REALNAME)\" -DUSE_ATOMIC #-DUSE_REUSESESSION

# 动态库编译选项
ifeq ($(OS),Darwin)
	LIBNAME	= $(APP).dylib
	SONAME	= $(APP).$(MAJORVER).dylib
	REALNAME= $(APP).$(VERSION).dylib
	SOFLAGS	= -dynamiclib -Wl,-install_name,$(SONAME) -compatibility_version $(MAJORVER) -current_version $(VERSION)
else
	LIBNAME	= $(APP).so
	SONAME	= $(LIBNAME).$(MAJORVER)
	REALNAME= $(LIBNAME).$(VERSION)
	SOFLAGS	= -shared -Wl,-soname,$(SONAME)
endif

# Linux定制参数
ifeq ($(OS),Linux)
	LFLAGS 	= -ggdb -pthread -lrt
	CFLAGS 	+= -finline-limit=1000
	CXXFLAGS+= -finline-limit=1000
endif

# ------------------------------------------------------------------------------
# 安装目录lib, include
LIBPATH		= $(PREFIX)/lib
INCLUDEPATH	= $(PREFIX)/include

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

# ------------------------------------------------------------------------------
OBJS 	= utils.o \
		  	epoll.o kqueue.o timer.o \
			event.o \
			threads.o \
			message.o channel.o session.o \
			iolayer.o

# ------------------------------------------------------------------------------
install : all
	rm -rf $(INCLUDEPATH)/evlite
	cp -a include $(INCLUDEPATH)/evlite
	rm -rf $(LIBPATH)/$(REALNAME); cp $(REALNAME) $(LIBPATH)
	rm -rf $(LIBPATH)/$(SONAME); ln -s $(REALNAME) $(LIBPATH)/$(SONAME)
	rm -rf $(LIBPATH)/$(LIBNAME); ln -s $(REALNAME) $(LIBPATH)/$(LIBNAME)

all : $(REALNAME)

$(REALNAME) : $(OBJS)
	$(CC) $(SOFLAGS) $(LFLAGS) $^ -o $@
	rm -rf $(SONAME); ln -s $@ $(SONAME)
	rm -rf $(LIBNAME); ln -s $@ $(LIBNAME)

test : test_events test_addtimer test_queue test_sidlist echoserver-lock echoserver iothreads_dispatcher

test_events : test_events.o $(OBJS)
	$(CC) $^ -o $@ $(LFLAGS)

test_addtimer : test_addtimer.o $(OBJS)
	$(CC) $^ -o $@ $(LFLAGS)

test_queue : test_queue.o
	$(CC) $^ -o $@ $(LFLAGS)

test_sidlist : test_sidlist.o utils.o
	$(CC) $^ -o $@ $(LFLAGS)

echoserver-lock : accept-lock-echoserver.o $(OBJS)
	$(CC) $^ -o $@ $(LFLAGS)

echoclient : io.o echoclient.o $(OBJS)
	$(CXX) $^ -o $@ $(LFLAGS)

echoserver : io.o echoserver.o $(OBJS)
	$(CXX) $^ -o $@ $(LFLAGS)

raw_echoserver : raw_echoserver.o $(OBJS)
	$(CC) $^ -o $@ $(LFLAGS)

pingpong : pingpong.o $(OBJS)
	$(CC) $^ -o $@ $(LFLAGS)

echostress :
	$(CC) test/echostress.c -o $@ -I/usr/local/include -L/usr/local/lib -levent

iothreads_dispatcher : test_iothreads.o $(OBJS)
	$(CC) $(LFLAGS) $^ -o $@

chatroom : chatroom_server chatroom_client

chatroom_server: io.o chatroom_server.o $(OBJS)
	$(CXX) $^ -o $@ $(LFLAGS)

chatroom_client: io.o chatroom_client.o $(OBJS)
	$(CXX) $^ -o $@ $(LFLAGS)

redis_client : io.o redis.o $(OBJS)
	$(CXX) $^ -o $@ $(LFLAGS) -lhiredis

clean :
	rm -rf *.o
	rm -rf *.log
	rm -rf core
	rm -rf *.core
	rm -rf core.*
	rm -rf vgcore.*
	rm -rf $(SONAME)
	rm -rf $(LIBNAME)
	rm -rf $(REALNAME)
	rm -rf test_events event.fifo
	rm -rf test_queue test_sidlist
	rm -rf chatroom_client chatroom_server
	rm -rf test_addtimer echoclient echostress raw_echoserver echoserver pingpong echoserver-lock iothreads_dispatcher redis_client

# --------------------------------------------------------
#
# gmake的规则
#
%.o : %.c
	$(CC) $(CFLAGS) -c $^ -o $@

%.o : %.cpp
	$(CXX) $(CXXFLAGS) -c $^ -o $@

VPATH = src:include:test
