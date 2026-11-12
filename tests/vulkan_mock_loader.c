/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* A synthetic Vulkan loader used ONLY by the host-side Linux tests. */
static int mock_instance, mock_device;
static int mode(const char *name)
{
    const char *value = getenv("NT_VULKAN_MOCK_CASE");
    return value != NULL && strcmp(value, name) == 0;
}
static int32_t mock_version(uint32_t *out)
{
    if (out == NULL) return -1;
    *out = (1u << 22) | ((mode("old") ? 2u : 3u) << 12);
    return 0;
}
static int32_t mock_create(const void *info, const void *allocator, void **out)
{
    (void)allocator;
    if (info == NULL || out == NULL) return -1;
    *out = &mock_instance;
    return 0;
}
static void mock_destroy(void *instance, const void *allocator)
{ (void)instance; (void)allocator; }
static int32_t mock_devices(void *instance, uint32_t *count, void **list)
{
    if (instance != &mock_instance || count == NULL) return -1;
    if (list == NULL) { *count = mode("empty") ? 0 : 1; return 0; }
    if (*count < 1) return -1;
    list[0] = &mock_device;
    *count = 1;
    return 0;
}
struct mock_extension { char name[256]; uint32_t version; };
static int32_t mock_extensions(void *device, const char *layer, uint32_t *count,
                               struct mock_extension *extensions)
{
    if (device != &mock_device || layer != NULL || count == NULL) return -1;
    if (extensions == NULL) { *count = 1; return 0; }
    if (*count < 1) return -1;
    memset(extensions, 0, sizeof(*extensions));
    const char *name = mode("noswap") ? "VK_EXT_debug_utils" : "VK_KHR_swapchain";
    memcpy(extensions[0].name, name, strlen(name));
    extensions[0].version = 1;
    *count = 1;
    return 0;
}
#define MOCK_ADDRESS(function) do { \
    void (*pointer)(void) = (void (*)(void))function; \
    void *bits = NULL; \
    _Static_assert(sizeof(pointer) == sizeof(bits), "Linux AMD64 pointer size"); \
    memcpy(&bits, &pointer, sizeof(bits)); \
    return bits; \
} while (0)
void *vkGetInstanceProcAddr(void *instance, const char *name)
{
    (void)instance;
    if (name == NULL) return NULL;
    if (strcmp(name, "vkEnumerateInstanceVersion") == 0)
        MOCK_ADDRESS(mock_version);
    if (strcmp(name, "vkCreateInstance") == 0)
        MOCK_ADDRESS(mock_create);
    if (strcmp(name, "vkDestroyInstance") == 0)
        MOCK_ADDRESS(mock_destroy);
    if (strcmp(name, "vkEnumeratePhysicalDevices") == 0)
        MOCK_ADDRESS(mock_devices);
    if (strcmp(name, "vkEnumerateDeviceExtensionProperties") == 0)
        MOCK_ADDRESS(mock_extensions);
    return NULL;
}
