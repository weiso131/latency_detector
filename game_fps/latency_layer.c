// Minimal Vulkan layer: intercepts vkQueuePresentKHR, measures frametime, and
// publishes fps/min/max once per second into a POSIX shared-memory buffer via
// seqlock (SPSC, latest value wins). The reader side (Rust) is separate.
//
// Env vars:
//   LATENCY_SHM_NAME  shm name (default "/game_fps" -> /dev/shm/game_fps)
//   LATENCY_VERBOSE   if set, also append per-second stats to this file path
//
// Ref: https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderLayerInterface.md

#define _GNU_SOURCE           // syscall(), clock_gettime, CLOCK_MONOTONIC

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <time.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/futex.h>

#include "fps_shm.h"

// Length of one measurement window, once a sample has been requested.
#define MEASURE_WINDOW_NS 1000000000ull

// Per-device state: next-layer function pointers + measurement accumulators.
typedef struct DeviceData {
    VkDevice device;
    PFN_vkGetDeviceProcAddr next_gdpa;
    PFN_vkQueuePresentKHR   next_present;

    int      have_last;        // seen a previous present (for frametime diff)
    uint64_t last_present_ns;  // timestamp of the previous present

    int      measuring;        // 1 while accumulating a requested window
    uint64_t window_start_ns;
    uint64_t frame_count;
    uint64_t min_ft_ns;
    uint64_t max_ft_ns;

    struct DeviceData *next;   // linked list for multiple devices
} DeviceData;

static DeviceData *g_devices = NULL;

// Next-layer GetInstanceProcAddr. We intercept no instance functions but must
// forward unknown names downwards, else the app gets NULL and crashes.
static PFN_vkGetInstanceProcAddr g_next_gipa = NULL;

// Every dispatchable handle starts with a pointer to its dispatch table; the
// loader uses it to identify the object. Queues share their device's key.
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
// Shared-memory buffer (request/response). Layout lives in fps_shm.h.
// ---------------------------------------------------------------------------
static FpsShm *g_shm = NULL;   // NULL if shm setup failed -> writes are skipped
static FILE   *g_verbose = NULL;

static void ipc_init(void) {
    const char *name = getenv("LATENCY_SHM_NAME");
    if (!name || !*name)
        name = FPS_SHM_DEFAULT_NAME;

    int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
    if (fd < 0)
        return;
    fchmod(fd, 0666);
    if (ftruncate(fd, sizeof(FpsShm)) != 0) {
        close(fd);
        return;
    }
    void *p = mmap(NULL, sizeof(FpsShm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED)
        return;
    g_shm = (FpsShm *)p;

    const char *vpath = getenv("LATENCY_VERBOSE");
    if (vpath && *vpath)
        g_verbose = fopen(vpath, "a");
}

// Wake the reader parked on FUTEX_WAIT(request). No-op if none is waiting.
static void futex_wake_request(void) {
    syscall(SYS_futex, &g_shm->request, FUTEX_WAKE, INT32_MAX, NULL, NULL, 0);
}

// Write one result, then release it: request = 0 marks "result ready" and, per
// the handshake, guarantees we won't touch the buffer again until the reader
// re-arms. The store-release publishes the field writes before request drops.
static void write_result(double fps, double min_ms, double max_ms,
                         uint64_t frames) {
    g_shm->fps = fps;
    g_shm->min_frametime_ms = min_ms;
    g_shm->max_frametime_ms = max_ms;
    g_shm->frame_count = frames;

    atomic_store_explicit(&g_shm->request, 0, memory_order_release);
    futex_wake_request();

    if (g_verbose) {
        fprintf(g_verbose,
                "[latency] fps=%.1f  min_frametime=%.3f ms  max_frametime=%.3f ms  (frames=%llu)\n",
                fps, min_ms, max_ms, (unsigned long long)frames);
        fflush(g_verbose);
    }
}

// Intercepts each present. Only accumulates while the reader has requested a
// sample (request == 1); otherwise it just tracks the last present time.
static VKAPI_ATTR VkResult VKAPI_CALL
layer_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo) {
    DeviceData *d = find_device_by_key(get_dispatch_key(queue));
    if (!d || !d->next_present)
        return VK_ERROR_INITIALIZATION_FAILED;

    uint64_t now = now_ns();
    uint64_t last = d->last_present_ns;
    int have_last = d->have_last;
    d->last_present_ns = now;
    d->have_last = 1;

    if (g_shm) {
        if (!d->measuring) {
            // Idle: start a window only when the reader asks for one.
            if (atomic_load_explicit(&g_shm->request, memory_order_acquire) == 1) {
                d->measuring = 1;
                reset_window(d, now);
            }
        } else if (have_last) {
            uint64_t ft = now - last;
            if (ft < d->min_ft_ns) d->min_ft_ns = ft;
            if (ft > d->max_ft_ns) d->max_ft_ns = ft;
            d->frame_count++;

            uint64_t elapsed = now - d->window_start_ns;
            if (elapsed >= MEASURE_WINDOW_NS && d->frame_count > 0) {
                double fps = (double)d->frame_count / ((double)elapsed / 1e9);
                write_result(fps,
                             (double)d->min_ft_ns / 1e6,
                             (double)d->max_ft_ns / 1e6,
                             d->frame_count);
                d->measuring = 0;
            }
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

    // fixed pool, linear lookup; up to 16 devices
    static DeviceData storage[16];
    static int used = 0;
    if (used < 16) {
        if (used == 0)
            ipc_init();
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
