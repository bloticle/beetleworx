#pragma once
#include <bit>
#include <cmath>
#include <cstdint>
namespace em2::em1::gekko_math {
// Shared Gekko estimate table already used by the original lighting adapter.
inline double ReciprocalSqrtEstimate(float value) {
  constexpr int64_t table[32][2]={
    {0x1a7e800,-0x568},{0x17cb800,-0x4f3},{0x1552800,-0x48d},{0x130c000,-0x435},
    {0x10f2000,-0x3e7},{0x0eff000,-0x3a2},{0x0d2e000,-0x365},{0x0b7c000,-0x32e},
    {0x09e5000,-0x2fc},{0x0867000,-0x2d0},{0x06ff000,-0x2a8},{0x05ab800,-0x283},
    {0x046a000,-0x261},{0x0339800,-0x243},{0x0218800,-0x226},{0x0105800,-0x20b},
    {0x3ffa000,-0x7a4},{0x3c29000,-0x700},{0x38aa000,-0x670},{0x3572000,-0x5f2},
    {0x3279000,-0x584},{0x2fb7000,-0x524},{0x2d26000,-0x4cc},{0x2ac0000,-0x47e},
    {0x2881000,-0x43a},{0x2665000,-0x3fa},{0x2468000,-0x3c2},{0x2287000,-0x38e},
    {0x20c1000,-0x35e},{0x1f12000,-0x332},{0x1d79000,-0x30a},{0x1bf4000,-0x2e6}};
  const auto bits=std::bit_cast<uint64_t>(double(value));
  const int exponent=int((bits>>52)&2047)-1023;
  const auto fraction=(bits&((uint64_t(1)<<52)-1))>>37;
  const int bank=exponent%2==0?16:0;
  const auto& row=table[bank+(fraction>>11)];
  const uint64_t mantissa=uint64_t(row[0]+row[1]*int64_t(fraction&2047));
  const int half=exponent/2-(exponent<0 && exponent%2!=0);
  return std::bit_cast<double>((uint64_t(1022-half)<<52)|(mantissa<<26));
}
// Retail DOL800071F0. fmuls rounds its second operand to25 significant bits.
inline float FastSqrt(float value){
  if(value==0)return 0;
  const double estimate=ReciprocalSqrtEstimate(value);const auto bits=std::bit_cast<uint64_t>(estimate);
  const double operand=std::bit_cast<double>(((bits>>28)+((bits>>27)&1))<<28);
  volatile float square=float(estimate*operand),half=float(estimate*.5);
  volatile float correction=std::fma(-float(square),value,3.f);
  volatile float inverse=float(correction)*float(half);
  volatile float result=value*(inverse>=0?float(inverse):value);
  return result;
}
}
