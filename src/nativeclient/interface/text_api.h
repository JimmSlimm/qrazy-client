/* The text engine's C API (text_engine.cpp, TEXT_API functions), the same in text.wasm and in the native static library (build-native-text.cmd|sh). NATIVE-CLIENT.md 9.7 / 9.8.
   One thread, no allocation after text_init (everything lives in the engine's fixed arena and tables). Fonts are byte ranges the host keeps alive and unchanged for the life of the engine. */
#ifndef QRAZY_TEXT_API_H
#define QRAZY_TEXT_API_H
#include <stdint.h>
struct QcMinimapOut;
#ifdef __cplusplus
extern "C" {
#endif
#define TEXT_SLOT_EMPTY   0xFFFFFFFFu   /* glyph without ink (space, empty mark): draw nothing */
#define TEXT_SLOT_PENDING 0xFFFFFFFEu   /* over the per-frame rasterisation budget: ask again next frame */

void text_init(void);                                          /* once, before anything else */
int  text_add_font(const uint8_t* data, uint32_t len);         /* returns the font index (order = fallback order) or -1; data stays valid and unchanged */
int  text_set_ui_scale(float scale);                          /* device pixels per layout pixel (1..4), before the first size: glyphs are rasterised at size * scale, layout and metrics stay in layout pixels; GlyphDef::reserved then carries 256 * wanted / rasterised size (0 = 1:1) */
int  text_add_size(uint32_t px);                              /* one R8 512x512 page per pixel size; returns the size id or -1 (max 4) */
void text_frame_begin(uint32_t raster_budget);                 /* once per frame: new glyph rasterisations allowed this frame */

/* lines (shaping cache + glyph cache) */
int      text_shape(const uint8_t* utf8, uint32_t len, uint32_t size_id, uint32_t flags);   /* flags bit 0 = right-to-left base direction; line handle or -1 */
int32_t  text_line_width26(int line);                          /* advance in 26.6 pixels */
uint32_t text_resolve(int line);                               /* glyph count; entries at text_resolved(): {int32 dx26, int32 dy26, uint32 slot, uint32 pad} */
const void* text_resolved(void);
uint32_t text_resolved_flags(void);                            /* bit 0: a glyph was over budget */
const void* text_glyph_def(uint32_t slot);                     /* {u16 x,y,w,h; i16 bearingX,bearingY; u32 page} */
const uint8_t* text_atlas_page(uint32_t size_id);              /* R8, text_atlas_size() squared */
uint32_t text_atlas_size(void);
const uint32_t* text_take_dirty(uint32_t size_id);             /* {x0,y0,x1,y1} since the last call, or NULL */
uint32_t text_stat(int which);                                 /* 0 shape misses 1 hits 2 rasterised 3 glyph hits 4 flushes 5 pending 6 oversize 7 evictions 8 arena allocs 9 frees 10 live bytes 11 peak bytes 12 failed */

/* the HUD draw list (hud_list.hpp v2) with dynamic atlas data */
void text_hud_init(void);                                      /* after text_init */
void text_hud_reset(void);                                     /* the next list resends every glyph def and every page in full */
void text_reset_hud(void);                                     /* a new HUD on this engine (interface 11, added after the first 11 release): every size class, glyph slot and cached line dropped, UI scale 1, the list state clean; fonts, nameplate bakes and the chat / console stores stay, text_set_ui_scale / text_add_size work again */
void text_hud_begin(uint32_t fb_width, uint32_t fb_height);
void text_hud_rect(uint32_t layer, float x, float y, float w, float h, uint32_t rgba);
uint32_t text_hud_text(uint32_t layer, float x, float y, uint32_t rgba, const uint8_t* utf8, uint32_t len, uint32_t size_id, uint32_t flags);   /* 1 = a glyph was over budget, 2 = failed */
uint32_t text_hud_finish(void);                                /* byte count of the list at text_hud_out(), 0 if it does not fit */
const uint8_t* text_hud_out(void);

/* text helpers for producers */
int32_t  text_measure26(const uint8_t* utf8, uint32_t len, uint32_t size_id, uint32_t flags);   /* advance in 26.6 pixels, QL colour codes (^0-^9) not counted */
uint32_t text_size_for_px(uint32_t px);                        /* size id of about px pixels (8..72): an existing size, a new one while there is room (max 4), else the nearest */
uint32_t text_hud_text_ql(uint32_t layer, float x, float y, uint32_t rgba, const uint8_t* utf8, uint32_t len, uint32_t size_id, uint32_t flags);   /* one line with QL colour codes; 1 = a glyph was over budget */
/* The nameplate image over other players and ghosts (interface 11, nothing else of the engine is touched: no atlas, no size class): TEXT_PLATE_W x TEXT_PLATE_H RGBA8, straight alpha, one text_plate_bake call per change of what it shows.
   name: UTF-8 with QL colour codes and the clan-tag marks of clan-tag.js (U+E002 tag U+E000 name; U+E001 after U+E002 = an official tag, underlined); drawn bold 40 px centred (the clan tag 0.78 of that), baseline y 44.
   sub: the second line (physics + weapon mode, the connection warning), bold 26 px #d0d0d0, centred, baseline y 84; may be empty. Returns 1 on success, 0 when it could not be drawn (no font yet, text too long). Fonts: the engine's bold chain. */
#define TEXT_PLATE_W 384
#define TEXT_PLATE_H 96
uint32_t text_plate_bake(const uint8_t* name, uint32_t name_len, const uint8_t* sub, uint32_t sub_len);
uint32_t text_plate_bake_color(const uint8_t* name, uint32_t name_len, const uint8_t* sub, uint32_t sub_len, uint32_t rgba);   /* the same plate with a base colour (0xAABBGGRR, alpha forced opaque) for text without its own QL colour code: the route viewer's speed tags (name = the speed text, sub empty) */
const uint8_t* text_plate_pixels(void);                      /* the last bake: TEXT_PLATE_W * TEXT_PLATE_H * 4 bytes, valid until the next bake */
/* A world text board (the built-in runway's START / FINISH / checkpoint signs): TEXT_BOARD_W x TEXT_BOARD_H RGBA8, straight alpha, top row first; one line, bold 72 px (the engine's bold chain), centred, baseline y 87,
   colour rgba (0xAABBGGRR, alpha forced opaque). Returns 1 on success. The pixels share the nameplate buffer: valid until the next plate or board bake. */
#define TEXT_BOARD_W 1024
#define TEXT_BOARD_H 128
uint32_t text_board_bake(const uint8_t* text, uint32_t len, uint32_t rgba);
const uint8_t* text_board_pixels(void);
/* wraps at spaces to max_width_px, at most max_lines lines, ellipsis on the last; returns lines drawn (low 16 bits) | (over budget << 16). Line height = the face's height at that size. */
uint32_t text_hud_text_wrapped(uint32_t layer, float x, float y, uint32_t rgba, const uint8_t* utf8, uint32_t len, uint32_t size_id, uint32_t flags, uint32_t max_width_px, uint32_t max_lines);

/* the first producers (hud_producers.hpp logic on this engine, with the hud-editor layout): crosshair, timer, vitals. All fields are 32 bit (no padding). */
typedef struct TextHudState {
  uint32_t fbWidth, fbHeight;
  uint32_t crosshair;                 /* 0 cross, 1 gap, 2 cross + dot, 3 dot, 4 circle, 5 circle + dot, 6 X (hud-editor.js CROSSHAIRS order) */
  uint32_t crosshairSize, crosshairThickness, crosshairColor, crosshairHidden;   /* px, px, RGBA8, 0/1 */
  int32_t  timerMs;                   /* < 0: no timer */
  float    timerX, timerY, timerScale;   /* hud-editor element centre as a fraction of the framebuffer (0..1); x or y < 0 = the default position */
  uint32_t timerHidden;               /* bit 0: hidden; bit 1: the status line is shown above the number (it moves the default position down) */
  int32_t  health, armor, ammo;       /* ammo < 0: none */
  float    vitalsX, vitalsY, vitalsScale;
  uint32_t vitalsFlags;               /* 1 hidden, 2 vertical, 4 hide ammo, 8 hide zero armor, 16 icons before the numbers, 32 numbers in plain white (the icons keep their colour), 64 health / armor bars (90 x 6 px, full at 100) */
  uint32_t sizeVitalsBase, sizeTimerBase;   /* pixel size at scale 1 (22 and 44 in the CSS defaults) */
  uint32_t healthColor, armorColor, ammoColor, ammoIcon;   /* RGBA8 tint of each icon and number (0 = the default colour); the ammo icon index (the held weapon's, hud_icons.hpp order) */
} TextHudState;
void text_hud_timer_status(const uint8_t* utf8, uint32_t len);   /* the timer's status line (READY, CHECKPOINT 2 / 5...) for the next text_hud_produce; drawn only with timerHidden bit 1; ASCII / UTF-8, cut at 47 bytes */
uint32_t text_hud_produce(const TextHudState* state);          /* adds the elements to the list being built (between text_hud_begin and text_hud_finish); returns 1 if a glyph was over budget */

/* more producers. Same layout rules as TextHudState: x, y = element centre as a fraction of the framebuffer, negative = the CSS default position. */
typedef struct TextSpeedState {
  uint32_t fbWidth, fbHeight;
  int32_t  speed;                     /* units per second */
  uint32_t barMax, barWidth, barHeight;   /* hud-editor speed settings: 1200 / 190 / 3 by default */
  uint32_t flags;                     /* 1 hidden, 2 no bar, 4 no unit */
  float    x, y, scale;
} TextSpeedState;
uint32_t text_hud_speedometer(const TextSpeedState* state);    /* the number (58 px) with "UPS" beside it and a bar under it */
uint32_t text_hud_fps(uint32_t fb_width, uint32_t fb_height, int32_t fps, float x, float y, float scale, uint32_t hidden);   /* "250 FPS", top right by default */
/* The core-produced HUD, first slice (GOAL-NO-THREEJS.md "B3 design"): the host does not build TextHudState / TextSpeedState any more. It writes the player's layout once per change into text_hud_layout_ptr() and the
   game numbers once per frame into text_hud_frame_ptr(), then calls text_hud_frame_go(), which draws timer, vitals, crosshair, speedometer and fps exactly as text_hud_produce + text_hud_speedometer + text_hud_fps do
   with the structs the JS feed used to build (the draw list is byte-equal: render-core/text/hud-core-oracle.test.js). All fields 32 bit but the doubles, which come first. */
typedef struct TextHudLayout {
  uint32_t crosshair, crosshairSize, crosshairThickness, crosshairColor, crosshairHidden;   /* as TextHudState */
  float    timerX, timerY, timerScale; uint32_t timerHidden;                                /* as TextHudState: bit 0 hidden, bit 1 status line shown */
  float    vitalsX, vitalsY, vitalsScale; uint32_t vitalsFlags;
  uint32_t speedBarMax, speedBarWidth, speedBarHeight, speedFlags; float speedX, speedY, speedScale;   /* as TextSpeedState */
  float    fpsX, fpsY, fpsScale; uint32_t fpsHidden;
  uint32_t wbLayout, wbFlags, wbOnlyOwned, wbOnlyWithAmmo; float wbX, wbY, wbScale;   /* the weapon bar: TextWeaponBar layout / flags (1 no icons, 2 no ammo, 4 hidden, 8 names, 16 only show while switching: the engine hides the bar 1500 ms after the last switch, see TextHudFrame.wbClock), which weapons it lists, placement */
  uint32_t puLayout, puFlags; float puX, puY, puScale;                                 /* the powerups list: TextPowerups layout / flags (1 hidden, 2 no icons, 4 no time, 8 bars, 16 names, 32 plain white names), placement */
} TextHudLayout;
#define TEXT_HUD_MAX_BAR 16
#define TEXT_HUD_MAX_PU 8
typedef struct TextHudFrame {
  double   health, armor, ammo;       /* the game's values (ammo < 0: none); the colours of the numbers follow health / armor */
  double   barAmmo[TEXT_HUD_MAX_BAR]; /* per bar weapon (bar order): its ammo, Infinity = infinite (0 for a weapon not owned) */
  double   puLeft[TEXT_HUD_MAX_PU];   /* per drawn powerup: seconds left, Infinity = no countdown */
  uint32_t fbWidth, fbHeight;
  int32_t  timerMs, held;             /* timerMs < 0: no timer; held: the held weapon's index in the bar's weapon order (hud_icons.hpp), -1 none */
  int32_t  speed, fps;                /* fps < 0: no fps counter */
  uint32_t barOwned;                  /* bit i: bar weapon i is owned */
  uint32_t puRaw;                     /* powerups running before the known-power filter: the list is drawn (even with no known one) when > 0 */
  uint32_t puCount;                   /* the known ones, to draw */
  uint32_t puIndex[TEXT_HUD_MAX_PU];  /* their index in the powerup table */
  uint32_t wbClock;                   /* the host's clock for the weapon bar's "only show while switching" (wbFlags 16): bits 0-29 = milliseconds (performance.now() / any steady ms clock, wraps), bit 30 = always shown (the HUD editor's preview), bit 31 = a weapon switch is in progress. The bar shows for 1500 ms after the held weapon changes (the engine sees that) or while bit 31 is set, then hides. 0 = no clock: the bar stays shown. (This was pad0: interface 11 additions only) */
} TextHudFrame;
void text_hud_set_weapon_table(const uint32_t* rgba, const int32_t* lowAmmo, uint32_t count);   /* once: the bar weapons in bar order (hud-status.js BAR_WEAPONS): RGBA8 colour (the held weapon's ammo icon and number take it) and the ammo at or under which the bar flags it low */
void text_hud_set_powerup_table(const uint32_t* rgba, const uint32_t* icon, uint32_t count, const uint8_t* labels, const uint32_t* labelLen);   /* once: the known powerups: colour, icon index, name (the names back to back in `labels`) */
uint32_t text_hud_layout_ptr(void);
uint32_t text_hud_frame_ptr(void);
void*    text_hud_layout_addr(void);   /* the same two blocks as FULL-WIDTH addresses: a 64-bit native host cannot use the 32-bit text_hud_*_ptr values (those are wasm heap offsets). memcpy a TextHudLayout / the 288-byte TextHudFrame there */
void*    text_hud_frame_addr(void);
uint32_t text_hud_frame_go(void);   /* between text_hud_begin and text_hud_finish; returns 1 if a glyph was over budget */
/* chat feed: messages separated by '\n' (oldest first, QL colour codes), wrapped to width_px, stacked upward from the bottom-left corner (left, bottom) in pixels; the newest at the bottom, older ones dropped when height_px is full */
/* scroll_lines = wrapped lines skipped from the newest (0 = bottom); the box clips a message cut by its top edge. Returns the total number of wrapped lines (low 16 bits; clamp the caller's scroll to total - height_px / line height) | (1 << 16 if a glyph was over budget). */
uint32_t text_hud_chat(float left, float bottom, uint32_t width_px, uint32_t height_px, uint32_t size_id, uint32_t rgba, const uint8_t* utf8, uint32_t len, uint32_t scroll_lines, uint32_t style);
/* input line: the text clipped to width_px and scrolled so the caret (byte offset into the UTF-8 text) stays visible; caret bar when blink_on. (left, baseline) in pixels. Returns the caret x relative to the box's left edge. */
int32_t text_hud_input(uint32_t layer, float left, float baseline, uint32_t width_px, uint32_t size_id, uint32_t rgba, const uint8_t* utf8, uint32_t len, uint32_t caret_byte, uint32_t blink_on, uint32_t style);
uint32_t text_wrap_lines(const uint8_t* utf8, uint32_t len, uint32_t size_id, uint32_t flags, uint32_t max_width_px, uint32_t max_lines);   /* lines text_hud_text_wrapped would use */

/* generic text elements (pickup message, notices, clock, player list, jump speeds, checkpoint split, race results ...): the caller formats the strings and chooses the anchor.
   (ax, ay) pixel anchor; h_align 0 left / 1 centre / 2 right; v_align 0 top / 1 middle / 2 bottom (of the whole block); lines separated by '\n', max 64, stacked downward with line_gap extra pixels; QL colour codes; shadow = 1 px dark copy. Returns 1 if a glyph was over budget. */
uint32_t text_hud_lines(uint32_t layer, float ax, float ay, uint32_t h_align, uint32_t v_align, uint32_t size_id, uint32_t rgba, uint32_t shadow, int32_t line_gap, const uint8_t* utf8, uint32_t len, uint32_t style);

/* icons (quad registry id 2, hud-assets/hud-icons.png: 18 cells of 32 px; indices in hud_icons.hpp: weapons 0..8 in BAR_WEAPONS order, health 9, armor 10, ammo 11, powerups 12..17) */
typedef struct TextWeaponItem { uint32_t icon, color; int32_t ammo; uint32_t flags; } TextWeaponItem;   /* ammo < 0: not shown (infinite or none); flags 1 held, 2 low ammo, 4 empty, 8 not owned */
typedef struct TextWeaponBar { uint32_t fbWidth, fbHeight, layout /* 0 vertical 1 horizontal */, flags /* 1 no icons, 2 no ammo, 4 hidden */, itemCount; float x, y, scale; } TextWeaponBar;
uint32_t text_hud_weapon_bar(const TextWeaponBar* bar, const TextWeaponItem* items);   /* at most 16 items; the caller filters (hud-status.js weaponBarItems) */
typedef struct TextPowerup { uint32_t icon, color; int32_t tenths; uint32_t labelOff, labelLen; } TextPowerup;       /* tenths of a second left ("12.3s"; from 100 s whole seconds x 10); < 0: no time shown for this one. labelOff / labelLen: its name in `labels` (UTF-8) */
typedef struct TextPowerups { uint32_t fbWidth, fbHeight, layout, flags /* 1 hidden, 2 no icons, 4 no time, 8 time bars, 16 names, 32 names in plain white */, count; float x, y, scale; } TextPowerups;
uint32_t text_hud_powerups(const TextPowerups* p, const TextPowerup* items, const uint8_t* labels);           /* at most 8 */

/* OLED dimming. Row bits use HUD_EL_* indices. Widget bits use HUD_BOX_* minus 32, with bit 9 for CGaz and bit 10 for chat. QL speedometer uses the speedometer bit. */
#define TEXT_HUD_OLED_ALPHA 153u
enum { TEXT_HUD_OLED_CGAZ = 9, TEXT_HUD_OLED_CHAT = 10 };
void text_hud_set_oled(uint32_t rows, uint32_t widgets);
void text_hud_alpha_begin(uint32_t alpha255);   /* the items pushed until text_hud_alpha_end get their colour alpha scaled by alpha255 / 255 */
void text_hud_alpha_end(void);
int text_add_style_font(uint32_t style, const uint8_t* data, uint32_t len);   /* style 2 = monospace face (the HUD's monospace text), 3 = black face (weight 900 numbers): leads that style's chain, the fonts added with text_add_font follow. After the text_add_font calls. */

/* Generic styled text block (every HUD element that is just text: records, jump speeds, start speed, checkpoint split, pickup message, notices, clock...). Rows of runs; a run has its own size / colour / weight / letter-spacing and the
   runs of a row share a baseline; rows are stacked (rowGap) and aligned (rowAlign 0 left, 1 centre, 2 right) inside a box (padX / padY / minWidth, bg, 1 px border). The box sits with its (hAlign, vAlign) point at (x, y) in pixels,
   or, with flags & 1, is centred on (x, y) (the HUD editor's placement). Everything scales with `scale`. runs[i].offset / len index `utf8`. At most 64 runs (a run is cut at 47 bytes) in 16 rows. */
typedef struct TextRun { uint32_t offset, len, px, rgba, flags /* 1 bold, 2 no shadow, 4 starts a new row, 8 per character (tabular digits), 16 a colour square of px, 32 an icon (index in offset, px wide), 64 monospace face, 128 black face, 256 the analog clock face (px wide; hour / minute / second hand angles in degrees in track / padX / padY, second < 0: no seconds hand) */; float track /* letter-spacing px */, gap /* px before the run, not the first of a row */; uint32_t bg /* background box behind the run (alpha 0: none), grown by padX px each side and padY px above and below */; float padX, padY; float lineH /* line height px (0: the face's own); the text is centred in it */; } TextRun;
typedef struct TextRowsBlock { uint32_t fbWidth, fbHeight, layer, hAlign, vAlign, rowAlign, flags /* 1 centre on x, y; 2 the border is only the top and bottom line */, runCount; float x, y, scale, rowGap, padX, padY, minWidth; uint32_t bg, border; } TextRowsBlock;
uint32_t text_hud_rows(const TextRowsBlock* block, const TextRun* runs, const uint8_t* utf8);   /* 1 = a glyph was over budget */

/* Row slots: a styled text block kept between frames. The host writes the TextRowsBlock, the TextRun array and the text into the slot's memory (text_hud_row_slot_ptr; the layout from text_hud_row_slot_info) only when the block's content changed,
   and draws it every frame by number: the same list items as text_hud_rows with those three arguments. 48 slots of up to 64 runs and 3072 text bytes. */
uint32_t text_hud_row_slot_ptr(uint32_t slot);
void*    text_hud_row_slot_addr(uint32_t slot);   /* the full-width address of the slot (null: no such slot): a native host uses this one */
void*    text_hud_chat_store_addr(void);          /* the chat text store (text_hud_chat_store_cap bytes), as a full-width address */
void*    text_hud_input_store_addr(void);         /* the input line's text store, as a full-width address */
/* RULE: every text_*_ptr above is a 32-bit WASM heap offset. A native host must use the text_*_addr twin; the native library does not even export the _ptr ones. */
uint32_t text_hud_row_slot_info(uint32_t what);
uint32_t text_hud_row_slot_draw(uint32_t slot);

/* The HUD's styled-text elements built inside the engine (render-core/text/hud_recipes.hpp): the recipe `kind` turns the game's text (utf8, in.textLen bytes) and the element's placement into row slots from in.slot on
   (records, checkpoint split, start speed, jump speeds, pickup message, notices, player list, race results, paused, clock, ...). Returns the number of slots written (0: nothing to draw / did not fit); draw slots slot .. slot+n-1
   with text_hud_row_slot_draw every frame. The page makes these calls only when an element's data changes (hud-recipes.mjs coreArgs) and sends them to a native host as "rows" in qrazyNativeHud. */
typedef struct TextRecipeIn {
  double   elX, elY, elScale;          /* the HUD editor's placement (doubles: the JS recipe multiplies in double, then rounds once); x / y < 0: not moved, scale 0: 1 */
  double   num, d0, d1, d2;            /* recipe specific numbers */
  uint32_t fbWidth, fbHeight, slot;    /* the first row slot written */
  uint32_t color, color2;              /* the element's option colours (RGBA8, as hud-recipes.mjs rgba() makes them) */
  uint32_t flags, aux;                 /* recipe specific */
  uint32_t textLen, pad0;
} TextRecipeIn;
enum { RECIPE_SLICK = 0, RECIPE_ANGLE = 1, RECIPE_PAUSED = 2, RECIPE_JUMPSPEEDS = 3, RECIPE_NOTICES = 4, RECIPE_PICKUP = 5, RECIPE_CLOCK = 6, RECIPE_INPUTCMDS = 7, RECIPE_STARTSPEED = 8, RECIPE_CPSPLIT = 9, RECIPE_RECORDS = 10, RECIPE_PLAYERS = 11, RECIPE_RACE = 12, RECIPE_LOGIN = 13, RECIPE_SPECTATOR = 14, RECIPE_VOTE = 15, RECIPE_MAPINFO = 16, RECIPE_BANNER = 17, RECIPE_PEER_RECORD = 18, RECIPE_LIQUID = 19, RECIPE_TRIGGER_LEGEND = 20, RECIPE_LOADING = 21, RECIPE_RACE_DIALOG = 22, RECIPE_RACE_SCORE = 23, RECIPE_STATUS = 24, RECIPE_COURSE = 25, RECIPE_HINTS = 26 };   /* 24-26 (additive): the header chrome a native host has no DOM for. STATUS: the brand wordmark + session / mode / weapon-mode labels (text: brand \x1f session \x1f mode \x1f weapons). COURSE: the map name card (text lines: eyebrow, title, subtitle). HINTS: the footer key hints (text lines: left, right) */
uint32_t text_hud_recipe(uint32_t kind, const TextRecipeIn* in, const uint8_t* utf8);

/* HUD ELEMENTS owned by the engine (interface version 5; plan F1): the fixed table of the styled-text elements, in draw order, with their row slots. A host says WHAT an element shows (text_hud_el_set: the recipe kind, its
   arguments and text, an expiry) or that it is gone (text_hud_el_clear), and calls text_hud_el_frame once per frame: it rebuilds the elements whose call or the frame size changed and draws every live one in the table's order.
   No slot numbers, no show lists, no per-element active / expiry logic in the host. The element's placement (in.elX, elY, elScale), colours and flags are part of the call, as with text_hud_recipe (fbWidth, fbHeight, slot are ignored:
   the frame call and the table decide). */
enum { HUD_EL_PICKUP = 0, HUD_EL_START_SPEED = 1, HUD_EL_CP_SPLIT = 2, HUD_EL_JUMP_SPEEDS = 3, HUD_EL_LOGIN = 4, HUD_EL_ANGLE = 5, HUD_EL_SLICK = 6, HUD_EL_NOTICES = 7, HUD_EL_RECORDS = 8, HUD_EL_PLAYERS = 9,
       HUD_EL_RACE = 10, HUD_EL_CLOCK = 11, HUD_EL_INPUT_CMDS = 12, HUD_EL_PAUSED = 13, HUD_EL_SPECTATOR = 14, HUD_EL_VOTE = 15, HUD_EL_MAPINFO = 16, HUD_EL_BANNER = 17, HUD_EL_PEER_RECORD = 18, HUD_EL_LIQUID = 19, HUD_EL_TRIGGER_LEGEND = 20, HUD_EL_LOADING = 21, HUD_EL_RACE_PANEL = 22, HUD_EL_RACE_CHALLENGE = 23, HUD_EL_RACE_SCORE = 24, HUD_EL_STATUS = 25, HUD_EL_COURSE = 26, HUD_EL_HINTS = 27, HUD_EL_COUNT = 28 };
int32_t  text_hud_el_set(uint32_t element, uint32_t kind, const TextRecipeIn* in, const uint8_t* utf8, double until_ms);   /* 1 changed, 0 the same call, -1 bad element / text over 4096 bytes. until_ms: the clock value after which the element is gone (the host's clock, the same one text_hud_el_frame gets); 0 = until cleared */
void     text_hud_el_clear(uint32_t element);
void     text_hud_el_clear_all(void);
uint32_t text_hud_el_frame(uint32_t fb_width, uint32_t fb_height, double now_ms);   /* returns the number of row slots drawn */
/* The same in two parts, for a host that draws the chat box / console log view (text_lv_draw) between them: part 0 draws the elements that belong BELOW the log view, part 1 the ones that belong ABOVE it (the map-info popup 16, the loading screen 21, the race dialogs 22 and 23), part 2 = both (= text_hud_el_frame). Call part 0, the log view, then part 1. */
uint32_t text_hud_el_frame_part(uint32_t fb_width, uint32_t fb_height, double now_ms, uint32_t part);
/* Where an element's first row slot was drawn in the last text_hud_el_frame, in the frame's layout pixels, for a host that makes parts of it clickable (the map-info popup's links and close button): 6 floats per record into `out`:
   record 0 = the box {x, y, width, height, run count, 0}; record i + 1 = run i {x, y, width, height, TextRun.flags, i}. A run with TextRun flag 512 (RUN_LINK) is a link / button; the i-th such run is the i-th link the element's text lists
   (map info: the QL leaderboard links in their order, then "Close"). At most max_runs runs; returns the runs written, 0 if the element was not drawn in the last frame. */
uint32_t text_hud_el_rects(uint32_t element, float* out, uint32_t max_runs);

/* WHERE A HUD ELEMENT WAS DRAWN (additive, interface 11): the box of an element in the list built since the last text_hud_begin, for the HUD editor's drag / resize / snap / hit boxes (the editor does not measure page elements;
   the engine knows what it drew). id: a HUD_EL_* element (the union of its row slots' boxes), or one of the HUD_BOX_* below (the producers' own elements). out: 4 floats {x, y, width, height} in the frame's layout pixels.
   Returns 1 when the element was drawn in that list, else 0 (hidden, nothing to show, not part of the frame). Elements the editor draws dimmed (hidden in the layout) have boxes too. A list that draws an element twice keeps the last. */
enum { HUD_BOX_TIMER = 32, HUD_BOX_SPEED = 33, HUD_BOX_VITALS = 34, HUD_BOX_WEAPONS = 35, HUD_BOX_POWERUPS = 36, HUD_BOX_FPS = 37, HUD_BOX_CROSSHAIR = 38, HUD_BOX_MINIMAP = 39, HUD_BOX_QL_SPEEDO = 40, HUD_BOX_CLIPPY = 41 };
uint32_t text_hud_box(uint32_t id, float* out);

/* ENTRY ANIMATION of the pickup message (HUD_EL_PICKUP), start speed (HUD_EL_START_SPEED) and checkpoint split (HUD_EL_CP_SPLIT) (additive, interface 11): the recipe flags' bits 28-29 (TEXT_ANIM_SHIFT) pick it: 0 none, 1 pop
   (scale + fade in), 2 slide (up + fade in). The engine keeps the clock: it starts at the first frame the element is drawn after text_hud_el_set changed it (a new text / expiry replays it), the host only supplies now_ms. The draw list
   carries it as a transform: a K_GLYPHS item's w > 0 is a scale (0 = 1) the executor applies to the run's glyph offsets and quads (hud_list.hpp). */
enum { TEXT_ANIM_SHIFT = 28 };

/* LOG VIEW (interface version 5, plan F2): the scrollable, selectable text view of the chat box (view 0) and the console (view 1) in the engine. The host appends entries (QL colour codes, inline RGB escapes; at most 384 bytes each,
   512 entries per view), the engine wraps them to the width, draws them bottom-up (newest at the bottom edge) clipped to the box with a scrollbar in the right-hand 10 px, scrolls (it stays at the bottom unless the reader scrolled up;
   lines added or dropped keep a scrolled-up view where it was) and handles the pointer for selection and the fold heads. The selected text is returned PLAIN; only the clipboard write is the host's.
   Everything is in the frame's layout pixels. The text model of the input line stays with the host (text_hud_input / text_hud_selection). */
enum { LV_F_MONO = 1, LV_F_FOLD_HEAD = 2 };
void     text_lv_clear(uint32_t view);
void     text_lv_config(uint32_t view, uint32_t width_px, uint32_t height_px, uint32_t size_id, uint32_t style, uint32_t gap_px, uint32_t max_entries);   /* style: text flags (2 bold, 4 mono); max_entries <= 512: older ones are dropped */
int32_t  text_lv_append(uint32_t view, const uint8_t* utf8, uint32_t len, uint32_t rgba, uint32_t flags, uint32_t group);   /* flags LV_F_*; group 1..255 = a member of a foldable block (the LV_F_FOLD_HEAD entry of the same group is its head); returns the entry's sequence number, -1 bad view */
void     text_lv_set_repeat(uint32_t view, uint32_t repeats_more);   /* the newest entry repeated: " (repeated N more)" in gray after it (0 removes it) */
void     text_lv_pop(uint32_t view);                                  /* removes the newest entry */
uint32_t text_lv_group_collapse(uint32_t view, uint32_t group, uint32_t collapsed);   /* fold / unfold a group; 1 when it changed */
uint32_t text_lv_group_is_collapsed(uint32_t view, uint32_t group);
void     text_lv_scroll(uint32_t view, int32_t delta_px);            /* > 0 = towards older lines */
void     text_lv_scroll_page(uint32_t view, int32_t dir);            /* +1 PageUp, -1 PageDown */
void     text_lv_scroll_end(uint32_t view, uint32_t to_bottom);      /* 1 = follow the newest line, 0 = the oldest */
uint32_t text_lv_line_height(uint32_t view);
void     text_lv_state(uint32_t view, float* out4);                  /* scroll px from the bottom, content height, view height, 1 when following the newest line */
uint32_t text_lv_has_selection(uint32_t view);
void     text_lv_select_clear(uint32_t view);
void     text_lv_select_all(uint32_t view);
uint32_t text_lv_copy(uint32_t view);                                /* the selected text, plain, entries joined by '\n': its length; the bytes are at text_lv_copy_addr() */
void*    text_lv_copy_addr(void);
uint32_t text_lv_copy_ptr(void);
uint32_t text_lv_pointer(uint32_t view, uint32_t event, float x, float y, uint32_t mods);   /* event 0 down, 1 move, 2 up, 3 double click, 4 triple click; mods bit 0 shift. Returns bits: 1 inside, 2 changed, 4 a fold head toggled, 8 keep calling text_lv_tick */
uint32_t text_lv_tick(uint32_t view, double dt_ms);                  /* drag auto-scroll while the pointer is held outside the box; 1 when it scrolled */
uint32_t text_lv_draw(uint32_t view, float left, float top, uint32_t width_px, uint32_t height_px, uint32_t size_id, uint32_t style, uint32_t gap_px, uint32_t sel_rgba, uint32_t layer);   /* 1 = a glyph was over budget */

/* QL's cg_speedometer styles (interface version 5): mode 1 = value + speed graph in the lower right corner, 2 = value + graph under the crosshair, 3 = value only under the crosshair; 0 / 4 draw nothing (4 is the Qrazy speedometer
   of text_hud_produce). Call once per frame with the horizontal speed, the scale of the graph (the speedometer's bar maximum, 1200 by default) and the frame clock in ms: the engine keeps the graph's history (one column per 25 ms).
   Returns 1 = a glyph was over budget (like text_hud_rows). Sizes follow the window height (10 vh graph, 2.5 vh value). */
uint32_t text_hud_ql_speedo(uint32_t fb_width, uint32_t fb_height, uint32_t mode, double speed, double max, double now_ms);

/* Clippy, the paperclip easter egg (clippy.js: shown at the bottom centre with a speech bubble; the host decides WHEN and for how long: 10 s). Call once per frame while he is up, between text_hud_begin and text_hud_finish.
   shown_ms: ms since he appeared (< 0 = draw nothing); the engine animates from it (slide-up with a bounce, the bubble pops in 0.45 s later, then he wiggles, blinks and looks around).
   utf8 / len: the bubble's text (UTF-8, at most 512 bytes), given when it changes; len 0 keeps the last one. Fixed pixel sizes (not the HUD scale), layer 3, drawn as solid triangles / rectangles + wrapped text: no image to load.
   His box (for a client that wants to dismiss him by a click) is text_hud_box(HUD_BOX_CLIPPY). Returns 1 = a glyph was over budget. */
uint32_t text_hud_clippy(uint32_t fb_width, uint32_t fb_height, double shown_ms, const uint8_t* utf8, uint32_t len);

/* a mouse click in an input line: the caret byte offset nearest to x_px pixels from the text's left edge (add the scroll text_hud_input applied: caret_x returned there = caret - scroll). Left-to-right text. */
uint32_t text_caret_from_x(const uint8_t* utf8, uint32_t len, uint32_t size_id, uint32_t style, int32_t x_px);

/* CGaz strafe HUD (cgaz.js maths and drawing order; rectangles in QL's 640x480 virtual screen scaled to the framebuffer, layer 0). Doubles first, then 32-bit fields. */
typedef struct TextCgazState {
  double yaw, vx, vz, vPrev, movementDir, fovX, accelWidth, ypos;   /* degrees, units/s, previous speed, -1/0/1, horizontal FOV degrees, accel bar width, band y in virtual px (< 0: 260) */
  double wish, accel, opposingAccel, friction, speed, preSpeed;     /* cgazParams / physics snapshot: opposingAccel 0 = none, friction 0 = none, speed / preSpeed < 0 = absent */
  uint32_t fbWidth, fbHeight, flags /* 1 hidden, 2 full style (compass) */, keysHeld, supported /* 0: no zones */, pad;
} TextCgazState;
void text_hud_cgaz(const TextCgazState* state);

/* caret positions from the shaped clusters (right-to-left runs mirrored, ligatures split evenly). Input text is shown as typed (colour codes literal). */
int32_t text_caret_x26(const uint8_t* utf8, uint32_t len, uint32_t size_id, uint32_t style, uint32_t caret_byte);   /* x of the caret in 26.6 pixels from the text's left edge */
/* selection highlight behind an input line: call BEFORE text_hud_input with the same box / text / caret; one rectangle per selected character */
void text_hud_selection(uint32_t layer, float left, float baseline, uint32_t width_px, uint32_t size_id, uint32_t style, const uint8_t* utf8, uint32_t len, uint32_t caret_byte, uint32_t from_byte, uint32_t to_byte, uint32_t rgba);

/* minimap (minimap.hpp, NATIVE-CLIENT.md 9.11): the baked top view is quad texture 3 (the host renders it once per map with rc_minimap_camera); rotated quads are item kind 6 (K_QUAD_ROT). Doubles first. */
typedef struct TextMinimapState {
  double cx, cz, S;                                          /* the bake's bounds (minimapBounds in minimap.js): centre x / z and side */
  double px, pz, yaw, speed, dt, prevWidth, range, speedScale;   /* player, yaw (radians), horizontal speed, seconds since the last frame, last frame's view width (0: none), flat range, speed-zoom percent */
  uint32_t fbWidth, fbHeight, zoom /* 0 full, 1 flat, 2 speed */, flags /* 1 hidden, 2 no rotate, 4 trail, 8 ghost line (past), 16 ghost line past + future, 32 circle shape, 64 FOV cone */, trailCount, ghostCount, ghostK, boxPx /* 0: 400 */;
  uint32_t trailColor, ghostColor, fovTenths, pad1;           /* RGBA8, RGBA8, the horizontal field of view in tenths of a degree (the cone), unused */
  float x, y, scale, pad2;                                   /* hud-editor centre fractions (< 0: bottom right, 36 px / 150 px from the border), scale */
} TextMinimapState;
double text_hud_minimap(const TextMinimapState* state, const float* trail_xz, const float* ghost_xz);   /* returns the view width used */
/* Live world pass: emits the box and overlays with no texture 3. Uses view's center, width, yaw, rotation and ghost counts. State supplies layout, player x/z, and overlay options. */
double text_hud_minimap_live(const TextMinimapState* state, const struct QcMinimapOut* view, const float* trail_xz, const float* ghost_xz);
double text_minimap_width(uint32_t zoom, double S, double range, double speed, double prev, double dt, double scale);

/* TEXT FIELD (interface 11, additive; render-core/text/text_field.hpp, CORE-TEXT-FIELD-DESIGN.md): the chat input and the console input. One implementation; the host feeds events, the engine owns the text, caret, selection, history,
   Tab completion, the open / close state machine and the drawing of the whole box (chrome, log view, prompt, field, selection, preedit, caret, hint, colour preview, close buttons). Everything is in the frame's layout pixels.
   All state is in fixed buffers (no allocation). Key codes: `key` = TF_KEY_* in the low 8 bits, the host's own physical-key id (any non-zero number, 0 = unknown) above them: (id << 8) | TF_KEY_*. */
enum { TF_NONE = 0, TF_CHAT = 1, TF_TEAM = 2, TF_CONSOLE = 3 };           /* modes */
enum { TF_STATE_CHAT = 1, TF_STATE_TEAM = 2, TF_STATE_CONSOLE = 4, TF_STATE_COMPOSING = 8, TF_STATE_ESCAPE_HELD = 16, TF_STATE_LOG_FOCUS = 32 };
enum { TF_R_CONSUMED = 1, TF_R_EVENTS = 2, TF_R_DIRTY = 4 };              /* result bits of the key / char / commit / paste / preedit calls */
enum { TF_P_INSIDE = 1, TF_P_CHANGED = 2, TF_P_FOLD = 4, TF_P_TICK = 8, TF_P_CONSUMED = 16, TF_P_EVENTS = 32 };   /* result bits of text_tf_pointer: the first four are text_lv_pointer's */
enum { TF_MOD_SHIFT = 1, TF_MOD_CTRL = 2, TF_MOD_ALT = 4, TF_MOD_META = 8, TF_MOD_REPEAT = 16 };
enum { TF_KEY_ENTER = 1, TF_KEY_KP_ENTER, TF_KEY_TAB, TF_KEY_ESCAPE, TF_KEY_BACKSPACE, TF_KEY_DELETE, TF_KEY_LEFT, TF_KEY_RIGHT, TF_KEY_UP, TF_KEY_DOWN,
       TF_KEY_HOME, TF_KEY_END, TF_KEY_PAGE_UP, TF_KEY_PAGE_DOWN, TF_KEY_A, TF_KEY_C, TF_KEY_V, TF_KEY_X, TF_KEY_INSERT, TF_KEY_BACKQUOTE /* the physical key left of 1: the console toggle */, TF_KEY_F11, TF_KEY_OTHER };
void     text_tf_config(double x, double y, uint32_t w, uint32_t h, double font_px, double con_height, double con_opacity, double con_scale);   /* on change only: the chat box (x, y = the bottom-left corner as a fraction of the framebuffer, < 0 = the default 36 px / 90 px up; w, h px, 0 = 640 x 440; font_px 0 = 16) and the console cvars (con_height, con_opacity, con_scale) */
uint32_t text_tf_vocab(uint32_t kind /* 0 slash commands + cvar names, 1 bang names, 2 !race options as "name axis" */, const uint8_t* utf8_lines, uint32_t len);   /* '\n'-separated names (no "/" or "!"), whenever the registry changes; returns the names kept */
void*    text_tf_stage_addr(void);                             /* a staging area of text_tf_stage_cap() bytes a host (wasm) writes a vocabulary into before text_tf_vocab: the engine's own memory, so a list over the host's 4 KB string scratch fits */
uint32_t text_tf_stage_ptr(void);
uint32_t text_tf_stage_cap(void);
uint32_t text_tf_complete(const uint8_t* utf8, uint32_t len);          /* the Tab completion of `utf8` over the vocabulary, without touching the field: 0 = nothing to complete, else the byte size of the result block at text_tf_complete_addr(): u32 text length, u32 match count, u32 match bytes, the text, then every match NUL-terminated */
void*    text_tf_complete_addr(void);
uint32_t text_tf_complete_ptr(void);
/* ---- the command registry (render-core/text/command_registry.hpp; additive, interface version unchanged) ----
   Names, aliases, cvar table (setting key, value rule, range, default), bind commands, help text, completion vocabulary and the parser for typed lines live here. The Tab completion uses the registry's vocabulary
   unless a host pushed one with text_tf_vocab (older hosts); a host that wants the page-owned commands (dev, desktop) listed calls text_cmd_call(TEXT_CMD_CONTEXT, ...) once at start.
   One call: the input fields, separated by US (0x1f), at most text_cmd_in_cap() bytes; call text_cmd_call(op, bytes, length), read the UTF-8 result at text_cmd_out_addr() (length returned, 0 = none / overflow).
   Results are records separated by RS (0x1e), fields by US; the formats per op are in research/COMMAND-REGISTRY-CORE.md. The registry owns no state: a typed line is run against the state the host passes in (cvar values, binds). */
enum { TEXT_CMD_REGISTRY = 0 /* the static tables as JSON */, TEXT_CMD_RUN = 1 /* mode 'l' chat line / 'c' console line, text, state -> records */, TEXT_CMD_CVAR = 2 /* name, value -> the settings to apply */, TEXT_CMD_BIND = 3 /* text, 'l' lenient -> parsed bind */,
       TEXT_CMD_QLKEY = 4, TEXT_CMD_KEYNAME = 5, TEXT_CMD_TOKENIZE = 6, TEXT_CMD_MAP = 7 /* resolveCommandMap: query, ids '\n'-joined */, TEXT_CMD_VOCAB = 8 /* kind '0' slash+cvars, '1' bang, '2' race options */, TEXT_CMD_CONTEXT = 9 /* flags in decimal: 1 dev server, 2 desktop client, 4 compositor-sync experiment, 8 desktop fullscreen API */,
       TEXT_CMD_HELP = 10, TEXT_CMD_SHARES = 11 /* is this typed line a result command shared with the room */, TEXT_CMD_ISCVAR = 12, TEXT_CMD_CVARS = 13 /* the cvar names on offer, '\n'-joined */, TEXT_CMD_RESERVED = 14 /* key code -> "<key> is reserved for ..." or empty */ };
uint32_t text_cmd_call(uint32_t op, const uint8_t* in, uint32_t in_len);   /* in = your bytes (copied) or text_cmd_in_addr() itself (a wasm host writes there and passes its address) */
void*    text_cmd_in_addr(void);
uint32_t text_cmd_in_ptr(void);
uint32_t text_cmd_in_cap(void);
void*    text_cmd_out_addr(void);
uint32_t text_cmd_out_ptr(void);
uint32_t text_tf_open(uint32_t mode, uint32_t suppress_key);   /* the host decided it may open (the guard stays the page's). Chat / team chat are refused while anything is open; the console closes an open chat without sending (event CLOSED, reason 5). suppress_key = the host's id of the key that opened it: chars and commits are dropped until its key-up. Returns the state bits */
uint32_t text_tf_close(uint32_t reason);                       /* host-side close (menu opened, map load, window lost focus): event CLOSED, nothing sent. reason 0 = 6 (host) */
uint32_t text_tf_state(void);                                  /* TF_STATE_* bits, O(1), allocation free: the page's chatOpen / pause / input-suppression gates read this */
uint32_t text_tf_key(uint32_t key, uint32_t mods, uint32_t down);   /* returns TF_R_*. While open everything is CONSUMED (the host stops it) except TF_KEY_F11 and key-ups the core did not take; closed, only TF_KEY_BACKQUOTE (the console toggle), Escape while it is held, and their key-ups matter. Ctrl+V / Shift+Insert queue TF_EV_PASTE_REQUEST, Ctrl+C / X queue TF_EV_CLIPBOARD_SET */
uint32_t text_tf_char(uint32_t codepoint);                     /* one committed character from the OS text-input path (control characters and surrogates are ignored); inserted at the caret, replacing the selection, cut at the UTF-16 limit (chat 150, console 300) */
uint32_t text_tf_commit(const uint8_t* utf8, uint32_t len);    /* committed text of an IME (ends the composition): inserted like text_tf_char, control characters dropped */
uint32_t text_tf_paste(const uint8_t* utf8, uint32_t len);     /* like commit, but control characters / line breaks become one space each (CRLF one) */
uint32_t text_tf_preedit(const uint8_t* utf8, uint32_t len, uint32_t cursor_from, uint32_t cursor_to, uint32_t underline_from, uint32_t underline_to);   /* the IME composition in progress, shown at the caret and not in the text (len 0 ends it); the four offsets are BYTE offsets into utf8 (underline_from == underline_to: the whole preedit is underlined) */
uint32_t text_tf_pointer(uint32_t event, float x, float y, uint32_t mods, uint32_t buttons);   /* event 0 down, 1 move, 2 up, 3 double click, 4 triple click (the text_lv_pointer ids); mods bit 0 shift; the field (caret, drag select, word, all), the log and the close buttons. Returns TF_P_* */
uint32_t text_tf_wheel(float x, float y, float delta_px);       /* scrolls the log when the pointer is over the box (delta_px > 0 = towards older lines); 1 = consumed */
uint32_t text_tf_tick(double dt_ms);                           /* drag auto-scroll of the log; call only while text_tf_state() is non-zero; 1 = it scrolled */
void     text_tf_caret_rect(float* out4);                      /* x, y, w, h of the caret (the preedit's caret while composing) in layout px, valid after the last text_tf_draw: where the host puts the IME candidate window */
typedef struct TextFieldEvent { uint32_t type, a, b, len; } TextFieldEvent;   /* 16 bytes */
enum { TF_EV_OPENED = 1 /* a = mode */, TF_EV_CLOSED = 2 /* a = mode, b = reason: 1 submit 2 escape 3 toggle key 4 close button 5 superseded 6 host */, TF_EV_SUBMIT = 3 /* a = mode, the bytes = the line, UTF-8, NOT trimmed */,
       TF_EV_CLIPBOARD_SET = 4 /* the bytes = the text to put on the clipboard */, TF_EV_PASTE_REQUEST = 5 /* the host reads its clipboard and calls text_tf_paste */, TF_EV_ESCAPE_RELEASED = 6 /* the Escape that closed the box was released: the host may re-grab the mouse */, TF_EV_FONT_WANTED = 7 /* a = 1: the console's monospace face is needed (first open only) */ };
uint32_t text_tf_poll(TextFieldEvent* out);                    /* pops the next event: 1, or 0 when empty; its bytes (len of them) are at text_tf_event_bytes() until the next input call or poll that queues an event */
const uint8_t* text_tf_event_bytes(void);
uint32_t text_tf_draw(uint32_t fb_width, uint32_t fb_height, double now_ms);   /* the whole box on layer 3 in the order drawLogView + the input box did (draw list byte-equal to hud-log.mjs); state 0 returns 0 at once; call where text_lv_draw is called today (between text_hud_el_frame_part 0 and 1); 1 = a glyph was over budget. The caret blinks from now_ms */
uint32_t text_tf_preview(uint32_t mode /* 0 off, 1 chat, 2 team chat */, const uint8_t* utf8, uint32_t len);   /* the HUD editor's chat box preview: while the field is CLOSED, text_tf_draw draws the chat box (chrome, log view 0 with whatever the host appended, the mode label, `utf8` as the sample text in the field, no caret movement); 0 = a real field is open (refused) or a bad mode; mode 0 ends it. Any open() takes over; nothing is stored beyond the sample text. */
uint32_t text_tf_info(uint32_t what);                          /* 0 text length in bytes, 1 caret byte, 2 anchor byte (selection = anchor .. caret), 3 hint length, 4 preedit length, 5 history entries, 6 text length in UTF-16 units, 7 the length limit in force */
const uint8_t* text_tf_bytes(uint32_t what);                   /* 0 the text, 1 the Tab hint, 2 the preedit (lengths from text_tf_info) */

/* wasm host only: the font store the host copies fonts into, and its size (a native host reads the files itself and passes any pointer to text_add_font) */
uint8_t* text_font_store(void);
uint32_t text_font_store_size(void);
#ifdef __cplusplus
}
#endif
#endif
