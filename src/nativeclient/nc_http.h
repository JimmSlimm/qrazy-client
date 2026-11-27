// One blocking GET from the hosted HTTPS origin, or localhost HTTP in staging mode.
// Redirects and bodies larger than maxBytes are rejected.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nc {
bool httpGet(const std::string& path, bool staging, std::vector<uint8_t>& body, size_t maxBytes, std::string& error, int timeoutMs = 20000);
}
