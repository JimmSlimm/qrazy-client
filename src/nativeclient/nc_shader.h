// In-process GLSL (Vulkan 450, as the render core writes it) to SPIR-V. glslang, no optimizer.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nc {
enum class ShaderStage { Vertex, Fragment };
void shaderInit();       // once, before any compile
void shaderShutdown();
// false = compile or link failure; `log` has glslang's message. Thread-safe after shaderInit.
bool compileGlsl(ShaderStage stage, const std::string& text, std::vector<uint32_t>& spirv, std::string& log);
}
