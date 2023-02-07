
#include <assert.h>
#include "xcurl.h"
#include "utils.h"

CurlRequest::CurlRequest( const std::string & url )
    : m_Url(url),
      m_Code(-1),
      m_Curl(nullptr),
      m_Headers(nullptr)
{
    m_Curl = curl_easy_init();
}

CurlRequest::~CurlRequest()
{
    if ( m_Curl != nullptr )
    {
        curl_easy_cleanup( m_Curl );
        m_Curl = nullptr;
    }

    if ( m_Headers != nullptr )
    {
        curl_slist_free_all( m_Headers );
        m_Headers = nullptr;
    }
}

bool CurlRequest::perform( HttpMethod method, int32_t timeout_ms )
{
    CURLcode result;

    // 准备
    prepare( method );
    // 设置选项
    initCurlOptions( timeout_ms );

    result = curl_easy_perform( m_Curl );
    curl_easy_getinfo( m_Curl, CURLINFO_RESPONSE_CODE, &m_Code );

    if ( result == CURLE_OK )
    {
        onResponse();
    }
    else
    {
        onError( curl_easy_strerror(result) );
    }

    return result == CURLE_OK;
}

void CurlRequest::addHead( const std::string & key, const std::string & value )
{
    std::string head;

    head += key;
    head += ":";
    head += value;

    m_Headers = curl_slist_append( m_Headers, head.c_str() );
}

void CurlRequest::addParam( const std::string & key, const std::string & value )
{
    m_Paramters[ key ] = value;
}

std::string CurlRequest::encode()
{
    std::string data;

    for ( auto it = m_Paramters.begin(); it != m_Paramters.end(); ++it )
    {
        if ( it != m_Paramters.begin() )
        {
            data += "&";
        }

        std::string value;
        // Url::encode( it->second, value );
        data += it->first;
        data +=  "=";
        data += value;
    }

    return data;
}

void CurlRequest::prepare( HttpMethod method )
{
    std::string fullurl = m_Url;
    std::string params = encode();

    switch ( method )
    {
        case eHttpMethod_Get :
            {
                if ( !m_Paramters.empty() )
                {
                    fullurl += "?";
                    fullurl += params;
                }
            }
            break;

        case eHttpMethod_Put :
            {
                if ( !m_Paramters.empty() )
                {
                    fullurl += "?";
                    fullurl += params;
                }

                // 设置PUT参数
                curl_easy_setopt( m_Curl, CURLOPT_CUSTOMREQUEST, "PUT" );
                curl_easy_setopt( m_Curl, CURLOPT_POSTFIELDS, m_HttpBody.c_str() );
                curl_easy_setopt( m_Curl, CURLOPT_POSTFIELDSIZE, m_HttpBody.size() );
            }
            break;

        case eHttpMethod_Post :
            {
                curl_easy_setopt( m_Curl, CURLOPT_POST, 1 );

                if ( m_HttpBody.empty() )
                {
                    // 没有额外提交的数据时, 优先使用POSTFIELDS
                    curl_easy_setopt( m_Curl, CURLOPT_POSTFIELDS, params.c_str() );
                    curl_easy_setopt( m_Curl, CURLOPT_POSTFIELDSIZE, params.size() );
                }
                else
                {
                    if ( !m_Paramters.empty() )
                    {
                        // 拼接完整的URL
                        fullurl += "?";
                        fullurl += params;
                    }

                    // 额外数据通过POSTFIELDS
                    curl_easy_setopt( m_Curl, CURLOPT_POSTFIELDS, m_HttpBody.c_str() );
                    curl_easy_setopt( m_Curl, CURLOPT_POSTFIELDSIZE, m_HttpBody.size() );
                }
            }
            break;

        default :
            break;
    }
}

void CurlRequest::initCurlOptions( int32_t timeout_ms, CURLSH * share )
{
    curl_easy_setopt( m_Curl, CURLOPT_URL, m_Url.c_str() );
    curl_easy_setopt( m_Curl, CURLOPT_WRITEDATA, &m_Response );
    curl_easy_setopt( m_Curl, CURLOPT_WRITEFUNCTION, writeCallback );
    curl_easy_setopt( m_Curl, CURLOPT_NOSIGNAL, 1 );
    curl_easy_setopt( m_Curl, CURLOPT_SSL_VERIFYPEER, 0 );
    curl_easy_setopt( m_Curl, CURLOPT_SSL_VERIFYHOST, 0 );
    curl_easy_setopt( m_Curl, CURLOPT_PRIVATE, this );
    // curl_easy_setopt( m_Curl, CURLOPT_VERBOSE, 1 );
    curl_easy_setopt( m_Curl, CURLOPT_SOCKOPTFUNCTION, setSocketOption );

    if ( m_Headers != nullptr )
    {
        curl_easy_setopt( m_Curl, CURLOPT_HTTPHEADER, m_Headers );
    }

    // 共享数据
    if ( share != nullptr )
    {
        curl_easy_setopt( m_Curl, CURLOPT_SHARE, share );
    }

    // 超时
    if ( timeout_ms > 0 )
    {
        curl_easy_setopt( m_Curl, CURLOPT_TIMEOUT_MS, timeout_ms );
        curl_easy_setopt( m_Curl, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms );
    }
}

int32_t CurlRequest::writeCallback( void * buffer, size_t size, size_t nmemb, void * data )
{
    size_t totalsize = size * nmemb;
    std::string * response = (std::string *)data;

    if ( response != nullptr )
    {
        response->append( (char *)buffer, totalsize );
    }

    return totalsize;
}

int32_t CurlRequest::setSocketOption( void * clientp, curl_socket_t curlfd, curlsocktype purpose )
{
    // 设置LINGER选项
    struct linger ling;
    ling.l_onoff = 1;
    ling.l_linger = 0;
    setsockopt( curlfd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling) );

    return 0;
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

struct CurlHelper
{
    static void onEvent( int32_t fd, int16_t ev, void * arg );
    static void onTimer( int32_t fd, int16_t ev, void * arg );

    static int timerCallback( CURLM * handler, long timeout_ms, void * param );
    static int socketCallback( CURL * handler, curl_socket_t s, int what, void * cbp, void * sockp );

    static void shareUnlock( CURL * handler, curl_lock_data data, void * userptr );
    static void shareLock( CURL * handler, curl_lock_data data, curl_lock_access access, void * userptr );
};

CurlAgent::CurlAgent()
    : m_Timer( nullptr ),
      m_EventBase( nullptr ),
      m_Handler( nullptr ),
      m_ShareCache( nullptr )
{
    pthread_mutex_init( &m_Lock, nullptr );
}

CurlAgent::~CurlAgent()
{
    pthread_mutex_destroy( &m_Lock );
}

bool CurlAgent::initialize()
{
    m_Timer = event_create();
    if ( m_Timer == nullptr )
    {
        return false;
    }

    m_EventBase = evsets_create( 8 );
    if ( m_EventBase == nullptr )
    {
        return false;
    }

    m_Handler = curl_multi_init();
    if ( m_Handler == nullptr )
    {
        return false;
    }

    m_ShareCache = curl_share_init();
    if ( m_ShareCache == nullptr )
    {
        return false;
    }

    event_set( m_Timer, -1, 0 );
    event_set_callback( m_Timer, CurlHelper::onTimer, this );
    evsets_add( m_EventBase, m_Timer, 0 );

    //curl_share_setopt( m_ShareCache, CURLSHOPT_USERDATA, this );
    //curl_share_setopt( m_ShareCache, CURLSHOPT_LOCKFUNC, CurlHelper::shareLock );
    //curl_share_setopt( m_ShareCache, CURLSHOPT_UNLOCKFUNC, CurlHelper::shareUnlock );
    curl_share_setopt( m_ShareCache, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS );
    curl_share_setopt( m_ShareCache, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE );
    curl_share_setopt( m_ShareCache, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT );

    curl_multi_setopt( m_Handler, CURLMOPT_TIMERDATA, this );
    curl_multi_setopt( m_Handler, CURLMOPT_TIMERFUNCTION, CurlHelper::timerCallback );
    curl_multi_setopt( m_Handler, CURLMOPT_SOCKETDATA, this );
    curl_multi_setopt( m_Handler, CURLMOPT_SOCKETFUNCTION, CurlHelper::socketCallback );

    return true;
}

void CurlAgent::finalize()
{
    if ( m_ShareCache != nullptr )
    {
        curl_share_cleanup( m_ShareCache );
        m_ShareCache = nullptr;
    }
    if ( m_Handler != nullptr )
    {
        curl_multi_cleanup( m_Handler );
        m_Handler = nullptr;
    }

    if ( m_Timer != nullptr )
    {
        evsets_del( m_EventBase, m_Timer );
        event_destroy( m_Timer );
        m_Timer = nullptr;
    }
    if ( m_EventBase != nullptr )
    {
        evsets_destroy( m_EventBase );
        m_EventBase = nullptr;
    }

    for ( const auto & t : m_TaskQueue )
    {
        delete t.request;
    }
    m_TaskQueue.clear();
}

int32_t CurlAgent::dispatch()
{
    std::deque<CurlTask> q;

    lock();
    std::swap( m_TaskQueue, q );
    unlock();

    for ( const auto & t : q )
    {
        int32_t timeout = t.timeout;
        HttpMethod method = t.method;
        CurlRequest * curlreq = t.request;

        curlreq->prepare( method );
        curlreq->initCurlOptions( timeout, m_ShareCache );
        curl_multi_add_handle( m_Handler, curlreq->getHandler() );
    }

    evsets_dispatch( m_EventBase );

    return q.size();
}

bool CurlAgent::perform( HttpMethod method, CurlRequest * request, int32_t timeout_ms )
{
    lock();
    m_TaskQueue.push_back(
            CurlTask(method, timeout_ms, request) );
    unlock();
    return true;
}

int32_t CurlAgent::processRequests( int32_t fd, int32_t action )
{
    CURLMsg * msg = nullptr;

    int32_t still_running = 1;
    int32_t nprocess = 0, nleft = 0;

    curl_multi_socket_action( m_Handler, fd, action, &still_running );

    while ( ( msg = curl_multi_info_read(m_Handler, &nleft) ) != nullptr )
    {
        CurlRequest * request = nullptr;

        // 请求不完整
        if ( msg->msg != CURLMSG_DONE )
        {
            continue;
        }

        CURL * curl = msg->easy_handle;
        CURLcode code = msg->data.result;

        // 移除请求
        ++nprocess;
        curl_multi_remove_handle( m_Handler, curl );

        // 获取请求信息
        curl_easy_getinfo( curl, CURLINFO_PRIVATE, &request );
        curl_easy_getinfo( curl, CURLINFO_RESPONSE_CODE, &(request->m_Code) );

        // 回调逻辑层
        if ( code == CURLE_OK )
        {
            request->onResponse();
        }
        else
        {
            request->onError( curl_easy_strerror(code) );
        }

        // 删除请求
        delete request;
    }

    if ( still_running <= 0 ) evsets_del( m_EventBase, m_Timer );

    return nprocess;
}

void CurlAgent::lock()
{
    pthread_mutex_lock( &m_Lock );
}

void CurlAgent::unlock()
{
    pthread_mutex_unlock( &m_Lock );
}

void CurlAgent::cancelTimer()
{
    evsets_del( m_EventBase, m_Timer );
}

void CurlAgent::scheduleTimer( int32_t timeout_ms )
{
    evsets_add( m_EventBase, m_Timer, timeout_ms );
}

void CurlHelper::onEvent( int32_t fd, int16_t ev, void * arg )
{
    int32_t action =
        ((ev & EV_READ) ? CURL_CSELECT_IN : 0)
        | ((ev & EV_WRITE) ? CURL_CSELECT_OUT : 0);
    static_cast<CurlAgent*>(arg)->processRequests( fd, action );
}

void CurlHelper::onTimer( int32_t fd, int16_t ev, void * arg )
{
    (void)fd;
    (void)ev;
    static_cast<CurlAgent *>(arg)->processRequests( CURL_SOCKET_TIMEOUT, 0 );
}

int CurlHelper::timerCallback( CURLM * handler, long timeout_ms, void * param )
{
    CurlAgent * agent = static_cast<CurlAgent *>(param);
    printf( "CurlHelper::timerCallback(Timeout:%ld) : Timestamp:%lu .\n", timeout_ms, milliseconds() );

    (void)handler;

    if ( timeout_ms == -1 )
    {
        agent->cancelTimer();
    }
    else
    {
        agent->scheduleTimer( timeout_ms );
    }

    return 0;
}

int CurlHelper::socketCallback( CURL * handler, curl_socket_t s, int what, void * cbp, void * sockp )
{
    event_t event = static_cast<event_t>(sockp);
    CurlAgent * agent = static_cast<CurlAgent *>(cbp);

    const char *whatstr[]={ "none", "IN", "OUT", "INOUT", "REMOVE" };
    printf( "CurlHelper::socketCallback(handler:%p, fd:%d) : Timestamp:%lu, Action:%s .\n",
            handler, s, milliseconds(), whatstr[what] );

    if ( what == CURL_POLL_REMOVE )
    {
        evsets_del( agent->evsets(), event );
        event_destroy( event );
    }
    else
    {
        int32_t ev = EV_PERSIST
            | ((what & CURL_POLL_IN) ? EV_READ : 0)
            | ((what & CURL_POLL_OUT) ? EV_WRITE : 0);

        if ( event != nullptr )
        {
            evsets_del( agent->evsets(), event );
        }
        else
        {
            event = event_create();
            assert( event != nullptr );
            curl_multi_assign( agent->handler(), s, event );
        }

        event_set( event, s, ev );
        event_set_callback( event, CurlHelper::onEvent, agent );
        evsets_add( agent->evsets(), event, -1 );
    }

    return 0;
}

void CurlHelper::shareUnlock( CURL *handle, curl_lock_data data, void * userptr )
{
    static_cast<CurlAgent *>(userptr)->unlock();
}

void CurlHelper::shareLock( CURL *handle, curl_lock_data data, curl_lock_access access, void * userptr )
{
    static_cast<CurlAgent *>(userptr)->lock();
}
