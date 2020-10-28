
#include <hiredis/hiredis.h>

#include "helper.h"

Slice RedisCommand::ping()
{
    char * buffer = NULL;
    ssize_t length = redisFormatCommand( &buffer, "PING" );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice RedisCommand::echo( const std::string & text )
{
    char * buffer = NULL;
    ssize_t length = redisFormatCommand(
            &buffer, "ECHO \"%s\"", text.c_str() );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice RedisCommand::auth( const std::string & password )
{
    char * buffer = NULL;
    ssize_t length = redisFormatCommand(
            &buffer, "AUTH %s", password.c_str() );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice RedisCommand::get( const std::string & key )
{
    char * buffer = NULL;
    ssize_t length = redisFormatCommand(
            &buffer, "GET %s", key.c_str() );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice RedisCommand::mget( const std::vector<std::string> & keys )
{
    size_t ncmds = keys.size() + 1;
    size_t argvlen[ ncmds ];
    const char * argv[ ncmds ];

    argv[0] = "MGET";
    argvlen[0] = 4;

    for ( size_t i = 0; i < keys.size(); ++i )
    {
        argv[i+1] = keys[i].c_str();
        argvlen[i+1] = keys[i].size();
    }

    char * buffer = NULL;
    ssize_t length = redisFormatCommandArgv(
            &buffer,
            keys.size()+1, argv, argvlen );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice RedisCommand::set( const std::string & key, const std::string & value )
{
    char * buffer = NULL;
    ssize_t length = redisFormatCommand(
            &buffer, "SET %s %b",
            key.c_str(), value.data(), value.size() );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice RedisCommand::incr( const std::string & key )
{
    char * buffer = NULL;
    ssize_t length = redisFormatCommand(
            &buffer, "INCR %s", key.c_str() );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice RedisCommand::incrby( const std::string & key, int32_t value )
{
    char * buffer = NULL;
    ssize_t length = redisFormatCommand(
            &buffer, "INCRBY %s %d", key.c_str(), value );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice RedisCommand::decr( const std::string & key )
{
    char * buffer = NULL;
    ssize_t length = redisFormatCommand(
            &buffer, "DECR %s", key.c_str() );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice RedisCommand::decrby( const std::string & key, int32_t value )
{
    char * buffer = NULL;
    ssize_t length = redisFormatCommand(
            &buffer, "DECRBY %s %d", key.c_str(), value );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice RedisCommand::lpush( const std::string & key, const std::vector<std::string> & values )
{
    size_t ncmds = values.size() + 2;

    size_t argvlen[ ncmds ];
    const char * argv[ ncmds ];

    argv[0] = "LPUSH";
    argvlen[0] = 5;
    argv[1] = key.c_str();
    argvlen[1] = key.size();

    for ( size_t i = 0; i < values.size(); ++i )
    {
        argv[i+2] = values[i].c_str();
        argvlen[i+2] = values[i].size();
    }

    char * buffer = NULL;
    ssize_t length = redisFormatCommandArgv(
            &buffer,
            ncmds, argv, argvlen );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice RedisCommand::rpush( const std::string & key, const std::vector<std::string> & values )
{
    size_t ncmds = values.size() + 2;

    size_t argvlen[ ncmds ];
    const char * argv[ ncmds ];

    argv[0] = "RPUSH";
    argvlen[0] = 5;
    argv[1] = key.c_str();
    argvlen[1] = key.size();

    for ( size_t i = 0; i < values.size(); ++i )
    {
        argv[i+2] = values[i].c_str();
        argvlen[i+2] = values[i].size();
    }

    char * buffer = NULL;
    ssize_t length = redisFormatCommandArgv(
            &buffer,
            ncmds, argv, argvlen );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice RedisCommand::lrange( const std::string & key, int32_t startidx, int32_t stopidx )
{
    char * buffer = NULL;
    ssize_t length = redisFormatCommand(
            &buffer, "LRANGE %s %d %d", key.c_str(), startidx, stopidx );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice RedisCommand::subscribe( const std::string & channel )
{
    char * buffer = NULL;
    ssize_t length = redisFormatCommand(
            &buffer, "SUBSCRIBE %s", channel.c_str() );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice RedisCommand::unsubscribe( const std::string & channel )
{
    char * buffer = NULL;
    ssize_t length = redisFormatCommand(
            &buffer, "UNSUBSCRIBE %s", channel.c_str() );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

Slice RedisCommand::publish( const std::string & channel, const std::string & message )
{
    char * buffer = NULL;
    ssize_t length = redisFormatCommand(
            &buffer, "PUBLISH %s \"%s\"", channel.c_str(), message.c_str() );

    if ( length < 0 )
    {
        return Slice();
    }

    return Slice( buffer, length );
}

template<> long long int ResultHelper::parse<long long int>( redisReply * reply )
{
    assert( reply->type == REDIS_REPLY_INTEGER );
    return reply->integer;
}

template<> std::string ResultHelper::parse<std::string>( redisReply * reply )
{
    assert( reply->type == REDIS_REPLY_STRING );
    return std::string( reply->str, reply->len );
}

template<> RedisStrList ResultHelper::parse<RedisStrList>( redisReply * reply )
{
    RedisStrList values;

    assert( reply->type == REDIS_REPLY_ARRAY );
    for ( size_t i = 0; i < reply->elements; ++i )
    {
        if ( reply->element[i]->type != REDIS_REPLY_STRING )
        {
            values.push_back( std::string() );
        }
        else
        {
            values.push_back( std::string(reply->element[i]->str, reply->element[i]->len) );
        }
    }

    return values;
}
