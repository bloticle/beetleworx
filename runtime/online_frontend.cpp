#include "online_address.h"
#include "online_local_route.h"
#include "online_frontend.h"
#include "online_checkpoint.h"
#include "online_invite.h"
#include "online_discovery.h"
#include "online_router.h"
#include "online_notice.h"
#include "demo_setup.h"
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <shlobj.h>
#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace em2::net {
namespace {
enum {Address=101,Checkpoint,Invitation,HostButton,JoinButton,CopyButton,CancelButton,Status,BrowseButton,Port,LANList,LANFind,LANJoin,LANStatus,InputBuffer,RouterOption,RouterInfo};
std::vector<std::wstring> LocalAddresses() {
    ULONG bytes=16384;
    std::vector<unsigned char> storage(bytes);
    ULONG status=ERROR_BUFFER_OVERFLOW;
    for(unsigned attempt=0;attempt<3 && status==ERROR_BUFFER_OVERFLOW;++attempt) {
        storage.resize(bytes);
        status=GetAdaptersAddresses(AF_UNSPEC,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|
            GAA_FLAG_SKIP_DNS_SERVER,nullptr,reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data()),&bytes);
    }
    if(status!=NO_ERROR)return {};
    std::vector<std::wstring> result;
    std::vector<LocalAddressInterface> interfaces;
    ULONG ipv4_route=0,ipv6_route=0;
    // Routing-table queries only: no packets, DNS, router changes or Internet
    // service requests. Lookup failure preserves the existing address order.
    auto best_route=[](const char* destination,ULONG& index){
        auto numeric=ParseNumericAddress(destination);
        if(!numeric || GetBestInterfaceEx(reinterpret_cast<sockaddr*>(&numeric->storage),&index)!=NO_ERROR)index=0;
    };
    best_route("1.1.1.1",ipv4_route);best_route("2606:4700:4700::1111",ipv6_route);
    for(auto* adapter=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());adapter;adapter=adapter->Next) {
        if(adapter->OperStatus!=IfOperStatusUp || adapter->IfType==IF_TYPE_SOFTWARE_LOOPBACK || adapter->IfType==IF_TYPE_TUNNEL)continue;
        for(auto* item=adapter->FirstUnicastAddress;item;item=item->Next) {
            if(!item->Address.lpSockaddr || item->DadState!=IpDadStatePreferred)continue;
            const auto family=item->Address.lpSockaddr->sa_family;
            const int size=family==AF_INET?sizeof(sockaddr_in):family==AF_INET6?sizeof(sockaddr_in6):0;
            if(!size || item->Address.iSockaddrLength<size)continue;
            NumericAddress numeric;numeric.size=size;
            std::memcpy(&numeric.storage,item->Address.lpSockaddr,size);
            const auto ascii=numeric.Text();
            if(!ParseNumericAddress(ascii))continue;
            if(!numeric.ipv6()) {
                const auto* ip=numeric.bytes();
                if(ip[0]==127 || (ip[0]==169 && ip[1]==254))continue;
            } else if(IN6_IS_ADDR_LOOPBACK(&reinterpret_cast<const sockaddr_in6*>(&numeric.storage)->sin6_addr))continue;
            const std::wstring text(ascii.begin(),ascii.end());
            if(std::find(result.begin(),result.end(),text)==result.end()){
                result.push_back(text);interfaces.push_back({family==AF_INET6?adapter->Ipv6IfIndex:adapter->IfIndex,family==AF_INET6});
            }
        }
    }
    const auto preferred=PreferredLocalAddress(interfaces,ipv4_route,ipv6_route);
    if(preferred)std::rotate(result.begin(),result.begin()+preferred,result.begin()+preferred+1);
    return result;
}
struct Screen {
    std::filesystem::path executable,content,user;
    std::unique_ptr<OnlineBoot> boot=std::make_unique<OnlineBoot>();
    std::optional<BootResult> result;
    bool cancelling=false;
    bool lan_requested=false;
    LANBrowser browser;LANAdvertiser advertisement;
    std::vector<LANGame> lan_rows;
    std::string advertised_code;
    bool try_router=false;
    std::unique_ptr<RouterMapping> router;
    HWND window=nullptr;
    std::wstring Text(int id) {
        auto control=GetDlgItem(window,id);const int size=GetWindowTextLengthW(control);
        std::wstring text(size+1,L'\0');GetWindowTextW(control,text.data(),size+1);text.resize(size);return text;
    }
    void Set(int id,const std::wstring& text){SetWindowTextW(GetDlgItem(window,id),text.c_str());}
    void Busy(bool busy) {
        for(int id:{Address,Port,Checkpoint,BrowseButton,HostButton,JoinButton,LANFind,LANJoin,LANList,InputBuffer,RouterOption})EnableWindow(GetDlgItem(window,id),!busy);
        // Invitation remains selectable so the host can share it while waiting.
        SendMessageW(GetDlgItem(window,Invitation),EM_SETREADONLY,busy,0);
    }
    void Begin(bool host) {
        const auto state=boot->status().state;
        if(cancelling || (state!=BootState::Idle && state!=BootState::Failed && state!=BootState::Cancelled))return;
        browser.Stop();advertisement.Stop();advertised_code.clear();lan_requested=false;
        router.reset();try_router=host && SendDlgItemMessageW(window,RouterOption,BM_GETCHECK,0,0)==BST_CHECKED;
        Set(CopyButton,try_router?L"Copy LAN invitation":L"Copy invitation");
        BOOL valid=FALSE;
        const auto port=host?GetDlgItemInt(window,Port,&valid,FALSE):0;
        if(host && (!valid || port>65535)){Set(Status,L"Choose a TCP port from 1–65535, or 0 for an automatic port.");return;}
        const auto delay=SendDlgItemMessageW(window,InputBuffer,CB_GETCURSEL,0,0);
        if(host && (delay<0 || delay>12)){Set(Status,L"Choose an input buffer from 0–12 ticks.");return;}
        if(boot->status().state!=BootState::Idle)boot=std::make_unique<OnlineBoot>();
        try {
            std::filesystem::path checkpoint;
            if(host){
                const auto source=ResolveCampaignSaveSource(std::filesystem::path(Text(Checkpoint)));
                if(!source.root){Set(Status,std::wstring(source.error.begin(),source.error.end()));return;}
                checkpoint=*source.root;Set(Checkpoint,checkpoint.wstring());
            }
            const auto parent=std::filesystem::absolute(user.parent_path()/"online_sessions");
            std::filesystem::create_directories(parent);
            auto destination=parent/("session-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
            BootPaths paths{executable,content,destination};
            const auto invite=Text(Invitation),address=Text(Address);
            const std::string advertised(address.begin(),address.end());
            const auto endpoint=ParseNumericAddress(advertised);
            if(try_router && (!endpoint || endpoint->ipv6())){Set(Status,L"Automatic router setup needs this PC's local IPv4 address. Choose one or turn the option off.");return;}
            const bool started=host?boot->Host(paths,checkpoint,endpoint && endpoint->ipv6()?"::":"0.0.0.0",
                advertised,uint16_t(port),uint8_t(delay)):boot->Join(paths,std::string(invite.begin(),invite.end()));
            if(!started){Set(Status,L"Check the IPv4/IPv6 address or invitation. IPv6 needs an unscoped unicast address.");return;}
            Busy(true);Set(Status,L"Checking installation files and preparing an isolated session save...");
        }catch(const std::exception&){Set(Status,L"Could not prepare session paths. Check the save folder and permissions.");}
    }
    void FindLAN() {
        const auto state=boot->status().state;
        if(cancelling || (state!=BootState::Idle && state!=BootState::Failed && state!=BootState::Cancelled))return;
        lan_requested=true;
        if(browser.Start(LocalLANDiscoveryTargets()))Set(LANStatus,L"Looking for IPv4 LAN hosts...");
        else Set(LANStatus,std::wstring(browser.error().begin(),browser.error().end())+L". Invitation codes still work.");
        RefreshLAN();
    }
    void RefreshLAN() {
        if(lan_rows==browser.games())return;
        auto control=GetDlgItem(window,LANList);const auto selected=SendMessageW(control,CB_GETCURSEL,0,0);
        const auto old=selected>=0 && size_t(selected)<lan_rows.size()?lan_rows[size_t(selected)].invitation:std::string{};
        lan_rows=browser.games();SendMessageW(control,CB_RESETCONTENT,0,0);int restore=0;
        for(size_t i=0;i<lan_rows.size();++i) {
            const auto& game=lan_rows[i];const std::string name=game.address+":"+std::to_string(game.port);
            const std::wstring label(name.begin(),name.end());SendMessageW(control,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
            if(game.invitation==old)restore=int(i);
        }
        if(!lan_rows.empty())SendMessageW(control,CB_SETCURSEL,restore,0);
    }
    void JoinLAN() {
        const auto selected=SendDlgItemMessageW(window,LANList,CB_GETCURSEL,0,0);
        if(selected<0 || size_t(selected)>=lan_rows.size()){Set(LANStatus,L"Find and select a LAN host first.");return;}
        const auto code=lan_rows[size_t(selected)].invitation;Set(Invitation,std::wstring(code.begin(),code.end()));Begin(false);
    }
    void Browse() {
        // This runs only after the user's Browse click. Use the legacy picker
        // if this UI thread already belongs to a different COM apartment.
        const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        BROWSEINFOW info{};info.hwndOwner=window;
        info.lpszTitle=L"Choose EM2's user_data, 42560816 or SAVEGAME folder";
        info.ulFlags=BIF_RETURNONLYFSDIRS|(SUCCEEDED(initialized)?BIF_NEWDIALOGSTYLE:0);
        if(auto selected=SHBrowseForFolderW(&info)) {
            std::array<wchar_t,MAX_PATH> path{};
            if(SHGetPathFromIDListW(selected,path.data()))Set(Checkpoint,path.data());
            CoTaskMemFree(selected);
        }
        if(SUCCEEDED(initialized))CoUninitialize();
    }
    void Poll() {
        boot->Poll();const auto status=boot->status();
        if(status.state==BootState::Failed || status.state==BootState::Cancelled){router.reset();try_router=false;}
        if(try_router && !router && status.state==BootState::WaitingForPeer) {
            router=std::make_unique<RouterMapping>();
            if(const auto invite=DecodeInvite(status.invitation))router->Start(invite->address,invite->port);
        }
        if(router) {
            router->Poll();const auto& mapped=router->status();
            const auto message=mapped.state==RouterState::Active?std::string("Internet invitation ready. Temporary router port ")+std::to_string(mapped.port)+"; reachability still depends on your network.":
                mapped.state==RouterState::Failed?mapped.error:"Requesting a temporary Internet connection from the router...";
            Set(RouterInfo,std::wstring(message.begin(),message.end()));
            Set(CopyButton,mapped.state==RouterState::Active?L"Copy Internet invitation":L"Copy LAN invitation");
        }
        if(status.state==BootState::WaitingForPeer && !cancelling) {
            if(advertised_code!=status.invitation) {
                advertised_code=status.invitation;const auto invite=DecodeInvite(status.invitation);
                if(invite)advertisement.Start(*invite);
            }
            advertisement.Poll();
        }else advertisement.Stop();
        browser.Poll();RefreshLAN();
        if(browser.scanning())Set(LANStatus,L"Looking for IPv4 LAN hosts...");
        else if(!browser.games().empty())Set(LANStatus,L"Select a host, then Join selected LAN game.");
        else if(lan_requested)Set(LANStatus,browser.error().empty()?L"No LAN hosts found. Try Find again or paste an invitation.":
            std::wstring(browser.error().begin(),browser.error().end())+L". Invitation codes still work.");
        if(cancelling && (status.state==BootState::Idle || status.state==BootState::Cancelled || status.state==BootState::Failed)) {
            DestroyWindow(window);return;
        }
        if(!status.invitation.empty()) {
            auto code=status.invitation;
            if(router)if(auto internet=InternetInvitation(code,router->status()))code=*internet;
            Set(Invitation,std::wstring(code.begin(),code.end()));
        }
        switch(status.state) {
        case BootState::Idle:return;
        case BootState::Preparing:Set(Status,L"Checking files and preparing a private copy of the save...");break;
        case BootState::WaitingForPeer: {
            const auto invite=DecodeInvite(status.invitation);
            Set(Status,L"Waiting for player two"+(invite?L" on TCP port "+std::to_wstring(invite->port):std::wstring{})+
                (advertisement.port()?L". LAN players can find this host; invitation codes also work.":L". Copy the invitation to share; LAN discovery is unavailable."));break;
        }
        case BootState::Connecting:Set(Status,L"Connecting and checking game compatibility...");break;
        case BootState::Transferring:Set(Status,L"Transferring the host's session save...");break;
        case BootState::Installing:Set(Status,L"Installing the session save in its own folder...");break;
        case BootState::Cancelling:Set(Status,L"Cancelling and finishing file operations...");break;
        case BootState::Failed:
            {const auto advice=OnlineFailureAdvice(status.error,false);
            Busy(false);Set(Status,std::wstring(advice.begin(),advice.end()));break;}
        case BootState::Ready:
            result=boot->TakeReady();
            if(result){if(router && !result->session->KeepRouterMapping(router))router.reset();DestroyWindow(window);}break;
        default:break;
        }
    }
    void Copy() {
        const auto text=Text(Invitation);if(text.empty() || !OpenClipboard(window))return;
        auto data=GlobalAlloc(GMEM_MOVEABLE,(text.size()+1)*sizeof(wchar_t));
        if(data) {
            auto* bytes=GlobalLock(data);
            if(bytes) {
                memcpy(bytes,text.c_str(),(text.size()+1)*sizeof(wchar_t));GlobalUnlock(data);
                if(!EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT,data))GlobalFree(data);
            }else GlobalFree(data);
        }
        CloseClipboard();
    }
};
LRESULT CALLBACK Procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    auto* screen=reinterpret_cast<Screen*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE) {
        screen=static_cast<Screen*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        screen->window=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(screen));
    }
    if(!screen)return DefWindowProcW(window,message,wparam,lparam);
    switch(message) {
    case WM_COMMAND:
        switch(LOWORD(wparam)) {
        case HostButton:screen->Begin(true);return 0;
        case JoinButton:screen->Begin(false);return 0;
        case CopyButton:screen->Copy();return 0;
        case BrowseButton:screen->Browse();return 0;
        case LANFind:screen->FindLAN();return 0;
        case LANJoin:screen->JoinLAN();return 0;
        case RouterOption:
            if(SendDlgItemMessageW(window,RouterOption,BM_GETCHECK,0,0)==BST_CHECKED) {
                const auto current=screen->Text(Address);const auto endpoint=ParseNumericAddress(std::string(current.begin(),current.end()));
                if(!endpoint || endpoint->ipv6())for(const auto& local:LocalAddresses()) {
                    const auto candidate=ParseNumericAddress(std::string(local.begin(),local.end()));
                    if(candidate && !candidate->ipv6()){screen->Set(Address,local);break;}
                }
            }
            screen->Set(RouterInfo,SendDlgItemMessageW(window,RouterOption,BM_GETCHECK,0,0)==BST_CHECKED?
                L"Hosting will request a temporary TCP port from a compatible router. Choose this PC's local IPv4 address.":
                L"LAN and direct invitations are available without automatic router setup.");return 0;
        case CancelButton:SendMessageW(window,WM_CLOSE,0,0);return 0;
        }break;
    case WM_TIMER:screen->Poll();return 0;
    case WM_CLOSE:screen->cancelling=true;screen->router.reset();screen->try_router=false;screen->browser.Stop();screen->advertisement.Stop();screen->boot->Cancel();screen->Busy(true);screen->Poll();return 0;
    case WM_DESTROY:KillTimer(window,1);return 0;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}
}
std::string PreferredOnlineHostAddress(){
    for(const auto& address:LocalAddresses()){
        const std::string ascii(address.begin(),address.end());const auto parsed=ParseNumericAddress(ascii);
        if(parsed && !parsed->ipv6())return ascii;
    }
    return {};
}
std::optional<BootResult> ShowOnlineFrontend(const std::filesystem::path& executable,
    const std::filesystem::path& content,const std::filesystem::path& user) {
    if(PublicDemoBuild())return {};
    const auto instance=GetModuleHandleW(nullptr);
    WNDCLASSW klass{};klass.lpfnWndProc=Procedure;klass.hInstance=instance;
    klass.lpszClassName=L"BeetleworxOnlineConnection";klass.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));
    klass.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);RegisterClassW(&klass);
    Screen screen;screen.executable=executable;screen.content=content;screen.user=user;
    auto window=CreateWindowExW(WS_EX_CONTROLPARENT,klass.lpszClassName,L"Beetleworx — Online co-op (experimental)",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,CW_USEDEFAULT,CW_USEDEFAULT,670,640,nullptr,nullptr,instance,&screen);
    if(!window)return {};
    auto control=[&](const wchar_t* type,const wchar_t* text,int id,int y,int height,DWORD style) {
        auto child=CreateWindowExW(!wcscmp(type,L"EDIT")?WS_EX_CLIENTEDGE:0,type,text,
            WS_CHILD|WS_VISIBLE|style,20,y,610,height,window,reinterpret_cast<HMENU>(INT_PTR(id)),instance,nullptr);
        SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);
        return child;
    };
    control(L"STATIC",L"LAN/direct IPv4 or IPv6. Input buffer trades response time for fewer network stalls. Allow the host port through firewalls; IPv4 NAT needs forwarding.",0,15,34,0);
    control(L"STATIC",L"Host: local IPv4/IPv6 address (editable for direct Internet use)",0,53,20,0);
    auto address=control(L"COMBOBOX",L"",Address,76,180,WS_TABSTOP|CBS_DROPDOWN|CBS_AUTOHSCROLL|WS_VSCROLL);
    SetWindowPos(address,nullptr,20,76,450,180,SWP_NOZORDER);
    for(const auto& ip:LocalAddresses())SendMessageW(address,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(ip.c_str()));
    SendMessageW(address,CB_SETCURSEL,0,0);
    auto port_label=control(L"STATIC",L"TCP port (0 = auto)",0,53,20,0);
    SetWindowPos(port_label,nullptr,490,53,140,20,SWP_NOZORDER);
    auto port=control(L"EDIT",L"0",Port,76,25,WS_TABSTOP|ES_NUMBER|ES_AUTOHSCROLL);
    SetWindowPos(port,nullptr,490,76,140,25,SWP_NOZORDER);
    SendMessageW(port,EM_SETLIMITTEXT,5,0);
    control(L"STATIC",L"Host save (current save selected; copied into a private session)",0,109,20,0);
    auto checkpoint=control(L"EDIT",(user/L"42560816").c_str(),Checkpoint,132,25,WS_TABSTOP|ES_AUTOHSCROLL);
    SetWindowPos(checkpoint,nullptr,20,132,485,25,SWP_NOZORDER);
    auto browse=control(L"BUTTON",L"Browse...",BrowseButton,132,25,WS_TABSTOP);
    SetWindowPos(browse,nullptr,515,132,115,25,SWP_NOZORDER);
    auto host=control(L"BUTTON",L"Host game",HostButton,167,29,WS_TABSTOP);
    SetWindowPos(host,nullptr,20,167,180,29,SWP_NOZORDER);
    auto buffer_label=control(L"STATIC",L"Host input buffer",0,173,20,0);
    SetWindowPos(buffer_label,nullptr,220,173,110,20,SWP_NOZORDER);
    auto buffer=control(L"COMBOBOX",L"",InputBuffer,167,180,WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL);
    SetWindowPos(buffer,nullptr,335,167,295,180,SWP_NOZORDER);
    for(unsigned ticks=0;ticks<=12;++ticks) {
        const auto label=std::to_wstring(ticks)+L" ticks (~"+std::to_wstring((ticks*1000+15)/30)+L" ms buffer)"+
            (ticks==2?L" — default":ticks==0?L" — lowest delay":L"");
        SendMessageW(buffer,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
    }
    SendMessageW(buffer,CB_SETCURSEL,2,0);
    control(L"BUTTON",L"Try automatic Internet connection (compatible router)",RouterOption,207,25,WS_TABSTOP|BS_AUTOCHECKBOX);
    control(L"STATIC",L"LAN and direct invitations are available without automatic router setup.",RouterInfo,238,35,0);
    control(L"STATIC",L"Invitation — paste a friend's code to join",0,281,20,0);
    control(L"EDIT",L"",Invitation,304,25,WS_TABSTOP|ES_AUTOHSCROLL);
    auto join=control(L"BUTTON",L"Join game",JoinButton,339,29,WS_TABSTOP);
    SetWindowPos(join,nullptr,20,339,295,29,SWP_NOZORDER);
    auto copy=control(L"BUTTON",L"Copy invitation",CopyButton,339,29,WS_TABSTOP);
    SetWindowPos(copy,nullptr,335,339,295,29,SWP_NOZORDER);
    control(L"STATIC",L"Find IPv4 games on this LAN (discovery UDP port 41037)",LANStatus,381,20,0);
    auto lan=control(L"COMBOBOX",L"",LANList,407,140,WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL);
    SetWindowPos(lan,nullptr,20,407,450,140,SWP_NOZORDER);
    auto find=control(L"BUTTON",L"Find LAN games",LANFind,407,25,WS_TABSTOP);
    SetWindowPos(find,nullptr,490,407,140,25,SWP_NOZORDER);
    control(L"BUTTON",L"Join selected LAN game",LANJoin,442,29,WS_TABSTOP);
    control(L"STATIC",L"Both players need the same build and game files. Session saves are isolated.",Status,484,60,0);
    control(L"BUTTON",L"Cancel",CancelButton,554,29,WS_TABSTOP);
    SendMessageW(GetDlgItem(window,Invitation),EM_SETLIMITTEXT,256,0);
    SetTimer(window,1,50,nullptr);ShowWindow(window,SW_SHOW);
    screen.FindLAN();
    MSG message{};
    while(IsWindow(window)) {
        const auto got=GetMessageW(&message,nullptr,0,0);
        if(got<=0){screen.boot->Cancel();DestroyWindow(window);if(got==0)PostQuitMessage(int(message.wParam));break;}
        if(!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}
    }
    return std::move(screen.result);
}
}
