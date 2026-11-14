#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include "online_internet_relay.h"
#include "online_discord.h"
#include "online_invite.h"
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

    SOCKET listener=INVALID_SOCKET;
    WSADATA winsock{};
    bool winsock_started=false;
    void Stop()noexcept{
        stopping=true;
        // Only the owning sender closes WinHTTP handles after its current
        // operation completes. Cross-thread close of synchronous I/O is unsafe.
        std::lock_guard lock(mutex);ClearDiscordParty(view.code);
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
        if(upgrade){auto socket=WinHttpWebSocketCompleteUpgrade(request.value,0);Need(socket!=nullptr,"Internet lobby upgrade failed");return socket;}
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
                    }else if(kind==3){Need(frame.size()==2 && (host?id!=0:id==0),"Invalid internet guest close");Close(channel.socket);channel.incoming.clear();channel.sent=0;}
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
                                Close(channel.socket);channel.incoming.clear();channel.sent=0;
                                if(host)output.push_back(Bytes{3,uint8_t(id)});else throw std::runtime_error("The local game left the internet session");
                                break;
                            }
                        }
                        if(channel.socket==INVALID_SOCKET)continue;
                        if(channel.sent==channel.incoming.size()){channel.incoming.clear();channel.sent=0;}
                        const auto count=recv(channel.socket,reinterpret_cast<char*>(buffer.data()),int(buffer.size()),0);
                        if(count>0){Bytes bytes{2,uint8_t(id)};bytes.insert(bytes.end(),buffer.begin(),buffer.begin()+count);output.push_back(std::move(bytes));}
                        else if(count==0 || WSAGetLastError()!=WSAEWOULDBLOCK){Close(channel.socket);channel.incoming.clear();channel.sent=0;
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
        struct Completion{std::atomic<bool>& flag;~Completion(){flag.store(true,std::memory_order_release);}} completion{completed};
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
            {std::lock_guard lock(mutex);view.ready=true;view.code=code;view.invitation=invitation;}
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
bool InternetRelay::finished()const noexcept{return impl_->completed.load(std::memory_order_acquire);}
bool InternetRelay::Host(std::string origin,std::string version,std::string invitation){return impl_->Start(true,std::move(origin),std::move(version),std::move(invitation));}
bool InternetRelay::Join(std::string origin,std::string version,std::string code){return impl_->Start(false,std::move(origin),std::move(version),std::move(code));}
InternetRelayStatus InternetRelay::status()const{std::lock_guard lock(impl_->mutex);return impl_->view;}
}
