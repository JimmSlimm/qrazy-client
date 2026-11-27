// The pipeline state of an object draw, owned by the core: ONE fixed-size record (ObjectState) every platform reads instead of looking at three.js materials. The host writes the FACTS of a material (the doubles
// below, in a fixed order) when it creates or changes it; derive() turns them into the record: the blend as the explicit factors of a Vulkan colour-blend attachment (three.js's preset / custom blending resolved,
// WebGLState.setBlending r180 is the reference), the depth state, the cull mode of the side, polygon offset, the colour write mask, the sprite rotation and the fat-line parameters, and a stable PIPELINE KEY.
// Enum values are Vulkan's own numbers (VkBlendFactor, VkBlendOp, VkCompareOp, cull: the packet's Cull enum), so a Vulkan client uses them as they are; a GL host maps them (tables in NATIVE-CLIENT.md 8.x).
// Freestanding, doubles for every number, uint32 for enums / flags. No allocation. The world's `Material` record maps onto the same type (frame_objects.hpp worldState).
#pragma once
#include <stdint.h>

namespace rc{namespace os{

// ---- the facts, an array of doubles written by the host in this order (FACT_NAMES lists them; rc_object_state_fact_name) --------------------------------------------------------------------
enum Fact:int{
  FD_TRANSPARENT=0,FD_BLENDING,           // three.js blending constants: 0 none, 1 normal, 2 additive, 3 subtractive, 4 multiply, 5 custom
  FD_BLEND_EQ,FD_BLEND_SRC,FD_BLEND_DST,  // custom blending: three.js equation (100..104) and factor (200..214) constants
  FD_BLEND_EQ_ALPHA,FD_BLEND_SRC_ALPHA,FD_BLEND_DST_ALPHA,   // -1 = null (the colour value is used)
  FD_BLEND_R,FD_BLEND_G,FD_BLEND_B,FD_BLEND_ALPHA,   // the constant colour (linear floats, as gl.blendColor takes them) and its alpha
  FD_PREMULTIPLIED,FD_DEPTH_TEST,FD_DEPTH_WRITE,FD_DEPTH_FUNC,FD_COLOR_WRITE,   // depth func: three.js 0..7
  FD_POLY_OFFSET,FD_POLY_FACTOR,FD_POLY_UNITS,
  FD_SIDE,FD_ALLOW_TWO_PASS,FD_ALPHA_TEST, // side 0 front / 1 back / 2 double; ALLOW_TWO_PASS = material.forceSinglePass === false
  FD_SPRITE_ROTATION,
  FD_LINE_WIDTH,FD_DASH_SIZE,FD_GAP_SIZE,FD_DASH_SCALE,FD_DASH_OFFSET,FD_LINE_DASHED,FD_LINE_WORLD_UNITS,
  FD_COUNT
};
static const char* const FACT_NAMES[FD_COUNT]={"transparent","blending","blendEquation","blendSrc","blendDst","blendEquationAlpha","blendSrcAlpha","blendDstAlpha","blendColorR","blendColorG","blendColorB","blendAlpha","premultipliedAlpha","depthTest","depthWrite",
  "depthFunc","colorWrite","polygonOffset","polygonOffsetFactor","polygonOffsetUnits","side","allowTwoPass","alphaTest","spriteRotation","lineWidth","dashSize","gapSize","dashScale","dashOffset","dashed","worldUnits"};

// ---- the record --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
enum StateFlag:uint32_t{
  OS_TRANSPARENT=1,        // drawn in the sorted back-to-front pass
  OS_DEPTH_TEST=2,OS_DEPTH_WRITE=4,
  OS_POLYGON_OFFSET=8,
  OS_OPAQUE=16,            // not transparent and normal blending: the program's alpha is forced to 1 (variant fact FACT_OPAQUE)
  OS_TWO_PASS=32,          // transparent + double sided + not forceSinglePass: back faces first, then front faces (two draws of the same geometry)
  OS_BLEND_ENABLED=64,     // the colour-blend attachment is enabled; the factors below are its state
  OS_PREMULTIPLIED=128,
  OS_BLEND_CONSTANT=256,   // a factor uses the constant colour: blendColor / blendAlpha are part of the state
  OS_DASHED=512,OS_WORLD_UNITS=1024,   // fat lines
  OS_INVALID_BLEND=2048    // the material asked for a blend three.js refuses (subtractive / multiply without premultipliedAlpha): normal blending is used
};
enum Topology:uint32_t{OT_TRIANGLES=0,OT_LINES=1,OT_LINE_STRIP=2,OT_LINE_LOOP=3};   // OT_LINE_LOOP: Vulkan has no loop, the host closes the strip (repeat the first vertex)
enum Equation:uint32_t{EQ_ADD=0,EQ_SUBTRACT=1,EQ_REVERSE_SUBTRACT=2,EQ_MIN=3,EQ_MAX=4};   // VkBlendOp
// VkBlendFactor: 0 ZERO, 1 ONE, 2 SRC_COLOR, 3 ONE_MINUS_SRC_COLOR, 4 DST_COLOR, 5 ONE_MINUS_DST_COLOR, 6 SRC_ALPHA, 7 ONE_MINUS_SRC_ALPHA, 8 DST_ALPHA, 9 ONE_MINUS_DST_ALPHA, 10 CONSTANT_COLOR, 11 ONE_MINUS_CONSTANT_COLOR,
// 12 CONSTANT_ALPHA, 13 ONE_MINUS_CONSTANT_ALPHA, 14 SRC_ALPHA_SATURATE (the packet's Factor enum is the first ten)
// depth func: VkCompareOp: 0 NEVER, 1 LESS, 2 EQUAL, 3 LESS_OR_EQUAL, 4 GREATER, 5 NOT_EQUAL, 6 GREATER_OR_EQUAL, 7 ALWAYS. cull: 0 back faces culled (front faces drawn), 1 front faces culled, 2 none (the packet's Cull enum)
struct ObjectState{
  uint32_t flags,side,cull,frontFace;   // side: three.js 0 front / 1 back / 2 double; frontFace 0 = the packet's winding (see NATIVE-CLIENT.md: counter-clockwise in GL, clockwise on screen in Vulkan after the clip-space flip)
  uint32_t topology,blendMode;          // blendMode: the three.js blending constant that was asked (info)
  uint32_t blendEq,blendEqAlpha,blendSrc,blendDst,blendSrcAlpha,blendDstAlpha;   // resolved: what the attachment uses (valid with OS_BLEND_ENABLED)
  uint32_t depthFunc,colorMask,pipelineKey,renderOrder;   // colorMask: VkColorComponentFlags (0xF all, 0 none); renderOrder: the node's, filled in the frame list
  uint32_t reserved[4];
  double polygonFactor,polygonUnits;    // depth bias: slope factor / constant units (dynamic state recommended)
  double blendAlpha,alphaTest,spriteRotation;
  double blendColor[3];                 // the constant colour, with OS_BLEND_CONSTANT (blend constants are dynamic state in Vulkan)
  double lineWidth,dashSize,gapSize,dashScale,dashOffset;   // fat lines (LineMaterial)
};
static_assert(sizeof(ObjectState)==16*4+16+13*8,"ObjectState");   // 184 bytes: u32 at 0..63, reserved 64..79, doubles from 80


}}  // namespace rc::os
