// SHA-256 (FIPS 180-4), small and portable: used to check a downloaded game core against the hash its file name and the site's manifest carry.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

namespace nc {

struct Sha256 {
  uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  uint8_t buf[64]; size_t n = 0; uint64_t total = 0;
  static uint32_t rr(uint32_t x, int k) { return (x >> k) | (x << (32 - k)); }
  void block(const uint8_t* p) {
    static const uint32_t K[64] = {0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t w[64]; for (int i = 0; i < 16; ++i) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; ++i) { const uint32_t s0 = rr(w[i - 15], 7) ^ rr(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = rr(w[i - 2], 17) ^ rr(w[i - 2], 19) ^ (w[i - 2] >> 10); w[i] = w[i - 16] + s0 + w[i - 7] + s1; }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) { const uint32_t t1 = hh + (rr(e, 6) ^ rr(e, 11) ^ rr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i], t2 = (rr(a, 2) ^ rr(a, 13) ^ rr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c)); hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2; }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  void update(const uint8_t* p, size_t len) { total += len; while (len) { const size_t k = len < 64 - n ? len : 64 - n; for (size_t i = 0; i < k; ++i) buf[n + i] = p[i]; n += k; p += k; len -= k; if (n == 64) { block(buf); n = 0; } } }
  std::string hex() {
    const uint64_t bits = total * 8; uint8_t pad = 0x80; update(&pad, 1); pad = 0; while (n != 56) update(&pad, 1);
    uint8_t L[8]; for (int i = 0; i < 8; ++i) L[i] = (uint8_t)(bits >> (56 - 8 * i)); update(L, 8);
    char out[65]; for (int i = 0; i < 8; ++i) snprintf(out + i * 8, 9, "%08x", h[i]); return std::string(out, 64);
  }
};

inline std::string sha256Hex(const void* data, size_t len) { Sha256 s; s.update((const uint8_t*)data, len); return s.hex(); }
// of a file; empty when it cannot be read
inline std::string sha256File(const std::string& path) {
  FILE* f = fopen(path.c_str(), "rb"); if (!f) return {};
  Sha256 s; uint8_t b[65536]; size_t k; while ((k = fread(b, 1, sizeof b, f)) > 0) s.update(b, k); fclose(f); return s.hex();
}

}  // namespace nc
