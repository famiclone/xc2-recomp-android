// Dynamic Vulkan function pointer table for Android (VK_NO_PROTOTYPES), supporting both the system Vulkan loader
// (Qualcomm Adreno) and custom Mesa Turnip drivers loaded via libadrenotools or dlopen (XC2_VK_DRIVER).
#pragma once

#ifdef __ANDROID__
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

// Pre-instance functions resolved via vkGetInstanceProcAddr(NULL, name).
#define VK_LOADER_GLOBAL_FUNCS(X) \
    X(vkCreateInstance) \
    X(vkEnumerateInstanceExtensionProperties) \
    X(vkEnumerateInstanceLayerProperties) \
    X(vkEnumerateInstanceVersion)

// Instance-level functions resolved via vkGetInstanceProcAddr(instance, name).
#define VK_LOADER_INSTANCE_FUNCS(X) \
    X(vkCreateDevice) \
    X(vkDestroyInstance) \
    X(vkDestroySurfaceKHR) \
    X(vkEnumerateDeviceExtensionProperties) \
    X(vkEnumeratePhysicalDevices) \
    X(vkGetDeviceProcAddr) \
    X(vkGetPhysicalDeviceFeatures) \
    X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetPhysicalDeviceFormatProperties) \
    X(vkGetPhysicalDeviceImageFormatProperties2) \
    X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceMemoryProperties2) \
    X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceProperties2) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)

// Device-level functions resolved via vkGetDeviceProcAddr(device, name).
#define VK_LOADER_DEVICE_FUNCS(X) \
    X(vkAcquireNextImageKHR) \
    X(vkAllocateCommandBuffers) \
    X(vkAllocateDescriptorSets) \
    X(vkAllocateMemory) \
    X(vkBeginCommandBuffer) \
    X(vkBindBufferMemory) \
    X(vkBindImageMemory) \
    X(vkCmdBeginQuery) \
    X(vkCmdBeginRendering) \
    X(vkCmdBindDescriptorSets) \
    X(vkCmdBindIndexBuffer) \
    X(vkCmdBindPipeline) \
    X(vkCmdBindVertexBuffers) \
    X(vkCmdBindVertexBuffers2) \
    X(vkCmdBlitImage) \
    X(vkCmdClearAttachments) \
    X(vkCmdClearColorImage) \
    X(vkCmdClearDepthStencilImage) \
    X(vkCmdCopyBuffer) \
    X(vkCmdCopyBufferToImage) \
    X(vkCmdCopyImage) \
    X(vkCmdCopyImageToBuffer) \
    X(vkCmdDispatch) \
    X(vkCmdDispatchIndirect) \
    X(vkCmdDraw) \
    X(vkCmdDrawIndexed) \
    X(vkCmdDrawIndexedIndirect) \
    X(vkCmdEndQuery) \
    X(vkCmdEndRendering) \
    X(vkCmdFillBuffer) \
    X(vkCmdPipelineBarrier) \
    X(vkCmdPipelineBarrier2) \
    X(vkCmdPushConstants) \
    X(vkCmdResetQueryPool) \
    X(vkCmdResolveImage) \
    X(vkCmdSetBlendConstants) \
    X(vkCmdSetCullMode) \
    X(vkCmdSetDepthBias) \
    X(vkCmdSetDepthBiasEnable) \
    X(vkCmdSetDepthCompareOp) \
    X(vkCmdSetDepthTestEnable) \
    X(vkCmdSetDepthWriteEnable) \
    X(vkCmdSetFrontFace) \
    X(vkCmdSetPrimitiveRestartEnable) \
    X(vkCmdSetPrimitiveTopology) \
    X(vkCmdSetRasterizerDiscardEnable) \
    X(vkCmdSetScissorWithCount) \
    X(vkCmdSetStencilCompareMask) \
    X(vkCmdSetStencilOp) \
    X(vkCmdSetStencilReference) \
    X(vkCmdSetStencilTestEnable) \
    X(vkCmdSetStencilWriteMask) \
    X(vkCmdSetViewportWithCount) \
    X(vkCmdWriteTimestamp) \
    X(vkCmdWriteTimestamp2) \
    X(vkCreateBuffer) \
    X(vkCreateBufferView) \
    X(vkCreateCommandPool) \
    X(vkCreateComputePipelines) \
    X(vkCreateDescriptorPool) \
    X(vkCreateDescriptorSetLayout) \
    X(vkCreateFence) \
    X(vkCreateGraphicsPipelines) \
    X(vkCreateImage) \
    X(vkCreateImageView) \
    X(vkCreatePipelineCache) \
    X(vkCreatePipelineLayout) \
    X(vkCreateQueryPool) \
    X(vkCreateSampler) \
    X(vkCreateSemaphore) \
    X(vkCreateShaderModule) \
    X(vkCreateSwapchainKHR) \
    X(vkDestroyBuffer) \
    X(vkDestroyBufferView) \
    X(vkDestroyCommandPool) \
    X(vkDestroyDescriptorPool) \
    X(vkDestroyDescriptorSetLayout) \
    X(vkDestroyDevice) \
    X(vkDestroyFence) \
    X(vkDestroyImage) \
    X(vkDestroyImageView) \
    X(vkDestroyPipeline) \
    X(vkDestroyPipelineCache) \
    X(vkDestroyPipelineLayout) \
    X(vkDestroyQueryPool) \
    X(vkDestroySampler) \
    X(vkDestroySemaphore) \
    X(vkDestroyShaderModule) \
    X(vkDestroySwapchainKHR) \
    X(vkDeviceWaitIdle) \
    X(vkEndCommandBuffer) \
    X(vkFlushMappedMemoryRanges) \
    X(vkFreeCommandBuffers) \
    X(vkFreeDescriptorSets) \
    X(vkFreeMemory) \
    X(vkGetBufferMemoryRequirements) \
    X(vkGetDeviceImageMemoryRequirements) \
    X(vkGetDeviceQueue) \
    X(vkGetFenceStatus) \
    X(vkGetImageMemoryRequirements) \
    X(vkGetPipelineCacheData) \
    X(vkGetQueryPoolResults) \
    X(vkGetSemaphoreCounterValue) \
    X(vkGetSwapchainImagesKHR) \
    X(vkInvalidateMappedMemoryRanges) \
    X(vkMapMemory) \
    X(vkQueuePresentKHR) \
    X(vkQueueSubmit) \
    X(vkQueueSubmit2) \
    X(vkQueueWaitIdle) \
    X(vkResetCommandBuffer) \
    X(vkResetDescriptorPool) \
    X(vkResetFences) \
    X(vkResetQueryPool) \
    X(vkSignalSemaphore) \
    X(vkUnmapMemory) \
    X(vkUpdateDescriptorSets) \
    X(vkWaitForFences) \
    X(vkWaitSemaphores)

extern PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
#define VK_LOADER_DECL(fn) extern PFN_##fn fn;
VK_LOADER_GLOBAL_FUNCS(VK_LOADER_DECL)
VK_LOADER_INSTANCE_FUNCS(VK_LOADER_DECL)
VK_LOADER_DEVICE_FUNCS(VK_LOADER_DECL)
#undef VK_LOADER_DECL

// Opens the Vulkan driver (or adopts SDL_pfn when SDL loaded the system/custom library) and resolves global symbols.
int vk_loader_open(PFN_vkGetInstanceProcAddr sdl_proc);
void vk_loader_load_instance(VkInstance instance);
void vk_loader_load_device(VkInstance instance, VkDevice device);
// 1 when a custom Turnip driver was opened directly via libadrenotools/dlopen rather than SDL_Vulkan_LoadLibrary.
int vk_loader_is_custom(void);
int vk_loader_create_android_surface(VkInstance instance, void *anative_window, VkSurfaceKHR *out);

#ifdef __cplusplus
}
#endif
#endif // __ANDROID__
