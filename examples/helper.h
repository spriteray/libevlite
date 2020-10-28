
#ifndef __SRC_REDIS_COMMAND_H__
#define __SRC_REDIS_COMMAND_H__

#include <string>
#include <cstdint>

#include "slice.h"

class RedisCommand
{
public :
    // PING
    static Slice ping();
    // ECHO
    static Slice echo( const std::string & text );
    // 验证命令
    static Slice auth( const std::string & password );
    // 获取数据
    static Slice get( const std::string & key );
    static Slice mget( const std::vector<std::string> & keys );
    // 更新命令
    static Slice set(
            const std::string & key, const std::string & value );
    // 自增
    static Slice incr( const std::string & key );
    static Slice incrby( const std::string & key, int32_t value );
    // 自减
    static Slice decr( const std::string & key );
    static Slice decrby( const std::string & key, int32_t value );
    // list
    static Slice lpush( const std::string & key, const std::vector<std::string> & values );
    static Slice rpush( const std::string & key, const std::vector<std::string> & values );
    static Slice lrange( const std::string & key, int32_t startidx, int32_t stopidx );
    // 订阅命令
    static Slice subscribe( const std::string & channel );
    // 取消订阅命令
    static Slice unsubscribe( const std::string & channel );
    // 发布命令
    static Slice publish( const std::string & channel, const std::string & message );
};

typedef std::vector<std::string> RedisStrList;

struct ResultHelper
{
    template<typename T> static T parse( redisReply * reply ) { return T(); }
};

#endif
