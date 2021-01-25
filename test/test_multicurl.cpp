
#include <stdio.h>
#include "utils.h"
#include "xcurl.h"

class TestRequest : public CurlRequest
{
public :
    TestRequest( const std::string & url )
        : CurlRequest( url ),
          m_Timestamp( milliseconds() )
    {}

    virtual ~TestRequest()
    {}

    virtual void onResponse()
    {
        printf( "onResponse(%lu, '%s', %lu)\n", milliseconds(), getResponse().c_str(), milliseconds()-m_Timestamp );
    }

    virtual void onError( const char * reason )
    {
        printf( "onError(%lu, '%s', %lu)\n", milliseconds(), reason, milliseconds()-m_Timestamp );
    }

private :
    int64_t     m_Timestamp;
};

void * iothread_main( void * arg )
{
    CurlAgent * agent = static_cast<CurlAgent *>(arg);

    while ( 1 )
    {
        agent->dispatch();
    }

    return nullptr;
}

int main()
{
    curl_global_init( CURL_GLOBAL_ALL );

    CurlAgent * agent = new CurlAgent();
    if ( agent == nullptr )
    {
        return -1;
    }

    if ( !agent->initialize() )
    {
        printf("CurlAgent::initialize() failed .\n");
        delete agent;
        return -2;
    }

    pthread_t tid;
    void * status = nullptr;
    pthread_create( &tid, nullptr, iothread_main, agent );

    for ( size_t i = 0; i < 10; ++i )
    {
        agent->perform(
                eHttpMethod_Get,
                new TestRequest("http://ipinfo.io"), 1000 );
        //agent->perform(
        //        eHttpMethod_Get,
        //        new TestRequest("http://172.21.161.138:3340/login"), 1000 );
        sleep(1);
    }

    pthread_join( tid, &status );

    agent->finalize();
    delete agent;

     curl_global_cleanup();

    return 0;
}
