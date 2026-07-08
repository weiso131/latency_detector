// 最小 Vulkan Layer：攔截 vkQueuePresentKHR 量 frametime，每秒印出 fps/min/max。
//
// 依 Vulkan Loader-Layer Interface 實作：
//   - 必須匯出 vkGetInstanceProcAddr / vkGetDeviceProcAddr（loader 的進入點）
//   - CreateInstance / CreateDevice 時把「下一層」的函式表存起來（dispatch table）
//   - QueuePresentKHR 攔截量時間後，再呼叫下一層真正的 present
//
// 參考：https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderLayerInterface.md

#define _POSIX_C_SOURCE 200809L   // 讓 clock_gettime / CLOCK_MONOTONIC 可見

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

// ---------------------------------------------------------------------------
// 每個 device 一份的狀態：下一層函式指標 + frametime 統計
// ---------------------------------------------------------------------------
typedef struct DeviceData {
    VkDevice device;
    PFN_vkGetDeviceProcAddr next_gdpa;   // 下一層的 GetDeviceProcAddr
    PFN_vkQueuePresentKHR   next_present; // 下一層真正的 present

    // frametime 統計
    int      have_last;        // 是否已有上一次 present 時間
    uint64_t last_present_ns;  // 上一次 present 的時間戳（奈秒）
    uint64_t window_start_ns;  // 這一秒統計視窗的起點

    uint64_t frame_count;      // 這一秒累積的 frame 數
    uint64_t min_ft_ns;        // 這一秒最小 frametime
    uint64_t max_ft_ns;        // 這一秒最大 frametime

    struct DeviceData *next;   // 簡單的鏈結串列（多 device 情況）
} DeviceData;

// 用 dispatch key（VkDevice / VkQueue 的第一個指標欄位）當索引查表。
// 這裡為求簡單，直接用鏈結串列線性找。
static DeviceData *g_devices = NULL;

// instance 的下一層 GetInstanceProcAddr。本 layer 只量 present，不攔任何
// instance 函式，但仍必須把「沒攔截的名字」轉呼叫下一層，否則應用程式拿到
// NULL 一呼叫就 segfault。為求最小實作用單一全域（假設只有一個 instance）。
static PFN_vkGetInstanceProcAddr g_next_gipa = NULL;

// Vulkan 每個 dispatchable handle 開頭都是一個指向 dispatch table 的指標，
// loader 用它來分辨物件。同一 device 底下的 queue 共用這個 key。
static void *get_dispatch_key(void *handle) {
    return *(void **)handle;
}

static DeviceData *find_device_by_key(void *key) {
    for (DeviceData *d = g_devices; d; d = d->next)
        if (get_dispatch_key(d->device) == key)
            return d;
    return NULL;
}

// ---------------------------------------------------------------------------
// 計時
// ---------------------------------------------------------------------------
static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void reset_window(DeviceData *d, uint64_t now) {
    d->window_start_ns = now;
    d->frame_count = 0;
    d->min_ft_ns = UINT64_MAX;
    d->max_ft_ns = 0;
}

// ---------------------------------------------------------------------------
// 攔截：QueuePresentKHR —— 每 frame present 一次
// ---------------------------------------------------------------------------
static VKAPI_ATTR VkResult VKAPI_CALL
layer_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo) {
    DeviceData *d = find_device_by_key(get_dispatch_key(queue));
    if (!d || !d->next_present) {
        // 沒記錄到這個 device（理論上不會發生）：無法往下呼叫，只能回錯
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    {
        uint64_t now = now_ns();

        if (d->have_last) {
            uint64_t ft = now - d->last_present_ns;   // 這個 frame 的 frametime
            if (ft < d->min_ft_ns) d->min_ft_ns = ft;
            if (ft > d->max_ft_ns) d->max_ft_ns = ft;
            d->frame_count++;
        } else {
            // 第一個 frame 沒有前一次可比，只記時間戳、開視窗
            d->have_last = 1;
            reset_window(d, now);
        }
        d->last_present_ns = now;

        // 每 1 秒輸出一次
        uint64_t elapsed = now - d->window_start_ns;
        if (elapsed >= 1000000000ull && d->frame_count > 0) {
            double secs = (double)elapsed / 1e9;
            double fps  = (double)d->frame_count / secs;
            printf("[latency] fps=%.1f  min_frametime=%.3f ms  max_frametime=%.3f ms  (frames=%llu)\n",
                   fps,
                   (double)d->min_ft_ns / 1e6,
                   (double)d->max_ft_ns / 1e6,
                   (unsigned long long)d->frame_count);
            fflush(stdout);
            reset_window(d, now);
        }
    }

    return d->next_present(queue, pPresentInfo);
}

// ---------------------------------------------------------------------------
// GetDeviceProcAddr —— 回傳我們攔截的函式，其餘往下一層丟
// ---------------------------------------------------------------------------
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
layer_GetDeviceProcAddr(VkDevice device, const char *pName) {
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
        return (PFN_vkVoidFunction)layer_GetDeviceProcAddr;
    if (strcmp(pName, "vkQueuePresentKHR") == 0)
        return (PFN_vkVoidFunction)layer_QueuePresentKHR;

    DeviceData *d = find_device_by_key(get_dispatch_key(device));
    if (d && d->next_gdpa)
        return d->next_gdpa(device, pName);
    return NULL;
}

// ---------------------------------------------------------------------------
// CreateDevice —— 呼叫下一層建立 device，並抓下一層的函式表
// ---------------------------------------------------------------------------
static VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateDevice(VkPhysicalDevice physicalDevice,
                   const VkDeviceCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator,
                   VkDevice *pDevice) {
    // 從 pNext 鏈找到 loader 塞進來的 layer link info
    VkLayerDeviceCreateInfo *chain = (VkLayerDeviceCreateInfo *)pCreateInfo->pNext;
    while (chain &&
           !(chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
             chain->function == VK_LAYER_LINK_INFO)) {
        chain = (VkLayerDeviceCreateInfo *)chain->pNext;
    }
    if (!chain)
        return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr   next_gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;

    // 往前推進 link，讓下一層拿到自己的 link info
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

    PFN_vkCreateDevice next_create =
        (PFN_vkCreateDevice)next_gipa(VK_NULL_HANDLE, "vkCreateDevice");
    if (!next_create)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res = next_create(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (res != VK_SUCCESS)
        return res;

    // 記錄這個 device 的狀態
    static DeviceData storage[16];  // 簡單起見：固定池，最多 16 個 device
    static int used = 0;
    if (used < 16) {
        DeviceData *d = &storage[used++];
        memset(d, 0, sizeof(*d));
        d->device = *pDevice;
        d->next_gdpa = next_gdpa;
        d->next_present =
            (PFN_vkQueuePresentKHR)next_gdpa(*pDevice, "vkQueuePresentKHR");
        d->min_ft_ns = UINT64_MAX;
        d->next = g_devices;
        g_devices = d;
    }
    return VK_SUCCESS;
}

// ---------------------------------------------------------------------------
// CreateInstance —— 只需推進 link 並轉呼叫，本 layer 不攔任何 instance 函式
// ---------------------------------------------------------------------------
static VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkInstance *pInstance) {
    VkLayerInstanceCreateInfo *chain = (VkLayerInstanceCreateInfo *)pCreateInfo->pNext;
    while (chain &&
           !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
             chain->function == VK_LAYER_LINK_INFO)) {
        chain = (VkLayerInstanceCreateInfo *)chain->pNext;
    }
    if (!chain)
        return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

    PFN_vkCreateInstance next_create =
        (PFN_vkCreateInstance)next_gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!next_create)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res = next_create(pCreateInfo, pAllocator, pInstance);
    if (res == VK_SUCCESS)
        g_next_gipa = next_gipa;   // 存下一層 gipa，供 fall-through 用
    return res;
}

// ---------------------------------------------------------------------------
// GetInstanceProcAddr —— loader 的主進入點
// ---------------------------------------------------------------------------
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
layer_GetInstanceProcAddr(VkInstance instance, const char *pName);

static PFN_vkVoidFunction intercept(const char *pName) {
    if (strcmp(pName, "vkGetInstanceProcAddr") == 0)
        return (PFN_vkVoidFunction)layer_GetInstanceProcAddr;
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
        return (PFN_vkVoidFunction)layer_GetDeviceProcAddr;
    if (strcmp(pName, "vkCreateInstance") == 0)
        return (PFN_vkVoidFunction)layer_CreateInstance;
    if (strcmp(pName, "vkCreateDevice") == 0)
        return (PFN_vkVoidFunction)layer_CreateDevice;
    if (strcmp(pName, "vkQueuePresentKHR") == 0)
        return (PFN_vkVoidFunction)layer_QueuePresentKHR;
    return NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
layer_GetInstanceProcAddr(VkInstance instance, const char *pName) {
    PFN_vkVoidFunction p = intercept(pName);
    if (p)
        return p;
    // 沒攔截的名字 → 一定要往下一層問，回傳真正的實作；
    // 否則應用程式拿到 NULL，一呼叫就崩潰。
    if (g_next_gipa)
        return g_next_gipa(instance, pName);
    return NULL;
}

// ---------------------------------------------------------------------------
// 匯出符號：loader 會找這個名字（negotiate 較新，這裡用經典入口）
// ---------------------------------------------------------------------------
__attribute__((visibility("default")))
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *pName) {
    return layer_GetInstanceProcAddr(instance, pName);
}

__attribute__((visibility("default")))
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *pName) {
    return layer_GetDeviceProcAddr(device, pName);
}
