// The in-match HUD as a 2D draw list (NATIVE-CLIENT.md section 9). The game (or the core) fills a HudList through HudBuilder; an executor (WebGL2 in the browser, Vulkan natively) only draws it.
// No allocation anywhere: the builder writes into buffers the host owns (capacity fixed at start); overflow sets a flag and drops the item, it never grows.
// Wire format (little endian, 4-byte aligned, version hud::VERSION = 2): Header 88 bytes, itemCount * Item (56), glyphCount * Glyph (12), defCount * GlyphDef (20), updateCount * AtlasUpdate (16), then uploadBytes of atlas pixels.
// Space: framebuffer pixels, origin top left, y down. Colours are straight (non-premultiplied) RGBA8, r in the low byte.
#pragma once
#include <stdint.h>

namespace hud {
static const uint32_t MAGIC = 0x31445548u;   // "HUD1"
static const uint32_t VERSION = 2;
static const int LAYERS = 8;                 // 0 world overlay (crosshair, CGaz), 1 vitals/timer/records, 2 notices, 3 chat, 4 scoreboard/list, 5 console, 6 menu overlay, 7 debug

// K_QUAD_ROT (added after the first executor shipped K_QUAD: an executor that does not know it draws nothing for it): a QUAD whose rectangle is rotated by the angle in `first` (float bits, radians, clockwise on screen) around the rectangle's centre.
// K_TRI (7): a filled triangle, (x, y), (w, h), (u0, v0) are its three points, color its colour (an older executor draws nothing for it). K_CIRCLE_CLIP_PUSH (8): like K_CLIP_PUSH, but the clip is the circle inscribed in the rectangle
// (x, y, w, h), edge anti-aliased by about a pixel; K_CLIP_POP ends it. Circle clips do not nest inside one another.
enum Kind : uint32_t { K_RECT = 1, K_QUAD = 2, K_GLYPHS = 3, K_CLIP_PUSH = 4, K_CLIP_POP = 5, K_QUAD_ROT = 6, K_TRI = 7, K_CIRCLE_CLIP_PUSH = 8 };
// blend (low 8 bits of Item::flags)
enum Blend : uint32_t { B_ALPHA = 0, B_ADD = 1, B_OPAQUE = 2 };   // alpha: SRC_ALPHA, ONE_MINUS_SRC_ALPHA; add: SRC_ALPHA, ONE; opaque: none

static const uint32_t F_OVERFLOW = 1, F_RESET = 2;   // Header::flags

struct Header {                // 88 bytes
  uint32_t magic, version, itemCount, glyphCount;
  uint32_t fbWidth, fbHeight, flags /*F_OVERFLOW, F_RESET*/, serial;   // serial: changes when the drawing changed or the list carries atlas data (defs / updates / reset)
  uint32_t hashLo, hashHi;     // FNV-1a 64 of items + glyphs (the drawing)
  uint32_t layerHash[LAYERS];  // FNV-1a 32 of the bytes of each layer (0 for an empty layer): re-record only layers whose hash changed
  uint32_t defCount, updateCount, uploadBytes, reserved;
};
struct Item {                  // 56 bytes
  uint32_t kind;
  uint32_t flags;              // bits 0-7 blend, bits 8-11 layer, bit 12 L_PLAIN (a layer-0 rectangle that is not a crosshair arm: the CGaz)
  float x, y, w, h;            // RECT / QUAD: the rectangle. GLYPHS: x,y = baseline-left origin, w,h = 0. CLIP: scissor rectangle (whole pixels)
  float u0, v0, u1, v1;        // QUAD: texture rectangle (0..1, v down)
  uint32_t color;              // RGBA8 (tint for QUAD / GLYPHS)
  uint32_t tex;                // QUAD: texture id the host registered; GLYPHS: font id (font atlas page)
  uint32_t first, count;       // GLYPHS: range in the glyph array
};
struct Glyph {                 // 12 bytes: one atlas quad of a run, position relative to the run origin, integer 26.6 pixels
  int32_t dx26, dy26;
  uint32_t glyphId;            // run font id 0 (dynamic atlas): a GlyphDef id the stream defined. Run font id >= 1 (static interim atlas): an index into that font's table
};
struct GlyphDef {              // 20 bytes: where a dynamic-atlas glyph is. A def with a known glyphId replaces the old one (the engine reuses ids after eviction)
  uint32_t glyphId;
  uint16_t x, y, w, h;         // rectangle in the page, pixels (R8 coverage, 1 px transparent padding around it)
  int16_t bearingX, bearingY;  // pixels from the run origin + glyph dx/dy (baseline-left) to the quad's top-left; bearingY negative above the baseline
  uint16_t page, reserved;
};
struct AtlasUpdate {           // 16 bytes: copy w*h tightly packed R8 bytes from the upload area (uploadOffset) to (x, y) of the page
  uint16_t page, reserved;
  uint16_t x, y, w, h;
  uint32_t uploadOffset;
};
static_assert(sizeof(Header) == 88 && sizeof(Item) == 56 && sizeof(Glyph) == 12 && sizeof(GlyphDef) == 20 && sizeof(AtlasUpdate) == 16, "wire layout");

inline uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | g << 8 | b << 16 | a << 24; }
static const uint32_t L_PLAIN = 16;   // OR-ed into a RECT's layer argument: layer 0 but NOT a crosshair arm (bit 12 of Item::flags), so a host does not centre-snap it
inline uint32_t flagsOf(uint32_t blend, uint32_t layer) { return (blend & 255u) | (layer & 15u) << 8 | (layer & L_PLAIN) << 8; }
inline uint32_t layerOf(const Item& it) { return it.flags >> 8 & 15u; }

}  // namespace hud
