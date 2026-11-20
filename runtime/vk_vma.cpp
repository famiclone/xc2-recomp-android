// Vulkan Memory Allocator implementation (AMD, MIT; from the MSYS2 package vulkan-memory-allocator, not copied).
// VMA is C++ only, so this is the one C++ file of the runtime; the rest uses the C interface of vk_mem_alloc.h.
#define VMA_IMPLEMENTATION
#ifdef __ANDROID__
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#else
#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#endif
#ifdef __clang__
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wunused-private-field"
#endif
#include <vulkan/vulkan.h>
#include "vk_mem_alloc.h"
