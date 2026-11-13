#include "demo_support.h"
#include "demo_support_json.h"
#include "em1_deflate.h"
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <bcrypt.h>
#define Z_SOLO
#define Z_PREFIX
#include "em1_compat/zlib/zlib.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <mutex>
#include <regex>
#include <set>
#include <span>
#include <sstream>
#include <thread>
#include <optional>
#include <memory>
#include <cstring>
#include <cstdlib>
namespace em2 { namespace {
namespace fs=std::filesystem;
using namespace demo_detail;
using Bytes=std::vector<uint8_t>;
constexpr uint64_t max_bundle=1024ull*1024*1024,max_payload=2ull*1024*1024*1024,max_file=512ull*1024*1024;
constexpr const char* api="https://api.github.com/repos/Bloticle/beetleworx/releases?per_page=100";
std::atomic<bool> stopping{false};
thread_local bool background_task=false;
void CancelCheck(){if(background_task&&stopping.load())throw std::runtime_error("Background update cancelled at shutdown");}
void UpdateProgress(uint64_t bytes);
void PlainAbsolute(const fs::path&p);
void Need(bool b,const std::string& why){if(!b)throw std::runtime_error(why);}
std::string Lower(std::string s){for(auto&c:s)if(c>='A'&&c<='Z')c+=32;return s;}
bool PreferenceValue(const std::string&command,bool current){if(command.empty())return !current;auto value=Lower(command);if(value=="on"||value=="true"||value=="1")return true;if(value=="off"||value=="false"||value=="0")return false;throw std::runtime_error("Choose on or off for automatic updates");}
std::wstring Wide(std::string_view s){if(s.empty())return{};const auto n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),nullptr,0);Need(n>0,"Invalid UTF-8 text");std::wstring v(n,0);MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),v.data(),n);return v;}
std::string Utf8(std::wstring_view s){if(s.empty())return{};auto n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),int(s.size()),nullptr,0,nullptr,nullptr);Need(n>0,"Invalid Windows text");std::string v(n,0);WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),int(s.size()),v.data(),n,nullptr,nullptr);return v;}
std::string PathText(const fs::path&p){return Utf8(p.native());}
std::wstring Env(const wchar_t*key){auto n=GetEnvironmentVariableW(key,nullptr,0);if(!n||n>32768)return{};std::wstring value(n,0);auto size=GetEnvironmentVariableW(key,value.data(),n);if(!size||size>=n)return{};value.resize(size);return value;}
std::string PrintableUtf8(std::string text){if(text.empty())return{};for(auto&c:text)if(static_cast<unsigned char>(c)<32&&c!='\r'&&c!='\n'&&c!='\t')c='?';auto n=MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),nullptr,0);if(n<=0){for(auto&c:text)if(static_cast<unsigned char>(c)>=127)c='?';return text;}std::wstring clean(n,0);MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),clean.data(),n);for(auto&c:clean)if(c>=0x7F&&c<=0x9F)c=L'?';return Utf8(clean);}
std::string Hex(std::span<const uint8_t> bytes){std::string s;for(auto b:bytes){s+="0123456789abcdef"[b>>4];s+="0123456789abcdef"[b&15];}return s;}
bool Digest(std::string_view s){return s.size()==64&&std::all_of(s.begin(),s.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F');});}
struct Sha {
 BCRYPT_HASH_HANDLE handle=nullptr;
 Sha(){Need(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE,&handle,nullptr,0,nullptr,0,0)>=0,"Cannot initialize SHA-256");}
 ~Sha(){if(handle)BCryptDestroyHash(handle);}
 void Add(std::span<const uint8_t> b){Need(b.size()<=ULONG_MAX&&BCryptHashData(handle,const_cast<PUCHAR>(b.data()),ULONG(b.size()),0)>=0,"SHA-256 failed");}
 std::string End(){std::array<uint8_t,32>b{};Need(BCryptFinishHash(handle,b.data(),ULONG(b.size()),0)>=0,"SHA-256 failed");return Hex(b);}
};
std::string Hash(std::span<const uint8_t>b){Sha h;h.Add(b);return h.End();}
std::string HashFile(const fs::path&p){std::ifstream f(p,std::ios::binary);Need(bool(f),"Cannot read release file for verification");Sha h;std::array<uint8_t,65536>b{};while(f){CancelCheck();f.read(reinterpret_cast<char*>(b.data()),b.size());auto n=f.gcount();if(n)h.Add(std::span(b.data(),size_t(n)));}Need(f.eof(),"Release file read failed");return h.End();}
Bytes Read(const fs::path&p,uint64_t limit){auto n=fs::file_size(p);Need(n<=limit,"File exceeds supported size");Bytes b(size_t(n),0);std::ifstream f(p,std::ios::binary);Need(bool(f.read(reinterpret_cast<char*>(b.data()),b.size())),"Cannot read file");return b;}
std::string String(const Bytes&b){return std::string(b.begin(),b.end());}
void Write(const fs::path&p,std::span<const uint8_t>b){fs::create_directories(p.parent_path());std::ofstream f(p,std::ios::binary|std::ios::trunc);Need(bool(f.write(reinterpret_cast<const char*>(b.data()),b.size()).flush()),"Cannot write support file");}
void WriteText(const fs::path&p,std::string_view s){Write(p,std::span(reinterpret_cast<const uint8_t*>(s.data()),s.size()));}
bool Reparse(const fs::path&p){auto a=GetFileAttributesW(p.c_str());return a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_REPARSE_POINT);}
void PlainTree(const fs::path&root,const fs::path&relative){Need(!Reparse(root),"Update directory is a symbolic link/reparse point");auto p=root;for(auto&part:relative){p/=part;Need(!Reparse(p),"Update path contains a symbolic link/reparse point");}}
std::string SafePath(std::string path){
 Need(!path.empty()&&path.size()<240&&path.front()!='/'&&path.front()!='\\'&&path.find('\\')==std::string::npos,"Unsafe update path");
 std::string lower=Lower(path);size_t at=0;while(at<=path.size()){auto end=path.find('/',at);if(end==std::string::npos)end=path.size();auto part=lower.substr(at,end-at);Need(!part.empty()&&part!="."&&part!=".."&&part.back()!='.'&&part.back()!=' ',"Unsafe update path component");
  for(unsigned char c:part)Need(c>=32&&c<127&&std::string_view(":*?\"<>|").find(char(c))==std::string_view::npos,"Unsafe update filename");
  auto stem=part.substr(0,part.find('.'));Need(stem!="con"&&stem!="prn"&&stem!="aux"&&stem!="nul"&&!(stem.size()==4&&(stem.starts_with("com")||stem.starts_with("lpt"))&&stem[3]>='0'&&stem[3]<='9'),"Reserved Windows update filename");
  if(at==0)Need(part!="user_data"&&part!="userdata"&&part!="saves"&&part!="mods"&&part!="reports"&&part!="screenshots"&&part!="game"&&part!="game-data"&&part!="roms"&&part!=".beetleworx-update","Update cannot replace user/game data");if(end==path.size())break;at=end+1;
 }
 return path;
}
struct Version {uint64_t a=0,b=0,c=0;std::vector<std::string> pre;};
Version Semver(std::string s){Need(s.size()<128,"Release version is too long");std::smatch m;static const std::regex re(R"(^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$)");Need(std::regex_match(s,m,re),"Release has no valid semantic version");Version v{std::stoull(m[1]),std::stoull(m[2]),std::stoull(m[3]),{}};std::stringstream p(m[4]);std::string x;while(std::getline(p,x,'.')){bool number=std::all_of(x.begin(),x.end(),[](char c){return c>='0'&&c<='9';});Need(!number||x.size()==1||x.front()!='0',"Invalid numeric prerelease identifier");v.pre.push_back(x);}return v;}
int Compare(const Version&a,const Version&b){for(auto pair:{std::pair(a.a,b.a),std::pair(a.b,b.b),std::pair(a.c,b.c)})if(pair.first!=pair.second)return pair.first<pair.second?-1:1;if(a.pre.empty()!=b.pre.empty())return a.pre.empty()?1:-1;for(size_t i=0;i<std::min(a.pre.size(),b.pre.size());++i){auto x=a.pre[i],y=b.pre[i];if(x==y)continue;auto num=[](auto&s){return std::all_of(s.begin(),s.end(),[](char c){return c>='0'&&c<='9';});};bool nx=num(x),ny=num(y);if(nx!=ny)return nx?-1:1;if(nx&&x.size()!=y.size())return x.size()<y.size()?-1:1;return x<y?-1:1;}return a.pre.size()==b.pre.size()?0:a.pre.size()<b.pre.size()?-1:1;}
struct File {std::string path,sha;uint64_t size;};
struct Manifest {std::string version,tag,exe_sha,raw;std::vector<File> files;};
Manifest ManifestFrom(std::string text){auto j=ParseJson(text);Need(j.At("format").UInt()==2,"Unsupported release manifest format");Need(j.At("product").Str()=="Beetleworx"&&j.At("executable").Str()=="beetleworx.exe","Release product/executable does not match Beetleworx");Manifest m;m.raw=std::move(text);m.version=j.At("version").Str();Semver(m.version);m.tag=j.At("tag").Str();Need(m.tag==m.version||m.tag=="v"+m.version,"Release tag and version disagree");m.exe_sha=Lower(j.At("exe_sha256").Str());Need(Digest(m.exe_sha),"Invalid release executable digest");auto&a=j.At("files");Need(a.kind==Json::Array&&!a.array.empty()&&a.array.size()<=20000,"Invalid release file inventory");std::set<std::string> names;uint64_t total=0;bool exe=false;for(auto&r:a.array){File f{SafePath(r.At("path").Str()),Lower(r.At("sha256").Str()),r.At("bytes").UInt()};Need(f.size<=max_file&&total<=max_payload-f.size&&Digest(f.sha),"Invalid update payload size/digest");total+=f.size;Need(names.insert(Lower(f.path)).second&&Lower(f.path)!="release-manifest.json"&&Lower(f.path)!="update-stage-owner.json","Duplicate/reserved release payload path");if(Lower(f.path)=="beetleworx.exe"){Need(f.path=="beetleworx.exe"&&f.sha==m.exe_sha,"Executable digest disagrees with manifest");exe=true;}m.files.push_back(std::move(f));}Need(exe,"Release is missing beetleworx.exe");return m;}
void VerifyTree(const fs::path&root,const Manifest&m){for(auto&f:m.files){CancelCheck();PlainTree(root,fs::path(f.path));auto p=root/fs::path(f.path);Need(fs::is_regular_file(p)&&fs::file_size(p)==f.size&&HashFile(p)==f.sha,"Release verification failed: "+f.path);}}
struct Response {unsigned status=0;Bytes bytes;};
using Fetch=std::function<Response(const std::string&,uint64_t)>;
struct Internet {std::atomic<HINTERNET> h{nullptr};~Internet(){if(auto p=h.exchange(nullptr))WinHttpCloseHandle(p);}operator HINTERNET()const{return h.load();}};
// Synchronous WinHTTP handles are owned and closed only by this worker.
// Closing a handle from the shutdown/UI thread during a blocking call is not
// supported by WinHTTP. Cancellation is cooperative at bounded phase/chunk
// boundaries; configured DNS/connect/send/receive timeouts are 3/3/3/5s.
bool AllowedUrl(const std::string&url,bool api_only=false){if(url.size()>2048||!url.starts_with("https://")||url.find_first_of("\r\n\t\\")!=std::string::npos)return false;auto host=url.substr(8,url.find('/',8)-8);host=Lower(host);return host=="api.github.com"||(!api_only&&(host=="github.com"||host=="release-assets.githubusercontent.com"||host=="objects.githubusercontent.com"));}
Response Http(std::string url,uint64_t limit){
 Internet session{WinHttpOpen(L"Beetleworx-demo-updater/1.0",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0)};Need(session.h.load()!=nullptr,"Cannot start secure update connection");Need(WinHttpSetTimeouts(session,3000,3000,3000,5000)!=0,"Cannot configure bounded update timeouts");DWORD retries=1;Need(WinHttpSetOption(session,WINHTTP_OPTION_CONNECT_RETRIES,&retries,sizeof(retries))!=0,"Cannot configure update connection retries");
 for(unsigned redirects=0;redirects<6;++redirects){Need(!stopping.load(),"Background update cancelled at shutdown");Need(AllowedUrl(url),"Update URL is not an approved HTTPS GitHub host");auto wide=Wide(url);URL_COMPONENTS u{};u.dwStructSize=sizeof(u);u.dwHostNameLength=u.dwUrlPathLength=u.dwExtraInfoLength=DWORD(-1);Need(WinHttpCrackUrl(wide.c_str(),DWORD(wide.size()),0,&u)!=0&&u.nScheme==INTERNET_SCHEME_HTTPS&&u.nPort==443,"Invalid secure update URL");std::wstring host(u.lpszHostName,u.dwHostNameLength),path(u.lpszUrlPath,u.dwUrlPathLength);if(u.dwExtraInfoLength)path.append(u.lpszExtraInfo,u.dwExtraInfoLength);
  Internet connection{WinHttpConnect(session,host.c_str(),443,0)};Need(connection.h.load()!=nullptr,"Cannot connect to GitHub");Internet request{WinHttpOpenRequest(connection,L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE)};Need(request.h.load()!=nullptr,"Cannot create update request");DWORD policy=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;Need(WinHttpSetOption(request,WINHTTP_OPTION_REDIRECT_POLICY,&policy,sizeof(policy))!=0,"Cannot restrict update redirects");const wchar_t*headers=L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";Need(WinHttpSendRequest(request,headers,DWORD(-1),WINHTTP_NO_REQUEST_DATA,0,0,0)!=0,"Cannot send GitHub update request");Need(!stopping.load(),"Background update cancelled at shutdown");Need(WinHttpReceiveResponse(request,nullptr)!=0,"Cannot receive GitHub update response");DWORD status=0,n=sizeof(status);Need(WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&n,nullptr)!=0,"Missing HTTP status");
  if(status>=300&&status<400){DWORD size=0;WinHttpQueryHeaders(request,WINHTTP_QUERY_LOCATION,nullptr,nullptr,&size,nullptr);Need(size>2&&size<8192,"Invalid update redirect");std::wstring location(size/2,0);Need(WinHttpQueryHeaders(request,WINHTTP_QUERY_LOCATION,nullptr,location.data(),&size,nullptr)!=0,"Cannot read update redirect");while(!location.empty()&&!location.back())location.pop_back();url=Utf8(location);continue;}
  Response out{status,{}};for(;;){Need(!stopping.load(),"Background update cancelled at shutdown");DWORD available=0;Need(WinHttpQueryDataAvailable(request,&available)!=0,"Update download interrupted");if(!available)break;Need(!stopping.load(),"Background update cancelled at shutdown");Need(available<=limit-out.bytes.size(),"Update response exceeds supported size");auto begin=out.bytes.size();out.bytes.resize(begin+available);DWORD received=0;Need(WinHttpReadData(request,out.bytes.data()+begin,available,&received)!=0&&received,"Update download interrupted");out.bytes.resize(begin+received);if(limit==max_bundle)UpdateProgress(out.bytes.size());}return out;
 }throw std::runtime_error("Too many GitHub redirects");
}
struct Release {std::string version,tag,url,sha;uint64_t bytes=0;};
std::optional<Release> SelectRelease(const Response&r,const Version&current){if(r.status==404)return{};Need(r.status==200,r.status==403||r.status==429?"GitHub update checks are rate limited. Try again later.":"GitHub release service is unavailable");auto j=ParseJson(String(r.bytes));Need(j.kind==Json::Array,"Invalid GitHub release list");std::optional<Release> best;for(auto&entry:j.array){if(entry.At("draft").Boolean())continue;auto tag=entry.At("tag_name").Str();auto v=tag.starts_with('v')?tag.substr(1):tag;Version ver;try{ver=Semver(v);}catch(...){continue;}if(Compare(ver,current)<=0||(best&&Compare(ver,Semver(best->version))<=0))continue;auto&assets=entry.At("assets");Need(assets.kind==Json::Array,"Invalid release assets");for(auto&a:assets.array){auto name=a.At("name").Str();if(!Lower(name).ends_with(".zip")||!Lower(name).starts_with("beetleworx"))continue;auto d=a.Find("digest");if(!d||d->kind!=Json::String||!d->text.starts_with("sha256:")||!Digest(d->text.substr(7)))continue;Release found{v,tag,a.At("browser_download_url").Str(),Lower(d->text.substr(7)),a.At("size").UInt()};Need(AllowedUrl(found.url)&&Lower(found.url).starts_with("https://github.com/bloticle/beetleworx/releases/download/"),"Release asset origin is not Bloticle/beetleworx");Need(found.bytes&&found.bytes<=max_bundle,"Release ZIP exceeds supported size");best=std::move(found);break;}}return best;}
uint16_t U16(std::span<const uint8_t>b,size_t a){Need(a<=b.size()&&b.size()-a>=2,"Truncated update ZIP");return b[a]|uint16_t(b[a+1])<<8;}
uint32_t U32(std::span<const uint8_t>b,size_t a){return U16(b,a)|uint32_t(U16(b,a+2))<<16;}
uint32_t Crc(std::span<const uint8_t>b){static const auto table=[] {std::array<uint32_t,256> t{};for(uint32_t i=0;i<256;++i){auto n=i;for(int bit=0;bit<8;++bit)n=(n>>1)^(0xEDB88320u&uint32_t(-int32_t(n&1)));t[i]=n;}return t;}();uint32_t n=~0u;for(auto c:b)n=(n>>8)^table[(n^c)&255];return~n;}
struct ZipEntry {std::string name;uint32_t offset,packed,size,crc;uint16_t method;};
std::vector<ZipEntry> ZipIndex(std::span<const uint8_t>b){Need(b.size()>=22&&b.size()<=max_bundle,"Invalid update ZIP size");size_t e=b.size()-22;for(;;){if(U32(b,e)==0x06054B50&&e+22+U16(b,e+20)==b.size())break;Need(e>0&&b.size()-e<=65557,"Missing ZIP directory");--e;}Need(!U16(b,e+4)&&!U16(b,e+6)&&U16(b,e+8)==U16(b,e+10),"Split update ZIP is unsupported");auto n=U16(b,e+10);auto size=U32(b,e+12),at=U32(b,e+16);Need(n&&n<20002&&uint64_t(at)+size==e,"Invalid ZIP directory bounds/ZIP64");std::vector<ZipEntry> out;std::set<std::string> names;size_t cursor=at;uint64_t total=0;for(unsigned i=0;i<n;++i){Need(U32(b,cursor)==0x02014B50,"Invalid ZIP entry");auto flags=U16(b,cursor+8),method=U16(b,cursor+10);Need((flags&~uint16_t(0x0808))==0&&(method==0||method==8),"Encrypted/unsupported ZIP entry");auto packed=U32(b,cursor+20),unpacked=U32(b,cursor+24);auto len=U16(b,cursor+28),extra=U16(b,cursor+30),comment=U16(b,cursor+32);Need(cursor+46ull+len+extra+comment<=e&&len&&U16(b,cursor+34)==0,"Invalid ZIP entry dimensions");auto attr=U32(b,cursor+38);Need(((attr>>16)&0170000)!=0120000,"ZIP symbolic links are unsupported");std::string name(reinterpret_cast<const char*>(b.data()+cursor+46),len);auto local=U32(b,cursor+42);auto crc=U32(b,cursor+16);cursor+=46+len+extra+comment;
  if(name.ends_with('/')){SafePath(name.substr(0,name.size()-1));Need(!packed&&!unpacked,"Invalid ZIP directory entry");continue;}SafePath(name);Need(names.insert(Lower(name)).second,"Duplicate ZIP path");Need(unpacked<=max_file&&total<=max_payload-unpacked,"ZIP payload exceeds supported size");total+=unpacked;
  Need(uint64_t(local)+30<=at&&U32(b,local)==0x04034B50&&U16(b,local+8)==method&&U16(b,local+6)==flags,"ZIP local/central headers disagree");auto ln=U16(b,local+26),le=U16(b,local+28);Need(uint64_t(local)+30+ln+le+packed<=at&&ln==len&&std::string_view(reinterpret_cast<const char*>(b.data()+local+30),ln)==name,"ZIP payload overlap/name mismatch");out.push_back({name,uint32_t(local+30+ln+le),packed,unpacked,crc,method});}
 Need(cursor==e,"ZIP directory length mismatch");return out;}
Bytes Unzip(std::span<const uint8_t>b,const ZipEntry&e){auto src=b.subspan(e.offset,e.packed);Bytes out(e.size);if(!e.method){Need(e.packed==e.size,"Stored ZIP size mismatch");std::copy(src.begin(),src.end(),out.begin());}else{z_stream z{};z.zalloc=[](voidpf,uInt n,uInt s)->voidpf{if(!n||!s||uint64_t(n)*s>1048576)return nullptr;return std::calloc(n,s);};z.zfree=[](voidpf,voidpf p){std::free(p);};Need(inflateInit2(&z,-15)==Z_OK,"Cannot decode update ZIP");struct End{z_stream*p;~End(){inflateEnd(p);}}end{&z};uint8_t empty=0;z.next_in=const_cast<Bytef*>(src.data());z.avail_in=e.packed;z.next_out=out.empty()?&empty:out.data();z.avail_out=e.size?e.size:1;Need(inflate(&z,Z_FINISH)==Z_STREAM_END&&z.total_out==e.size&&!z.avail_in,"Invalid compressed ZIP payload");}Need(Crc(out)==e.crc,"ZIP CRC mismatch");return out;}
std::string StageOwner(const Manifest&m){std::string s="{\"format\":1,\"owner\":\"Beetleworx update stage\",\"files\":[";bool first=true;for(auto&f:m.files){if(!first)s+=',';first=false;s+="{\"path\":"+Quote(f.path)+",\"sha256\":"+Quote(f.sha)+"}";}s+=",{\"path\":\"release-manifest.json\",\"sha256\":"+Quote(Hash(std::span(reinterpret_cast<const uint8_t*>(m.raw.data()),m.raw.size())))+"}]}";return s;}
Manifest Stage(const Bytes&zip,const Release&release,const fs::path&stage){Need(zip.size()==release.bytes&&Hash(zip)==release.sha,"Downloaded release SHA-256 does not match GitHub");auto entries=ZipIndex(zip);auto meta=std::find_if(entries.begin(),entries.end(),[](auto&e){return e.name=="release-manifest.json";});Need(meta!=entries.end()&&meta->size<=8*1024*1024,"Release ZIP is missing its root manifest");auto m=ManifestFrom(String(Unzip(zip,*meta)));Need(m.version==release.version&&m.tag==release.tag,"Release manifest/tag mismatch");Need(entries.size()==m.files.size()+1,"ZIP contains undeclared/missing files");if(fs::exists(stage)){
 Need(!Reparse(stage)&&fs::is_directory(stage),"Staging path is not a plain directory");
 bool reusable=false;try{auto existing=ManifestFrom(String(Read(stage/"release-manifest.json",8*1024*1024)));VerifyTree(stage,existing);reusable=existing.raw==m.raw;}catch(...){}
 if(reusable)return m;
 auto retired=stage.parent_path()/("incomplete-"+std::to_string(GetTickCount64()));PlainAbsolute(retired);
 Need(MoveFileExW(stage.c_str(),retired.c_str(),MOVEFILE_WRITE_THROUGH)!=0,"Cannot retire an incomplete staged update");
 }fs::create_directories(stage);WriteText(stage/"update-stage-owner.json",StageOwner(m));for(auto&f:m.files){CancelCheck();auto e=std::find_if(entries.begin(),entries.end(),[&](auto&x){return x.name==f.path;});Need(e!=entries.end()&&e->size==f.size,"ZIP file inventory mismatch");auto payload=Unzip(zip,*e);Need(Hash(payload)==f.sha,"ZIP file SHA-256 mismatch: "+f.path);PlainTree(stage,fs::path(f.path));Write(stage/fs::path(f.path),payload);}WriteText(stage/"release-manifest.json",m.raw);VerifyTree(stage,m);return m;}
void PlainAbsolute(const fs::path&p){auto absolute=fs::absolute(p).lexically_normal();auto part=absolute.root_path();Need(!Reparse(part),"Update filesystem root is a reparse point");for(auto&piece:absolute.relative_path()){part/=piece;Need(!Reparse(part),"Update move path contains a reparse point");}}
struct LeaseBusy : std::runtime_error {using std::runtime_error::runtime_error;};
struct FileLease {
 HANDLE handle=INVALID_HANDLE_VALUE;
 FileLease(const fs::path&p,bool exclusive){PlainAbsolute(p);fs::create_directories(p.parent_path());handle=CreateFileW(p.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);Need(handle!=INVALID_HANDLE_VALUE,"Update folder is not writable");OVERLAPPED range{};const DWORD flags=LOCKFILE_FAIL_IMMEDIATELY|(exclusive?LOCKFILE_EXCLUSIVE_LOCK:0);if(!LockFileEx(handle,flags,0,1,0,&range)){auto reason=GetLastError();CloseHandle(handle);handle=INVALID_HANDLE_VALUE;if(reason==ERROR_LOCK_VIOLATION||reason==ERROR_IO_PENDING)throw LeaseBusy("Another Beetleworx instance is using this installation. Close it before updating.");throw std::runtime_error("Cannot lock the Beetleworx update folder");}}
 ~FileLease(){if(handle!=INVALID_HANDLE_VALUE){OVERLAPPED range{};UnlockFileEx(handle,0,1,0,&range);CloseHandle(handle);}}
 FileLease(const FileLease&)=delete;FileLease&operator=(const FileLease&)=delete;
};
void Move(const fs::path&from,const fs::path&to){PlainAbsolute(from);PlainAbsolute(to);fs::create_directories(to.parent_path());Need(MoveFileExW(from.c_str(),to.c_str(),MOVEFILE_WRITE_THROUGH)!=0,"A release file is locked or the update folder is not writable. No update installed.");}
struct Change {std::string path,old_sha,new_sha;};
std::vector<Change> PlanInstall(const fs::path&root,const Manifest&old,const Manifest&next){VerifyTree(root,old);Need(String(Read(root/"release-manifest.json",8*1024*1024))==old.raw,"Installed release identity changed");std::map<std::string,Change> changes;for(auto&f:old.files)changes.emplace(Lower(f.path),Change{f.path,f.sha,{}});for(auto&f:next.files){auto key=Lower(f.path);auto p=changes.find(key);if(p==changes.end()){Need(!fs::exists(root/fs::path(f.path)),"Update would overwrite an untracked local file: "+f.path);changes.emplace(key,Change{f.path,{},f.sha});}else{Need(p->second.path==f.path,"Update changes filename case");p->second.new_sha=f.sha;}}
 std::vector<Change> out;for(auto&[_,c]:changes)if(c.old_sha!=c.new_sha)out.push_back(c);
 // Executable is swapped last among binaries; manifest is always the commit.
 std::stable_sort(out.begin(),out.end(),[](auto&a,auto&b){return(a.path=="beetleworx.exe")<(b.path=="beetleworx.exe");});out.push_back({"release-manifest.json",Hash(std::span(reinterpret_cast<const uint8_t*>(old.raw.data()),old.raw.size())),Hash(std::span(reinterpret_cast<const uint8_t*>(next.raw.data()),next.raw.size()))});return out;
}
std::string Journal(const std::vector<Change>&list,const std::string&stage,bool committed){std::string s="{\"format\":1,\"phase\":"+Quote(committed?"committed":"prepared")+",\"stage\":"+Quote(stage)+",\"files\":[";bool first=true;for(auto&c:list){if(!first)s+=',';first=false;s+="{\"path\":"+Quote(c.path)+",\"old\":"+Quote(c.old_sha)+",\"new\":"+Quote(c.new_sha)+"}";}return s+"]}";}
void DurableJournal(const fs::path&p,const std::string&text){PlainAbsolute(p);auto temporary=p;temporary+=L".tmp";PlainAbsolute(temporary);WriteText(temporary,text);HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);Need(file!=INVALID_HANDLE_VALUE,"Cannot flush update journal");const bool flushed=FlushFileBuffers(file)!=0;CloseHandle(file);Need(flushed,"Cannot make update journal durable");Need(MoveFileExW(temporary.c_str(),p.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0,"Cannot commit update journal");}
struct Recovery {std::vector<Change> files;std::string stage;bool committed=false;};
Recovery ReadJournal(const fs::path&p){auto j=ParseJson(String(Read(p,8*1024*1024)));Need(j.At("format").UInt()==1,"Unsupported update recovery journal");Recovery recovery;recovery.stage=SafePath(j.At("stage").Str());Need(recovery.stage.starts_with("stage-")&&recovery.stage.find('/')==std::string::npos,"Invalid recovery staging directory");auto phase=j.At("phase").Str();Need(phase=="prepared"||phase=="committed","Invalid update recovery phase");recovery.committed=phase=="committed";auto&a=j.At("files");Need(a.kind==Json::Array&&a.array.size()<=40001,"Invalid update recovery inventory");std::set<std::string> names;for(auto&x:a.array){Change c{SafePath(x.At("path").Str()),x.At("old").Str(),x.At("new").Str()};Need((c.old_sha.empty()||Digest(c.old_sha))&&(c.new_sha.empty()||Digest(c.new_sha))&&names.insert(Lower(c.path)).second,"Invalid update recovery digest/path");recovery.files.push_back(c);}return recovery;}
void Rollback(const fs::path&root,const fs::path&work,const fs::path&stage,const std::vector<Change>&list){
 PlainAbsolute(stage);
 for(auto p=list.rbegin();p!=list.rend();++p){auto relative=fs::path(p->path),target=root/relative,backup=work/"backup"/relative,staged=stage/relative;PlainTree(root,relative);PlainTree(work/"backup",relative);PlainTree(stage,relative);
 auto restore_staged=[&]{Need(!p->new_sha.empty()&&HashFile(target)==p->new_sha,"A file changed during update recovery; preserved for manual recovery");if(fs::exists(staged)){Need(HashFile(staged)==p->new_sha,"Conflicting staged file during update recovery");auto quarantine=work/("interrupted-"+std::to_string(GetTickCount64()))/relative;Move(target,quarantine);}else Move(target,staged);};
 if(fs::exists(backup)){Need(!p->old_sha.empty()&&HashFile(backup)==p->old_sha,"Update recovery backup differs from expected release");if(fs::exists(target))restore_staged();Move(backup,target);}else if(p->old_sha.empty()&&fs::exists(target))restore_staged();}
 if(fs::exists(work/"backup")){std::vector<fs::path> directories;for(auto&item:fs::recursive_directory_iterator(work/"backup")){Need(item.is_directory()&&!Reparse(item.path()),"Unexpected retained update backup; recovery preserved");directories.push_back(item.path());}std::sort(directories.begin(),directories.end(),[](auto&a,auto&b){return a.native().size()>b.native().size();});for(auto&p:directories)fs::remove(p);fs::remove(work/"backup");}
 fs::remove(work/"transaction.json");
}
void RetireCommitted(const fs::path&work){static std::atomic<unsigned> counter{0};auto history=work/("history-"+std::to_string(GetTickCount64())+"-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(counter.fetch_add(1)));PlainAbsolute(history);if(fs::exists(work/"backup"))Move(work/"backup",history/"backup");if(fs::exists(work/"transaction.json"))Move(work/"transaction.json",history/"transaction.json");}
// Caller owns the exclusive running lease. Return true when the canonical
// image changed underneath this process; the SDK must not start in that case.
bool RecoverUpdate(const fs::path&root,const fs::path&work){auto journal=work/"transaction.json";if(!fs::exists(journal))return false;auto recovery=ReadJournal(journal);if(recovery.committed){auto active=ManifestFrom(String(Read(root/"release-manifest.json",8*1024*1024)));VerifyTree(root,active);RetireCommitted(work);return false;}auto canonical=root/"beetleworx.exe";auto before=fs::is_regular_file(canonical)?HashFile(canonical):std::string{};Rollback(root,work,work/recovery.stage,recovery.files);return HashFile(canonical)!=before;}
void Install(const fs::path&root,const fs::path&stage,const Manifest&old,const Manifest&next){auto work=root/".beetleworx-update";PlainTree(root,".beetleworx-update");FileLease exclusive(work/"running.lock",true);Need(stage.parent_path()==work&&stage.filename().string().starts_with("stage-"),"Invalid staged update location");VerifyTree(stage,next);auto changes=PlanInstall(root,old,next);Need(!fs::exists(work/"transaction.json")&&!fs::exists(work/"backup"),"Previous update recovery is pending");DurableJournal(work/"transaction.json",Journal(changes,stage.filename().string(),false));
 try{for(auto&c:changes){auto p=fs::path(c.path),target=root/p;PlainTree(root,p);if(!c.old_sha.empty()){Need(HashFile(target)==c.old_sha,"Installed file changed during update");Move(target,work/"backup"/p);}if(!c.new_sha.empty()){Need(HashFile(stage/p)==c.new_sha,"Staged file changed during update");Move(stage/p,target);}}VerifyTree(root,next);Need(String(Read(root/"release-manifest.json",8*1024*1024))==next.raw,"Update manifest commit failed");DurableJournal(work/"transaction.json",Journal(changes,stage.filename().string(),true));}
 catch(...){try{Rollback(root,work,stage,changes);}catch(const std::exception&e){throw std::runtime_error(std::string("Update interrupted; recovery retained: ")+e.what());}throw;}
 // The new release is committed. Retiring backups is housekeeping; a locked
 // history directory must not turn a valid installation into a false failure.
 try{RetireCommitted(work);}catch(...){}
}
bool RemoveOwnedCache(const fs::path&work,const fs::path&directory,const std::map<std::string,std::string>&files,bool exact_hashes){
 try{Need(fs::weakly_canonical(directory).parent_path()==fs::weakly_canonical(work),"Cache directory escaped updater root");PlainAbsolute(directory);
  for(auto&entry:fs::recursive_directory_iterator(directory)){Need(!Reparse(entry.path()),"Cache contains a reparse point");auto rel=entry.path().lexically_relative(directory).generic_string();if(entry.is_directory()){bool prefix=false;for(auto&[name,_]:files)if(name.starts_with(rel+"/")){prefix=true;break;}Need(prefix,"Cache contains an unknown directory");}else{Need(entry.is_regular_file()&&files.contains(rel)&&entry.file_size()<=max_file,"Cache contains an unknown file");if(exact_hashes&&!files.at(rel).empty())Need(HashFile(entry.path())==files.at(rel),"Cache contents were modified");}}
  std::error_code ec;fs::remove_all(directory,ec);return !ec;
 }catch(...){return false;}
}
void PruneOwnedCaches(const fs::path&work){
 if(!fs::is_directory(work)||Reparse(work))return;
 std::vector<fs::path> history,stages,incomplete;for(auto&entry:fs::directory_iterator(work)){if(!entry.is_directory()||Reparse(entry.path()))continue;auto name=entry.path().filename().string();if(name.starts_with("history-"))history.push_back(entry.path());else if(name.starts_with("stage-"))stages.push_back(entry.path());else if(name.starts_with("incomplete-"))incomplete.push_back(entry.path());}
 auto newest=[](auto&a,auto&b){return fs::last_write_time(a)>fs::last_write_time(b);};std::sort(history.begin(),history.end(),newest);std::sort(stages.begin(),stages.end(),newest);
 // Only validated completed history is eligible, and retain the newest one.
 for(size_t i=1;i<history.size();++i)try{auto r=ReadJournal(history[i]/"transaction.json");if(!r.committed)continue;std::map<std::string,std::string> allowed{{"transaction.json",{}}};for(auto&f:r.files)if(!f.old_sha.empty())allowed.emplace("backup/"+f.path,f.old_sha);RemoveOwnedCache(work,history[i],allowed,true);}catch(...){}
 auto remove_stage=[&](const fs::path&p){try{auto j=ParseJson(String(Read(p/"update-stage-owner.json",8*1024*1024)));Need(j.At("format").UInt()==1&&j.At("owner").Str()=="Beetleworx update stage","Unknown staging owner");auto&a=j.At("files");Need(a.kind==Json::Array&&a.array.size()<=20001,"Invalid staging inventory");std::map<std::string,std::string> allowed{{"update-stage-owner.json",{}}};for(auto&f:a.array){auto name=SafePath(f.At("path").Str());Need(Digest(f.At("sha256").Str())&&allowed.emplace(name,std::string{}).second,"Invalid staging ownership record");}RemoveOwnedCache(work,p,allowed,false);}catch(...){}};
 for(size_t i=1;i<stages.size();++i)remove_stage(stages[i]);for(auto&p:incomplete)remove_stage(p);
}
std::string Redact(std::string s,const fs::path&exe,const fs::path&user){
 auto replace=[&](std::string find,const char*with){if(find.empty())return;size_t p=0;while((p=s.find(find,p))!=std::string::npos){s.replace(p,find.size(),with);p+=std::strlen(with);}};
 replace(PathText(user),"<user_data>");replace(PathText(exe),"<install>");
 if(auto p=Env(L"USERPROFILE");!p.empty())replace(PathText(fs::path(p)),"<profile>");
 s=std::regex_replace(s,std::regex(R"([A-Za-z]:[\\/][^\r\n\t"<>]*)"),"<local path>");
 s=std::regex_replace(s,std::regex(R"(\b(?:[0-9]{1,3}\.){3}[0-9]{1,3}\b)"),"<IP>");
 s=std::regex_replace(s,std::regex(R"((?:\\\\|//)[^\s"<>]+)"),"<network path>");
 // IPv6 full/compressed forms; avoid masking ordinary hh:mm:ss timestamps.
 s=std::regex_replace(s,std::regex(R"((?:[0-9A-Fa-f]{1,4}:){7}[0-9A-Fa-f]{1,4}|[0-9A-Fa-f:]*::[0-9A-Fa-f:.%_-]*)"),"<IPv6>");
 std::istringstream input(s);std::string line,out;while(std::getline(input,line)){auto low=Lower(line);if(low.find("token")!=std::string::npos||low.find("password")!=std::string::npos||low.find("authorization")!=std::string::npos||low.find("invite")!=std::string::npos||low.find("invitation")!=std::string::npos||low.find("join secret")!=std::string::npos||low.find("join code")!=std::string::npos)out+="<sensitive log line omitted>\n";else out+=line+'\n';}return out;
}
std::string Tail(const fs::path&p,size_t maximum){if(!fs::is_regular_file(p)||Reparse(p))return "(not present)\n";auto n=fs::file_size(p),take=std::min<uint64_t>(n,maximum);std::ifstream f(p,std::ios::binary);f.seekg(std::streamoff(n-take));std::string s(size_t(take),0);f.read(s.data(),s.size());s.resize(size_t(f.gcount()));return(n>take?"(bounded recent tail)\n":"")+s;}
std::string GameLogExcerpt(const fs::path& path) {
 constexpr size_t budget=131072, startup_bytes=24576;
 if(!fs::is_regular_file(path)||Reparse(path))return "(not present)\n";
 const auto size=fs::file_size(path);
 if(size<=budget)return Tail(path,budget);
 std::ifstream log(path,std::ios::binary);
 std::string startup(startup_bytes,'\0');
 log.read(startup.data(),startup.size());
 startup.resize(size_t(log.gcount()));
 const auto last_line=startup.rfind('\n');
 if(last_line!=std::string::npos)startup.resize(last_line+1);
 else startup.clear();
 std::string recent(budget-startup_bytes,'\0');
 log.clear();
 log.seekg(std::streamoff(size-recent.size()-1));
 const auto previous=log.get();
 log.read(recent.data(),recent.size());
 recent.resize(size_t(log.gcount()));
 if(previous!='\n'){
  const auto end=recent.find('\n');
  recent.erase(0,end==std::string::npos?recent.size():end+1);
 }
 return "(bounded startup excerpt)\n"+startup+"(bounded recent tail)\n"+recent;
}
std::string LogModifiedUtc(const fs::path& path){
 WIN32_FILE_ATTRIBUTE_DATA attributes{};SYSTEMTIME utc{};
 if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&attributes)||!FileTimeToSystemTime(&attributes.ftLastWriteTime,&utc))return "unavailable";
 char value[48]{};std::snprintf(value,sizeof(value),"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",utc.wYear,utc.wMonth,utc.wDay,utc.wHour,utc.wMinute,utc.wSecond,utc.wMilliseconds);return value;
}
std::string Report(const fs::path&exe,const fs::path&user,const std::string&version,const std::string&description){
 SYSTEM_INFO sys{};GetNativeSystemInfo(&sys);MEMORYSTATUSEX mem{sizeof(mem)};GlobalMemoryStatusEx(&mem);DISPLAY_DEVICEW display{sizeof(display)};EnumDisplayDevicesW(nullptr,0,&display,0);
 std::ostringstream s;s<<"Beetleworx diagnostic report\nVersion: "<<version<<"\nBuild SHA-256: ";try{s<<HashFile(exe/"beetleworx.exe");}catch(...){s<<"unavailable";}s<<"\nWindows architecture: "<<sys.wProcessorArchitecture<<"\nLogical processors: "<<sys.dwNumberOfProcessors<<"\nRAM MiB: "<<(mem.ullTotalPhys/(1024*1024))<<"\nGPU: "<<Utf8(display.DeviceString)<<"\n\nDescription:\n"<<description.substr(0,4096)<<"\n\nPrivacy: bounded text logs/settings only. No saves, ROMs, memory dumps, account credentials or automatic upload. Review this text before export.\n";
 s<<"\nBuild SHA-256 above identifies the installed file at report export. Crash logs may belong to earlier runs or versions; use their recorded image identity and timestamps.\n";
 for(auto name:{"release-manifest.json"}){s<<"\n--- "<<name<<" ---\n";auto p=exe/name;if(fs::is_regular_file(p)&&fs::file_size(p)<=8*1024*1024){try{auto m=ManifestFrom(String(Read(p,8*1024*1024)));s<<"Product: Beetleworx\nVersion: "<<m.version<<"\nTag: "<<m.tag<<"\nExecutable: "<<m.exe_sha<<"\nManifest files: "<<m.files.size()<<"\nManifest SHA-256: "<<HashFile(p)<<'\n';}catch(...){s<<"Unavailable/invalid manifest\n";}}}
 const std::array<std::pair<const char*,fs::path>,5> paths={{{"pc-settings.ini",user/"pc-settings.ini"},{"beetleworx.log",exe/"beetleworx.log"},{"user diagnostic log",user/"diagnostics"/"beetleworx.log"},{"crash.log",exe/"crash.log"},{"user crash.log",user/"diagnostics"/"crash.log"}}};
 for(auto&[name,p]:paths){
  const std::string_view section=name;
  const auto excerpt=section=="beetleworx.log"||section=="user diagnostic log"
      ?GameLogExcerpt(p):Tail(p,section=="pc-settings.ini"?32768:131072);
  s<<"\n--- "<<name<<" ---\n";
  if(fs::is_regular_file(p)&&!Reparse(p)){
   s<<"File last modified UTC: "<<LogModifiedUtc(p)<<'\n';
   if((section=="crash.log"||section=="user crash.log")&&excerpt.find("image_pe_timestamp=")==std::string::npos)
    s<<"Crash build identity: not recorded (legacy log; the installed build above may differ).\n";
  }
  s<<Redact(excerpt,exe,user);
 }
 return PrintableUtf8(s.str());
}
std::string Encode(std::string_view s){std::string out;for(unsigned char c:s){if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~')out+=char(c);else{out+='%';out+="0123456789ABCDEF"[c>>4];out+="0123456789ABCDEF"[c&15];}}return out;}
struct Context {
 std::mutex mutex;std::jthread worker;fs::path exe,user,stage;Manifest installed;std::optional<Manifest> staged;std::optional<Release> release;DemoSupportSnapshot view;bool initialized=false;Fetch fetch=Http;std::unique_ptr<FileLease> lease;
 ~Context(){stopping.store(true);if(worker.joinable())worker.join();}
};
Context context;
void UpdateProgress(uint64_t bytes){std::lock_guard lock(context.mutex);context.view.downloaded=bytes;}
void SetState(DemoSupportState state,std::string text){std::lock_guard lock(context.mutex);context.view.state=state;context.view.status=std::move(text);}
void Download(){Release release;fs::path stage;Fetch fetch;{std::lock_guard lock(context.mutex);Need(context.view.clean_release&&context.release.has_value(),"Verified installed release/update selection required");release=*context.release;stage=context.exe/".beetleworx-update"/("stage-"+release.sha.substr(0,16));context.stage=stage;fetch=context.fetch;context.view.total=release.bytes;context.view.downloaded=0;context.view.state=DemoSupportState::Downloading;context.view.status="Downloading verified update in the background...";}
 PlainTree(context.exe,fs::path(".beetleworx-update")/stage.filename());FileLease writer(stage.parent_path()/"stage.lock",true);auto response=fetch(release.url,max_bundle);Need(response.status==200,"Update download failed");auto manifest=Stage(response.bytes,release,stage);{std::lock_guard lock(context.mutex);context.staged=std::move(manifest);context.view.downloaded=release.bytes;context.view.state=DemoSupportState::Ready;context.view.install_on_exit=context.view.auto_update;context.view.status=context.view.auto_update?"Update ready. It will install when you exit normally.":"Update ready. Choose Install on exit when convenient.";}}
void CheckUpdates(){Fetch fetch;Version version;{std::lock_guard lock(context.mutex);Need(context.view.clean_release,"This development/modified build cannot update itself. Install an official packaged release first.");version=Semver(context.installed.version);fetch=context.fetch;context.view.state=DemoSupportState::Checking;context.view.status="Checking public demo releases...";}
 auto response=fetch(api,8*1024*1024);auto release=SelectRelease(response,version);bool automatic=false;{std::lock_guard lock(context.mutex);context.release=release;if(!release){context.view.state=DemoSupportState::NoRelease;context.view.status=response.status==404?"No public releases yet. Your installation is unchanged.":"You have the newest available public demo.";return;}context.view.update_version=release->version;context.view.state=DemoSupportState::Available;context.view.status="Update "+release->version+" is available.";automatic=context.view.auto_update;}if(automatic)Download();}
void Start(std::function<void()> fn){if(context.worker.joinable())context.worker.join();context.worker=std::jthread([fn=std::move(fn)]{background_task=true;SetThreadPriority(GetCurrentThread(),THREAD_MODE_BACKGROUND_BEGIN);try{fn();}catch(const std::exception&e){SetState(DemoSupportState::Error,e.what());}SetThreadPriority(GetCurrentThread(),THREAD_MODE_BACKGROUND_END);background_task=false;});}
bool Busy(){return context.view.state==DemoSupportState::Checking||context.view.state==DemoSupportState::Downloading;}
bool Open(const fs::path&target){return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",target.c_str(),nullptr,nullptr,SW_SHOWNORMAL))>32;}
}
void InitializeDemoSupport(const fs::path&exe,const fs::path&user){
 if(context.worker.joinable())context.worker.join();std::unique_lock lock(context.mutex);stopping.store(false);context.lease.reset();context.exe=fs::absolute(exe).lexically_normal();context.user=fs::absolute(user).lexically_normal();context.stage.clear();context.staged.reset();context.release.reset();context.initialized=true;context.view={};context.view.status="Support is ready. Reports stay on this PC until you choose to share them.";
 bool recovering=false;
 try{
  PlainTree(context.exe,".beetleworx-update");auto work=context.exe/".beetleworx-update";auto journal=work/"transaction.json";
  recovering=fs::exists(journal);
  Need(recovering||fs::is_regular_file(context.exe/"release-manifest.json"),"No official release manifest");
  context.lease=std::make_unique<FileLease>(work/"running.lock",false);
  if(fs::exists(journal)){
   recovering=true;context.lease.reset();
   {
    FileLease recovery_lease(work/"running.lock",true);
    if(RecoverUpdate(context.exe,work)){context.view.restart_required=true;context.view.status="Recovered the previous Beetleworx build. Reopen beetleworx.exe before playing.";return;}
   }
   context.lease=std::make_unique<FileLease>(work/"running.lock",false);recovering=false;
  }
  context.installed=ManifestFrom(String(Read(context.exe/"release-manifest.json",8*1024*1024)));VerifyTree(context.exe,context.installed);context.view.clean_release=true;context.view.version=context.installed.version;
  try{FileLease writer(work/"stage.lock",true);PruneOwnedCaches(work);}catch(...){}
  auto preference=context.user/"support-preferences.json";context.view.auto_update=true;if(fs::is_regular_file(preference))try{context.view.auto_update=ParseJson(String(Read(preference,4096))).At("auto_update").Boolean();}catch(...){context.view.auto_update=false;context.view.status="Update preferences were invalid. Manual updates are available.";}
 }catch(const LeaseBusy&e){context.lease.reset();context.view.restart_required=true;context.view.status=e.what();return;}
 catch(const std::exception&e){if(recovering)context.lease.reset();context.view.version="development";context.view.clean_release=false;context.view.auto_update=false;context.view.restart_required=recovering;context.view.status=std::string(recovering?"Update recovery must finish before playing: ":"Automatic updates unavailable: ")+e.what();}
 const bool automatic=context.view.clean_release&&context.view.auto_update;
 if(automatic)context.view.state=DemoSupportState::Checking;
 lock.unlock();if(automatic)Start(CheckUpdates);
}
DemoSupportSnapshot SnapshotDemoSupport(){std::lock_guard lock(context.mutex);return context.view;}
bool SupportOption(int id,const std::string&command,std::string&out){try{
 if(id==7310){auto s=SnapshotDemoSupport();out=s.status;return true;}if(id==7311){out=SnapshotDemoSupport().report_preview;return true;}if(id==7312){auto s=SnapshotDemoSupport();out=s.update_version.empty()?s.status:"Update "+s.update_version+" - "+s.status;return true;}
 fs::path exe,user;std::string version;{std::lock_guard lock(context.mutex);Need(context.initialized,"Support has not initialized");exe=context.exe;user=context.user;version=context.view.version;}
 if(id==7300||id==7301){std::string report;if(id==7300){report=Report(exe,user,version,command);std::lock_guard lock(context.mutex);context.view.report_preview=std::move(report);context.view.report_exported=false;out="Report preview prepared. Review the text before choosing Export.";return true;}
  {std::lock_guard lock(context.mutex);Need(!context.view.report_preview.empty(),"Preview the report before exporting it");report=context.view.report_preview;}
  auto hash=Hash(std::span(reinterpret_cast<const uint8_t*>(report.data()),report.size()));auto name="beetleworx-report-"+hash.substr(0,16)+".txt";auto path=user/"reports"/name;PlainTree(user,fs::path("reports")/name);if(fs::exists(path))Need(HashFile(path)==hash,"An existing report has different contents");else WriteText(path,report);std::lock_guard lock(context.mutex);context.view.report_preview=std::move(report);context.view.report_path=PathText(path);context.view.report_sha256=hash;context.view.report_exported=true;out="Report exported to "+PathText(path)+". Nothing was uploaded.";return true;}
 if(id==7302){auto s=SnapshotDemoSupport();Need(s.report_exported,"Review and export a report first");auto filename=PathText(fs::path(Wide(s.report_path)).filename());std::string body="Version: "+s.version+"\n\nWhat happened / how to reproduce:\n"+command.substr(0,1500)+"\n\nLocal diagnostic report: "+filename+"\nSHA-256: "+s.report_sha256+"\n\nAttach the exported report after reviewing it. This draft does not upload the file.";auto url="https://github.com/Bloticle/beetleworx/issues/new?title="+Encode("[Demo bug] "+s.version)+"&body="+Encode(body);Need(Open(fs::path(Wide(url))),"Cannot open the issue draft in your browser");out="Opened an editable GitHub draft. Review it and submit when ready.";return true;}
 if(id==7307){PlainTree(user,"reports");fs::create_directories(user/"reports");Need(Open(user/"reports"),"Cannot open the reports folder");out="Opened reports folder.";return true;}
 if(id==7303||id==7304){{std::lock_guard lock(context.mutex);Need(!Busy(),"An update operation is already running");Need(context.view.clean_release,"Updates require an unmodified official release with a valid version/hash manifest");context.view.state=id==7303?DemoSupportState::Checking:DemoSupportState::Downloading;}Start(id==7303?CheckUpdates:Download);out=id==7303?"Checking releases in the background...":"Downloading in the background...";return true;}
 if(id==7305||id==7308){std::lock_guard lock(context.mutex);if(id==7305)Need(context.staged.has_value()&&context.view.state==DemoSupportState::Ready,"Download and verify an update first");context.view.install_on_exit=id==7305;out=id==7305?"Update will install after you exit the game normally.":"Update installation on exit cancelled.";return true;}
 if(id==7306){bool value;{std::lock_guard lock(context.mutex);Need(context.view.clean_release,"Automatic updates require an unmodified official release");value=PreferenceValue(command,context.view.auto_update);}PlainTree(user,"support-preferences.json");DurableJournal(user/"support-preferences.json",std::string("{\"auto_update\":")+(value?"true}":"false}"));{std::lock_guard lock(context.mutex);context.view.auto_update=value;if(!value)context.view.install_on_exit=false;else if(context.staged&&context.view.state==DemoSupportState::Ready)context.view.install_on_exit=true;}out=value?"Automatic updates enabled; downloads happen in the background and install after normal exit.":"Automatic updates disabled.";return true;}
 return false;
 }catch(const std::exception&e){out=e.what();return false;}}
bool ShutdownDemoSupport(bool restart,std::string&out){try{stopping.store(true);if(context.worker.joinable())context.worker.join();fs::path exe,stage;Manifest old,next;{std::lock_guard lock(context.mutex);context.lease.reset();if(!context.initialized||!context.view.install_on_exit||!context.staged){out="No update installation scheduled.";return true;}exe=context.exe;stage=context.stage;old=context.installed;next=*context.staged;}Install(exe,stage,old,next);out="Updated Beetleworx to "+next.version+". Your user data was preserved.";
 if(restart){auto canonical=exe/"beetleworx.exe";std::wstring command=L"\""+canonical.wstring()+L"\"";STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION process{};Need(CreateProcessW(canonical.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,exe.c_str(),&startup,&process)!=0,"Update installed; please reopen beetleworx.exe manually");CloseHandle(process.hThread);CloseHandle(process.hProcess);}return true;
 }catch(const std::exception&e){out=e.what();return false;}}
namespace {
struct PreviewWindow {HWND owner=nullptr,description=nullptr,preview=nullptr,status=nullptr;std::string description_exported,base_report;};
std::string WindowText(HWND h){int n=GetWindowTextLengthW(h);Need(n>=0&&n<=65536,"Support text exceeds its limit");std::wstring s(size_t(n)+1,0);GetWindowTextW(h,s.data(),n+1);s.resize(n);return Utf8(s);}
LRESULT CALLBACK PreviewProc(HWND h,UINT message,WPARAM w,LPARAM l){auto state=reinterpret_cast<PreviewWindow*>(GetWindowLongPtrW(h,GWLP_USERDATA));
 if(message==WM_NCCREATE){state=static_cast<PreviewWindow*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(state));}
 if(message==WM_COMMAND&&state){auto id=LOWORD(w);
 if(id==1001&&HIWORD(w)==EN_CHANGE&&state->preview){try{auto description=WindowText(state->description);auto report=state->base_report;const auto begin=report.find("Description:\n");const auto end=report.find("\n\nPrivacy:",begin);Need(begin!=std::string::npos&&end!=std::string::npos,"Report description boundary is missing");report.replace(begin+13,end-(begin+13),description);{std::lock_guard lock(context.mutex);context.view.report_preview=report;context.view.report_exported=false;}SetWindowTextW(state->preview,Wide(report).c_str());}catch(...){}return 0;}
 if(id==1005){DestroyWindow(h);return 0;}if(id==1003||id==1004){std::string result;try{auto description=WindowText(state->description);if(id==1003){
   // Export exactly the visible snapshot. Editing the description updates it
   // on EN_CHANGE without collecting any newer, unreviewed log lines.
   if(SupportOption(7301,{},result))state->description_exported=description;
  }else{Need(SnapshotDemoSupport().report_exported,"Export the reviewed report first");Need(description==state->description_exported,"Description changed. Export the updated report before opening a draft.");SupportOption(7302,description,result);}SetWindowTextW(state->status,Wide(result).c_str());}catch(const std::exception&e){try{SetWindowTextW(state->status,Wide(PrintableUtf8(e.what())).c_str());}catch(...){SetWindowTextW(state->status,L"Support action failed.");}}return 0;}}
 if(message==WM_CLOSE){DestroyWindow(h);return 0;}return DefWindowProcW(h,message,w,l);
}
}
bool ShowDemoSupportPreview(void*owner,std::string&out){try{
 Need(SupportOption(7300,{},out),out);PreviewWindow state;state.owner=static_cast<HWND>(owner);if(!state.owner)state.owner=GetActiveWindow();
 const wchar_t* klass=L"BeetleworxSupportPreview";WNDCLASSW wc{};wc.lpfnWndProc=PreviewProc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=klass;wc.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));wc.hbrBackground=HBRUSH(COLOR_BTNFACE+1);if(!RegisterClassW(&wc))Need(GetLastError()==ERROR_CLASS_ALREADY_EXISTS,"Cannot create report preview class");
 HWND h=CreateWindowExW(WS_EX_DLGMODALFRAME,klass,L"Beetleworx - Review diagnostic report",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU, CW_USEDEFAULT,CW_USEDEFAULT,850,730,state.owner,nullptr,wc.hInstance,&state);Need(h!=nullptr,"Cannot open report preview");
 auto control=[&](const wchar_t*type,const wchar_t*text,DWORD style,int x,int y,int width,int height,int id){auto c=CreateWindowExW(std::wstring_view(type)==L"EDIT"?WS_EX_CLIENTEDGE:0,type,text,WS_CHILD|WS_VISIBLE|style,x,y,width,height,h,reinterpret_cast<HMENU>(INT_PTR(id)),wc.hInstance,nullptr);SendMessageW(c,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);return c;};
 control(L"STATIC",L"Describe what happened and how to reproduce it (optional):",0,14,12,800,20,0);
 state.description=control(L"EDIT",L"",ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL|WS_TABSTOP,14,36,804,80,1001);SendMessageW(state.description,EM_SETLIMITTEXT,4096,0);
 control(L"STATIC",L"Review the bounded report below. Export saves it locally; no file is uploaded.",0,14,124,804,22,0);
 auto snapshot=SnapshotDemoSupport();state.base_report=snapshot.report_preview;state.preview=control(L"EDIT",L"",ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|ES_AUTOHSCROLL|WS_VSCROLL|WS_HSCROLL|WS_TABSTOP,14,150,804,430,1002);SendMessageW(state.preview,EM_SETLIMITTEXT,1024*1024,0);SetWindowTextW(state.preview,Wide(snapshot.report_preview).c_str());
 control(L"BUTTON",L"Export report",BS_PUSHBUTTON|WS_TABSTOP,14,590,160,30,1003);
 control(L"BUTTON",L"Open GitHub issue draft",BS_PUSHBUTTON|WS_TABSTOP,186,590,200,30,1004);
 control(L"BUTTON",L"Close",BS_PUSHBUTTON|WS_TABSTOP,708,590,110,30,1005);
 state.status=control(L"STATIC",L"Nothing has been exported or uploaded.",0,14,632,804,52,1006);
 if(state.owner)EnableWindow(state.owner,FALSE);ShowWindow(h,SW_SHOW);SetForegroundWindow(h);SetFocus(state.description);
 MSG msg{};while(IsWindow(h)){const auto got=GetMessageW(&msg,nullptr,0,0);if(got<=0){DestroyWindow(h);if(got==0)PostQuitMessage(int(msg.wParam));break;}if(!IsDialogMessageW(h,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
 if(state.owner){EnableWindow(state.owner,TRUE);SetForegroundWindow(state.owner);}out="Report preview closed.";return true;
 }catch(const std::exception&e){out=e.what();return false;}}
bool TestLiveDemoUpdate(const fs::path& root,std::string& error){
 try{
  Need(root.is_absolute() && !fs::exists(root) && fs::is_directory(root.parent_path()),"Live update test needs a new absolute directory with an existing parent");
  PlainAbsolute(root);fs::create_directory(root);
  constexpr const char* previous_version="0.1.0-demo.1";
  auto listing=Http(api,8*1024*1024);
  auto next=SelectRelease(listing,Semver(previous_version));Need(next.has_value(),"No newer published demo is available for the live update test");
  auto old_details=Http("https://api.github.com/repos/Bloticle/beetleworx/releases/tags/v0.1.0-demo.1",8*1024*1024);
  const auto old_array="["+String(old_details.bytes)+"]";
  auto previous=SelectRelease({old_details.status,Bytes(old_array.begin(),old_array.end())},Semver("0.0.0"));
  Need(previous && previous->version==previous_version,"Previous published demo is unavailable");
  auto installation=root/"installation";
  auto old_download=Http(previous->url,max_bundle);Need(old_download.status==200,"Previous demo download failed");
  auto old=Stage(old_download.bytes,*previous,installation);old_download.bytes.clear();old_download.bytes.shrink_to_fit();
  for(const auto& name:{"user_data/update-test-sentinel","game-data/update-test-sentinel","mods/update-test-sentinel"})WriteText(installation/name,"preserve existing user files");
  auto downloaded=Http(next->url,max_bundle);Need(downloaded.status==200,"Published update download failed");
  auto stage=installation/".beetleworx-update"/("stage-"+next->sha.substr(0,16));
  auto updated=Stage(downloaded.bytes,*next,stage);downloaded.bytes.clear();downloaded.bytes.shrink_to_fit();
  Install(installation,stage,old,updated);VerifyTree(installation,updated);
  for(const auto& name:{"user_data/update-test-sentinel","game-data/update-test-sentinel","mods/update-test-sentinel"})Need(String(Read(installation/name,1024))=="preserve existing user files","Update changed a user-file sentinel");
  std::ostringstream report;report<<"{\"from\":\""<<previous->version<<"\",\"to\":\""<<next->version<<"\",\"zip_sha256\":\""<<next->sha<<"\",\"installed_exe_sha256\":\""<<HashFile(installation/"beetleworx.exe")<<"\",\"files\":"<<updated.files.size()<<",\"user_files_preserved\":true,\"scope\":\"Live GitHub discovery and WinHTTP downloads, release and file hash verification, normal transactional installer into an isolated copy. No downloaded executable launched and no normal installation changed.\"}\n";
  WriteText(root/"live-update-results.json",report.str());error.clear();return true;
 }catch(const std::exception& exception){error=exception.what();return false;}
}
bool TestDemoSupport(std::string&error){
 try{
  auto rejects=[](auto fn){try{fn();return false;}catch(...){return true;}};
  Need(Compare(Semver("0.1.0-demo.10"),Semver("0.1.0-demo.2"))>0&&Compare(Semver("0.1.0"),Semver("0.1.0-demo.10"))>0,"Semver precedence regression");
  Need(PreferenceValue("on",true)&&PreferenceValue("on",false)&&!PreferenceValue("off",true)&&!PreferenceValue("off",false)&&PreferenceValue("",false)&&!PreferenceValue("",true)&&rejects([]{PreferenceValue("invalid",false);}),"Automatic update choice is not idempotent");
  Need(rejects([]{Semver("1.0.0-01");})&&rejects([]{Semver("01.0.0");}),"Malformed semver admitted");
  for(auto p:{"../x","/root","a/../x","user_data/save.dat","game-data/default.xex","mods/a.png","a:stream","CON.txt","a\\b","a./b","a//b"})Need(rejects([&]{SafePath(p);}),"Unsafe update path admitted");
  Need(SafePath("pc-assets/patches/UIOptions.pak.em2patch")=="pc-assets/patches/UIOptions.pak.em2patch","Valid update path rejected");
  Need(rejects([]{ParseJson("{\"a\":1,\"a\":2}");})&&rejects([]{ParseJson("[1,]");})&&ParseJson("{\"x\":\"\\uD83D\\uDE00\"}").At("x").Str().size()==4,"JSON parser regression");
  Need(!SelectRelease({404,{}},Semver("0.1.0")),"Missing repository is not a friendly no-release state");
  auto b=[](std::string s){return Bytes(s.begin(),s.end());};
  const auto old_exe=b("old executable fixture"),new_exe=b("new executable fixture"),old_data=b("old asset"),new_data=b("new asset");
  auto manifest=[&](std::string version,const Bytes&exe,const Bytes&data){return "{\"format\":2,\"product\":\"Beetleworx\",\"executable\":\"beetleworx.exe\",\"version\":"+Quote(version)+",\"tag\":"+Quote("v"+version)+",\"exe_sha256\":"+Quote(Hash(exe))+",\"files\":[{\"path\":\"beetleworx.exe\",\"bytes\":"+std::to_string(exe.size())+",\"sha256\":"+Quote(Hash(exe))+"},{\"path\":\"pc-assets/data.bin\",\"bytes\":"+std::to_string(data.size())+",\"sha256\":"+Quote(Hash(data))+"}]}";};
  auto old=ManifestFrom(manifest("0.1.0-demo.1",old_exe,old_data)),next=ManifestFrom(manifest("0.1.0-demo.2",new_exe,new_data));
  // Real filesystem and transport fixtures, never the running game directory.
  auto temp=fs::temp_directory_path();auto root=temp/("beetleworx-support-test-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));Need(fs::create_directory(root),"Cannot create isolated support test directory");
  struct Cleanup {fs::path root,temp;~Cleanup(){try{if(root.parent_path()==temp&&root.filename().string().starts_with("beetleworx-support-test-")&&!Reparse(root)){std::error_code ec;fs::remove_all(root,ec);}}catch(...){}}}cleanup{root,temp};
  auto putRelease=[&](const fs::path&where,const Manifest&m,const Bytes&exe,const Bytes&data){Write(where/"beetleworx.exe",exe);Write(where/"pc-assets/data.bin",data);WriteText(where/"release-manifest.json",m.raw);};
  putRelease(root,old,old_exe,old_data);WriteText(root/"user_data"/"save.txt","save must survive");
  const auto game_log=root/"beetleworx.log";
  WriteText(game_log,"GPU startup\n"+std::string(150000,'x')+"\n[gpu] Texture fetch warning\n");
  const auto report_log=GameLogExcerpt(game_log);
  Need(report_log.find("GPU startup\n")!=std::string::npos &&
       report_log.ends_with("[gpu] Texture fetch warning\n") &&
       report_log.find("xxxx")==std::string::npos,"Diagnostic log lost startup or recent messages");
  WriteText(game_log,"short log\n");
  Need(GameLogExcerpt(game_log)=="short log\n","Diagnostic excerpt changed a short log");
  WriteText(root/"crash.log","Beetleworx unhandled exception\ncode=0xC0000005\n");
  const auto legacy_report=Report(root,root/"user_data",old.version,"");
  Need(legacy_report.find("Crash build identity: not recorded (legacy log;")!=std::string::npos &&
       legacy_report.find("File last modified UTC: 20")!=std::string::npos &&
       legacy_report.find("installed file at report export")!=std::string::npos,
       "Historical crash log was attributed to the currently installed build");
  WriteText(root/"crash.log","Beetleworx unhandled exception\nimage_pe_timestamp=12345678\nimage_codeview=fixture\n");
  const auto identified_report=Report(root,root/"user_data",old.version,"");
  Need(identified_report.find("image_pe_timestamp=12345678")!=std::string::npos &&
       identified_report.find("Crash build identity: not recorded")==std::string::npos,
       "Recorded crash image identity was lost or marked unknown");
  auto zip=[](const std::vector<std::pair<std::string,Bytes>>&files){Bytes out,central;auto u16=[](Bytes&o,uint16_t n){o.push_back(uint8_t(n));o.push_back(uint8_t(n>>8));};auto u32=[&](Bytes&o,uint32_t n){u16(o,uint16_t(n));u16(o,uint16_t(n>>16));};for(auto&[name,data]:files){auto offset=uint32_t(out.size()),crc=Crc(data);u32(out,0x04034B50);u16(out,20);u16(out,0);u16(out,0);u32(out,0);u32(out,crc);u32(out,uint32_t(data.size()));u32(out,uint32_t(data.size()));u16(out,uint16_t(name.size()));u16(out,0);out.insert(out.end(),name.begin(),name.end());out.insert(out.end(),data.begin(),data.end());u32(central,0x02014B50);u16(central,20);u16(central,20);u16(central,0);u16(central,0);u32(central,0);u32(central,crc);u32(central,uint32_t(data.size()));u32(central,uint32_t(data.size()));u16(central,uint16_t(name.size()));for(int i=0;i<4;++i)u16(central,0);u32(central,0);u32(central,offset);central.insert(central.end(),name.begin(),name.end());}auto start=uint32_t(out.size());out.insert(out.end(),central.begin(),central.end());u32(out,0x06054B50);u16(out,0);u16(out,0);u16(out,uint16_t(files.size()));u16(out,uint16_t(files.size()));u32(out,uint32_t(central.size()));u32(out,start);u16(out,0);return out;};
  auto archive=zip({{"beetleworx.exe",new_exe},{"pc-assets/data.bin",new_data},{"release-manifest.json",b(next.raw)}});
  auto compressed=em1::DeflateNativePakResource(new_data);Need(compressed.size()>6,"Compressed fixture is invalid");Bytes raw(compressed.begin()+2,compressed.end()-4);Need(Unzip(raw,{"compressed.bin",0,uint32_t(raw.size()),uint32_t(new_data.size()),Crc(new_data),8})==new_data,"Real raw-Deflate ZIP payload regression");
  Release release{next.version,next.tag,"https://github.com/bloticle/beetleworx/releases/download/v0.1.0-demo.2/Beetleworx.zip",Hash(archive),archive.size()};
  std::string listing="[{\"draft\":false,\"prerelease\":true,\"tag_name\":"+Quote(next.tag)+",\"assets\":[{\"name\":\"Beetleworx.zip\",\"digest\":"+Quote("sha256:"+release.sha)+",\"browser_download_url\":"+Quote(release.url)+",\"size\":"+std::to_string(release.bytes)+"}]}]";
  unsigned fetches=0;Fetch mock=[&](const std::string&url,uint64_t limit){++fetches;if(url==api)return Response{200,b(listing)};Need(url==release.url&&limit>=archive.size(),"Unexpected update request in fixture");return Response{200,archive};};
  auto selected=SelectRelease(mock(api,8*1024*1024),Semver(old.version));Need(selected.has_value()&&selected->version==next.version,"Public demo prerelease was skipped");auto downloaded=mock(selected->url,max_bundle);auto stage=root/".beetleworx-update"/"stage-fixture";auto staged=Stage(downloaded.bytes,*selected,stage);Need(fetches==2&&staged.exe_sha==Hash(new_exe),"Mock update transport/staging regression");
  auto recovery_work=root/".beetleworx-update";
  {FileLease first(recovery_work/"running.lock",false),second(recovery_work/"running.lock",false);Need(rejects([&]{Install(root,stage,old,next);}),"Install ignored another live game lease");VerifyTree(root,old);VerifyTree(stage,next);}
  {FileLease writer(recovery_work/"stage.lock",true);Need(rejects([&]{FileLease duplicate_writer(recovery_work/"stage.lock",true);}),"Multiple workers admitted to one staging directory");}
  auto tampered=archive;tampered[35]^=1;Need(rejects([&]{Stage(tampered,release,root/"bad-digest");}),"Corrupt update digest admitted");
  auto traversal=zip({{"../evil",b("x")}});Need(rejects([&]{ZipIndex(traversal);}),"ZIP traversal admitted");
  auto duplicate=zip({{"A.txt",b("x")},{"a.txt",b("y")}});Need(rejects([&]{ZipIndex(duplicate);}),"Case-colliding ZIP paths admitted");
  // A locked original file must roll back prior moves, not leave a mixed build.
  HANDLE locked=CreateFileW((root/"beetleworx.exe").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);Need(locked!=INVALID_HANDLE_VALUE,"Cannot create locked-file update fixture");bool failed=rejects([&]{Install(root,stage,old,next);});CloseHandle(locked);Need(failed,"Locked installed binary was overwritten");VerifyTree(root,old);Need(String(Read(root/"user_data/save.txt",128))=="save must survive","Update touched user data");
  auto changes=PlanInstall(root,old,next);DurableJournal(recovery_work/"transaction.json",Journal(changes,stage.filename().string(),false));Move(root/"pc-assets/data.bin",recovery_work/"backup/pc-assets/data.bin");Move(stage/"pc-assets/data.bin",root/"pc-assets/data.bin");Need(!RecoverUpdate(root,recovery_work),"Asset-only recovery unnecessarily requested restart");VerifyTree(root,old);VerifyTree(stage,next);
  DurableJournal(recovery_work/"transaction.json",Journal(changes,stage.filename().string(),false));Move(root/"beetleworx.exe",recovery_work/"backup/beetleworx.exe");Move(stage/"beetleworx.exe",root/"beetleworx.exe");Need(RecoverUpdate(root,recovery_work),"Recovery changed the running executable without requiring restart");VerifyTree(root,old);VerifyTree(stage,next);
  // Production rollback restores the entire verified stage for a true retry.
  VerifyTree(stage,next);Install(root,stage,old,next);VerifyTree(root,next);Need(String(Read(root/"user_data/save.txt",128))=="save must survive","Successful update touched saves");
  Need(!fs::exists(root/".beetleworx-update/transaction.json"),"Committed transaction remained pending");
  auto third_data=b("third asset, same executable");auto third=ManifestFrom(manifest("0.1.0-demo.3",new_exe,third_data));auto third_stage=root/".beetleworx-update"/"stage-fixture-three";putRelease(third_stage,third,new_exe,third_data);Install(root,third_stage,next,third);VerifyTree(root,third);
  auto fourth_data=b("fourth asset, same executable");auto fourth=ManifestFrom(manifest("0.1.0-demo.4",new_exe,fourth_data));auto fourth_stage=root/".beetleworx-update"/"stage-fixture-four";putRelease(fourth_stage,fourth,new_exe,fourth_data);Install(root,fourth_stage,third,fourth);VerifyTree(root,fourth);Need(String(Read(root/"user_data/save.txt",128))=="save must survive","Asset-only updates touched saves");
  auto extra=fourth;extra.files.push_back({"local.bin",Hash(new_data),new_data.size()});WriteText(root/"local.bin","personal local file");Need(rejects([&]{PlanInstall(root,fourth,extra);}),"Update admitted an untracked-file overwrite");
  auto incomplete=recovery_work/"incomplete-owned";WriteText(incomplete/"update-stage-owner.json",StageOwner(fourth));WriteText(incomplete/"pc-assets/data.bin","partial download");auto unknown=recovery_work/"incomplete-unknown";WriteText(unknown/"update-stage-owner.json",StageOwner(fourth));WriteText(unknown/"personal.txt","must remain");PruneOwnedCaches(recovery_work);Need(!fs::exists(incomplete)&&fs::exists(unknown/"personal.txt"),"Cache cleanup removed unknown files or retained known partial payload");unsigned histories=0;for(auto&e:fs::directory_iterator(recovery_work))if(e.is_directory()&&e.path().filename().string().starts_with("history-"))++histories;Need(histories==1,"Completed update history retention is unbounded");
  auto redacted=Redact("token=secret\nD:\\Private\\file.log\nIP 192.168.1.2\n\\\\NAS\\private\\game.rvz\nIPv6 2001:db8::1\ninvitation secret abcdef\n",root,root/"user_data");Need(redacted.find("secret")==std::string::npos&&redacted.find("Private")==std::string::npos&&redacted.find("192.168.1.2")==std::string::npos&&redacted.find("NAS")==std::string::npos&&redacted.find("2001:db8")==std::string::npos&&redacted.find("abcdef")==std::string::npos,"Report privacy regression");
  std::string damaged="log";damaged.push_back(char(0x80));damaged.push_back('\0');damaged.push_back(char(1));damaged+="\nnext line";auto printable=PrintableUtf8(damaged);Need(printable.find('\0')==std::string::npos&&printable.find(char(1))==std::string::npos&&Wide(printable).find(L"next line")!=std::wstring::npos,"Invalid/control log bytes truncated the report preview");
  auto unicode=root/fs::path(L"\u6e2c\u8a66");WriteText(unicode/"report.txt","UTF-8 path fixture");Need(Wide(PathText(unicode))==unicode.native()&&String(Read(unicode/"report.txt",128))=="UTF-8 path fixture","Windows Unicode report path was not preserved");Need(Redact(PathText(unicode)+"\\log.txt\n",root,unicode).find("<user_data>")!=std::string::npos,"Unicode user path was not redacted");
  if(auto package=Env(L"BEETLEWORX_SUPPORT_PACKAGE_FIXTURE");!package.empty()){
   auto real_zip=Read(fs::path(package),max_bundle);auto entries=ZipIndex(real_zip);auto metadata=std::find_if(entries.begin(),entries.end(),[](const auto&e){return e.name=="release-manifest.json";});Need(metadata!=entries.end()&&metadata->size<=8*1024*1024,"Producer package manifest missing");auto producer=ManifestFrom(String(Unzip(real_zip,*metadata)));Need(entries.size()==producer.files.size()+1,"Producer ZIP inventory mismatch");for(const auto&file:producer.files){auto row=std::find_if(entries.begin(),entries.end(),[&](const auto&e){return e.name==file.path;});Need(row!=entries.end()&&row->size==file.size&&Hash(Unzip(real_zip,*row))==file.sha,"Producer ZIP file checksum mismatch: "+file.path);}
  }
  error.clear();return true;
 }catch(const std::exception&e){error=e.what();return false;}
}
bool TestRunningDemoSupportSwap(std::string&error){
 fs::path canonical,backup,replacement,retired;std::string expected;bool moved=false;
 auto identity=[](const fs::path&p){HANDLE h=CreateFileW(p.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);Need(h!=INVALID_HANDLE_VALUE,"Cannot open running image identity");BY_HANDLE_FILE_INFORMATION info{};bool ok=GetFileInformationByHandle(h,&info)!=0;CloseHandle(h);Need(ok,"Cannot inspect running image identity");return std::array<DWORD,3>{info.dwVolumeSerialNumber,info.nFileIndexHigh,info.nFileIndexLow};};
 std::array<DWORD,3> original{};
 auto restore=[&]{if(!moved)return;if(fs::exists(canonical)){Need(HashFile(canonical)==expected,"Canonical image changed during diagnostic; original backup retained");Move(canonical,retired);}Move(backup,canonical);moved=false;Need(HashFile(canonical)==expected&&identity(canonical)==original,"Original mapped image identity was not restored");};
 try{
  std::wstring module(32768,0);auto n=GetModuleFileNameW(nullptr,module.data(),DWORD(module.size()));Need(n&&n<module.size(),"Cannot locate running canonical executable");module.resize(n);canonical=fs::path(module);Need(Lower(canonical.filename().string())=="beetleworx.exe","Mapped-image test requires the canonical beetleworx.exe");PlainAbsolute(canonical);expected=HashFile(canonical);original=identity(canonical);
  auto prefix=".beetleworx-swap-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64());backup=canonical.parent_path()/(prefix+".old");replacement=canonical.parent_path()/(prefix+".new");retired=canonical.parent_path()/(prefix+".verified-copy");Need(!fs::exists(backup)&&!fs::exists(replacement)&&!fs::exists(retired),"Mapped-image diagnostic path already exists");
  Need(CopyFileW(canonical.c_str(),replacement.c_str(),TRUE)!=0,"Cannot stage running-image verification copy");Need(HashFile(replacement)==expected,"Running-image diagnostic copy hash differs");Move(canonical,backup);moved=true;Move(replacement,canonical);Need(HashFile(canonical)==expected,"Replacement canonical image hash differs");restore();
  for(auto&p:{replacement,retired})if(fs::exists(p)){Need(HashFile(p)==expected,"Unexpected mapped-image diagnostic temporary contents");fs::remove(p);}error.clear();return true;
 }catch(const std::exception&e){error=e.what();try{restore();}catch(const std::exception&restore_error){error+="; original-image restore needs attention: ";error+=restore_error.what();if(!backup.empty())error+="; original backup: "+PathText(backup);}return false;}
}
} // namespace em2
