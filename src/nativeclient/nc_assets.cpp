#include "nc_assets.h"
#include "miniz.h"
#if defined(NC_WINDOWS)
#include <wincodec.h>
#else
#define STB_IMAGE_IMPLEMENTATION   // Linux: PNG / JPEG through stb_image (public domain); TGA stays our own reader
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"
#endif
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unordered_map>
#include <algorithm>
#include <cstring>

namespace nc {

std::string lowerStr(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

struct FileSource::Zip { mz_zip_archive z; std::unordered_map<std::string, mz_uint> index; std::vector<std::pair<std::string, mz_uint>> entries; int shaderTier; };

FileSource::FileSource() {}
FileSource::~FileSource() { for (Zip* z : zips_) { mz_zip_reader_end(&z->z); delete z; } }
FileSource::FileSource(FileSource&& o) noexcept : root(std::move(o.root)), zips_(std::move(o.zips_)), vfiles_(std::move(o.vfiles_)) { o.zips_.clear(); }
FileSource& FileSource::operator=(FileSource&& o) noexcept { if (this != &o) { for (Zip* z : zips_) { mz_zip_reader_end(&z->z); delete z; } root = std::move(o.root); zips_ = std::move(o.zips_); vfiles_ = std::move(o.vfiles_); o.zips_.clear(); } return *this; }

static std::string normName(std::string s) { for (auto& c : s) { if (c == (char)92) c = '/'; c = (char)tolower((unsigned char)c); } return s; }

bool FileSource::mountZip(const std::string& path, std::string* error, uint64_t offset, int shaderTier) {
  Zip* z = new Zip(); memset(&z->z, 0, sizeof z->z); z->shaderTier = shaderTier;
  // miniz takes archive_size 0 to mean the whole file, which is wrong behind an offset (it would look for the end of the archive `offset` bytes past the real end): say how long the archive is
  std::error_code sizeEc; const uint64_t fileSize = std::filesystem::file_size(path, sizeEc); const uint64_t archiveSize = !sizeEc && offset && fileSize > offset ? fileSize - offset : 0;
  if (!mz_zip_reader_init_file_v2(&z->z, path.c_str(), 0, offset, archiveSize)) { if (error) *error = std::string("cannot open ") + path + ": " + mz_zip_get_error_string(mz_zip_get_last_error(&z->z)); delete z; return false; }
  const mz_uint n = mz_zip_reader_get_num_files(&z->z);
  for (mz_uint i = 0; i < n; ++i) {
    if (mz_zip_reader_is_file_a_directory(&z->z, i)) continue;
    char name[512]; if (!mz_zip_reader_get_filename(&z->z, i, name, sizeof name)) continue;
    z->entries.emplace_back(name, i); z->index[normName(name)] = i;   // a later duplicate inside one archive wins, as in the game
  }
  zips_.push_back(z); return true;
}
int FileSource::archiveCount() const { return (int)zips_.size(); }

bool FileSource::readFrom(int a, const std::string& rel, std::vector<uint8_t>& out) const {
  if (a < 0 || a >= (int)zips_.size()) return false; Zip* z = zips_[a]; auto it = z->index.find(normName(rel)); if (it == z->index.end()) return false;
  return readEntry(a, it->second, out);
}
bool FileSource::readEntry(int a, unsigned index, std::vector<uint8_t>& out) const {
  Zip* z = zips_[a];
  mz_zip_archive_file_stat st; if (!mz_zip_reader_file_stat(&z->z, index, &st)) return false;
  out.resize((size_t)st.m_uncomp_size); if (st.m_uncomp_size == 0) return true;
  if (!mz_zip_reader_extract_to_mem(&z->z, index, out.data(), out.size(), 0)) { out.clear(); return false; }
  return true;
}
bool FileSource::mountFile(const std::string& virtualPath, const std::string& path, uint64_t offset, uint64_t size) { vfiles_.push_back({normName(virtualPath), path, offset, size}); return true; }

bool FileSource::read(const std::string& rel, std::vector<uint8_t>& out, int* archive, std::string* actualName) const {
  if (archive) *archive = -1;
  if (actualName) *actualName = rel;
  { const std::string key = normName(rel);
    for (const VFile& v : vfiles_) if (v.name == key) {
      FILE* f = fopen(v.path.c_str(), "rb"); if (!f) return false;
      out.resize((size_t)v.size); bool ok = fseek(f, (long)v.offset, SEEK_SET) == 0 && (v.size == 0 || fread(out.data(), 1, (size_t)v.size, f) == (size_t)v.size); fclose(f); if (!ok) out.clear(); return ok;
    } }
  const std::string key = normName(rel);
  for (int a = 0; a < (int)zips_.size(); ++a) {
    Zip* z = zips_[a];
    const auto entry = z->index.find(key);
    if (entry == z->index.end() || !readEntry(a, entry->second, out)) continue;
    if (archive) *archive = a;
    if (actualName) {
      char name[512];
      if (mz_zip_reader_get_filename(&z->z, entry->second, name, sizeof name)) *actualName = name;
    }
    return true;
  }
  if (root.empty()) return false;
  FILE* f = fopen((root + "/" + rel).c_str(), "rb"); if (!f) return false;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); out.resize((size_t)n);
  const bool ok = n == 0 || fread(out.data(), 1, (size_t)n, f) == (size_t)n; fclose(f); if (!ok) out.clear(); return ok;
}
bool FileSource::exists(const std::string& rel) const {
  const std::string key = normName(rel); for (const VFile& v : vfiles_) if (v.name == key) return true; for (Zip* z : zips_) if (z->index.count(key)) return true;
  if (root.empty()) return false; FILE* f = fopen((root + "/" + rel).c_str(), "rb"); if (!f) return false; fclose(f); return true;
}
std::vector<std::string> FileSource::listArchives() const {
  std::vector<std::string> out;
  for (const Zip* z : zips_) for (const auto& entry : z->entries) out.push_back(entry.first);
  for (const VFile& v : vfiles_) out.push_back(v.name);
  if (!root.empty()) {
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
      if (it->is_regular_file(ec)) out.push_back(it->path().lexically_relative(root).generic_string());
    }
  }
  return out;
}

bool FileSource::readShaderSources(int selectedArchive, std::vector<ShaderSource>& out) const {
  for (int a = 0; a < archiveCount(); ++a) {
    const Zip* z = zips_[a];
    const int tier = a == selectedArchive ? 3 : z->shaderTier;
    for (const auto& entry : z->entries) {
      const std::string name = normName(entry.first);
      if (name.size() < 7 || name.compare(name.size() - 7, 7, ".shader") != 0) continue;
      out.push_back({tier, {}});
      if (!readEntry(a, entry.second, out.back().bytes)) {
        logf("cannot read shader script %s from archive %d", entry.first.c_str(), a);
        return false;
      }
    }
  }
  if (root.empty()) return true;
  const auto scripts = std::filesystem::path(root) / "scripts";
  std::error_code ec;
  if (!std::filesystem::exists(scripts, ec)) return !ec;
  std::vector<std::filesystem::path> loose;
  for (std::filesystem::recursive_directory_iterator it(scripts, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->is_regular_file(ec) && lowerStr(it->path().extension().string()) == ".shader") loose.push_back(it->path());
  }
  if (ec) return false;
  std::sort(loose.begin(), loose.end());
  for (const auto& path : loose) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    out.push_back({2, {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()}});
    if (file.bad()) return false;
  }
  return true;
}

static bool decodeTga(const std::vector<uint8_t>& d, std::vector<uint8_t>& rgba, int& w, int& h) {
  if (d.size() < 18) return false;
  const int idLen = d[0], type = d[2], bpp = d[16], desc = d[17]; w = d[12] | (d[13] << 8); h = d[14] | (d[15] << 8);
  if (!w || !h || (type != 2 && type != 3 && type != 10 && type != 11) || (bpp != 24 && bpp != 32 && bpp != 8)) return false;
  const int px = bpp / 8; size_t at = 18 + idLen + (d[1] ? ((d[5] | (d[6] << 8)) * (d[7] / 8)) : 0);
  std::vector<uint8_t> pix((size_t)w * h * px);
  if (type == 2 || type == 3) { if (at + pix.size() > d.size()) return false; memcpy(pix.data(), &d[at], pix.size()); }
  else {
    size_t o = 0;
    while (o < pix.size() && at < d.size()) {
      int hdr = d[at++]; int n = (hdr & 127) + 1;
      if (hdr & 128) { if (at + px > d.size()) return false; for (int i = 0; i < n && o < pix.size(); ++i, o += px) memcpy(&pix[o], &d[at], px); at += px; }
      else { size_t bytes = (size_t)n * px; if (at + bytes > d.size()) return false; memcpy(&pix[o], &d[at], std::min(bytes, pix.size() - o)); o += bytes; at += bytes; }
    }
  }
  rgba.resize((size_t)w * h * 4);
  for (int y = 0; y < h; ++y) {
    const int sy = (desc & 0x20) ? y : h - 1 - y;
    for (int x = 0; x < w; ++x) {
      const uint8_t* s = &pix[((size_t)sy * w + x) * px]; uint8_t* o = &rgba[((size_t)y * w + x) * 4];
      if (px == 1) { o[0] = o[1] = o[2] = s[0]; o[3] = 255; } else { o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o[3] = px == 4 ? s[3] : 255; }
    }
  }
  return true;
}

#if defined(NC_WINDOWS)
static IWICImagingFactory* wicFactory() {
  static IWICImagingFactory* fac = nullptr;
  if (!fac) { CoInitializeEx(nullptr, COINIT_MULTITHREADED); if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac)))) fac = nullptr; }
  return fac;
}

bool decodeEncodedImage(const uint8_t* data, size_t len, std::vector<uint8_t>& rgba, int& w, int& h) {
  IWICImagingFactory* fac = wicFactory(); if (!fac) return false;
  IWICStream* st = nullptr; IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr; bool ok = false;
  if (SUCCEEDED(fac->CreateStream(&st)) && SUCCEEDED(st->InitializeFromMemory((WICInProcPointer)data, (DWORD)len)) &&
      SUCCEEDED(fac->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(fac->CreateFormatConverter(&cv)) &&
      SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
    UINT uw, uh; cv->GetSize(&uw, &uh); w = (int)uw; h = (int)uh; rgba.resize((size_t)w * h * 4); ok = SUCCEEDED(cv->CopyPixels(nullptr, uw * 4, (UINT)rgba.size(), rgba.data()));
  }
  if (cv) cv->Release(); if (fr) fr->Release(); if (dec) dec->Release(); if (st) st->Release();
  return ok;
}
#else
bool decodeEncodedImage(const uint8_t* data, size_t len, std::vector<uint8_t>& rgba, int& w, int& h) {
  int comp = 0; unsigned char* px = stbi_load_from_memory(data, (int)len, &w, &h, &comp, 4); if (!px) return false;
  rgba.assign(px, px + (size_t)w * (size_t)h * 4); stbi_image_free(px); return w > 0 && h > 0;
}
#endif

bool loadImage(const FileSource& fs, const std::string& nameIn, std::vector<uint8_t>& rgba, int& w, int& h) {
  const std::string name = lowerStr(nameIn); std::string stem = name; const size_t dot = stem.find_last_of('.');
  if (dot != std::string::npos && dot > stem.find_last_of('/') + 0) stem = stem.substr(0, dot);
  std::vector<std::string> cands = {name}; for (const char* e : {".tga", ".jpg", ".jpeg", ".png"}) cands.push_back(stem + e);
  for (auto& c : cands) {
    std::vector<uint8_t> d; if (!fs.read(c, d) || d.empty()) continue;
    const std::string ext = c.size() > 4 ? c.substr(c.find_last_of('.')) : "";
    if (ext == ".tga" ? decodeTga(d, rgba, w, h) : decodeEncodedImage(d.data(), d.size(), rgba, w, h)) return true;
  }
  return false;
}

Image makeTexture(const Context& c, const uint8_t* rgba, int w, int h, bool flipRows, bool srgb) {
  uint32_t mips = 1; for (int m = std::max(w, h); m > 1; m >>= 1) ++mips;
  Buf st = makeBuf(c, (size_t)w * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  for (int y = 0; y < h; ++y) memcpy(st.ptr + (size_t)y * w * 4, rgba + (size_t)(flipRows ? h - 1 - y : y) * w * 4, (size_t)w * 4);
  Image im = makeImage(c, w, h, srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, mips, VK_IMAGE_ASPECT_COLOR_BIT);
  oneShot(c, [&](VkCommandBuffer cb) {
    imageBarrier(cb, im.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, mips);
    VkBufferImageCopy bc = {}; bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; bc.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
    vkCmdCopyBufferToImage(cb, st.buf, im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    int mw = w, mh = h;
    for (uint32_t i = 1; i < mips; ++i) {
      imageBarrier(cb, im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, i - 1, 1);
      VkImageBlit bl = {}; bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1}; bl.srcOffsets[1] = {mw, mh, 1}; mw = std::max(1, mw / 2); mh = std::max(1, mh / 2);
      bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1}; bl.dstOffsets[1] = {mw, mh, 1};
      vkCmdBlitImage(cb, im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
    }
    if (mips > 1) imageBarrier(cb, im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, mips - 1);
    imageBarrier(cb, im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, mips - 1, 1);
  });
  vkDestroyBuffer(c.dev, st.buf, nullptr); vkFreeMemory(c.dev, st.mem, nullptr);
  return im;
}

}  // namespace nc
