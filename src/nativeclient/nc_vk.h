// Native client: Vulkan context (instance, device, swapchain, small resource helpers). No game knowledge here.
#pragma once
#include "nc_platform.h"
#define VK_NO_PROTOTYPES
#if defined(NC_WINDOWS)
#define VK_USE_PLATFORM_WIN32_KHR
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <vulkan/vulkan.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#define NC_VK_FUNCS(X) \
  X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
  X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFeatures) X(vkGetPhysicalDeviceSurfaceSupportKHR) \
  X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR) X(vkDestroySurfaceKHR) \
  X(vkCreateDevice) X(vkGetDeviceQueue) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) \
  X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) \
  X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateSampler) X(vkCreateShaderModule) \
  X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkCreateDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateRenderPass) \
  X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateGraphicsPipelines) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) X(vkBeginCommandBuffer) \
  X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdBlitImage) X(vkCmdCopyImageToBuffer) X(vkCmdBeginRenderPass) \
  X(vkCmdEndRenderPass) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed) X(vkCmdDraw) \
  X(vkCmdPushConstants) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetDepthBias) X(vkCmdSetBlendConstants) X(vkCmdClearAttachments) X(vkCreateFence) X(vkWaitForFences) X(vkResetFences) \
  X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkDestroyDevice) X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkDeviceWaitIdle) X(vkCreateQueryPool) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults)
#define NC_DECL(n) extern PFN_##n n;
NC_VK_FUNCS(NC_DECL)
#undef NC_DECL

namespace nc {

#define NC_CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { nc::fatal("%s failed: %d (%s:%d)", #x, (int)r_, __FILE__, __LINE__); } } while (0)
extern bool g_throwOnFatal;   // embedded in another process: a fatal error throws std::runtime_error instead of ending that process
[[noreturn]] void fatal(const char* fmt, ...);
void setLogFile(const std::string& path);   // opens (truncates) the file the log is also written to, unless one is open
void logf(const char* fmt, ...);   // stdout + the log file when one is open

struct Buf { VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; uint8_t* ptr = nullptr; size_t size = 0; };
struct Image { VkImage img = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; int w = 0, h = 0; VkFormat fmt = VK_FORMAT_UNDEFINED; };

struct Context {
  VkInstance inst = VK_NULL_HANDLE; VkPhysicalDevice pd = VK_NULL_HANDLE; VkDevice dev = VK_NULL_HANDLE; VkQueue queue = VK_NULL_HANDLE; uint32_t qfam = 0;
  VkSurfaceKHR surf = VK_NULL_HANDLE; VkPhysicalDeviceProperties props = {}; VkPhysicalDeviceMemoryProperties mem = {}; VkCommandPool pool = VK_NULL_HANDLE;
  float timestampPeriod = 1.0f;   // ns per tick, for GPU timing
#if defined(NC_WINDOWS)
  PFN_vkGetPhysicalDeviceSurfacePresentModes2EXT windowedPresentModes = nullptr;
#endif
};

// Loads vulkan-1 and the global entry points. false = no Vulkan runtime.
bool loadLoader();
// Instance + surface for the native window, then the best GPU (discrete first) that can present to it.
bool createContext(Context& c, void* nativeWindow, void* nativeInstance);

Buf makeBuf(const Context& c, size_t size, VkBufferUsageFlags usage);   // host visible + coherent, persistently mapped
Image makeImage(const Context& c, int w, int h, VkFormat fmt, VkImageUsageFlags usage, uint32_t mips, VkImageAspectFlags aspect);
void imageBarrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds,
                  uint32_t baseMip = 0, uint32_t mips = 1, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);
template <class F> void oneShot(const Context& c, F fn) {
  VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ai.commandPool = c.pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
  VkCommandBuffer cb; NC_CK(vkAllocateCommandBuffers(c.dev, &ai, &cb));
  VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; NC_CK(vkBeginCommandBuffer(cb, &bi)); fn(cb); NC_CK(vkEndCommandBuffer(cb));
  VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cb; NC_CK(vkQueueSubmit(c.queue, 1, &si, VK_NULL_HANDLE)); NC_CK(vkQueueWaitIdle(c.queue));
  vkFreeCommandBuffers(c.dev, c.pool, 1, &cb);
}

enum class PresentMode { Immediate, Mailbox, Fifo };

// Swapchain with a depth image and one framebuffer per image for `renderPass` (colour attachment 0 = the swapchain image, 1 = depth).
struct Swapchain {
  VkSwapchainKHR sc = VK_NULL_HANDLE; VkFormat format = VK_FORMAT_B8G8R8A8_UNORM; int w = 0, h = 0; PresentMode requested = PresentMode::Immediate; PresentMode actual = PresentMode::Fifo;
  std::vector<VkImage> images; std::vector<VkImageView> views; std::vector<VkFramebuffer> fbs; std::vector<VkSemaphore> done; Image depth;
};
// (Re)creates the swapchain for the surface at w x h. The old chain is retired. renderPass may be VK_NULL_HANDLE (framebuffers are then built later by the caller from `views`).
void buildSwapchain(const Context& c, Swapchain& s, int w, int h, PresentMode pm, VkRenderPass renderPass);
// Shutdown, in this order after the device is idle: the swapchain's own objects, then the surface, the device (which frees everything made on it) and the instance. Without it the window stays owned by the old swapchain and the next start on the same window fails.
void destroySwapchain(const Context& c, Swapchain& s);
void destroyContext(Context& c);
VkRenderPass makeScreenRenderPass(const Context& c, VkFormat colour, bool clearColour);

}  // namespace nc
