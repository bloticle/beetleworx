#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <vector>
namespace em2::archive_detail {
inline void Require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
struct File {
 HANDLE value=INVALID_HANDLE_VALUE;uint64_t size=0;
 explicit File(const std::filesystem::path& path){value=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);LARGE_INTEGER length{};
  if(value==INVALID_HANDLE_VALUE || !GetFileSizeEx(value,&length)){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);throw std::runtime_error("Cannot open archive read-only");}size=uint64_t(length.QuadPart);}
 File(const File&)=delete;File& operator=(const File&)=delete;
 ~File(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
 void Read(uint64_t at,std::span<uint8_t> out){Require(at<=size && out.size()<=size-at && out.size()<=MAXDWORD,"Archive read exceeds its container");if(out.empty())return;LARGE_INTEGER offset{};offset.QuadPart=LONGLONG(at);DWORD got=0;
  Require(SetFilePointerEx(value,offset,nullptr,FILE_BEGIN) && ReadFile(value,out.data(),DWORD(out.size()),&got,nullptr) && got==out.size(),"Short archive read");}
};
struct Hash {
 BCRYPT_ALG_HANDLE provider=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;std::vector<uint8_t> object;
 Hash(){Require(BCryptOpenAlgorithmProvider(&provider,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,"Cannot initialize archive SHA-256");DWORD length=0,got=0;
  try{Require(BCryptGetProperty(provider,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&length),4,&got,0)>=0 && length<=1024*1024,"Invalid archive hash workspace");object.resize(length);Require(BCryptCreateHash(provider,&hash,object.data(),length,nullptr,0,0)>=0,"Cannot initialize archive hash");}
  catch(...){BCryptCloseAlgorithmProvider(provider,0);throw;}}
 Hash(const Hash&)=delete;Hash& operator=(const Hash&)=delete;
 ~Hash(){if(hash)BCryptDestroyHash(hash);if(provider)BCryptCloseAlgorithmProvider(provider,0);}
 void Add(std::span<const uint8_t> bytes){Require(bytes.size()<=ULONG_MAX,"Archive hash chunk is too large");if(!bytes.empty())Require(BCryptHashData(hash,const_cast<PUCHAR>(bytes.data()),ULONG(bytes.size()),0)>=0,"Cannot hash archive bytes");}
 std::array<uint8_t,32> Finish(){std::array<uint8_t,32> out{};Require(BCryptFinishHash(hash,out.data(),DWORD(out.size()),0)>=0,"Cannot finish archive hash");return out;}
};
}
