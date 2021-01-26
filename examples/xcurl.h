
#ifndef __SRC_HTTP_XCURL_H__
#define __SRC_HTTP_XCURL_H__

//
// https://curl.se/libcurl/c/hiperfifo.html
// http://www.voidcn.com/article/p-nzggwldk-bpx.html
//

#include <map>
#include <deque>
#include <string>
#include "event.h"
#include <pthread.h>
#include <curl/curl.h>

enum HttpMethod
{
    eHttpMethod_None    = 0,
    eHttpMethod_Get     = 1,
    eHttpMethod_Put     = 2,
    eHttpMethod_Post    = 3,
};

class CurlRequest
{
public :
    CurlRequest( const std::string & url );
    virtual ~CurlRequest();

    virtual void onResponse() = 0;
    virtual void onError( const char * reason ) = 0;

public :
    // 提交请求
    bool perform( HttpMethod method, int32_t timeout_ms = 0 );

    // 获取URL
    CURL * getHandler() const { return m_Curl; }
    const std::string & getUrl() const { return m_Url; }
    // 获取响应
    long getCode() const { return m_Code; }
    const std::string & getResponse() const { return m_Response; }

    // 设置HTTPBody
    void setBody( const std::string & body ) { m_HttpBody = body; }
    // 添加HTTP头
    void addHead( const std::string & key, const std::string & value );
    // 添加参数
    void addParam( const std::string & key, const std::string & value );

private :
    friend class CurlAgent;
    typedef std::map<std::string, std::string> ParameterList;

    // 编码
    std::string encode();
    // 准备
    void prepare( HttpMethod method );
    // 设置CURL选项
    void initCurlOptions( int32_t timeout_ms, CURLSH * share = nullptr );
    // 回调函数
    static int32_t writeCallback( void * buffer, size_t size, size_t nmemb, void * data );
    static int32_t setSocketOption( void * clientp, curl_socket_t curlfd, curlsocktype purpose );

protected :
    std::string             m_Url;
    long                    m_Code;
    std::string             m_Response;

private :
    CURL *                  m_Curl;
    struct curl_slist *     m_Headers;
    std::string             m_HttpBody;
    ParameterList           m_Paramters;
};

class CurlAgent
{
public :
    CurlAgent();
    ~CurlAgent();

public :
    bool initialize();
    void finalize();

    CURLM * handler() const { return m_Handler; }
    evsets_t evsets() const { return m_EventBase; }

    // 分发任务
    int32_t dispatch();
    // 分发任务
    bool perform( HttpMethod method,
            CurlRequest * request, int32_t timeout_ms = 0 );

private :
    friend struct CurlHelper;

    // 加锁/解锁
    void lock();
    void unlock();

    // 取消和定时
    void cancelTimer();
    void scheduleTimer( int32_t timeout_ms );
    // 处理请求
    int32_t processRequests( int32_t fd, int32_t action );

private :
    struct CurlTask
    {
        HttpMethod      method;
        int32_t         timeout;
        CurlRequest *   request;

        CurlTask()
            : method(eHttpMethod_None),
              timeout(0),
              request(nullptr)
        {}

        CurlTask( HttpMethod method_, int32_t timeout_, CurlRequest * request_ )
            : method(method_),
              timeout(timeout_),
              request(request_)
        {}
    };

    event_t                 m_Timer;        // 超时事件
    evsets_t                m_EventBase;
    CURLM *                 m_Handler;
    CURLSH *                m_ShareCache;   // 共享的缓存
    pthread_mutex_t         m_Lock;
    std::deque<CurlTask>    m_TaskQueue;
};

#endif
