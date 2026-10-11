#include "vk_texture.h"

#if __has_include(<vulkan/vulkan.h>)
#define RECLINE_HAVE_VULKAN 1
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <cstring>
#include <sstream>
#include <vector>

#include <dlfcn.h>
#endif

namespace recline {

#if RECLINE_HAVE_VULKAN


// `VkFooInfo x{sType}` value-initializes the remaining fields, as intended.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

struct VkOverlayTexture::Impl {
  ~Impl() { Destroy(); }

  bool Init(uint32_t w, uint32_t h, std::string* err) {
    w_ = w;
    h_ = h;
    if (!InitInstance(err) || !InitDevice(err) || !InitResources(err)) {
      Destroy();
      return false;
    }
    return true;
  }

  bool Upload(vr::VROverlayHandle_t overlay, const uint8_t* rgba, std::string* err) {
    // Rarely called (only on visible changes): keep it simple and fully
    // synchronous. Waiting for the queue also guarantees vrclient's copy of
    // the previously set image has finished before it is rewritten.
    vkQueueWaitIdle(queue_);
    Image& img = images_[next_];
    next_ ^= 1;
    memcpy(stagingPtr_, rgba, size_t(w_) * h_ * 4);

    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &bi);
    Barrier(img, img.initialized ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w_, h_, 1};
    vkCmdCopyBufferToImage(cmd_, staging_, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &region);
    // OpenVR expects Vulkan textures in TRANSFER_SRC_OPTIMAL.
    Barrier(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    vkEndCommandBuffer(cmd_);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    if (vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) {
      *err = "vkQueueSubmit failed";
      return false;
    }
    vkQueueWaitIdle(queue_);
    img.initialized = true;

    vr::VRVulkanTextureData_t data{};
    data.m_nImage = uint64_t(img.image);
    data.m_pDevice = reinterpret_cast<VkDevice_T*>(device_);
    data.m_pPhysicalDevice = reinterpret_cast<VkPhysicalDevice_T*>(phys_);
    data.m_pInstance = reinterpret_cast<VkInstance_T*>(instance_);
    data.m_pQueue = reinterpret_cast<VkQueue_T*>(queue_);
    data.m_nQueueFamilyIndex = queueFamily_;
    data.m_nWidth = w_;
    data.m_nHeight = h_;
    data.m_nFormat = VK_FORMAT_R8G8B8A8_UNORM;
    data.m_nSampleCount = 1;
    vr::Texture_t tex{&data, vr::TextureType_Vulkan, vr::ColorSpace_Gamma};
    const vr::EVROverlayError oe = vr::VROverlay()->SetOverlayTexture(overlay, &tex);
    if (oe != vr::VROverlayError_None) {
      *err = std::string("SetOverlayTexture: ") + vr::VROverlay()->GetOverlayErrorNameFromEnum(oe);
      return false;
    }
    return true;
  }

  struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    bool initialized = false;
  };

  static std::vector<std::string> Split(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream is(s);
    std::string t;
    while (is >> t) out.push_back(t);
    return out;
  }
  static std::vector<const char*> CStrs(const std::vector<std::string>& v) {
    std::vector<const char*> out;
    for (const auto& s : v) out.push_back(s.c_str());
    return out;
  }

#define RECLINE_VK_LOAD(scope, obj, name) \
  name = reinterpret_cast<PFN_##name>(scope(obj, #name)); \
  if (!name) { *err = "missing Vulkan entry point " #name; return false; }

  bool InitInstance(std::string* err) {
    lib_ = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib_) {
      *err = "libvulkan.so.1 not available";
      return false;
    }
    vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib_, "vkGetInstanceProcAddr"));
    if (!vkGetInstanceProcAddr) {
      *err = "vkGetInstanceProcAddr not found";
      return false;
    }
    RECLINE_VK_LOAD(vkGetInstanceProcAddr, VK_NULL_HANDLE, vkCreateInstance);

    char buf[4096] = {};
    vr::VRCompositor()->GetVulkanInstanceExtensionsRequired(buf, sizeof buf);
    instExts_ = Split(buf);
    const auto exts = CStrs(instExts_);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "recline_overlay";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = uint32_t(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    if (vkCreateInstance(&ci, nullptr, &instance_) != VK_SUCCESS) {
      *err = "vkCreateInstance failed (required extensions: " + std::string(buf) + ")";
      return false;
    }
    RECLINE_VK_LOAD(vkGetInstanceProcAddr, instance_, vkDestroyInstance);
    RECLINE_VK_LOAD(vkGetInstanceProcAddr, instance_, vkEnumeratePhysicalDevices);
    RECLINE_VK_LOAD(vkGetInstanceProcAddr, instance_, vkGetPhysicalDeviceQueueFamilyProperties);
    RECLINE_VK_LOAD(vkGetInstanceProcAddr, instance_, vkGetPhysicalDeviceMemoryProperties);
    RECLINE_VK_LOAD(vkGetInstanceProcAddr, instance_, vkCreateDevice);
    RECLINE_VK_LOAD(vkGetInstanceProcAddr, instance_, vkGetDeviceProcAddr);
    return true;
  }

  bool InitDevice(std::string* err) {
    uint64_t out = 0;
    vr::VRSystem()->GetOutputDevice(&out, vr::TextureType_Vulkan,
                                    reinterpret_cast<VkInstance_T*>(instance_));
    phys_ = reinterpret_cast<VkPhysicalDevice>(out);
    if (!phys_) {
      uint32_t n = 1;
      VkPhysicalDevice first = VK_NULL_HANDLE;
      vkEnumeratePhysicalDevices(instance_, &n, &first);
      phys_ = first;
    }
    if (!phys_) {
      *err = "no Vulkan physical device";
      return false;
    }
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys_, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qf(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(phys_, &nq, qf.data());
    queueFamily_ = UINT32_MAX;
    for (uint32_t i = 0; i < nq; ++i)
      if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
        queueFamily_ = i;
        break;
      }
    if (queueFamily_ == UINT32_MAX) {
      *err = "no graphics queue";
      return false;
    }
    char buf[4096] = {};
    vr::VRCompositor()->GetVulkanDeviceExtensionsRequired(reinterpret_cast<VkPhysicalDevice_T*>(phys_),
                                                          buf, sizeof buf);
    devExts_ = Split(buf);
    const auto exts = CStrs(devExts_);
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(exts.size());
    dci.ppEnabledExtensionNames = exts.data();
    if (vkCreateDevice(phys_, &dci, nullptr, &device_) != VK_SUCCESS) {
      *err = "vkCreateDevice failed (required extensions: " + std::string(buf) + ")";
      return false;
    }
#define RECLINE_VK_DEV(name) RECLINE_VK_LOAD(vkGetDeviceProcAddr, device_, name)
    RECLINE_VK_DEV(vkDestroyDevice);
    RECLINE_VK_DEV(vkDeviceWaitIdle);
    RECLINE_VK_DEV(vkGetDeviceQueue);
    RECLINE_VK_DEV(vkCreateImage);
    RECLINE_VK_DEV(vkDestroyImage);
    RECLINE_VK_DEV(vkGetImageMemoryRequirements);
    RECLINE_VK_DEV(vkAllocateMemory);
    RECLINE_VK_DEV(vkFreeMemory);
    RECLINE_VK_DEV(vkBindImageMemory);
    RECLINE_VK_DEV(vkCreateBuffer);
    RECLINE_VK_DEV(vkDestroyBuffer);
    RECLINE_VK_DEV(vkGetBufferMemoryRequirements);
    RECLINE_VK_DEV(vkBindBufferMemory);
    RECLINE_VK_DEV(vkMapMemory);
    RECLINE_VK_DEV(vkCreateCommandPool);
    RECLINE_VK_DEV(vkDestroyCommandPool);
    RECLINE_VK_DEV(vkAllocateCommandBuffers);
    RECLINE_VK_DEV(vkBeginCommandBuffer);
    RECLINE_VK_DEV(vkEndCommandBuffer);
    RECLINE_VK_DEV(vkResetCommandBuffer);
    RECLINE_VK_DEV(vkCmdPipelineBarrier);
    RECLINE_VK_DEV(vkCmdCopyBufferToImage);
    RECLINE_VK_DEV(vkQueueSubmit);
    RECLINE_VK_DEV(vkQueueWaitIdle);
#undef RECLINE_VK_DEV
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
    return true;
  }

  bool FindMemory(uint32_t typeBits, VkMemoryPropertyFlags props, uint32_t* out) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
      if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) {
        *out = i;
        return true;
      }
    return false;
  }

  bool InitResources(std::string* err) {
    for (Image& img : images_) {
      VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      ici.imageType = VK_IMAGE_TYPE_2D;
      ici.format = VK_FORMAT_R8G8B8A8_UNORM;
      ici.extent = {w_, h_, 1};
      ici.mipLevels = 1;
      ici.arrayLayers = 1;
      ici.samples = VK_SAMPLE_COUNT_1_BIT;
      ici.tiling = VK_IMAGE_TILING_OPTIMAL;
      ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                  VK_IMAGE_USAGE_SAMPLED_BIT;
      ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      if (vkCreateImage(device_, &ici, nullptr, &img.image) != VK_SUCCESS) {
        *err = "vkCreateImage failed";
        return false;
      }
      VkMemoryRequirements mr;
      vkGetImageMemoryRequirements(device_, img.image, &mr);
      VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      ai.allocationSize = mr.size;
      if (!FindMemory(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &ai.memoryTypeIndex) ||
          vkAllocateMemory(device_, &ai, nullptr, &img.mem) != VK_SUCCESS ||
          vkBindImageMemory(device_, img.image, img.mem, 0) != VK_SUCCESS) {
        *err = "image memory allocation failed";
        return false;
      }
    }
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = VkDeviceSize(w_) * h_ * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(device_, &bci, nullptr, &staging_) != VK_SUCCESS) {
      *err = "vkCreateBuffer failed";
      return false;
    }
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(device_, staging_, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    if (!FindMemory(mr.memoryTypeBits,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    &ai.memoryTypeIndex) ||
        vkAllocateMemory(device_, &ai, nullptr, &stagingMem_) != VK_SUCCESS ||
        vkBindBufferMemory(device_, staging_, stagingMem_, 0) != VK_SUCCESS ||
        vkMapMemory(device_, stagingMem_, 0, VK_WHOLE_SIZE, 0, &stagingPtr_) != VK_SUCCESS) {
      *err = "staging memory allocation failed";
      return false;
    }
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queueFamily_;
    if (vkCreateCommandPool(device_, &pci, nullptr, &pool_) != VK_SUCCESS) {
      *err = "vkCreateCommandPool failed";
      return false;
    }
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &cai, &cmd_) != VK_SUCCESS) {
      *err = "vkAllocateCommandBuffers failed";
      return false;
    }
    return true;
  }
#undef RECLINE_VK_LOAD

  void Barrier(Image& img, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess,
               VkAccessFlags dstAccess) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &b);
  }

  void Destroy() {
    if (device_) {
      vkDeviceWaitIdle(device_);
      if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
      if (staging_) vkDestroyBuffer(device_, staging_, nullptr);
      if (stagingMem_) vkFreeMemory(device_, stagingMem_, nullptr);
      for (Image& img : images_) {
        if (img.image) vkDestroyImage(device_, img.image, nullptr);
        if (img.mem) vkFreeMemory(device_, img.mem, nullptr);
        img = Image{};
      }
      vkDestroyDevice(device_, nullptr);
    }
    if (instance_ && vkDestroyInstance) vkDestroyInstance(instance_, nullptr);
    device_ = VK_NULL_HANDLE;
    instance_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    staging_ = VK_NULL_HANDLE;
    stagingMem_ = VK_NULL_HANDLE;
    // lib_ stays loaded: unloading a Vulkan loader is not worth the risk.
  }

  uint32_t w_ = 0, h_ = 0;
  void* lib_ = nullptr;
  std::vector<std::string> instExts_, devExts_;
  VkInstance instance_ = VK_NULL_HANDLE;
  VkPhysicalDevice phys_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t queueFamily_ = 0;
  Image images_[2];
  int next_ = 0;
  VkBuffer staging_ = VK_NULL_HANDLE;
  VkDeviceMemory stagingMem_ = VK_NULL_HANDLE;
  void* stagingPtr_ = nullptr;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;

  PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
  PFN_vkCreateInstance vkCreateInstance = nullptr;
  PFN_vkDestroyInstance vkDestroyInstance = nullptr;
  PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices = nullptr;
  PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
  PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
  PFN_vkCreateDevice vkCreateDevice = nullptr;
  PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = nullptr;
  PFN_vkDestroyDevice vkDestroyDevice = nullptr;
  PFN_vkDeviceWaitIdle vkDeviceWaitIdle = nullptr;
  PFN_vkGetDeviceQueue vkGetDeviceQueue = nullptr;
  PFN_vkCreateImage vkCreateImage = nullptr;
  PFN_vkDestroyImage vkDestroyImage = nullptr;
  PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements = nullptr;
  PFN_vkAllocateMemory vkAllocateMemory = nullptr;
  PFN_vkFreeMemory vkFreeMemory = nullptr;
  PFN_vkBindImageMemory vkBindImageMemory = nullptr;
  PFN_vkCreateBuffer vkCreateBuffer = nullptr;
  PFN_vkDestroyBuffer vkDestroyBuffer = nullptr;
  PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements = nullptr;
  PFN_vkBindBufferMemory vkBindBufferMemory = nullptr;
  PFN_vkMapMemory vkMapMemory = nullptr;
  PFN_vkCreateCommandPool vkCreateCommandPool = nullptr;
  PFN_vkDestroyCommandPool vkDestroyCommandPool = nullptr;
  PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = nullptr;
  PFN_vkBeginCommandBuffer vkBeginCommandBuffer = nullptr;
  PFN_vkEndCommandBuffer vkEndCommandBuffer = nullptr;
  PFN_vkResetCommandBuffer vkResetCommandBuffer = nullptr;
  PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier = nullptr;
  PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage = nullptr;
  PFN_vkQueueSubmit vkQueueSubmit = nullptr;
  PFN_vkQueueWaitIdle vkQueueWaitIdle = nullptr;
};

#pragma GCC diagnostic pop

#else  // !RECLINE_HAVE_VULKAN

struct VkOverlayTexture::Impl {
  bool Init(uint32_t, uint32_t, std::string* err) {
    *err = "built without Vulkan headers";
    return false;
  }
  bool Upload(vr::VROverlayHandle_t, const uint8_t*, std::string* err) {
    *err = "built without Vulkan headers";
    return false;
  }
};

#endif

VkOverlayTexture::VkOverlayTexture() : impl_(std::make_unique<Impl>()) {}
VkOverlayTexture::~VkOverlayTexture() = default;

bool VkOverlayTexture::Init(uint32_t w, uint32_t h, std::string* err) {
  return impl_->Init(w, h, err);
}

bool VkOverlayTexture::Upload(vr::VROverlayHandle_t overlay, const uint8_t* rgba,
                              std::string* err) {
  return impl_->Upload(overlay, rgba, err);
}

}  // namespace recline
