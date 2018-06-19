#include <iostream>
#include <atomic>

#include <glog/logging.h>
#include <gflags/gflags.h>
#include <assert.h>
//evpp
#include <evpp/event_loop.h>
#include <evpp/event_loop_thread.h>
#include <evpp/event_loop_thread_pool.h>
#include <evpp/http/http_server.h>
#include <evpp/tcp_server.h>
#include <evpp/tcp_client.h>
#include <evpp/buffer.h>
#include <evpp/tcp_conn.h>
#include <evpp/httpc/conn_pool.h>
#include <evpp/httpc/request.h>
#include <evpp/httpc/conn.h>
#include <evpp/httpc/response.h>
#include <evpp/utility.h>   //Stringspilt
//etcd v3 cppClient
#include <etcd/Client.hpp>
//boost
#include <boost/format.hpp>
//linux ip address
#include <unistd.h>
#include <ifaddrs.h>
#include <string.h>
#include <arpa/inet.h>
//folly base cpp library

using namespace std;
using namespace std::placeholders;

DEFINE_string(etcdurl,"http://127.0.0.1:2379","The etcd service register center address."); //etcd服务注册中心地址
DEFINE_int32(timeout,100,"http request timeout(/ms).");//AsyncHTTP请求超时时间，毫秒
DEFINE_int32(listenPort,20000,"The listen port of consumer agent.");//consumer agent server本地监听端口
DEFINE_int32(threadnum,2,"The num of WorkerThreadPool size.");  //工作线程池线程数量
DEFINE_string(logs,"../logs","The logs dir.");    //日志输出目录

std::atomic_long service_count={0L};   //负载均衡，轮询
std::atomic_int service_index={0L};
/*
 * 服务注册器（服务注册与发现）
 * consumer-agent从etcd server查询提供服务的provider-agent endpoint列表
 * 2018/05/28
 */
//Endpoint结构体
struct Endpoint{
public:
    Endpoint(string ip, int p):ipAddress(ip),port(p){}
    string ipAddress;  //ip地址
    int port;       //端口

    string getUrl()    //endpoint url
    {
        ostringstream oss;
        oss<<ipAddress<<":"<<port;
        return oss.str();
    }
};
//etcd服务注册与发现
class EtcdRegistry{
public:
    //etcd服务注册器构造函数
    EtcdRegistry(string etcdurl) :etcd_(etcdurl),rootPath("dubbomesh"),fmt_provider("/%s/%s/%s:%d"), fmt_consumer("/%s/%s") {}

    /*
     * Provider端注册服务
     */
    void registerService(string serviceName, int port)
    {
        int RetryNum = 10; //注册服务重试次数
        //服务注册的key为： /dubbomesh/com.alibaba.dubbo.performance.demo.provider.IHelloService
        string hostIP = getDocker0IPAddr(); //docker0网卡ip地址
        string strKey = (fmt_provider%rootPath%serviceName%hostIP%port).str();
        LOG(WARNING)<<"register strKey:["<<strKey<<"] to etcd server ==============================";
        cout<<"register strKey:["<<strKey<<"] to etcd server =============================="<<endl;
        int retrycount = 0;
        while (retrycount<RetryNum)
        {
            retrycount++;
            etcd::Response resp = etcd_.add(strKey, "").get();  //目前只需要创建key，对应的value暂时不用，先留空
            int error_code = resp.error_code();
            if(error_code!=0)  //error_code == 0 正确
            {
                if(error_code==105)  //Key already exists
                {
                    LOG(WARNING)<<"[etcd register error]:Key already exits!";
                    cout<<"[etcd register error]:Key already exits!"<<endl;
                }
            }
            else
            {
                if(0 == etcd_.get(strKey).get().error_code())  // Key not found
                {
                    LOG(WARNING)<<"[etcd register success]: provider service register success!";
                    cout<<"[etcd register success]: provider service register success!"<<endl;
                    break;
                }
            }
        }

    }


    /*
     * consumer端 获取可以提供服务的Endpoint列表
     */
    vector<Endpoint> findService(string serviceName)
    {
        vector<Endpoint> result;  //获取结果
        string strKey = (fmt_consumer%rootPath%serviceName).str(); //Key = rootPath + serviceName
        cout<<"[etcd consumer get service list]:get Key="<<strKey<<endl;

        etcd::Response response = etcd_.ls(strKey).get();   //获取strKey的响应
        if(response.error_code()!=0)  //response error
        {
            LOG(ERROR)<<"[etcd get key error ]: error_code="<<response.error_code();
            cout<<"[etcd get key error ]: error_code="<<response.error_code()<<endl;
        }
        else
        {
            etcd::Keys keys = response.keys();  //获取以strKey为前缀的所有Keys
            for(int i=0;i<keys.size();i++)
            {
                vector<string> vec1,vec2;
                int p=0;
                evpp::StringSplit(keys[i],"/",0,vec1); //字符串分割"/"
                evpp::StringSplit(vec1[3],":",0,vec2); //字符串分割":
                ss.clear();
                ss<<vec2[1];
                ss>>p;
                Endpoint ep(vec2[0],p);
                result.push_back(ep);
            }
        }
        return result;
    }

    /*
     * 获取docker0网卡的ip地址
     */
    string getDocker0IPAddr()
    {
        string result;
        struct ifaddrs *ifap0=NULL, *ifap=NULL;
        void * tmpAddrPtr=NULL;
        getifaddrs(&ifap0);
        ifap=ifap0;
        bool flag= false ;
        while (ifap!=NULL)
        {
            if (ifap->ifa_addr->sa_family==AF_INET)
            {
                tmpAddrPtr=&((struct sockaddr_in *)ifap->ifa_addr)->sin_addr;
                char addressBuffer[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, tmpAddrPtr, addressBuffer, INET_ADDRSTRLEN);
                if(strcmp(addressBuffer,"127.0.0.1")!=0)
                {
                    if(strcmp(ifap->ifa_name,"eth0")==0)
                    {
                        result = addressBuffer;
                        flag = true;
                    }
                }
            }
            ifap=ifap->ifa_next;
        }
        if (ifap0) { freeifaddrs(ifap0); ifap0 = NULL; }
        if(flag) return result;
        else
        {
            LOG(ERROR)<<"[ETCD ERROR]: Not found docker0 !";
            cout<<"[ETCD ERROR]: Not found docker0 !"<<endl;
            return result;
        }
    }

private:
    etcd::Client etcd_;   //etcd 客户端
    boost::format fmt_provider;    //boost格式化字符串(provider端)
    boost::format fmt_consumer;    //boost格式化字符串(consumer端)
    string rootPath;      //etcd key rootPath
    stringstream ss;
};


/*
 * consumer-agent的主体类
 * consumer agent (with a async-http-server and a async-http-client)
 * edit by chijinxin
 * 2018/05/31
 */
class ConsumerAgent{
public:
    //Consumer-agent 构造函数
    ConsumerAgent(evpp::EventLoop* loop,
                  std::shared_ptr<evpp::EventLoopThreadPool> tpool,
                  const std::string& etcdurl,
                  const int listenport,
                  const int HttpServerThreadNum = 2,
                  const int timeout=1000
                  )
            :listenPort_(listenport),     //asyncHttpServer local listen port
             timeout_(timeout),           //request timeout
             loop_(loop),                 //base loop
             tpool_(tpool),               //thread pool
             asyncHttpServer_(HttpServerThreadNum),//async-http-server implement by tcp-server
             etcdclient_(etcdurl)         //etcd v3 client
             //connPool_(new evpp::httpc::ConnPool("127.0.0.1",30000,evpp::Duration(300)))
            {
                vector<Endpoint> endpoints = etcdclient_.findService("com.alibaba.dubbo.performance.demo.provider.IHelloService");
                if(endpoints.size()!=0)
                {
                    for_each(endpoints.begin(),endpoints.end(),
                             [](Endpoint x)
                             {
                                 LOG(WARNING)<<"[ConsumerAgent construct] endpoints:"<<x.getUrl();
                                 cout<<"[ConsumerAgent construct] endpoints:"<<x.getUrl()<<endl;
                             });

                    std::vector<std::shared_ptr<evpp::httpc::ConnPool>> servicePool;

                    for(int i=0;i<endpoints.size();i++)
                    {
                        servicePool.push_back(std::make_shared<evpp::httpc::ConnPool>
                                ( endpoints[i].ipAddress,
                                  endpoints[i].port,
                                  evpp::Duration(timeout_),
                                  400
                                ));
                    }
                    ServiceProviderCache_.insert(std::make_pair<string,std::vector<std::shared_ptr<evpp::httpc::ConnPool>>>
                            ("com.alibaba.dubbo.performance.demo.provider.IHelloService", std::move(servicePool) ));

                }
                else LOG(ERROR)<<"[ConsumerAgent construct] void endpoints!!!";


            }
    //consumer-agent 析构函数
    ~ConsumerAgent()
    {
        //client_.Disconnect();
        asyncHttpServer_.Stop();
        tpool_->Stop();
        loop_->Stop();
    }

public:
    /*
     * consumer Agent start
     */
    void Start()
    {
        //1. 启动AsyncHttpServer Consumer-Agent-Server
        asyncHttpServer_.SetThreadDispatchPolicy(evpp::ThreadDispatchPolicy::kIPAddressHashing); //设置asyncHttpServer ip分发策略
        asyncHttpServer_.RegisterDefaultHandler(std::bind(&ConsumerAgent::AsyncHttpServerDefaultHandler,this,_1,_2,_3));
        asyncHttpServer_.Init(listenPort_);
        asyncHttpServer_.Start();
        loop_->Run();
    }


/*回调函数/句柄处理函数*/
public:
    /*AsyncHttpServer DefaultHandler*/
    void AsyncHttpServerDefaultHandler(evpp::EventLoop* loop,
                                       const evpp::http::ContextPtr& ctx,
                                       const evpp::http::HTTPSendResponseCallback& cb)
    {
        //解析HTTP请求数据，获取content-type:application/x-www.form-url请求表单数据
        std::string cbBody = ctx->body().ToString();
        vector<string> FormDataSet;
        evpp::StringSplit(cbBody,"&",0,FormDataSet); //字符串分割（"&"）

        if(FormDataSet.size()!=4)
        {
            LOG(ERROR)<<"[consumer-agent HttpMessageProcess]:FormDataSet!==4";
            return;
        }

        string InterfaceName  = FormDataSet[0].substr(FormDataSet[0].find("=")+1,FormDataSet[0].npos);

        /*
         * 负载均衡
         * edit by chijinxin
         * 2018/06/08
         */
        auto search = ServiceProviderCache_.find(InterfaceName);
        if(search!=ServiceProviderCache_.end())   //在服务提供者缓存列表中查找服务，获得服务提供者列表
        {
            service_index = (service_count++)%(search->second.size());
            if( service_index < 0 || service_index >= search->second.size()) service_index=0;
            //LOG(WARNING)<<"[load balance] service_index="<<service_index;
            evpp::httpc::PostRequest* r = new evpp::httpc::PostRequest(search->second[service_index].get(), tpool_->GetNextLoop() ,"/", cbBody);
            /*response callback functor*/
            auto f = [cb,ctx,r](const std::shared_ptr<evpp::httpc::Response>& response)
            {
                cb(response->body().ToString());
                delete r;
            };
            r->Execute(f);
        }
    }
/*
 * content-type:application/x-www.form-url
 * application/x-www.form-url的C++实现
 * edit by chijinxin
 * 2018/05/30
 */
public:

    unsigned char ToHex(unsigned char x)
    {
        return  x > 9 ? x + 55 : x + 48;
    }

    unsigned char FromHex(unsigned char x)
    {
        unsigned char y;
        if (x >= 'A' && x <= 'Z') y = x - 'A' + 10;
        else if (x >= 'a' && x <= 'z') y = x - 'a' + 10;
        else if (x >= '0' && x <= '9') y = x - '0';
        else assert(0);
        return y;
    }
    //application/x-www.form-url编码
    std::string UrlEncode(const std::string& str)
    {
        std::string strTemp = "";
        size_t length = str.length();
        for (size_t i = 0; i < length; i++)
        {
            if (isalnum((unsigned char)str[i]) ||
                (str[i] == '-') ||
                (str[i] == '_') ||
                (str[i] == '.') ||
                (str[i] == '~'))
                strTemp += str[i];
            else if (str[i] == ' ')
                strTemp += "+";
            else
            {
                strTemp += '%';
                strTemp += ToHex((unsigned char)str[i] >> 4);
                strTemp += ToHex((unsigned char)str[i] % 16);
            }
        }
        return strTemp;
    }
    //application/x-www.form-url解码
    std::string UrlDecode(const std::string& str)
    {
        std::string strTemp = "";
        size_t length = str.length();
        for (size_t i = 0; i < length; i++)
        {
            if (str[i] == '+') strTemp += ' ';
            else if (str[i] == '%')
            {
                assert(i + 2 < length);
                unsigned char high = FromHex((unsigned char)str[++i]);
                unsigned char low = FromHex((unsigned char)str[++i]);
                strTemp += high*16 + low;
            }
            else strTemp += str[i];
        }
        return strTemp;
    }

/*consumer-agent数据成员*/
private:
    EtcdRegistry etcdclient_;            //etcd客户端，服务注册与发现
    int listenPort_;                     //AsyncHttpServer listen port
    evpp::http::Server asyncHttpServer_; //异步http服务器，监听本地20000端口，接收consumer转发来的请求消息
    int timeout_;   //http请求超时时间
    //std::shared_ptr<evpp::httpc::ConnPool> connPool_;  //http请求池

    std::unordered_map<std::string,std::vector<std::shared_ptr<evpp::httpc::ConnPool>>> ServiceProviderCache_;  //服务提供者缓存

    std::shared_ptr<evpp::EventLoop> loop_; //base loop
    std::shared_ptr<evpp::EventLoopThreadPool> tpool_; //工作线程池

    const static size_t kHeaderLen = 16;    //Dubbo协议头部长度为16个字节
};



int main(int argc, char *argv[])
{
    cout<<"Consumer-Agent !!!"<<endl;
    google::ParseCommandLineFlags(&argc,&argv,true);
    //glog日志
    FLAGS_log_dir=FLAGS_logs; //日志输出目录
    FLAGS_logtostderr=0;      //设置日志标准错误输出
    google::InitGoogleLogging(argv[0]); //初始化Lgging

    /*
     * 初始化consumer agent
     */
    evpp::EventLoop loop;  //base io loop
    std::shared_ptr<evpp::EventLoopThreadPool> tpool( new evpp::EventLoopThreadPool(&loop, FLAGS_threadnum) );  //初始化工作线程池
    tpool->Start(true);  //启动线程池
    ConsumerAgent consumerAgent(&loop, tpool,FLAGS_etcdurl,FLAGS_listenPort,FLAGS_threadnum,FLAGS_timeout);

    /*
     * 启动provider-agent
     */
    consumerAgent.Start();  //启动provider-agent

    return 0;
}
