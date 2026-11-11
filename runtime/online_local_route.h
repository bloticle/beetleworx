#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
namespace em2::net {
struct LocalAddressInterface {uint32_t index;bool ipv6;};
// Interface enumeration order may favor a virtual adapter that has no default
// route. Prefer the routed IPv4 interface for the lobby's NAT-capable default,
// then routed IPv6; retain enumeration order for ties and offline fallback.
constexpr std::size_t PreferredLocalAddress(std::span<const LocalAddressInterface> addresses,uint32_t ipv4_route,uint32_t ipv6_route){
    std::size_t best=0;unsigned score=0;
    for(std::size_t i=0;i<addresses.size();++i){const auto& address=addresses[i];const unsigned candidate=address.index && address.index==(address.ipv6?ipv6_route:ipv4_route)?(address.ipv6?1:2):0;if(candidate>score){best=i;score=candidate;}}
    return best;
}
namespace local_route_checks {
// A low-metric VPN enumerated first must not hide the actual Internet route.
constexpr std::array mixed{LocalAddressInterface{16,false},LocalAddressInterface{9,true},LocalAddressInterface{9,false}};
static_assert(PreferredLocalAddress(mixed,9,9)==2);
static_assert(PreferredLocalAddress(mixed,0,9)==1);
static_assert(PreferredLocalAddress(mixed,0,0)==0);
static_assert(PreferredLocalAddress(mixed,16,9)==0);
// Family-specific indices must not select a candidate from the other family.
constexpr std::array families{LocalAddressInterface{7,true},LocalAddressInterface{8,false}};
static_assert(PreferredLocalAddress(families,7,8)==0);
// Multiple addresses on the preferred adapter keep their existing order.
constexpr std::array ties{LocalAddressInterface{3,false},LocalAddressInterface{8,false},LocalAddressInterface{8,false}};
static_assert(PreferredLocalAddress(ties,8,0)==1);
static_assert(PreferredLocalAddress({},0,0)==0);
}
}
