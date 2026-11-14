#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include "online_internet_relay.h"
#include "online_discord.h"
#include "online_invite.h"
#include "online_voice_transport.h"
#include "demo_support_json.h"
#include <rex/logging.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace em2::net {
namespace {
using Bytes=std::vector<uint8_t>;
using namespace std::chrono_literals;
void Need(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
std::wstring Wide(const std::string& text){
    Need(std::all_of(text.begin(),text.end(),[](unsigned char c){return c>=32 && c<127;}),"Invalid relay request text");
    return {text.begin(),text.end()};
}
struct Http {
    HINTERNET value=nullptr;
    ~Http(){if(value)WinHttpCloseHandle(value);}
    explicit Http(HINTERNET handle=nullptr):value(handle){}
    Http(const Http&)=delete;
};
bool Nonblocking(SOCKET socket){u_long yes=1;BOOL delay=TRUE;return ioctlsocket(socket,FIONBIO,&yes)==0 &&
    setsockopt(socket,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&delay),sizeof(delay))==0;}
void Close(SOCKET& socket){if(socket!=INVALID_SOCKET)closesocket(socket);socket=INVALID_SOCKET;}
std::string Quote(const std::string& text){
    Need(std::all_of(text.begin(),text.end(),[](unsigned char c){return c>=32 && c<127 && c!='"' && c!='\\';}),"Invalid relay metadata");
    return "\""+text+"\"";
}
struct MediaRoutes {
    std::mutex mutex;
    std::array<unsigned,256> peers{};
};
// The gameplay worker owns these jobs, while the simulation owns only a proxy
// to bounded queues. Destroying a voice transport never joins HTTP/socket work.
class MediaWorker {
    using Clock=std::chrono::steady_clock;
    struct Queued {uint8_t channel=0;VoiceDatagram datagram;Clock::time_point at;};
    struct State {
        std::mutex mutex;
        std::deque<Queued> outgoing,incoming;
        std::atomic<bool> stop=false,ready=false,finished=false;
        bool host=false;
        std::shared_ptr<MediaRoutes> routes;
    };
    class Proxy final:public VoiceDatagramChannel {
        std::shared_ptr<State> state;
    public:
        explicit Proxy(std::shared_ptr<State> value):state(std::move(value)){}
        ~Proxy(){state->stop=true;}
        bool ready()const override{return state->ready && !state->stop;}
        bool Send(unsigned destination,std::span<const uint8_t> bytes)override{
            if(!ready() || bytes.empty() || bytes.size()>kVoiceDatagramBytes)return false;
            unsigned channel=0;
            if(state->host){
                if(destination<2 || destination>16)return false;
                std::lock_guard lock(state->routes->mutex);
                for(unsigned i=1;i<256;++i)if(state->routes->peers[i]==destination){channel=i;break;}
                if(!channel)return false;
            }else if(destination!=1)return false;
            Queued item;item.channel=uint8_t(channel);item.at=Clock::now();item.datagram.size=uint16_t(bytes.size());
            std::copy(bytes.begin(),bytes.end(),item.datagram.data.begin());
            std::lock_guard lock(state->mutex);
            if(state->stop || !state->ready)return false;
            while(state->outgoing.size()>=128)state->outgoing.pop_front();
            state->outgoing.push_back(std::move(item));return true;
        }
        std::optional<VoiceDatagram> Receive()override{
            const auto now=Clock::now();std::lock_guard lock(state->mutex);
            while(!state->incoming.empty()){
                auto item=std::move(state->incoming.front());state->incoming.pop_front();
                if(now-item.at>150ms)continue;
                if(state->host){
                    std::lock_guard route_lock(state->routes->mutex);
                    if(state->routes->peers[item.channel]!=item.datagram.sender)continue;
                }
                return item.datagram;
            }
            return {};
        }
    };
    std::shared_ptr<State> state=std::make_shared<State>();
    std::thread thread;
    std::weak_ptr<VoiceDatagramChannel> proxy;
    void Pump(HINTERNET socket){
        std::atomic<bool> disconnected=false;
        std::thread receiver([&]{
            try{
                std::array<uint8_t,kVoiceDatagramBytes+2> bytes{};Bytes frame;
                while(!state->stop && !disconnected){
                    DWORD count=0;WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
                    Need(WinHttpWebSocketReceive(socket,bytes.data(),DWORD(bytes.size()),&count,&type)==NO_ERROR,"Voice receive failed");
                    if(type==WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE){Need(count==4 && std::equal(bytes.begin(),bytes.begin()+4,"pong"),"Invalid voice heartbeat");continue;}
                    Need(type==WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE || type==WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,"Voice closed");
                    Need(frame.size()+count<=bytes.size(),"Oversized voice frame");frame.insert(frame.end(),bytes.begin(),bytes.begin()+count);
                    if(type==WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE)continue;
                    Need(frame.size()>2 && frame[0]==2 && (state->host?frame[1]!=0:frame[1]==0),"Invalid voice route");
                    Queued item;item.channel=frame[1];item.at=Clock::now();item.datagram.size=uint16_t(frame.size()-2);
                    unsigned sender=1;
                    if(state->host){std::lock_guard lock(state->routes->mutex);sender=state->routes->peers[item.channel];}
                    if(sender){
                        item.datagram.sender=sender;std::copy(frame.begin()+2,frame.end(),item.datagram.data.begin());
                        std::lock_guard lock(state->mutex);while(state->incoming.size()>=256)state->incoming.pop_front();state->incoming.push_back(std::move(item));
                    }
                    frame.clear();
                }
            }catch(...){disconnected=true;}
        });
        state->ready=true;
        try{
            auto heartbeat=Clock::now();
            while(!state->stop && !disconnected){
                std::deque<Queued> output;
                {std::lock_guard lock(state->mutex);output.swap(state->outgoing);}
                for(const auto& item:output){
                    if(Clock::now()-item.at>150ms)continue;
                    if(state->host){std::lock_guard lock(state->routes->mutex);if(!state->routes->peers[item.channel])continue;}
                    Bytes frame{2,item.channel};frame.insert(frame.end(),item.datagram.data.begin(),item.datagram.data.begin()+item.datagram.size);
                    Need(WinHttpWebSocketSend(socket,WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,frame.data(),DWORD(frame.size()))==NO_ERROR,"Voice send failed");
                }
                if(Clock::now()-heartbeat>=5s){Need(WinHttpWebSocketSend(socket,WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,const_cast<char*>("ping"),4)==NO_ERROR,"Voice heartbeat failed");heartbeat=Clock::now();}
                std::this_thread::sleep_for(2ms);
            }
        }catch(...){disconnected=true;}
        state->ready=false;
        // Close only from the sender, after all its synchronous sends finish.
        DWORD timeout=250;WinHttpSetOption(socket,WINHTTP_OPTION_WEB_SOCKET_CLOSE_TIMEOUT,&timeout,sizeof(timeout));
        WinHttpWebSocketClose(socket,WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,nullptr,0);receiver.join();
        std::lock_guard lock(state->mutex);state->outgoing.clear();state->incoming.clear();
    }
public:
    MediaWorker(bool host,std::shared_ptr<MediaRoutes> routes,std::wstring hostname,INTERNET_PORT port,
                std::string code,std::string version,std::string credential,std::shared_ptr<VoiceDatagramChannel>& initial){
        state->host=host;state->routes=std::move(routes);
        initial=std::make_shared<Proxy>(state);proxy=initial;
        thread=std::thread([this,hostname=std::move(hostname),port,code=std::move(code),version=std::move(version),credential=std::move(credential)]()mutable{
            struct Completion{State& state;std::string& secret;~Completion(){SecureZeroMemory(secret.data(),secret.size());state.ready=false;state.finished=true;}}completion{*state,credential};
            unsigned failures=0;
            while(!state->stop){
                try{
                    Http session(WinHttpOpen(L"Beetleworx-Voice/1",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0));Need(session.value,"Voice HTTP initialization failed");
                    Need(WinHttpSetTimeouts(session.value,5000,5000,5000,5000),"Voice timeout configuration failed");
                    Http connection(WinHttpConnect(session.value,hostname.c_str(),port,0));Need(connection.value,"Voice service unavailable");
                    auto path=Wide("/v1/rooms/"+code+"/voice");
                    Http request(WinHttpOpenRequest(connection.value,L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE));Need(request.value,"Voice request failed");
                    DWORD redirects=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
                    Need(WinHttpSetOption(request.value,WINHTTP_OPTION_REDIRECT_POLICY,&redirects,sizeof(redirects)) && WinHttpSetOption(request.value,WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,nullptr,0),"Voice upgrade configuration failed");
                    auto headers=L"X-Beetleworx-Version: "+Wide(version)+L"\r\nAuthorization: Bearer "+Wide(credential)+L"\r\n";
                    Need(WinHttpSendRequest(request.value,headers.c_str(),DWORD(headers.size()),WINHTTP_NO_REQUEST_DATA,0,0,0) && WinHttpReceiveResponse(request.value,nullptr),"Voice service unavailable");
                    DWORD status=0,size=sizeof(status);Need(WinHttpQueryHeaders(request.value,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX) && status==101,"Voice upgrade rejected");
                    if(state->stop)break;
                    Http socket(WinHttpWebSocketCompleteUpgrade(request.value,0));Need(socket.value,"Voice upgrade failed");
                    failures=0;Pump(socket.value);
                }catch(...){state->ready=false;}
                if(!state->stop && failures++==0)REXLOG_INFO("Internet voice connection unavailable; gameplay retained; media will retry independently");
                const auto retry=Clock::now()+std::chrono::seconds(std::min(10u,1u+failures));
                while(!state->stop && Clock::now()<retry)std::this_thread::sleep_for(10ms);
            }
        });
    }
    ~MediaWorker(){Stop();if(thread.joinable())thread.join();}
    void Stop(){state->stop=true;}
    bool finished()const{return state->finished.load();}
    std::shared_ptr<VoiceDatagramChannel> Channel(){
        if(state->stop)return {};
        auto result=proxy.lock();if(!result){result=std::make_shared<Proxy>(state);proxy=result;}return result;
    }
    static bool TestQueues(){
        unsigned checks=0;bool ok=true;
        const auto check=[&](bool value,const char* label){++checks;if(!value){ok=false;REXLOG_ERROR("Internet voice queue regression: {}",label);}return value;};
        auto state=std::make_shared<State>();state->host=true;state->ready=true;state->routes=std::make_shared<MediaRoutes>();
        state->routes->peers[255]=16;
        auto proxy=std::make_shared<Proxy>(state);
        std::array<uint8_t,3> payload{7,8,9};
        check(proxy->Send(16,payload) && state->outgoing.back().channel==255,"native peer 16 maps to admitted transport channel 255");
        check(!proxy->Send(255,payload) && !proxy->Send(2,payload),"unmapped peers and transport IDs cannot select a destination");
        for(unsigned i=0;i<160;++i)proxy->Send(16,payload);
        check(state->outgoing.size()==128,"outgoing media is bounded");
        Queued item;item.channel=255;item.at=Clock::now();item.datagram.sender=16;item.datagram.size=3;std::copy(payload.begin(),payload.end(),item.datagram.data.begin());
        state->incoming.push_back(item);auto received=proxy->Receive();
        check(received && received->sender==16 && received->size==3 && received->data[2]==9,"trusted admitted sender and payload preserved");
        state->incoming.push_back(item);state->routes->peers[255]=0;
        check(!proxy->Receive(),"queued media from a retired gameplay channel discarded");
        state->routes->peers[255]=16;item.at=Clock::now()-151ms;state->incoming.push_back(item);
        check(!proxy->Receive(),"stale media discarded");
        state->host=false;check(proxy->Send(1,payload) && state->outgoing.back().channel==0 && !proxy->Send(16,payload),"guest media can target only the admitted host");
        state->ready=false;check(!proxy->Send(1,payload),"disconnected media does not enqueue");
        proxy.reset();check(state->stop,"destroying transport requests worker stop without joining");
        REXLOG_INFO("Internet voice queue regression: {} checks, {}",checks,ok?"PASS":"FAIL");return ok;
    }
};
}
struct InternetRelay::Impl {
    mutable std::mutex mutex;
    InternetRelayStatus view;
    std::atomic<bool> stopping=false;
    std::atomic<bool> completed=false;
    std::atomic<const char*> phase="idle";
    std::atomic<DWORD> http_status=0;
    std::atomic<uint64_t> sent_payload=0,received_payload=0;
    void Phase(const char* next){
        phase.store(next,std::memory_order_release);
        REXLOG_INFO("Internet relay {}: {}",host?"host":"guest",next);
    }
    std::atomic<HINTERNET> websocket=nullptr;
    std::jthread main;
    bool started=false,host=false;
    std::atomic<bool> joinable=true;
    struct Channel {SOCKET socket=INVALID_SOCKET;bool connecting=false;Bytes incoming;size_t sent=0;};
    std::array<Channel,256> channels;
    std::shared_ptr<MediaRoutes> media_routes=std::make_shared<MediaRoutes>();
    std::vector<std::unique_ptr<MediaWorker>> media_workers;
    std::wstring media_hostname;
    INTERNET_PORT media_port=443;
    std::string media_code,media_version,media_credential;
    void CollectMedia(){
        std::array<std::unique_ptr<MediaWorker>,8> retired;size_t count=0;
        {std::lock_guard lock(mutex);
            for(auto it=media_workers.begin();it!=media_workers.end();){
                if((*it)->finished()){retired[count++]=std::move(*it);it=media_workers.erase(it);}else ++it;
            }
        }
    }
    void FinishMedia(){
        std::vector<std::unique_ptr<MediaWorker>> retired;
        {std::lock_guard lock(mutex);for(auto& worker:media_workers)worker->Stop();retired.swap(media_workers);
            SecureZeroMemory(media_credential.data(),media_credential.size());media_credential.clear();}
        // Joining happens on the relay owner, never on the simulation thread.
    }
    void CloseChannel(Channel& channel,unsigned id){
        Close(channel.socket);std::lock_guard lock(media_routes->mutex);media_routes->peers[id]=0;
    }

    SOCKET listener=INVALID_SOCKET;
    WSADATA winsock{};
    bool winsock_started=false;
    void Stop()noexcept{
        stopping=true;
        // Only the owning sender closes WinHTTP handles after its current
        // operation completes. Cross-thread close of synchronous I/O is unsafe.
        std::lock_guard lock(mutex);for(auto& worker:media_workers)worker->Stop();ClearDiscordParty(view.code);
    }
    ~Impl(){
        Stop();
        if(main.joinable())main.join();
        if(auto handle=websocket.exchange(nullptr))WinHttpCloseHandle(handle);
        for(auto& channel:channels)Close(channel.socket);
        Close(listener);if(winsock_started)WSACleanup();
    }
    void Fail(std::string why){std::lock_guard lock(mutex);if(!view.failed){
        view.failed=true;view.ready=false;view.error=std::move(why);
        // No endpoint, room code, host credential or packet contents in logs.
        REXLOG_ERROR("Internet relay {} failed: phase {}, last HTTP status {}, tx payload {} bytes, rx payload {} bytes",
            host?"host":"guest",phase.load(std::memory_order_acquire),http_status.load(),sent_payload.load(),received_payload.load());
    }}
    bool status_failed(){std::lock_guard lock(mutex);return view.failed;}
    void Check(){Need(!stopping,"Internet connection cancelled");}
    struct Endpoint {std::wstring host;INTERNET_PORT port=443;};
    Endpoint Origin(const std::string& text){
        Need(text.size()<=512,"Relay address is too long");auto url=Wide(text);
        URL_COMPONENTS parts{};parts.dwStructSize=sizeof(parts);parts.dwHostNameLength=DWORD(-1);parts.dwUrlPathLength=DWORD(-1);
        parts.dwUserNameLength=DWORD(-1);parts.dwPasswordLength=DWORD(-1);parts.dwExtraInfoLength=DWORD(-1);
        Need(WinHttpCrackUrl(url.c_str(),0,0,&parts) && parts.nScheme==INTERNET_SCHEME_HTTPS && parts.dwHostNameLength &&
            !parts.dwUserNameLength && !parts.dwPasswordLength && !parts.dwExtraInfoLength &&
            (!parts.dwUrlPathLength || (parts.dwUrlPathLength==1 && *parts.lpszUrlPath==L'/')),"The internet service requires a valid HTTPS origin");
        return {{parts.lpszHostName,parts.dwHostNameLength},parts.nPort};
    }
    HINTERNET Request(HINTERNET connection,const std::string& path,const std::string& version,const std::string& body,bool upgrade,const std::string& key={}){
        Check();auto route=Wide(path);Http request(WinHttpOpenRequest(connection,body.empty()?L"GET":L"POST",route.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE));
        Need(request.value!=nullptr,"Cannot create internet service request");
        DWORD redirects=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        Need(WinHttpSetOption(request.value,WINHTTP_OPTION_REDIRECT_POLICY,&redirects,sizeof(redirects)),"Cannot secure internet request");
        if(upgrade)Need(WinHttpSetOption(request.value,WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,nullptr,0),"Cannot start internet WebSocket");
        auto headers=L"Content-Type: application/json\r\nX-Beetleworx-Version: "+Wide(version)+L"\r\n";
        if(!key.empty())headers+=L"Authorization: Bearer "+Wide(key)+L"\r\n";
        Need(WinHttpSendRequest(request.value,headers.c_str(),DWORD(headers.size()),body.empty()?WINHTTP_NO_REQUEST_DATA:const_cast<char*>(body.data()),DWORD(body.size()),DWORD(body.size()),0) &&
            WinHttpReceiveResponse(request.value,nullptr),"Cannot reach the internet lobby service. Check your connection and retry.");
        DWORD status=0,size=sizeof(status);Need(WinHttpQueryHeaders(request.value,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX),"Invalid internet service response");
        http_status.store(status,std::memory_order_release);
        if(status!=(upgrade?101u:200u)){
            std::string text;std::array<char,1024> buffer{};DWORD count=0;
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
            for(;;){
                Check();Need(std::chrono::steady_clock::now()<deadline,"Internet service error response timed out");
                Need(WinHttpReadData(request.value,buffer.data(),DWORD(buffer.size()),&count),"Internet service error response was interrupted");
                if(!count)break;Need(text.size()+count<=8192,"Internet service error is too large");text.append(buffer.data(),count);
            }
            std::string message="Internet lobby service could not complete this request.";
            try{auto json=demo_detail::ParseJson(text);message=json.At("error").Str();
                if(status==409)try{message+=". Install Beetleworx "+json.At("requiredVersion").Str()+" to join.";}catch(...){}
            }catch(...){}
            Need(message.size()<=512,"Invalid internet service error");throw std::runtime_error(message);
        }
        if(upgrade){
            if(!host){
                std::array<wchar_t,128> token{};DWORD bytes=DWORD(token.size()*sizeof(wchar_t));
                if(WinHttpQueryHeaders(request.value,WINHTTP_QUERY_CUSTOM,L"X-Beetleworx-Media-Token",token.data(),&bytes,WINHTTP_NO_HEADER_INDEX)){
                    std::wstring_view value(token.data(),std::min<size_t>(token.size(),bytes/sizeof(wchar_t)));
                    if(!value.empty() && value.back()==L'\0')value.remove_suffix(1);
                    if(value.size()==64 && std::all_of(value.begin(),value.end(),[](wchar_t c){return (c>=L'0' && c<=L'9') || (c>=L'a' && c<=L'f');})){
                        std::lock_guard lock(mutex);media_credential.assign(value.begin(),value.end());
                    }
                }
                SecureZeroMemory(token.data(),sizeof(token));
            }
            auto socket=WinHttpWebSocketCompleteUpgrade(request.value,0);Need(socket!=nullptr,"Internet lobby upgrade failed");return socket;
        }
        auto result=request.value;request.value=nullptr;return result;
    }
    demo_detail::Json JsonRequest(HINTERNET connection,const std::string& path,const std::string& version,const std::string& body={}){
        Http request(Request(connection,path,version,body,false));std::string text;std::array<char,2048> buffer{};
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        for(;;){Check();Need(std::chrono::steady_clock::now()<deadline,"Internet lobby response timed out");DWORD count=0;Need(WinHttpReadData(request.value,buffer.data(),DWORD(buffer.size()),&count),"Internet response was interrupted");if(!count)break;
            Need(text.size()+count<=8192,"Internet lobby response is too large");text.append(buffer.data(),count);}
        return demo_detail::ParseJson(text);
    }
    void Send(HINTERNET handle,const Bytes& bytes){Check();Need(WinHttpWebSocketSend(handle,WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
        const_cast<uint8_t*>(bytes.data()),DWORD(bytes.size()))==NO_ERROR,"The internet game connection was interrupted");
        if(bytes.size()>2 && bytes[0]==2)sent_payload.fetch_add(bytes.size()-2,std::memory_order_relaxed);
    }
    void Pump(HINTERNET handle,uint16_t host_port){
        std::jthread receiver([&,this]{
            try{
                Bytes frame;std::array<uint8_t,65538> buffer{};
                while(!stopping){
                    DWORD count=0;WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
                    Need(WinHttpWebSocketReceive(handle,buffer.data(),DWORD(buffer.size()),&count,&type)==NO_ERROR,"The internet game connection closed");
                    Need(type!=WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE,"The host closed the internet lobby");
                    if(type==WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE){Need(count==4 && std::equal(buffer.begin(),buffer.begin()+4,"pong"),"Invalid internet heartbeat");continue;}
                    Need(type==WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE || type==WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,"Invalid internet game frame");
                    Need(frame.size()+count<=65538,"Internet game frame exceeds limit");frame.insert(frame.end(),buffer.begin(),buffer.begin()+count);
                    if(type==WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE)continue;
                    Need(frame.size()>=2,"Invalid internet game frame");auto kind=frame[0],id=frame[1];
                    std::lock_guard lock(mutex);auto& channel=channels[id];
                    if(host && kind==1){
                        Need(id && frame.size()==2 && channel.socket==INVALID_SOCKET,"Invalid internet guest admission");
                        channel.socket=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);Need(channel.socket!=INVALID_SOCKET && Nonblocking(channel.socket),"Cannot accept internet guest locally");
                        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(host_port);
                        const auto result=connect(channel.socket,reinterpret_cast<sockaddr*>(&address),sizeof(address));
                        Need(result==0 || WSAGetLastError()==WSAEWOULDBLOCK,"Cannot connect internet guest to the game");channel.connecting=result!=0;
                    }else if(kind==3){Need(frame.size()==2 && (host?id!=0:id==0),"Invalid internet guest close");CloseChannel(channel,id);channel.incoming.clear();channel.sent=0;}
                    else if(kind==2){
                        Need(frame.size()>2 && (host?id!=0:id==0),"Invalid internet channel");
                        received_payload.fetch_add(frame.size()-2,std::memory_order_relaxed);
                        // A native close can race buffered relay data; never
                        // recreate a socket or redirect those bytes elsewhere.
                        if(channel.socket!=INVALID_SOCKET){
                            if(channel.sent){channel.incoming.erase(channel.incoming.begin(),channel.incoming.begin()+channel.sent);channel.sent=0;}
                            Need(channel.incoming.size()+frame.size()-2<=256*1024,"Internet receive queue exceeded limit");
                            channel.incoming.insert(channel.incoming.end(),frame.begin()+2,frame.end());
                        }else if(!host){
                            Need(channel.incoming.size()+frame.size()-2<=256*1024,"Internet receive queue exceeded limit");
                            channel.incoming.insert(channel.incoming.end(),frame.begin()+2,frame.end());
                        }
                    }else Need(false,"Invalid internet channel operation");
                    frame.clear();
                }
            }catch(const std::exception& error){if(!stopping)Fail(error.what());stopping=true;}
        });
        try{
            auto heartbeat=std::chrono::steady_clock::now();std::array<uint8_t,8192> buffer{};
            bool published_joinable=true;
            while(!stopping){
                CollectMedia();
                std::deque<Bytes> output;
                if(host){const bool current=joinable.load(std::memory_order_acquire);
                    if(current!=published_joinable){output.push_back(Bytes{4,0,uint8_t(current)});published_joinable=current;}
                }
                {
                    std::lock_guard lock(mutex);
                    if(!host && listener!=INVALID_SOCKET && channels[0].socket==INVALID_SOCKET){
                        auto client=accept(listener,nullptr,nullptr);
                        if(client!=INVALID_SOCKET){Need(Nonblocking(client),"Cannot configure local internet connection");channels[0].socket=client;Close(listener);}
                        else Need(WSAGetLastError()==WSAEWOULDBLOCK,"Cannot accept local internet connection");
                    }
                    for(unsigned id=0;id<channels.size();++id){auto& channel=channels[id];if(channel.socket==INVALID_SOCKET)continue;
                        if(channel.connecting){
                            fd_set writable,errors;FD_ZERO(&writable);FD_ZERO(&errors);FD_SET(channel.socket,&writable);FD_SET(channel.socket,&errors);timeval timeout{};
                            const auto result=select(0,nullptr,&writable,&errors,&timeout);Need(result>=0 && !FD_ISSET(channel.socket,&errors),"Could not attach internet guest to the game");
                            if(!FD_ISSET(channel.socket,&writable))continue;channel.connecting=false;
                        }
                        while(channel.sent<channel.incoming.size()){
                            auto count=send(channel.socket,reinterpret_cast<const char*>(channel.incoming.data()+channel.sent),int(channel.incoming.size()-channel.sent),0);
                            if(count>0)channel.sent+=size_t(count);
                            else if(count<0 && WSAGetLastError()==WSAEWOULDBLOCK)break;
                            else{
                                CloseChannel(channel,id);channel.incoming.clear();channel.sent=0;
                                if(host)output.push_back(Bytes{3,uint8_t(id)});else throw std::runtime_error("The local game left the internet session");
                                break;
                            }
                        }
                        if(channel.socket==INVALID_SOCKET)continue;
                        if(channel.sent==channel.incoming.size()){channel.incoming.clear();channel.sent=0;}
                        const auto count=recv(channel.socket,reinterpret_cast<char*>(buffer.data()),int(buffer.size()),0);
                        if(count>0){Bytes bytes{2,uint8_t(id)};bytes.insert(bytes.end(),buffer.begin(),buffer.begin()+count);output.push_back(std::move(bytes));}
                        else if(count==0 || WSAGetLastError()!=WSAEWOULDBLOCK){CloseChannel(channel,id);channel.incoming.clear();channel.sent=0;
                            if(host)output.push_back(Bytes{3,uint8_t(id)});else throw std::runtime_error("The local game left the internet session");}
                    }
                }
                for(const auto& bytes:output)Send(handle,bytes);
                if(!stopping && std::chrono::steady_clock::now()-heartbeat>=5s){
                    Need(WinHttpWebSocketSend(handle,WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,const_cast<char*>("ping"),4)==NO_ERROR,"Internet lobby heartbeat failed");heartbeat=std::chrono::steady_clock::now();
                }
                std::this_thread::sleep_for(1ms);
            }
        }catch(const std::exception& error){if(!stopping)Fail(error.what());stopping=true;}
        // No send is pending on this owning thread. WebSocketClose cancels
        // the receiver; the actual handle stays alive until that thread exits.
        DWORD close_timeout=250;
        WinHttpSetOption(handle,WINHTTP_OPTION_WEB_SOCKET_CLOSE_TIMEOUT,&close_timeout,sizeof(close_timeout));
        WinHttpWebSocketClose(handle,WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,nullptr,0);
        receiver.join();
        if(auto owned=websocket.exchange(nullptr))WinHttpCloseHandle(owned);
    }
    void Run(std::string origin,std::string version,std::string value){
        struct Completion{Impl& owner;~Completion(){owner.stopping=true;owner.FinishMedia();owner.completed.store(true,std::memory_order_release);}} completion{*this};
        try{
            Phase("validating endpoint");auto endpoint=Origin(origin);winsock_started=WSAStartup(MAKEWORD(2,2),&winsock)==0;Need(winsock_started,"Cannot initialize internet game sockets");
            Http session(WinHttpOpen(L"Beetleworx/1",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0));
            Need(session.value!=nullptr,"Cannot initialize internet connection");
            Need(WinHttpSetTimeouts(session.value,5000,5000,5000,5000),"Cannot bound internet connection waits");
            Http connection(WinHttpConnect(session.value,endpoint.host.c_str(),endpoint.port,0));Need(connection.value!=nullptr,"Cannot open internet service");
            std::string code,key,invitation;uint16_t port=0;
            if(host){
                const auto decoded=DecodeInvite(value);Need(decoded.has_value() && decoded->address=="127.0.0.1","Internet hosting requires an isolated loopback listener");port=decoded->port;
                Phase("creating hosted room");
                auto result=JsonRequest(connection.value,"/v1/rooms",version,"{\"version\":"+Quote(version)+",\"invitation\":"+Quote(value)+"}");
                code=result.At("code").Str();key=result.At("hostKey").Str();
            }else code=value;
            Need(code.size()==8 && std::all_of(code.begin(),code.end(),[](char c){return std::string_view("ABCDEFGHJKLMNPQRSTUVWXYZ23456789").find(c)!=std::string_view::npos;}),"Use the host's eight-character room code");
            Check();
            if(!host){
                Phase("looking up hosted room");
                auto result=JsonRequest(connection.value,"/v1/rooms/"+code,version);auto decoded=DecodeInvite(result.At("invitation").Str());Need(decoded.has_value(),"Invalid internet session invitation");
                listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);Need(listener!=INVALID_SOCKET && Nonblocking(listener),"Cannot create local internet bridge");
                sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
                Need(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0 && listen(listener,1)==0,"Cannot listen for the local game");
                int length=sizeof(address);Need(getsockname(listener,reinterpret_cast<sockaddr*>(&address),&length)==0,"Cannot identify local internet bridge");
                decoded->address="127.0.0.1";decoded->port=ntohs(address.sin_port);auto encoded=EncodeInvite(*decoded);Need(encoded.has_value(),"Invalid local internet session");invitation=*encoded;
            }
            Phase("opening TLS WebSocket");
            auto socket=Request(connection.value,"/v1/rooms/"+code+(host?"/host":"/join"),version,{},true,key);
            websocket=socket;
            {std::lock_guard lock(mutex);
                media_hostname=endpoint.host;media_port=endpoint.port;media_code=code;media_version=version;
                if(host)media_credential=key;
                SecureZeroMemory(key.data(),key.size());
                view.ready=true;view.code=code;view.invitation=invitation;}
            if(stopping){if(auto owned=websocket.exchange(nullptr))WinHttpCloseHandle(owned);return;}
            Phase("bridging native Room stream");
            Pump(socket,port);
            REXLOG_INFO("Internet relay {} ended: tx payload {} bytes, rx payload {} bytes, failure {}",
                host?"host":"guest",sent_payload.load(),received_payload.load(),status_failed());
        }catch(const std::exception& error){if(!stopping)Fail(error.what());}
    }
    bool Start(bool hosting,std::string origin,std::string version,std::string value){
        if(started)return false;started=true;host=hosting;
        try{
            main=std::jthread([this,origin=std::move(origin),version=std::move(version),value=std::move(value)]{Run(origin,version,value);});return true;
        }catch(...){completed.store(true,std::memory_order_release);Fail("Cannot start the internet connection worker. Try again.");return false;}
    }
};
InternetRelay::InternetRelay():impl_(std::make_unique<Impl>()){}
InternetRelay::~InternetRelay()=default;
void InternetRelay::Stop()noexcept{impl_->Stop();}
void InternetRelay::SetJoinable(bool value){if(impl_->host)impl_->joinable.store(value,std::memory_order_release);}
bool InternetRelay::BindVoicePeer(unsigned peer,unsigned short port){
    auto& p=*impl_;if(peer<2 || peer>16 || !port)return false;
    std::lock_guard lock(p.mutex);if(!p.host || p.stopping || !p.view.ready)return false;
    for(unsigned id=1;id<256;++id){
        auto& channel=p.channels[id];if(channel.socket==INVALID_SOCKET)continue;
        sockaddr_in address{};int size=sizeof(address);
        if(getsockname(channel.socket,reinterpret_cast<sockaddr*>(&address),&size)!=0 || address.sin_family!=AF_INET || ntohs(address.sin_port)!=port)continue;
        std::lock_guard route_lock(p.media_routes->mutex);
        if(p.media_routes->peers[id] && p.media_routes->peers[id]!=peer)return false;
        for(unsigned other=1;other<256;++other)if(other!=id && p.media_routes->peers[other]==peer)return false;
        p.media_routes->peers[id]=peer;return true;
    }
    return false;
}
std::shared_ptr<VoiceDatagramChannel> InternetRelay::VoiceChannel(){
    auto& p=*impl_;std::lock_guard lock(p.mutex);
    if(p.stopping || !p.view.ready || p.media_credential.empty())return {};
    for(auto& worker:p.media_workers)if(!worker->finished())if(auto proxy=worker->Channel())return proxy;
    if(p.media_workers.size()>=8)return {};
    p.media_workers.reserve(8);
    std::shared_ptr<VoiceDatagramChannel> proxy;
    auto worker=std::make_unique<MediaWorker>(p.host,p.media_routes,p.media_hostname,p.media_port,p.media_code,p.media_version,p.media_credential,proxy);
    p.media_workers.push_back(std::move(worker));return proxy;
}
bool TestOnlineInternetVoiceRelay(std::string origin){
    if(!MediaWorker::TestQueues())return false;
    if(origin.empty())return true;
    try{
        WSADATA data{};Need(WSAStartup(MAKEWORD(2,2),&data)==0,"Fixture Winsock failed");
        struct Cleanup{~Cleanup(){WSACleanup();}}cleanup;
        struct Sockets{
            SOCKET listener=INVALID_SOCKET,host=INVALID_SOCKET,guest=INVALID_SOCKET;
            ~Sockets(){Close(listener);Close(host);Close(guest);}
        }sockets;
        using Clock=std::chrono::steady_clock;
        const auto wait=[](auto condition){const auto end=Clock::now()+15s;do{if(condition())return true;std::this_thread::sleep_for(2ms);}while(Clock::now()<end);return false;};
        sockets.listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);Need(sockets.listener!=INVALID_SOCKET && Nonblocking(sockets.listener),"Fixture listener failed");
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        Need(bind(sockets.listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0 && listen(sockets.listener,1)==0,"Fixture bind failed");
        int length=sizeof(address);Need(getsockname(sockets.listener,reinterpret_cast<sockaddr*>(&address),&length)==0,"Fixture listener port failed");
        Invite invite;invite.address="127.0.0.1";invite.port=ntohs(address.sin_port);Need(NewSessionNonce(invite.session),"Fixture nonce failed");
        auto encoded=EncodeInvite(invite);Need(encoded.has_value(),"Fixture invitation failed");
        InternetRelay host,guest;
        Need(host.Host(origin,"0.1.0-demo.4",*encoded) && wait([&]{auto s=host.status();Need(!s.failed,"Native relay host creation failed");return s.ready;}),"Native relay host timed out");
        Need(guest.Join(origin,"0.1.0-demo.4",host.status().code) && wait([&]{auto s=guest.status();Need(!s.failed,"Native relay guest join failed");return s.ready;}),"Native relay guest timed out");
        auto local=DecodeInvite(guest.status().invitation);Need(local.has_value(),"Fixture local invitation invalid");
        sockets.guest=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);Need(sockets.guest!=INVALID_SOCKET && Nonblocking(sockets.guest),"Fixture guest socket failed");
        address.sin_port=htons(local->port);
        const auto result=connect(sockets.guest,reinterpret_cast<sockaddr*>(&address),sizeof(address));Need(result==0 || WSAGetLastError()==WSAEWOULDBLOCK,"Fixture local connect failed");
        Need(wait([&]{sockets.host=accept(sockets.listener,nullptr,nullptr);return sockets.host!=INVALID_SOCKET;}),"Native relay host admission timed out");
        Need(Nonblocking(sockets.host),"Fixture accepted socket failed");
        length=sizeof(address);Need(getpeername(sockets.host,reinterpret_cast<sockaddr*>(&address),&length)==0 && host.BindVoicePeer(2,ntohs(address.sin_port)),"Native admitted channel mapping failed");
        const auto game_transfer=[&](SOCKET from,SOCKET to){
            std::array<char,4> bytes{'t','e','s','t'},received{};
            Need(send(from,bytes.data(),int(bytes.size()),0)==int(bytes.size()),"Fixture gameplay send failed");
            size_t at=0;Need(wait([&]{auto n=recv(to,received.data()+at,int(received.size()-at),0);if(n>0)at+=size_t(n);else Need(n<0 && WSAGetLastError()==WSAEWOULDBLOCK,"Fixture gameplay disconnected");return at==received.size();}),"Fixture gameplay transfer timed out");
            Need(bytes==received,"Native gameplay bytes changed");
        };
        game_transfer(sockets.guest,sockets.host);game_transfer(sockets.host,sockets.guest);
        host.SetJoinable(false);
        for(unsigned generation=0;generation<2;++generation){
            VoiceTransport host_voice,guest_voice;auto host_channel=host.VoiceChannel(),guest_channel=guest.VoiceChannel();
            Need(host_channel && guest_channel,"Private native media credentials unavailable");
            Need(host_voice.UseDatagramChannel(std::move(host_channel)) && guest_voice.UseDatagramChannel(std::move(guest_channel)),"Native media channel install failed");
            Need(host_voice.Host("127.0.0.1",invite.session),"Fixture voice host failed");
            VoiceKeys keys;Need(host_voice.AddPeer(2,"127.0.0.1",keys) && guest_voice.Join("127.0.0.1",host_voice.port(),2,invite.session,keys),"Fixture voice key offer failed");
            host_voice.SetRoster(3);guest_voice.SetRoster(3);
            Need(wait([&]{host_voice.Poll();guest_voice.Poll();return host_voice.peer_ready(2) && guest_voice.ready();}),"Encrypted native media handshake timed out");
            VoicePcm pcm{};VoiceEncoder encoder;VoicePacket host_packet,guest_packet;
            Need(encoder.Encode(100+generation,pcm,host_packet) && encoder.Encode(200+generation,pcm,guest_packet),"Fixture Opus encode failed");
            Need(host_voice.Send(host_packet) && guest_voice.Send(guest_packet),"Encrypted native media send failed");
            std::vector<ReceivedVoice> at_host,at_guest;
            Need(wait([&]{host_voice.Poll();guest_voice.Poll();auto h=host_voice.TakeReceived(),g=guest_voice.TakeReceived();at_host.insert(at_host.end(),h.begin(),h.end());at_guest.insert(at_guest.end(),g.begin(),g.end());return !at_host.empty() && !at_guest.empty();}),"Encrypted native media delivery timed out");
            Need(at_host.size()==1 && at_guest.size()==1 && at_host[0].peer==2 && at_guest[0].peer==1 && at_host[0].packet.sequence==guest_packet.sequence && at_guest[0].packet.sequence==host_packet.sequence && at_host[0].packet.data==guest_packet.data && at_guest[0].packet.data==host_packet.data,"Native encrypted audio identity/payload changed");
            REXLOG_INFO("Native internet media fixture generation {}: encrypted handshake and Opus payloads PASS",generation);
        }
        std::this_thread::sleep_for(300ms);
        game_transfer(sockets.guest,sockets.host);game_transfer(sockets.host,sockets.guest);
        REXLOG_INFO("Native internet media fixture: PASS; game admission, channel binding, two encrypted voice generations after join lock, game stream retained after media retirement; no devices or native worlds exercised");
        return true;
    }catch(const std::exception& error){REXLOG_ERROR("Native internet media fixture: FAIL: {}",error.what());return false;}
}
bool InternetRelay::finished()const noexcept{return impl_->completed.load(std::memory_order_acquire);}
bool InternetRelay::Host(std::string origin,std::string version,std::string invitation){return impl_->Start(true,std::move(origin),std::move(version),std::move(invitation));}
bool InternetRelay::Join(std::string origin,std::string version,std::string code){return impl_->Start(false,std::move(origin),std::move(version),std::move(code));}
InternetRelayStatus InternetRelay::status()const{std::lock_guard lock(impl_->mutex);return impl_->view;}
}
