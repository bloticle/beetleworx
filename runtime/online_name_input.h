#pragma once
#include <cstdint>
#include <string>
#include <string_view>
namespace em2::net {
// Window-owned UTF-16 events, native-menu-owned lifetime. Caller serializes access.
class LobbyNameInput {
public:
    explicit LobbyNameInput(size_t scalars=24,size_t bytes=96):max_scalars_(scalars),max_bytes_(bytes){}
    bool Begin(std::string_view utf8);
    void End(){active_=false;high_=0;}
    bool active()const{return active_;}
    bool Character(uint32_t utf16);
    void Key(unsigned key,bool shift,bool control);
    std::string text()const;
    unsigned cursor_utf16()const;
    unsigned anchor_utf16()const;
private:
    bool Insert(char32_t);
    void EraseSelection();
    std::u32string text_;
    size_t cursor_=0,anchor_=0;
    uint16_t high_=0;
    bool active_=false;
    size_t max_scalars_=24,max_bytes_=96;
};
bool TestLobbyNameInput();
}
