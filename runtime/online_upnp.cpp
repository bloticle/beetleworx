#include "online_upnp.h"
#include "online_address.h"
#include <upnp.h>
#include <wrl/client.h>
#include <bcrypt.h>
#include <rex/logging.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace em2::net {
namespace {
using Microsoft::WRL::ComPtr;
using Time=RouterMapping::Time;using namespace std::chrono_literals;
constexpr uint32_t LeaseSeconds=120;
// One outstanding native operation per process, even across cancel/retry. A
// blocking Windows RPC retains its own state until it returns and cleans up.
std::atomic<bool> worker_busy=false;
struct Var {
    VARIANT value{};
    ~Var(){VariantClear(&value);}
    Var()=default;Var(const Var&)=delete;Var& operator=(const Var&)=delete;
};
struct Bstr {
    BSTR value=nullptr;
    Bstr()=default;
    explicit Bstr(const wchar_t* text):value(SysAllocString(text)){}
    ~Bstr(){SysFreeString(value);}
    Bstr(const Bstr&)=delete;Bstr& operator=(const Bstr&)=delete;
};
struct Args:Var {
    bool valid=false;
    explicit Args(unsigned count){value.vt=VT_ARRAY|VT_VARIANT;value.parray=SafeArrayCreateVector(VT_VARIANT,0,count);valid=value.parray!=nullptr;}
    void Put(LONG index,VARIANT item){if(valid)valid=SUCCEEDED(SafeArrayPutElement(value.parray,&index,&item));}
    void Text(LONG index,std::string_view text) {
        std::wstring wide(text.begin(),text.end());Var item;item.value.vt=VT_BSTR;item.value.bstrVal=SysAllocStringLen(wide.data(),UINT(wide.size()));
        if(!item.value.bstrVal){valid=false;return;}Put(index,item.value);
    }
    void Port(LONG index,uint16_t n){VARIANT item{};item.vt=VT_UI2;item.uiVal=n;Put(index,item);}
    void Seconds(LONG index,uint32_t n){VARIANT item{};item.vt=VT_UI4;item.ulVal=n;Put(index,item);}
    void Boolean(LONG index,bool b){VARIANT item{};item.vt=VT_BOOL;item.boolVal=b?VARIANT_TRUE:VARIANT_FALSE;Put(index,item);}
};
std::string Error(const char* action,HRESULT hr) {
    if(hr>=UPNP_E_ACTION_SPECIFIC_BASE && hr<=UPNP_E_ACTION_SPECIFIC_MAX) {
        const auto code=600+uint32_t(hr-UPNP_E_ACTION_SPECIFIC_BASE);
        if(code==725)return "This UPnP router only accepts permanent mappings; use manual forwarding or LAN.";
        return std::string("UPnP ")+action+" refused (code "+std::to_string(code)+"). Use LAN or manual forwarding.";
    }
    return std::string("UPnP ")+action+" failed (Windows code "+std::to_string(uint32_t(hr))+"). Check Windows SSDP/UPnP services or use LAN/manual forwarding.";
}
bool Text(const VARIANT& v,std::string& result) {
    if(v.vt!=VT_BSTR || !v.bstrVal || SysStringLen(v.bstrVal)>2048)return false;
    const auto length=SysStringLen(v.bstrVal);if(std::wstring_view(v.bstrVal,length).find(L'\0')!=std::wstring_view::npos)return false;
    result.clear();if(!length)return true;
    const int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,v.bstrVal,int(length),nullptr,0,nullptr,nullptr);
    if(!bytes)return false;result.resize(bytes);
    return WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,v.bstrVal,int(length),result.data(),bytes,nullptr,nullptr)==bytes;
}
bool Number(const VARIANT& v,uint32_t& result) {
    switch(v.vt) {
    case VT_UI1:result=v.bVal;return true;
    case VT_UI2:result=v.uiVal;return true;
    case VT_UI4:result=v.ulVal;return true;
    case VT_I4:if(v.lVal<0)return false;result=uint32_t(v.lVal);return true;
    case VT_BSTR:{std::string s;if(!Text(v,s) || s.empty())return false;const auto r=std::from_chars(s.data(),s.data()+s.size(),result);return r.ec==std::errc{} && r.ptr==s.data()+s.size();}
    default:return false;
    }
}
bool ArrayCount(VARIANT& array,unsigned expected) {
    if(array.vt!=(VT_ARRAY|VT_VARIANT) || !array.parray || SafeArrayGetDim(array.parray)!=1)return false;
    LONG first=0,last=-1;if(FAILED(SafeArrayGetLBound(array.parray,1,&first)) || FAILED(SafeArrayGetUBound(array.parray,1,&last)) ||
        int64_t(last)-first+1!=expected)return false;
    return true;
}
bool Element(VARIANT& array,unsigned index,unsigned expected,Var& item) {
    if(index>=expected || !ArrayCount(array,expected))return false;
    LONG first=0;if(FAILED(SafeArrayGetLBound(array.parray,1,&first)))return false;
    LONG at=first+LONG(index);return SUCCEEDED(SafeArrayGetElement(array.parray,&at,&item.value));
}
// Accept only an HTTP document fetched from the selected numeric on-link gateway.
// No host-name resolution, credentials, URL escapes in the authority, or redirects
// to a different discovered device are accepted by this selection check.
bool GatewayDocument(std::wstring_view url,std::string_view gateway) {
    if(url.size()>2048 || !url.starts_with(L"http://") || url.find(L'\0')!=url.npos)return false;
    const auto slash=url.find(L'/',7);if(slash==url.npos)return false;
    auto authority=url.substr(7,slash-7);const auto colon=authority.find(L':');auto host=authority.substr(0,colon);
    const std::wstring expected(gateway.begin(),gateway.end());if(host!=expected)return false;
    if(colon!=authority.npos) {
        const auto port=authority.substr(colon+1);if(port.empty() || port.size()>5)return false;
        unsigned n=0;for(auto c:port){if(c<L'0' || c>L'9')return false;n=n*10+unsigned(c-L'0');}if(!n || n>65535)return false;
    }
    return true;
}
template<class Collection,class Interface,class Function>
bool Each(Collection* collection,Function function) {
    ComPtr<IUnknown> unknown;ComPtr<IEnumVARIANT> enumerator;
    if(!collection || FAILED(collection->get__NewEnum(&unknown)) || FAILED(unknown.As(&enumerator)))return false;
    for(unsigned i=0;i<128;++i) {
        Var value;ULONG fetched=0;if(enumerator->Next(1,&value.value,&fetched)!=S_OK || fetched!=1)return false;
        ComPtr<Interface> item;
        if(value.value.vt==VT_DISPATCH && value.value.pdispVal) value.value.pdispVal->QueryInterface(IID_PPV_ARGS(&item));
        else if(value.value.vt==VT_UNKNOWN && value.value.punkVal)value.value.punkVal->QueryInterface(IID_PPV_ARGS(&item));
        if(item && function(item.Get()))return true;
    }
    return false;
}
class WindowsGateway final:public UpnpGateway {
    HRESULT initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    ComPtr<IUPnPService> service;
    unsigned visited=0;
    bool FindService(IUPnPDevice* device,unsigned depth) {
        if(depth>8 || ++visited>256)return false;
        ComPtr<IUPnPServices> services;
        if(SUCCEEDED(device->get_Services(&services)) && Each<IUPnPServices,IUPnPService>(services.Get(),[&](IUPnPService* candidate){
            Bstr type;if(FAILED(candidate->get_ServiceTypeIdentifier(&type.value)) || !type.value)return false;
            const std::wstring_view id(type.value,SysStringLen(type.value));
            if(id==L"urn:schemas-upnp-org:service:WANIPConnection:1" || id==L"urn:schemas-upnp-org:service:WANIPConnection:2" ||
               id==L"urn:schemas-upnp-org:service:WANPPPConnection:1"){service=candidate;return true;}return false;
        }))return true;
        ComPtr<IUPnPDevices> children;
        return SUCCEEDED(device->get_Children(&children)) && Each<IUPnPDevices,IUPnPDevice>(children.Get(),[&](IUPnPDevice* child){return FindService(child,depth+1);});
    }
    HRESULT Invoke(const wchar_t* name,Args& args,Var& output,Var& result) {
        Bstr action(name);Args empty(0);
        if(!action.value || !args.valid || !empty.valid)return E_OUTOFMEMORY;
        // InvokeAction requires an empty output array on entry, even for actions
        // without outputs. Transfer it to the scoped output VARIANT.
        output.value=empty.value;empty.value={};
        return service->InvokeAction(action.value,args.value,&output.value,&result.value);
    }
public:
    WindowsGateway()=default;
    explicit WindowsGateway(IUPnPService* fixture):service(fixture){}
    ~WindowsGateway()override{service.Reset();if(SUCCEEDED(initialized))CoUninitialize();}
    bool Discover(std::string_view gateway,std::string& error)override {
        if(FAILED(initialized)){error=Error("COM initialization",initialized);return false;}
        ComPtr<IUPnPDeviceFinder> finder;auto hr=CoCreateInstance(__uuidof(UPnPDeviceFinder),nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&finder));
        if(FAILED(hr)){error=Error("discovery",hr);return false;}
        Bstr type(L"urn:schemas-upnp-org:device:InternetGatewayDevice:1");ComPtr<IUPnPDevices> devices;
        hr=finder->FindByType(type.value,0,&devices);if(FAILED(hr)){error=Error("discovery",hr);return false;}
        if(Each<IUPnPDevices,IUPnPDevice>(devices.Get(),[&](IUPnPDevice* root){
            ComPtr<IUPnPDeviceDocumentAccess> access;Bstr url;
            return SUCCEEDED(root->QueryInterface(IID_PPV_ARGS(&access))) && SUCCEEDED(access->GetDocumentURL(&url.value)) && url.value &&
                GatewayDocument({url.value,SysStringLen(url.value)},gateway) && FindService(root,0);
        }))return true;
        error="No compatible UPnP service found on the selected gateway. Enable UPnP/Windows SSDP discovery, or use LAN/manual forwarding.";return false;
    }
    bool ExternalAddress(std::string& address,std::string& error)override {
        Args args(0);Var out,result;const auto hr=Invoke(L"GetExternalIPAddress",args,out,result);
        if(FAILED(hr)){error=Error("external address",hr);return false;}
        Var item;if(Element(out.value,0,1,item) && Text(item.value,address))return true;
        // Some service descriptions designate their sole output as a retval.
        if(ArrayCount(out.value,0) && Text(result.value,address))return true;
        error="UPnP external-address response was malformed.";return false;
    }
    UpnpQuery Query(uint16_t external,UpnpEntry& entry,std::string& error)override {
        Args args(3);args.Text(0,"");args.Port(1,external);args.Text(2,"TCP");Var out,result;
        const auto hr=Invoke(L"GetSpecificPortMappingEntry",args,out,result);
        if(hr==UPNP_E_ACTION_SPECIFIC_BASE+(714-600))return UpnpQuery::Missing;
        if(FAILED(hr)){error=Error("mapping lookup",hr);return UpnpQuery::Failed;}
        std::array<Var,5> fields;for(unsigned i=0;i<5;++i)if(!Element(out.value,i,5,fields[i])){error="UPnP mapping response was malformed.";return UpnpQuery::Failed;}
        uint32_t port=0,enabled=0;
        const bool valid=Number(fields[0].value,port) && port && port<=65535 && Text(fields[1].value,entry.client) &&
            Text(fields[3].value,entry.description) && Number(fields[4].value,entry.lifetime);
        if(fields[2].value.vt==VT_BOOL)enabled=fields[2].value.boolVal!=VARIANT_FALSE;
        else if(!Number(fields[2].value,enabled) || enabled>1){error="UPnP mapping state was malformed.";return UpnpQuery::Failed;}
        if(!valid){error="UPnP mapping response was malformed.";return UpnpQuery::Failed;}
        entry.internal=uint16_t(port);entry.enabled=enabled!=0;return UpnpQuery::Found;
    }
    bool Add(uint16_t external,const UpnpEntry& entry,std::string& error)override {
        Args args(8);args.Text(0,"");args.Port(1,external);args.Text(2,"TCP");args.Port(3,entry.internal);args.Text(4,entry.client);
        args.Boolean(5,true);args.Text(6,entry.description);args.Seconds(7,entry.lifetime);Var out,result;
        const auto hr=Invoke(L"AddPortMapping",args,out,result);if(FAILED(hr)){error=Error("temporary mapping",hr);return false;}return true;
    }
    bool Delete(uint16_t external,std::string& error)override {
        Args args(3);args.Text(0,"");args.Port(1,external);args.Text(2,"TCP");Var out,result;
        const auto hr=Invoke(L"DeletePortMapping",args,out,result);if(FAILED(hr)){error=Error("cleanup",hr);return false;}return true;
    }
};
struct Shared {
    std::mutex mutex;std::condition_variable changed;
    bool stop=false,renew=false,finished=false;
    RouterStatus status{RouterState::Discovering};
    uint64_t generation=0;uint32_t lifetime=0;Time issued{};
};
bool Owned(const UpnpEntry& actual,const UpnpEntry& wanted) {
    return actual.client==wanted.client && actual.internal==wanted.internal && actual.description==wanted.description;
}
bool Cancelled(const std::shared_ptr<Shared>& state){std::lock_guard lock(state->mutex);return state->stop;}
void Failed(const std::shared_ptr<Shared>& state,std::string error) {
    std::lock_guard lock(state->mutex);state->status={RouterState::Failed,{},std::move(error)};
}
void Worker(std::shared_ptr<Shared> state,std::string gateway,UpnpEntry wanted,Time issued,UpnpFactory factory,uint16_t seed) {
    struct Finished {
        std::shared_ptr<Shared> state;
        ~Finished(){worker_busy=false;{std::lock_guard lock(state->mutex);state->finished=true;}state->changed.notify_all();}
    } finished{state};
    try {
        if(Cancelled(state))return;
        auto backend=factory();if(!backend){Failed(state,"UPnP backend unavailable.");return;}
        uint16_t external=0;
        struct Cleanup {
            UpnpGateway& backend;const UpnpEntry& wanted;uint16_t& external;
            ~Cleanup(){try{UpnpEntry actual;std::string error;if(external && backend.Query(external,actual,error)==UpnpQuery::Found && Owned(actual,wanted))backend.Delete(external,error);}catch(...){}}
        } cleanup{*backend,wanted,external};
        std::string error,address;
        if(Cancelled(state))return;
        if(!backend->Discover(gateway,error)){Failed(state,error);return;}
        if(Cancelled(state))return;
        if(!backend->ExternalAddress(address,error)){Failed(state,error);return;}
        const auto public_address=ParseNumericAddress(address);
        if(!public_address || !PublicRouterV4(*public_address)){Failed(state,"UPnP gateway has no public IPv4 address. A VPN or relay may be needed.");return;}
        for(unsigned attempt=0;attempt<8;++attempt) {
            if(Cancelled(state))return;
            const uint16_t candidate=attempt?uint16_t(49152+(unsigned(seed)+257*attempt)%16384):wanted.internal;
            UpnpEntry existing;const auto found=backend->Query(candidate,existing,error);
            if(found==UpnpQuery::Failed){Failed(state,error);return;}
            if(found==UpnpQuery::Missing){external=candidate;break;}
        }
        if(!external){Failed(state,"UPnP external ports are occupied. Existing mappings were preserved; use manual forwarding.");return;}
        for(;;) {
            if(Cancelled(state))return;
            {std::lock_guard lock(state->mutex);if(!state->generation)state->status.state=RouterState::Mapping;}
            // Set external before Add so a lost ACK/cancellation still performs
            // an ownership-checked cleanup, rather than leaking the lease.
            if(!backend->Add(external,wanted,error)){Failed(state,error);return;}
            if(Cancelled(state))return;
            UpnpEntry actual;
            if(backend->Query(external,actual,error)!=UpnpQuery::Found){Failed(state,error.empty()?"UPnP temporary mapping could not be verified.":error);return;}
            if(!Owned(actual,wanted) || !actual.enabled || !actual.lifetime || actual.lifetime>LeaseSeconds){Failed(state,"UPnP router did not confirm the requested temporary mapping.");return;}
            if(Cancelled(state))return;
            if(!backend->ExternalAddress(address,error)){Failed(state,error);return;}
            const auto current=ParseNumericAddress(address);
            if(!current || !PublicRouterV4(*current)){Failed(state,"UPnP public address changed to an unusable endpoint.");return;}
            {
                std::unique_lock lock(state->mutex);if(state->stop)return;
                state->status={RouterState::Active,address,{},external};state->lifetime=actual.lifetime;state->issued=issued;++state->generation;
                state->changed.notify_all();state->changed.wait(lock,[&]{return state->stop || state->renew;});
                if(state->stop)return;issued=state->issued;state->renew=false;
            }
            const auto query=backend->Query(external,actual,error);
            if(query!=UpnpQuery::Found || !Owned(actual,wanted)) {
                Failed(state,query==UpnpQuery::Failed?error:"UPnP mapping ownership changed. It will not be overwritten or deleted.");return;
            }
        }
    } catch(...) {Failed(state,"UPnP setup failed. LAN and manual invitations still work.");}
}
}
std::unique_ptr<UpnpGateway> MakeNativeUpnpGateway(){return std::make_unique<WindowsGateway>();}
bool UpnpWorkerIdleForTest(){return !worker_busy.load();}
bool TestNativeUpnpAutomation() {
    bool ok=true;unsigned checks=0;
    auto check=[&](bool value,const char* label){++checks;if(!value){ok=false;REXLOG_ERROR("UPnP automation: {}",label);}return value;};
    struct Service final:IUPnPService {
        ULONG refs=1;std::function<HRESULT(BSTR,VARIANT,VARIANT*,VARIANT*)> action;
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out)override{if(!out)return E_POINTER;*out=nullptr;if(iid==__uuidof(IUnknown) || iid==__uuidof(IDispatch) || iid==__uuidof(IUPnPService)){*out=this;AddRef();return S_OK;}return E_NOINTERFACE;}
        ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}
        ULONG STDMETHODCALLTYPE Release()override{return --refs;}
        HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT*)override{return E_NOTIMPL;}
        HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT,LCID,ITypeInfo**)override{return E_NOTIMPL;}
        HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID,LPOLESTR*,UINT,LCID,DISPID*)override{return E_NOTIMPL;}
        HRESULT STDMETHODCALLTYPE Invoke(DISPID,REFIID,LCID,WORD,DISPPARAMS*,VARIANT*,EXCEPINFO*,UINT*)override{return E_NOTIMPL;}
        HRESULT STDMETHODCALLTYPE QueryStateVariable(BSTR,VARIANT*)override{return E_NOTIMPL;}
        HRESULT STDMETHODCALLTYPE InvokeAction(BSTR name,VARIANT in,VARIANT* out,VARIANT* result)override{return action(name,in,out,result);}
        HRESULT STDMETHODCALLTYPE get_ServiceTypeIdentifier(BSTR*)override{return E_NOTIMPL;}
        HRESULT STDMETHODCALLTYPE AddCallback(IUnknown*)override{return E_NOTIMPL;}
        HRESULT STDMETHODCALLTYPE get_Id(BSTR*)override{return E_NOTIMPL;}
        HRESULT STDMETHODCALLTYPE get_LastTransportStatus(long*)override{return E_NOTIMPL;}
    } service;
    {
        WindowsGateway backend(&service);std::string error,address;
        auto shape=[&](VARIANT in,unsigned n,VARIANT* out){return ArrayCount(in,n) && out && ArrayCount(*out,0);};
        auto matches=[&](VARIANT in,unsigned index,unsigned n,VARTYPE type,std::string_view text,uint32_t number){Var item;if(!Element(in,index,n,item) || item.value.vt!=type)return false;std::string s;uint32_t v=0;return type==VT_BSTR?(Text(item.value,s) && s==text):(Number(item.value,v) && v==number);};
        service.action=[&](BSTR name,VARIANT in,VARIANT* out,VARIANT*){
            check(std::wstring_view(name)==L"GetExternalIPAddress" && shape(in,0,out),"external-address empty input/output arrays");
            Args reply(1);reply.Text(0,"8.8.8.8");VariantClear(out);*out=reply.value;reply.value={};return S_OK;
        };
        check(backend.ExternalAddress(address,error) && address=="8.8.8.8","external-address output decoded");
        UpnpEntry entry{"192.168.1.50","EM2Recomp fixture",45001,120,true};
        service.action=[&](BSTR name,VARIANT in,VARIANT* out,VARIANT*){
            Var enabled;bool fields=Element(in,5,8,enabled) && enabled.value.vt==VT_BOOL && enabled.value.boolVal==VARIANT_TRUE;
            fields=fields && matches(in,0,8,VT_BSTR,"",0) && matches(in,1,8,VT_UI2,"",41234) && matches(in,2,8,VT_BSTR,"TCP",0) &&
                matches(in,3,8,VT_UI2,"",45001) && matches(in,4,8,VT_BSTR,"192.168.1.50",0) && matches(in,6,8,VT_BSTR,"EM2Recomp fixture",0) && matches(in,7,8,VT_UI4,"",120);
            check(std::wstring_view(name)==L"AddPortMapping" && shape(in,8,out) && fields,"exact TCP, client, port, enabled flag, description and 120-second lease marshalling");return S_OK;
        };
        check(backend.Add(41234,entry,error),"native Add success");
        service.action=[&](BSTR name,VARIANT in,VARIANT* out,VARIANT*){
            check(std::wstring_view(name)==L"GetSpecificPortMappingEntry" && shape(in,3,out) && matches(in,0,3,VT_BSTR,"",0) && matches(in,1,3,VT_UI2,"",41234) && matches(in,2,3,VT_BSTR,"TCP",0),"exact mapping lookup tuple");
            Args reply(5);reply.Port(0,45001);reply.Text(1,"192.168.1.50");reply.Boolean(2,true);reply.Text(3,"EM2Recomp fixture");reply.Seconds(4,119);
            VariantClear(out);*out=reply.value;reply.value={};return S_OK;
        };
        UpnpEntry actual;check(backend.Query(41234,actual,error)==UpnpQuery::Found && Owned(actual,entry) && actual.enabled && actual.lifetime==119,"five output fields decoded");
        service.action=[](BSTR,VARIANT,VARIANT*,VARIANT*){return UPNP_E_ACTION_SPECIFIC_BASE+114;};
        check(backend.Query(41234,actual,error)==UpnpQuery::Missing,"714 alone means missing entry");
        service.action=[](BSTR,VARIANT,VARIANT*,VARIANT*){return UPNP_E_DEVICE_TIMEOUT;};
        check(backend.Query(41234,actual,error)==UpnpQuery::Failed,"transport failure never treated as unoccupied port");
        service.action=[](BSTR,VARIANT,VARIANT*,VARIANT*){return UPNP_E_ACTION_SPECIFIC_BASE+125;};
        check(!backend.Add(41234,entry,error) && error.find("permanent")!=error.npos,"725 refuses permanent-lease fallback");
        service.action=[&](BSTR name,VARIANT in,VARIANT* out,VARIANT*){
            check(std::wstring_view(name)==L"DeletePortMapping" && shape(in,3,out) && matches(in,0,3,VT_BSTR,"",0) && matches(in,1,3,VT_UI2,"",41234) && matches(in,2,3,VT_BSTR,"TCP",0),"deletion is exact external TCP tuple");return S_OK;
        };
        check(backend.Delete(41234,error),"native Delete success");
        service.action=[](BSTR,VARIANT,VARIANT* out,VARIANT*){Args reply(4);VariantClear(out);*out=reply.value;reply.value={};return S_OK;};
        check(backend.Query(41234,actual,error)==UpnpQuery::Failed,"truncated successful lookup rejected");
        service.action=[](BSTR,VARIANT,VARIANT*,VARIANT* result){result->vt=VT_BSTR;result->bstrVal=SysAllocString(L"8.8.4.4");return S_OK;};
        check(backend.ExternalAddress(address,error) && address=="8.8.4.4","sole retval supported for external address");
    }
    check(service.refs==1,"native service reference released");
    check(GatewayDocument(L"http://192.168.1.1:5000/root.xml","192.168.1.1"),"numeric selected-gateway description accepted");
    for(auto url:{L"https://192.168.1.1/root.xml",L"http://192.168.1.2/root.xml",L"http://192.168.1.1.evil/root.xml",L"http://192.168.1.1@8.8.8.8/root.xml",L"http://router/root.xml",L"http://192.168.1.1:0/root.xml",L"http://192.168.1.1:65536/root.xml",L"http://192.168.1.1%2f@8.8.8.8/root.xml"})
        check(!GatewayDocument(url,"192.168.1.1"),"unselected/malformed/hostname gateway document rejected");
    if(ok)REXLOG_INFO("UPnP native automation passed {} checks: exact COM action tuples/types, output/error handling and gateway-document selection; no discovery or router contacted",checks);
    return ok;
}
struct UpnpLease::Impl {
    RouterStatus status;std::shared_ptr<Shared> shared;
    uint64_t generation=0;bool pending=false;Time deadline{},renew{},expires{};
};
UpnpLease::UpnpLease():impl_(std::make_unique<Impl>()){}
UpnpLease::~UpnpLease(){Stop();}
const RouterStatus& UpnpLease::status()const{return impl_->status;}
void UpnpLease::Stop(){auto& p=*impl_;if(p.shared){{std::lock_guard lock(p.shared->mutex);p.shared->stop=true;}p.shared->changed.notify_all();p.shared.reset();}p.status={};p.generation=0;p.pending=false;}
bool UpnpLease::Start(std::string local,std::string gateway,uint16_t internal,Time now,UpnpFactory factory) {
    Stop();auto& p=*impl_;
    auto fail=[&](std::string error){p.status={RouterState::Failed,{},std::move(error)};return false;};
    const auto source=ParseNumericAddress(local),target=ParseNumericAddress(gateway);
    if(!source || !target || source->ipv6() || target->ipv6() || !internal || !factory)return fail("UPnP endpoint unavailable.");
    std::array<uint8_t,12> nonce{};
    if(BCryptGenRandom(nullptr,nonce.data(),ULONG(nonce.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)return fail("Cannot create UPnP mapping identity.");
    UpnpEntry wanted{local,"beetleworx ",internal,LeaseSeconds,true};constexpr char hex[]="0123456789abcdef";
    for(auto b:nonce){wanted.description+=hex[b>>4];wanted.description+=hex[b&15];}
    bool idle=false;if(!worker_busy.compare_exchange_strong(idle,true))return fail("The previous UPnP operation is still finishing. Retry automatic setup shortly; LAN remains available.");
    try {
        p.shared=std::make_shared<Shared>();p.status={RouterState::Discovering};p.pending=true;p.deadline=now+30s;
        std::thread(Worker,p.shared,std::move(gateway),std::move(wanted),now,std::move(factory),uint16_t(unsigned(nonce[0])*256+nonce[1])).detach();
    }catch(...){p.shared.reset();worker_busy=false;return fail("Cannot start UPnP setup worker.");}
    return true;
}
void UpnpLease::Poll(Time now) {
    auto& p=*impl_;if(!p.shared || p.status.state==RouterState::Failed)return;
    auto fail=[&](const char* error){p.status={RouterState::Failed,{},error};std::lock_guard lock(p.shared->mutex);p.shared->stop=true;p.shared->changed.notify_all();};
    {
        std::lock_guard lock(p.shared->mutex);p.status=p.shared->status;
        if(p.generation!=p.shared->generation) {
            p.generation=p.shared->generation;p.pending=false;
            p.expires=p.shared->issued+std::chrono::seconds(p.shared->lifetime);
            p.renew=p.shared->issued+std::chrono::milliseconds(p.shared->lifetime*500);
        }
    }
    if(p.status.state==RouterState::Failed)return;
    if(p.generation && now>=p.expires){fail("UPnP temporary mapping expired. LAN and manual invitations still work.");return;}
    if(p.pending && now>=p.deadline){fail("UPnP router setup timed out. Cleanup will finish in the background; LAN remains available.");return;}
    if(p.generation && !p.pending && now>=p.renew) {
        std::lock_guard lock(p.shared->mutex);p.shared->issued=now;p.shared->renew=true;p.pending=true;p.deadline=now+30s;p.shared->changed.notify_all();
    }
}
}
