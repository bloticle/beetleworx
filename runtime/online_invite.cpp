#include "online_address.h"
#include "online_invite.h"
#include <windows.h>
#include <bcrypt.h>
#include <rex/logging.h>
#include <algorithm>
#include <charconv>
#include <span>
#include <thread>

namespace em2::net {
namespace {
constexpr std::string_view alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZ234567",prefix="EM2-";
template<class T> bool Nonzero(const T& bytes){return std::any_of(bytes.begin(),bytes.end(),[](auto v){return v!=0;});}
bool Valid(const Invite& invite){return invite.port && invite.input_delay<=12 && invite.verification_window>=1 && invite.verification_window<=8 &&
    invite.first_tick<=UINT64_MAX-invite.input_delay && Nonzero(invite.session) && ParseNumericAddress(invite.address).has_value();}
uint32_t Checksum(std::span<const uint8_t> bytes) {
    uint32_t crc=0xFFFFFFFF;
    for(auto byte:bytes){crc^=byte;for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^((crc&1)?0xEDB88320u:0);}
    return ~crc;
}
std::string EncodePayload(std::span<const uint8_t> bytes){
    std::string text(prefix);uint32_t accumulator=0;unsigned bits=0;
    for(auto byte:bytes){accumulator=(accumulator<<8)|byte;bits+=8;while(bits>=5){bits-=5;text+=alphabet[(accumulator>>bits)&31];}}
    if(bits)text+=alphabet[(accumulator<<(5-bits))&31];return text;
}

char Upper(char c){return c>='a'&&c<='z'?char(c-'a'+'A'):c;}
bool Space(char c){return c==' '||c=='\r'||c=='\n'||c=='\t';}
}
std::optional<std::string> EncodeInvite(const Invite& invite) {
    if(!Valid(invite))return {};
    const auto address=*ParseNumericAddress(invite.address);
    const bool extended=invite.verification_window>1;
    std::vector<uint8_t> bytes((address.ipv6()?48:36)+(extended?1:0));size_t at=0;
    auto put=[&](uint64_t value,unsigned count){for(unsigned i=count;i;--i)bytes[at++]=uint8_t(value>>((i-1)*8));};
    put((address.ipv6()?2:1)+(extended?2:0),1);for(unsigned i=0;i<(address.ipv6()?16u:4u);++i)put(address.bytes()[i],1);put(invite.port,2);
    for(auto byte:invite.session)put(byte,1);put(invite.first_tick,8);put(invite.input_delay,1);if(extended)put(invite.verification_window,1);
    put(Checksum(std::span(bytes).first(bytes.size()-4)),4);
    return EncodePayload(bytes);
}
std::optional<Invite> DecodeInvite(std::string_view text) {
    // Bound the input before trimming, allocation or any parsing.
    if(text.size()>128)return {};
    while(!text.empty() && Space(text.front()))text.remove_prefix(1);
    while(!text.empty() && Space(text.back()))text.remove_suffix(1);
    if(text.size()!=62 && text.size()!=64 && text.size()!=81 && text.size()!=83)return {};
    for(size_t i=0;i<prefix.size();++i)if(Upper(text[i])!=prefix[i])return {};
    const auto payload_size=text.size()==62?36:text.size()==64?37:text.size()==81?48:49;
    std::vector<uint8_t> bytes(payload_size);size_t at=0;uint32_t accumulator=0;unsigned bits=0;
    for(char c:text.substr(prefix.size())) {
        const auto digit=alphabet.find(Upper(c));if(digit==std::string_view::npos)return {};
        accumulator=(accumulator<<5)|uint32_t(digit);bits+=5;
        if(bits>=8){bits-=8;if(at>=bytes.size())return {};bytes[at++]=uint8_t(accumulator>>bits);}
    }
    if(at!=bytes.size() || (accumulator&((1u<<bits)-1)))return {};
    at=0;auto get=[&](unsigned count){uint64_t value=0;while(count--)value=(value<<8)|bytes[at++];return value;};
    const auto version=get(1);
    const bool extended=version==3 || version==4,ipv6=version==2 || version==4;
    if(version<1 || version>4 || bytes.size()!=size_t((ipv6?48:36)+(extended?1:0)))return {};
    Invite invite;
    if(!ipv6) {
        for(unsigned i=0;i<4;++i){if(i)invite.address+='.';invite.address+=std::to_string(get(1));}
    } else {
        // Reconstruct a numeric string before applying the shared endpoint
        // validator; reject mapped, scoped, wildcard and multicast invitations.
        for(unsigned i=0;i<8;++i) {
            if(i)invite.address+=':';
            std::array<char,4> word{};const auto value=get(2);
            const auto converted=std::to_chars(word.data(),word.data()+word.size(),value,16);
            invite.address.append(word.data(),converted.ptr);
        }
        const auto address=ParseNumericAddress(invite.address);if(!address)return {};
        invite.address=address->Text();if(invite.address.empty())return {};
    }
    invite.port=uint16_t(get(2));for(auto& byte:invite.session)byte=uint8_t(get(1));
    invite.first_tick=get(8);invite.input_delay=uint8_t(get(1));
    if(extended){invite.verification_window=uint8_t(get(1));if(invite.verification_window<2)return {};}
    if(get(4)!=Checksum(std::span(bytes).first(bytes.size()-4)) || !Valid(invite))return {};
    return invite;
}
std::optional<Identity> IdentityForInvite(const Identity& local,const Invite& invite,bool download_checkpoint) {
    if(!Valid(invite) || !Nonzero(local.build) || !Nonzero(local.content) || (!download_checkpoint && !Nonzero(local.checkpoint)))return {};
    auto identity=local;identity.session=invite.session;identity.first_tick=invite.first_tick;identity.input_delay=invite.input_delay;
    return identity;
}
bool NewSessionNonce(std::array<uint8_t,16>& output) {
    std::array<uint8_t,16> nonce{};
    if(BCryptGenRandom(nullptr,nonce.data(),ULONG(nonce.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0 || !Nonzero(nonce))return false;
    output=nonce;return true;
}
bool TestOnlineInvites() {
    auto check=[](bool ok,const char* message){if(!ok)REXLOG_ERROR("Online invite test: {}",message);return ok;};
    constexpr std::array<uint8_t,9> crc_text{'1','2','3','4','5','6','7','8','9'};
    if(!check(Checksum(crc_text)==0xCBF43926,"standard CRC32 reference"))return false;
    Invite invite{"127.0.0.1",41036};for(unsigned i=0;i<16;++i)invite.session[i]=uint8_t(i+1);
    invite.first_tick=0x100000001ull;invite.input_delay=12;
    const auto encoded=EncodeInvite(invite);
    if(!check(encoded && encoded->size()==62 && DecodeInvite(*encoded)==invite,"endpoint, nonce and 64-bit timeline roundtrip"))return false;
    // Independently generated with Python struct, base64.b32encode and zlib.crc32.
    if(!check(*encoded=="EM2-AF7QAAABUBGACAQDAQCQMBYIBEFAWDANBYHRAAAAAAAQAAAAAEGGXGVKXY","cross-language invitation reference vector"))return false;
    auto ipv6=invite;ipv6.address="2001:db8::1234";
    const auto code6=EncodeInvite(ipv6);
    if(!check(code6 && code6->size()==81 && DecodeInvite(*code6)==ipv6,"IPv6 invitation roundtrip"))return false;
    if(!check(*code6=="EM2-AIQACDNYAAAAAAAAAAAAAAAACI2KATABAIBQIBIGA4EASCQLBQGQ4DYQAAAAAAIAAAAACDAMM5WBS",
        "independent Python IPv6 reference vector"))return false;
    for(size_t i=4;i<code6->size();++i)for(char c:alphabet)if(c!=(*code6)[i]) {
        auto damaged=*code6;damaged[i]=c;
        if(!check(!DecodeInvite(damaged),"every one-character IPv6 invitation corruption rejected"))return false;
    }
    auto unpack=[](std::string_view code){std::vector<uint8_t> bytes;uint32_t value=0;unsigned bits=0;
        for(char digit:code.substr(4)){value=(value<<5)|uint32_t(alphabet.find(digit));bits+=5;if(bits>=8){bits-=8;bytes.push_back(uint8_t(value>>bits));}}return bytes;};
    auto reseal=[](std::vector<uint8_t> bytes){const auto crc=Checksum(std::span(bytes).first(bytes.size()-4));
        for(unsigned i=0;i<4;++i)bytes[bytes.size()-4+i]=uint8_t(crc>>((3-i)*8));return EncodePayload(bytes);};
    for(unsigned window=2;window<=8;++window)for(bool ipv6_address:{false,true}){
        auto modern=ipv6_address?ipv6:invite;modern.verification_window=uint8_t(window);
        const auto code=EncodeInvite(modern);
        if(!check(code && code->size()==(ipv6_address?83u:64u) && DecodeInvite(*code)==modern,
            "every bounded verification window roundtrips with IPv4 and IPv6"))return false;
        auto broken=*code;broken[12]=broken[12]=='A'?'B':'A';
        if(!check(!DecodeInvite(broken) && !DecodeInvite(*code+"A"),"modern invite corruption and extended framing rejected"))return false;
        const auto payload=unpack(*code);
        for(unsigned invalid_window:{0u,1u,9u,255u}){auto invalid_payload=payload;invalid_payload[invalid_payload.size()-5]=uint8_t(invalid_window);
            if(!check(!DecodeInvite(reseal(invalid_payload)),"valid CRC cannot permit out-of-range or noncanonical new-version window"))return false;}
        for(unsigned version:{0u,5u,255u}){auto invalid_payload=payload;invalid_payload[0]=uint8_t(version);
            if(!check(!DecodeInvite(reseal(invalid_payload)),"valid CRC cannot permit unsupported invitation versions"))return false;}
        auto old_version=payload;old_version[0]=ipv6_address?2:1;
        if(!check(!DecodeInvite(reseal(old_version)),"old invitation version cannot consume modern window payload length"))return false;
    }
    for(unsigned invalid_window:{0u,9u}){auto invalid_window_invite=invite;invalid_window_invite.verification_window=uint8_t(invalid_window);
        if(!check(!EncodeInvite(invalid_window_invite),"invalid window rejected before encoding"))return false;}
    auto expanded=ipv6;expanded.address="2001:0DB8:0000:0000:0000:0000:0000:1234";
    if(!check(EncodeInvite(expanded)==code6,"expanded uppercase IPv6 uses canonical binary invitation"))return false;
    for(const auto address:{"::","ff02::1","fe80::1","fe80::1%3","::ffff:127.0.0.1","::127.0.0.1","[::1]","::1:41036","2001:db8::1::2"}) {
        auto invalid6=ipv6;invalid6.address=address;
        if(!check(!EncodeInvite(invalid6),"unsupported or malformed IPv6 invitation rejected"))return false;
    }
    auto embedded6=ipv6;embedded6.address=std::string("::1\0extra",9);
    if(!check(!EncodeInvite(embedded6) && !DecodeInvite(*code6+"A"),"IPv6 embedded null and extended framing rejected"))return false;
    // Version/address-length and trailing base32 padding are checked independently
    // of the CRC: a valid checksum must not make the wrong framing acceptable.
    if(!check(!DecodeInvite("EM2-AEQACDNYAAAAAAAAAAAAAAAACI2KATABAIBQIBIGA4EASCQLBQGQ4DYQAAAAAAIAAAAACDDSD4SL6"),
        "valid CRC cannot make version 1 accept a version 2 payload length"))return false;
    if(!check(ParseNumericAddress("0.0.0.0",true) && ParseNumericAddress("::",true) &&
        !ParseNumericAddress("0.0.0.0") && !ParseNumericAddress("::") && ParseNumericAddress("::1") &&
        ParseNumericAddress("fd12:3456:789a::1"),"wildcards only allowed for binds; loopback and ULA are usable endpoints"))return false;
    auto lowercase=*encoded;for(auto& c:lowercase)if(c>='A'&&c<='Z')c=char(c-'A'+'a');
    if(!check(DecodeInvite(" \r\n"+lowercase+"\t")==invite,"clipboard whitespace and case handling"))return false;
    for(size_t i=4;i<encoded->size();++i)for(char c:alphabet)if(c!=(*encoded)[i]) {
        auto damaged=*encoded;damaged[i]=c;
        if(!check(!DecodeInvite(damaged),"every one-character payload/checksum error is rejected"))return false;
    }
    for(auto address:{"0.0.0.0","224.0.0.1","255.255.255.255","127.1","01.2.3.4","256.1.2.3","127.0.0.1:10","127.0.0.1."}) {
        auto invalid=invite;invalid.address=address;if(!check(!EncodeInvite(invalid),"invalid or non-unicast invite endpoint rejected"))return false;
    }
    auto invalid=invite;invalid.port=0;if(!check(!EncodeInvite(invalid),"zero port rejected"))return false;
    invalid=invite;invalid.input_delay=13;if(!check(!EncodeInvite(invalid),"unsupported delay rejected"))return false;
    invalid=invite;invalid.first_tick=UINT64_MAX;if(!check(!EncodeInvite(invalid),"invitation cannot overflow the delayed input timeline"))return false;
    invalid=invite;invalid.session={};if(!check(!EncodeInvite(invalid),"empty nonce rejected"))return false;
    if(!check(!DecodeInvite(std::string(129,' ')) && !DecodeInvite(encoded->substr(1)) && !DecodeInvite(*encoded+"A"),"bounded length and exact framing"))return false;
    Identity local;local.build[0]=1;local.content[0]=2;local.checkpoint[0]=3;local.session[0]=99;
    const auto joined=IdentityForInvite(local,invite);
    if(!check(joined && joined->build==local.build && joined->content==local.content && joined->checkpoint==local.checkpoint &&
        joined->session==invite.session && joined->first_tick==invite.first_tick && joined->input_delay==12,
        "invitation retains all locally measured compatibility hashes"))return false;
    local.content={};if(!check(!IdentityForInvite(local,invite),"invitation cannot supply missing local compatibility evidence"))return false;
    std::array<uint8_t,16> a{},b{};
    if(!check(NewSessionNonce(a) && NewSessionNonce(b) && a!=b,"fresh OS-generated session nonces"))return false;
    // Real sockets verify that only the invitation's session/timeline is shared.
    local.content[0]=2;local.first_tick=invite.first_tick;local.input_delay=invite.input_delay;local.session=a;
    Session host,client;
    if(!check(host.Host(local,"127.0.0.1",0),"host socket opens"))return false;
    Invite live{"127.0.0.1",host.port(),a,local.first_tick,local.input_delay};
    const auto code=EncodeInvite(live);const auto decoded=code?DecodeInvite(*code):std::nullopt;
    auto own=local;own.session=b;const auto peer=decoded?IdentityForInvite(own,*decoded):std::nullopt;
    if(!check(peer && client.Join(*peer,decoded->address,decoded->port),"client connects with invitation and independent local identity"))return false;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(std::chrono::steady_clock::now()<deadline && (host.state()!=State::Ready || client.state()!=State::Ready)) {
        host.Poll();client.Poll();std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if(!check(host.state()==State::Ready && client.state()==State::Ready && host.identity()==client.identity(),"invitation produces a compatible real TCP session"))return false;
    host.Disconnect();client.Disconnect();
    for(unsigned field=0;field<3;++field) {
        if(!check(host.Host(local,"127.0.0.1",0),"mismatch test host socket opens"))return false;
        live.port=host.port();auto different=local;different.session=b;
        if(field==0)different.build[0]^=0x80;
        if(field==1)different.content[0]^=0x80;
        if(field==2)different.checkpoint[0]^=0x80;
        const auto mismatched=IdentityForInvite(different,live);
        if(!check(mismatched && client.Join(*mismatched,live.address,live.port),"invitation retains mismatched local evidence for handshake rejection"))return false;
        const auto timeout=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(std::chrono::steady_clock::now()<timeout && host.state()!=State::Error && client.state()!=State::Error) {
            host.Poll();client.Poll();std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if(!check(host.state()==State::Error || client.state()==State::Error,"invitation cannot bypass build/content/checkpoint compatibility checks"))return false;
        host.Disconnect();client.Disconnect();
    }
    REXLOG_INFO("Online invite tests passed: old/new window serialization, typo rejection, endpoint bounds, preserved local identities, OS nonces and real socket handshake");return true;
}
}
