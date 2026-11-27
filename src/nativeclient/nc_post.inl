// Included by nc_world.cpp: r_renderScale, r_gamma and r_contrast. The frame is drawn into an offscreen image (scaled), then one fullscreen triangle puts it on the screen
// (bilinear, as the browser's canvas upscale; gamma then contrast as the page's CSS filter). At the default values the client never calls this: no image, no pass.

static const char* const POST_VS = "#version 450\nlayout(location=0) out vec2 uv;\nvoid main(){uv=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);gl_Position=vec4(uv*2.0-1.0,0.0,1.0);}\n";
static const char* const POST_FS = "#version 450\nlayout(location=0) in vec2 uv;layout(location=0) out vec4 o;layout(set=0,binding=0) uniform sampler2D src;layout(push_constant) uniform P{float exponent;float contrast;}p;\n"
  "void main(){vec3 c=texture(src,uv).rgb;if(p.exponent!=1.0||p.contrast!=1.0){c=pow(c,vec3(p.exponent));c=clamp(c*p.contrast+0.5-0.5*p.contrast,0.0,1.0);}o=vec4(c,1.0);}\n";

static void postFree(const Context& c, Image& im) { if (im.view) vkDestroyImageView(c.dev, im.view, nullptr); if (im.img) vkDestroyImage(c.dev, im.img, nullptr); if (im.mem) vkFreeMemory(c.dev, im.mem, nullptr); im = Image(); }

void PostPass::resize(const Context& c, VkRenderPass screenPass, int nw, int nh) {
  if (!pipe) {
    std::vector<uint32_t> sv, sf; std::string log; if (!compileGlsl(ShaderStage::Vertex, POST_VS, sv, log) || !compileGlsl(ShaderStage::Fragment, POST_FS, sf, log)) fatal("post pass shader: %s", log.c_str());
    VkShaderModule vs, fs; VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; ci.codeSize = sv.size() * 4; ci.pCode = sv.data(); NC_CK(vkCreateShaderModule(c.dev, &ci, nullptr, &vs)); ci.codeSize = sf.size() * 4; ci.pCode = sf.data(); NC_CK(vkCreateShaderModule(c.dev, &ci, nullptr, &fs));
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; si.magFilter = si.minFilter = VK_FILTER_LINEAR; si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; NC_CK(vkCreateSampler(c.dev, &si, nullptr, &sampler));
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}; VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount = 1; li.pBindings = &b; VkDescriptorSetLayout sl; NC_CK(vkCreateDescriptorSetLayout(c.dev, &li, nullptr, &sl));
    VkPushConstantRange pc = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8}; VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount = 1; pl.pSetLayouts = &sl; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pc; NC_CK(vkCreatePipelineLayout(c.dev, &pl, nullptr, &layout));
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}; VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pi.maxSets = 1; pi.poolSizeCount = 1; pi.pPoolSizes = &ps; VkDescriptorPool pool; NC_CK(vkCreateDescriptorPool(c.dev, &pi, nullptr, &pool));
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ai.descriptorPool = pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &sl; NC_CK(vkAllocateDescriptorSets(c.dev, &ai, &set));
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}}; st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main"; st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO}; VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount = 1; vp.scissorCount = 1; VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT; VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState ba = {}; ba.colorWriteMask = 15; VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount = 1; cb.pAttachments = &ba;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR}; VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; gi.stageCount = 2; gi.pStages = st; gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia; gi.pViewportState = &vp; gi.pRasterizationState = &rs; gi.pMultisampleState = &ms; gi.pDepthStencilState = &ds; gi.pColorBlendState = &cb; gi.pDynamicState = &dy; gi.layout = layout; gi.renderPass = screenPass;
    NC_CK(vkCreateGraphicsPipelines(c.dev, VK_NULL_HANDLE, 1, &gi, nullptr, &pipe));
  }
  NC_CK(vkDeviceWaitIdle(c.dev)); if (fb) vkDestroyFramebuffer(c.dev, fb, nullptr); postFree(c, color); postFree(c, depth); w = nw; h = nh;
  color = makeImage(c, w, h, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1, VK_IMAGE_ASPECT_COLOR_BIT);
  depth = makeImage(c, w, h, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 1, VK_IMAGE_ASPECT_DEPTH_BIT);
  VkImageView v[2] = {color.view, depth.view}; VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fi.renderPass = screenPass; fi.attachmentCount = 2; fi.pAttachments = v; fi.width = (uint32_t)w; fi.height = (uint32_t)h; fi.layers = 1; NC_CK(vkCreateFramebuffer(c.dev, &fi, nullptr, &fb));
  VkDescriptorImageInfo ii = {sampler, color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}; VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; wr.dstSet = set; wr.descriptorCount = 1; wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &ii; vkUpdateDescriptorSets(c.dev, 1, &wr, 0, nullptr);
  logf("post pass: scene %d x %d", w, h);
}

void PostPass::record(VkCommandBuffer cb, VkRenderPass screenPass, VkFramebuffer target, int sw, int sh, double gamma, double contrast,
                      const std::function<void(VkCommandBuffer)>& overlay) {
  imageBarrier(cb, color.img, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  VkClearValue cv[2] = {}; cv[1].depthStencil = {1.0f, 0}; VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; rb.renderPass = screenPass; rb.framebuffer = target; rb.renderArea = {{0, 0}, {(uint32_t)sw, (uint32_t)sh}}; rb.clearValueCount = 2; rb.pClearValues = cv; vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
  VkViewport vp = {0, 0, (float)sw, (float)sh, 0, 1}; VkRect2D sc = {{0, 0}, {(uint32_t)sw, (uint32_t)sh}}; vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
  const float p[2] = {(float)(gamma > 0 ? 1.0 / gamma : 1.0), (float)contrast}; vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
  vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof p, p); vkCmdDraw(cb, 3, 1, 0, 0);
  if (overlay) overlay(cb);
  vkCmdEndRenderPass(cb);
}
