/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "ntnative/vulkan_probe.h"

#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Minimal ABI definitions for the Vulkan loader's stable, host-native calls.
 * No Windows PE DLL, proprietary code or guest function is executed. */
typedef void *nt_vk_instance;
typedef void *nt_vk_physical_device;
typedef int32_t nt_vk_result;
typedef uint32_t nt_vk_flags;
typedef struct {
    uint32_t s_type;
    const void *next;
    const char *name;
    uint32_t application_version;
    const char *engine;
    uint32_t engine_version;
    uint32_t api_version;
} nt_vk_application_info;
typedef struct {
    uint32_t s_type;
    const void *next;
    nt_vk_flags flags;
    const nt_vk_application_info *application;
    uint32_t enabled_layer_count;
    const char *const *layers;
    uint32_t enabled_extension_count;
    const char *const *extensions;
} nt_vk_instance_info;
typedef struct {
    char name[256];
    uint32_t spec_version;
} nt_vk_extension;
typedef void (*nt_vk_void_function)(void);
typedef nt_vk_void_function (*nt_vk_get_proc)(nt_vk_instance, const char *);
typedef nt_vk_result (*nt_vk_enumerate_version)(uint32_t *);
typedef nt_vk_result (*nt_vk_create_instance)(const nt_vk_instance_info *, const void *, nt_vk_instance *);
typedef void (*nt_vk_destroy_instance)(nt_vk_instance, const void *);
typedef nt_vk_result (*nt_vk_enumerate_devices)(nt_vk_instance, uint32_t *, nt_vk_physical_device *);
typedef nt_vk_result (*nt_vk_enumerate_extensions)(nt_vk_physical_device, const char *, uint32_t *, nt_vk_extension *);

#define NT_VK_SUCCESS 0
#define NT_VK_INCOMPLETE 5
#define NT_VK_API_1_0 (1u << 22)
#define NT_VK_S_TYPE_APPLICATION_INFO 0
#define NT_VK_S_TYPE_INSTANCE_CREATE_INFO 1
#define NT_VK_MAX_DEVICES 64u
#define NT_VK_MAX_EXTENSIONS 2048u

static int load_proc(nt_vk_get_proc get, nt_vk_instance instance, const char *name,
                     void *destination, size_t pointer_size)
{
    const nt_vk_void_function generic = get(instance, name);
    if (generic == NULL || pointer_size != sizeof(generic))
        return 0;
    memcpy(destination, &generic, pointer_size);
    return 1;
}

int nt_vulkan_version_at_least(uint32_t version, unsigned major, unsigned minor)
{
    const uint32_t version_major = (version >> 22) & 0x7fu;
    const uint32_t version_minor = (version >> 12) & 0x3ffu;
    return version_major > major || (version_major == major && version_minor >= minor);
}

const char *nt_vulkan_state_name(enum nt_vulkan_state state)
{
    switch (state) {
    case NT_VULKAN_LOADER_MISSING: return "Vulkan loader unavailable";
    case NT_VULKAN_SYMBOL_MISSING: return "Vulkan loader entry points unavailable";
    case NT_VULKAN_LOADER_ERROR: return "Vulkan instance or device query failed";
    case NT_VULKAN_NO_PHYSICAL_DEVICE: return "No Vulkan physical devices found";
    case NT_VULKAN_NO_SWAPCHAIN: return "No device with VK_KHR_swapchain found";
    case NT_VULKAN_API_BELOW_1_3: return "Vulkan loader version below 1.3";
    case NT_VULKAN_PRELIMINARY_READY: return "Vulkan preflight complete";
    }
    return "Unknown Vulkan probe result";
}

static int has_swapchain(const nt_vk_extension *extensions, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) {
        if (memchr(extensions[i].name, 0, sizeof(extensions[i].name)) != NULL &&
            strcmp(extensions[i].name, "VK_KHR_swapchain") == 0)
            return 1;
    }
    return 0;
}

void nt_vulkan_probe(struct nt_vulkan_report *report)
{
    if (report == NULL)
        return;
    *report = (struct nt_vulkan_report){.state = NT_VULKAN_LOADER_MISSING,
                                         .loader_api_version = NT_VK_API_1_0};
    void *library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (library == NULL)
        return;

    nt_vk_get_proc get = NULL;
    void *symbol = dlsym(library, "vkGetInstanceProcAddr");
    if (symbol != NULL && sizeof(symbol) == sizeof(get))
        memcpy(&get, &symbol, sizeof(get));
    if (get == NULL) {
        report->state = NT_VULKAN_SYMBOL_MISSING;
        dlclose(library);
        return;
    }
    nt_vk_enumerate_version get_version = NULL;
    if (load_proc(get, NULL, "vkEnumerateInstanceVersion", &get_version, sizeof(get_version))) {
        if ((report->last_vk_result = get_version(&report->loader_api_version)) != NT_VK_SUCCESS) {
            report->state = NT_VULKAN_LOADER_ERROR;
            dlclose(library);
            return;
        }
    }
    nt_vk_create_instance create = NULL;
    if (!load_proc(get, NULL, "vkCreateInstance", &create, sizeof(create))) {
        report->state = NT_VULKAN_SYMBOL_MISSING;
        dlclose(library);
        return;
    }
    const nt_vk_application_info application = {NT_VK_S_TYPE_APPLICATION_INFO, NULL,
        "nt-native-linux Vulkan preflight", 0, "nt-native-linux", 0, NT_VK_API_1_0};
    const nt_vk_instance_info info = {NT_VK_S_TYPE_INSTANCE_CREATE_INFO, NULL, 0,
        &application, 0, NULL, 0, NULL};
    nt_vk_instance instance = NULL;
    report->last_vk_result = create(&info, NULL, &instance);
    if (report->last_vk_result != NT_VK_SUCCESS || instance == NULL) {
        report->state = NT_VULKAN_LOADER_ERROR;
        dlclose(library);
        return;
    }
    nt_vk_destroy_instance destroy = NULL;
    nt_vk_enumerate_devices devices = NULL;
    nt_vk_enumerate_extensions extensions = NULL;
    if (!load_proc(get, instance, "vkDestroyInstance", &destroy, sizeof(destroy)) ||
        !load_proc(get, instance, "vkEnumeratePhysicalDevices", &devices, sizeof(devices)) ||
        !load_proc(get, instance, "vkEnumerateDeviceExtensionProperties", &extensions, sizeof(extensions))) {
        report->state = NT_VULKAN_SYMBOL_MISSING;
        goto cleanup;
    }
    uint32_t count = 0;
    report->last_vk_result = devices(instance, &count, NULL);
    if (report->last_vk_result != NT_VK_SUCCESS || count > NT_VK_MAX_DEVICES) {
        report->state = NT_VULKAN_LOADER_ERROR;
        goto cleanup;
    }
    if (count == 0) {
        report->state = NT_VULKAN_NO_PHYSICAL_DEVICE;
        goto cleanup;
    }
    nt_vk_physical_device *physical = calloc(count, sizeof(*physical));
    if (physical == NULL) {
        report->state = NT_VULKAN_LOADER_ERROR;
        goto cleanup;
    }
    uint32_t capacity = count;
    report->last_vk_result = devices(instance, &capacity, physical);
    if ((report->last_vk_result != NT_VK_SUCCESS && report->last_vk_result != NT_VK_INCOMPLETE) ||
        capacity > count) {
        report->state = NT_VULKAN_LOADER_ERROR;
        free(physical);
        goto cleanup;
    }
    report->physical_devices = capacity;
    for (uint32_t i = 0; i < capacity; ++i) {
        uint32_t ext_count = 0;
        if (extensions(physical[i], NULL, &ext_count, NULL) != NT_VK_SUCCESS ||
            ext_count == 0 || ext_count > NT_VK_MAX_EXTENSIONS)
            continue;
        nt_vk_extension *ext = calloc(ext_count, sizeof(*ext));
        if (ext == NULL)
            continue;
        uint32_t ext_capacity = ext_count;
        const nt_vk_result ext_status = extensions(physical[i], NULL, &ext_capacity, ext);
        if ((ext_status == NT_VK_SUCCESS || ext_status == NT_VK_INCOMPLETE) &&
            ext_capacity <= ext_count && has_swapchain(ext, ext_capacity))
            ++report->swapchain_devices;
        free(ext);
    }
    free(physical);
    report->state = report->swapchain_devices == 0 ? NT_VULKAN_NO_SWAPCHAIN :
        nt_vulkan_version_at_least(report->loader_api_version, 1, 3) ?
        NT_VULKAN_PRELIMINARY_READY : NT_VULKAN_API_BELOW_1_3;
cleanup:
    if (destroy != NULL)
        destroy(instance, NULL);
    /* Destroy the Vulkan instance before releasing its loader. */
    dlclose(library);
}
