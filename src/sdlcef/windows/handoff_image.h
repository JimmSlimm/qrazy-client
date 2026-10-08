#pragma once
#include "handoff_pipe.h"
#include <filesystem>
namespace Handoff {
inline bool NoLinks(std::filesystem::path p) {
  for(;;){DWORD a=GetFileAttributesW(p.c_str());if(a==INVALID_FILE_ATTRIBUTES||a&FILE_ATTRIBUTE_REPARSE_POINT)return false;auto next=p.parent_path();if(next==p||next.empty())break;p=next;}return true;
}
class ImagePin {
 public:
  Handle file;std::filesystem::path path;
  bool Open(const std::filesystem::path& source,const std::string& expected) {
    if(expected.size()!=64||!NoLinks(source)||file.value!=INVALID_HANDLE_VALUE)return false;
    path=std::filesystem::absolute(source).lexically_normal();
    file.value=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(file.value==INVALID_HANDLE_VALUE)return false;
    BY_HANDLE_FILE_INFORMATION info{};if(!GetFileInformationByHandle(file.value,&info)||info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))return false;
    BCRYPT_ALG_HANDLE alg=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
    if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return false;
    bool ok=BCryptCreateHash(alg,&hash,nullptr,0,nullptr,0,0)>=0;std::array<unsigned char,65536> data{};DWORD n=0;
    while(ok){if(!ReadFile(file.value,data.data(),static_cast<DWORD>(data.size()),&n,nullptr)){ok=false;break;}if(!n)break;ok=BCryptHashData(hash,data.data(),n,0)>=0;}
    unsigned char digest[32]{};if(ok)ok=BCryptFinishHash(hash,digest,32,0)>=0;
    if(hash)BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(alg,0);
    std::string actual;for(auto b:digest){actual+="0123456789abcdef"[b>>4];actual+="0123456789abcdef"[b&15];}
    return ok&&actual==expected;
  }
  bool Process(HANDLE peer) const {
    wchar_t image[32768]{};DWORD size=32768;
    if(!peer||peer==INVALID_HANDLE_VALUE||WaitForSingleObject(peer,0)!=WAIT_TIMEOUT||!SameUser(peer)||
      !QueryFullProcessImageNameW(peer,0,image,&size))return false;
    return CompareStringOrdinal(path.c_str(),-1,image,static_cast<int>(size),TRUE)==CSTR_EQUAL;
  }
};
}
