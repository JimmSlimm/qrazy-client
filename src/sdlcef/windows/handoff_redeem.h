#pragma once
// Redemption runs on the CEF UI thread in the SAME request context that will
// load the game. A separate helper profile cannot preserve session-only login.
#include <windows.h>
#include <chrono>
#include <cmath>
#include <string>
#include <array>
#include <atomic>
#include <bcrypt.h>
#include "include/cef_urlrequest.h"
#include "include/cef_cookie.h"
#include "include/cef_parser.h"
#include "include/cef_task.h"
#pragma comment(lib,"bcrypt.lib")
namespace Handoff {
constexpr char Origin[]="https://qrazy-game.onrender.com/";
using CookieIdentity=std::array<unsigned char,32>;
inline bool HashCookie(const std::string& token,CookieIdentity& digest) {
  if(token.size()!=64)return false;for(char c:token)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
  return BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,reinterpret_cast<PUCHAR>(const_cast<char*>(token.data())),static_cast<ULONG>(token.size()),digest.data(),static_cast<ULONG>(digest.size()))>=0;
}
inline bool CookieHeader(std::string& header,bool remember,CookieIdentity& identity) {
  const std::string prefix="qrazy_session=",attributes="; Path=/; HttpOnly; SameSite=Strict; Secure";
  bool ok=false;
  if(header.starts_with(prefix)&&header.size()>=prefix.size()+64+attributes.size()&&header.substr(prefix.size()+64,attributes.size())==attributes) {
    auto trailing=header.substr(prefix.size()+64+attributes.size());
    ok=remember?trailing.starts_with("; Max-Age=")&&trailing.size()>10&&trailing.size()<=17:trailing.empty();
    if(remember&&ok)for(size_t i=10;i<trailing.size();++i)if(trailing[i]<'0'||trailing[i]>'9')ok=false;
    std::string token=header.substr(prefix.size(),64);ok=ok&&HashCookie(token,identity);SecureZeroMemory(token.data(),token.size());
  }
  if(!header.empty())SecureZeroMemory(header.data(),header.size());header.clear();return ok;
}
class Redeem final : public CefURLRequestClient {
 public:
  ~Redeem() override{if(!body_.empty())SecureZeroMemory(body_.data(),body_.size());}
  bool done=false,valid=false,remember=false;
  double expires_at=0;
  CookieIdentity cookie_identity{};
  bool Start(const char* code,size_t length,CefRefPtr<CefRequestContext> context) {
    if(started_||!context||length!=43)return false;
    for(size_t i=0;i<length;++i)if(!((code[i]>='A'&&code[i]<='Z')||(code[i]>='a'&&code[i]<='z')||(code[i]>='0'&&code[i]<='9')||code[i]=='_'||code[i]=='-'))return false;
    started_=true;deadline_=std::chrono::steady_clock::now()+std::chrono::seconds(8);
    auto request=CefRequest::Create();request->SetURL(std::string(Origin)+"auth/handoff/redeem");request->SetMethod("POST");
    request->SetFlags(UR_FLAG_DISABLE_CACHE|UR_FLAG_ALLOW_STORED_CREDENTIALS|UR_FLAG_STOP_ON_REDIRECT|UR_FLAG_NO_RETRY_ON_5XX);
    request->SetHeaderByName("Content-Type","application/json",true);
    request->SetHeaderByName("X-Qrazy-Auth","1",true);request->SetHeaderByName("Origin","https://qrazy-game.onrender.com",true);
    std::string body="{\"code\":\"";body.append(code,length);body+="\"}";
    auto data=CefPostData::Create();auto element=CefPostDataElement::Create();element->SetToBytes(body.size(),body.data());data->AddElement(element);request->SetPostData(data);
    SecureZeroMemory(body.data(),body.size());request_=CefURLRequest::Create(request,this,context);return request_.get()!=nullptr;
  }
  void Poll() {
    if(!done&&started_&&std::chrono::steady_clock::now()>=deadline_){done=true;valid=false;if(request_)request_->Cancel();request_=nullptr;if(!body_.empty())SecureZeroMemory(body_.data(),body_.size());body_.clear();}
  }
  void Cancel() {
    done=true;valid=false;if(request_)request_->Cancel();request_=nullptr;
    if(!body_.empty())SecureZeroMemory(body_.data(),body_.size());body_.clear();
  }
  void OnRequestComplete(CefRefPtr<CefURLRequest> request) override {
    if(done)return;
    done=true;auto response=request->GetResponse();
    if(std::chrono::steady_clock::now()<deadline_&&request->GetRequestStatus()==UR_SUCCESS&&response&&response->GetStatus()==200&&
      response->GetMimeType()=="application/json"&&!request->ResponseWasCached()&&request->GetRequest()->GetURL()==std::string(Origin)+"auth/handoff/redeem") {
      auto value=CefParseJSON(body_,JSON_PARSER_RFC);auto d=value&&value->GetType()==VTYPE_DICTIONARY?value->GetDictionary():nullptr;
      if(d&&d->GetSize()==2&&d->GetType("remember")==VTYPE_BOOL&&(d->GetType("expiresAt")==VTYPE_DOUBLE||d->GetType("expiresAt")==VTYPE_INT)) {
        expires_at=d->GetType("expiresAt")==VTYPE_DOUBLE?d->GetDouble("expiresAt"):d->GetInt("expiresAt");
        auto now=std::chrono::duration<double,std::milli>(std::chrono::system_clock::now().time_since_epoch()).count();
        valid=std::isfinite(expires_at)&&std::floor(expires_at)==expires_at&&expires_at>now&&expires_at<=9007199254740991.;remember=d->GetBool("remember");
        auto header=response->GetHeaderByName("Set-Cookie").ToString();valid=valid&&CookieHeader(header,remember,cookie_identity);
      }
    }
    if(!body_.empty())SecureZeroMemory(body_.data(),body_.size());body_.clear();request_=nullptr;
  }
  void OnUploadProgress(CefRefPtr<CefURLRequest>,int64_t,int64_t) override{}
  void OnDownloadProgress(CefRefPtr<CefURLRequest> request,int64_t current,int64_t total) override{if(current>512||total>512)request->Cancel();}
  void OnDownloadData(CefRefPtr<CefURLRequest> request,const void* data,size_t length) override {
    if(done||body_.size()+length>512){request->Cancel();return;}body_.append(static_cast<const char*>(data),length);
  }
  bool GetAuthCredentials(bool,const CefString&,int,const CefString&,const CefString&,CefRefPtr<CefAuthCallback>) override{return false;}
 private:
  bool started_=false;std::string body_;std::chrono::steady_clock::time_point deadline_;
  CefRefPtr<CefURLRequest> request_;
  IMPLEMENT_REFCOUNTING(Redeem);
};
class CookieCheck final : public CefCookieVisitor {
 public:
  // Cookie visitors run on CEF's IO thread. Startup polls from the UI thread;
  // publishing completion must also publish the validation result.
  std::atomic<bool> done{false},valid{false};
  explicit CookieCheck(bool remembered,double expiry,const CookieIdentity& identity):remember_(remembered),expiry_(expiry),identity_(identity){}
  bool Visit(const CefCookie& cookie,int count,int total,bool&) override {
    if(CefString(&cookie.name)=="qrazy_session") {
      ++matches_;auto path=CefString(&cookie.path).ToString(),domain=CefString(&cookie.domain).ToString();
      bool ok=cookie.secure&&cookie.httponly&&cookie.same_site==CEF_COOKIE_SAME_SITE_STRICT_MODE&&path=="/"&&
        domain=="qrazy-game.onrender.com"&&static_cast<bool>(cookie.has_expires)==remember_;
      CookieIdentity actual{};auto token=CefString(&cookie.value).ToString();ok=ok&&HashCookie(token,actual)&&actual==identity_;SecureZeroMemory(token.data(),token.size());
      if(remember_) {
        // Max-Age is rounded down by the server. Do not extend it to the JSON
        // expiry or convert a session cookie to a persistent cookie.
        const double cookie_ms=(static_cast<double>(cookie.expires.val)/1000.)-11644473600000.;
        ok=ok&&cookie_ms<=expiry_+1000&&cookie_ms>=expiry_-10000;
      }
      valid=ok&&matches_==1;
    }
    if(count+1==total){valid=valid&&matches_==1;done=true;}return true;
  }
 private:
  bool remember_;double expiry_;CookieIdentity identity_;int matches_=0;
  IMPLEMENT_REFCOUNTING(CookieCheck);
};
class CookieFlush final : public CefCompletionCallback {
 public:std::atomic<bool> done{false};void OnComplete() override{done=true;}
 private:IMPLEMENT_REFCOUNTING(CookieFlush);
};
// Startup must keep the game URL unloaded until Passed(). The browser must be
// created with Context(), preserving the exact context used for redemption and
// its session-only cookie. Only a Passed result may be acknowledged to the
// authenticated native broker; failure never authorizes recovery cleanup.
class PreGameGate {
 public:
  enum class Phase {Idle,Request,Cookie,Flush,Passed,Failed};
  ~PreGameGate(){if(redeem_&&!Passed())redeem_->Cancel();}
  bool Start(std::array<char,43>& code,CefRefPtr<CefRequestContext> context) {
    if(phase_!=Phase::Idle||!CefCurrentlyOn(TID_UI)||!context) {
      SecureZeroMemory(code.data(),code.size());return false;
    }
    context_=context;deadline_=std::chrono::steady_clock::now()+std::chrono::seconds(12);
    redeem_=new Redeem;bool started=redeem_->Start(code.data(),code.size(),context_);
    SecureZeroMemory(code.data(),code.size());phase_=started?Phase::Request:Phase::Failed;return started;
  }
  void Poll() {
    if(!CefCurrentlyOn(TID_UI)||phase_==Phase::Idle||phase_==Phase::Passed||phase_==Phase::Failed)return;
    if(std::chrono::steady_clock::now()>=deadline_){Fail();return;}
    if(phase_==Phase::Request) {
      redeem_->Poll();if(!redeem_->done)return;if(!redeem_->valid){Fail();return;}
      manager_=context_->GetCookieManager(nullptr);if(!manager_){Fail();return;}
      check_=new CookieCheck(redeem_->remember,redeem_->expires_at,redeem_->cookie_identity);
      if(!manager_->VisitUrlCookies(Origin,true,check_)){Fail();return;}phase_=Phase::Cookie;
    }else if(phase_==Phase::Cookie) {
      if(!check_->done.load())return;if(!check_->valid.load()){Fail();return;}
      flush_=new CookieFlush;if(!manager_->FlushStore(flush_)){Fail();return;}phase_=Phase::Flush;
    }else if(phase_==Phase::Flush&&flush_->done.load()) {
      phase_=Phase::Passed;redeem_=nullptr;check_=nullptr;flush_=nullptr;manager_=nullptr;
    }
  }
  bool Passed() const{return phase_==Phase::Passed;}
  bool Failed() const{return phase_==Phase::Failed;}
  Phase State() const{return phase_;}
  CefRefPtr<CefRequestContext> Context() const{return context_;}
 private:
  void Fail(){phase_=Phase::Failed;if(redeem_)redeem_->Cancel();redeem_=nullptr;check_=nullptr;flush_=nullptr;manager_=nullptr;}
  Phase phase_=Phase::Idle;std::chrono::steady_clock::time_point deadline_{};
  CefRefPtr<CefRequestContext> context_;CefRefPtr<CefCookieManager> manager_;
  CefRefPtr<Redeem> redeem_;CefRefPtr<CookieCheck> check_;CefRefPtr<CookieFlush> flush_;
};
}
