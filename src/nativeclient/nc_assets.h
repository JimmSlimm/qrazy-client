// Files and images. A FileSource answers "give me this game file" (an extracted directory now; pk3 archives are a later FileSource). Decoding: TGA by hand, PNG/JPEG through WIC.
#pragma once
#include "nc_vk.h"
#include <string>
#include <vector>
#include <map>

namespace nc {

// Game files from a loose directory and any number of .pk3 (zip) archives. Lookup is case-insensitive and forward-slash. The first mount that has a name wins, so mount the highest priority first
// (a map's own archive before the shared libraries); the loose directory `root`, when set, is searched after the archives.
struct FileSource {
  std::string root;   // directory with extracted game files, forward slashes (may be empty)
  FileSource();
  ~FileSource();
  FileSource(const FileSource&) = delete; FileSource& operator=(const FileSource&) = delete;
  FileSource(FileSource&&) noexcept; FileSource& operator=(FileSource&&) noexcept;
  bool mountZip(const std::string& path, std::string* error = nullptr, uint64_t offset = 0, int shaderTier = 2);   // offset: where the archive starts inside the file (the asset store's QAS1 header is 4096 bytes)   // false = unreadable / not a zip
  bool read(const std::string& rel, std::vector<uint8_t>& out, int* archive = nullptr, std::string* actualName = nullptr) const;
  bool exists(const std::string& rel) const;
  std::vector<std::string> listArchives() const;
  struct ShaderSource { int tier; std::vector<uint8_t> bytes; };
  bool readShaderSources(int selectedArchive, std::vector<ShaderSource>& out) const;
  // One file placed at a virtual path (e.g. a model the asset store holds as a QAS1 file): `size` bytes at `offset` of `path`.
  bool mountFile(const std::string& virtualPath, const std::string& path, uint64_t offset, uint64_t size);
  bool readFrom(int archive, const std::string& rel, std::vector<uint8_t>& out) const;
  int archiveCount() const;
 private:
  bool readEntry(int archive, unsigned index, std::vector<uint8_t>& out) const;
  struct Zip; std::vector<Zip*> zips_;
  struct VFile { std::string name, path; uint64_t offset, size; }; std::vector<VFile> vfiles_;
};

std::string lowerStr(std::string s);
// name = a game path with or without extension; tries as given, then .tga .jpg .jpeg .png. false = not found or undecodable. Rows are top-down.
bool loadImage(const FileSource& fs, const std::string& name, std::vector<uint8_t>& rgba, int& w, int& h);
bool decodeEncodedImage(const uint8_t* data, size_t len, std::vector<uint8_t>& rgba, int& w, int& h);   // PNG / JPEG in memory

// RGBA8 UNORM texture with a full mip chain made by blits. flipRows: the game's textures are stored bottom-up for GL.
Image makeTexture(const Context& c, const uint8_t* rgba, int w, int h, bool flipRows, bool srgb = false);   // srgb: the texels are sRGB-encoded colour (the map's images, as the frame packet says) and are sampled to linear; false: linear data (lightmap pages, the HUD)

}  // namespace nc
