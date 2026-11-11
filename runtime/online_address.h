#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace em2::net {
// Numeric endpoints only: no resolver work on the UI/simulation thread. Scoped
// link-local addresses cannot be shared as portable invitations (zone IDs are
// local to each machine), so use global/ULA IPv6, loopback, or IPv4.
struct NumericAddress {
    sockaddr_storage storage{};
    int size=0;
    bool ipv6() const {return storage.ss_family==AF_INET6;}
    void Port(uint16_t value) {
        if(ipv6())reinterpret_cast<sockaddr_in6*>(&storage)->sin6_port=htons(value);
        else reinterpret_cast<sockaddr_in*>(&storage)->sin_port=htons(value);
    }
    uint16_t Port() const {
        return ntohs(ipv6()?reinterpret_cast<const sockaddr_in6*>(&storage)->sin6_port:
            reinterpret_cast<const sockaddr_in*>(&storage)->sin_port);
    }
    const uint8_t* bytes() const {
        return ipv6()?reinterpret_cast<const uint8_t*>(&reinterpret_cast<const sockaddr_in6*>(&storage)->sin6_addr):
            reinterpret_cast<const uint8_t*>(&reinterpret_cast<const sockaddr_in*>(&storage)->sin_addr);
    }
    std::string Text() const {
        std::array<char,INET6_ADDRSTRLEN> output{};
        return InetNtopA(storage.ss_family,bytes(),output.data(),output.size())?std::string(output.data()):std::string{};
    }
};
inline bool PublicRouterV4(const NumericAddress& address) {
    if(address.ipv6())return false;const auto* b=address.bytes();
    return b[0]!=0 && b[0]!=10 && b[0]!=127 && b[0]<224 &&
        !(b[0]==100 && (b[1]&192)==64) && !(b[0]==169 && b[1]==254) &&
        !(b[0]==172 && (b[1]&240)==16) && !(b[0]==192 && b[1]==168) &&
        !(b[0]==192 && b[1]==0 && (b[2]==0 || b[2]==2)) &&
        !(b[0]==192 && b[1]==88 && b[2]==99) &&
        !(b[0]==198 && (b[1]==18 || b[1]==19 || (b[1]==51 && b[2]==100))) &&
        !(b[0]==203 && b[1]==0 && b[2]==113);
}
inline std::optional<NumericAddress> ParseNumericAddress(std::string_view text,bool allow_wildcard=false) {
    if(text.empty() || text.size()>=INET6_ADDRSTRLEN || text.find('\0')!=std::string_view::npos)return {};
    NumericAddress result;
    if(text.find(':')==std::string_view::npos) {
        std::array<uint8_t,4> bytes{};
        for(unsigned i=0;i<4;++i) {
            const auto end=text.find('.');const auto part=text.substr(0,end);
            unsigned value=0;const auto parsed=std::from_chars(part.data(),part.data()+part.size(),value);
            if(part.empty() || (part.size()>1 && part[0]=='0') || parsed.ec!=std::errc{} || parsed.ptr!=part.data()+part.size() || value>255)return {};
            bytes[i]=uint8_t(value);
            if(i==3){if(end!=std::string_view::npos)return {};}
            else {if(end==std::string_view::npos)return {};text.remove_prefix(end+1);}
        }
        const bool wildcard=bytes==std::array<uint8_t,4>{};
        if((bytes[0]==0 && !(allow_wildcard && wildcard)) || bytes[0]>=224)return {};
        auto& address=*reinterpret_cast<sockaddr_in*>(&result.storage);
        address.sin_family=AF_INET;std::memcpy(&address.sin_addr,bytes.data(),4);result.size=sizeof(address);
    } else {
        auto& address=*reinterpret_cast<sockaddr_in6*>(&result.storage);
        address.sin6_family=AF_INET6;result.size=sizeof(address);
        if(InetPtonA(AF_INET6,std::string(text).c_str(),&address.sin6_addr)!=1)return {};
        const auto* bytes=result.bytes();
        const bool prefix_zero=std::all_of(bytes,bytes+12,[](auto b){return b==0;});
        const bool loopback=prefix_zero && bytes[12]==0 && bytes[13]==0 && bytes[14]==0 && bytes[15]==1;
        const bool wildcard=prefix_zero && std::all_of(bytes+12,bytes+16,[](auto b){return b==0;});
        const bool mapped=std::all_of(bytes,bytes+10,[](auto b){return b==0;}) && bytes[10]==255 && bytes[11]==255;
        if(bytes[0]==255 || (bytes[0]==254 && (bytes[1]&192)==128) || mapped ||
            (prefix_zero && !loopback && !(allow_wildcard && wildcard)))return {};
    }
    return result;
}
}
