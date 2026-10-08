#pragma once
// Native Windows QAS1 store; same asset accounting and renderer contract.
// Serialized background I/O only. No game download or gameplay logic.
#include <bcrypt.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <memory>
#include <atomic>
#include <cwctype>
#include "include/cef_base.h"
#pragma comment(lib,"bcrypt.lib")
namespace QrazyWindows {
constexpr uint64_t AssetTotal=16ull*1024*1024*1024,AssetReserve=1024ull*1024*1024;
constexpr DWORD AssetHeader=4096,AssetChunk=1048576;
inline bool NoReparse(const std::filesystem::path& path) {
  for(auto p=path;!p.empty();p=p.parent_path()) {
    DWORD attr=GetFileAttributesW(p.c_str());
    if(attr!=INVALID_FILE_ATTRIBUTES&&(attr&FILE_ATTRIBUTE_REPARSE_POINT))return false;
    if(p==p.parent_path())break;
  }return true;
}
struct File {
  HANDLE handle=INVALID_HANDLE_VALUE;
  ~File(){Close();}
  void Close(){if(handle!=INVALID_HANDLE_VALUE){CloseHandle(handle);handle=INVALID_HANDLE_VALUE;}}
  bool Open(const std::filesystem::path& p,DWORD access,DWORD creation,DWORD sharing=FILE_SHARE_READ) {
    if(!NoReparse(p))return false;
    handle=CreateFileW(p.c_str(),access,sharing,nullptr,creation,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    BY_HANDLE_FILE_INFORMATION info{};
    return handle!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(handle,&info)&&!(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY));
  }
  bool Read(void* bytes,DWORD size){DWORD n=0;return ReadFile(handle,bytes,size,&n,nullptr)&&n==size;}
  bool Write(const void* bytes,DWORD size){DWORD n=0;return WriteFile(handle,bytes,size,&n,nullptr)&&n==size;}
  uint64_t Size(){LARGE_INTEGER n{};return GetFileSizeEx(handle,&n)?static_cast<uint64_t>(n.QuadPart):UINT64_MAX;}
};
class Hash {
  BCRYPT_ALG_HANDLE algorithm_=nullptr;
  BCRYPT_HASH_HANDLE hash_=nullptr;
 public:
  Hash(){if(BCryptOpenAlgorithmProvider(&algorithm_,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0)BCryptCreateHash(algorithm_,&hash_,nullptr,0,nullptr,0,0);}
  ~Hash(){if(hash_)BCryptDestroyHash(hash_);if(algorithm_)BCryptCloseAlgorithmProvider(algorithm_,0);}
  bool Add(const void* data,size_t size){return hash_&&size<=UINT32_MAX&&BCryptHashData(hash_,reinterpret_cast<PUCHAR>(const_cast<void*>(data)),static_cast<ULONG>(size),0)>=0;}
  std::string Finish(){unsigned char bytes[32];if(!hash_||BCryptFinishHash(hash_,bytes,32,0)<0)return {};static const char hex[]="0123456789abcdef";std::string value;for(auto b:bytes){value+=hex[b>>4];value+=hex[b&15];}return value;}
};
inline std::string Serialize(CefRefPtr<CefDictionaryValue> d){auto v=CefValue::Create();v->SetDictionary(d);return CefWriteJSON(v,JSON_WRITER_DEFAULT);}
inline bool Digest(const std::string& value){return value.size()==64&&value.find_first_not_of("0123456789abcdef")==std::string::npos;}
inline CefRefPtr<CefDictionaryValue> Error(const char* text){auto d=CefDictionaryValue::Create();d->SetBool("ok",false);d->SetString("error",text);return d;}
inline CefRefPtr<CefDictionaryValue> Success(CefRefPtr<CefValue> value=nullptr){auto d=CefDictionaryValue::Create();d->SetBool("ok",true);if(value)d->SetValue("data",value);return d;}
inline CefRefPtr<CefValue> Value(CefRefPtr<CefDictionaryValue> d){auto v=CefValue::Create();v->SetDictionary(d);return v;}
inline CefRefPtr<CefDictionaryValue> Config(const std::string& op,CefRefPtr<CefListValue> args) {
  if(!args||args->GetSize()!=(op=="config-write"?2u:1u)||args->GetType(0)!=VTYPE_STRING)return Error("Invalid config selection");
  std::filesystem::path path(args->GetString(0).ToWString());auto extension=path.extension().wstring();
  std::transform(extension.begin(),extension.end(),extension.begin(),[](wchar_t ch){return static_cast<wchar_t>(towlower(ch));});
  if(!path.is_absolute()||extension!=L".cfg"||!NoReparse(path))return Error("Expected a regular cfg file");
  auto result=CefDictionaryValue::Create();result->SetBool("cancelled",false);
  if(op=="config-read") {
    File file;if(!file.Open(path,GENERIC_READ,OPEN_EXISTING)||file.Size()>AssetChunk)return Error("Config is unavailable or exceeds 1 MiB");
    std::string text(static_cast<size_t>(file.Size()),'\0');if(!file.Read(text.data(),static_cast<DWORD>(text.size())))return Error("Config read failed");
    if(text.rfind("\xef\xbb\xbf",0)==0)text.erase(0,3);
    if(text.find('\0')!=std::string::npos||(!text.empty()&&!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0)))return Error("Expected UTF-8 config text");
    result->SetString("name",path.filename().wstring());result->SetString("text",text);
  }else {
    if(args->GetType(1)!=VTYPE_STRING)return Error("Invalid config text");std::string text=args->GetString(1);
    if(text.size()>AssetChunk||text.find('\0')!=std::string::npos)return Error("Config exceeds 1 MiB or contains invalid text");
    unsigned char random[16];if(BCryptGenRandom(nullptr,random,sizeof(random),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)return Error("Config save unavailable");Hash hash;hash.Add(random,sizeof(random));
    auto temp=path.parent_path()/(L"."+path.filename().wstring()+L"."+CefString(hash.Finish()).ToWString()+L".partial");
    File file;if(!file.Open(temp,GENERIC_WRITE,CREATE_NEW,0))return Error("Config save unavailable");
    bool ok=file.Write(text.data(),static_cast<DWORD>(text.size()))&&FlushFileBuffers(file.handle);file.Close();
    if(ok&&NoReparse(path))ok=MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);else ok=false;
    if(!ok){DeleteFileW(temp.c_str());return Error("Config replacement failed; original file retained");}
  }return Success(Value(result));
}
struct Transfer {
  File file;Hash hash;
  std::filesystem::path temporary,destination;
  std::string kind,key,expected;
  uint64_t size=0,offset=0;
  bool writing=false;
  ~Transfer(){file.Close();if(!temporary.empty())DeleteFileW(temporary.c_str());}
};
class Assets {
  uint64_t total_,reserve_;
  std::filesystem::path directory_;
  std::map<std::string,std::unique_ptr<Transfer>> transfers_;
  static int Limit(const std::string& kind){return kind=="sound"?64*static_cast<int>(AssetChunk):kind=="map"||kind=="shader"||kind=="texture"?512*static_cast<int>(AssetChunk):0;}
  std::filesystem::path Filename(const std::string& kind,const std::string& key){Hash hash;hash.Add(kind.data(),kind.size());char zero=0;hash.Add(&zero,1);hash.Add(key.data(),key.size());auto name=hash.Finish();return name.empty()?std::filesystem::path():directory_/(name+".asset");}
  bool Key(const std::string& kind,const std::string& key){if(!Limit(kind)||key.empty()||key.size()>2048)return false;for(unsigned char ch:key)if(ch<32||ch==127)return false;return true;}
  std::string Token(){unsigned char bytes[16];if(BCryptGenRandom(nullptr,bytes,sizeof(bytes),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)return {};Hash hash;hash.Add(bytes,sizeof(bytes));return hash.Finish();}
  bool Room(uint64_t size) {
    uint64_t used=0,reserved=0;
    std::error_code error;std::filesystem::directory_iterator it(directory_,error);if(error)return false;
    for(const auto& entry:it){if(!NoReparse(entry.path()))return false;File f;if(!f.Open(entry.path(),GENERIC_READ,OPEN_EXISTING,FILE_SHARE_READ|FILE_SHARE_WRITE))return false;auto n=f.Size();if(n>total_-used)return false;used+=n;}
    for(auto& item:transfers_)if(item.second->writing)reserved+=item.second->size-item.second->offset;
    uint64_t required=AssetHeader+size;ULARGE_INTEGER free{};
    return used<=total_&&reserved<=total_-used&&required<=total_-used-reserved&&GetDiskFreeSpaceExW(directory_.c_str(),&free,nullptr,nullptr)&&free.QuadPart>=reserve_+reserved+required;
  }
  std::unique_ptr<Transfer> ReadMetadata(const std::string& kind,const std::string& key,bool& missing) {
    auto result=std::make_unique<Transfer>();auto filename=Filename(kind,key);missing=GetFileAttributesW(filename.c_str())==INVALID_FILE_ATTRIBUTES&&GetLastError()==ERROR_FILE_NOT_FOUND;
    if(!result->file.Open(filename,GENERIC_READ,OPEN_EXISTING))return nullptr;
    unsigned char header[AssetHeader];if(!result->file.Read(header,AssetHeader)||memcmp(header,"QAS1",4))return nullptr;
    uint32_t len=0;memcpy(&len,header+4,4);if(!len||len>AssetHeader-8)return nullptr;
    auto value=CefParseJSON(std::string(reinterpret_cast<char*>(header+8),len),JSON_PARSER_RFC);auto d=value&&value->GetType()==VTYPE_DICTIONARY?value->GetDictionary():nullptr;
    if(!d||d->GetString("kind")!=kind||d->GetString("key")!=key||d->GetType("size")!=VTYPE_INT||d->GetInt("size")<1||d->GetInt("size")>Limit(kind)||!Digest(d->GetString("sha256"))||result->file.Size()!=AssetHeader+static_cast<uint64_t>(d->GetInt("size")))return nullptr;
    result->kind=kind;result->key=key;result->size=d->GetInt("size");result->expected=d->GetString("sha256");return result;
  }
 public:
  explicit Assets(uint64_t total=AssetTotal,uint64_t reserve=AssetReserve):total_(total),reserve_(reserve){}
  bool Initialize(const std::filesystem::path& profile){directory_=profile/"desktop-assets";std::error_code error;if(!NoReparse(directory_))return false;std::filesystem::create_directories(directory_,error);return !error&&NoReparse(directory_);}
  void Reset(){transfers_.clear();}
  CefRefPtr<CefDictionaryValue> Call(const std::string& op,CefRefPtr<CefListValue> args) {
    if(op=="reset"){Reset();return Success();}
    if(!args)return Error("Invalid asset request");
    if(op=="has"||op=="openRead") {
      if(args->GetSize()!=2||args->GetType(0)!=VTYPE_STRING||args->GetType(1)!=VTYPE_STRING)return Error("Invalid asset key");
      std::string kind=args->GetString(0),key=args->GetString(1);if(!Key(kind,key))return Error("Invalid asset key");
      bool missing=false;auto t=ReadMetadata(kind,key,missing);
      if(!t&&!missing)return Error("Desktop asset is corrupt or unavailable; download it again");
      auto value=CefValue::Create();
      if(op=="has"){value->SetBool(!!t);return Success(value);}
      if(!t){value->SetNull();return Success(value);}if(transfers_.size()>=4)return Error("At most four transfers may be open");
      auto token=Token();if(token.empty())return Error("Desktop storage unavailable");
      auto data=CefDictionaryValue::Create();data->SetString("token",token);data->SetInt("size",static_cast<int>(t->size));data->SetString("sha256",t->expected);transfers_[token]=std::move(t);return Success(Value(data));
    }
    if(op=="beginWrite") {
      if(args->GetSize()!=1||args->GetType(0)!=VTYPE_DICTIONARY)return Error("Invalid asset descriptor");
      auto d=args->GetDictionary(0);std::string kind=d->GetString("kind"),key=d->GetString("key"),expected=d->GetString("sha256");
      int size=d->GetInt("size");if(!Key(kind,key)||d->GetType("size")!=VTYPE_INT||size<1||size>Limit(kind)||
        (d->HasKey("sha256")&&d->GetType("sha256")!=VTYPE_NULL&&d->GetType("sha256")!=VTYPE_STRING)||(!expected.empty()&&!Digest(expected)))return Error("Invalid asset descriptor");
      if(transfers_.size()>=4)return Error("At most four transfers may be open");
      if(!Room(size))return Error("Desktop saving needs room within 16 GiB and a 1 GiB free reserve; existing downloads are preserved");
      auto token=Token();if(token.empty())return Error("Desktop storage unavailable");auto t=std::make_unique<Transfer>();t->writing=true;t->kind=kind;t->key=key;t->size=size;t->expected=expected;t->destination=Filename(kind,key);auto temporary=directory_/(token+".partial");
      if(!t->file.Open(temporary,GENERIC_READ|GENERIC_WRITE,CREATE_NEW,FILE_SHARE_READ|FILE_SHARE_WRITE))return Error("Desktop storage unavailable");
      t->temporary=temporary;unsigned char header[AssetHeader]{};if(!t->file.Write(header,AssetHeader))return Error("Desktop storage unavailable");
      auto data=CefDictionaryValue::Create();data->SetString("token",token);data->SetInt("maxChunkBytes",AssetChunk);transfers_[token]=std::move(t);return Success(Value(data));
    }
    if(args->GetSize()<1||args->GetType(0)!=VTYPE_STRING)return Error("Invalid transfer token");
    std::string token=args->GetString(0);auto found=transfers_.find(token);if(found==transfers_.end())return Error("Unknown or expired transfer");auto& t=*found->second;
    bool writing=op=="writeChunk"||op=="finishWrite"||op=="abortWrite";
    if(t.writing!=writing)return Error("Wrong transfer mode");
    if(op=="closeRead"||op=="abortWrite"){transfers_.erase(found);return Success();}
    if(op=="readChunk") {
      std::vector<unsigned char> bytes(static_cast<size_t>(std::min<uint64_t>(AssetChunk,t.size-t.offset)));
      if(!t.file.Read(bytes.data(),static_cast<DWORD>(bytes.size()))||!t.hash.Add(bytes.data(),bytes.size())){transfers_.erase(found);return Error("Asset read failed");}
      t.offset+=bytes.size();bool done=t.offset==t.size;
      if(done&&t.hash.Finish()!=t.expected){transfers_.erase(found);return Error("Asset checksum mismatch; download it again");}
      auto data=CefDictionaryValue::Create();data->SetString("base64",CefBase64Encode(bytes.data(),bytes.size()));data->SetBool("done",done);if(done)transfers_.erase(found);return Success(Value(data));
    }
    if(op=="writeChunk") {
      if(args->GetSize()!=2||args->GetType(1)!=VTYPE_STRING||args->GetString(1).length()>1400000){transfers_.erase(found);return Error("Invalid chunk");}
      auto binary=CefBase64Decode(args->GetString(1));size_t n=binary?binary->GetSize():0;
      if(!n||n>AssetChunk||n>t.size-t.offset){transfers_.erase(found);return Error("Invalid chunk size");}
      std::vector<unsigned char> bytes(n);binary->GetData(bytes.data(),n,0);
      if(CefBase64Encode(bytes.data(),n)!=args->GetString(1)){transfers_.erase(found);return Error("Invalid chunk encoding");}
      if(!t.file.Write(bytes.data(),static_cast<DWORD>(n))||!t.hash.Add(bytes.data(),n)){transfers_.erase(found);return Error("Asset write failed");}t.offset+=n;return Success();
    }
    if(op=="finishWrite") {
      auto digest=t.hash.Finish();
      if(t.offset!=t.size||digest.empty()||(!t.expected.empty()&&t.expected!=digest)){transfers_.erase(found);return Error("Incomplete asset or checksum mismatch");}
      auto d=CefDictionaryValue::Create();d->SetString("kind",t.kind);d->SetString("key",t.key);d->SetInt("size",static_cast<int>(t.size));d->SetString("sha256",digest);auto json=Serialize(d);
      if(json.size()>AssetHeader-8){transfers_.erase(found);return Error("Asset metadata too large");}
      unsigned char header[AssetHeader]{};memcpy(header,"QAS1",4);uint32_t len=static_cast<uint32_t>(json.size());memcpy(header+4,&len,4);memcpy(header+8,json.data(),json.size());LARGE_INTEGER zero{};
      bool ok=SetFilePointerEx(t.file.handle,zero,nullptr,FILE_BEGIN)&&t.file.Write(header,AssetHeader)&&FlushFileBuffers(t.file.handle);t.file.Close();
      if(!ok||!NoReparse(t.destination)||!MoveFileExW(t.temporary.c_str(),t.destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){transfers_.erase(found);return Error("Asset replacement failed; original download retained");}
      t.temporary.clear();transfers_.erase(found);return Success(Value(d));
    }
    return Error("Unknown asset operation");
  }
};
struct AssetJob {int id;unsigned epoch;std::string op,response;CefRefPtr<CefListValue> args;};
class AssetWorker {
  Assets assets_;std::thread thread_;std::mutex mutex_;std::condition_variable wake_;
  std::deque<AssetJob> queue_,replies_;bool stopping_=false;size_t pending_=0;
 public:
  std::atomic<unsigned> epoch{0};
  ~AssetWorker(){Stop();}
  bool Start(const std::filesystem::path& profile){if(!assets_.Initialize(profile))return false;thread_=std::thread([this]{for(;;){AssetJob job;{std::unique_lock<std::mutex> lock(mutex_);wake_.wait(lock,[&]{return stopping_||!queue_.empty();});if(stopping_)break;job=std::move(queue_.front());queue_.pop_front();}if(job.op=="reset"||job.epoch==epoch.load())job.response=Serialize(job.op=="config-read"||job.op=="config-write"?Config(job.op,job.args):assets_.Call(job.op,job.args));{std::lock_guard<std::mutex> lock(mutex_);--pending_;if(job.id&&!job.response.empty())replies_.push_back(std::move(job));}}assets_.Reset();});return true;}
  bool Submit(int id,const std::string& op,CefRefPtr<CefListValue> args){std::lock_guard<std::mutex> lock(mutex_);if(!thread_.joinable()||stopping_||pending_>=16)return false;++pending_;queue_.push_back({id,epoch.load(),op,{},args->Copy()});wake_.notify_one();return true;}
  void Reset(){++epoch;std::lock_guard<std::mutex> lock(mutex_);pending_-=queue_.size();queue_.clear();replies_.clear();if(thread_.joinable()&&!stopping_){++pending_;queue_.push_back({0,epoch.load(),"reset",{},nullptr});wake_.notify_one();}}
  std::deque<AssetJob> Take(){std::lock_guard<std::mutex> lock(mutex_);std::deque<AssetJob> result;result.swap(replies_);return result;}
  void Stop(){{std::lock_guard<std::mutex> lock(mutex_);stopping_=true;wake_.notify_one();}if(thread_.joinable()){CancelSynchronousIo(thread_.native_handle());thread_.join();}std::lock_guard<std::mutex> lock(mutex_);queue_.clear();replies_.clear();pending_=0;}
};
} // namespace QrazyWindows
