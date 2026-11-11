#include "online_name_input.h"
#include <algorithm>
#include <rex/logging.h>
namespace em2::net {
namespace {
void Append(std::string& out,char32_t c){
    if(c<128)out.push_back(char(c));
    else if(c<2048){out.push_back(char(0xC0|(c>>6)));out.push_back(char(0x80|(c&63)));}
    else if(c<65536){out.push_back(char(0xE0|(c>>12)));out.push_back(char(0x80|((c>>6)&63)));out.push_back(char(0x80|(c&63)));}
    else {out.push_back(char(0xF0|(c>>18)));out.push_back(char(0x80|((c>>12)&63)));out.push_back(char(0x80|((c>>6)&63)));out.push_back(char(0x80|(c&63)));}
}
bool Allowed(char32_t c){return c>=32 && !(c>=127 && c<=159) && !(c>=0xD800 && c<=0xDFFF) && c<=0x10FFFF && c!=0x2028 && c!=0x2029;}
}
bool LobbyNameInput::Begin(std::string_view source){
    std::u32string decoded;
    for(size_t i=0;i<source.size();){
        auto first=uint8_t(source[i++]);char32_t c=first;unsigned extra=0;
        if(first>=0xC2 && first<=0xDF){c=first&31;extra=1;}
        else if(first>=0xE0 && first<=0xEF){c=first&15;extra=2;}
        else if(first>=0xF0 && first<=0xF4){c=first&7;extra=3;}
        else if(first>=128)return false;
        auto count=extra;
        while(extra--){if(i==source.size() || (uint8_t(source[i])&0xC0)!=0x80)return false;c=(c<<6)|(uint8_t(source[i++])&63);}
        if(!Allowed(c) || (count==1 && c<128) || (count==2 && c<2048) || (count==3 && c<65536) || decoded.size()==24)return false;
        decoded.push_back(c);
    }
    text_=std::move(decoded);cursor_=text_.size();anchor_=0;high_=0;active_=true;return true;
}
void LobbyNameInput::EraseSelection(){
    const auto first=std::min(cursor_,anchor_),last=std::max(cursor_,anchor_);
    text_.erase(first,last-first);cursor_=anchor_=first;
}
bool LobbyNameInput::Insert(char32_t c){
    if(!Allowed(c) || text_.size()-(std::max(cursor_,anchor_)-std::min(cursor_,anchor_))>=24)return false;
    EraseSelection();text_.insert(text_.begin()+cursor_,c);anchor_=++cursor_;return true;
}
bool LobbyNameInput::Character(uint32_t unit){
    if(!active_)return false;
    if(unit>=0xD800 && unit<=0xDBFF){high_=uint16_t(unit);return true;}
    const auto high=high_;high_=0;
    if(unit>=0xDC00 && unit<=0xDFFF){if(high)Insert(0x10000+((high-0xD800)<<10)+(unit-0xDC00));return true;}
    if(unit<=0x10FFFF)Insert(char32_t(unit));return true;
}
void LobbyNameInput::Key(unsigned key,bool shift,bool control){
    if(!active_)return;high_=0;
    if(control && key=='A'){anchor_=0;cursor_=text_.size();return;}
    if(key==0x08 || key==0x2E){
        if(anchor_!=cursor_){EraseSelection();return;}
        if(key==0x08 && cursor_){--cursor_;text_.erase(cursor_,1);anchor_=cursor_;}
        else if(key==0x2E && cursor_<text_.size())text_.erase(cursor_,1);
        return;
    }
    size_t next=cursor_;
    if(key==0x24)next=0;
    else if(key==0x23)next=text_.size();
    else if(key==0x25)next=!shift && anchor_!=cursor_?std::min(anchor_,cursor_):(cursor_?cursor_-1:0);
    else if(key==0x27)next=!shift && anchor_!=cursor_?std::max(anchor_,cursor_):std::min(cursor_+1,text_.size());
    else return;
    cursor_=next;if(!shift)anchor_=cursor_;
}
std::string LobbyNameInput::text()const{std::string out;for(auto c:text_)Append(out,c);return out;}
unsigned LobbyNameInput::cursor_utf16()const{unsigned n=0;for(size_t i=0;i<cursor_;++i)n+=text_[i]>=0x10000?2:1;return n;}
unsigned LobbyNameInput::anchor_utf16()const{unsigned n=0;for(size_t i=0;i<anchor_;++i)n+=text_[i]>=0x10000?2:1;return n;}
bool TestLobbyNameInput(){
    bool ok=true;auto check=[&](bool value,const char* why){if(!value){REXLOG_ERROR("Lobby name input: {}",why);ok=false;}};
    LobbyNameInput edit;check(edit.Begin("Old name"),"initial valid name");edit.Character('B');edit.Character('o');edit.Character('b');
    check(edit.text()=="Bob","typing B replaces selection instead of invoking Back");
    edit.Key(0x25,false,false);edit.Character('!');check(edit.text()=="Bo!b","insert at cursor");
    edit.Key(0x08,false,false);edit.Key(0x2E,false,false);check(edit.text()=="Bo","backspace/delete operate at cursor");
    edit.Key('A',false,true);edit.Character(0xD83D);edit.Character(0xDE00);
    check(edit.text()=="\xF0\x9F\x98\x80" && edit.cursor_utf16()==2 && edit.anchor_utf16()==2,"UTF16 surrogate pair becomes one scalar");
    edit.Character(0xDE00);edit.Character(0x2028);edit.Character('\n');check(edit.text()=="\xF0\x9F\x98\x80","orphan surrogate and controls ignored");
    edit.Key(0x24,false,false);edit.Key(0x23,true,false);edit.Character('X');check(edit.text()=="X","shift selection replacement");
    edit.Begin("");for(unsigned i=0;i<30;++i){edit.Character(0xD83D);edit.Character(0xDE00);}
    check(edit.text().size()==96 && edit.cursor_utf16()==48,"24 nonBMP scalars and 96byte maximum");
    edit.Key(0x08,false,false);check(edit.text().size()==92,"backspace removes complete nonBMP scalar");
    const auto before=edit.text();check(!edit.Begin("\xC0\xAF") && edit.text()==before,"malformed initial UTF8 rejected atomically");
    edit.End();check(!edit.Character('A') && edit.text()==before,"inactive editor cannot capture text");
    if(ok)REXLOG_INFO("Native lobby name input passed: selection, B character, cursor, delete/backspace, Unicode pairs, controls, bounds and inactive ownership");return ok;
}
}
