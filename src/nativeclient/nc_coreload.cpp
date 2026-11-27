#include "nc_coreload.h"
#include "nc_http.h"
#include "nc_json.h"
#include "nc_sha256.h"
#include "nc_vk.h"
#include "nc_platform.h"
#include "nc_coretable.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <thread>

#if defined(NC_WINDOWS)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace nc {
namespace fs = std::filesystem;

static void* g_module = nullptr;
static std::string g_used;
static CoreNet g_net;
const CoreNet& coreNet() { return g_net; }
static decltype(&qc_ses_match) g_match = nullptr;
static decltype(&qc_ses_hud) g_hud = nullptr;
static long long g_build = 0;
static constexpr size_t kMaxCoreBytes = 64 * 1024 * 1024;
#if defined(NC_WINDOWS)
static constexpr const char* kCoreExtension = ".dll";
#else
static constexpr const char* kCoreExtension = ".so";
#endif
void* coreModule() { return g_module; }
bool coreHasMatchHud() { return g_match && g_hud; }
int matchOp(QcMatchOp op, double a, double b, double c, std::string_view text) {
  const double in[4] = {a, b, c, 0};
  return coreHasMatchHud() && text.size() <= 255 ? g_match(op, in, text.data(), int(text.size())) : -1;
}
int matchHud(QcHudFrame& out) { return coreHasMatchHud() ? g_hud(&out) : -1; }

// qrazy_core-<digits>-<64 lowercase hex>.<dll|so>
static bool parseName(const std::string& n, CoreCandidate& c) {
  const std::string pre = "qrazy_core-"; if (n.compare(0, pre.size(), pre) != 0) return false;
  size_t i = pre.size(), d = i; while (i < n.size() && n[i] >= '0' && n[i] <= '9') ++i; if (i == d || i - d > 15 || i >= n.size() || n[i] != '-') return false;
  const long long build = atoll(n.substr(d, i - d).c_str()); ++i; if (n.size() < i + 64) return false;
  const std::string sha = n.substr(i, 64); for (char ch : sha) if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
  const std::string ext = n.substr(i + 64); if (build <= 0 || ext != kCoreExtension) return false;
  c.name = n; c.build = build; c.sha = sha; return true;
}

std::vector<CoreCandidate> listCores(const std::vector<std::string>& dirs) {
  std::vector<CoreCandidate> out;
  for (const std::string& d : dirs) {
    if (d.empty()) continue;
    std::error_code ec; for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code e2; if (!fs::is_regular_file(it->symlink_status(e2))) continue; CoreCandidate c; if (!parseName(it->path().filename().string(), c)) continue; c.path = it->path().string(); out.push_back(c);
    }
  }
  std::stable_sort(out.begin(), out.end(), [](const CoreCandidate& a, const CoreCandidate& b) { return a.build > b.build; });
  return out;
}

// load `path`, check its interface version; null and a reason on failure
static void* openCore(const std::string& path, int want, std::string& why, bool bind = true) {
#if defined(NC_WINDOWS)
  HMODULE m = LoadLibraryExA(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS); if (!m) { why = "could not be loaded"; return nullptr; }
  typedef int (*Ver)(); Ver v = (Ver)GetProcAddress(m, "qc_interface_version");
  if (!v) { FreeLibrary(m); why = "is not a game core"; return nullptr; }
  if (v() != want) { FreeLibrary(m); why = "speaks another interface version"; return nullptr; }
  std::string missing; if (!bindCoreTable(m, missing, bind)) { FreeLibrary(m); why = "lacks the function " + missing; return nullptr; }
  return m;
#else
  void* m = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL); if (!m) { why = "could not be loaded"; return nullptr; }
  typedef int (*Ver)(); Ver v = (Ver)dlsym(m, "qc_interface_version");
  if (!v || v() != want) { dlclose(m); why = "speaks another interface version"; return nullptr; }
  std::string missing; if (!bindCoreTable(m, missing, bind)) { dlclose(m); why = "lacks the function " + missing; return nullptr; }
  return m;
#endif
}

bool loadCore(const std::vector<std::string>& dirs, const std::string& devPath, int interfaceVersion, std::string& used, std::string& why) {
  why.clear(); std::string last;
  if (g_module) { used = g_used; return true; }
  const auto accept = [&](void* module, const std::string& name, const std::string& sha, long long build) {
    g_module = module; g_used = used = name; g_build = build;
#if defined(NC_WINDOWS)
    const auto symbol = [module](const char* key) { return GetProcAddress((HMODULE)module, key); };
#else
    const auto symbol = [module](const char* key) { return dlsym(module, key); };
#endif
    g_match = reinterpret_cast<decltype(g_match)>(symbol("qc_ses_match"));
    g_hud = reinterpret_cast<decltype(g_hud)>(symbol("qc_ses_hud"));
    CoreNet net;
    net.in = reinterpret_cast<decltype(net.in)>(symbol("qc_ses_net_in"));
    net.out = reinterpret_cast<decltype(net.out)>(symbol("qc_ses_net_out"));
    net.state = reinterpret_cast<decltype(net.state)>(symbol("qc_ses_net_state"));
    net.fact = reinterpret_cast<decltype(net.fact)>(symbol("qc_ses_net_fact"));
    net.effect = reinterpret_cast<decltype(net.effect)>(symbol("qc_ses_net_effect"));
    if (net) g_net = net;
    logf("game core: %s, build %lld, interface %d, SHA-256 %s, match HUD %s, native multiplayer %s",
         name.c_str(), build, interfaceVersion, sha.c_str(), coreHasMatchHud() ? "available" : "legacy", coreNet() ? "available" : "legacy");
    return true;
  };
  for (const CoreCandidate& c : listCores(dirs)) {
    std::error_code ec;
    if (fs::file_size(c.path, ec) > kMaxCoreBytes || ec || sha256File(c.path) != c.sha) { logf("core %s skipped: SHA-256 mismatch", c.name.c_str()); last = "The game core file is damaged."; continue; }
    const auto path = fs::absolute(c.path).string();
    std::string w; void* m = openCore(path, interfaceVersion, w); if (!m) { logf("core %s skipped: %s", c.name.c_str(), w.c_str()); last = "The game core " + w + "."; continue; }
    return accept(m, path, c.sha, c.build);
  }
  std::error_code ec;
  if (!devPath.empty() && fs::is_regular_file(devPath, ec)) {
    const auto path = fs::absolute(devPath).string();
    std::string reason;
    if (void* module = openCore(path, interfaceVersion, reason))
      return accept(module, path, sha256File(path), 0);
    last = "The game core " + reason + ".";
  }
  why = (last.empty() ? "No compatible game core was found." : last) + std::string(" Reinstall Qrazy or check your connection and start again.");
  return false;
}

static std::thread g_updater;

static void runUpdate(std::string cacheDir, bool staging, int iface, long long currentBuild) {
  try {
    const std::string directory = staging ? "/native/staging/" : "/native/";
    const std::string manifest = directory + "core-manifest-" + std::to_string(iface) + ".json";
    std::vector<uint8_t> body;
    std::string error;
    if (!httpGet(manifest, staging, body, 1 << 20, error)) {
      logf("core update: manifest not read (%s)", error.c_str());
      return;
    }
    J root;
    const std::string json(body.begin(), body.end());
    JP parser{json.data(), json.data() + json.size()};
    root = parser.value();
    parser.ws();
    if (!parser.ok || parser.p != parser.e || root.t != J::Obj || root.num("interfaceVersion", -1) != iface) {
      logf("core update: invalid manifest or interface");
      return;
    }

    const double number = root.num("build", -1);
    if (!(number >= 1 && number <= 999999999999999.0) || std::floor(number) != number) {
      logf("core update: invalid build");
      return;
    }
    const auto build = static_cast<long long>(number);
    if (build <= currentBuild) return;

#if defined(NC_WINDOWS)
    constexpr const char* platform = "win32-x64";
#else
    constexpr const char* platform = "linux-x64";
#endif
    const J* files = root.get("files");
    const J* entry = files && files->t == J::Obj ? files->get(platform) : nullptr;
    if (!entry || entry->t != J::Obj) {
      logf("core update: no core for %s", platform);
      return;
    }
    const std::string siteFile = entry->str("file");
    const std::string sha = entry->str("sha256");
    const double size = entry->num("size", -1);
    const std::string file = "qrazy_core-" + std::to_string(build) + "-" + sha + kCoreExtension;
    CoreCandidate candidate;
    if (siteFile.empty() || siteFile.size() > 128 || siteFile.front() == '.' ||
        siteFile.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != std::string::npos ||
        fs::path(siteFile).extension() != kCoreExtension ||
        !(size >= 1 && size <= kMaxCoreBytes) || std::floor(size) != size || !parseName(file, candidate)) {
      logf("core update: invalid platform entry");
      return;
    }
    if (!httpGet(directory + siteFile, staging, body, static_cast<size_t>(size), error)) {
      logf("core update: download failed (%s)", error.c_str());
      return;
    }
    if (body.size() != static_cast<size_t>(size) || sha256Hex(body.data(), body.size()) != sha) {
      logf("core update: download rejected (size or hash)");
      return;
    }

    const auto cache = fs::absolute(cacheDir);
    fs::create_directories(cache);
    if (fs::is_symlink(fs::symlink_status(cache))) return;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto incoming = cache / (".incoming-" + std::to_string(stamp));
    if (!fs::create_directory(incoming)) return;
    const auto part = incoming / file;
    struct Cleanup {
      fs::path part, directory;
      ~Cleanup() {
        std::error_code ec;
        fs::remove(part, ec);
        fs::remove(directory, ec);
      }
    } cleanup{part, incoming};

    FILE* output = fopen(part.string().c_str(), "wb");
    if (!output) return;
    const bool written = fwrite(body.data(), 1, body.size(), output) == body.size();
    if (fclose(output) != 0 || !written) return;

    // Validate without changing the Linux dispatch table used by the running core.
    void* module = openCore(part.string(), iface, error, false);
    if (!module) {
      logf("core update: download rejected (%s)", error.c_str());
      return;
    }
#if defined(NC_WINDOWS)
    FreeLibrary(static_cast<HMODULE>(module));
#else
    dlclose(module);
#endif
    const auto destination = cache / file;
    std::error_code ec;
    fs::rename(part, destination, ec);
    if (ec) {
      logf("core update: could not store %s (%s)", file.c_str(), ec.message().c_str());
      return;
    }
    logf("core update: %s stored for the next start", file.c_str());
  } catch (const std::exception& error) {
    logf("core update: failed (%s)", error.what());
  }
}

void startCoreUpdate(const std::string& cacheDir, bool staging, int interfaceVersion) {
  if (g_updater.joinable() || cacheDir.empty() || interfaceVersion != 11) return;
  try {
    g_updater = std::thread(runUpdate, cacheDir, staging, interfaceVersion, g_build);
  } catch (const std::exception& error) {
    logf("core update: could not start (%s)", error.what());
  }
}
void waitCoreUpdate() { if (g_updater.joinable()) g_updater.join(); }

}  // namespace nc
