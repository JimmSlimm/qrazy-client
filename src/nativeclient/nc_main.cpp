// qrazy-native: the standalone executable (development). The application loop is in nc_app.cpp.
#include "nc_app.h"
#include "nc_platform.h"
#include <cstdio>
#include <cstring>
#include <string>
#ifdef NC_WINDOWS
#include <windows.h>
#else
#include "nc_coreload.h"
#include "qrazy_core_api.h"
#include <unistd.h>
#endif

static std::string programDir() {
#ifdef NC_WINDOWS
  char path[MAX_PATH * 2]; const DWORD n = GetModuleFileNameA(nullptr, path, sizeof path); std::string p(path, n); for (char& c : p) if (c == (char)92) c = '/';
#else
  char path[4096]; const ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1); std::string p = n > 0 ? std::string(path, (size_t)n) : std::string("./x");
#endif
  return p.substr(0, p.rfind('/'));
}

int main(int argc, char** argv) {
  const std::string dir = programDir();
#ifndef NC_WINDOWS
  // Linux: the game core is a library opened here, before anything calls it: <program dir>/core/qrazy_core-<build>-<sha256>.so, else a plain <program dir>/libqrazy_core.so (dev builds).
  { std::string used, why; if (!nc::loadCore({dir + "/core"}, dir + "/libqrazy_core.so", QC_INTERFACE_VERSION, used, why)) { fprintf(stderr, "Qrazy cannot start: %s\n", why.c_str()); return 1; } }
  if (argc == 2 && !strcmp(argv[1], "--core-check")) return 0;
#endif
  // the client's own fonts and HUD sheets, when installed next to the program (otherwise the dev paths of nc_app.cpp apply; --fonts / --hudassets override)
  { const std::string assets = dir + "/native-assets"; FILE* f = fopen((assets + "/hud/hud-quads.table.bin").c_str(), "rb"); if (f) { fclose(f); appSetAssetsDir(assets); } }
  // --twice (a test of the host's match after match): the application runs twice in this process, as the embedded client does when a player starts a second match
  bool twice = false; for (int i = 1; i < argc; ++i) if (!strcmp(argv[i], "--twice")) twice = true;
  if (twice) { const int first = appRun(argc, argv, nullptr, nullptr, nullptr); if (first != 0) return first; }
  return appRun(argc, argv, nullptr, nullptr, nullptr);
}
