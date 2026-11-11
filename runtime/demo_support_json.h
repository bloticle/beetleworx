#pragma once
#include <charconv>
#include <map>
#include <vector>
#include <string>
#include <string_view>
#include <stdexcept>
#include <cstdint>
namespace em2::demo_detail {
struct Json {
  enum Kind{Null,Bool,Number,String,Array,Object}kind=Null;
  std::string text;bool flag=false;std::vector<Json> array;std::map<std::string,Json> object;
  const Json& At(std::string_view k)const{auto p=object.find(std::string(k));if(kind!=Object||p==object.end())throw std::runtime_error("Required JSON field missing: "+std::string(k));return p->second;}
  const Json* Find(std::string_view k)const{auto p=object.find(std::string(k));return kind==Object&&p!=object.end()?&p->second:nullptr;}
  std::string Str()const{if(kind!=String)throw std::runtime_error("Expected JSON string");return text;}
  bool Boolean()const{if(kind!=Bool)throw std::runtime_error("Expected JSON boolean");return flag;}
  uint64_t UInt()const{uint64_t n=0;auto r=std::from_chars(text.data(),text.data()+text.size(),n);if(kind!=Number||r.ec!=std::errc{}||r.ptr!=text.data()+text.size())throw std::runtime_error("Expected unsigned JSON integer");return n;}
};
inline std::string Quote(std::string_view s){std::string out="\"";for(unsigned char c:s){if(c=='"'||c=='\\'){out+='\\';out+=char(c);}else if(c<32){out+="\\u00";out+="0123456789abcdef"[c>>4];out+="0123456789abcdef"[c&15];}else out+=char(c);}return out+'"';}
class JsonReader {
  std::string_view input;size_t at=0,nodes=0;
  void Need(bool b){if(!b)throw std::runtime_error("Invalid or excessive JSON document");}
  void Space(){while(at<input.size()&&(input[at]==' '||input[at]=='\t'||input[at]=='\r'||input[at]=='\n'))++at;}
  char Take(){Need(at<input.size());return input[at++];}
  uint32_t Hex4(){uint32_t n=0;for(int i=0;i<4;++i){auto c=Take();n*=16;if(c>='0'&&c<='9')n+=c-'0';else if(c>='a'&&c<='f')n+=c-'a'+10;else if(c>='A'&&c<='F')n+=c-'A'+10;else Need(false);}return n;}
  void Utf8(std::string& s,uint32_t n){if(n<128)s+=char(n);else if(n<2048){s+=char(0xC0|(n>>6));s+=char(0x80|(n&63));}else if(n<65536){s+=char(0xE0|(n>>12));s+=char(0x80|((n>>6)&63));s+=char(0x80|(n&63));}else{s+=char(0xF0|(n>>18));s+=char(0x80|((n>>12)&63));s+=char(0x80|((n>>6)&63));s+=char(0x80|(n&63));}}
  std::string Text(){Need(Take()=='"');std::string s;for(;;){auto c=static_cast<unsigned char>(Take());if(c=='"')return s;Need(c>=32);if(c!='\\'){s+=char(c);continue;}switch(Take()){case'"':s+='"';break;case'\\':s+='\\';break;case'/':s+='/';break;case'b':s+='\b';break;case'f':s+='\f';break;case'n':s+='\n';break;case'r':s+='\r';break;case't':s+='\t';break;case'u':{auto n=Hex4();if(n>=0xD800&&n<=0xDBFF){Need(Take()=='\\'&&Take()=='u');auto low=Hex4();Need(low>=0xDC00&&low<=0xDFFF);n=0x10000+((n-0xD800)<<10)+(low-0xDC00);}else Need(n<0xDC00||n>0xDFFF);Utf8(s,n);break;}default:Need(false);}}}
  Json Value(unsigned depth){Need(depth<48&&++nodes<200000);Space();Json v;auto c=at<input.size()?input[at]:0;
    if(c=='"'){v.kind=Json::String;v.text=Text();}
    else if(c=='{'||c=='['){++at;const bool obj=c=='{';v.kind=obj?Json::Object:Json::Array;Space();if(at<input.size()&&input[at]==(obj?'}':']')){++at;return v;}for(;;){Space();if(obj){auto k=Text();Space();Need(Take()==':');auto child=Value(depth+1);Need(v.object.emplace(std::move(k),std::move(child)).second);}else v.array.push_back(Value(depth+1));Space();auto end=Take();if(end==(obj?'}':']'))break;Need(end==',');}}
    else if(c=='t'||c=='f'||c=='n'){auto word=c=='t'?"true":c=='f'?"false":"null";auto n=std::char_traits<char>::length(word);Need(input.substr(at,n)==word);at+=n;v.kind=c=='n'?Json::Null:Json::Bool;v.flag=c=='t';}
    else{v.kind=Json::Number;auto begin=at;if(c=='-')++at;Need(at<input.size());if(input[at]=='0')++at;else{Need(input[at]>='1'&&input[at]<='9');while(at<input.size()&&input[at]>='0'&&input[at]<='9')++at;}if(at<input.size()&&input[at]=='.'){++at;auto first=at;while(at<input.size()&&input[at]>='0'&&input[at]<='9')++at;Need(at>first);}if(at<input.size()&&(input[at]=='e'||input[at]=='E')){++at;if(at<input.size()&&(input[at]=='+'||input[at]=='-'))++at;auto first=at;while(at<input.size()&&input[at]>='0'&&input[at]<='9')++at;Need(at>first);}v.text=std::string(input.substr(begin,at-begin));}return v;}
public:explicit JsonReader(std::string_view s):input(s){}
  Json Parse(){auto v=Value(0);Space();Need(at==input.size());return v;}
};
inline Json ParseJson(std::string_view s){if(s.starts_with("\xEF\xBB\xBF"))s.remove_prefix(3);return JsonReader(s).Parse();}
}
