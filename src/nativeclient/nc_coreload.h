// Choosing, loading and updating the game core library (qrazy_core.dll / libqrazy_core.so).
// Candidates are the cache folder (copies the site served) and the bundled folder (installed with the client). A file's name carries its own metadata: qrazy_core-<build>-<sha256>.<ext>.
// Downloaded candidates are staged only after verification against the HTTPS manifest.
#pragma once
#include "qrazy_core_api.h"
#include <string>
#include <string_view>
#include <vector>

namespace nc {
struct CoreNet {
  decltype(&qc_ses_net_in) in = nullptr;
  decltype(&qc_ses_net_out) out = nullptr;
  decltype(&qc_ses_net_state) state = nullptr;
  decltype(&qc_ses_net_fact) fact = nullptr;
  decltype(&qc_ses_net_effect) effect = nullptr;
  explicit operator bool() const { return in && out && state && fact && effect; }
};
const CoreNet& coreNet();

struct CoreCandidate { std::string path, name, sha; long long build = 0; };

// Every well-named file in `dirs`, best (highest build) first. Does not check hashes.
std::vector<CoreCandidate> listCores(const std::vector<std::string>& dirs);

// Tries installed and staged builds in descending order, validating the digest and interface.
// On success `used` names what is loaded. On failure `why` says what to tell the player.
bool loadCore(const std::vector<std::string>& dirs, const std::string& devPath, int interfaceVersion, std::string& used, std::string& why);
void* coreModule();   // the loaded library (windows: HMODULE), for the delay-load hook
bool coreHasMatchHud();
int matchOp(QcMatchOp op, double a = 0, double b = 0, double c = 0, std::string_view text = {});
int matchHud(QcHudFrame& out);

// Background retrieval of a newer core from the selected channel.
void startCoreUpdate(const std::string& cacheDir, bool staging, int interfaceVersion);
void waitCoreUpdate();   // joins the background thread (tests, shutdown)

}  // namespace nc
