#include "wsi_lab_protocol.h"
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

using namespace warvk::host;
static_assert(sizeof(void*)==8,"RH3 Vulkan host must be a real Win64 process");
namespace {
struct GpuFailure {const char* stage;VkResult result;};
void VkCheck(VkResult result,const char* stage) {
  if(result!=VK_SUCCESS)throw GpuFailure{stage,result};
}
template<typename T> bool HasExtension(const std::vector<T>& list,const char* wanted) {
  for(const auto& e:list)if(!std::strcmp(e.extensionName,wanted))return true;
  return false;
}
void CheckWindow(HWND window,DWORD parent,uint32_t width,uint32_t height) {
  DWORD actual=0;RECT r{};
  wsi::Require(IsWindow(window) && GetWindowThreadProcessId(window,&actual)!=0 && actual==parent,
               "window-owner");
  wsi::Require(GetClientRect(window,&r)!=FALSE && r.right-r.left==LONG(width) &&
               r.bottom-r.top==LONG(height),"window-extent");
}
#define INSTANCE_FUNCTIONS(X) \
  X(vkDestroyInstance) X(vkCreateWin32SurfaceKHR) X(vkDestroySurfaceKHR) \
  X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceQueueFamilyProperties) \
  X(vkGetPhysicalDeviceProperties) X(vkEnumerateDeviceExtensionProperties) \
  X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
  X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceFeatures2) \
  X(vkCreateDevice) X(vkGetDeviceProcAddr)
#define DEVICE_FUNCTIONS(X) \
  X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) \
  X(vkGetSwapchainImagesKHR) X(vkCreateCommandPool) X(vkDestroyCommandPool) \
  X(vkAllocateCommandBuffers) X(vkResetCommandBuffer) X(vkBeginCommandBuffer) \
  X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdClearColorImage) \
  X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCreateFence) X(vkDestroyFence) \
  X(vkResetFences) X(vkWaitForFences) X(vkAcquireNextImageKHR) X(vkQueueSubmit) X(vkQueuePresentKHR)
class Renderer {
public:
  ~Renderer() {
    // A failed GPU/WSI wait is terminal. Do not release possibly-live objects,
    // unload the driver, or fake completion. Exact process exit owns containment.
    // The runner's watchdog makes a hung driver a failure, never a successful drain.
    if(m_safe)shutdown();
  }
  void initialize(HWND window,DWORD parent) {
    m_window=window;m_parent=parent;
    m_loader=LoadLibraryExW(L"vulkan-1.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    wsi::Require(m_loader!=nullptr,"system-vulkan-loader");
    m_get=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(m_loader,"vkGetInstanceProcAddr"));
    wsi::Require(m_get!=nullptr,"loader-entry");
    auto version=reinterpret_cast<PFN_vkEnumerateInstanceVersion>(m_get(nullptr,"vkEnumerateInstanceVersion"));
    uint32_t api=VK_API_VERSION_1_0;
    wsi::Require(version!=nullptr,"vulkan-1.1-required");
    VkCheck(version(&api),"instance-version");
    wsi::Require(api>=VK_API_VERSION_1_1,"vulkan-1.1-required");
    auto enumerate=reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(m_get(nullptr,"vkEnumerateInstanceExtensionProperties"));
    auto create=reinterpret_cast<PFN_vkCreateInstance>(m_get(nullptr,"vkCreateInstance"));
    wsi::Require(enumerate && create,"instance-functions");
    uint32_t count=0;VkCheck(enumerate(nullptr,&count,nullptr),"instance-extension-count");
    wsi::Require(count>0 && count<=4096,"instance-extension-bound");
    std::vector<VkExtensionProperties> extensions(count);
    VkCheck(enumerate(nullptr,&count,extensions.data()),"instance-extensions");extensions.resize(count);
    const char* requested[]={VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
      VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME};
    for(const auto* ext:requested)wsi::Require(HasExtension(extensions,ext),"required-instance-extension-unsupported");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName="WarVK RH3 isolated WSI lab";app.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo=&app;info.enabledExtensionCount=4;info.ppEnabledExtensionNames=requested;
    VkCheck(create(&info,nullptr,&m_instance),"create-instance");
#define LOAD_INSTANCE(name) name=reinterpret_cast<PFN_##name>(m_get(m_instance,#name));wsi::Require(name!=nullptr,"missing-" #name);
    INSTANCE_FUNCTIONS(LOAD_INSTANCE)
#undef LOAD_INSTANCE
    VkWin32SurfaceCreateInfoKHR surface{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    surface.hwnd=m_window;
    // This is the HWND owner's instance, not GetModuleHandle(nullptr) in the host.
    surface.hinstance=reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window,GWLP_HINSTANCE));
    wsi::Require(surface.hinstance!=nullptr,"window-instance");
    VkCheck(vkCreateWin32SurfaceKHR(m_instance,&surface,nullptr,&m_surface),"create-cross-process-surface");
    VkCheck(vkEnumeratePhysicalDevices(m_instance,&count,nullptr),"physical-count");
    wsi::Require(count>0 && count<=32,"physical-bound");
    std::vector<VkPhysicalDevice> physical(count);
    VkCheck(vkEnumeratePhysicalDevices(m_instance,&count,physical.data()),"physical-list");physical.resize(count);
    for(auto candidate:physical) {
      uint32_t n=0;VkCheck(vkEnumerateDeviceExtensionProperties(candidate,nullptr,&n,nullptr),"device-extension-count");
      wsi::Require(n<=4096,"device-extension-bound");
      std::vector<VkExtensionProperties> available(n);
      VkCheck(vkEnumerateDeviceExtensionProperties(candidate,nullptr,&n,available.data()),"device-extensions");available.resize(n);
      if(!HasExtension(available,VK_KHR_SWAPCHAIN_EXTENSION_NAME) ||
         !HasExtension(available,VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME))continue;
      VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
      VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};features.pNext=&maintenance;
      vkGetPhysicalDeviceFeatures2(candidate,&features);
      if(!maintenance.swapchainMaintenance1)continue;
      vkGetPhysicalDeviceQueueFamilyProperties(candidate,&n,nullptr);
      wsi::Require(n>0 && n<=128,"queue-family-bound");
      std::vector<VkQueueFamilyProperties> queues(n);vkGetPhysicalDeviceQueueFamilyProperties(candidate,&n,queues.data());
      for(uint32_t i=0;i<n;++i) {
        VkBool32 present=VK_FALSE;
        VkCheck(vkGetPhysicalDeviceSurfaceSupportKHR(candidate,i,m_surface,&present),"surface-support");
        if(queues[i].queueCount && (queues[i].queueFlags&VK_QUEUE_GRAPHICS_BIT) && present) {
          m_physical=candidate;m_family=i;break;
        }
      }
      if(m_physical)break;
    }
    wsi::Require(m_physical!=VK_NULL_HANDLE,"present-fence-capable-device-unsupported");
    VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(m_physical,&properties);
    vendor=properties.vendorID;device=properties.deviceID;driver=properties.driverVersion;
    float priority=1.0f;VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex=m_family;queue.queueCount=1;queue.pQueuePriorities=&priority;
    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
    maintenance.swapchainMaintenance1=VK_TRUE;
    const char* deviceExtensions[]={VK_KHR_SWAPCHAIN_EXTENSION_NAME,VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME};
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};deviceInfo.pNext=&maintenance;
    deviceInfo.queueCreateInfoCount=1;deviceInfo.pQueueCreateInfos=&queue;
    deviceInfo.enabledExtensionCount=2;deviceInfo.ppEnabledExtensionNames=deviceExtensions;
    VkCheck(vkCreateDevice(m_physical,&deviceInfo,nullptr,&m_device),"create-device");
#define LOAD_DEVICE(name) name=reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(m_device,#name));wsi::Require(name!=nullptr,"missing-" #name);
    DEVICE_FUNCTIONS(LOAD_DEVICE)
#undef LOAD_DEVICE
    vkGetDeviceQueue(m_device,m_family,0,&m_queue);
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pool.queueFamilyIndex=m_family;
    VkCheck(vkCreateCommandPool(m_device,&pool,nullptr,&m_pool),"create-command-pool");
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool=m_pool;allocation.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;allocation.commandBufferCount=1;
    VkCheck(vkAllocateCommandBuffers(m_device,&allocation,&m_command),"allocate-command");
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkCheck(vkCreateSemaphore(m_device,&semaphore,nullptr,&m_acquired),"create-acquire-semaphore");
    VkCheck(vkCreateSemaphore(m_device,&semaphore,nullptr,&m_rendered),"create-render-semaphore");
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkCheck(vkCreateFence(m_device,&fence,nullptr,&m_gpuFence),"create-gpu-fence");
    VkCheck(vkCreateFence(m_device,&fence,nullptr,&m_presentFence),"create-present-fence");
  }
  void createSwapchain(uint32_t width,uint32_t height) {
    wsi::Require(m_safe && m_device,"swapchain-unsafe-owner");
    CheckWindow(m_window,m_parent,width,height);
    if(m_swapchain) {vkDestroySwapchainKHR(m_device,m_swapchain,nullptr);m_swapchain=VK_NULL_HANDLE;}
    m_images.clear();
    VkSurfaceCapabilitiesKHR caps{};
    VkCheck(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physical,m_surface,&caps),"surface-capabilities");
    wsi::Require((caps.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT)!=0,"swapchain-transfer-unsupported");
    VkExtent2D extent{width,height};
    if(caps.currentExtent.width!=UINT32_MAX)extent=caps.currentExtent;
    wsi::Require(extent.width==width && extent.height==height &&
      width>=caps.minImageExtent.width && width<=caps.maxImageExtent.width &&
      height>=caps.minImageExtent.height && height<=caps.maxImageExtent.height,"surface-extent");
    uint32_t n=0;VkCheck(vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical,m_surface,&n,nullptr),"surface-format-count");
    wsi::Require(n>0 && n<=256,"surface-format-bound");
    std::vector<VkSurfaceFormatKHR> formats(n);
    VkCheck(vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical,m_surface,&n,formats.data()),"surface-formats");
    VkSurfaceFormatKHR format{};bool found=false;
    for(uint32_t i=0;i<n;++i)if((formats[i].format==VK_FORMAT_B8G8R8A8_UNORM ||
       formats[i].format==VK_FORMAT_R8G8B8A8_UNORM) && formats[i].colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      format=formats[i];found=true;break;
    }
    if(n==1 && formats[0].format==VK_FORMAT_UNDEFINED) {
      format={VK_FORMAT_B8G8R8A8_UNORM,VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};found=true;
    }
    wsi::Require(found,"surface-rgba-format-unsupported");
    const uint32_t images=std::max(2u,caps.minImageCount);
    wsi::Require(!caps.maxImageCount || images<=caps.maxImageCount,"surface-image-count");
    VkCompositeAlphaFlagBitsKHR alpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for(auto bit:{VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                 VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
      if(caps.supportedCompositeAlpha&bit){alpha=bit;break;}
    wsi::Require((caps.supportedCompositeAlpha&alpha)!=0,"surface-alpha");
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface=m_surface;info.minImageCount=images;info.imageFormat=format.format;
    info.imageColorSpace=format.colorSpace;info.imageExtent=extent;info.imageArrayLayers=1;
    info.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;info.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform=caps.currentTransform;info.compositeAlpha=alpha;info.presentMode=VK_PRESENT_MODE_FIFO_KHR;
    info.clipped=VK_TRUE;
    VkCheck(vkCreateSwapchainKHR(m_device,&info,nullptr,&m_swapchain),"create-swapchain");
    VkCheck(vkGetSwapchainImagesKHR(m_device,m_swapchain,&n,nullptr),"swapchain-image-count");
    wsi::Require(n>0 && n<=16,"swapchain-image-bound");m_images.resize(n);
    VkCheck(vkGetSwapchainImagesKHR(m_device,m_swapchain,&n,m_images.data()),"swapchain-images");m_images.resize(n);
    m_width=width;m_height=height;++swapchainsCreated;
  }
  void draw() {
    CheckWindow(m_window,m_parent,m_width,m_height);
    wsi::Require(m_safe && m_swapchain,"draw-state");
    // Fence timeout/device loss is terminal. A render fence alone does not
    // release the presentation engine's use of the image/semaphore/swapchain.
    m_safe=false;
    uint32_t index=0;
    VkCheck(vkAcquireNextImageKHR(m_device,m_swapchain,5000000000ull,m_acquired,VK_NULL_HANDLE,&index),"acquire-image");
    wsi::Require(index<m_images.size(),"acquire-index");
    VkCheck(vkResetCommandBuffer(m_command,0),"reset-command");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkCheck(vkBeginCommandBuffer(m_command,&begin),"begin-command");
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.image=m_images[index];
    barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(m_command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0,0,nullptr,0,nullptr,1,&barrier);
    VkClearColorValue color{};color.float32[0]=presented%2?0.15f:0.65f;
    color.float32[1]=0.25f;color.float32[2]=0.45f;color.float32[3]=1.0f;
    vkCmdClearColorImage(m_command,m_images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&color,1,&barrier.subresourceRange);
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=0;
    vkCmdPipelineBarrier(m_command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0,0,nullptr,0,nullptr,1,&barrier);
    VkCheck(vkEndCommandBuffer(m_command),"end-command");
    VkFence fences[]={m_gpuFence,m_presentFence};VkCheck(vkResetFences(m_device,2,fences),"reset-fences");
    const VkPipelineStageFlags stage=VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&m_acquired;
    submit.pWaitDstStageMask=&stage;submit.commandBufferCount=1;submit.pCommandBuffers=&m_command;
    submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&m_rendered;
    VkCheck(vkQueueSubmit(m_queue,1,&submit,m_gpuFence),"queue-submit");
    VkSwapchainPresentFenceInfoEXT completion{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT};
    completion.swapchainCount=1;completion.pFences=&m_presentFence;
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};present.pNext=&completion;
    present.waitSemaphoreCount=1;present.pWaitSemaphores=&m_rendered;
    present.swapchainCount=1;present.pSwapchains=&m_swapchain;present.pImageIndices=&index;
    VkCheck(vkQueuePresentKHR(m_queue,&present),"queue-present");
    VkCheck(vkWaitForFences(m_device,1,&m_gpuFence,VK_TRUE,5000000000ull),"render-fence-timeout-or-error");
    ++gpuCompleted;
    VkCheck(vkWaitForFences(m_device,1,&m_presentFence,VK_TRUE,5000000000ull),"present-fence-timeout-or-error");
    ++presented;m_safe=true;
  }
  void shutdown() noexcept {
    if(!m_safe)return;
    if(m_device) {
      if(m_swapchain && vkDestroySwapchainKHR)vkDestroySwapchainKHR(m_device,m_swapchain,nullptr);
      if(m_gpuFence && vkDestroyFence)vkDestroyFence(m_device,m_gpuFence,nullptr);
      if(m_presentFence && vkDestroyFence)vkDestroyFence(m_device,m_presentFence,nullptr);
      if(m_acquired && vkDestroySemaphore)vkDestroySemaphore(m_device,m_acquired,nullptr);
      if(m_rendered && vkDestroySemaphore)vkDestroySemaphore(m_device,m_rendered,nullptr);
      if(m_pool && vkDestroyCommandPool)vkDestroyCommandPool(m_device,m_pool,nullptr);
      if(vkDestroyDevice)vkDestroyDevice(m_device,nullptr);
    }
    if(m_surface && vkDestroySurfaceKHR)vkDestroySurfaceKHR(m_instance,m_surface,nullptr);
    if(m_instance && vkDestroyInstance)vkDestroyInstance(m_instance,nullptr);
    if(m_loader)FreeLibrary(m_loader);
    m_device=VK_NULL_HANDLE;m_instance=VK_NULL_HANDLE;m_surface=VK_NULL_HANDLE;m_loader=nullptr;
    m_swapchain=VK_NULL_HANDLE;m_safe=false;cleaned=true;
  }
  uint32_t presented=0,gpuCompleted=0,swapchainsCreated=0,vendor=0,device=0,driver=0;
  bool cleaned=false;
private:
  HMODULE m_loader=nullptr;PFN_vkGetInstanceProcAddr m_get=nullptr;
#define DECLARE(name) PFN_##name name=nullptr;
  INSTANCE_FUNCTIONS(DECLARE)
  DEVICE_FUNCTIONS(DECLARE)
#undef DECLARE
  HWND m_window=nullptr;DWORD m_parent=0;bool m_safe=true;
  VkInstance m_instance=VK_NULL_HANDLE;VkSurfaceKHR m_surface=VK_NULL_HANDLE;
  VkPhysicalDevice m_physical=VK_NULL_HANDLE;VkDevice m_device=VK_NULL_HANDLE;
  VkQueue m_queue=VK_NULL_HANDLE;uint32_t m_family=0,m_width=0,m_height=0;
  VkSwapchainKHR m_swapchain=VK_NULL_HANDLE;std::vector<VkImage> m_images;
  VkCommandPool m_pool=VK_NULL_HANDLE;VkCommandBuffer m_command=VK_NULL_HANDLE;
  VkSemaphore m_acquired=VK_NULL_HANDLE,m_rendered=VK_NULL_HANDLE;
  VkFence m_gpuFence=VK_NULL_HANDLE,m_presentFence=VK_NULL_HANDLE;
};
}
int wmain(int argc,wchar_t** argv) {
  lab::IoStats stats;Renderer gpu;bool verified=false,windowVerified=false,ok=false;
  const char* reason="none";int32_t vkResult=0;uint32_t accepted=0,resizes=0;
  try {
    wsi::Require(argc==4,"arguments");
    const auto nonce=lab::ParseNonce(argv[1]);const auto pid=wsi::Number(argv[2]);
    wsi::Require(pid>0 && pid<=MAXDWORD,"parent-pid");
    auto parent=lab::OpenPeer(DWORD(pid),wsi::Number(argv[3]));
    auto pipe=lab::CreateServer(nonce,lab::Channel::Present3);
    lab::Accept(pipe.get(),parent.get(),stats);lab::VerifyPeer(pipe.get(),true,DWORD(pid));verified=true;
    uint64_t sequence=1,epoch=0;uint32_t hwnd=0,width=0,height=0;bool hello=false,active=false;
    for(;;) {
      auto m=wsi::Read(pipe.get(),parent.get(),stats);
      wsi::Require(!m.reply && m.senderBits==32 && m.peerBits==64 && m.nonce==nonce &&
        m.sequence==sequence && m.presented==0 && m.gpuCompleted==0 && m.resized==0,"request-identity");
      if(m.op==wsi::Op::Hello) {
        wsi::Require(!hello && m.sequence==1 && m.windowEpoch==0 && !m.hwnd && !m.width && !m.height && !m.frames,"hello-state");
        hello=true;
      } else if(m.op==wsi::Op::Create) {
        wsi::Require(hello && !active && epoch==0 && m.windowEpoch==1 && m.hwnd &&
          m.width && m.height && !m.frames,"create-state");
        HWND window=wsi::DecodeWindow(m.hwnd);CheckWindow(window,DWORD(pid),m.width,m.height);windowVerified=true;
        gpu.initialize(window,DWORD(pid));gpu.createSwapchain(m.width,m.height);
        active=true;epoch=m.windowEpoch;hwnd=m.hwnd;width=m.width;height=m.height;
      } else {
        wsi::Require(active && m.hwnd==hwnd,"active-window");
        if(m.op==wsi::Op::Resize) {
          wsi::Require(m.windowEpoch==epoch+1 && m.width && m.height && !m.frames &&
            resizes==0 && gpu.presented==wsi::FramesPerBatch,"resize-state");
          gpu.createSwapchain(m.width,m.height);epoch=m.windowEpoch;width=m.width;height=m.height;++resizes;
        } else {
          wsi::Require(m.windowEpoch==epoch && m.width==width && m.height==height,"active-scope");
          if(m.op==wsi::Op::Frames) {
            wsi::Require(m.frames==wsi::FramesPerBatch && gpu.presented==resizes*wsi::FramesPerBatch,"frame-state");
            for(uint32_t i=0;i<m.frames;++i)gpu.draw();
          } else {
            wsi::Require(m.op==wsi::Op::Close && !m.frames && gpu.presented==6 && resizes==1,"close-state");
            gpu.shutdown();wsi::Require(gpu.cleaned,"close-cleanup");active=false;
          }
        }
      }
      ++sequence;++accepted;m.reply=true;m.senderBits=64;m.peerBits=32;
      m.presented=gpu.presented;m.gpuCompleted=gpu.gpuCompleted;m.resized=resizes;
      wsi::Write(pipe.get(),parent.get(),m,stats);
      if(m.op==wsi::Op::Close){ok=true;break;}
    }
  } catch(const GpuFailure& e) {reason=e.stage;vkResult=int32_t(e.result);}
    catch(const lab::Failure& e) {reason=lab::FaultName(e.fault);}
    catch(const std::runtime_error& e) {
      // All runtime_error text here is a fixed literal from the strict protocol.
      std::printf("{\"bits\":64,\"ok\":false,\"reason\":\"%s\",\"peerVerified\":%s,"
        "\"windowOwnerVerified\":%s,\"accepted\":%u,\"presented\":%u,\"gpuCompleted\":%u,"
        "\"vkResult\":0,\"gameTouched\":false}\n",e.what(),verified?"true":"false",
        windowVerified?"true":"false",accepted,gpu.presented,gpu.gpuCompleted);
      std::fflush(stdout);return 2;
    } catch(...) {reason="unexpected-exception";}
  std::printf("{\"bits\":64,\"ok\":%s,\"reason\":\"%s\",\"vkResult\":%d,"
    "\"peerVerified\":%s,\"windowOwnerVerified\":%s,\"accepted\":%u,"
    "\"presented\":%u,\"gpuCompleted\":%u,\"resizes\":%u,\"swapchainsCreated\":%u,"
    "\"vendorId\":%u,\"deviceId\":%u,\"driverVersion\":%u,\"normalGpuCleanup\":%s,"
    "\"presentFenceRequired\":true,\"cpuImageTransfers\":0,\"gameTouched\":false}\n",
    ok?"true":"false",reason,vkResult,verified?"true":"false",windowVerified?"true":"false",
    accepted,gpu.presented,gpu.gpuCompleted,resizes,gpu.swapchainsCreated,gpu.vendor,gpu.device,gpu.driver,
    gpu.cleaned?"true":"false");
  std::fflush(stdout);return ok?0:2;
}
