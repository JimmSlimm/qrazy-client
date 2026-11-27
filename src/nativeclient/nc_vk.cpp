#include "nc_vk.h"
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <stdexcept>
#if !defined(NC_WINDOWS)
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>   // Linux: SDL owns the window and creates the surface (Wayland); no platform surface code here
#endif

#define NC_DEF(n) PFN_##n n;
NC_VK_FUNCS(NC_DEF)
#undef NC_DEF

namespace nc {

static FILE* g_log = nullptr;
void setLogFile(const std::string& path) { if (!g_log && !path.empty()) g_log = fopen(path.c_str(), "w"); }   // the host's own log file (the QRAZY_NC_LOG variable still wins when set)
void logf(const char* fmt, ...) {
  va_list a; va_start(a, fmt); char buf[1024]; vsnprintf(buf, sizeof buf, fmt, a); va_end(a);
  fputs(buf, stdout); fputc('\n', stdout); fflush(stdout);
  if (!g_log) { const char* p = getenv("QRAZY_NC_LOG"); if (p && *p) g_log = fopen(p, "w"); }
  if (g_log) { fputs(buf, g_log); fputc('\n', g_log); fflush(g_log); }
}
bool g_throwOnFatal = false;
void fatal(const char* fmt, ...) {
  va_list a; va_start(a, fmt); char buf[1024]; vsnprintf(buf, sizeof buf, fmt, a); va_end(a);
  logf("FATAL: %s", buf);
  if (g_throwOnFatal) throw std::runtime_error(buf);
  exit(1);
}

static PFN_vkGetInstanceProcAddr pGIPA = nullptr;

bool loadLoader() {
#if defined(NC_WINDOWS)
  HMODULE lib = LoadLibraryA("vulkan-1.dll"); if (!lib) return false;
  pGIPA = (PFN_vkGetInstanceProcAddr)GetProcAddress(lib, "vkGetInstanceProcAddr");
#else
  if (!SDL_Vulkan_LoadLibrary(nullptr)) return false;
  pGIPA = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
#endif
  if (!pGIPA) return false;
  vkCreateInstance = (PFN_vkCreateInstance)pGIPA(nullptr, "vkCreateInstance");
  vkEnumerateInstanceExtensionProperties = (PFN_vkEnumerateInstanceExtensionProperties)pGIPA(nullptr, "vkEnumerateInstanceExtensionProperties");
  return vkCreateInstance != nullptr;
}

#if defined(NC_WINDOWS)
static bool hasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
  return std::any_of(extensions.begin(), extensions.end(), [name](const VkExtensionProperties& ext) {
    return std::strcmp(ext.extensionName, name) == 0;
  });
}
#endif

bool createContext(Context& c, void* nativeWindow, void* nativeInstance) {
#if defined(NC_WINDOWS)
  const char* iext[] = {"VK_KHR_surface", "VK_KHR_win32_surface",
    VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME};
  uint32_t extensionCount = 0;
  NC_CK(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr));
  std::vector<VkExtensionProperties> instanceExtensions(extensionCount);
  NC_CK(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, instanceExtensions.data()));
  instanceExtensions.resize(extensionCount);
  const bool fullscreenPolicyAvailable = hasExtension(instanceExtensions, iext[2]) && hasExtension(instanceExtensions, iext[3]);
  const uint32_t iextCount = fullscreenPolicyAvailable ? 4 : 2;
#else
  Uint32 sdlCount = 0; const char* const* sdlExt = SDL_Vulkan_GetInstanceExtensions(&sdlCount); if (!sdlExt) return false;   // the window system's surface extensions, as SDL knows them
  const char* const* iext = sdlExt; const uint32_t iextCount = sdlCount;
#endif
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_0; app.pApplicationName = "qrazy-native";
  VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app; ici.enabledExtensionCount = iextCount; ici.ppEnabledExtensionNames = iext;
  { const VkResult r = vkCreateInstance(&ici, nullptr, &c.inst);
    if (r == VK_ERROR_INCOMPATIBLE_DRIVER) fatal("No Vulkan graphics driver was found on this computer.");
    if (r != VK_SUCCESS) fatal("Vulkan could not start (error %d).", (int)r); }
#define NC_LOADI(n) if (!n) n = (PFN_##n)pGIPA(c.inst, #n);
  NC_VK_FUNCS(NC_LOADI)
#undef NC_LOADI
#if defined(NC_WINDOWS)
  { auto create = (PFN_vkCreateWin32SurfaceKHR)pGIPA(c.inst, "vkCreateWin32SurfaceKHR");
    VkWin32SurfaceCreateInfoKHR sci = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR}; sci.hinstance = (HINSTANCE)nativeInstance; sci.hwnd = (HWND)nativeWindow; NC_CK(create(c.inst, &sci, nullptr, &c.surf)); }
#else
  (void)nativeInstance; if (!SDL_Vulkan_CreateSurface((SDL_Window*)nativeWindow, c.inst, nullptr, &c.surf)) { logf("SDL_Vulkan_CreateSurface: %s", SDL_GetError()); return false; }
#endif
  uint32_t n = 0; vkEnumeratePhysicalDevices(c.inst, &n, nullptr); std::vector<VkPhysicalDevice> pds(n); vkEnumeratePhysicalDevices(c.inst, &n, pds.data());
  const char* want = getenv("QRAZY_NC_GPU");   // substring of the device name, for choosing between the iGPU and the dGPU
  for (int pass = 0; pass < 2 && !c.pd; ++pass) for (auto d : pds) {
    VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(d, &p);
    if (want && *want) { if (!strstr(p.deviceName, want)) continue; } else if (pass == 0 && p.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) continue;
    uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr); std::vector<VkQueueFamilyProperties> qp(qn); vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qp.data());
    for (uint32_t i = 0; i < qn; ++i) {
      VkBool32 ok = VK_FALSE; vkGetPhysicalDeviceSurfaceSupportKHR(d, i, c.surf, &ok);
      if (ok && (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { c.pd = d; c.qfam = i; c.props = p; break; }
    }
    if (c.pd) break;
  }
  if (!c.pd) return false;
  logf("GPU: %s (Vulkan %u.%u.%u)", c.props.deviceName, VK_VERSION_MAJOR(c.props.apiVersion), VK_VERSION_MINOR(c.props.apiVersion), VK_VERSION_PATCH(c.props.apiVersion));
  c.timestampPeriod = c.props.limits.timestampPeriod;
  vkGetPhysicalDeviceMemoryProperties(c.pd, &c.mem);
  VkPhysicalDeviceFeatures have = {}; vkGetPhysicalDeviceFeatures(c.pd, &have);
  VkPhysicalDeviceFeatures use = {}; use.samplerAnisotropy = have.samplerAnisotropy; use.fillModeNonSolid = have.fillModeNonSolid;
  float prio = 1; VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qci.queueFamilyIndex = c.qfam; qci.queueCount = 1; qci.pQueuePriorities = &prio;
  const char* dext[] = {"VK_KHR_swapchain"
#if defined(NC_WINDOWS)
    , VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME
#endif
  };
  VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = dext; dci.pEnabledFeatures = &use;
#if defined(NC_WINDOWS)
  if (fullscreenPolicyAvailable) {
    const auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(pGIPA(c.inst, "vkEnumerateDeviceExtensionProperties"));
    extensionCount = 0;
    NC_CK(enumerate(c.pd, nullptr, &extensionCount, nullptr));
    std::vector<VkExtensionProperties> deviceExtensions(extensionCount);
    NC_CK(enumerate(c.pd, nullptr, &extensionCount, deviceExtensions.data()));
    deviceExtensions.resize(extensionCount);
    if (hasExtension(deviceExtensions, dext[1])) dci.enabledExtensionCount = 2;
  }
#endif
  NC_CK(vkCreateDevice(c.pd, &dci, nullptr, &c.dev)); vkGetDeviceQueue(c.dev, c.qfam, 0, &c.queue);
#if defined(NC_WINDOWS)
  if (dci.enabledExtensionCount == 2) {
    c.windowedPresentModes = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfacePresentModes2EXT>(
      pGIPA(c.inst, "vkGetPhysicalDeviceSurfacePresentModes2EXT"));
  }
  logf("Vulkan fullscreen policy: %s", c.windowedPresentModes ? "available" : "driver extension unavailable");
#endif
  VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = c.qfam;
  NC_CK(vkCreateCommandPool(c.dev, &pci, nullptr, &c.pool));
  return true;
}

static uint32_t memType(const Context& c, uint32_t bits, VkMemoryPropertyFlags want) {
  for (uint32_t i = 0; i < c.mem.memoryTypeCount; ++i) if ((bits & (1u << i)) && (c.mem.memoryTypes[i].propertyFlags & want) == want) return i;
  fatal("no memory type for flags 0x%x", (unsigned)want);
}

Buf makeBuf(const Context& c, size_t size, VkBufferUsageFlags usage) {
  Buf b; b.size = size; VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; ci.size = size; ci.usage = usage; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; NC_CK(vkCreateBuffer(c.dev, &ci, nullptr, &b.buf));
  VkMemoryRequirements mr; vkGetBufferMemoryRequirements(c.dev, b.buf, &mr);
  VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = mr.size; ai.memoryTypeIndex = memType(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  NC_CK(vkAllocateMemory(c.dev, &ai, nullptr, &b.mem)); NC_CK(vkBindBufferMemory(c.dev, b.buf, b.mem, 0)); NC_CK(vkMapMemory(c.dev, b.mem, 0, VK_WHOLE_SIZE, 0, (void**)&b.ptr));
  return b;
}

Image makeImage(const Context& c, int w, int h, VkFormat fmt, VkImageUsageFlags usage, uint32_t mips, VkImageAspectFlags aspect) {
  Image im; im.w = w; im.h = h; im.fmt = fmt;
  VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt; ci.extent = {(uint32_t)w, (uint32_t)h, 1}; ci.mipLevels = mips; ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = usage; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; NC_CK(vkCreateImage(c.dev, &ci, nullptr, &im.img));
  VkMemoryRequirements mr; vkGetImageMemoryRequirements(c.dev, im.img, &mr);
  VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = mr.size; ai.memoryTypeIndex = memType(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  NC_CK(vkAllocateMemory(c.dev, &ai, nullptr, &im.mem)); NC_CK(vkBindImageMemory(c.dev, im.img, im.mem, 0));
  VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = im.img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt; vi.subresourceRange = {aspect, 0, mips, 0, 1};
  NC_CK(vkCreateImageView(c.dev, &vi, nullptr, &im.view));
  return im;
}

void imageBarrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds,
                  uint32_t baseMip, uint32_t mips, VkImageAspectFlags aspect) {
  VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.srcAccessMask = sa; b.dstAccessMask = da; b.oldLayout = from; b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = img; b.subresourceRange = {aspect, baseMip, mips, 0, 1};
  vkCmdPipelineBarrier(cb, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
}

VkRenderPass makeScreenRenderPass(const Context& c, VkFormat colour, bool clearColour) {
  VkAttachmentDescription at[2] = {};
  at[0].format = colour; at[0].samples = VK_SAMPLE_COUNT_1_BIT; at[0].loadOp = clearColour ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  at[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; at[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; at[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  at[1].format = VK_FORMAT_D32_SFLOAT; at[1].samples = VK_SAMPLE_COUNT_1_BIT; at[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; at[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  at[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; at[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; at[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  VkAttachmentReference cr = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, dr = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sp = {}; sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount = 1; sp.pColorAttachments = &cr; sp.pDepthStencilAttachment = &dr;
  VkSubpassDependency dep = {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0,
                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 0};
  VkRenderPassCreateInfo rpi = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; rpi.attachmentCount = 2; rpi.pAttachments = at; rpi.subpassCount = 1; rpi.pSubpasses = &sp; rpi.dependencyCount = 1; rpi.pDependencies = &dep;
  VkRenderPass rp; NC_CK(vkCreateRenderPass(c.dev, &rpi, nullptr, &rp)); return rp;
}

void destroySwapchain(const Context& c, Swapchain& s) {
  for (VkFramebuffer f : s.fbs) if (f) vkDestroyFramebuffer(c.dev, f, nullptr);
  for (VkImageView v : s.views) if (v) vkDestroyImageView(c.dev, v, nullptr);
  for (VkSemaphore m : s.done) if (m) vkDestroySemaphore(c.dev, m, nullptr);
  if (s.depth.view) vkDestroyImageView(c.dev, s.depth.view, nullptr); if (s.depth.img) vkDestroyImage(c.dev, s.depth.img, nullptr); if (s.depth.mem) vkFreeMemory(c.dev, s.depth.mem, nullptr);
  if (s.sc) vkDestroySwapchainKHR(c.dev, s.sc, nullptr); s = Swapchain();
}
void destroyContext(Context& c) {
  if (c.dev) vkDeviceWaitIdle(c.dev);
  if (c.surf) vkDestroySurfaceKHR(c.inst, c.surf, nullptr); c.surf = VK_NULL_HANDLE;
  if (c.dev) vkDestroyDevice(c.dev, nullptr); c.dev = VK_NULL_HANDLE;
  if (c.inst) vkDestroyInstance(c.inst, nullptr); c.inst = VK_NULL_HANDLE;
}

void buildSwapchain(const Context& c, Swapchain& s, int w, int h, PresentMode pm, VkRenderPass renderPass) {
  VkSurfaceCapabilitiesKHR caps; NC_CK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(c.pd, c.surf, &caps));
  if (caps.currentExtent.width != 0xFFFFFFFFu) { w = (int)caps.currentExtent.width; h = (int)caps.currentExtent.height; }
#if defined(NC_WINDOWS)
  // Immediate presentation can cause a disruptive display format switch when the driver takes over scanout.
  VkSurfaceFullScreenExclusiveInfoEXT fullscreen = {VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT};
  fullscreen.fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT;
  const bool windowed = c.windowedPresentModes && pm == PresentMode::Immediate;
  VkPhysicalDeviceSurfaceInfo2KHR surface = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR};
  surface.surface = c.surf;
  surface.pNext = &fullscreen;
#endif
  const auto queryPresentModes = [&](uint32_t* count, VkPresentModeKHR* modes) {
#if defined(NC_WINDOWS)
    if (windowed) return c.windowedPresentModes(c.pd, &surface, count, modes);
#endif
    return vkGetPhysicalDeviceSurfacePresentModesKHR(c.pd, c.surf, count, modes);
  };
  uint32_t pmn = 0;
  NC_CK(queryPresentModes(&pmn, nullptr));
  std::vector<VkPresentModeKHR> pms(pmn);
  NC_CK(queryPresentModes(&pmn, pms.data()));
  pms.resize(pmn);
  s.requested = pm;
  VkPresentModeKHR want = pm == PresentMode::Immediate ? VK_PRESENT_MODE_IMMEDIATE_KHR : pm == PresentMode::Mailbox ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
  if (std::find(pms.begin(), pms.end(), want) == pms.end()) want = VK_PRESENT_MODE_FIFO_KHR;
  s.actual = want == VK_PRESENT_MODE_IMMEDIATE_KHR ? PresentMode::Immediate : want == VK_PRESENT_MODE_MAILBOX_KHR ? PresentMode::Mailbox : PresentMode::Fifo;
  vkDeviceWaitIdle(c.dev);
  for (auto f : s.fbs) vkDestroyFramebuffer(c.dev, f, nullptr);
  for (auto v : s.views) vkDestroyImageView(c.dev, v, nullptr);
  s.fbs.clear(); s.views.clear();
  VkSwapchainCreateInfoKHR sc = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR}; sc.surface = c.surf; sc.minImageCount = std::max(2u, caps.minImageCount);
#if defined(NC_WINDOWS)
  if (windowed) sc.pNext = &fullscreen;
#endif
  if (caps.maxImageCount && sc.minImageCount > caps.maxImageCount) sc.minImageCount = caps.maxImageCount;
  s.format = VK_FORMAT_B8G8R8A8_UNORM; sc.imageFormat = s.format; sc.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR; sc.imageExtent = {(uint32_t)w, (uint32_t)h}; sc.imageArrayLayers = 1;
  sc.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT; sc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; sc.preTransform = caps.currentTransform;
  sc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; sc.presentMode = want; sc.clipped = VK_TRUE; sc.oldSwapchain = s.sc;
  VkSwapchainKHR ns; NC_CK(vkCreateSwapchainKHR(c.dev, &sc, nullptr, &ns)); if (s.sc) vkDestroySwapchainKHR(c.dev, s.sc, nullptr); s.sc = ns; s.w = w; s.h = h;
#if defined(NC_WINDOWS)
  if (windowed) logf("Vulkan swapchain: exclusive fullscreen disallowed, present mode %s",
                     want == VK_PRESENT_MODE_IMMEDIATE_KHR ? "Immediate" : "FIFO fallback");
#endif
  uint32_t n = 0; vkGetSwapchainImagesKHR(c.dev, s.sc, &n, nullptr); s.images.resize(n); vkGetSwapchainImagesKHR(c.dev, s.sc, &n, s.images.data());
  if (s.depth.img) { vkDestroyImageView(c.dev, s.depth.view, nullptr); vkDestroyImage(c.dev, s.depth.img, nullptr); vkFreeMemory(c.dev, s.depth.mem, nullptr); }
  s.depth = makeImage(c, w, h, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 1, VK_IMAGE_ASPECT_DEPTH_BIT);
  for (auto im : s.images) {
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = im; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = s.format; vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView v; NC_CK(vkCreateImageView(c.dev, &vi, nullptr, &v)); s.views.push_back(v);
    if (renderPass) {
      VkImageView at[2] = {v, s.depth.view}; VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fi.renderPass = renderPass; fi.attachmentCount = 2; fi.pAttachments = at; fi.width = w; fi.height = h; fi.layers = 1;
      VkFramebuffer fb; NC_CK(vkCreateFramebuffer(c.dev, &fi, nullptr, &fb)); s.fbs.push_back(fb);
    }
  }
  VkSemaphoreCreateInfo semi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; if (s.done.size() != n) { s.done.assign(n, VK_NULL_HANDLE); for (auto& d : s.done) NC_CK(vkCreateSemaphore(c.dev, &semi, nullptr, &d)); }
}

}  // namespace nc
