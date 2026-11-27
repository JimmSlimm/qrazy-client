#include "nc_shader.h"
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>

namespace nc {

void shaderInit() { glslang::InitializeProcess(); }
void shaderShutdown() { glslang::FinalizeProcess(); }

bool compileGlsl(ShaderStage stage, const std::string& text, std::vector<uint32_t>& spirv, std::string& log) {
  const EShLanguage lang = stage == ShaderStage::Vertex ? EShLangVertex : EShLangFragment;
  glslang::TShader sh(lang); const char* src = text.c_str(); sh.setStrings(&src, 1);
  sh.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);   // the core's text is #version 450 with explicit locations
  sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
  sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
  const EShMessages msgs = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
  if (!sh.parse(GetDefaultResources(), 450, false, msgs)) { log = std::string(sh.getInfoLog()) + sh.getInfoDebugLog(); return false; }
  glslang::TProgram prog; prog.addShader(&sh);
  if (!prog.link(msgs)) { log = std::string(prog.getInfoLog()) + prog.getInfoDebugLog(); return false; }
  glslang::SpvOptions opt; opt.disableOptimizer = true; opt.validate = false;
  glslang::GlslangToSpv(*prog.getIntermediate(lang), spirv, &opt);
  return !spirv.empty();
}

}  // namespace nc
