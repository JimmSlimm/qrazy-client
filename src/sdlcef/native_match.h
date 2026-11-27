#pragma once

#include "include/cef_parser.h"
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace QrazyNativeMatch {

inline constexpr std::size_t AssetHeader = 4096;

struct NativeMatch{bool start=false;const char* code="unsupported";bool stop=false;bool resume=false;bool simop=false;bool soundfact=false;std::string cmdline;std::string error;};
inline bool NativeText(const std::string& s,size_t max){if(s.empty()||s.size()>max)return false;for(unsigned char c:s)if(c<32||c==127||c=='"')return false;return true;}
inline uint64_t NativePayloadSize(const std::filesystem::path& path){   /* QAS1 header: "QAS1", u32 json length, json with "size" */
  std::ifstream in(path,std::ios::binary);unsigned char header[AssetHeader];if(!in.read(reinterpret_cast<char*>(header),sizeof(header))||memcmp(header,"QAS1",4))return 0;
  uint32_t len=0;memcpy(&len,header+4,4);if(len==0||len>AssetHeader-8)return 0;
  auto v=CefParseJSON(std::string(reinterpret_cast<char*>(header+8),len),JSON_PARSER_RFC);if(!v||v->GetType()!=VTYPE_DICTIONARY)return 0;auto d=v->GetDictionary();return d->GetType("size")==VTYPE_INT?static_cast<uint64_t>(d->GetInt("size")):0;
}

template <class AssetPath>
bool ParseNativeMatch(const std::string& json,AssetPath assetPath,const std::filesystem::path& fixDir,NativeMatch& out){
  auto value=CefParseJSON(json,JSON_PARSER_RFC);if(!value||value->GetType()!=VTYPE_DICTIONARY){out.error="not a JSON object";return false;}
  auto d=value->GetDictionary();const std::string op=d->GetString("op");
  if(op=="stop"){out.stop=true;return true;}
  if(op=="soundfact"){out.simop=true;out.soundfact=true;return true;}
  if(op=="respawn"||op=="mode"||op=="giveitem"||op=="stat"||op=="removepowerups"||op=="pose"||op=="openText"||op=="closeText"||op=="runack"||op=="ghost"||op=="watchsound"||op=="follow"||op=="capture"||op=="custommodel"||op=="custompickup"){out.simop=true;return true;}   /* ops for the running sim, forwarded as they are: /kill or the respawn button, a mode / physics change */
  if(op=="cmd"){   /* a practice command for the core: 16 numbers and a text */
    auto in=d->GetType("in")==VTYPE_LIST?d->GetList("in"):nullptr;bool ok=d->HasKey("cmd")&&in&&in->GetSize()==16&&d->GetType("text")==VTYPE_STRING;
    for(size_t i=0;ok&&i<16;++i)ok=in->GetType(i)==VTYPE_INT||in->GetType(i)==VTYPE_DOUBLE;
    if(!ok){out.error="bad cmd";return false;}out.simop=true;return true;}
  if(op=="resume"){out.resume=true;return true;}   /* back from the menu into the same match */
  if(op!="start"){out.error="unknown op";return false;}out.start=true;
  const bool runway=d->GetType("runway")==VTYPE_BOOL&&d->GetBool("runway");   /* the built-in runway has no BSP: the core builds it */
  const std::string bsp=d->GetString("bsp");
  if(!runway&&(!NativeText(bsp,128)||bsp.rfind("maps/",0)!=0||bsp.size()<10||bsp.substr(bsp.size()-4)!=".bsp"||bsp.find("..")!=std::string::npos)){out.error="bad bsp";out.code="bsp";return false;}
  std::string cmd=runway?std::string("--sim --runway"):"--sim --bsp \""+bsp+"\"";
  if(d->GetType("hudFeed")==VTYPE_INT&&d->GetInt("hudFeed")==1)cmd+=" --hud-feed";
  if((d->GetType("audioFeed")==VTYPE_INT&&d->GetInt("audioFeed")==1)||(d->GetType("audioFeed")==VTYPE_DOUBLE&&d->GetDouble("audioFeed")==1))cmd+=" --audio-feed";
  auto assets=d->GetType("assets")==VTYPE_LIST?d->GetList("assets"):nullptr;int mounted=0;
  if(assets)for(size_t i=0;i<assets->GetSize()&&i<64;++i){
    if(assets->GetType(i)!=VTYPE_DICTIONARY)continue;
    auto a=assets->GetDictionary(i);const std::string kind=a->GetString("kind"),key=a->GetString("key");
    if(kind=="model"){   /* an item model: a single .glb at a virtual path; a model that is not stored is simply absent */
      const std::string vpath=a->GetString("path");if(!NativeText(vpath,128)||vpath.rfind("assets/models/",0)!=0||vpath.size()<20||vpath.substr(vpath.size()-4)!=".glb"||vpath.find("..")!=std::string::npos||vpath.find('=')!=std::string::npos||vpath.find('+')!=std::string::npos)continue;
      auto mp=assetPath(kind,key);std::error_code mec;if(mp.empty()||!std::filesystem::is_regular_file(mp,mec))continue;
      const uint64_t size=NativePayloadSize(mp);const std::string p=mp.string();if(!size||!NativeText(p,1024)||p.find('+')!=std::string::npos||p.find('@')!=std::string::npos)continue;
      const std::string payload="="+p+"@"+std::to_string(AssetHeader)+"+"+std::to_string(size)+std::string(1,34);
      cmd+=" --file "+std::string(1,34)+vpath+payload;
      // The page already resolved the model manifest. Preserve its logical name from the cache key.
      const std::string prefix="https://qrazy-model-cache.invalid/";
      const auto version=key.find("?v=",prefix.size());
      if(key.rfind(prefix,0)==0&&version!=std::string::npos&&key.substr(version+3)==vpath.substr(14)){
        const std::string name=key.substr(prefix.size(),version-prefix.size());
        if(!name.empty()&&name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")==std::string::npos)
          cmd+=" --file "+std::string(1,34)+"assets/models/by-name/"+name+".glb"+payload;
      }
      continue;
    }
    if(kind!="map"&&kind!="shader"&&kind!="texture"&&kind!="sound")continue;
    auto path=assetPath(kind,key);std::error_code ec;if(path.empty()||!std::filesystem::is_regular_file(path,ec)){out.error="asset not stored: "+kind;out.code="asset";return false;}   /* a QAS1 file: the archive starts after the header */
    const std::string p=path.string();if(!NativeText(p,1024)){out.error="asset path unusable";out.code="asset";return false;}
    const std::string oaShaders="https://qrazy-oa-cache.invalid/oa-shaders.pk3";
    const bool openArena=kind=="shader"&&(key==oaShaders||key.rfind(oaShaders+"?",0)==0);
    const bool pooled=kind=="shader"&&a->GetType("shaderTier")==VTYPE_INT&&a->GetInt("shaderTier")==0;
    cmd+=std::string(openArena?" --oa-pk3 ":pooled?" --pooled-pk3 ":" --pk3 ")+"\""+p+"@"+std::to_string(AssetHeader)+"\"";++mounted;
  }
  if(cmd.find(" --audio-feed")!=std::string::npos){
    for(const char* pack:{"funny-sounds.pk3","funny-special.pk3","funny-sounds-2.pk3","funny-sounds-3.pk3"}){
      auto path=assetPath("sound",std::string("https://qrazy-oa-cache.invalid/")+pack);
      const std::string p=path.string();
      if(NativeText(p,1024)&&cmd.find(p+"@")==std::string::npos)cmd+=" --funny-pk3 \""+p+"@"+std::to_string(AssetHeader)+"\"";
    }
  }
  if(!mounted&&!runway){out.error="no assets";out.code="asset";return false;}
  if(d->GetInt("netFeed")==1){
    const std::string profile=d->GetString("profileId");
    if(!profile.empty()&&!NativeText(profile,23)){out.error="bad profile ID";return false;}
    cmd+=" --net-feed";
    if(!profile.empty())cmd+=" --profile-id \""+profile+"\"";
  }

  std::string name=d->GetString("name");if(!name.empty()){std::string clean;for(unsigned char c:name)if(clean.size()<32&&(isalnum(c)||c=='_'||c=='-'||c==' '))clean+=static_cast<char>(c);if(!clean.empty())cmd+=" --name \""+clean+"\"";}
  const std::string fixes=d->GetString("fixes");
  if(!fixes.empty()&&fixes.size()<=65536){std::error_code ec;auto dir=fixDir;std::filesystem::create_directories(dir,ec);auto file=dir/"map-fixes.json";std::ofstream f(file,std::ios::binary|std::ios::trunc);f.write(fixes.data(),static_cast<std::streamsize>(fixes.size()));f.close();const std::string p=file.string();if(NativeText(p,1024))cmd+=" --fixes \""+p+"\"";}
  if(d->GetType("settings")==VTYPE_DICTIONARY){auto st=d->GetDictionary("settings");   /* the player's settings: mouse, view, volume, voice, key binds (KeyboardEvent.code names) */
    auto number=[&](const char* key,double lo,double hi,double& v){auto t=st->GetType(key);if(t==VTYPE_DOUBLE)v=st->GetDouble(key);else if(t==VTYPE_INT)v=st->GetInt(key);else return false;return v>=lo&&v<=hi;};
    double v=0;
    if(number("sensitivity",0.01,100,v))cmd+=" --sens "+std::to_string(v);
    if(number("fov",30,150,v))cmd+=" --fov "+std::to_string(v);
    if(number("volume",0,1,v))cmd+=" --volume "+std::to_string(v);
    const std::string voice=st->GetString("voice");if(!voice.empty()&&voice.size()<=24&&voice.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")==std::string::npos)cmd+=" --voice "+voice;
    if(st->GetType("binds")==VTYPE_DICTIONARY){auto binds=st->GetDictionary("binds");CefDictionaryValue::KeyList keys;binds->GetKeys(keys);
      for(const auto& k:keys){const std::string action=k.ToString();if(binds->GetType(k)!=VTYPE_LIST||action.find_first_not_of("abcdefghijklmnopqrstuvwxyz")!=std::string::npos)continue;auto list=binds->GetList(k);std::string codes;
        for(size_t i=0;i<list->GetSize()&&i<4;++i){if(list->GetType(i)!=VTYPE_STRING)continue;const std::string code=list->GetString(i).ToString();if(code.empty()||code.size()>16||code.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789")!=std::string::npos)continue;if(!codes.empty())codes+=",";codes+=code;}
        if(!codes.empty())cmd+=" --bind "+action+"="+codes;}}
  }
  {const bool vql=d->GetType("vql")!=VTYPE_BOOL||d->GetBool("vql");if(!vql)cmd+=" --pql";if(d->GetString("weaponMode")=="s")cmd+=" --wmode s";if(d->GetType("hook")==VTYPE_BOOL&&d->GetBool("hook"))cmd+=" --hook";}   /* physics, weapon set (!s / !w) and hook-strafe of the match */
  out.cmdline=cmd;return true;
}

}  // namespace QrazyNativeMatch
