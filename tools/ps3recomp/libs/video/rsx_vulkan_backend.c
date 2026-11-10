/*
 * ps3recomp - Vulkan RSX Backend. See rsx_vulkan_backend.h.
 *
 * The file, in order:
 *   loader and device      libvulkan by dlopen, Vulkan 1.0 entry points only;
 *                          device choice, BC textures when the device has them
 *   memory, images, submit helpers
 *   vtable fallback path   the null backend's fixed-function contract:
 *                          position = attribute 0, flat colour = attribute 3
 *                          of each triangle's first vertex, texcoord0 =
 *                          attribute 8, fetched and expanded through the
 *                          shared rsx_fetch_attrib(); unit 0 decoded through
 *                          rsx_texture_decode(); depth test, no blend/stencil
 *   vtable guest programs  the same path running the decompilers' HLSL, with
 *                          whole mip chains and per-unit samplers
 *   optional window        SDL2 + swapchain; a present blits the frame into it
 *   draw engine backend    rsx_draw_backend: targets, clears, present and
 *                          readback (E1), pipelines and draws (E2), textures,
 *                          surface views and samplers (E3), depth snapshots
 *                          (E4) -- the stage names in the commit history
 *   public entry points    init picks the path, shutdown undoes everything
 *
 * Deliberately synchronous: every clear, draw and present is one one-shot
 * command buffer, submitted and waited on. Slow and trivially correct; it is
 * what lets the engine's submit_and_wait and staging reuse cost nothing, and
 * batching is the first thing to change once correctness is settled.
 *
 * Images stay in VK_IMAGE_LAYOUT_GENERAL for their whole life: legal for
 * transfers, sampling and as attachments, so there is no layout bookkeeping.
 *
 * Shaders: libs/video/vulkan/rsx_vk_fallback.{vert,frag}, embedded as SPIR-V.
 * Regenerate after editing them with:
 *   glslangValidator -V --target-env vulkan1.0 --vn rsx_vk_fallback_vert_spv \
 *       -o rsx_vk_fallback_vert.spv.h rsx_vk_fallback.vert
 *   glslangValidator -V --target-env vulkan1.0 --vn rsx_vk_fallback_frag_spv \
 *       -o rsx_vk_fallback_frag.spv.h rsx_vk_fallback.frag
 * then `expand -t 4` both outputs: glslang indents with tabs, the project
 * with spaces.
 */

/* Builds on Linux, Android and Windows (vulkan-1.dll via the loader shim). */

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
static unsigned s_eobj_gen;       /* bumped whenever a backend object handle is (re)assigned */
static int s_vk_foreign_bind;   /* a bind outside vk_eng_draw_batched: its reuse state is stale */
static unsigned long long s_vkc[8];   /* RSX_ENG_STATS: creations per window (image, memory, framebuffer, sampler, view, pipeline, buffer) */

#include "rsx_vulkan_backend.h"
#include "rsx_vertex_fetch.h"
#include "rsx_texture_layout.h"
#include "rsx_vp_decompiler.h"
#include "rsx_fp_decompiler.h"
#include "rsx_shader_spirv.h"
#include "rsx_draw_engine.h"
#include "vulkan/rsx_vk_fallback_vert.spv.h"
#include "vulkan/rsx_vk_fallback_frag.spv.h"
#include "vulkan/rsx_vk_overlay_vert.spv.h"
#include "vulkan/rsx_vk_overlay_frag.spv.h"
#include "../input/touch_controls.h"
#include "rsx_host_ui.h"
#include <math.h>

/* register(t48) -> the cube-array binding (see rsx_fp_cube_array_style). */
#define VK_CUBE_BINDING (RSX_SPIRV_TEXTURE_BINDING + 48u)
static int vk_make_dummy_cube(void);

/* SDL2 is already required off Windows (cellPad, cellAudio); here it only
 * opens the optional window. Same include style as those two. */
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

#ifdef _WIN32
/* The loader is vulkan-1.dll; a tiny dlopen shim keeps one loading path. */
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  define RTLD_NOW 0
#  define RTLD_LOCAL 0
static void* dlopen(const char* name, int flags) { (void)flags; return (void*)LoadLibraryA(name); }
static void* dlsym(void* lib, const char* sym) { return (void*)GetProcAddress((HMODULE)lib, sym); }
static int dlclose(void* lib) { return FreeLibrary((HMODULE)lib) ? 0 : -1; }
static const char* dlerror(void)
{
    static char buf[64];
    snprintf(buf, sizeof buf, "Win32 error %lu", (unsigned long)GetLastError());
    return buf;
}
#else
#  include <dlfcn.h>
#endif
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Run-time loaded entry points (Vulkan 1.0 core only)
 * -------------------------------------------------------------------------*/
#define VK_GLOBAL_FNS(X) X(vkCreateInstance)
#define VK_INSTANCE_FNS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) \
    X(vkGetPhysicalDeviceFeatures) \
    X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties)
#define VK_DEVICE_FNS(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
    X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkInvalidateMappedMemoryRanges) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkQueueSubmit) \
    X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkResetDescriptorPool) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkCreatePipelineCache) X(vkGetPipelineCacheData) X(vkDestroyPipelineCache) X(vkMergePipelineCaches) \
    X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCmdPipelineBarrier) X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) \
    X(vkCmdCopyImageToBuffer) X(vkCmdCopyBufferToImage) X(vkCmdCopyImage) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) \
    X(vkCmdBindVertexBuffers) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdDraw) \
    X(vkCmdSetStencilReference) X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCmdBlitImage) X(vkCmdSetDepthBias) X(vkCmdSetBlendConstants)
/* Window-system entry points, loaded only when a window was requested, so a
 * headless run never depends on a driver that can present. */
#define VK_WSI_INSTANCE_FNS(X) \
    X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define VK_WSI_DEVICE_FNS(X) \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR)

#define VK_DECLARE(name) static PFN_##name p##name;
static PFN_vkGetInstanceProcAddr pvkGetInstanceProcAddr;
VK_GLOBAL_FNS(VK_DECLARE)
VK_INSTANCE_FNS(VK_DECLARE)
VK_DEVICE_FNS(VK_DECLARE)
VK_WSI_INSTANCE_FNS(VK_DECLARE)
VK_WSI_DEVICE_FNS(VK_DECLARE)

/* ---------------------------------------------------------------------------
 * Backend state
 * -------------------------------------------------------------------------*/
typedef struct vk_vertex {
    float pos[4];
    float col[4];
    float uv[2];
} vk_vertex;

typedef struct vk_buffer {
    VkBuffer       buf;
    VkDeviceMemory mem;
    void*          ptr;
    VkDeviceSize   size;
    int            coherent;
} vk_buffer;

typedef struct vk_image {
    VkImage        img;
    VkDeviceMemory mem;           /* a pool block's memory when `sub` is set */
    VkImageView    view;
    u32            sub;           /* piece of the image memory pool, 0 = own allocation */
} vk_image;

#define VK_SAMPLER_CACHE 32
#define VK_PIPELINE_SLOTS (2 * 2 * 8)   /* ztest x zwrite x compare op */

typedef struct vk_state {
    void*            lib;
    VkInstance       instance;
    VkPhysicalDevice phys;
    VkDevice         device;
    VkQueue          queue;
    u32              queue_family;
    VkPhysicalDeviceMemoryProperties memprops;

    u32              width, height;   /* the display image: the guest's x res_scale */
    u32              guest_w, guest_h; /* the display size the guest renders for */
    float            aniso;           /* RACACIT_ANISO, 0 = off              */
    float            max_aniso;       /* the device's limit, 0 = no anisotropy */
    int              trilinear;       /* RACACIT_TRILINEAR=1                 */
    vk_image         color;           /* R8G8B8A8_UNORM                 */
    vk_image         depth;           /* D32_SFLOAT or fallback         */
    VkFormat         depth_format;
    /* The engine's depth targets: depth AND stencil (RSX zeta is Z24S8).
     * A Crack in Time masks its light volumes with two-sided stencil; on a
     * depth-only format Vulkan passes every stencil test, and each volume
     * lit or shadowed its whole screen rectangle -- the box around Ratchet. */
    int              depth_clamp_ok;
    VkFormat         eng_depth_format;
    VkImageAspectFlags eng_depth_aspect;

    VkRenderPass     render_pass;
    VkFramebuffer    framebuffer;
    VkShaderModule   vs, fs;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool desc_pool;
    VkDescriptorSet  desc_set;
    VkPipelineLayout pipe_layout;
    VkPipeline       pipelines[VK_PIPELINE_SLOTS];
    VkSampler        sampler;

    vk_image         dummy;           /* 1x1 magenta: "nothing bound"    */
    vk_image         dummy_cube;      /* 1x1x6 magenta cube, same purpose */
    vk_image         tex[RSX_MAX_TEXTURES];   /* decoded guest textures  */
    int              tex_ready[RSX_MAX_TEXTURES];
    /* Guest path only: each unit's sampler, built from its wrap, filter and
     * LOD registers (the fallback path keeps `sampler`, the null backend's
     * nearest + repeat). Cached by register value; VK_NULL_HANDLE = use
     * `sampler`. */
    VkSampler        tex_sampler[RSX_MAX_TEXTURES];
    struct { u64 key; VkSampler s; } samp_cache[VK_SAMPLER_CACHE];
    u32              samp_count;

    vk_buffer        vertices;        /* expanded triangle list          */
    vk_vertex*       cpu_verts;
    size_t           cpu_cap, cpu_count;

    vk_buffer        readback;
    VkCommandPool    pool;
    VkCommandBuffer  cmd;
    VkFence          fence;

    /* Optional window (PS3RECOMP_VK_WINDOW=1). Rendering stays offscreen
     * either way -- readback and the tests are unchanged -- and a present
     * additionally blits the frame into the window's swapchain. */
    int              eng_active;      /* the register-file engine drives us */
    int              bc_ok;           /* textureCompressionBC enabled       */
    int              windowed;
    SDL_Window*      window;
    const char*      inst_exts[16];
    u32              inst_ext_count;
    VkSurfaceKHR     surface;
    VkSwapchainKHR   swapchain;
    VkExtent2D       sc_extent;
    u32              sc_count;
    VkImage          sc_images[8];
    VkSemaphore      sc_done[8];      /* per image: the present waits on it */
    VkSemaphore      sem_acquire;
    double           hold_seconds;

    const rsx_state* state;           /* live RSX state, for draws       */
    u32              clear_argb;
    u32              last_center;
    u32              presented;
    const char*      dump_path;
} vk_state;

static vk_state s_vk;
static void vk_apply_filter_options(const rsx_host_options* o);

#define VK_LOG(...)  fprintf(stderr, "[RSX vulkan] " __VA_ARGS__)
static void vk_device_lost_report(VkResult r);
#define VK_CHECK(call, what)                                               \
    do { VkResult r_ = (call);                                             \
         if (r_ != VK_SUCCESS) {                                           \
             vk_device_lost_report(r_);                                    \
             VK_LOG("%s failed (VkResult %d)\n", (what), (int)r_);         \
             return -1; } } while (0)

/* ---------------------------------------------------------------------------
 * Loading
 * -------------------------------------------------------------------------*/
static int vk_load_library(void)
{
#ifdef _WIN32
    static const char* names[] = { "vulkan-1.dll" };
#else
    static const char* names[] = { "libvulkan.so.1", "libvulkan.so" };
#endif
    for (size_t i = 0; i < sizeof names / sizeof names[0] && !s_vk.lib; i++)
        s_vk.lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!s_vk.lib) {
        VK_LOG("no Vulkan loader found (dlopen libvulkan.so.1: %s)\n", dlerror());
        return -1;
    }
    pvkGetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)dlsym(s_vk.lib, "vkGetInstanceProcAddr");
    if (!pvkGetInstanceProcAddr) { VK_LOG("libvulkan has no vkGetInstanceProcAddr\n"); return -1; }
#define LOAD_G(name) \
    if (!(p##name = (PFN_##name)pvkGetInstanceProcAddr(VK_NULL_HANDLE, #name))) \
        { VK_LOG("missing global function " #name "\n"); return -1; }
    VK_GLOBAL_FNS(LOAD_G)
    return 0;
}

static int vk_load_instance_functions(void)
{
#define LOAD_I(name) \
    if (!(p##name = (PFN_##name)pvkGetInstanceProcAddr(s_vk.instance, #name))) \
        { VK_LOG("missing instance function " #name "\n"); return -1; }
    VK_INSTANCE_FNS(LOAD_I)
    return 0;
}

static int vk_load_wsi_instance_functions(void)
{
    VK_WSI_INSTANCE_FNS(LOAD_I)
    return 0;
}

static int vk_load_device_functions(void)
{
#define LOAD_D(name) \
    if (!(p##name = (PFN_##name)pvkGetDeviceProcAddr(s_vk.device, #name))) \
        { VK_LOG("missing device function " #name "\n"); return -1; }
    VK_DEVICE_FNS(LOAD_D)
    return 0;
}

static int vk_load_wsi_device_functions(void)
{
    VK_WSI_DEVICE_FNS(LOAD_D)
    return 0;
}

/* VK_EXT_device_fault, when the driver has it: what the GPU tripped on when
 * the device is lost (page fault and its address, watchdog timeout, the
 * vendor's own codes), logged once. Without it a lost device says nothing
 * beyond VK_ERROR_DEVICE_LOST. */
static PFN_vkGetDeviceFaultInfoEXT pvkGetDeviceFaultInfoEXT;
static int s_vk_pcc;   /* VK_EXT_pipeline_creation_cache_control on: vk_eng_variant compiles new pipelines in the background */
static void vk_device_lost_report(VkResult r)
{
    static int done;
    if (r != VK_ERROR_DEVICE_LOST || done) return;
    done = 1;
    VK_LOG("DEVICE LOST -- the GPU stopped this process's work (driver reset)\n");
    if (!pvkGetDeviceFaultInfoEXT || !s_vk.device) { VK_LOG("device fault: no VK_EXT_device_fault, no details\n"); return; }
    VkDeviceFaultCountsEXT c = { .sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT };
    if (pvkGetDeviceFaultInfoEXT(s_vk.device, &c, NULL) != VK_SUCCESS && !c.addressInfoCount && !c.vendorInfoCount) {
        VK_LOG("device fault: no report\n"); return;
    }
    if (c.addressInfoCount > 64) c.addressInfoCount = 64;
    if (c.vendorInfoCount > 64) c.vendorInfoCount = 64;
    c.vendorBinarySize = 0;
    VkDeviceFaultAddressInfoEXT ai[64]; VkDeviceFaultVendorInfoEXT vi[64];
    memset(ai, 0, sizeof ai); memset(vi, 0, sizeof vi);
    VkDeviceFaultInfoEXT fi = { .sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT,
                                .pAddressInfos = c.addressInfoCount ? ai : NULL,
                                .pVendorInfos = c.vendorInfoCount ? vi : NULL };
    const VkResult fr = pvkGetDeviceFaultInfoEXT(s_vk.device, &c, &fi);
    /* Refused: nothing in fi/ai/vi is meaningful (the reporter must not be
     * what crashes during a device loss). */
    if (fr != VK_SUCCESS && fr != VK_INCOMPLETE) { VK_LOG("device fault: query failed (VkResult %d)\n", (int)fr); return; }
    fi.description[VK_MAX_DESCRIPTION_SIZE - 1] = 0;
    for (u32 i = 0; i < c.vendorInfoCount; i++) vi[i].description[VK_MAX_DESCRIPTION_SIZE - 1] = 0;
    VK_LOG("device fault (VkResult %d): \"%s\", %u address(es), %u vendor record(s)\n",
           (int)fr, fi.description, c.addressInfoCount, c.vendorInfoCount);
    static const char* kind[] = { "none", "read (invalid)", "write (invalid)", "execute (invalid)",
                                  "instruction pointer unknown", "instruction pointer invalid",
                                  "instruction pointer fault" };
    for (u32 i = 0; i < c.addressInfoCount; i++)
        VK_LOG("device fault: %s at 0x%llX (+/- 0x%llX)\n",
               (u32)ai[i].addressType < 7u ? kind[ai[i].addressType] : "?",
               (unsigned long long)ai[i].reportedAddress, (unsigned long long)ai[i].addressPrecision);
    for (u32 i = 0; i < c.vendorInfoCount; i++)
        VK_LOG("device fault: vendor \"%s\" code 0x%llX data 0x%llX\n", vi[i].description,
               (unsigned long long)vi[i].vendorFaultCode, (unsigned long long)vi[i].vendorFaultData);
}

/* ---------------------------------------------------------------------------
 * Device selection: PS3RECOMP_VK_DEVICE if set, else the first discrete GPU,
 * else integrated, else anything that is not a CPU rasteriser, else whatever
 * is there (llvmpipe/lavapipe on a GPU-less CI runner).
 * -------------------------------------------------------------------------*/
static int vk_device_rank(VkPhysicalDeviceType t)
{
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return 0;
    default:                                     return 1;
    }
}

static int vk_find_graphics_queue(VkPhysicalDevice pd, u32* family)
{
    u32 n = 0;
    pvkGetPhysicalDeviceQueueFamilyProperties(pd, &n, NULL);
    if (n == 0 || n > 64) return -1;
    VkQueueFamilyProperties props[64];
    pvkGetPhysicalDeviceQueueFamilyProperties(pd, &n, props);
    for (u32 i = 0; i < n; i++) {
        if (!(props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        if (s_vk.windowed) {           /* the same queue must also present */
            VkBool32 ok = VK_FALSE;
            if (pvkGetPhysicalDeviceSurfaceSupportKHR(pd, i, s_vk.surface, &ok) != VK_SUCCESS || !ok)
                continue;
        }
        *family = i; return 0;
    }
    return -1;
}

static int vk_has_device_extension(VkPhysicalDevice pd, const char* name)
{
    u32 n = 0;
    if (pvkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL) != VK_SUCCESS || n == 0) return 0;
    VkExtensionProperties* e = (VkExtensionProperties*)malloc(n * sizeof *e);
    if (!e) return 0;
    int found = 0;
    if (pvkEnumerateDeviceExtensionProperties(pd, NULL, &n, e) == VK_SUCCESS)
        for (u32 i = 0; i < n && !found; i++) found = strcmp(e[i].extensionName, name) == 0;
    free(e);
    return found;
}

static int vk_pick_device(void)
{
    u32 n = 0;
    VK_CHECK(pvkEnumeratePhysicalDevices(s_vk.instance, &n, NULL), "vkEnumeratePhysicalDevices");
    if (n == 0) { VK_LOG("no Vulkan physical devices\n"); return -1; }
    if (n > 16) n = 16;
    VkPhysicalDevice devs[16];
    VK_CHECK(pvkEnumeratePhysicalDevices(s_vk.instance, &n, devs), "vkEnumeratePhysicalDevices");

    int best = -1, best_rank = -1;
    const char* forced = getenv("PS3RECOMP_VK_DEVICE");
    for (u32 i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        pvkGetPhysicalDeviceProperties(devs[i], &p);
        u32 fam;
        int usable = vk_find_graphics_queue(devs[i], &fam) == 0 &&
                     (!s_vk.windowed ||
                      vk_has_device_extension(devs[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME));
        VK_LOG("device %u: %s (type %d, api %u.%u.%u)%s\n", i, p.deviceName,
               (int)p.deviceType, VK_API_VERSION_MAJOR(p.apiVersion),
               VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion),
               usable ? "" : (s_vk.windowed ? " -- cannot present to the window"
                                            : " -- no graphics queue"));
        if (!usable) continue;
        if (forced) {
            if ((u32)atoi(forced) == i) { best = (int)i; break; }
            continue;
        }
        int rank = vk_device_rank(p.deviceType);
        if (rank > best_rank) { best = (int)i; best_rank = rank; }
    }
    if (best < 0) {
        if (forced) VK_LOG("PS3RECOMP_VK_DEVICE=%s is not a usable device\n", forced);
        else        VK_LOG("no device with a graphics queue\n");
        return -1;
    }
    s_vk.phys = devs[best];
    vk_find_graphics_queue(s_vk.phys, &s_vk.queue_family);
    pvkGetPhysicalDeviceMemoryProperties(s_vk.phys, &s_vk.memprops);
    VkPhysicalDeviceProperties p;
    pvkGetPhysicalDeviceProperties(s_vk.phys, &p);
    VK_LOG("using device %d: %s\n", best, p.deviceName);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Memory, images, buffers
 * -------------------------------------------------------------------------*/
static int vk_find_memory_type(u32 type_bits, VkMemoryPropertyFlags want, u32* out)
{
    for (u32 i = 0; i < s_vk.memprops.memoryTypeCount; i++)
        if ((type_bits & (1u << i)) &&
            (s_vk.memprops.memoryTypes[i].propertyFlags & want) == want) {
            *out = i; return 0;
        }
    return -1;
}

/* Device memory allocations alive (Windows drivers allow 4096 at once):
 * every vkAllocateMemory / vkFreeMemory goes through these two. */
static volatile long s_live_allocs;
static VkResult vk_alloc_mem(const VkMemoryAllocateInfo* mai, VkDeviceMemory* m)
{
    const VkResult r = pvkAllocateMemory(s_vk.device, mai, NULL, m);
    if (r == VK_SUCCESS) __atomic_add_fetch(&s_live_allocs, 1, __ATOMIC_RELAXED);
    return r;
}
static void vk_free_mem(VkDeviceMemory m)
{
    if (!m) return;
    pvkFreeMemory(s_vk.device, m, NULL);
    __atomic_sub_fetch(&s_live_allocs, 1, __ATOMIC_RELAXED);
}

/* ---------------------------------------------------------------------------
 * Image memory pool.
 *
 * Every image used to get a vkAllocateMemory of its own. The texture cache
 * alone holds thousands of images (a level streams new ones all the time),
 * Windows drivers allow 4096 allocations in all (maxMemoryAllocationCount),
 * so the texture count had to stay low and evict constantly, and each
 * allocation costs the driver far more than carving a piece out of a block.
 * Images of up to RSX_VK_POOL_KB (4096) now take a piece of a 64 MB block,
 * split buddy-style into power-of-two pieces of 4 KB and up; a piece is
 * aligned to its own size, so any alignment the image asks for up to that
 * holds. Only optimal-tiling images live in the blocks, so
 * bufferImageGranularity never applies. A block left empty goes back to the
 * driver when another empty block of its memory type is kept already.
 * RSX_VK_POOL=0: an allocation per image, as before.
 * -------------------------------------------------------------------------*/
#define VK_POOL_UNIT_LOG2  12u
#define VK_POOL_BLOCK_LOG2 26u
#define VK_POOL_ORDERS     (VK_POOL_BLOCK_LOG2 - VK_POOL_UNIT_LOG2 + 1u)     /* 4 KB .. 64 MB */
#define VK_POOL_UNITS      (1u << (VK_POOL_BLOCK_LOG2 - VK_POOL_UNIT_LOG2))  /* 4 KB units in a block */
#define VK_POOL_MAX_BLOCKS 255u
typedef struct {
    VkDeviceMemory mem;
    u32  type, used_units;
    u32  nfree[VK_POOL_ORDERS];                /* free pieces of each order */
    u64  bits[2u * VK_POOL_UNITS / 64u];       /* free-piece bitmap, order k from bit 2U - (2U >> k) */
    u8   order_at[VK_POOL_UNITS];              /* order + 1 of the piece in use at each unit */
} vk_pool_blk;
static vk_pool_blk* s_pool[VK_POOL_MAX_BLOCKS];
static u32 s_pool_n, s_pool_blocks, s_pool_pieces;
static long s_pool_max = -1;                   /* largest pooled image, bytes; 0 = off */
static SDL_SpinLock s_pool_lock;

static inline u32 vk_pool_bit(u32 k, u32 s) { return 2u * VK_POOL_UNITS - ((2u * VK_POOL_UNITS) >> k) + s; }
static inline int vk_pool_isfree(const vk_pool_blk* b, u32 k, u32 s)
{
    const u32 i = vk_pool_bit(k, s);
    return (int)((b->bits[i >> 6] >> (i & 63u)) & 1u);
}
static inline void vk_pool_set(vk_pool_blk* b, u32 k, u32 s, int on)
{
    const u32 i = vk_pool_bit(k, s);
    if (on) b->bits[i >> 6] |= 1ull << (i & 63u); else b->bits[i >> 6] &= ~(1ull << (i & 63u));
}

/* A piece of order k out of block b: its first unit, or -1. */
static int vk_pool_blk_alloc(vk_pool_blk* b, u32 k)
{
    u32 o = k;
    while (o < VK_POOL_ORDERS && !b->nfree[o]) o++;
    if (o >= VK_POOL_ORDERS) return -1;
    const u32 n = VK_POOL_UNITS >> o;
    int s = -1;
    for (u32 i = 0; i < n && s < 0; ) {
        const u32 bit = vk_pool_bit(o, i), sh = bit & 63u, run = 64u - sh < n - i ? 64u - sh : n - i;
        u64 w = b->bits[bit >> 6] >> sh;
        if (run < 64u) w &= (1ull << run) - 1u;
        if (w) s = (int)(i + (u32)__builtin_ctzll(w));
        i += run;
    }
    if (s < 0) return -1;
    vk_pool_set(b, o, (u32)s, 0); b->nfree[o]--;
    while (o > k) { o--; s <<= 1; vk_pool_set(b, o, (u32)s + 1u, 1); b->nfree[o]++; }
    const u32 unit = (u32)s << k;
    b->order_at[unit] = (u8)(k + 1u);
    b->used_units += 1u << k;
    return (int)unit;
}

static void vk_pool_blk_free(vk_pool_blk* b, u32 unit)
{
    u32 k = b->order_at[unit] - 1u;
    b->order_at[unit] = 0;
    b->used_units -= 1u << k;
    u32 s = unit >> k;
    while (k + 1u < VK_POOL_ORDERS && vk_pool_isfree(b, k, s ^ 1u)) {
        vk_pool_set(b, k, s ^ 1u, 0); b->nfree[k]--;
        s >>= 1; k++;
    }
    vk_pool_set(b, k, s, 1); b->nfree[k]++;
}

/* Give back a piece (vk_image.sub). */
static void vk_pool_free(u32 sub)
{
    const u32 bi = (sub >> 16) - 1u, unit = sub & 0xFFFFu;
    SDL_AtomicLock(&s_pool_lock);
    vk_pool_blk* b = bi < VK_POOL_MAX_BLOCKS ? s_pool[bi] : NULL;
    if (b && unit < VK_POOL_UNITS && b->order_at[unit]) {
        vk_pool_blk_free(b, unit);
        s_pool_pieces--;
        if (!b->used_units)
            for (u32 j = 0; j < s_pool_n; j++)
                if (j != bi && s_pool[j] && s_pool[j]->type == b->type && !s_pool[j]->used_units) {
                    vk_free_mem(b->mem);
                    free(b); s_pool[bi] = NULL; s_pool_blocks--;
                    break;
                }
    }
    SDL_AtomicUnlock(&s_pool_lock);
}

/* A piece of `bytes` aligned to `align` from memory type `type`: its block's
 * memory and offset; 0 when the pool cannot serve it. */
static u32 vk_pool_alloc(u32 type, VkDeviceSize bytes, VkDeviceSize align, VkDeviceMemory* mem, VkDeviceSize* off)
{
    VkDeviceSize piece = (VkDeviceSize)1u << VK_POOL_UNIT_LOG2;
    u32 k = 0;
    while (piece < bytes || piece < align) { piece <<= 1; k++; }
    if (k >= VK_POOL_ORDERS) return 0;
    u32 sub = 0;
    SDL_AtomicLock(&s_pool_lock);
    for (u32 j = 0; j < s_pool_n && !sub; j++) {
        vk_pool_blk* b = s_pool[j];
        if (!b || b->type != type) continue;
        const int u = vk_pool_blk_alloc(b, k);
        if (u >= 0) { sub = ((j + 1u) << 16) | (u32)u; *mem = b->mem; *off = (VkDeviceSize)u << VK_POOL_UNIT_LOG2; }
    }
    if (!sub) {
        u32 j = 0;
        while (j < VK_POOL_MAX_BLOCKS && s_pool[j]) j++;
        vk_pool_blk* b = j < VK_POOL_MAX_BLOCKS ? (vk_pool_blk*)calloc(1, sizeof *b) : NULL;
        if (b) {
            VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                         .allocationSize = (VkDeviceSize)1u << VK_POOL_BLOCK_LOG2, .memoryTypeIndex = type };
            if ((s_vkc[1]++, vk_alloc_mem(&mai, &b->mem)) == VK_SUCCESS) {
                b->type = type;
                b->nfree[VK_POOL_ORDERS - 1u] = 1;
                vk_pool_set(b, VK_POOL_ORDERS - 1u, 0, 1);
                s_pool[j] = b;
                if (j >= s_pool_n) s_pool_n = j + 1u;
                s_pool_blocks++;
                const int u = vk_pool_blk_alloc(b, k);
                if (u >= 0) { sub = ((j + 1u) << 16) | (u32)u; *mem = b->mem; *off = (VkDeviceSize)u << VK_POOL_UNIT_LOG2; }
                static u32 logs;
                if (logs < 64u) { logs++;
                    VK_LOG("image pool: %u blocks of %u MB (memory type %u), %u images in them\n",
                           s_pool_blocks, 1u << (VK_POOL_BLOCK_LOG2 - 20u), type, s_pool_pieces + (sub ? 1u : 0u)); }
            } else {
                free(b);
            }
        }
    }
    if (sub) s_pool_pieces++;
    SDL_AtomicUnlock(&s_pool_lock);
    return sub;
}

static void vk_pool_shutdown(void)
{
    for (u32 j = 0; j < s_pool_n; j++)
        if (s_pool[j]) { vk_free_mem(s_pool[j]->mem); free(s_pool[j]); s_pool[j] = NULL; }
    s_pool_n = s_pool_blocks = s_pool_pieces = 0;
}

/* For the [mem] log line (game/main.cpp): the image pool's bytes in use, its
 * blocks' total and their count. A block goes back to the driver only when
 * nothing in it is live, so blocks that keep being added while `live` stays
 * well below the total means pieces scattered over them (fragmentation). */
void rsx_vk_pool_stats(unsigned long long* live, unsigned long long* cap, unsigned* blocks)
{
    unsigned long long l = 0; unsigned n = 0;
    SDL_AtomicLock(&s_pool_lock);
    for (u32 j = 0; j < s_pool_n; j++)
        if (s_pool[j]) { l += (unsigned long long)s_pool[j]->used_units << VK_POOL_UNIT_LOG2; n++; }
    SDL_AtomicUnlock(&s_pool_lock);
    if (live) *live = l;
    if (cap) *cap = (unsigned long long)n << VK_POOL_BLOCK_LOG2;
    if (blocks) *blocks = n;
}

#ifdef _WIN32
#define COBJMACROS
#include <dxgi1_4.h>
/* This process's use of the GPU's own memory and the budget Windows gives it
 * (DXGI 1.4, on the adapter with the most dedicated memory), in MB; -1 when
 * unknown. The owner's v60 Axiom fight ran out (50 failed image allocations
 * at 2.0x), with no way to tell from the log how full it was. */
static int vk_vram_mb(unsigned* used, unsigned* budget)
{
    static IDXGIAdapter3* ad; static int tried;
    if (!tried) {
        tried = 1;
        static const GUID iid_f1 = { 0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };
        static const GUID iid_a3 = { 0x645967a4, 0x1392, 0x4310, { 0xa7, 0x98, 0x80, 0x53, 0xce, 0x3e, 0x93, 0xfd } };
        typedef HRESULT (WINAPI *cf_t)(REFIID, void**);
        HMODULE m = LoadLibraryA("dxgi.dll");
        cf_t cf = m ? (cf_t)(void*)GetProcAddress(m, "CreateDXGIFactory1") : NULL;
        IDXGIFactory1* f = NULL;
        if (cf && SUCCEEDED(cf(&iid_f1, (void**)&f))) {
            IDXGIAdapter1* a; SIZE_T best = 0;
            for (UINT i = 0; IDXGIFactory1_EnumAdapters1(f, i, &a) == S_OK; i++) {
                DXGI_ADAPTER_DESC1 d; IDXGIAdapter3* a3 = NULL;
                if (SUCCEEDED(IDXGIAdapter1_GetDesc1(a, &d)) && d.DedicatedVideoMemory > best &&
                    SUCCEEDED(IDXGIAdapter1_QueryInterface(a, &iid_a3, (void**)&a3))) {
                    if (ad) IDXGIAdapter3_Release(ad);
                    ad = a3; best = d.DedicatedVideoMemory;
                }
                IDXGIAdapter1_Release(a);
            }
            IDXGIFactory1_Release(f);
        }
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO mi;
    if (!ad || FAILED(IDXGIAdapter3_QueryVideoMemoryInfo(ad, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi))) return -1;
    *used = (unsigned)(mi.CurrentUsage >> 20); *budget = (unsigned)(mi.Budget >> 20);
    return 0;
}
#else
static int vk_vram_mb(unsigned* used, unsigned* budget) { (void)used; (void)budget; return -1; }
#endif
static void vk_bt_reclaim(void);

/* Memory for a created image: a pool piece, else an allocation of its own;
 * bound, with im->mem and im->sub filled in. */
extern volatile int g_rsx_vram_pressure;   /* rsx_draw_engine.c: evict at the next budget pass */
static int vk_image_memory(vk_image* im)
{
    VkMemoryRequirements req;
    pvkGetImageMemoryRequirements(s_vk.device, im->img, &req);
    u32 type;
    if (vk_find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        vk_find_memory_type(req.memoryTypeBits, 0, &type)) {
        VK_LOG("no memory type for an image\n"); return -1;
    }
    if (s_pool_max < 0) {
        const char* e = getenv("RSX_VK_POOL"); const char* kb = getenv("RSX_VK_POOL_KB");
        const long v = kb ? atol(kb) : 0;
        s_pool_max = e && *e == '0' ? 0 : (v > 0 ? v : 4096) << 10;
        if (s_pool_max > (1l << VK_POOL_BLOCK_LOG2)) s_pool_max = 1l << VK_POOL_BLOCK_LOG2;
    }
    im->sub = 0;
    if (s_pool_max && req.size <= (VkDeviceSize)s_pool_max) {
        VkDeviceMemory mem; VkDeviceSize off;
        const u32 sub = vk_pool_alloc(type, req.size, req.alignment, &mem, &off);
        if (sub) {
            if (pvkBindImageMemory(s_vk.device, im->img, mem, off) == VK_SUCCESS) { im->mem = mem; im->sub = sub; return 0; }
            vk_pool_free(sub);
        }
    }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    VkResult ar = (s_vkc[1]++, vk_alloc_mem(&mai, &im->mem));
    if (ar != VK_SUCCESS) {
        /* Out of memory: let the GPU finish the submitted batches, which frees
         * the images released meanwhile (deferred until then), and try once
         * more before the target or texture goes missing. */
        const VkResult first = ar;
        { extern volatile int g_rsx_vram_tight; g_rsx_vram_tight = 1; }
        { extern void vk_rbin_flush(void); vk_rbin_flush(); }   /* parked render targets first */
        vk_bt_reclaim();
        ar = (s_vkc[1]++, vk_alloc_mem(&mai, &im->mem));
        static unsigned logs;
        if (logs < 64u || ar != VK_SUCCESS) {
            logs++;
            unsigned u = 0, b = 0;
            if (vk_vram_mb(&u, &b)) u = b = 0;
            VK_LOG("vkAllocateMemory(image, %llu KB) failed (VkResult %d)%s; VRAM %u MB used of %u MB budget, %u pool blocks, %ld allocations alive\n",
                   (unsigned long long)(req.size >> 10), (int)first,
                   ar == VK_SUCCESS ? ", fine once the GPU caught up" : ", again after the GPU caught up", u, b, s_pool_blocks, (long)s_live_allocs);
        }
        if (ar != VK_SUCCESS) { im->mem = VK_NULL_HANDLE; g_rsx_vram_pressure = 1; return -1; }
    }
    if (pvkBindImageMemory(s_vk.device, im->img, im->mem, 0) != VK_SUCCESS) {
        VK_LOG("vkBindImageMemory failed\n"); return -1;
    }
    return 0;
}

static int vk_create_image_levels(vk_image* im, VkFormat fmt, u32 w, u32 h, u32 levels,
                                  VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    if (!levels) levels = 1;
    im->sub = 0; im->mem = VK_NULL_HANDLE;
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt, .extent = { w, h, 1 }, .mipLevels = levels, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VK_CHECK((s_vkc[0]++, pvkCreateImage(s_vk.device, &ici, NULL, &im->img)), "vkCreateImage");
    if (vk_image_memory(im)) return -1;
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = { aspect, 0, levels, 0, 1 },
    };
    VK_CHECK((s_vkc[4]++, pvkCreateImageView(s_vk.device, &vci, NULL, &im->view)), "vkCreateImageView");
    return 0;
}

/* RSX_ENG_STATS: images created, by purpose (colour target, depth target,
 * texture, colour snapshot, depth snapshot). */
static unsigned long long s_img_why[5];
static int vk_create_image(vk_image* im, VkFormat fmt, u32 w, u32 h,
                           VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    return vk_create_image_levels(im, fmt, w, h, 1, usage, aspect);
}

/* ---- Render-target recycle bin ---------------------------------------------
 * A Crack in Time re-packs its shadow-map pool as casters come and go, and
 * every new (address, size) is a new depth target: the owner's 9 minutes of
 * Great Clock D created and dropped 4,250 of them, each a vkCreateImage, a
 * pool allocation, a view and a framebuffer, at the moment the pass needs it.
 * A released colour or depth target's image is parked here instead, and a
 * new target of the same format and size takes it back: no allocation, its
 * view and memory intact. The create path clears it as it would a new one,
 * after a full barrier, so whatever the GPU still had queued on it runs
 * first. FIFO, 64 images / 256 MB; emptied before any failed allocation
 * retries. RSX_VK_RECYCLE=0 turns it off. */
static void vk_destroy_image(vk_image* im);
enum { VK_RBIN_MAX = 64 };
static struct { vk_image im; VkFormat fmt; u32 w, h, kind; u64 bytes; } s_rbin[VK_RBIN_MAX];
static u32 s_rbin_n;
static u64 s_rbin_bytes;
static unsigned long long s_rbin_hits, s_rbin_parked;
static int vk_rbin_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("RSX_VK_RECYCLE"); on = !(e && *e == '0'); }
    return on;
}
void vk_rbin_flush(void)
{
    while (s_rbin_n) vk_destroy_image(&s_rbin[--s_rbin_n].im);
    s_rbin_bytes = 0;
}
/* Takes a parked image of this kind, format and size into *im: 1 if found. */
static int vk_rbin_take(u32 kind, VkFormat fmt, u32 w, u32 h, vk_image* im)
{
    for (u32 i = s_rbin_n; i-- > 0;)
        if (s_rbin[i].kind == kind && s_rbin[i].fmt == fmt && s_rbin[i].w == w && s_rbin[i].h == h) {
            *im = s_rbin[i].im;
            s_rbin_bytes -= s_rbin[i].bytes;
            memmove(&s_rbin[i], &s_rbin[i + 1], (size_t)(s_rbin_n - i - 1u) * sizeof s_rbin[0]);
            s_rbin_n--;
            s_rbin_hits++;
            return 1;
        }
    return 0;
}
/* Parks *im (left empty): 1 if parked, 0 if the caller destroys it. */
static int vk_rbin_park(u32 kind, VkFormat fmt, u32 w, u32 h, u32 bpp, vk_image* im)
{
    const u64 bytes = (u64)w * h * (bpp ? bpp : 4u);
    if (!vk_rbin_on() || !im->img || bytes > (256ull << 20)) return 0;
    while (s_rbin_n && (s_rbin_n == VK_RBIN_MAX || s_rbin_bytes + bytes > (256ull << 20))) {
        s_rbin_bytes -= s_rbin[0].bytes;
        vk_destroy_image(&s_rbin[0].im);
        memmove(&s_rbin[0], &s_rbin[1], (size_t)(s_rbin_n - 1u) * sizeof s_rbin[0]);
        s_rbin_n--;
    }
    s_rbin[s_rbin_n].im = *im; s_rbin[s_rbin_n].fmt = fmt;
    s_rbin[s_rbin_n].w = w; s_rbin[s_rbin_n].h = h; s_rbin[s_rbin_n].kind = kind; s_rbin[s_rbin_n].bytes = bytes;
    s_rbin_n++; s_rbin_bytes += bytes; s_rbin_parked++;
    memset(im, 0, sizeof *im);
    return 1;
}

static int vk_bt_defer_image(const vk_image* im);
static int vk_bt_defer_buffer(const void* b);
static void vk_destroy_image(vk_image* im)
{
    if (vk_bt_defer_image(im)) { memset(im, 0, sizeof *im); return; }
    if (im->view) pvkDestroyImageView(s_vk.device, im->view, NULL);
    if (im->img)  pvkDestroyImage(s_vk.device, im->img, NULL);
    if (im->sub) vk_pool_free(im->sub);
    else if (im->mem) vk_free_mem(im->mem);
    memset(im, 0, sizeof *im);
}

/* Failure-only context: static heap capacity is not the available budget.
 * __func__/__LINE__ identify the direct allocator caller without changing
 * allocation policy, successful-path logging, or partial-object ownership. */
static void vk_buffer_alloc_failure(const char* kind, const char* caller, u32 line,
                                    VkDeviceSize requested, VkBufferUsageFlags usage,
                                    const VkMemoryRequirements* req, u32 type,
                                    VkResult result);

static int vk_create_host_buffer_impl(vk_buffer* b, VkDeviceSize size, VkBufferUsageFlags usage,
                                      const char* caller, u32 line)
{
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = size, .usage = usage,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VK_CHECK((s_vkc[6]++, pvkCreateBuffer(s_vk.device, &bci, NULL, &b->buf)), "vkCreateBuffer");
    VkMemoryRequirements req;
    pvkGetBufferMemoryRequirements(s_vk.device, b->buf, &req);
    u32 type;
    const VkMemoryPropertyFlags vis = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    const VkMemoryPropertyFlags coh = vis | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkMemoryPropertyFlags cached = vis | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    /* A readback (the GPU writes, the CPU reads): CPU-cached memory. Read
     * through an uncached mapping a 1280x720 frame dump took ~200 ms. */
    if (usage == VK_BUFFER_USAGE_TRANSFER_DST_BIT &&
        vk_find_memory_type(req.memoryTypeBits, cached | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &type) == 0) b->coherent = 1;
    else if (usage == VK_BUFFER_USAGE_TRANSFER_DST_BIT &&
        vk_find_memory_type(req.memoryTypeBits, cached, &type) == 0) b->coherent = 0;
    else if (vk_find_memory_type(req.memoryTypeBits, coh, &type) == 0) b->coherent = 1;
    else if (vk_find_memory_type(req.memoryTypeBits, vis, &type) == 0) b->coherent = 0;
    else { VK_LOG("no host-visible memory for a buffer\n"); return -1; }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    { const VkResult r_ = (s_vkc[1]++, vk_alloc_mem(&mai, &b->mem));
      if (r_ != VK_SUCCESS) {
          vk_buffer_alloc_failure("host", caller, line, size, usage, &req, type, r_);
          vk_device_lost_report(r_);
          VK_LOG("vkAllocateMemory(buffer) failed (VkResult %d): %llu bytes, usage 0x%X, memory type %u\n",
                 (int)r_, (unsigned long long)req.size, (unsigned)usage, type);
          b->mem = VK_NULL_HANDLE; return -1; } }
    VK_CHECK(pvkBindBufferMemory(s_vk.device, b->buf, b->mem, 0), "vkBindBufferMemory");
    VK_CHECK(pvkMapMemory(s_vk.device, b->mem, 0, VK_WHOLE_SIZE, 0, &b->ptr), "vkMapMemory");
    b->size = size;
    return 0;
}

/* A GPU-only buffer (no mapping): scratch space for copies between images. */
static int vk_create_device_buffer_impl(vk_buffer* b, VkDeviceSize size, VkBufferUsageFlags usage,
                                      const char* caller, u32 line)
{
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = size, .usage = usage,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VK_CHECK((s_vkc[6]++, pvkCreateBuffer(s_vk.device, &bci, NULL, &b->buf)), "vkCreateBuffer");
    VkMemoryRequirements req;
    pvkGetBufferMemoryRequirements(s_vk.device, b->buf, &req);
    u32 type;
    if (vk_find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        vk_find_memory_type(req.memoryTypeBits, 0, &type)) { VK_LOG("no memory for a device buffer%c", 10); return -1; }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    const VkResult alloc_result = (s_vkc[1]++, vk_alloc_mem(&mai, &b->mem));
    if (alloc_result != VK_SUCCESS)
        vk_buffer_alloc_failure("device", caller, line, size, usage, &req, type, alloc_result);
    VK_CHECK(alloc_result, "vkAllocateMemory(device buffer)");
    VK_CHECK(pvkBindBufferMemory(s_vk.device, b->buf, b->mem, 0), "vkBindBufferMemory");
    b->size = size;
    return 0;
}

/* Evaluate each argument once; record the allocating caller, not this helper. */
#define vk_create_host_buffer(b, size, usage) \
    vk_create_host_buffer_impl((b), (size), (usage), __func__, __LINE__)
#define vk_create_device_buffer(b, size, usage) \
    vk_create_device_buffer_impl((b), (size), (usage), __func__, __LINE__)

static void vk_destroy_buffer(vk_buffer* b)
{
    if (vk_bt_defer_buffer(b)) { memset(b, 0, sizeof *b); return; }
    if (b->ptr) pvkUnmapMemory(s_vk.device, b->mem);
    if (b->buf) pvkDestroyBuffer(s_vk.device, b->buf, NULL);
    if (b->mem) vk_free_mem(b->mem);
    memset(b, 0, sizeof *b);
}

/* Host writes to non-coherent memory need a flush; there is no
 * vkFlushMappedMemoryRanges loaded, so writes only ever go to coherent
 * buffers (every desktop/L4T driver exposes one). Reads invalidate. */
static int vk_invalidate(vk_buffer* b)
{
    if (b->coherent) return 0;
    VkMappedMemoryRange r = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                              .memory = b->mem, .offset = 0, .size = VK_WHOLE_SIZE };
    return pvkInvalidateMappedMemoryRanges(s_vk.device, 1, &r) == VK_SUCCESS ? 0 : -1;
}

/* ---------------------------------------------------------------------------
 * One-shot command submission
 * -------------------------------------------------------------------------*/
/* ---------------------------------------------------------------------------
 * Batched submission
 *
 * Every operation used to be its own one-shot command buffer, submitted and
 * waited on: trivially correct, and fatal on a phone, where thousands of
 * submit+wait round trips a frame (and a render pass begun and ended around
 * every draw, i.e. a full attachment load/store per draw on a tiler) cost
 * seconds per frame. Operations now record into one command buffer per batch
 * from a ring of three; a batch is submitted at present, when its arena or
 * descriptor pool fills, or when the CPU needs a result (vk_submit_and_wait,
 * which keeps its old wait-for-everything meaning). Consecutive draws into the
 * same framebuffer share one render pass. Per-draw data (vertices, indices,
 * constants) comes from the batch's host-visible arena and each draw gets its
 * own descriptor set, so nothing a recorded draw reads is overwritten before
 * it runs. Objects destroyed while a batch may still use them are parked on
 * that batch's garbage list until its fence signals.
 * -------------------------------------------------------------------------*/
#define VK_BT_SLOTS     3
#define VK_BT_ARENA     (48u << 20)
#define VK_BT_SETS      4096u
typedef struct { int kind; uint64_t a, b, c; } vk_garbage;   /* 1 buffer 2 image 3 framebuffer 5 geometry block 8 pooled image */
static void vk_geo_free_now(u32 h);
typedef struct {
    VkCommandBuffer cmd;
    VkFence         fence;
    int             submitted;
    vk_buffer       arena;
    VkDeviceSize    used;
    VkDescriptorPool pool;
    vk_garbage*     gb;
    u32             ngb, capgb;
} vk_bt_slot;
static struct {
    vk_bt_slot      s[VK_BT_SLOTS];
    u32             cur;
    int             ready, recording, in_pass, shutting_down;
    VkRenderPass    rp;
    VkFramebuffer   fb;
    u32             w, h;
    VkCommandBuffer sync_cmd;
    VkDeviceSize    ubo_align;
    u32             epoch;       /* bumped per command buffer: what it binds is its own */
} s_bt;
#ifdef __ANDROID__
static u32 s_bt_draws;           /* draws in the open batch (vk_eng_draw_batched's early submit) */
#endif

/* No Vulkan query or allocation here: report cached selection/arena state only. */
static void vk_buffer_alloc_failure(const char* kind, const char* caller, u32 line,
                                    VkDeviceSize requested, VkBufferUsageFlags usage,
                                    const VkMemoryRequirements* req, u32 type,
                                    VkResult result)
{
    const int type_known = type < s_vk.memprops.memoryTypeCount;
    const u32 heap = type_known ? s_vk.memprops.memoryTypes[type].heapIndex : UINT32_MAX;
    const int heap_known = heap < s_vk.memprops.memoryHeapCount;
    const VkMemoryPropertyFlags props = type_known ? s_vk.memprops.memoryTypes[type].propertyFlags : 0;
    const VkDeviceSize capacity = heap_known ? s_vk.memprops.memoryHeaps[heap].size : 0;
    const vk_bt_slot* slot = s_bt.cur < VK_BT_SLOTS ? &s_bt.s[s_bt.cur] : NULL;
    VK_LOG("buffer_alloc_fail kind=%s caller=%s line=%u result=%d requested=%llu allocation=%llu alignment=%llu "
           "type_bits=0x%08X usage=0x%08X type=%u props=0x%08X heap=%u heap_known=%d heap_capacity=%llu "
           "presented=%u batch_ready=%d recording=%d slot=%u slot_submitted=%d arena_used=%llu arena_capacity=%llu\n",
           kind, caller, line, (int)result, (unsigned long long)requested,
           (unsigned long long)req->size, (unsigned long long)req->alignment,
           req->memoryTypeBits, usage, type, props, heap, heap_known, (unsigned long long)capacity,
           s_vk.presented, s_bt.ready, s_bt.recording, s_bt.cur, slot ? slot->submitted : 0,
           (unsigned long long)(slot ? slot->used : 0),
           (unsigned long long)(slot ? slot->arena.size : 0));
}

static void vk_bt_free_garbage(vk_bt_slot* sl)
{
    for (u32 i = 0; i < sl->ngb; i++) {
        vk_garbage* g = &sl->gb[i];
        if (g->kind == 1) {
            if (g->c) pvkUnmapMemory(s_vk.device, (VkDeviceMemory)(uintptr_t)g->b);
            if (g->a) pvkDestroyBuffer(s_vk.device, (VkBuffer)(uintptr_t)g->a, NULL);
            if (g->b) vk_free_mem((VkDeviceMemory)(uintptr_t)g->b);
        } else if (g->kind == 2) {
            if (g->c) pvkDestroyImageView(s_vk.device, (VkImageView)(uintptr_t)g->c, NULL);
            if (g->a) pvkDestroyImage(s_vk.device, (VkImage)(uintptr_t)g->a, NULL);
            if (g->b) vk_free_mem((VkDeviceMemory)(uintptr_t)g->b);
        } else if (g->kind == 3) {
            pvkDestroyFramebuffer(s_vk.device, (VkFramebuffer)(uintptr_t)g->a, NULL);
        } else if (g->kind == 4) {
            pvkDestroyImageView(s_vk.device, (VkImageView)(uintptr_t)g->a, NULL);
        } else if (g->kind == 5) {
            vk_geo_free_now((u32)g->a);
        } else if (g->kind == 6) {
            pvkDestroySampler(s_vk.device, (VkSampler)(uintptr_t)g->a, NULL);
        } else if (g->kind == 7) {
            pvkDestroyPipeline(s_vk.device, (VkPipeline)(uintptr_t)g->a, NULL);
        } else if (g->kind == 8) {
            if (g->c) pvkDestroyImageView(s_vk.device, (VkImageView)(uintptr_t)g->c, NULL);
            if (g->a) pvkDestroyImage(s_vk.device, (VkImage)(uintptr_t)g->a, NULL);
            vk_pool_free((u32)g->b);
        }
    }
    sl->ngb = 0;
}

/* The slot whose completion a just-destroyed object must wait for: the one
 * recording, else the most recently submitted; none -> destroy now. */
static vk_bt_slot* vk_bt_owner(void)
{
    if (!s_bt.ready || s_bt.shutting_down) return NULL;
    if (s_bt.recording) return &s_bt.s[s_bt.cur];
    vk_bt_slot* prev = &s_bt.s[(s_bt.cur + VK_BT_SLOTS - 1) % VK_BT_SLOTS];
    return prev->submitted ? prev : NULL;
}
static int vk_bt_push(int kind, uint64_t a, uint64_t b, uint64_t c)
{
    vk_bt_slot* sl = vk_bt_owner();
    if (!sl) return 0;
    if (sl->ngb == sl->capgb) {
        u32 n = sl->capgb ? sl->capgb * 2 : 256;
        vk_garbage* g = (vk_garbage*)realloc(sl->gb, n * sizeof *g);
        if (!g) {
            /* No room to defer it: leak the object rather than let the caller
             * free one the GPU may still use (its pool piece would be handed
             * out again at once). */
            static int said;
            if (!said) { said = 1; VK_LOG("deferred-free list full (out of memory): objects leak from here%c", 10); }
            return 1;
        }
        sl->gb = g; sl->capgb = n;
    }
    sl->gb[sl->ngb++] = (vk_garbage){ kind, a, b, c };
    return 1;
}
static int vk_bt_defer_image(const vk_image* im)
{
    if (!im->img && !im->view && !im->mem) return 0;
    if (!im->img && im->view && !im->mem)               /* a bare view */
        return vk_bt_push(4, (uint64_t)(uintptr_t)im->view, 0, 0);
    if (im->sub)
        return vk_bt_push(8, (uint64_t)(uintptr_t)im->img, im->sub, (uint64_t)(uintptr_t)im->view);
    return vk_bt_push(2, (uint64_t)(uintptr_t)im->img, (uint64_t)(uintptr_t)im->mem,
                      (uint64_t)(uintptr_t)im->view);
}
static int vk_bt_defer_buffer(const void* pb)
{
    const vk_buffer* b = (const vk_buffer*)pb;
    if (!b->buf && !b->mem) return 0;
    return vk_bt_push(1, (uint64_t)(uintptr_t)b->buf, (uint64_t)(uintptr_t)b->mem, b->ptr ? 1 : 0);
}
static int vk_bt_defer_framebuffer(VkFramebuffer fb)
{
    return fb ? vk_bt_push(3, (uint64_t)(uintptr_t)fb, 0, 0) : 0;
}

static void vk_bt_end_pass(void)
{
    if (s_bt.in_pass) { pvkCmdEndRenderPass(s_vk.cmd); s_bt.in_pass = 0; }
}

/* Everything recorded before is ordered before everything after: the
 * guarantee the old submit-and-wait gave between any two operations. */
static void vk_full_barrier(void)
{
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                           .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

/* RSX_ENG_STATS: where the render thread waits on the GPU or the driver. */
extern unsigned long long ps3_qpc_us(void);
static unsigned long long s_vkt_wait_us, s_vkt_submit_us, s_vkt_waits, s_vkt_submits;
/* 0 = the slot's work is done (its garbage freed); -1 = the wait failed (a
 * lost device): nothing of the slot is reset, freed or reused. */
static int vk_bt_wait_slot(vk_bt_slot* sl)
{
    if (!sl->submitted) return 0;
    const unsigned long long _t0 = ps3_qpc_us();
    for (int tries = 0; ; tries++) {
        VkResult r = pvkWaitForFences(s_vk.device, 1, &sl->fence, VK_TRUE, 2000000000ull);
        if (r == VK_SUCCESS) break;
        if (r != VK_TIMEOUT) {
            vk_device_lost_report(r);
            static int said;
            if (!said) { said = 1; VK_LOG("batch slot %d: fence wait failed (VkResult %d); the slot is not reused\n", (int)(sl - s_bt.s), (int)r); }
            return -1;
        }
        VK_LOG("batch slot %d: GPU has not finished after %d s (arena %llu bytes used)\n",
               (int)(sl - s_bt.s), 2 * (tries + 1), (unsigned long long)sl->used);
    }
    { extern unsigned long long g_rsx_gpu_wait_us; const unsigned long long d_ = ps3_qpc_us() - _t0;
      s_vkt_wait_us += d_; s_vkt_waits++; g_rsx_gpu_wait_us += d_; }
    pvkResetFences(s_vk.device, 1, &sl->fence);
    sl->submitted = 0;
    vk_bt_free_garbage(sl);
    return 0;
}

/* Every submitted slot done: the garbage they held (released images and
 * buffers) is freed. For an allocation that failed. */
static void vk_bt_reclaim(void)
{
    for (int i = 0; i < VK_BT_SLOTS; i++)
        if (!(s_bt.recording && i == (int)s_bt.cur)) vk_bt_wait_slot(&s_bt.s[i]);
}

/* Open the current slot for recording (waiting out its previous use). */
static int vk_bt_open(void)
{
    if (s_bt.recording) return 0;
    vk_bt_slot* sl = &s_bt.s[s_bt.cur];
    if (vk_bt_wait_slot(sl)) return -1;
    sl->used = 0;
    if (sl->pool) pvkResetDescriptorPool(s_vk.device, sl->pool, 0);
    VK_CHECK(pvkResetCommandBuffer(sl->cmd, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VK_CHECK(pvkBeginCommandBuffer(sl->cmd, &bi), "vkBeginCommandBuffer");
    s_vk.cmd = sl->cmd;
    s_bt.recording = 1;
    s_bt.epoch++;
    return 0;
}

/* Submit the open batch. `wait`: block until the GPU finished it (and
 * everything before). Optional semaphores for the window present. */
static int vk_bt_submit(int wait, VkSemaphore wait_sem, VkSemaphore signal_sem)
{
    if (!s_bt.recording) {
        if (wait) for (u32 i = 0; i < VK_BT_SLOTS; i++) vk_bt_wait_slot(&s_bt.s[i]);
        return 0;
    }
    vk_bt_slot* sl = &s_bt.s[s_bt.cur];
    vk_bt_end_pass();
    VK_CHECK(pvkEndCommandBuffer(sl->cmd), "vkEndCommandBuffer");
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .waitSemaphoreCount = wait_sem ? 1u : 0u, .pWaitSemaphores = &wait_sem,
                        .pWaitDstStageMask = &ws,
                        .commandBufferCount = 1, .pCommandBuffers = &sl->cmd,
                        .signalSemaphoreCount = signal_sem ? 1u : 0u, .pSignalSemaphores = &signal_sem };
    s_bt.recording = 0;
#ifdef __ANDROID__
    s_bt_draws = 0;
#endif
    { const unsigned long long _t0 = ps3_qpc_us();
      VK_CHECK(pvkQueueSubmit(s_vk.queue, 1, &si, sl->fence), "vkQueueSubmit");
      s_vkt_submit_us += ps3_qpc_us() - _t0; s_vkt_submits++; }
    sl->submitted = 1;
    s_bt.cur = (s_bt.cur + 1) % VK_BT_SLOTS;
    if (wait) for (u32 i = 0; i < VK_BT_SLOTS; i++) vk_bt_wait_slot(&s_bt.s[i]);
    return 0;
}

/* Bump-allocate `bytes` from the open batch's arena; NULL when full. */
static void* vk_bt_alloc(VkDeviceSize bytes, VkDeviceSize align, VkDeviceSize* off)
{
    vk_bt_slot* sl = &s_bt.s[s_bt.cur];
    if (!sl->arena.ptr) return NULL;
    VkDeviceSize o = (sl->used + align - 1) / align * align;
    if (o + bytes > sl->arena.size) return NULL;
    sl->used = o + bytes;
    *off = o;
    return (u8*)sl->arena.ptr + o;
}

static int vk_bt_init(void)
{
    VkPhysicalDeviceProperties p;
    pvkGetPhysicalDeviceProperties(s_vk.phys, &p);
    s_bt.ubo_align = p.limits.minUniformBufferOffsetAlignment ? p.limits.minUniformBufferOffsetAlignment : 256;
    s_bt.sync_cmd = s_vk.cmd;
    for (u32 i = 0; i < VK_BT_SLOTS; i++) {
        vk_bt_slot* sl = &s_bt.s[i];
        VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                            .commandPool = s_vk.pool,
                                            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                            .commandBufferCount = 1 };
        VK_CHECK(pvkAllocateCommandBuffers(s_vk.device, &cai, &sl->cmd), "vkAllocateCommandBuffers(batch)");
        VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VK_CHECK(pvkCreateFence(s_vk.device, &fci, NULL, &sl->fence), "vkCreateFence(batch)");
        if (vk_create_host_buffer(&sl->arena, VK_BT_ARENA,
                                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
            !sl->arena.coherent) {
            VK_LOG("batch arena: no host-coherent memory\n"); return -1;
        }
        VkDescriptorPoolSize ps[3] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2u * VK_BT_SETS },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2u * RSX_MAX_TEXTURES * VK_BT_SETS },
            { VK_DESCRIPTOR_TYPE_SAMPLER, RSX_MAX_TEXTURES * VK_BT_SETS },
        };
        VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                           .maxSets = VK_BT_SETS, .poolSizeCount = 3, .pPoolSizes = ps };
        VK_CHECK(pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &sl->pool), "vkCreateDescriptorPool(batch)");
    }
    s_bt.ready = 1;
    return 0;
}

static void vk_bt_shutdown(void)
{
    if (!s_bt.ready) return;
    if (s_bt.recording) vk_bt_submit(1, VK_NULL_HANDLE, VK_NULL_HANDLE);
    for (u32 i = 0; i < VK_BT_SLOTS; i++) vk_bt_wait_slot(&s_bt.s[i]);
    s_bt.shutting_down = 1;
    for (u32 i = 0; i < VK_BT_SLOTS; i++) {
        vk_bt_slot* sl = &s_bt.s[i];
        vk_bt_free_garbage(sl);
        free(sl->gb); sl->gb = NULL; sl->capgb = 0;
        vk_destroy_buffer(&sl->arena);
        if (sl->pool) pvkDestroyDescriptorPool(s_vk.device, sl->pool, NULL);
        if (sl->fence) pvkDestroyFence(s_vk.device, sl->fence, NULL);
    }
    s_vk.cmd = s_bt.sync_cmd;
    s_bt.ready = 0;
}

/* Start recording an operation: open the batch, leave any render pass (the
 * callers record transfers and clears), and order it after everything so far. */
static int vk_begin(void)
{
    if (!s_bt.ready) {
        VK_CHECK(pvkResetCommandBuffer(s_vk.cmd, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        VK_CHECK(pvkBeginCommandBuffer(s_vk.cmd, &bi), "vkBeginCommandBuffer");
        return 0;
    }
    if (vk_bt_open()) return -1;
    vk_bt_end_pass();
    vk_full_barrier();
    return 0;
}

/* The old contract: everything recorded so far has executed when this returns. */
static int vk_submit_and_wait(void)
{
    if (!s_bt.ready) {
        VK_CHECK(pvkEndCommandBuffer(s_vk.cmd), "vkEndCommandBuffer");
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                            .commandBufferCount = 1, .pCommandBuffers = &s_vk.cmd };
        VK_CHECK(pvkResetFences(s_vk.device, 1, &s_vk.fence), "vkResetFences");
        VK_CHECK(pvkQueueSubmit(s_vk.queue, 1, &si, s_vk.fence), "vkQueueSubmit");
        VK_CHECK(pvkWaitForFences(s_vk.device, 1, &s_vk.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        return 0;
    }
    return vk_bt_submit(1, VK_NULL_HANDLE, VK_NULL_HANDLE);
}

/* End of an operation whose result only the GPU needs: stays in the batch. */
static int vk_defer(void) { return 0; }

/* Order this command buffer's use of an image after everything submitted
 * before it; from UNDEFINED it is also the one-time move to GENERAL. */
static void vk_barrier_image(VkImage img, VkImageAspectFlags aspect, VkImageLayout old_layout)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = old_layout, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img, .subresourceRange = { aspect, 0, VK_REMAINING_MIP_LEVELS, 0,
                                            VK_REMAINING_ARRAY_LAYERS },
    };
    pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

/* ---------------------------------------------------------------------------
 * Texture unit 0
 * -------------------------------------------------------------------------*/
static void vk_write_texture_descriptor(void)
{
    VkDescriptorImageInfo ii = { .sampler = s_vk.sampler,
                                 .imageView = s_vk.tex_ready[0] ? s_vk.tex[0].view
                                                                : s_vk.dummy.view,
                                 .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                               .dstSet = s_vk.desc_set, .dstBinding = 0,
                               .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                               .pImageInfo = &ii };
    pvkUpdateDescriptorSets(s_vk.device, 1, &w, 0, NULL);
}

/* Replace *dst with an RGBA8 image of `nlv` levels: level m is lw[m] x lh[m]
 * texels, tightly packed, the levels one after another in `rgba`. */
static int vk_upload_texture_levels(vk_image* dst, const u8* rgba,
                                    const u32* lw, const u32* lh, u32 nlv)
{
    VkBufferImageCopy regions[RSX_MAX_TEXTURE_LEVELS];
    if (!nlv || nlv > RSX_MAX_TEXTURE_LEVELS) return -1;
    VkDeviceSize total = 0;
    for (u32 m = 0; m < nlv; m++) {
        regions[m] = (VkBufferImageCopy){
            .bufferOffset = total,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1 },
            .imageExtent = { lw[m], lh[m], 1 },
        };
        total += (VkDeviceSize)lw[m] * lh[m] * 4u;
    }
    vk_buffer staging = {0};
    if (vk_create_host_buffer(&staging, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
        !staging.coherent) {
        vk_destroy_buffer(&staging); return -1;
    }
    memcpy(staging.ptr, rgba, (size_t)total);

    vk_image fresh = {0};
    if (vk_create_image_levels(&fresh, VK_FORMAT_R8G8B8A8_UNORM, lw[0], lh[0], nlv,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT)) {
        vk_destroy_image(&fresh); vk_destroy_buffer(&staging); return -1;
    }
    int rc = vk_begin();
    if (!rc) {
        vk_barrier_image(fresh.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
        pvkCmdCopyBufferToImage(s_vk.cmd, staging.buf, fresh.img, VK_IMAGE_LAYOUT_GENERAL,
                                nlv, regions);
        rc = vk_submit_and_wait();
    }
    vk_destroy_buffer(&staging);
    if (rc) { vk_destroy_image(&fresh); return -1; }

    vk_destroy_image(dst);          /* idle: every submission is waited on */
    *dst = fresh;
    return 0;
}

static int vk_upload_texture(vk_image* dst, const u8* rgba, u32 w, u32 h)
{
    return vk_upload_texture_levels(dst, rgba, &w, &h, 1);
}

/* NV4097_SET_TEXTURE_ADDRESS wrap field, one per axis -- the table the Metal
 * backend and the live draw engine use: 1 WRAP, 2 MIRROR, 3 CLAMP_TO_EDGE,
 * 4 BORDER, 5 CLAMP, 6..8 MIRROR_ONCE. Vulkan 1.0 has MIRROR_CLAMP_TO_EDGE
 * only behind an extension, so the MIRROR_ONCE family clamps to edge here --
 * exact for texcoords in [0,1], the mirrored half is what is lost. */
static VkSamplerAddressMode vk_gcm_wrap(u32 w)
{
    switch (w & 0xFu) {
    case 1:  return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2:  return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 4:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;      /* 3, 5..8, unset */
    }
}

/* vk_gcm_wrap, with BORDER as clamp-to-edge when the border colour is one
 * core Vulkan cannot express (vk_eng_sampler bc 3). */
static VkSamplerAddressMode vk_gcm_wrap_bc(u32 w, u32 bc)
{
    const VkSamplerAddressMode m = vk_gcm_wrap(w);
    return m == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER && bc == 3u ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : m;
}

/* A unit's sampler from its registers, decoded as the Metal backend's
 * samp_slot_for does: SET_TEXTURE_FILTER min at [18:16] (1 NEAREST,
 * 2 LINEAR, 3..6 the four nearest/linear x nearest/linear-mip pairs), mag at
 * [26:24]; SET_TEXTURE_CONTROL0 max LOD at [18:7], min LOD at [30:19], 4.8
 * fixed point. The LOD bias (FILTER [12:0]) is not applied, as in Metal.
 * VK_NULL_HANDLE on failure: the caller falls back to `sampler`. */
static VkSampler vk_unit_sampler(const rsx_texture_state* t)
{
    const u32 minf = (t->filter >> 16) & 7u, magf = (t->filter >> 24) & 7u;
    const u32 lod  = (t->control0 >> 7) & 0xFFFFFFu;       /* max then min LOD */
    const u64 key  = (u64)minf | ((u64)magf << 3)
                   | ((u64)(t->address & 0x000F0F0Fu) << 6) | ((u64)lod << 26);
    for (u32 i = 0; i < s_vk.samp_count; i++)
        if (s_vk.samp_cache[i].key == key) return s_vk.samp_cache[i].s;
    if (s_vk.samp_count >= VK_SAMPLER_CACHE) return VK_NULL_HANDLE;

    const int mip_present = (minf >= 3);
    const int mip_linear  = (minf == 5 || minf == 6);
    /* A min filter with no mip term samples level 0 only: pin the range. */
    const float min_lod = (float)((t->control0 >> 19) & 0xFFFu) / 256.0f;
    float max_lod = mip_present ? (float)((t->control0 >> 7) & 0xFFFu) / 256.0f : 0.0f;
    if (max_lod < min_lod) max_lod = min_lod;
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = (magf == 2) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = (minf == 2 || minf == 4 || minf == 6) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = vk_gcm_wrap(t->address),
        .addressModeV = vk_gcm_wrap(t->address >> 8),
        .addressModeW = vk_gcm_wrap(t->address >> 16),
        .minLod = min_lod, .maxLod = max_lod,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
    };
    VkSampler smp = VK_NULL_HANDLE;
    if ((s_vkc[3]++, pvkCreateSampler(s_vk.device, &sci, NULL, &smp)) != VK_SUCCESS) return VK_NULL_HANDLE;
    s_vk.samp_cache[s_vk.samp_count].key = key;
    s_vk.samp_cache[s_vk.samp_count].s   = smp;
    s_vk.samp_count++;
    return smp;
}

/* One level of a guest texture into tightly packed RGBA8, with the null
 * backend's decode (rsx_null_backend.c, nullsw_bind_texture): formats that
 * decode to fewer than four bytes are splayed, alpha forced opaque. */
static int vk_decode_level_rgba8(u8* dst, const u8* src, u32 w, u32 h,
                                 const rsx_tex_layout* tl)
{
    const u32 pitch = w * 4u;
    if (tl->fmt == RSX_TEXFMT_R8G8B8A8) {
        rsx_texture_decode(dst, pitch, src, w, h, tl, rsx_texture_argb_is_rgba());
        return 0;
    }
    const u32 srcp = tl->row_bytes;
    u8* tmp = (u8*)malloc((size_t)srcp * h);
    if (!tmp) return -1;
    rsx_texture_decode(tmp, srcp, src, w, h, tl, 0);
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++) {
            const u8* sp = tmp + (size_t)y * srcp + (size_t)x * tl->bytes_per_texel;
            u8* dp = dst + (size_t)y * pitch + (size_t)x * 4u;
            dp[0] = sp[0];
            dp[1] = tl->bytes_per_texel > 1 ? sp[1] : sp[0];
            dp[2] = tl->bytes_per_texel > 2 ? sp[2] : sp[0];
            dp[3] = 255;
        }
    free(tmp);
    return 0;
}

/* Unit `unit` has nothing usable bound. The fallback path's descriptor only
 * ever shows unit 0, so only that one needs rewriting. */
static void vk_tex_off(u32 unit)
{
    s_vk.tex_ready[unit] = 0;
    s_vk.tex_sampler[unit] = VK_NULL_HANDLE;
    if (unit == 0) vk_write_texture_descriptor();
}

/* Same decode as the headless null backend (rsx_null_backend.c,
 * nullsw_bind_texture), so both backends sample identical texels. */
static void vk_cb_bind_texture(void* ud, u32 unit, const rsx_texture_state* t)
{
    (void)ud;
    if (unit >= RSX_MAX_TEXTURES || !t || !s_vk.device) return;
    if (!(t->control0 & 0x80000000u) || !t->offset) { vk_tex_off(unit); return; }

    u32 w = (t->image_rect >> 16) & 0xFFFFu;
    u32 h =  t->image_rect        & 0xFFFFu;
    if (!w || !h || w > 4096u || h > 4096u) { vk_tex_off(unit); return; }

    u32 ea = cellGcmResolveLocated((t->format & 3u) == 1u, t->offset);
    if (!vm_base || ea == 0xFFFFFFFFu) { vk_tex_off(unit); return; }
    const u32 fmt = (t->format >> 8) & 0xFFu;

    /* The whole mip chain, as the Metal backend reads it: SET_TEXTURE_FORMAT's
     * level count above bit 15 and SET_TEXTURE_CONTROL3's row pitch, laid out
     * by the shared rsx_texture_mip_chain() (which clamps the count to what
     * the dimensions allow). Cube maps are not handled yet: face 0 only. */
    const u32 levels    = (t->format >> 16) & 0xFFFFu;
    const u32 row_pitch = t->control3 & 0xFFFFFu;
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 nlv = rsx_texture_mip_chain(fmt, w, h, levels, row_pitch, lv);
    if (!nlv || lv[0].tl.compressed) { vk_tex_off(unit); return; }   /* no BC path yet */

    size_t total = 0, off[RSX_MAX_TEXTURE_LEVELS];
    u32 lw[RSX_MAX_TEXTURE_LEVELS], lh[RSX_MAX_TEXTURE_LEVELS];
    for (u32 m = 0; m < nlv; m++) {
        off[m] = total;
        lw[m] = lv[m].w; lh[m] = lv[m].h;
        total += (size_t)lv[m].w * lv[m].h * 4u;
    }
    u8* buf = (u8*)malloc(total);
    if (!buf) { vk_tex_off(unit); return; }
    for (u32 m = 0; m < nlv; m++)
        if (vk_decode_level_rgba8(buf + off[m], vm_base + ea + lv[m].offset,
                                  lv[m].w, lv[m].h, &lv[m].tl)) {
            free(buf); vk_tex_off(unit); return;
        }
    s_vk.tex_ready[unit] = vk_upload_texture_levels(&s_vk.tex[unit], buf, lw, lh, nlv) == 0;
    free(buf);
    s_vk.tex_sampler[unit] = s_vk.tex_ready[unit] ? vk_unit_sampler(t) : VK_NULL_HANDLE;
    if (unit == 0) vk_write_texture_descriptor();
}

/* ---------------------------------------------------------------------------
 * Render pass, framebuffer, pipelines
 * -------------------------------------------------------------------------*/
static void vk_pick_eng_depth_format(void)
{
    /* D32_SFLOAT_S8_UINT keeps depth snapshots a plain float copy of the
     * depth aspect; D24_UNORM_S8_UINT has the stencil but no snapshots. */
    static const VkFormat candidates[] = { VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT };
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        VkFormatProperties fp;
        pvkGetPhysicalDeviceFormatProperties(s_vk.phys, candidates[i], &fp);
        if ((fp.optimalTilingFeatures & need) == need) {
            s_vk.eng_depth_format = candidates[i];
            s_vk.eng_depth_aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            { extern unsigned g_rsx_depth_bpp; g_rsx_depth_bpp = candidates[i] == VK_FORMAT_D32_SFLOAT_S8_UINT ? 8u : 4u; }
            return;
        }
    }
    VK_LOG("engine: no depth+stencil format; stencil tests will always pass%c", 10);
    s_vk.eng_depth_format = s_vk.depth_format;
    s_vk.eng_depth_aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
}

static int vk_pick_depth_format(void)
{
    static const VkFormat candidates[] = {
        VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D16_UNORM
    };
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                      VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        VkFormatProperties fp;
        pvkGetPhysicalDeviceFormatProperties(s_vk.phys, candidates[i], &fp);
        if ((fp.optimalTilingFeatures & need) == need) {
            s_vk.depth_format = candidates[i]; return 0;
        }
    }
    /* TRANSFER_DST is a 1.1 format feature bit; 1.0 drivers may not report
     * it although clears work. Retry on the attachment bit alone. */
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        VkFormatProperties fp;
        pvkGetPhysicalDeviceFormatProperties(s_vk.phys, candidates[i], &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            s_vk.depth_format = candidates[i]; return 0;
        }
    }
    VK_LOG("no usable depth format\n");
    return -1;
}

static int vk_create_render_pass(void)
{
    VkAttachmentDescription att[2] = {
        { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
          .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL },
        { .format = s_vk.depth_format, .samples = VK_SAMPLE_COUNT_1_BIT,
          .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL },
    };
    VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_GENERAL };
    VkAttachmentReference dref = { 1, VK_IMAGE_LAYOUT_GENERAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 .colorAttachmentCount = 1, .pColorAttachments = &cref,
                                 .pDepthStencilAttachment = &dref };
    VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                   .attachmentCount = 2, .pAttachments = att,
                                   .subpassCount = 1, .pSubpasses = &sub };
    VK_CHECK(pvkCreateRenderPass(s_vk.device, &rci, NULL, &s_vk.render_pass), "vkCreateRenderPass");

    VkImageView views[2] = { s_vk.color.view, s_vk.depth.view };
    VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                    .renderPass = s_vk.render_pass, .attachmentCount = 2,
                                    .pAttachments = views, .width = s_vk.width,
                                    .height = s_vk.height, .layers = 1 };
    VK_CHECK((s_vkc[2]++, pvkCreateFramebuffer(s_vk.device, &fci, NULL, &s_vk.framebuffer)), "vkCreateFramebuffer");
    return 0;
}

static int vk_create_shader(const uint32_t* code, size_t bytes, VkShaderModule* out)
{
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = bytes, .pCode = code };
    VK_CHECK(pvkCreateShaderModule(s_vk.device, &ci, NULL, out), "vkCreateShaderModule");
    return 0;
}

static int vk_create_pipeline_objects(void)
{
    if (vk_create_shader(rsx_vk_fallback_vert_spv, sizeof rsx_vk_fallback_vert_spv, &s_vk.vs) ||
        vk_create_shader(rsx_vk_fallback_frag_spv, sizeof rsx_vk_fallback_frag_spv, &s_vk.fs))
        return -1;

    VkDescriptorSetLayoutBinding bind = {
        .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
    VkDescriptorSetLayoutCreateInfo lci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &bind };
    VK_CHECK(pvkCreateDescriptorSetLayout(s_vk.device, &lci, NULL, &s_vk.set_layout),
             "vkCreateDescriptorSetLayout");

    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
    VK_CHECK(pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_vk.desc_pool), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = s_vk.desc_pool,
                                        .descriptorSetCount = 1, .pSetLayouts = &s_vk.set_layout };
    VK_CHECK(pvkAllocateDescriptorSets(s_vk.device, &dai, &s_vk.desc_set), "vkAllocateDescriptorSets");

    VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(int32_t) };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                        .setLayoutCount = 1, .pSetLayouts = &s_vk.set_layout,
                                        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    VK_CHECK(pvkCreatePipelineLayout(s_vk.device, &plci, NULL, &s_vk.pipe_layout),
             "vkCreatePipelineLayout");

    /* Nearest + repeat: the null backend's point sample with wrapping. */
    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                .maxLod = 0.0f };
    VK_CHECK((s_vkc[3]++, pvkCreateSampler(s_vk.device, &sci, NULL, &s_vk.sampler)), "vkCreateSampler");

    /* A 1x1 magenta dummy keeps the descriptor valid before any bind --
     * the same "not bound" colour the null backend's sampler returns. */
    static const u8 magenta[4] = { 0xFF, 0x00, 0xFF, 0xFF };
    if (vk_upload_texture(&s_vk.dummy, magenta, 1, 1)) return -1;
    vk_write_texture_descriptor();
    return 0;
}

/* NV4097 depth function is the GL enum 0x200 NEVER .. 0x207 ALWAYS, which is
 * VkCompareOp 0..7 in the same order. Anything else passes, as in the null
 * and D3D12 backends (including the 1 rsx_state_init seeds). */
static u32 vk_compare_slot(u32 gl_func)
{
    return (gl_func >= 0x200u && gl_func <= 0x207u) ? gl_func - 0x200u
                                                    : (u32)VK_COMPARE_OP_ALWAYS;
}

static VkPipeline vk_get_pipeline(int ztest, int zwrite, u32 cmp)
{
    u32 slot = ((u32)ztest * 2u + (u32)zwrite) * 8u + cmp;
    if (s_vk.pipelines[slot]) return s_vk.pipelines[slot];

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = s_vk.vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = s_vk.fs, .pName = "main" },
    };
    VkVertexInputBindingDescription vb = { 0, sizeof(vk_vertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[3] = {
        { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(vk_vertex, pos) },
        { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(vk_vertex, col) },
        { 2, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(vk_vertex, uv)  },
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
        .vertexAttributeDescriptionCount = 3, .pVertexAttributeDescriptions = va };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    /* No culling: the null backend fills both windings. */
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = ztest ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = zwrite ? VK_TRUE : VK_FALSE,
        .depthCompareOp = (VkCompareOp)cmp };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vi,
        .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
        .pDynamicState = &dy, .layout = s_vk.pipe_layout, .renderPass = s_vk.render_pass,
        .subpass = 0 };
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = (s_vkc[5]++, pvkCreateGraphicsPipelines(s_vk.device, VK_NULL_HANDLE, 1, &gci, NULL, &p));
    if (r != VK_SUCCESS) { VK_LOG("vkCreateGraphicsPipelines failed (%d)\n", (int)r); return VK_NULL_HANDLE; }
    s_vk.pipelines[slot] = p;
    return p;
}

/* ---------------------------------------------------------------------------
 * Draw path
 * -------------------------------------------------------------------------*/
static void vk_cb_track_state(void* ud, const rsx_state* state)
{
    (void)ud; s_vk.state = state;
}

static int vk_push_vertex(const float pos[4], const float col[4], const float uv[4])
{
    if (s_vk.cpu_count == s_vk.cpu_cap) {
        size_t cap = s_vk.cpu_cap ? s_vk.cpu_cap * 2 : 1024;
        vk_vertex* n = (vk_vertex*)realloc(s_vk.cpu_verts, cap * sizeof *n);
        if (!n) return -1;
        s_vk.cpu_verts = n; s_vk.cpu_cap = cap;
    }
    vk_vertex* v = &s_vk.cpu_verts[s_vk.cpu_count++];
    memcpy(v->pos, pos, sizeof v->pos);
    memcpy(v->col, col, sizeof v->col);
    v->uv[0] = uv[0]; v->uv[1] = uv[1];
    return 0;
}

/* One triangle, colour from its first vertex (the null backend's flat shade). */
static void vk_emit_tri(const rsx_state* st, u32 i0, u32 i1, u32 i2)
{
    float p[3][4], uv[3][4], col[4];
    rsx_fetch_attrib(st, 0, i0, p[0]);
    rsx_fetch_attrib(st, 0, i1, p[1]);
    rsx_fetch_attrib(st, 0, i2, p[2]);
    rsx_fetch_attrib(st, 3, i0, col);
    rsx_fetch_attrib(st, 8, i0, uv[0]);
    rsx_fetch_attrib(st, 8, i1, uv[1]);
    rsx_fetch_attrib(st, 8, i2, uv[2]);
    for (int k = 0; k < 3; k++) vk_push_vertex(p[k], col, uv[k]);
}

/* Called once per expanded triangle: the fixed-function path records the
 * null backend's flat-shaded vertex, the guest path all sixteen attributes. */
typedef void (*vk_tri_fn)(const rsx_state* st, u32 i0, u32 i1, u32 i2);

/* Primitive expansion, identical to the null backend's draw_prim(). */
static void vk_expand(const rsx_state* st, u32 prim, u32 first, u32 count, vk_tri_fn emit)
{
    switch (prim) {
    case RSX_PRIMITIVE_TRIANGLES:
        for (u32 i = 0; i + 2 < count; i += 3) emit(st, first + i, first + i + 1, first + i + 2);
        break;
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
        for (u32 i = 0; i + 2 < count; i++)
            emit(st, first + i, first + i + 1 + (i & 1), first + i + 2 - (i & 1));
        break;
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_POLYGON:
        for (u32 i = 1; i + 1 < count; i++) emit(st, first, first + i, first + i + 1);
        break;
    case RSX_PRIMITIVE_QUADS:
        for (u32 i = 0; i + 3 < count; i += 4) {
            emit(st, first + i, first + i + 1, first + i + 2);
            emit(st, first + i, first + i + 2, first + i + 3);
        }
        break;
    case RSX_PRIMITIVE_QUAD_STRIP:
        for (u32 i = 0; i + 3 < count; i += 2) {
            emit(st, first + i, first + i + 1, first + i + 3);
            emit(st, first + i, first + i + 3, first + i + 2);
        }
        break;
    default:
        break;   /* points and lines: no coverage, as in the null backend */
    }
}

static int vk_ensure_vertex_buffer(VkDeviceSize bytes)
{
    if (s_vk.vertices.buf && s_vk.vertices.size >= bytes) return 0;
    VkDeviceSize size = s_vk.vertices.size ? s_vk.vertices.size : 64 * 1024;
    while (size < bytes) size *= 2;
    vk_destroy_buffer(&s_vk.vertices);   /* idle: every submission is waited on */
    if (vk_create_host_buffer(&s_vk.vertices, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return -1;
    if (!s_vk.vertices.coherent) { VK_LOG("vertex memory is not host-coherent\n"); return -1; }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Guest programs (stage V3)
 *
 * With the HLSL -> SPIR-V translator built and PS3RECOMP_VK_GUEST_PROGRAMS=1,
 * a draw runs the guest's own vertex and fragment programs, as the Metal
 * backend's guest path does and keyed the same way: decompiled to HLSL by the
 * shared decompilers, translated by rsx_shader_spirv.cpp, cached per program.
 * Constants use the Metal/D3D12 layout: VPConst is the 512 transform
 * constants plus the viewport epilogue, PSConstants the fragment program's
 * inline constants plus fp_alpha.
 *
 * Vertices are still fetched and expanded on the CPU through the shared
 * rsx_fetch_attrib(), now all sixteen attributes (a disabled one reads its
 * constant register, as on the hardware). Every RSX vertex format therefore
 * arrives as float4, and the pipeline has one fixed input layout: location n
 * is attribute n, which is what the vertex decompiler declares.
 *
 * A program that needs a cube map or a vertex texture falls back to the
 * fixed-function path, with a message. Not honoured on this path yet: the RSX
 * viewport and scissor (the whole target is used, as on the fallback path),
 * blending, stencil, culling, per-unit sampler state.
 * -------------------------------------------------------------------------*/
#define VK_VP_CACHE      256
#define VK_FP_CACHE      512
#define VK_GPIPE_CACHE   512
#define VK_FP_MAX_BYTES  4096u              /* bound on a fragment program, as Metal/D3D12 */
#define VK_HLSL_BYTES    (256u * 1024u)
#define VK_SPV_WORDS     (256u * 1024u)
#define VK_VPCONST_BYTES ((RSX_MAX_VERTEX_CONSTANTS + 2u) * 16u)
#define VK_GUEST_ATTRS   16u
#define VK_GUEST_FLOATS  (VK_GUEST_ATTRS * 4u)   /* per vertex */

typedef struct { u32 hash; VkShaderModule mod; } vk_vp_entry;
typedef struct { u64 key; VkShaderModule mod; u32 nconst; } vk_fp_entry;
typedef struct { int vs, fs; u32 depth_key; VkPipeline pipe; } vk_gpipe_entry;

static struct {
    int                   on;
    char*                 hlsl;
    u32*                  spv;
    char                  log[4096];
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout      pipe_layout;
    VkPipelineCache       pcache;           /* RSX_PIPELINE_CACHE: kept on disk */
    unsigned              pcache_dirty;     /* pipelines built since the last save */
    u32                   pcache_salt;      /* RSX_PIPELINE_SALT (tests): vk_vdesc_build */
    VkDescriptorPool      pool;
    VkDescriptorSet       set;
    vk_buffer             ubo;              /* VPConst at 0, PSConstants at ps_off */
    VkDeviceSize          ps_off;
    VkDeviceSize          max_ubo_range;
    vk_buffer             vertices;
    float*                cpu;
    size_t                cpu_cap, cpu_count;   /* in vertices */
    vk_vp_entry           vp[VK_VP_CACHE];     u32 vp_count;
    vk_fp_entry           fp[VK_FP_CACHE];     u32 fp_count;
    vk_gpipe_entry        pipes[VK_GPIPE_CACHE]; u32 pipe_count;
    rsx_fp_constant_block consts;
    u32                   draws, last_draws;
    u32                   warned;
} s_g;

int rsx_vulkan_backend_guest_programs(void)
{
    const char* v = getenv("PS3RECOMP_VK_GUEST_PROGRAMS");
    return rsx_hlsl_to_spirv_available() && v && v[0] && v[0] != '0';
}

static u32 vk_fnv1a32(const u8* p, u32 n)
{
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static u64 vk_fnv1a64(const void* d, size_t n, u64 h)
{
    const u8* p = (const u8*)d;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

/* Built from the registers, as the Metal and D3D12 backends do. */
static u32 vk_cube_mask(const rsx_state* st)
{
    u32 m = 0;
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        const rsx_texture_state* t = &st->textures[u];
        const u32 w = (t->image_rect >> 16) & 0xFFFFu, h = t->image_rect & 0xFFFFu;
        if ((t->control0 & 0x80000000u) && (t->format & 4u) && w && w == h) m |= 1u << u;
    }
    return m;
}

static u32 vk_vtex_mask(const rsx_state* st)
{
    u32 m = 0;
    for (u32 u = 0; u < RSX_MAX_VERTEX_TEXTURES; u++)
        if (st->vertex_textures[u].control0 & 0x80000000u) m |= 1u << u;
    return m;
}

static VkShaderModule vk_guest_module(const char* what, int stage)
{
    u32 n = 0;
    if (rsx_hlsl_to_spirv(s_g.hlsl, stage, s_g.spv, VK_SPV_WORDS, &n,
                          s_g.log, sizeof s_g.log) != 0) {
        VK_LOG("%s: %s\n", what, s_g.log);
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = (size_t)n * 4u, .pCode = s_g.spv };
    VkShaderModule m = VK_NULL_HANDLE;
    if (pvkCreateShaderModule(s_vk.device, &ci, NULL, &m) != VK_SUCCESS) {
        VK_LOG("%s: vkCreateShaderModule failed\n", what);
        return VK_NULL_HANDLE;
    }
    return m;
}

/* The vertex program, keyed on its own bytes to its end bit (as Metal). */
static int vk_vp_slot(const rsx_state* st)
{
    if (st->vp_ucode_bytes < 16) return -1;
    u32 vstart = st->transform_program_start * 16u;
    if (vstart >= st->vp_ucode_bytes) vstart = 0;
    const u8* uc = st->vp_ucode + vstart;
    const u32 avail = st->vp_ucode_bytes - vstart;
    const u32 instrs = rsx_vp_program_size_instrs(uc, avail);
    const u32 len = instrs ? instrs * 16u : avail;
    const u32 hash = vk_fnv1a32(uc, len);
    for (u32 i = 0; i < s_g.vp_count; i++)
        if (s_g.vp[i].hash == hash) return s_g.vp[i].mod ? (int)i : -1;
    if (s_g.vp_count >= VK_VP_CACHE) return -1;

    char what[64];
    snprintf(what, sizeof what, "vertex program %08X", hash);
    VkShaderModule m = VK_NULL_HANDLE;
    const int ni = rsx_vp_decompile_ex(uc, len, 0, s_g.hlsl, VK_HLSL_BYTES);
    if (ni <= 0) VK_LOG("%s: decompile failed (%d)\n", what, ni);
    else         m = vk_guest_module(what, RSX_SHADER_STAGE_VERTEX);
    if (s_g.vp_count < 32)
        VK_LOG("%s: %d instrs -> %s\n", what, ni, m ? "ok" : "FAILED (fixed-function instead)");
    const int slot = (int)s_g.vp_count++;
    s_g.vp[slot].hash = hash;
    s_g.vp[slot].mod  = m;
    return m ? slot : -1;
}

/* The fragment program, resolved and keyed as the Metal backend's
 * fp_slot_for: structure (inline constants live in PSConstants), export
 * width, and the alpha test patched into the source. No cube units here --
 * the caller has already sent those to the fixed-function path. */
static int vk_fp_slot(const rsx_state* st, const u8** uc)
{
    if (!vm_base || st->shader_program == 0) return -1;
    const u32 off = cellGcmResolveLocated((st->shader_program & 3u) == 1u,
                                          st->shader_program & ~3u);
    if (off == 0xFFFFFFFFu) return -1;
    *uc = vm_base + off;
    u64 key = rsx_fp_structural_hash(*uc, VK_FP_MAX_BYTES, 1469598103934665603ull);
    if (!key) return -1;                       /* malformed or unterminated */
    const u32 ctrl     = st->shader_control;
    const u32 ctrl_key = ctrl & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS;
    const u32 alpha_en = (st->alpha_test_enable && st->alpha_func != 0x0207u) ? 1u : 0u;
    const u32 alpha_fn = alpha_en ? st->alpha_func : 0u;
    key = vk_fnv1a64(&ctrl_key, sizeof ctrl_key, key);
    key = vk_fnv1a64(&alpha_en, sizeof alpha_en, key);
    key = vk_fnv1a64(&alpha_fn, sizeof alpha_fn, key);
    for (u32 i = 0; i < s_g.fp_count; i++)
        if (s_g.fp[i].key == key) return s_g.fp[i].mod ? (int)i : -1;
    if (s_g.fp_count >= VK_FP_CACHE) return -1;

    char what[64];
    snprintf(what, sizeof what, "fragment program %016llX", (unsigned long long)key);
    VkShaderModule m = VK_NULL_HANDLE;
    u32 nconst = 0;
    int ni = rsx_fp_decompile_buffered_ex(*uc, VK_FP_MAX_BYTES, ctrl, 0,
                                          s_g.hlsl, VK_HLSL_BYTES, &nconst);
    if (ni > 0 && alpha_en &&
        rsx_fp_apply_alpha_test_buffered(s_g.hlsl, VK_HLSL_BYTES, st->alpha_func) < 0)
        ni = -1;
    if (ni <= 0) VK_LOG("%s: decompile failed (%d)\n", what, ni);
    else         m = vk_guest_module(what, RSX_SHADER_STAGE_FRAGMENT);
    if (s_g.fp_count < 32)
        VK_LOG("%s: %d instrs, %u constants -> %s\n", what, ni, nconst,
               m ? "ok" : "FAILED (fixed-function instead)");
    const int slot = (int)s_g.fp_count++;
    s_g.fp[slot].key    = key;
    s_g.fp[slot].mod    = m;
    s_g.fp[slot].nconst = nconst;
    return m ? slot : -1;
}

static VkPipeline vk_guest_pipeline(int vs, int fs, int ztest, int zwrite, u32 cmp)
{
    const u32 dkey = (u32)ztest | ((u32)zwrite << 1) | (cmp << 2);
    for (u32 i = 0; i < s_g.pipe_count; i++)
        if (s_g.pipes[i].vs == vs && s_g.pipes[i].fs == fs && s_g.pipes[i].depth_key == dkey)
            return s_g.pipes[i].pipe;
    if (s_g.pipe_count >= VK_GPIPE_CACHE) return VK_NULL_HANDLE;

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = s_g.vp[vs].mod, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = s_g.fp[fs].mod, .pName = "main" },
    };
    VkVertexInputBindingDescription vb = { 0, VK_GUEST_FLOATS * 4u, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[VK_GUEST_ATTRS];
    for (u32 a = 0; a < VK_GUEST_ATTRS; a++) {
        va[a].location = a;
        va[a].binding  = 0;
        va[a].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
        va[a].offset   = a * 16u;
    }
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
        .vertexAttributeDescriptionCount = VK_GUEST_ATTRS, .pVertexAttributeDescriptions = va };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = ztest ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = zwrite ? VK_TRUE : VK_FALSE,
        .depthCompareOp = (VkCompareOp)cmp };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vi,
        .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
        .pDynamicState = &dy, .layout = s_g.pipe_layout, .renderPass = s_vk.render_pass,
        .subpass = 0 };
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = (s_vkc[5]++, pvkCreateGraphicsPipelines(s_vk.device, VK_NULL_HANDLE, 1, &gci, NULL, &p));
    if (r != VK_SUCCESS) {
        VK_LOG("guest pipeline: vkCreateGraphicsPipelines failed (%d)\n", (int)r);
        return VK_NULL_HANDLE;
    }
    vk_gpipe_entry* e = &s_g.pipes[s_g.pipe_count++];
    e->vs = vs; e->fs = fs; e->depth_key = dkey; e->pipe = p;
    return p;
}

/* One vertex: all sixteen attributes as float4, attribute n at float 4n. */
static int vk_guest_push(const rsx_state* st, u32 idx)
{
    if (s_g.cpu_count == s_g.cpu_cap) {
        size_t cap = s_g.cpu_cap ? s_g.cpu_cap * 2 : 1024;
        float* n = (float*)realloc(s_g.cpu, cap * VK_GUEST_FLOATS * sizeof(float));
        if (!n) return -1;
        s_g.cpu = n; s_g.cpu_cap = cap;
    }
    float* v = s_g.cpu + s_g.cpu_count * VK_GUEST_FLOATS;
    for (u32 a = 0; a < VK_GUEST_ATTRS; a++) rsx_fetch_attrib(st, (int)a, idx, v + a * 4u);
    s_g.cpu_count++;
    return 0;
}

static void vk_guest_tri(const rsx_state* st, u32 i0, u32 i1, u32 i2)
{
    vk_guest_push(st, i0);
    vk_guest_push(st, i1);
    vk_guest_push(st, i2);
}

/* RSX_PIPELINE_CACHE=<file>: the driver's pipeline cache, loaded at start and
 * written back (to <file>.tmp, then renamed) a few seconds after new
 * pipelines were built, so a later run skips the driver's own compile. A
 * driver rejects data from another driver or device itself (the header
 * carries the vendor, device and cache UUID), so a stale file is harmless --
 * and when it is (a driver update), RSX_PIPELINE_LIST rebuilds the pipelines
 * already seen in the background (vk_plist_open, by the engine's pipelines). */
static void vk_plist_open(int rebuild);
static void vk_plist_close(void);
static SDL_atomic_t s_pw_dirty;          /* the pre-warm built pipelines: save the cache */
static void vk_pcache_init(void)
{
    const char* path = getenv("RSX_PIPELINE_CACHE");
    void* data = NULL; size_t n = 0;
    if (path && *path) {
        FILE* f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
            /* RSX_PIPELINE_CACHE_MB (default 192): a bigger file is not
             * loaded -- the driver keeps all of it in RAM for the whole run,
             * and nothing ever prunes it (a dev run/ reached 85 MB with debug
             * variants). The next save writes only what this run builds. High
             * enough that a whole playthrough's pipelines never trip it, or
             * every later start would rebuild them all. */
            const char* mb_ = getenv("RSX_PIPELINE_CACHE_MB");
            const long cap_ = (long)(mb_ && atoi(mb_) > 0 ? atoi(mb_) : 192) << 20;
            if (len > cap_) VK_LOG("pipeline cache: %s is %ld MB, over the %ld MB cap: starting fresh\n",
                                   path, len >> 20, cap_ >> 20);
            else if (len > 0 && (data = malloc((size_t)len)) &&
                     fread(data, 1, (size_t)len, f) == (size_t)len) n = (size_t)len;
            fclose(f);
        }
    }
    VkPipelineCacheCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
                                     .initialDataSize = n, .pInitialData = n ? data : NULL };
    int rebuild = n == 0;                                    /* nothing to start from */
    if (pvkCreatePipelineCache(s_vk.device, &ci, NULL, &s_g.pcache) != VK_SUCCESS) {
        ci.initialDataSize = 0; ci.pInitialData = NULL;      /* data refused: start empty */
        rebuild = 1;
        if (pvkCreatePipelineCache(s_vk.device, &ci, NULL, &s_g.pcache) != VK_SUCCESS)
            s_g.pcache = VK_NULL_HANDLE;
    }
    if (path && *path) VK_LOG("pipeline cache: %s, %zu bytes loaded\n", path, n);
    /* The header names the driver that built it (32 bytes: size, version,
     * vendor, device, pipelineCacheUUID); a driver update changes the UUID and
     * the driver then ignores the whole file without a word -- every pipeline
     * is compiled again the first time it is drawn (owner, 2026-10-20: more
     * stutter right after a graphics driver update). Say so in log.txt. */
    if (n >= 32) {
        VkPhysicalDeviceProperties p;
        pvkGetPhysicalDeviceProperties(s_vk.phys, &p);
        if (memcmp((const unsigned char*)data + 16, p.pipelineCacheUUID, VK_UUID_SIZE) != 0) {
            VK_LOG("pipeline cache: built by another driver version (graphics driver updated?) -- not usable, "
                   "rebuilding\n");
            rebuild = 1;
        }
    }
    free(data);
    { const char* s = getenv("RSX_PIPELINE_SALT"); s_g.pcache_salt = s ? (u32)strtoul(s, NULL, 0) : 0u; }
    if (s_g.pcache) vk_plist_open(rebuild);                  /* and the pre-warm, if needed */
}

/* The save runs on a thread of its own: fetching an 18 MB cache from the
 * driver and writing it took ~160 ms, and done from the draw path it was a
 * visible hitch every 5 s while new pipelines kept appearing. Reading a
 * pipeline cache's data needs no external synchronisation against pipeline
 * creation, so the render thread carries on. */
static SDL_atomic_t s_pcache_busy;
static volatile int s_pw_running;      /* the pre-warm builds into the main cache */
static void vk_ap_merge(void);
static int SDLCALL vk_pcache_save_thread(void* arg)
{
    const char* path = (const char*)arg;
    size_t n = 0;
    if (pvkGetPipelineCacheData(s_vk.device, s_g.pcache, &n, NULL) == VK_SUCCESS && n) {
        void* data = malloc(n);
        if (data && pvkGetPipelineCacheData(s_vk.device, s_g.pcache, &n, data) == VK_SUCCESS) {
            char tmp[1100];
            snprintf(tmp, sizeof tmp, "%s.tmp", path);
            FILE* f = fopen(tmp, "wb");
            if (f) {
                const int ok = fwrite(data, 1, n, f) == n;
                fclose(f);
                remove(path);
                if (!ok || rename(tmp, path) != 0) s_g.pcache_dirty++;   /* try again later */
            }
        }
        free(data);
    }
    SDL_AtomicSet(&s_pcache_busy, 0);
    return 0;
}

static void vk_pcache_save(int force)
{
    static unsigned long long s_last;
    static const char* path = (const char*)-1;
    if (path == (const char*)-1) path = getenv("RSX_PIPELINE_CACHE");
    if (SDL_AtomicSet(&s_pw_dirty, 0)) s_g.pcache_dirty++;
    if (!s_g.pcache || !s_g.pcache_dirty || !path || !*path) return;
    const unsigned long long now = ps3_qpc_us();
    if (!force && now - s_last < 5000000ull) return;      /* batch a burst of builds */
    if (!SDL_AtomicCAS(&s_pcache_busy, 0, 1)) return;     /* one save at a time */
    vk_ap_merge();
    s_last = now;
    s_g.pcache_dirty = 0;                                 /* builds from here on re-dirty it */
    if (force) { vk_pcache_save_thread((void*)path); return; }
    SDL_Thread* t = SDL_CreateThread(vk_pcache_save_thread, "pcache-save", (void*)path);
    if (t) SDL_DetachThread(t);
    else vk_pcache_save_thread((void*)path);
}

static int vk_guest_init(void)
{
    if (!rsx_vulkan_backend_guest_programs()) return 0;
    s_g.hlsl = (char*)malloc(VK_HLSL_BYTES);
    s_g.spv  = (u32*)malloc(VK_SPV_WORDS * sizeof(u32));
    if (!s_g.hlsl || !s_g.spv) { VK_LOG("guest programs: out of memory\n"); return -1; }

    VkPhysicalDeviceProperties p;
    pvkGetPhysicalDeviceProperties(s_vk.phys, &p);
    s_g.max_ubo_range = p.limits.maxUniformBufferRange;
    VkDeviceSize align = p.limits.minUniformBufferOffsetAlignment;
    if (align == 0) align = 256;
    s_g.ps_off = (VK_VPCONST_BYTES + align - 1u) / align * align;
    if (VK_VPCONST_BYTES > s_g.max_ubo_range) {
        VK_LOG("guest programs: maxUniformBufferRange %llu < VPConst\n",
               (unsigned long long)s_g.max_ubo_range);
        return -1;
    }

    const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding b[5] = {
        /* Dynamic: where the two constant blocks sit is a bind-time offset, so
         * a descriptor set depends only on the textures and samplers and the
         * batched path can reuse one across draws (vk_eng_set_for). */
        { RSX_SPIRV_VPCONST_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, both, NULL },
        { RSX_SPIRV_PSCONST_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, both, NULL },
        { RSX_SPIRV_TEXTURE_BINDING, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, RSX_MAX_TEXTURES,
          VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { RSX_SPIRV_SAMPLER_BINDING, VK_DESCRIPTOR_TYPE_SAMPLER, RSX_MAX_TEXTURES,
          VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        /* TextureCube rsx_texcube[16] : register(t48) -- the fragment
         * decompiler's cube array style (rsx_fp_cube_array_style). */
        { VK_CUBE_BINDING, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, RSX_MAX_TEXTURES,
          VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
    };
    VkDescriptorSetLayoutCreateInfo lci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 5, .pBindings = b };
    VK_CHECK(pvkCreateDescriptorSetLayout(s_vk.device, &lci, NULL, &s_g.set_layout),
             "vkCreateDescriptorSetLayout(guest)");
    VkDescriptorPoolSize ps[3] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2u * RSX_MAX_TEXTURES },
        { VK_DESCRIPTOR_TYPE_SAMPLER, RSX_MAX_TEXTURES },
    };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 1, .poolSizeCount = 3, .pPoolSizes = ps };
    VK_CHECK(pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_g.pool),
             "vkCreateDescriptorPool(guest)");
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = s_g.pool, .descriptorSetCount = 1,
                                        .pSetLayouts = &s_g.set_layout };
    VK_CHECK(pvkAllocateDescriptorSets(s_vk.device, &dai, &s_g.set), "vkAllocateDescriptorSets(guest)");
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                        .setLayoutCount = 1, .pSetLayouts = &s_g.set_layout };
    VK_CHECK(pvkCreatePipelineLayout(s_vk.device, &plci, NULL, &s_g.pipe_layout),
             "vkCreatePipelineLayout(guest)");
    if (vk_create_host_buffer(&s_g.ubo,
                              s_g.ps_off + (RSX_FP_MAX_INLINE_CONSTANTS + 1u) * 16u,
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) || !s_g.ubo.coherent) {
        VK_LOG("guest programs: no host-coherent uniform memory\n");
        return -1;
    }
    if (vk_make_dummy_cube()) { VK_LOG("guest programs: cannot create the dummy cube\n"); return -1; }
    { extern int rsx_fp_cube_array_style; rsx_fp_cube_array_style = 1; }
    vk_pcache_init();
    s_g.on = 1;
    VK_LOG("guest programs: on (HLSL -> SPIR-V through glslang)\n");
    return 0;
}

static void vk_guest_shutdown(void)
{
    vk_plist_close();                    /* stops the pre-warm: it uses the layout below */
    if (s_vk.device) {
        for (u32 i = 0; i < s_g.pipe_count; i++)
            if (s_g.pipes[i].pipe) pvkDestroyPipeline(s_vk.device, s_g.pipes[i].pipe, NULL);
        for (u32 i = 0; i < s_g.vp_count; i++)
            if (s_g.vp[i].mod) pvkDestroyShaderModule(s_vk.device, s_g.vp[i].mod, NULL);
        for (u32 i = 0; i < s_g.fp_count; i++)
            if (s_g.fp[i].mod) pvkDestroyShaderModule(s_vk.device, s_g.fp[i].mod, NULL);
        if (s_g.pipe_layout) pvkDestroyPipelineLayout(s_vk.device, s_g.pipe_layout, NULL);
        if (s_g.pool)        pvkDestroyDescriptorPool(s_vk.device, s_g.pool, NULL);
        if (s_g.set_layout)  pvkDestroyDescriptorSetLayout(s_vk.device, s_g.set_layout, NULL);
        vk_destroy_buffer(&s_g.ubo);
        vk_destroy_buffer(&s_g.vertices);
    }
    free(s_g.hlsl);
    free(s_g.spv);
    free(s_g.cpu);
    memset(&s_g, 0, sizeof s_g);
}

static int vk_ensure_buffer(vk_buffer* b, VkDeviceSize bytes, VkBufferUsageFlags usage)
{
    if (b->buf && b->size >= bytes) return 0;
    VkDeviceSize size = b->size ? b->size : 64 * 1024;
    while (size < bytes) size *= 2;
    vk_destroy_buffer(b);                 /* idle: every submission is waited on */
    if (vk_create_host_buffer(b, size, usage)) return -1;
    if (!b->coherent) { VK_LOG("vertex memory is not host-coherent\n"); return -1; }
    return 0;
}


/* Returns 1 when the draw went through the guest programs (or had nothing to
 * draw), 0 when the caller must use the fixed-function path instead. */
static int vk_guest_draw(const rsx_state* st, u32 prim, u32 first, u32 count)
{
    if (!s_g.on) return 0;
    if (vk_cube_mask(st) || vk_vtex_mask(st)) {
        if (!(s_g.warned & 1u)) {
            VK_LOG("guest programs: cube maps and vertex textures are not supported "
                   "yet; those draws use the fixed-function path\n");
            s_g.warned |= 1u;
        }
        return 0;
    }
    const int vs = vk_vp_slot(st);
    if (vs < 0) return 0;
    const u8* fp_uc = NULL;
    const int fs = vk_fp_slot(st, &fp_uc);
    if (fs < 0) return 0;
    /* The constant count must be what the program was compiled against. */
    if (rsx_fp_collect_constants(fp_uc, VK_FP_MAX_BYTES, &s_g.consts) < 0 ||
        s_g.consts.count != s_g.fp[fs].nconst)
        return 0;
    const u32 nslots = s_g.consts.count ? s_g.consts.count : 1u;
    const u32 fp_len = (nslots + 1u) * 16u;
    if (fp_len > s_g.max_ubo_range) return 0;

    const int ztest  = st->depth_test_enable ? 1 : 0;
    const int zwrite = ztest && st->depth_mask;
    VkPipeline pipe = vk_guest_pipeline(vs, fs, ztest, zwrite, vk_compare_slot(st->depth_func));
    if (!pipe) return 0;

    s_g.cpu_count = 0;
    vk_expand(st, prim, first, count, vk_guest_tri);
    if (!s_g.cpu_count) return 1;
    const VkDeviceSize bytes = (VkDeviceSize)s_g.cpu_count * VK_GUEST_FLOATS * sizeof(float);
    if (vk_ensure_buffer(&s_g.vertices, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return 0;
    memcpy(s_g.vertices.ptr, s_g.cpu, (size_t)bytes);

    /* VPConst: the 512 transform constants, then the viewport epilogue as the
     * Metal and D3D12 backends write it -- x/y identity, and z remapped from
     * GL clip space to [0, 1] when the guest programmed a z scale. */
    u8* vpc = (u8*)s_g.ubo.ptr;
    memcpy(vpc, st->vertex_constants, RSX_MAX_VERTEX_CONSTANTS * 16u);
    float* vpx = (float*)(vpc + RSX_MAX_VERTEX_CONSTANTS * 16u);
    vpx[0] = vpx[1] = vpx[3] = 1.0f;
    vpx[4] = vpx[5] = vpx[7] = 0.0f;
    if (st->viewport_scale[2] != 0.0f) { vpx[2] = st->viewport_scale[2]; vpx[6] = st->viewport_offset[2]; }
    else                                { vpx[2] = 1.0f;                  vpx[6] = 0.0f; }
    /* PSConstants: inline constants as host-order bit patterns, then fp_alpha. */
    u8* fpc = (u8*)s_g.ubo.ptr + s_g.ps_off;
    memset(fpc, 0, fp_len);
    if (s_g.consts.count) memcpy(fpc, s_g.consts.values, s_g.consts.count * 16u);
    float* alpha = (float*)(fpc + nslots * 16u);
    alpha[0] = rsx_fp_alpha_ref(st->alpha_ref, st->surface_format & 0x1Fu);

    /* Descriptors: rewritten per draw, which is safe because every submission
     * is waited on before the next one is recorded. */
    VkDescriptorBufferInfo bi[2] = {
        { s_g.ubo.buf, 0, VK_VPCONST_BYTES },
        { s_g.ubo.buf, s_g.ps_off, fp_len },
    };
    VkDescriptorImageInfo ii[RSX_MAX_TEXTURES], si[RSX_MAX_TEXTURES];
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        ii[u] = (VkDescriptorImageInfo){ VK_NULL_HANDLE,
                                         s_vk.tex_ready[u] ? s_vk.tex[u].view : s_vk.dummy.view,
                                         VK_IMAGE_LAYOUT_GENERAL };
        si[u] = (VkDescriptorImageInfo){ s_vk.tex_sampler[u] ? s_vk.tex_sampler[u] : s_vk.sampler,
                                         VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    }
    VkWriteDescriptorSet w[4] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPIRV_VPCONST_BINDING, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .pBufferInfo = &bi[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPIRV_PSCONST_BINDING, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .pBufferInfo = &bi[1] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPIRV_TEXTURE_BINDING, .descriptorCount = RSX_MAX_TEXTURES,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = ii },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPIRV_SAMPLER_BINDING, .descriptorCount = RSX_MAX_TEXTURES,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = si },
    };
    pvkUpdateDescriptorSets(s_vk.device, 4, w, 0, NULL);

    if (vk_begin()) return 0;
    vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image(s_vk.depth.img, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = s_vk.render_pass, .framebuffer = s_vk.framebuffer,
                                  .renderArea = { { 0, 0 }, { s_vk.width, s_vk.height } } };
    pvkCmdBeginRenderPass(s_vk.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    s_vk_foreign_bind = 1;   /* the batched path's cached binds are gone */
    VkViewport vpt = { 0.0f, 0.0f, (float)s_vk.width, (float)s_vk.height, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { s_vk.width, s_vk.height } };
    pvkCmdSetViewport(s_vk.cmd, 0, 1, &vpt);
    pvkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
    { static const uint32_t zero_offs[2] = { 0, 0 };
      pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_g.pipe_layout,
                               0, 1, &s_g.set, 2, zero_offs); }
    VkDeviceSize off = 0;
    pvkCmdBindVertexBuffers(s_vk.cmd, 0, 1, &s_g.vertices.buf, &off);
    pvkCmdDraw(s_vk.cmd, (u32)s_g.cpu_count, 1, 0, 0);
    pvkCmdEndRenderPass(s_vk.cmd);
    if (vk_submit_and_wait()) return 0;
    s_g.draws++;
    return 1;
}

static void vk_draw(u32 prim, u32 first, u32 count)
{
    const rsx_state* st = s_vk.state;
    if (!st || !s_vk.device || count < 3) return;
    if (vk_guest_draw(st, prim, first, count)) return;

    s_vk.cpu_count = 0;
    vk_expand(st, prim, first, count, vk_emit_tri);
    if (!s_vk.cpu_count) return;

    const VkDeviceSize bytes = (VkDeviceSize)s_vk.cpu_count * sizeof(vk_vertex);
    if (vk_ensure_vertex_buffer(bytes)) return;
    memcpy(s_vk.vertices.ptr, s_vk.cpu_verts, (size_t)bytes);

    /* Depth writes need the test enabled, as on the hardware and in both
     * reference backends. */
    const int ztest  = st->depth_test_enable ? 1 : 0;
    const int zwrite = ztest && st->depth_mask;
    VkPipeline pipe = vk_get_pipeline(ztest, zwrite, vk_compare_slot(st->depth_func));
    if (!pipe) return;
    const int32_t textured = (s_vk.tex_ready[0] && st->vertex_attribs[8].enabled) ? 1 : 0;

    if (vk_begin()) return;
    vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image(s_vk.depth.img, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = s_vk.render_pass, .framebuffer = s_vk.framebuffer,
                                  .renderArea = { { 0, 0 }, { s_vk.width, s_vk.height } } };
    pvkCmdBeginRenderPass(s_vk.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    s_vk_foreign_bind = 1;   /* the batched path's cached binds are gone */
    /* Whole target, as the null backend's ndc_to_px maps it. */
    VkViewport vp = { 0.0f, 0.0f, (float)s_vk.width, (float)s_vk.height, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { s_vk.width, s_vk.height } };
    pvkCmdSetViewport(s_vk.cmd, 0, 1, &vp);
    pvkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
    pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_vk.pipe_layout,
                             0, 1, &s_vk.desc_set, 0, NULL);
    pvkCmdPushConstants(s_vk.cmd, s_vk.pipe_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof textured, &textured);
    VkDeviceSize off = 0;
    pvkCmdBindVertexBuffers(s_vk.cmd, 0, 1, &s_vk.vertices.buf, &off);
    pvkCmdDraw(s_vk.cmd, (u32)s_vk.cpu_count, 1, 0, 0);
    pvkCmdEndRenderPass(s_vk.cmd);
    vk_submit_and_wait();
}

static void vk_cb_draw_arrays(void* ud, u32 primitive, u32 first, u32 count)
{
    (void)ud;
    vk_draw(primitive, first, count);
}

static void vk_cb_draw_indexed(void* ud, u32 primitive, u32 offset, u32 count)
{
    /* Not read yet, as in the null backend: the harness geometry is
     * non-indexed. Real titles need this; it lands with the index fetch. */
    (void)ud; (void)primitive; (void)offset; (void)count;
}

/* ---------------------------------------------------------------------------
 * Clear and present
 * -------------------------------------------------------------------------*/
static void vk_cb_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{
    (void)ud; (void)stencil;                       /* no stencil buffer yet */
    s_vk.clear_argb = color;
    /* CLEAR_SURFACE bits: 0x01 depth, 0x02 stencil, 0xF0 the colour channels
     * (all or nothing, as in the null backend). */
    const int do_color = (flags & 0xF0u) != 0;
    const int do_depth = (flags & 0x01u) != 0;
    if (!s_vk.device || (!do_color && !do_depth)) return;

    if (vk_begin()) return;
    if (do_color) {
        VkClearColorValue v;
        v.float32[0] = (float)((color >> 16) & 0xFFu) / 255.0f;   /* R */
        v.float32[1] = (float)((color >>  8) & 0xFFu) / 255.0f;   /* G */
        v.float32[2] = (float)( color        & 0xFFu) / 255.0f;   /* B */
        v.float32[3] = (float)((color >> 24) & 0xFFu) / 255.0f;   /* A */
        VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        pvkCmdClearColorImage(s_vk.cmd, s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
    }
    if (do_depth) {
        VkClearDepthStencilValue d = { depth, 0 };
        VkImageSubresourceRange r = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        vk_barrier_image(s_vk.depth.img, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_GENERAL);
        pvkCmdClearDepthStencilImage(s_vk.cmd, s_vk.depth.img, VK_IMAGE_LAYOUT_GENERAL, &d, 1, &r);
    }
    vk_submit_and_wait();
}

/* ---------------------------------------------------------------------------
 * Optional window: PS3RECOMP_VK_WINDOW=1
 *
 * An SDL2 window with a Vulkan swapchain. Rendering still targets the
 * offscreen image, so readback and every test are unchanged; a present then
 * also blits that image into the next swapchain image. Any failure on this
 * path drops back to headless with a message instead of failing the run.
 * -------------------------------------------------------------------------*/
static int vk_env_on(const char* name)
{
    const char* v = getenv(name);
    return v && v[0] && v[0] != '0';
}

static void vk_window_drop(const char* why)
{
    if (why) VK_LOG("window disabled, continuing headless: %s\n", why);
    if (s_vk.device && pvkDeviceWaitIdle) {
        pvkDeviceWaitIdle(s_vk.device);
        for (u32 i = 0; i < 8; i++)
            if (s_vk.sc_done[i]) {
                pvkDestroySemaphore(s_vk.device, s_vk.sc_done[i], NULL);
                s_vk.sc_done[i] = VK_NULL_HANDLE;
            }
        if (s_vk.sem_acquire) pvkDestroySemaphore(s_vk.device, s_vk.sem_acquire, NULL);
        if (s_vk.swapchain && pvkDestroySwapchainKHR)
            pvkDestroySwapchainKHR(s_vk.device, s_vk.swapchain, NULL);
    }
    if (s_vk.surface && s_vk.instance && pvkDestroySurfaceKHR)
        pvkDestroySurfaceKHR(s_vk.instance, s_vk.surface, NULL);
    if (s_vk.window) {
        SDL_DestroyWindow(s_vk.window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
    s_vk.window = NULL;
    s_vk.surface = VK_NULL_HANDLE;
    s_vk.swapchain = VK_NULL_HANDLE;
    s_vk.sem_acquire = VK_NULL_HANDLE;
    s_vk.sc_count = 0;
    s_vk.windowed = 0;
}

static void vk_window_open(const char* title)
{
    if (!vk_env_on("PS3RECOMP_VK_WINDOW")) return;
    rsx_host_ui_load_options();       /* also the DPI hint, before video init */
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        VK_LOG("window disabled, continuing headless: SDL video init failed: %s\n", SDL_GetError());
        return;
    }
    /* Controllers, if cellPadInit has not started them already (it usually
     * has: the game calls it before the window opens -- v79 meant this to be
     * the place, and the owner's 0.8 log showed it is not). Same hints as
     * cellPad.c pad_init_backend, which must precede the first controller
     * opened; Raw Input off there is what makes Xbox pads read on any thread. */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");   /* Xbox pads through XInput: cellPad.c pad_init_backend */
    SDL_SetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "0");  /* read by the pad poll only, not this pump: the same */
    SDL_SetHint(SDL_HINT_AUTO_UPDATE_SENSORS, "0");
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
        VK_LOG("controller init failed here (cellPadInit retries): %s\n", SDL_GetError());
    Uint32 flags = SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN;
    int win_w = (int)s_vk.width, win_h = (int)s_vk.height;
#ifdef __ANDROID__
    if (vk_env_on("PS3RECOMP_VK_FULLSCREEN")) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
#else
    /* Display mode and window size from the settings menu (rsx_host_ui.h). */
    { unsigned uf = 0; rsx_host_ui_initial_window(&win_w, &win_h, &uf); flags |= uf; }
#endif
    s_vk.window = SDL_CreateWindow(title && title[0] ? title : "ps3recomp",
                                   SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   win_w, win_h, flags);
    s_vk.windowed = 1;          /* so vk_window_drop also shuts SDL video down */
    if (!s_vk.window) {
        char why[256];
        snprintf(why, sizeof why, "SDL_CreateWindow failed: %s", SDL_GetError());
        vk_window_drop(why);
        return;
    }
    /* A game composes no text: SDL starts text input (an IME / Text Services
     * attachment on Windows) for every window; turn it off. */
    SDL_StopTextInput();
    unsigned n = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(s_vk.window, &n, NULL) || n == 0 || n > 16 ||
        !SDL_Vulkan_GetInstanceExtensions(s_vk.window, &n, s_vk.inst_exts)) {
        char why[256];
        snprintf(why, sizeof why, "no Vulkan surface extensions (%s)", SDL_GetError());
        vk_window_drop(why);
        return;
    }
    s_vk.inst_ext_count = n;
    const char* hold = getenv("PS3RECOMP_VK_HOLD");
    s_vk.hold_seconds = hold ? atof(hold) : 0.0;
    VK_LOG("window: SDL video driver '%s', %u surface extension(s)\n",
           SDL_GetCurrentVideoDriver(), n);
}

static VkPresentModeKHR vk_pick_present_mode(void);
static int vk_create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(pvkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_vk.phys, s_vk.surface, &caps),
             "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        VK_LOG("swapchain images cannot be blit destinations\n"); return -1;
    }

    u32 nf = 0;
    VK_CHECK(pvkGetPhysicalDeviceSurfaceFormatsKHR(s_vk.phys, s_vk.surface, &nf, NULL),
             "vkGetPhysicalDeviceSurfaceFormatsKHR");
    if (nf == 0) { VK_LOG("surface reports no formats\n"); return -1; }
    if (nf > 64) nf = 64;
    VkSurfaceFormatKHR fmts[64];
    VK_CHECK(pvkGetPhysicalDeviceSurfaceFormatsKHR(s_vk.phys, s_vk.surface, &nf, fmts),
             "vkGetPhysicalDeviceSurfaceFormatsKHR");
    /* UNORM, not SRGB: the offscreen target holds the guest's values as-is. */
    VkSurfaceFormatKHR fmt = fmts[0];
    if (nf == 1 && fmts[0].format == VK_FORMAT_UNDEFINED) {
        fmt.format = VK_FORMAT_B8G8R8A8_UNORM;
    } else {
        static const VkFormat pref[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
        int found = 0;
        for (size_t k = 0; k < 2 && !found; k++)
            for (u32 i = 0; i < nf; i++)
                if (fmts[i].format == pref[k]) { fmt = fmts[i]; found = 1; break; }
    }
    VkFormatProperties fp;
    pvkGetPhysicalDeviceFormatProperties(s_vk.phys, fmt.format, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        VK_LOG("swapchain format %d cannot be a blit destination\n", (int)fmt.format); return -1;
    }

    VkExtent2D ext = caps.currentExtent;
    /* Android reports the panel's native orientation: a landscape app on a
     * portrait-native phone gets currentTransform ROTATE_90/270 and a
     * portrait extent. We blit unrotated, so ask for IDENTITY (the
     * compositor rotates) and swap the extent to match. */
    VkSurfaceTransformFlagBitsKHR xform = caps.currentTransform;
    if ((caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) &&
        xform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) {
        if (ext.width != 0xFFFFFFFFu &&
            (xform & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR |
                      VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR |
                      VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_90_BIT_KHR |
                      VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_270_BIT_KHR))) {
            u32 t = ext.width; ext.width = ext.height; ext.height = t;
        }
        xform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    }
    if (ext.width == 0xFFFFFFFFu) {
        int w = 0, hh = 0;
        SDL_Vulkan_GetDrawableSize(s_vk.window, &w, &hh);
        ext.width = (u32)w; ext.height = (u32)hh;
        if (ext.width  < caps.minImageExtent.width)  ext.width  = caps.minImageExtent.width;
        if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
        if (ext.width  > caps.maxImageExtent.width)  ext.width  = caps.maxImageExtent.width;
        if (ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
    }
    /* A minimised window has a 0x0 surface. That is not a failure: the caller
     * keeps the old swapchain, stops presenting and asks again later (1). */
    if (!ext.width || !ext.height) return 1;

    u32 count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;

    static const VkCompositeAlphaFlagBitsKHR alphas[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
    };
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (size_t k = 0; k < 4; k++)
        if (caps.supportedCompositeAlpha & alphas[k]) { alpha = alphas[k]; break; }

    /* MAILBOX keeps the swapchain from ever blocking the frame thread (the
     * FIFO walker runs there): the title paces itself with its own flip and
     * vblank waits, and FIFO's acquire stalled the RSX whenever the display
     * refresh and the title's pacing drifted -- and caps at the refresh.
     * Three images so a frame can be queued while one is shown. */
    if (vk_pick_present_mode() == VK_PRESENT_MODE_MAILBOX_KHR && count < 3) count = 3;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    VkSwapchainKHR old = s_vk.swapchain;
    VkSwapchainCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = s_vk.surface, .minImageCount = count,
        .imageFormat = fmt.format, .imageColorSpace = fmt.colorSpace,
        .imageExtent = ext, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = xform, .compositeAlpha = alpha,
        .presentMode = vk_pick_present_mode(),
        .clipped = VK_TRUE, .oldSwapchain = old,
    };
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkResult r = pvkCreateSwapchainKHR(s_vk.device, &sci, NULL, &sc);
    if (old) pvkDestroySwapchainKHR(s_vk.device, old, NULL);
    s_vk.swapchain = VK_NULL_HANDLE;
    if (r != VK_SUCCESS) { VK_LOG("vkCreateSwapchainKHR failed (VkResult %d)\n", (int)r); return -1; }
    s_vk.swapchain = sc;

    u32 n = 0;
    VK_CHECK(pvkGetSwapchainImagesKHR(s_vk.device, sc, &n, NULL), "vkGetSwapchainImagesKHR");
    if (n == 0 || n > 8) { VK_LOG("unsupported swapchain image count %u\n", n); return -1; }
    VK_CHECK(pvkGetSwapchainImagesKHR(s_vk.device, sc, &n, s_vk.sc_images), "vkGetSwapchainImagesKHR");

    for (u32 i = 0; i < 8; i++)
        if (s_vk.sc_done[i]) {
            pvkDestroySemaphore(s_vk.device, s_vk.sc_done[i], NULL);
            s_vk.sc_done[i] = VK_NULL_HANDLE;
        }
    VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (u32 i = 0; i < n; i++)
        VK_CHECK(pvkCreateSemaphore(s_vk.device, &semi, NULL, &s_vk.sc_done[i]), "vkCreateSemaphore");

    s_vk.sc_extent = ext;
    s_vk.sc_count = n;
    VK_LOG("window swapchain %ux%u, %u images, format %d\n", ext.width, ext.height, n, (int)fmt.format);
    return 0;
}

/* Set while the window has no drawable area (minimised). Dropping the window
 * for that left the title headless for the rest of the session: minimise once
 * and the picture never came back, while the game and its audio ran on. */
static int s_vk_win_minimized = 0;

/* Rebuild the swapchain; a 0x0 surface parks the window instead of losing it. */
static void vk_swapchain_rebuild(void)
{
    int r = vk_create_swapchain();
    if (r == 1) {
        if (!s_vk_win_minimized) VK_LOG("window has no drawable area (minimised) -- presenting paused\n");
        s_vk_win_minimized = 1;
        return;
    }
    if (r) { vk_window_drop("the swapchain could not be recreated"); return; }
    if (s_vk_win_minimized) VK_LOG("window restored -- presenting resumed\n");
    s_vk_win_minimized = 0;
}

static void vk_swapchain_lost(void)
{
    pvkDeviceWaitIdle(s_vk.device);
    vk_swapchain_rebuild();
}

/* The window behind the surface went away (Android destroys the native
 * window when the app is backgrounded and makes a new one on resume):
 * rebuild the surface itself, then the swapchain on it. */
static void vk_surface_lost(void)
{
    pvkDeviceWaitIdle(s_vk.device);
    if (s_vk.swapchain) { pvkDestroySwapchainKHR(s_vk.device, s_vk.swapchain, NULL); s_vk.swapchain = VK_NULL_HANDLE; }
    if (s_vk.surface) { pvkDestroySurfaceKHR(s_vk.instance, s_vk.surface, NULL); s_vk.surface = VK_NULL_HANDLE; }
    if (!SDL_Vulkan_CreateSurface(s_vk.window, s_vk.instance, &s_vk.surface)) {
        char why[256];
        snprintf(why, sizeof why, "SDL_Vulkan_CreateSurface (after surface loss) failed: %s", SDL_GetError());
        vk_window_drop(why);
        return;
    }
    VkBool32 ok = VK_FALSE;
    if (pvkGetPhysicalDeviceSurfaceSupportKHR(s_vk.phys, s_vk.queue_family, s_vk.surface, &ok) != VK_SUCCESS || !ok) {
        vk_window_drop("the recreated surface cannot be presented from the graphics queue");
        return;
    }
    VK_LOG("window surface recreated\n");
    vk_swapchain_rebuild();
}

/* SUBOPTIMAL also comes back every frame when we deliberately present with
 * an IDENTITY transform on a rotated panel (see vk_create_swapchain), so only
 * rebuild when the surface size really changed. */
static void vk_swapchain_suboptimal(void)
{
    VkSurfaceCapabilitiesKHR caps;
    if (pvkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_vk.phys, s_vk.surface, &caps) != VK_SUCCESS) return;
    VkExtent2D e = caps.currentExtent;
    if (e.width == 0xFFFFFFFFu) return;
    if (caps.currentTransform & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR |
                                 VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_90_BIT_KHR |
                                 VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_270_BIT_KHR)) {
        u32 t = e.width; e.width = e.height; e.height = t;
    }
    if (e.width != s_vk.sc_extent.width || e.height != s_vk.sc_extent.height) vk_swapchain_lost();
}

/* ---------------------------------------------------------------------------
 * Presentation: aspect ratio, upscaling, brightness and the host overlay
 * (the settings menu and FPS counter, rsx_host_ui.h)
 *
 * The frame reaches the window one of two ways:
 *   - plain: the display image is blitted into the swapchain image, fitted to
 *     the guest's 16:9 with black bars (or stretched, as before), linear or
 *     nearest -- what every frame did until now, minus the stretching;
 *   - through a pass, when the sharp upscaler, a brightness change or the
 *     overlay is on: a shader draws the display image into a window-sized
 *     image (bicubic + contrast-adaptive sharpening, brightness curve), the
 *     overlay is drawn on top at the window's own resolution, and the result
 *     is copied 1:1 into the swapchain image.
 * The shaders are HLSL, translated by the same glslang path as the guest's.
 * Any failure here falls back to the plain path for the rest of the run.
 * -------------------------------------------------------------------------*/
static int vk_eng_upload_rows(VkImage img, u32 layer, u32 mip, u32 w, u32 h,
                              size_t tight, u32 rows, const void* src, u32 row_bytes);

static const char* const k_pp_vs =
"struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
"VSOut main(uint id : SV_VertexID) {\n"
"    VSOut o;\n"
"    float2 p = float2(id == 1u ? 3.0 : -1.0, id == 2u ? -3.0 : 1.0);\n"
"    o.pos = float4(p, 0.0, 1.0);\n"
"    o.uv = float2((p.x + 1.0) * 0.5, (1.0 - p.y) * 0.5);\n"
"    return o;\n"
"}\n";

/* pc_src = (w, h, 1/w, 1/h) of the display image; pc_par = (mode, sharpness
 * 0..1, brightness exponent, 0). mode 1: Catmull-Rom (five bilinear taps)
 * then a contrast-adaptive sharpen whose strength falls where the
 * neighbourhood is already contrasty, so edges do not ring. */
static const char* const k_pp_ps =
"Texture2D src : register(t0);\n"
"SamplerState smp : register(s0);\n"
"[[vk::push_constant]] cbuffer PC { float4 pc_src; float4 pc_par; };\n"
"float3 tap(float2 uv) { return src.SampleLevel(smp, uv, 0.0).rgb; }\n"
"float3 bicubic(float2 uv) {\n"
"    float2 p = uv * pc_src.xy - 0.5;\n"
"    float2 f = frac(p);\n"
"    float2 tc = (floor(p) + 0.5) * pc_src.zw;\n"
"    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));\n"
"    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
"    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));\n"
"    float2 w3 = f * f * (-0.5 + 0.5 * f);\n"
"    float2 w12 = w1 + w2;\n"
"    float2 t0 = tc - pc_src.zw, t3 = tc + 2.0 * pc_src.zw, t12 = tc + (w2 / w12) * pc_src.zw;\n"
"    float3 c = tap(float2(t12.x, t0.y)) * (w12.x * w0.y) + tap(float2(t0.x, t12.y)) * (w0.x * w12.y)\n"
"             + tap(t12) * (w12.x * w12.y) + tap(float2(t3.x, t12.y)) * (w3.x * w12.y)\n"
"             + tap(float2(t12.x, t3.y)) * (w12.x * w3.y);\n"
"    float ws = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;\n"
"    return c / ws;\n"
"}\n"
"float3 sharpen(float2 uv, float3 c) {\n"
"    float2 t = pc_src.zw;\n"
"    float3 n = tap(uv + float2(0.0, -t.y)), s = tap(uv + float2(0.0, t.y));\n"
"    float3 e = tap(uv + float2(t.x, 0.0)), w = tap(uv + float2(-t.x, 0.0));\n"
"    float3 mn = min(c, min(min(n, s), min(e, w)));\n"
"    float3 mx = max(c, max(max(n, s), max(e, w)));\n"
"    float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4)));\n"
"    float3 wt = amp * (-1.0 / lerp(8.0, 4.5, pc_par.y));\n"
"    return saturate((c + (n + s + e + w) * wt) / (1.0 + 4.0 * wt));\n"
"}\n"
"float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
"    float3 c;\n"
"    if (pc_par.x > 0.5) c = sharpen(uv, saturate(bicubic(uv)));\n"
"    else c = tap(uv);\n"
"    return float4(pow(saturate(c), pc_par.z), 1.0);\n"
"}\n";

static const char* const k_ui_vs =
"[[vk::push_constant]] cbuffer PC { float4 pc_src; float4 pc_par; };\n"
"struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };\n"
"struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 col : COLOR0; };\n"
"VSOut main(VSIn i) {\n"
"    VSOut o;\n"
"    o.pos = float4(i.pos.x * pc_src.x - 1.0, 1.0 - i.pos.y * pc_src.y, 0.0, 1.0);\n"
"    o.uv = i.uv; o.col = i.col;\n"
"    return o;\n"
"}\n";

static const char* const k_ui_ps =
"Texture2D font : register(t0);\n"
"SamplerState smp : register(s0);\n"
"float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0, float4 col : COLOR0) : SV_Target {\n"
"    float4 t = font.SampleLevel(smp, uv, 0.0);\n"
"    return float4(col.rgb * t.rgb, col.a * t.a);\n"
"}\n";

#define VK_UI_MAX_VERTS 98304u

static struct {
    int failed, ready;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkRenderPass rp;
    VkPipeline post, ui;
    VkSampler lin, near_;
    vk_image img; u32 w, h; VkFramebuffer fb;
    vk_image font; unsigned font_gen, sheet_gen; int atlas_w, atlas_h;
    rsx_ui_font cpu_font;
    rsx_ui_vtx* verts;
    unsigned swap_gen, sampler_gen;
    char shot[512];                 /* dev: write the next composed frame here */
} s_pp;

static int vk_pp_shader(const char* hlsl, int stage, VkShaderModule* out)
{
    u32 words = 0;
    if (rsx_hlsl_to_spirv(hlsl, stage, s_g.spv, VK_SPV_WORDS, &words, s_g.log, sizeof s_g.log) || !words) {
        VK_LOG("presentation shader: %s\n", s_g.log);
        return -1;
    }
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = (size_t)words * 4u, .pCode = s_g.spv };
    return pvkCreateShaderModule(s_vk.device, &ci, NULL, out) == VK_SUCCESS ? 0 : -1;
}

static VkPipeline vk_pp_pipeline(VkShaderModule vs, VkShaderModule fs, int ui)
{
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = fs, .pName = "main" } };
    VkVertexInputBindingDescription vb = { 0, sizeof(rsx_ui_vtx), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[3] = {
        { 0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(rsx_ui_vtx, x) },
        { 1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(rsx_ui_vtx, u) },
        { 2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(rsx_ui_vtx, rgba) } };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = ui ? 1u : 0u, .pVertexBindingDescriptions = &vb,
        .vertexAttributeDescriptionCount = ui ? 3u : 0u, .pVertexAttributeDescriptions = va };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = {
        .blendEnable = ui ? VK_TRUE : VK_FALSE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    VkPipelineDepthStencilStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pDepthStencilState = &ds, .pColorBlendState = &cb, .pDynamicState = &dy,
        .layout = s_pp.layout, .renderPass = s_pp.rp, .subpass = 0 };
    VkPipeline p = VK_NULL_HANDLE;
    if ((s_vkc[5]++, pvkCreateGraphicsPipelines(s_vk.device, VK_NULL_HANDLE, 1, &gci, NULL, &p)) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return p;
}

/* Build the pass's objects once; 0 when usable. */
static int vk_pp_init(void)
{
    if (s_pp.ready) return 0;
    if (s_pp.failed || !s_bt.ready || !s_g.spv || !rsx_hlsl_to_spirv_available()) return -1;
    s_pp.failed = 1;
    VkDescriptorSetLayoutBinding b[2] = {
        { RSX_SPIRV_TEXTURE_BINDING, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { RSX_SPIRV_SAMPLER_BINDING, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL } };
    VkDescriptorSetLayoutCreateInfo dl = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = 2, .pBindings = b };
    if (pvkCreateDescriptorSetLayout(s_vk.device, &dl, NULL, &s_pp.dsl) != VK_SUCCESS) return -1;
    VkPushConstantRange pr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32 };
    VkPipelineLayoutCreateInfo pl = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                      .setLayoutCount = 1, .pSetLayouts = &s_pp.dsl,
                                      .pushConstantRangeCount = 1, .pPushConstantRanges = &pr };
    if (pvkCreatePipelineLayout(s_vk.device, &pl, NULL, &s_pp.layout) != VK_SUCCESS) return -1;
    VkAttachmentDescription att = { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_GENERAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 .colorAttachmentCount = 1, .pColorAttachments = &cref };
    VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                   .attachmentCount = 1, .pAttachments = &att, .subpassCount = 1, .pSubpasses = &sub };
    if (pvkCreateRenderPass(s_vk.device, &rci, NULL, &s_pp.rp) != VK_SUCCESS) return -1;
    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.0f,
        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK };
    if (pvkCreateSampler(s_vk.device, &sci, NULL, &s_pp.lin) != VK_SUCCESS) return -1;
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    if (pvkCreateSampler(s_vk.device, &sci, NULL, &s_pp.near_) != VK_SUCCESS) return -1;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE, uvs = VK_NULL_HANDLE, ufs = VK_NULL_HANDLE;
    int bad = vk_pp_shader(k_pp_vs, RSX_SHADER_STAGE_VERTEX, &vs) || vk_pp_shader(k_pp_ps, RSX_SHADER_STAGE_FRAGMENT, &fs) ||
              vk_pp_shader(k_ui_vs, RSX_SHADER_STAGE_VERTEX, &uvs) || vk_pp_shader(k_ui_ps, RSX_SHADER_STAGE_FRAGMENT, &ufs);
    if (!bad) {
        s_pp.post = vk_pp_pipeline(vs, fs, 0);
        s_pp.ui = vk_pp_pipeline(uvs, ufs, 1);
        bad = !s_pp.post || !s_pp.ui;
    }
    if (vs) pvkDestroyShaderModule(s_vk.device, vs, NULL);
    if (fs) pvkDestroyShaderModule(s_vk.device, fs, NULL);
    if (uvs) pvkDestroyShaderModule(s_vk.device, uvs, NULL);
    if (ufs) pvkDestroyShaderModule(s_vk.device, ufs, NULL);
    if (bad) { VK_LOG("presentation pass unavailable; plain blit to the window\n"); return -1; }
    s_pp.verts = (rsx_ui_vtx*)malloc(VK_UI_MAX_VERTS * sizeof(rsx_ui_vtx));
    if (!s_pp.verts) return -1;
    s_pp.failed = 0; s_pp.ready = 1;
    VK_LOG("presentation pass ready (upscaling, brightness, settings overlay)\n");
    return 0;
}

static void vk_pp_shutdown(void)
{
    if (!s_vk.device) return;
    if (s_pp.fb) pvkDestroyFramebuffer(s_vk.device, s_pp.fb, NULL);
    vk_destroy_image(&s_pp.img);
    vk_destroy_image(&s_pp.font);
    if (s_pp.post) pvkDestroyPipeline(s_vk.device, s_pp.post, NULL);
    if (s_pp.ui) pvkDestroyPipeline(s_vk.device, s_pp.ui, NULL);
    if (s_pp.lin) pvkDestroySampler(s_vk.device, s_pp.lin, NULL);
    if (s_pp.near_) pvkDestroySampler(s_vk.device, s_pp.near_, NULL);
    if (s_pp.rp) pvkDestroyRenderPass(s_vk.device, s_pp.rp, NULL);
    if (s_pp.layout) pvkDestroyPipelineLayout(s_vk.device, s_pp.layout, NULL);
    if (s_pp.dsl) pvkDestroyDescriptorSetLayout(s_vk.device, s_pp.dsl, NULL);
    free(s_pp.verts);
    free(s_pp.cpu_font.pixels);
    memset(&s_pp, 0, sizeof s_pp);
}

/* The window-sized image the pass draws into. */
static int vk_pp_target(u32 w, u32 h)
{
    if (s_pp.img.img && s_pp.w == w && s_pp.h == h) return 0;
    if (s_pp.fb && !vk_bt_defer_framebuffer(s_pp.fb)) pvkDestroyFramebuffer(s_vk.device, s_pp.fb, NULL);
    s_pp.fb = VK_NULL_HANDLE;
    vk_destroy_image(&s_pp.img);
    s_pp.w = s_pp.h = 0;
    if (vk_create_image(&s_pp.img, VK_FORMAT_R8G8B8A8_UNORM, w, h,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT)) { vk_destroy_image(&s_pp.img); return -1; }
    VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = s_pp.rp,
                                    .attachmentCount = 1, .pAttachments = &s_pp.img.view,
                                    .width = w, .height = h, .layers = 1 };
    if ((s_vkc[2]++, pvkCreateFramebuffer(s_vk.device, &fci, NULL, &s_pp.fb)) != VK_SUCCESS) {
        vk_destroy_image(&s_pp.img); return -1;
    }
    s_pp.w = w; s_pp.h = h;
    return 0;
}

/* The overlay's atlas at the size this window wants, on the GPU: the font
 * (white, coverage in alpha) with the save screen's icon sheet below it
 * (rsx_host_ui_atlas_size). */
static int vk_pp_font(u32 w, u32 h)
{
    const int font_new = rsx_host_ui_font_for(&s_pp.cpu_font, (int)w, (int)h);
    const unsigned sheet_gen = rsx_host_ui_sheet(NULL);
    if (!font_new && s_pp.font.img && s_pp.font_gen == s_pp.cpu_font.gen && s_pp.sheet_gen == sheet_gen) return 0;
    if (!s_pp.cpu_font.pixels) return -1;
    const rsx_ui_font* f = &s_pp.cpu_font;
    int aw = 0, ah = 0;
    rsx_host_ui_atlas_size(f, &aw, &ah);
    u8* px = (u8*)calloc((size_t)aw * (size_t)ah, 4);
    if (!px) return -1;
    for (int y = 0; y < f->atlas_h; y++)
        for (int x = 0; x < f->atlas_w; x++) {
            u8* d = px + ((size_t)y * aw + x) * 4;
            d[0] = d[1] = d[2] = 255; d[3] = f->pixels[(size_t)y * f->atlas_w + x];
        }
    const unsigned got = rsx_host_ui_sheet(px + (size_t)f->atlas_h * aw * 4);
    if (aw != RSX_UI_SHEET_W) {   /* the sheet came in at its own width: spread its rows */
        for (int y = RSX_UI_SHEET_H - 1; y >= 0; y--) {
            memmove(px + ((size_t)(f->atlas_h + y) * aw) * 4, px + (size_t)f->atlas_h * aw * 4 + (size_t)y * RSX_UI_SHEET_W * 4,
                    (size_t)RSX_UI_SHEET_W * 4);
            memset(px + ((size_t)(f->atlas_h + y) * aw + RSX_UI_SHEET_W) * 4, 0, (size_t)(aw - RSX_UI_SHEET_W) * 4);
        }
    }
    if (!s_pp.font.img || s_pp.atlas_w != aw || s_pp.atlas_h != ah) {
        vk_destroy_image(&s_pp.font);
        if (vk_create_image(&s_pp.font, VK_FORMAT_R8G8B8A8_UNORM, (u32)aw, (u32)ah,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT)) {
            vk_destroy_image(&s_pp.font); free(px); return -1;
        }
    }
    if (vk_begin()) { free(px); return -1; }
    vk_barrier_image(s_pp.font.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    const int rc = vk_eng_upload_rows(s_pp.font.img, 0, 0, (u32)aw, (u32)ah, (size_t)aw * 4,
                                      (u32)ah, px, (u32)aw * 4);
    free(px);
    if (rc) return -1;
    s_pp.font_gen = f->gen; s_pp.sheet_gen = got; s_pp.atlas_w = aw; s_pp.atlas_h = ah;
    return 0;
}

static VkDescriptorSet vk_pp_set(VkImageView view, VkSampler smp)
{
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = s_bt.s[s_bt.cur].pool, .descriptorSetCount = 1,
                                        .pSetLayouts = &s_pp.dsl };
    if (pvkAllocateDescriptorSets(s_vk.device, &dai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkDescriptorImageInfo ii = { VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo si = { smp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    VkWriteDescriptorSet w[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = RSX_SPIRV_TEXTURE_BINDING,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &ii },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = RSX_SPIRV_SAMPLER_BINDING,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = &si } };
    pvkUpdateDescriptorSets(s_vk.device, 2, w, 0, NULL);
    return set;
}

/* Where the guest's frame goes in a w x h window. */
static void vk_pp_rect(const rsx_host_options* o, u32 w, u32 h, int32_t r[4])
{
    r[0] = 0; r[1] = 0; r[2] = (int32_t)w; r[3] = (int32_t)h;
    if (!o->keep_aspect || !s_vk.guest_w || !s_vk.guest_h) return;
    const double ga = (double)s_vk.guest_w / (double)s_vk.guest_h, wa = (double)w / (double)h;
    if (wa > ga + 1e-3) { const int32_t dw = (int32_t)(h * ga + 0.5); r[0] = ((int32_t)w - dw) / 2; r[2] = dw; }
    else if (wa < ga - 1e-3) { const int32_t dh = (int32_t)(w / ga + 0.5); r[1] = ((int32_t)h - dh) / 2; r[3] = dh; }
}

static struct { vk_buffer b; char path[512]; u32 w, h; int pending; } s_pp_shot;
static void vk_pp_write_shot(void);

/* Record the frame into swapchain image `dst` (in TRANSFER_DST_OPTIMAL).
 * Returns 0 when it did, -1 to have the caller do the plain blit. */
static int vk_pp_compose(VkImage dst, u32 w, u32 h, const rsx_host_options* o)
{
    if (!s_pp.shot[0]) rsx_host_ui_take_shot(s_pp.shot, (int)sizeof s_pp.shot);
    const int ui = rsx_host_ui_visible();
    const int need = o->upscale == RSX_UI_UPSCALE_SHARP || o->brightness || ui || s_pp.shot[0];
    if (!need || vk_pp_init() || vk_pp_target(w, h)) return -1;
    if (ui && vk_pp_font(w, h)) return -1;
    if (vk_begin()) return -1;
    VkDescriptorSet scene = vk_pp_set(s_vk.color.view, o->upscale == RSX_UI_UPSCALE_NEAREST ? s_pp.near_ : s_pp.lin);
    VkDescriptorSet font = (ui && s_pp.font.view) ? vk_pp_set(s_pp.font.view, s_pp.lin) : VK_NULL_HANDLE;
    if (!scene) return -1;
    int nv = 0;
    if (font) nv = rsx_host_ui_build(&s_pp.cpu_font, (int)w, (int)h, s_pp.verts, (int)VK_UI_MAX_VERTS);
    VkDeviceSize voff = 0;
    void* vdst = nv ? vk_bt_alloc((VkDeviceSize)nv * sizeof(rsx_ui_vtx), 16, &voff) : NULL;
    if (vdst) memcpy(vdst, s_pp.verts, (size_t)nv * sizeof(rsx_ui_vtx));
    else nv = 0;

    int32_t r[4]; vk_pp_rect(o, w, h, r);
    VkClearValue black = { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } };
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = s_pp.rp,
                                  .framebuffer = s_pp.fb, .renderArea = { { 0, 0 }, { w, h } },
                                  .clearValueCount = 1, .pClearValues = &black };
    pvkCmdBeginRenderPass(s_vk.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    s_vk_foreign_bind = 1;
    VkViewport vp = { (float)r[0], (float)r[1], (float)r[2], (float)r[3], 0.0f, 1.0f };
    VkRect2D sc = { { r[0], r[1] }, { (u32)r[2], (u32)r[3] } };
    pvkCmdSetViewport(s_vk.cmd, 0, 1, &vp);
    pvkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
    pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pp.post);
    pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pp.layout, 0, 1, &scene, 0, NULL);
    const float pc[8] = { (float)s_vk.width, (float)s_vk.height, 1.0f / (float)s_vk.width, 1.0f / (float)s_vk.height,
                          o->upscale == RSX_UI_UPSCALE_SHARP ? 1.0f : 0.0f, (float)o->sharpness / 10.0f,
                          exp2f(-0.1f * (float)o->brightness), 0.0f };
    pvkCmdPushConstants(s_vk.cmd, s_pp.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    pvkCmdDraw(s_vk.cmd, 3, 1, 0, 0);
    if (nv) {
        VkViewport fvp = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
        VkRect2D fsc = { { 0, 0 }, { w, h } };
        pvkCmdSetViewport(s_vk.cmd, 0, 1, &fvp);
        pvkCmdSetScissor(s_vk.cmd, 0, 1, &fsc);
        pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pp.ui);
        pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pp.layout, 0, 1, &font, 0, NULL);
        const float upc[8] = { 2.0f / (float)w, 2.0f / (float)h, 0, 0, 0, 0, 0, 0 };
        pvkCmdPushConstants(s_vk.cmd, s_pp.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof upc, upc);
        pvkCmdBindVertexBuffers(s_vk.cmd, 0, 1, &s_bt.s[s_bt.cur].arena.buf, &voff);
        pvkCmdDraw(s_vk.cmd, (u32)nv, 1, 0, 0);
    }
    pvkCmdEndRenderPass(s_vk.cmd);
    vk_full_barrier();
    VkImageCopy cp = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                       .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .extent = { w, h, 1 } };
    VkImageBlit bl = { .srcSubresource = cp.srcSubresource, .srcOffsets = { { 0, 0, 0 }, { (int32_t)w, (int32_t)h, 1 } },
                       .dstSubresource = cp.dstSubresource, .dstOffsets = { { 0, 0, 0 }, { (int32_t)w, (int32_t)h, 1 } } };
    /* A blit, not a copy: the swapchain is usually BGRA. 1:1, so NEAREST. */
    pvkCmdBlitImage(s_vk.cmd, s_pp.img.img, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1, &bl, VK_FILTER_NEAREST);
    if (s_pp.shot[0]) vk_pp_write_shot();
    return 0;
}

/* Dev: the composed frame as a PPM (RACACIT_UI_FILE "shot <path>"). The
 * copy is recorded with the frame; vk_pp_flush_shot writes the file once the
 * present's batch has been submitted. Waits for the GPU, so only on request. */
static void vk_pp_write_shot(void)
{
    vk_buffer st = {0};
    if (s_pp_shot.pending ||
        vk_create_host_buffer(&st, (VkDeviceSize)s_pp.w * s_pp.h * 4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        vk_destroy_buffer(&st); s_pp.shot[0] = 0; return;
    }
    vk_full_barrier();
    VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                 .imageExtent = { s_pp.w, s_pp.h, 1 } };
    pvkCmdCopyImageToBuffer(s_vk.cmd, s_pp.img.img, VK_IMAGE_LAYOUT_GENERAL, st.buf, 1, &region);
    s_pp_shot.b = st; s_pp_shot.w = s_pp.w; s_pp_shot.h = s_pp.h;
    snprintf(s_pp_shot.path, sizeof s_pp_shot.path, "%s", s_pp.shot);
    s_pp_shot.pending = 1;
    s_pp.shot[0] = 0;
}

static void vk_pp_flush_shot(void)
{
    if (!s_pp_shot.pending) return;
    s_pp_shot.pending = 0;
    vk_bt_submit(1, VK_NULL_HANDLE, VK_NULL_HANDLE);
    vk_invalidate(&s_pp_shot.b);
    FILE* f = fopen(s_pp_shot.path, "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", s_pp_shot.w, s_pp_shot.h);
        const unsigned char* px = (const unsigned char*)s_pp_shot.b.ptr;
        const size_t n = (size_t)s_pp_shot.w * s_pp_shot.h;
        unsigned char* rgb = (unsigned char*)malloc(n * 3);
        if (rgb) { for (size_t i = 0; i < n; i++) { rgb[i*3] = px[i*4]; rgb[i*3+1] = px[i*4+1]; rgb[i*3+2] = px[i*4+2]; }
                   fwrite(rgb, 1, n * 3, f); free(rgb); }
        fclose(f);
        VK_LOG("composed frame written to %s\n", s_pp_shot.path);
    }
    vk_destroy_buffer(&s_pp_shot.b);
}

/* The plain path: the display image into `r` of the swapchain image, bars
 * cleared to black. */
static void vk_pp_plain(VkImage dst, u32 w, u32 h, const rsx_host_options* o)
{
    int32_t r[4]; vk_pp_rect(o, w, h, r);
    if (r[2] != (int32_t)w || r[3] != (int32_t)h) {
        VkClearColorValue black = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };
        VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        pvkCmdClearColorImage(s_vk.cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rr);
        VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = dst, .subresourceRange = rr };
        pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 0, NULL, 0, NULL, 1, &b);
    }
    VkImageBlit blit = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets = { { 0, 0, 0 }, { (int32_t)s_vk.width, (int32_t)s_vk.height, 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = { { r[0], r[1], 0 }, { r[0] + r[2], r[1] + r[3], 1 } },
    };
    pvkCmdBlitImage(s_vk.cmd, s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1, &blit, o->upscale == RSX_UI_UPSCALE_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
}

/* Blit the offscreen target into the next swapchain image and present it. */
/* RACACIT_PRESENT_MODE=fifo|mailbox|immediate (default mailbox when the
 * device has it, else fifo -- the one mode every driver has). */
static int s_present_picked = -1;      /* -1: pick again (the VSync option changed) */
static VkPresentModeKHR vk_pick_present_mode(void)
{
    int picked = s_present_picked;
    if (picked >= 0) return (VkPresentModeKHR)picked;
    rsx_host_options o; rsx_host_ui_options(&o);
    VkPresentModeKHR want = o.vsync == RSX_UI_VSYNC_FIFO ? VK_PRESENT_MODE_FIFO_KHR
                          : o.vsync == RSX_UI_VSYNC_IMMEDIATE ? VK_PRESENT_MODE_IMMEDIATE_KHR
                          : VK_PRESENT_MODE_MAILBOX_KHR;
    VkPresentModeKHR modes[16]; u32 n = 16;
    picked = VK_PRESENT_MODE_FIFO_KHR;
    if (pvkGetPhysicalDeviceSurfacePresentModesKHR &&
        pvkGetPhysicalDeviceSurfacePresentModesKHR(s_vk.phys, s_vk.surface, &n, modes) >= 0)
        for (u32 i = 0; i < n; i++) if (modes[i] == want) picked = want;
    /* Without MAILBOX, "Off (no tearing)" is IMMEDIATE where the driver has it. */
    if (picked == VK_PRESENT_MODE_FIFO_KHR && want == VK_PRESENT_MODE_MAILBOX_KHR)
        for (u32 i = 0; i < n; i++) if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) picked = VK_PRESENT_MODE_IMMEDIATE_KHR;
    VK_LOG("present mode %s%c", picked == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" :
           picked == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : "fifo", 10);
    s_present_picked = picked;
    return (VkPresentModeKHR)picked;
}

/* Where the window present spends its time, for vk_eng_present's slow-present
 * line: acquire, compose + submit (the batch, which can wait for an earlier
 * frame), queue present. */
static unsigned long long s_pw_acq, s_pw_sub, s_pw_qp, s_pw_ui, s_pw_blit;
static void vk_present_window(void)
{
    extern unsigned long long ps3_qpc_us(void);
    s_pw_acq = s_pw_sub = s_pw_qp = 0;
    if (!s_vk.windowed || !s_vk.swapchain) return;
    rsx_host_options opt; rsx_host_ui_options(&opt);
    /* A display-mode or VSync change: a new swapchain (and present mode). */
    if (opt.swap_gen != s_pp.swap_gen) {
        s_pp.swap_gen = opt.swap_gen;
        s_present_picked = -1;
        vk_swapchain_lost();
        if (s_vk_win_minimized || !s_vk.windowed || !s_vk.swapchain) return;
    }

    if (s_vk_win_minimized) {
        /* Still 0x0? Then there is nothing to present into. */
        VkSurfaceCapabilitiesKHR caps;
        if (pvkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_vk.phys, s_vk.surface, &caps) != VK_SUCCESS) return;
        if (!caps.currentExtent.width || !caps.currentExtent.height) return;
        vk_swapchain_lost();
        if (s_vk_win_minimized || !s_vk.windowed || !s_vk.swapchain) return;
    }

#ifdef __ANDROID__
    /* Android: the frame's drawing goes to the GPU now, before the swapchain
     * image is acquired; only the copy onto that image waits for it. Gated on
     * the acquire, the whole frame waited for the compositor to hand an image
     * back. */
    if (s_bt.ready && s_bt.recording) vk_bt_submit(0, VK_NULL_HANDLE, VK_NULL_HANDLE);
#endif
    u32 idx = 0;
    unsigned long long pw0 = ps3_qpc_us();
    VkResult r = pvkAcquireNextImageKHR(s_vk.device, s_vk.swapchain, UINT64_MAX,
                                        s_vk.sem_acquire, VK_NULL_HANDLE, &idx);
    s_pw_acq = ps3_qpc_us() - pw0; pw0 += s_pw_acq;
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { vk_swapchain_lost(); return; }
    if (r == VK_ERROR_SURFACE_LOST_KHR) { vk_surface_lost(); return; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        VK_LOG("vkAcquireNextImageKHR failed (VkResult %d)\n", (int)r); return;
    }

    if (vk_begin()) return;
    vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = s_vk.sc_images[idx], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          0, 0, NULL, 0, NULL, 1, &b);
    if (vk_pp_compose(s_vk.sc_images[idx], s_vk.sc_extent.width, s_vk.sc_extent.height, &opt))
        vk_pp_plain(s_vk.sc_images[idx], s_vk.sc_extent.width, s_vk.sc_extent.height, &opt);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                          0, 0, NULL, 0, NULL, 1, &b);
    if (s_bt.ready) {
        /* The frame's batch goes to the GPU here, gated on the acquire and
         * signalling the present -- no CPU wait. */
        if (vk_bt_submit(0, s_vk.sem_acquire, s_vk.sc_done[idx])) { VK_LOG("vkQueueSubmit (window) failed\n"); return; }
    } else {
        if (pvkEndCommandBuffer(s_vk.cmd) != VK_SUCCESS) { VK_LOG("vkEndCommandBuffer failed\n"); return; }
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &s_vk.sem_acquire,
            .pWaitDstStageMask = &wait_stage,
            .commandBufferCount = 1, .pCommandBuffers = &s_vk.cmd,
            .signalSemaphoreCount = 1, .pSignalSemaphores = &s_vk.sc_done[idx],
        };
        pvkResetFences(s_vk.device, 1, &s_vk.fence);
        if (pvkQueueSubmit(s_vk.queue, 1, &si, s_vk.fence) != VK_SUCCESS) {
            VK_LOG("vkQueueSubmit (window) failed\n"); return;
        }
        pvkWaitForFences(s_vk.device, 1, &s_vk.fence, VK_TRUE, UINT64_MAX);
    }

    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &s_vk.sc_done[idx],
        .swapchainCount = 1, .pSwapchains = &s_vk.swapchain, .pImageIndices = &idx,
    };
    s_pw_sub = ps3_qpc_us() - pw0; pw0 += s_pw_sub;
    r = pvkQueuePresentKHR(s_vk.queue, &pi);
    s_pw_qp = ps3_qpc_us() - pw0;
    vk_pp_flush_shot();

    if (r == VK_ERROR_OUT_OF_DATE_KHR) vk_swapchain_lost();
    else if (r == VK_ERROR_SURFACE_LOST_KHR) vk_surface_lost();
    else if (r == VK_SUBOPTIMAL_KHR) vk_swapchain_suboptimal();
    else if (r != VK_SUCCESS) VK_LOG("vkQueuePresentKHR failed (VkResult %d)\n", (int)r);
}

static void vk_dump_ppm(const unsigned char* rgba)
{
    /* A path with a printf conversion ("frame_%04u.ppm") writes numbered
     * frames instead of overwriting one file, and PS3RECOMP_VK_DUMP_EVERY=<n>
     * keeps only every n-th present -- what a long headless boot needs to show
     * how far it got, rather than only where it ended. */
    static long s_every = -1;
    if (s_every < 0) { const char* e = getenv("PS3RECOMP_VK_DUMP_EVERY");
                       s_every = e ? atol(e) : 1; if (s_every < 1) s_every = 1; }
    if ((s_vk.presented - 1) % (u32)s_every) return;
    /* PS3RECOMP_VK_DUMP_GATE=<file>: only while that file exists (a short
     * window of every frame, e.g. one animation, without dumping the boot). */
    { static const char* gate = (const char*)1;
      if (gate == (const char*)1) gate = getenv("PS3RECOMP_VK_DUMP_GATE");
      if (gate) { FILE* gf = fopen(gate, "rb"); if (!gf) return; fclose(gf); } }
    char path[512];
    if (strchr(s_vk.dump_path, '%'))
        snprintf(path, sizeof path, s_vk.dump_path, (unsigned)(s_vk.presented / (u32)s_every));
    else
        snprintf(path, sizeof path, "%s", s_vk.dump_path);
    FILE* f = fopen(path, "wb");
    if (!f) { VK_LOG("cannot open %s for the frame dump\n", s_vk.dump_path); return; }
    fprintf(f, "P6\n%u %u\n255\n", s_vk.width, s_vk.height);
    /* One write: a call per pixel made each dump a ~170 ms hitch. */
    const size_t px = (size_t)s_vk.width * s_vk.height;
    unsigned char* rgb = (unsigned char*)malloc(px * 3);
    if (rgb) {
        for (size_t i = 0; i < px; i++) { rgb[i * 3] = rgba[i * 4]; rgb[i * 3 + 1] = rgba[i * 4 + 1]; rgb[i * 3 + 2] = rgba[i * 4 + 2]; }
        fwrite(rgb, 1, px * 3, f);
        free(rgb);
    } else {
        for (size_t i = 0; i < px; i++) fwrite(rgba + i * 4, 1, 3, f);
    }
    fclose(f);
}

/* ---------------------------------------------------------------------------
 * Host overlay: on-screen touch controls (touch_controls.h)
 *
 * Drawn into the offscreen frame just before it is presented, with its own
 * tiny alpha-blended pipeline: position (Vulkan NDC) + colour per vertex,
 * geometry built on the CPU each frame from the shared layout.
 * -------------------------------------------------------------------------*/
typedef struct { float x, y, r, g, b, a; } vk_ov_vtx;
static struct {
    VkShaderModule vs, fs;
    VkPipelineLayout layout;
    VkPipeline pipe;
    int failed;
    vk_ov_vtx* v; u32 n, cap;
    float sx, sy;          /* NDC per pixel */
    float ax;              /* x squash so circles stay round on the screen */
} s_ov;

static void ov_push(float px, float py, const float c[4])
{
    if (s_ov.n == s_ov.cap) {
        u32 cap = s_ov.cap ? s_ov.cap * 2 : 4096;
        vk_ov_vtx* nv = (vk_ov_vtx*)realloc(s_ov.v, cap * sizeof *nv);
        if (!nv) return;
        s_ov.v = nv; s_ov.cap = cap;
    }
    s_ov.v[s_ov.n++] = (vk_ov_vtx){ px * s_ov.sx - 1.0f, py * s_ov.sy - 1.0f, c[0], c[1], c[2], c[3] };
}
static void ov_tri(float x0, float y0, float x1, float y1, float x2, float y2, const float c[4])
{ ov_push(x0, y0, c); ov_push(x1, y1, c); ov_push(x2, y2, c); }
static void ov_quad(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, const float c[4])
{ ov_tri(x0, y0, x1, y1, x2, y2, c); ov_tri(x0, y0, x2, y2, x3, y3, c); }
/* Ellipse (rx already squashed by the caller via s_ov.ax). */
static void ov_disc(float cx, float cy, float rx, float ry, const float c[4])
{
    const int N = 28;
    for (int i = 0; i < N; i++) {
        float a0 = 6.2831853f * i / N, a1 = 6.2831853f * (i + 1) / N;
        ov_tri(cx, cy, cx + cosf(a0) * rx, cy + sinf(a0) * ry, cx + cosf(a1) * rx, cy + sinf(a1) * ry, c);
    }
}
static void ov_ring(float cx, float cy, float rx, float ry, float t, const float c[4])
{
    const int N = 28;
    for (int i = 0; i < N; i++) {
        float a0 = 6.2831853f * i / N, a1 = 6.2831853f * (i + 1) / N;
        float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
        float ix = rx - t * s_ov.ax, iy = ry - t;
        ov_quad(cx + c0 * ix, cy + s0 * iy, cx + c0 * rx, cy + s0 * ry,
                cx + c1 * rx, cy + s1 * ry, cx + c1 * ix, cy + s1 * iy, c);
    }
}
static void ov_line(float x0, float y0, float x1, float y1, float w, const float c[4])
{
    /* Thickness in screen pixels: perpendicular computed in unsquashed space. */
    float dx = (x1 - x0) / s_ov.ax, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy);
    if (l <= 0.0f) return;
    float nx = -dy / l * w * 0.5f * s_ov.ax, ny = dx / l * w * 0.5f;
    ov_quad(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, c);
}

static int vk_overlay_pipeline(void)
{
    if (s_ov.pipe) return 0;
    if (s_ov.failed || !s_vk.render_pass) return -1;
    s_ov.failed = 1;
    if (vk_create_shader(rsx_vk_overlay_vert_spv, sizeof rsx_vk_overlay_vert_spv, &s_ov.vs) ||
        vk_create_shader(rsx_vk_overlay_frag_spv, sizeof rsx_vk_overlay_frag_spv, &s_ov.fs)) return -1;
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VK_CHECK(pvkCreatePipelineLayout(s_vk.device, &plci, NULL, &s_ov.layout), "vkCreatePipelineLayout(overlay)");
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = s_ov.vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = s_ov.fs, .pName = "main" },
    };
    VkVertexInputBindingDescription vb = { 0, sizeof(vk_ov_vtx), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[2] = {
        { 0, 0, VK_FORMAT_R32G32_SFLOAT,       0 },
        { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 8 },
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
        .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = va };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState cba = {
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vi,
        .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
        .pDynamicState = &dy, .layout = s_ov.layout, .renderPass = s_vk.render_pass, .subpass = 0 };
    VK_CHECK((s_vkc[5]++, pvkCreateGraphicsPipelines(s_vk.device, VK_NULL_HANDLE, 1, &gci, NULL, &s_ov.pipe)),
             "vkCreateGraphicsPipelines(overlay)");
    s_ov.failed = 0;
    return 0;
}

static void vk_overlay_build(void)
{
    const float W = (float)s_vk.width, H = (float)s_vk.height;
    /* The frame's shape on the screen -- 16:9 between bars (aspect kept) or
     * the whole screen (stretched): a button is round only in the shape it is
     * shown at. A near-square foldable or a 20:9 phone is not 16:9. The touch
     * hit test gets the same rectangle, so a tap lands where it is drawn. */
    float shown = W / H;
    if (s_vk.windowed && s_vk.sc_extent.width && s_vk.sc_extent.height) {
        rsx_host_options o; rsx_host_ui_options(&o);
        const float sw = (float)s_vk.sc_extent.width, sh = (float)s_vk.sc_extent.height;
        int32_t r[4]; vk_pp_rect(&o, s_vk.sc_extent.width, s_vk.sc_extent.height, r);
        if (r[2] > 0 && r[3] > 0) {
            shown = (float)r[2] / (float)r[3];
            touch_controls_set_frame((float)r[0] / sw, (float)r[1] / sh,
                                     (float)(r[0] + r[2]) / sw, (float)(r[1] + r[3]) / sh, shown);
        }
    }
    s_ov.n = 0;
    s_ov.sx = 2.0f / W; s_ov.sy = 2.0f / H;
    s_ov.ax = (W / H) / shown;
    const tc_button* L; const int n = touch_controls_layout(&L);
    const uint32_t pressed = touch_controls_pressed();
    /* RACACIT_TOUCH_OPACITY fades all of it (touch_controls.h). */
    const float op = touch_controls_opacity();
    const float fill[4]  = { 0.0f, 0.0f, 0.0f, 0.22f * op };
    const float fillp[4] = { 1.0f, 1.0f, 1.0f, 0.35f * op };
    const float edge[4]  = { 1.0f, 1.0f, 1.0f, 0.50f * op };
    const float glyph[4] = { 1.0f, 1.0f, 1.0f, 0.80f * op };
    const float c_cross[4] = { 0.49f, 0.66f, 1.00f, 0.90f * op }, c_circle[4] = { 1.00f, 0.42f, 0.42f, 0.90f * op };
    const float c_square[4] = { 0.95f, 0.61f, 0.88f, 0.90f * op }, c_tri[4] = { 0.44f, 0.88f, 0.71f, 0.90f * op };
    for (int i = 0; i < n; i++) {
        const tc_button* k = &L[i];
        const float cx = k->x * W, cy = k->y * H, ry = k->r * H, rx = ry * s_ov.ax;
        const int on = (pressed >> i) & 1u;
        const float t = ry * 0.08f;
        if (k->shape == TC_SHAPE_STICK) {
            int act; float ax, ay, tx, ty;
            touch_controls_stick(&act, &ax, &ay, &tx, &ty);
            float bx = act ? ax * W : cx, by = act ? ay * H : cy;
            ov_disc(bx, by, rx, ry, fill);
            ov_ring(bx, by, rx, ry, t, edge);
            float kx = act ? tx * W : bx, ky = act ? ty * H : by;
            float lim = ry * 0.85f, dx = (kx - bx) / s_ov.ax, dy = ky - by, d = sqrtf(dx * dx + dy * dy);
            if (d > lim) { kx = bx + dx / d * lim * s_ov.ax; ky = by + dy / d * lim; }
            ov_disc(kx, ky, rx * 0.45f, ry * 0.45f, act ? fillp : edge);
            continue;
        }
        float erx = (k->shape == TC_SHAPE_PILL) ? rx * 1.7f : rx;
        ov_disc(cx, cy, erx, ry, on ? fillp : fill);
        ov_ring(cx, cy, erx, ry, t, edge);
        const float g = ry * 0.45f, gx = g * s_ov.ax, lw = ry * 0.14f;
        switch (k->shape) {
        case TC_SHAPE_CROSS:
            ov_line(cx - gx, cy - g, cx + gx, cy + g, lw, c_cross);
            ov_line(cx - gx, cy + g, cx + gx, cy - g, lw, c_cross); break;
        case TC_SHAPE_CIRCLE:
            ov_ring(cx, cy, gx, g, lw, c_circle); break;
        case TC_SHAPE_SQUARE:
            ov_line(cx - gx, cy - g, cx + gx, cy - g, lw, c_square); ov_line(cx + gx, cy - g, cx + gx, cy + g, lw, c_square);
            ov_line(cx + gx, cy + g, cx - gx, cy + g, lw, c_square); ov_line(cx - gx, cy + g, cx - gx, cy - g, lw, c_square); break;
        case TC_SHAPE_TRIANGLE:
            ov_line(cx, cy - g, cx + gx, cy + g * 0.8f, lw, c_tri); ov_line(cx + gx, cy + g * 0.8f, cx - gx, cy + g * 0.8f, lw, c_tri);
            ov_line(cx - gx, cy + g * 0.8f, cx, cy - g, lw, c_tri); break;
        case TC_SHAPE_UP:    ov_tri(cx, cy - g, cx + gx, cy + g * 0.6f, cx - gx, cy + g * 0.6f, glyph); break;
        case TC_SHAPE_DOWN:  ov_tri(cx, cy + g, cx - gx, cy - g * 0.6f, cx + gx, cy - g * 0.6f, glyph); break;
        case TC_SHAPE_LEFT:  ov_tri(cx - gx, cy, cx + gx * 0.6f, cy - g, cx + gx * 0.6f, cy + g, glyph); break;
        case TC_SHAPE_RIGHT: ov_tri(cx + gx, cy, cx - gx * 0.6f, cy + g, cx - gx * 0.6f, cy - g, glyph); break;
        case TC_SHAPE_PILL: {
            /* 1 bar = L1/R1/SELECT, 2 bars = L2/R2, play triangle = START,
             * a ring with a dot = L3/R3 (a stick click), 3 short bars = the
             * settings menu (mask 0: no pad button). */
            if (k->mask == 0) {
                for (int j = -1; j <= 1; j++) ov_line(cx - gx * 0.9f, cy + g * 0.45f * j, cx + gx * 0.9f, cy + g * 0.45f * j, lw, glyph);
                break; }
            if (k->mask == 0x0002u || k->mask == 0x0004u) {
                ov_ring(cx, cy, gx * 0.95f, g * 0.95f, lw, glyph); ov_disc(cx, cy, gx * 0.35f, g * 0.35f, glyph); break; }
            if (k->mask == 0x0008u) { ov_tri(cx - gx * 0.6f, cy - g * 0.7f, cx + gx * 0.8f, cy, cx - gx * 0.6f, cy + g * 0.7f, glyph); break; }
            const int two = (k->mask == 0x0100u || k->mask == 0x0200u);
            if (two) { ov_line(cx - gx * 1.2f, cy - g * 0.3f, cx + gx * 1.2f, cy - g * 0.3f, lw, glyph);
                       ov_line(cx - gx * 1.2f, cy + g * 0.3f, cx + gx * 1.2f, cy + g * 0.3f, lw, glyph); }
            else ov_line(cx - gx * 1.2f, cy, cx + gx * 1.2f, cy, lw, glyph);
            break; }
        default: break;
        }
    }
}

/* Record the overlay into the open batch, over s_vk.color. */
static void vk_overlay_draw(void)
{
    if (!s_bt.ready || !touch_controls_active() || vk_overlay_pipeline()) return;
    vk_overlay_build();
    if (!s_ov.n) return;
    if (vk_begin()) return;
    VkDeviceSize off = 0;
    void* dst = vk_bt_alloc((VkDeviceSize)s_ov.n * sizeof(vk_ov_vtx), 16, &off);
    if (!dst) return;
    memcpy(dst, s_ov.v, (size_t)s_ov.n * sizeof(vk_ov_vtx));
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = s_vk.render_pass, .framebuffer = s_vk.framebuffer,
                                  .renderArea = { { 0, 0 }, { s_vk.width, s_vk.height } } };
    pvkCmdBeginRenderPass(s_vk.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_ov.pipe);
    s_vk_foreign_bind = 1;   /* the batched path's cached binds are gone */
    VkViewport vp = { 0.0f, 0.0f, (float)s_vk.width, (float)s_vk.height, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { s_vk.width, s_vk.height } };
    pvkCmdSetViewport(s_vk.cmd, 0, 1, &vp);
    pvkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
    pvkCmdBindVertexBuffers(s_vk.cmd, 0, 1, &s_bt.s[s_bt.cur].arena.buf, &off);
    pvkCmdDraw(s_vk.cmd, s_ov.n, 1, 0, 0);
    pvkCmdEndRenderPass(s_vk.cmd);
}

static void vk_cb_present(void* ud, u32 buffer_id)
{
    (void)ud; (void)buffer_id;
    if (!s_vk.device) return;
    extern unsigned long long ps3_qpc_us(void);
    const unsigned long long ui0 = ps3_qpc_us();
    rsx_host_ui_on_present();
    { rsx_host_options o; rsx_host_ui_options(&o);
      if (o.sampler_gen != s_pp.sampler_gen) { s_pp.sampler_gen = o.sampler_gen; vk_apply_filter_options(&o); } }
    vk_overlay_draw();
    s_pw_ui = ps3_qpc_us() - ui0;

    /* Read the frame back only when someone will look at the pixels: the PPM
     * dump (on the presents it keeps) or a headless run's centre-pixel check.
     * A windowed frame goes straight to the swapchain with no CPU wait. */
    if (s_bt.ready) {
        static long s_every = -1;
        if (s_every < 0) { const char* e = getenv("PS3RECOMP_VK_DUMP_EVERY");
                           s_every = e ? atol(e) : 1; if (s_every < 1) s_every = 1; }
        const int dump_this = s_vk.dump_path && (s_vk.presented % (u32)s_every) == 0;
        if (!dump_this && (s_vk.windowed || s_vk.dump_path)) {
            s_vk.presented++;
            s_g.last_draws = s_g.draws;
            s_g.draws = 0;
            if (s_vk.windowed && s_vk.swapchain) vk_present_window();
            else vk_bt_submit(0, VK_NULL_HANDLE, VK_NULL_HANDLE);
            return;
        }
    }

    if (vk_begin()) return;
    vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy region = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { s_vk.width, s_vk.height, 1 },
    };
    pvkCmdCopyImageToBuffer(s_vk.cmd, s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL,
                            s_vk.readback.buf, 1, &region);
    VkBufferMemoryBarrier host = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = s_vk.readback.buf, .offset = 0, .size = VK_WHOLE_SIZE,
    };
    pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                          0, 0, NULL, 1, &host, 0, NULL);
    if (vk_submit_and_wait()) return;

    vk_invalidate(&s_vk.readback);
    const unsigned char* px = (const unsigned char*)s_vk.readback.ptr;
    size_t c = ((size_t)(s_vk.height / 2) * s_vk.width + s_vk.width / 2) * 4;
    s_vk.last_center = ((u32)px[c] << 16) | ((u32)px[c + 1] << 8) | (u32)px[c + 2];
    s_vk.presented++;
    s_g.last_draws = s_g.draws;
    s_g.draws = 0;
    if (s_vk.dump_path) vk_dump_ppm(px);
    vk_present_window();
}

/* ---------------------------------------------------------------------------
 * Register-file draw engine backend (rsx_draw_engine.h)
 *
 * The path the Metal backend takes: the shared engine walks the register
 * file, owns surfaces, texture caching, vertex compaction and pipeline keys,
 * and drives this backend through rsx_draw_backend. Selected with
 * PS3RECOMP_RSX_ENGINE=dispatch; only one of the two paths is registered at a
 * time, since the FIFO walker feeds both and would record every draw twice.
 *
 * Every operation is still one synchronous submission, so submit_and_wait has
 * nothing left to do by the time the engine calls it -- which is exactly the
 * contract it asks for (staging is free to reuse on return).
 *
 * Stage E1: colour and depth targets, clears, present and readback.
 * Pipelines, draws and textures are the next stages; until then
 * pipeline_create reports 0, which the engine caches as "cannot build".
 * -------------------------------------------------------------------------*/
#define VK_ENG_MAX_OBJ 32768         /* the engine's texture cache is 16384, plus targets, depth, views */

enum { VK_ENG_FREE = 0, VK_ENG_COLOR, VK_ENG_DEPTH, VK_ENG_TEXTURE, VK_ENG_VIEW };

typedef struct vk_eng_obj {
    u8       kind;
    u8       lazy;                   /* COLOR: no image yet (vk_eng_color_back) */
    VkFormat fmt;
    u32      bpp;                    /* bytes per texel, 0 for depth     */
    u32      w, h;                   /* the image's own size                */
    u32      lw, lh;                 /* the guest's: w,h / RACACIT_RES_SCALE for
                                        targets and their snapshots         */
    float    sc;                     /* DEPTH and its snapshots: the scale it is drawn
                                        at when not the scene's (a shadow map's), else 0 */
    vk_image im;                     /* a VIEW owns only im.view            */
    u32      block;                  /* bytes per 4x4 block, 0 = not BC     */
    u32      surface, remap, rsx_fmt;/* VIEW: what it is a view of          */
    u8       cube;                   /* TEXTURE: six layers, a CUBE view   */
    u32      views;                  /* COLOR: its first VIEW, 0 = none     */
    u32      next_view;              /* VIEW: the next view of its surface  */
    u32      spare;                  /* DEPTH: the snapshot at the other size it is read at */
} vk_eng_obj;

static vk_eng_obj s_eobj[VK_ENG_MAX_OBJ];
/* RSX_ENG_STATS: calls that return something the engine waits on. */
static unsigned long long s_sync_calls[11];


/* Defined with the E2 pipeline state further down. */
static void vk_eng_drop_framebuffers(void);
static void vk_eng_drop_framebuffers_using(u32 handle);
static void vk_eng2_shutdown(void* user);

static struct {
    u32 rt[RSX_BE_MAX_COLOR_TARGETS], nrt, depth;
} s_eng_bound;

static VkFormat vk_eng_format(rsx_be_format f, u32* bpp)
{
    switch (f) {
    case RSX_BE_FMT_R8:            *bpp = 1;  return VK_FORMAT_R8_UNORM;
    case RSX_BE_FMT_R8G8:          *bpp = 2;  return VK_FORMAT_R8G8_UNORM;
    case RSX_BE_FMT_R8G8B8A8:      *bpp = 4;  return VK_FORMAT_R8G8B8A8_UNORM;
    case RSX_BE_FMT_R16:           *bpp = 2;  return VK_FORMAT_R16_UNORM;
    case RSX_BE_FMT_R16G16:        *bpp = 4;  return VK_FORMAT_R16G16_UNORM;
    case RSX_BE_FMT_R16G16F:       *bpp = 4;  return VK_FORMAT_R16G16_SFLOAT;
    case RSX_BE_FMT_R16G16B16A16F: *bpp = 8;  return VK_FORMAT_R16G16B16A16_SFLOAT;
    case RSX_BE_FMT_R32F:          *bpp = 4;  return VK_FORMAT_R32_SFLOAT;
    case RSX_BE_FMT_R32G32B32A32F: *bpp = 16; return VK_FORMAT_R32G32B32A32_SFLOAT;
    case RSX_BE_FMT_BC1:           *bpp = 8;  return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;  /* per block */
    case RSX_BE_FMT_BC2:           *bpp = 16; return VK_FORMAT_BC2_UNORM_BLOCK;
    case RSX_BE_FMT_BC3:           *bpp = 16; return VK_FORMAT_BC3_UNORM_BLOCK;
    default:                       *bpp = 0;  return VK_FORMAT_UNDEFINED;
    }
}

static u32 vk_eng_alloc(void)
{
    s_eobj_gen++;
    /* Round-robin from the last allocation: snapshots and texture refreshes
     * allocate every frame, and a scan from 1 walked the whole live table. */
    static u32 next = 1;
    for (u32 k = 0; k < VK_ENG_MAX_OBJ - 1u; k++) {
        const u32 i = 1u + (next - 1u + k) % (VK_ENG_MAX_OBJ - 1u);
        if (s_eobj[i].kind == VK_ENG_FREE) { next = i + 1u; return i; }
    }
    VK_LOG("engine: object table full (%d)\n", VK_ENG_MAX_OBJ);
    return 0;
}

static int vk_eng_color_back(vk_eng_obj* o);
/* Getting a colour target means using it: this is what gives it its image. */
static vk_eng_obj* vk_eng_get(u32 handle, int kind)
{
    if (!handle || handle >= VK_ENG_MAX_OBJ) return NULL;
    vk_eng_obj* o = &s_eobj[handle];
    if (o->kind != kind) return NULL;
    if (o->lazy && vk_eng_color_back(o)) return NULL;
    return o;
}

/* Copy `rows` rows of `w` texels from host memory into one level / layer of
 * an image that is in GENERAL. */
static int vk_eng_upload_rows(VkImage img, u32 layer, u32 mip, u32 w, u32 h,
                              size_t tight, u32 rows, const void* src, u32 row_bytes)
{
    if (tight > row_bytes) tight = row_bytes;
    const VkDeviceSize bytes = (VkDeviceSize)tight * rows;
    int rc = vk_begin();
    if (rc) return rc;
    /* Staging from the batch arena when it fits (no allocation per upload);
     * a dedicated buffer otherwise, released with the batch. */
    vk_buffer st = {0};
    VkBuffer src_buf = VK_NULL_HANDLE; VkDeviceSize src_off = 0;
    u8* dst = s_bt.ready ? (u8*)vk_bt_alloc(bytes, 16, &src_off) : NULL;
    if (dst) {
        src_buf = s_bt.s[s_bt.cur].arena.buf;
    } else {
        if (vk_create_host_buffer(&st, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) || !st.coherent) {
            vk_destroy_buffer(&st); return -1; }
        dst = (u8*)st.ptr; src_buf = st.buf; src_off = 0;
    }
    for (u32 y = 0; y < rows; y++)
        memcpy(dst + (size_t)y * tight, (const u8*)src + (size_t)y * row_bytes, tight);
    VkBufferImageCopy region = {
        .bufferOffset = src_off,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1 },
        .imageExtent = { w, h, 1 },
    };
    pvkCmdCopyBufferToImage(s_vk.cmd, src_buf, img, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    rc = vk_defer();
    if (st.buf) vk_destroy_buffer(&st);
    return rc;
}

static int vk_eng_upload(VkImage img, u32 layer, u32 mip, u32 w, u32 h, u32 bpp,
                         const void* src, u32 row_bytes)
{
    return vk_eng_upload_rows(img, layer, mip, w, h, (size_t)w * bpp, h, src, row_bytes);
}

/* NV4097 TEXTURE_CONTROL1 crossbar as a Vulkan component mapping: the shared
 * rsx_texture_component_remap() gives, in A,R,G,B order, which uploaded
 * channel (0..3 = R,G,B,A) or constant each output takes -- the table the
 * Metal backend's swizzle_sel reads. */
static VkComponentSwizzle vk_remap_sel(u8 sel)
{
    switch (sel) {
    case 0: return VK_COMPONENT_SWIZZLE_R;
    case 1: return VK_COMPONENT_SWIZZLE_G;
    case 2: return VK_COMPONENT_SWIZZLE_B;
    case 3: return VK_COMPONENT_SWIZZLE_A;
    case RSX_REMAP_ONE: return VK_COMPONENT_SWIZZLE_ONE;
    default: return VK_COMPONENT_SWIZZLE_ZERO;
    }
}
static VkComponentMapping vk_remap(u32 remap, u32 rsx_fmt)
{
    u8 sel[4];
    rsx_texture_component_remap(remap, rsx_fmt & 0x9Fu, sel);
    VkComponentMapping m = { vk_remap_sel(sel[1]), vk_remap_sel(sel[2]),
                             vk_remap_sel(sel[3]), vk_remap_sel(sel[0]) };
    return m;
}
static int vk_eng_make_view_ex(VkImage img, VkFormat fmt, u32 levels, VkComponentMapping map,
                               int cube, VkImageView* out)
{
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
        .viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D,
        .format = fmt, .components = map,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels ? levels : 1, 0, cube ? 6u : 1u },
    };
    return (s_vkc[4]++, pvkCreateImageView(s_vk.device, &vci, NULL, out)) == VK_SUCCESS ? 0 : -1;
}
static int vk_eng_make_view(VkImage img, VkFormat fmt, u32 levels, VkComponentMapping map,
                            VkImageView* out)
{
    return vk_eng_make_view_ex(img, fmt, levels, map, 0, out);
}

/* The "nothing bound" cube: every cube slot of the descriptor set needs a
 * valid CUBE view whether or not the program samples it. */
static int vk_make_dummy_cube(void)
{
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { 1, 1, 1 }, .mipLevels = 1, .arrayLayers = 6,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    vk_image* d = &s_vk.dummy_cube;
    VkMemoryRequirements req; u32 type;
    if ((s_vkc[0]++, pvkCreateImage(s_vk.device, &ici, NULL, &d->img)) != VK_SUCCESS) return -1;
    pvkGetImageMemoryRequirements(s_vk.device, d->img, &req);
    if (vk_find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        vk_find_memory_type(req.memoryTypeBits, 0, &type)) return -1;
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    if ((s_vkc[1]++, vk_alloc_mem(&mai, &d->mem)) != VK_SUCCESS ||
        pvkBindImageMemory(s_vk.device, d->img, d->mem, 0) != VK_SUCCESS) return -1;
    VkComponentMapping id = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                              VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
    if (vk_eng_make_view_ex(d->img, VK_FORMAT_R8G8B8A8_UNORM, 1, id, 1, &d->view)) return -1;
    if (vk_begin()) return -1;
    vk_barrier_image(d->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    if (vk_submit_and_wait()) return -1;
    static const u8 magenta[4] = { 0xFF, 0x00, 0xFF, 0xFF };
    for (u32 f = 0; f < 6; f++)
        if (vk_eng_upload_rows(d->img, f, 0, 1, 1, 4, 1, magenta, 4)) return -1;
    return 0;
}

static int vk_eng_init(void* user, u32 width, u32 height)
{
    (void)user; (void)width; (void)height;
    memset(s_eobj, 0, sizeof s_eobj);
    memset(&s_eng_bound, 0, sizeof s_eng_bound);
    return s_vk.device ? 0 : -1;
}

static void vk_eng_release(void* user, u32 handle)
{
    (void)user;
    if (!handle || handle >= VK_ENG_MAX_OBJ || s_eobj[handle].kind == VK_ENG_FREE) return;
    if (s_eobj[handle].kind == VK_ENG_COLOR)
        for (u32 i = s_eobj[handle].views; i && i < VK_ENG_MAX_OBJ; ) {
            const u32 next = s_eobj[i].next_view;
            if (s_eobj[i].kind == VK_ENG_VIEW && s_eobj[i].surface == handle) {
                vk_eng_drop_framebuffers_using(i);
                vk_destroy_image(&s_eobj[i].im);
                memset(&s_eobj[i], 0, sizeof s_eobj[i]);
            }
            i = next;
        }
    if (s_eobj[handle].kind == VK_ENG_VIEW) {   /* unlink from its surface */
        vk_eng_obj* c = &s_eobj[s_eobj[handle].surface];
        if (c->kind == VK_ENG_COLOR)
            for (u32* l = &c->views; *l; l = &s_eobj[*l].next_view)
                if (*l == handle) { *l = s_eobj[handle].next_view; break; }
    }
    if ((s_eobj[handle].kind == VK_ENG_DEPTH || s_eobj[handle].kind == VK_ENG_COLOR) &&
        s_eobj[handle].surface) {          /* its snapshot texture */
        const u32 snap = s_eobj[handle].surface;
        s_eobj[handle].surface = 0;
        if (vk_eng_get(snap, VK_ENG_TEXTURE)) vk_eng_release(user, snap);
    }
    if (s_eobj[handle].kind == VK_ENG_DEPTH && s_eobj[handle].spare) {
        const u32 sp = s_eobj[handle].spare;
        s_eobj[handle].spare = 0;
        if (vk_eng_get(sp, VK_ENG_TEXTURE)) vk_eng_release(user, sp);
    }
    if (s_eobj[handle].kind == VK_ENG_COLOR || s_eobj[handle].kind == VK_ENG_DEPTH ||
        s_eobj[handle].kind == VK_ENG_VIEW)
        vk_eng_drop_framebuffers_using(handle);
    { vk_eng_obj* o = &s_eobj[handle];           /* a render target's image waits for the next of its size */
      if (!o->lazy && !((o->kind == VK_ENG_COLOR || o->kind == VK_ENG_DEPTH) &&
            vk_rbin_park((u32)o->kind, o->fmt, o->w, o->h, o->kind == VK_ENG_DEPTH ? 8u : o->bpp, &o->im)))
          vk_destroy_image(&o->im);              /* idle: submissions are waited on */ }
    memset(&s_eobj[handle], 0, sizeof s_eobj[handle]);
}

static void vk_eng_shutdown(void* user)
{
    vk_eng2_shutdown(user);
    for (u32 i = 1; i < VK_ENG_MAX_OBJ; i++) vk_eng_release(user, i);
    vk_rbin_flush();
}

static void vk_eng_submit_and_wait(void* user, u32 reason)
{
    s_sync_calls[0]++;
    (void)user; (void)reason;      /* every submission has already completed */
}

/* ---- RACACIT_RES_SCALE: internal resolution ------------------------------
 * Every colour and depth target -- and the copies made of them for sampling
 * -- is created this many times the size the guest asked for, and viewports,
 * scissors, blits and copies into them are scaled to match; the guest keeps
 * seeing its own sizes (lw/lh) everywhere. 2 renders A Crack in Time's
 * 960x704 scene at 1920x1408, shadow maps and effects buffers included;
 * 0.5..0.75 is for weak GPUs. Textures read with normalised coordinates
 * need nothing; WPOS in guest shaders is divided back (rsx_fp_wpos_scale). */
static float s_rs = 1.0f;
static inline u32 vk_rs(u32 v) { return s_rs == 1.0f ? v : (u32)((float)v * s_rs + 0.5f); }
static inline u32 vk_lw(const vk_eng_obj* o) { return o->lw ? o->lw : o->w; }
static inline u32 vk_lh(const vk_eng_obj* o) { return o->lh ? o->lh : o->h; }
/* RACACIT_SHADOW_RES: shadow maps -- small power-of-two depth targets -- are
 * drawn at their own, higher scale (rsx_depth_target_scale; default twice the
 * scene's on a dedicated card, 2048 pixels a side at most), so their texels are a quarter the
 * area on the ground. A depth-only pass draws into one with a scratch colour
 * image of its size in place of its own colour target (vk_eng_draw); one that
 * a pass also draws colour into goes back to the scene's scale for good
 * (vk_eng_depth_demote). vk_os is vk_rs for any target: its own scale. */
extern float rsx_depth_target_scale(u32 w, u32 h);
static unsigned long long s_zhi_made, s_zhi_demoted;
static inline u32 vk_os(const vk_eng_obj* o, u32 v)
{
    return o->sc > 0.0f ? (u32)((float)v * o->sc + 0.5f) : vk_rs(v);
}

/* Guest bytes seeding a scaled target: uploaded at the guest's size into a
 * temporary image, then stretched over the target. */
static void vk_eng_seed_scaled(vk_eng_obj* o, u32 w, u32 h, u32 bpp, const void* seed, u32 row_bytes)
{
    vk_image tmp = {0};
    if (vk_create_image(&tmp, o->fmt, w, h, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT) || vk_begin()) { vk_destroy_image(&tmp); return; }
    vk_barrier_image(tmp.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    if (vk_defer()) { vk_destroy_image(&tmp); return; }
    vk_eng_upload(tmp.img, 0, 0, w, h, bpp, seed, row_bytes);
    if (!vk_begin()) {
        vk_barrier_image(tmp.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        VkImageBlit b = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .srcOffsets = { { 0, 0, 0 }, { (int32_t)w, (int32_t)h, 1 } },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstOffsets = { { 0, 0, 0 }, { (int32_t)o->w, (int32_t)o->h, 1 } } };
        pvkCmdBlitImage(s_vk.cmd, tmp.img, VK_IMAGE_LAYOUT_GENERAL, o->im.img, VK_IMAGE_LAYOUT_GENERAL,
                        1, &b, VK_FILTER_LINEAR);
        vk_defer();
    }
    vk_destroy_image(&tmp);   /* deferred until the GPU is done with it */
}

static int vk_eng_rt_fi(VkFormat f);
/* A colour target's image, at its first use: UNDEFINED -> GENERAL, transparent
 * black so the contents are defined. 0 if it has one now. */
static int vk_eng_color_back(vk_eng_obj* o)
{
    o->lazy = 0;
    if (!vk_rbin_take(VK_ENG_COLOR, o->fmt, o->w, o->h, &o->im)) {
        s_img_why[0]++;
        if (vk_create_image(&o->im, o->fmt, o->w, o->h,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT)) {
            vk_destroy_image(&o->im); o->lazy = 1; return -1;
        }
    }
    if (vk_begin()) return -1;
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearColorValue zero = { .float32 = { 0.0f, 0.0f, 0.0f, 0.0f } };
    VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    pvkCmdClearColorImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &r);
    return vk_defer() ? -1 : 0;
}

static u32 vk_eng_color_target_create(void* user, rsx_be_format fmt, u32 w, u32 h,
                                      const void* seed, u32 seed_row_bytes)
{
    s_sync_calls[2]++;
    (void)user;
    u32 bpp;
    const VkFormat vf = vk_eng_format(fmt, &bpp);
    /* The formats with a render pass (vk_eng_rt_fi): RGBA8 and RGBA16F. */
    if (vk_eng_rt_fi(vf) < 0 || !w || !h || w > 4096u || h > 4096u) {
        VK_LOG("engine: colour target format %d %ux%u not supported yet\n", (int)fmt, w, h);
        return 0;
    }
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    const u32 pw = vk_rs(w), ph = vk_rs(h);
    o->kind = VK_ENG_COLOR; o->fmt = vf; o->bpp = bpp; o->w = pw; o->h = ph; o->lw = w; o->lh = h;
    /* No image until something uses it. Every shadow map has a colour target at its
     * address that its depth-only passes never touch (vk_eng_zonly_color stands in): 64 MB
     * allocated and cleared at 4x for a 1024x1024 map, for nothing, as often as the
     * game's shadow pool moves the map. */
    o->lazy = 1;
    if (!(seed && seed_row_bytes)) return hd;
    if (vk_eng_color_back(o)) { vk_eng_release(user, hd); return 0; }
    {
        if (pw == w && ph == h) vk_eng_upload(o->im.img, 0, 0, w, h, bpp, seed, seed_row_bytes);
        else vk_eng_seed_scaled(o, w, h, bpp, seed, seed_row_bytes);
    }
    return hd;
}

static u32 vk_eng_depth_target_create(void* user, u32 w, u32 h)
{
    s_sync_calls[4]++;
    (void)user;
    if (!w || !h || w > 4096u || h > 4096u) return 0;
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    extern int rsx_depth_target_is_map(u32 w, u32 h);
    const float zsc = rsx_depth_target_is_map(w, h) ? rsx_depth_target_scale(w, h) : 0.0f;
    const u32 pw = zsc > 0.0f ? (u32)((float)w * zsc + 0.5f) : vk_rs(w);
    const u32 ph = zsc > 0.0f ? (u32)((float)h * zsc + 0.5f) : vk_rs(h);
    if (!vk_rbin_take(VK_ENG_DEPTH, s_vk.eng_depth_format, pw, ph, &o->im)) {
        s_img_why[1]++;
        if (vk_create_image(&o->im, s_vk.eng_depth_format, pw, ph,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            s_vk.eng_depth_aspect)) {
            vk_destroy_image(&o->im); return 0;
        }
    }
    o->kind = VK_ENG_DEPTH; o->fmt = s_vk.eng_depth_format; o->w = pw; o->h = ph; o->lw = w; o->lh = h;
    o->sc = zsc;
    if (zsc > 0.0f) s_zhi_made++;
    /* Far plane, for the reason the display depth starts there. */
    if (vk_begin()) { vk_eng_release(user, hd); return 0; }
    vk_barrier_image(o->im.img, s_vk.eng_depth_aspect, VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearDepthStencilValue far_plane = { 1.0f, 0 };
    VkImageSubresourceRange r = { s_vk.eng_depth_aspect, 0, 1, 0, 1 };
    pvkCmdClearDepthStencilImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &far_plane, 1, &r);
    if (vk_defer()) { vk_eng_release(user, hd); return 0; }
    return hd;
}

static void vk_eng_bind_targets(void* user, const u32* surfaces, u32 count, u32 depth)
{
    (void)user;
    if (count > RSX_BE_MAX_COLOR_TARGETS) count = RSX_BE_MAX_COLOR_TARGETS;
    for (u32 i = 0; i < RSX_BE_MAX_COLOR_TARGETS; i++)
        s_eng_bound.rt[i] = (i < count && surfaces) ? surfaces[i] : 0;
    s_eng_bound.nrt = count;
    s_eng_bound.depth = depth;
}

static void vk_eng_clear_color(void* user, u32 surface, const float rgba[4])
{
    (void)user;
    /* The debug hook reports the last colour asked for, as Metal's does. */
    s_vk.clear_argb = ((u32)(rgba[3] * 255.0f + 0.5f) << 24) |
                      ((u32)(rgba[0] * 255.0f + 0.5f) << 16) |
                      ((u32)(rgba[1] * 255.0f + 0.5f) <<  8) |
                       (u32)(rgba[2] * 255.0f + 0.5f);
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    if (!o || vk_begin()) return;
    VkClearColorValue v;
    memcpy(v.float32, rgba, sizeof v.float32);
    VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    pvkCmdClearColorImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
    vk_defer();
}

static void vk_eng_clear_depth_stencil(void* user, u32 depth, u32 flags,
                                       float depth_value, u8 stencil)
{
    (void)user;
    vk_eng_obj* o = vk_eng_get(depth, VK_ENG_DEPTH);
    VkImageAspectFlags clr = 0;
    if (flags & RSX_BE_CLEAR_DEPTH) clr |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if ((flags & RSX_BE_CLEAR_STENCIL) && (s_vk.eng_depth_aspect & VK_IMAGE_ASPECT_STENCIL_BIT))
        clr |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!o || !clr || vk_begin()) return;
    VkClearDepthStencilValue d = { depth_value, stencil };
    VkImageSubresourceRange r = { clr, 0, 1, 0, 1 };
    vk_barrier_image(o->im.img, s_vk.eng_depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
    pvkCmdClearDepthStencilImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &d, 1, &r);
    vk_defer();
}

/* The named surface becomes the frame: blit it onto the display image, then
 * the existing present does the readback, the PPM dump and the window. */
static void vk_eng_present(void* user, u32 surface)
{
    (void)user;
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    extern unsigned long long ps3_qpc_us(void);
    const unsigned long long b0 = ps3_qpc_us();
    /* Twice a second, how close the card is: past 88% of its budget new shadow
     * maps stay at the scene's scale; past 95% the engine evicts too. Waiting
     * for an allocation to fail meant a GPU wait for every one that did. */
    { static unsigned long long last, last_ev; unsigned u, bd;
      extern volatile int g_rsx_vram_tight;
      if (b0 - last >= 500000ull && !vk_vram_mb(&u, &bd) && bd) { last = b0;
          const int was = g_rsx_vram_tight;
          if ((unsigned long long)u * 100u > (unsigned long long)bd * 88u) g_rsx_vram_tight = 1;
          else if ((unsigned long long)u * 100u < (unsigned long long)bd * 78u) g_rsx_vram_tight = 0;
          if (was != g_rsx_vram_tight) VK_LOG("video memory %u MB of %u MB budget: shadow maps %s%c", u, bd,
                                              g_rsx_vram_tight ? "at the scene's scale for now" : "back to their own scale", 10);
          if ((unsigned long long)u * 100u > (unsigned long long)bd * 95u && b0 - last_ev >= 5000000ull) {
              last_ev = b0; g_rsx_vram_pressure = 1; } } }
    {   /* Once a minute: how full the GPU's memory is (vk_vram_mb). */
        static unsigned long long last; unsigned u, b;
        if (b0 - last >= 60000000ull) { last = b0;
            if (!vk_vram_mb(&u, &b)) VK_LOG("VRAM: %u MB used of %u MB budget, %u pool blocks of 64 MB, %ld allocations alive; "
                                            "render targets recycled %llu of %llu released; shadow maps at their own scale "
                                            "%llu made, %llu back to the scene's%c", u, b, s_pool_blocks, (long)s_live_allocs,
                                            s_rbin_hits, s_rbin_parked, s_zhi_made, s_zhi_demoted, 10); }
    }
    if (o && !vk_begin()) {
        vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        VkImageBlit blit = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .srcOffsets = { { 0, 0, 0 }, { (int32_t)o->w, (int32_t)o->h, 1 } },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstOffsets = { { 0, 0, 0 }, { (int32_t)s_vk.width, (int32_t)s_vk.height, 1 } },
        };
        pvkCmdBlitImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL,
                        s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                        (o->w == s_vk.width && o->h == s_vk.height) ? VK_FILTER_NEAREST
                                                                    : VK_FILTER_LINEAR);
        vk_defer();
    }
    s_pw_blit = ps3_qpc_us() - b0;
    const unsigned long long t0 = ps3_qpc_us();
    const int saving = SDL_AtomicGet(&s_pcache_busy);
    vk_cb_present(&s_vk, 0);
    const unsigned long long t1 = ps3_qpc_us();
    vk_pcache_save(0);
    const unsigned long long t2 = ps3_qpc_us();
    /* A present of 30 ms or more: where it went (RSX_HITCH's "present"). */
    if (t2 - t0 + s_pw_blit >= 30000ull) { static int n;
        if (n++ < 200)
            VK_LOG("[present-slow] %.1f ms: blit %.1f, ui %.1f, acquire %.1f, compose+submit %.1f, queue-present %.1f, cache save %.1f%s%c",
                   (t2 - t0 + s_pw_blit) / 1e3, s_pw_blit / 1e3, s_pw_ui / 1e3, s_pw_acq / 1e3, s_pw_sub / 1e3, s_pw_qp / 1e3, (t2 - t1) / 1e3,
                   saving ? " -- pipeline-cache save was running" : "", 10); }
}

static int vk_eng_readback(void* user, u32 surface, u32 x, u32 y, u32 w, u32 h,
                           void* out, u32 out_pitch)
{
    s_sync_calls[9]++;
    (void)user;
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    if (!o) o = vk_eng_get(surface, VK_ENG_TEXTURE);    /* a depth snapshot (R32F) */
    if (!o || !out || !out_pitch || !w || !h ||
        (u64)x + w > vk_lw(o) || (u64)y + h > vk_lh(o)) return -1;
    const size_t tight = (size_t)w * o->bpp;
    if (!o->bpp || out_pitch < tight) return -1;           /* rows would overlap */
    vk_buffer st = {0};
    if (vk_create_host_buffer(&st, (VkDeviceSize)tight * h, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        vk_destroy_buffer(&st); return -1;
    }
    /* A scaled image goes back to the guest's size first: the region is
     * stretched into a temporary of w x h (averaged for colour; depth is
     * point-sampled, as an average of depths is no depth at all). */
    vk_image down = {0};
    const int scaled = o->w != vk_lw(o) || o->h != vk_lh(o);
    if (scaled && vk_create_image(&down, o->fmt, w, h, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                  VK_IMAGE_ASPECT_COLOR_BIT)) {
        vk_destroy_image(&down); vk_destroy_buffer(&st); return -1;
    }
    int rc = -1;
    if (!vk_begin()) {
        vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        VkImage src = o->im.img;
        int32_t rx = (int32_t)x, ry = (int32_t)y;
        if (scaled) {
            vk_barrier_image(down.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
            VkImageBlit b = {
                .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .srcOffsets = { { (int32_t)vk_os(o, x), (int32_t)vk_os(o, y), 0 },
                                { (int32_t)vk_os(o, x + w), (int32_t)vk_os(o, y + h), 1 } },
                .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .dstOffsets = { { 0, 0, 0 }, { (int32_t)w, (int32_t)h, 1 } } };
            pvkCmdBlitImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, down.img, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                            o->fmt == VK_FORMAT_R8G8B8A8_UNORM ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
            vk_full_barrier();
            src = down.img; rx = ry = 0;
        }
        VkBufferImageCopy region = {
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageOffset = { rx, ry, 0 }, .imageExtent = { w, h, 1 },
        };
        pvkCmdCopyImageToBuffer(s_vk.cmd, src, VK_IMAGE_LAYOUT_GENERAL, st.buf, 1, &region);
        VkBufferMemoryBarrier hb = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = st.buf, .offset = 0, .size = VK_WHOLE_SIZE,
        };
        pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                              0, 0, NULL, 1, &hb, 0, NULL);
        if (!vk_submit_and_wait() && !vk_invalidate(&st)) {
            for (u32 row = 0; row < h; row++)
                memcpy((u8*)out + (size_t)row * out_pitch, (const u8*)st.ptr + (size_t)row * tight, tight);
            rc = 0;
        }
    }
    vk_destroy_image(&down);
    vk_destroy_buffer(&st);
    return rc;
}

/* ---- E3: textures ---------------------------------------------------------
 * The engine decodes every level to host rows (rsx_texture_decode) and hands
 * them over one upload each; the crossbar is a property of the view, so it
 * becomes the image view's component mapping. Cube maps are not handled yet:
 * the pixel decompiler declares cube units as separate bindings, which the
 * shared set layout does not have, so faces == 6 reports 0 (placeholder). */
static u32 vk_eng_texture_create(void* user, rsx_be_format fmt, u32 w, u32 h,
                                 u32 mips, u32 faces, u32 remap, u32 rsx_fmt)
{
    s_sync_calls[1]++;
    (void)user;
    u32 bpp;
    const VkFormat vf = vk_eng_format(fmt, &bpp);
    const int bc = (fmt == RSX_BE_FMT_BC1 || fmt == RSX_BE_FMT_BC2 || fmt == RSX_BE_FMT_BC3);
    if (vf == VK_FORMAT_UNDEFINED || !w || !h || w > 4096u || h > 4096u || (bc && !s_vk.bc_ok))
        return 0;
    const int cube = (faces == 6);
    if (cube && w != h) return 0;            /* Vulkan cube faces are square */
    if (!mips) mips = 1;
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0u, .imageType = VK_IMAGE_TYPE_2D,
        .format = vf, .extent = { w, h, 1 }, .mipLevels = mips, .arrayLayers = cube ? 6u : 1u,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    s_img_why[2]++;
    o->im.sub = 0; o->im.mem = VK_NULL_HANDLE;
    if ((s_vkc[0]++, pvkCreateImage(s_vk.device, &ici, NULL, &o->im.img)) != VK_SUCCESS) goto fail;
    if (vk_image_memory(&o->im) ||
        vk_eng_make_view_ex(o->im.img, vf, mips, vk_remap(remap, rsx_fmt), cube, &o->im.view))
        goto fail;
    o->kind = VK_ENG_TEXTURE; o->fmt = vf; o->w = w; o->h = h; o->cube = (u8)cube;
    o->bpp = bc ? 0 : bpp; o->block = bc ? bpp : 0;
    /* UNDEFINED -> GENERAL for every level before the uploads arrive. */
    if (vk_begin()) goto fail;
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    if (vk_defer()) goto fail;
    return hd;
fail:
    vk_destroy_image(&o->im);
    memset(o, 0, sizeof *o);
    return 0;
}

static void vk_eng_texture_upload(void* user, u32 texture, u32 face, u32 mip, u32 w, u32 h,
                                  const void* src, u32 row_bytes, u32 rows)
{
    (void)user;
    vk_eng_obj* o = vk_eng_get(texture, VK_ENG_TEXTURE);
    if (!o || !src || !row_bytes || !rows || face >= (o->cube ? 6u : 1u)) return;
    /* A BC row is a row of 4x4 blocks; the engine's `rows` already counts them. */
    const size_t tight = o->block ? (size_t)((w + 3u) / 4u) * o->block : (size_t)w * o->bpp;
    vk_eng_upload_rows(o->im.img, face, mip, w, h, tight, rows, src, row_bytes);
}

/* A colour target sampled with a unit's crossbar: an extra view of the same
 * image, cached by (surface, crossbar, format) and released with the target. */
static u32 vk_eng_surface_view(void* user, u32 surface, u32 remap, u32 rsx_format)
{
    s_sync_calls[3]++;
    (void)user;
    vk_eng_obj* t = vk_eng_get(surface, VK_ENG_COLOR);
    if (!t) return 0;
    /* Its own views only: a scan of the whole object table here was ~7% of
     * the RSX thread in Zolar Forest. */
    for (u32 i = t->views; i; i = s_eobj[i].next_view)
        if (s_eobj[i].remap == remap && s_eobj[i].rsx_fmt == rsx_format)
            return i;
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    if (vk_eng_make_view(t->im.img, t->fmt, 1, vk_remap(remap, rsx_format), &o->im.view)) {
        memset(o, 0, sizeof *o); return 0;
    }
    o->kind = VK_ENG_VIEW; o->fmt = t->fmt; o->w = t->w; o->h = t->h; o->lw = t->lw; o->lh = t->lh;
    o->surface = surface; o->remap = remap; o->rsx_fmt = rsx_format;
    o->next_view = t->views; t->views = hd;
    return hd;
}
/* ---- E4: depth snapshots --------------------------------------------------
 * A depth target read as a texture, which the engine asks for only after a
 * depth-writing draw. Metal resolves it with a fullscreen pass into R32Float;
 * Vulkan can do it with two copies on the GPU, since a D32_SFLOAT depth
 * aspect copies out as 32-bit floats: depth -> buffer -> an R32_SFLOAT
 * texture. Other depth formats would need a conversion pass: they report 0.
 *
 * Each depth target keeps ONE snapshot texture (its handle in `surface`),
 * refilled on every request, so nothing accumulates however often the engine
 * invalidates and asks again; it goes when the depth target goes. */
/* A copy of a colour target as a texture (kept in the target's `surface`
 * field, like a depth target's snapshot), for a draw that samples the target
 * it writes: A Crack in Time resolves its 2x multisampled scene in place, a
 * full-screen pass that reads 0x880000 at 1920x704 and writes it at 960x704.
 * Reading the target itself is undefined; reading guest memory instead (the
 * old fallback) read black and blacked out every frame of gameplay. */
static u32 vk_eng_color_snapshot(void* user, u32 color, u32 w, u32 h)
{
    s_sync_calls[6]++;
    vk_eng_obj* c = vk_eng_get(color, VK_ENG_COLOR);
    if (!c) return 0;
    if (w > vk_lw(c)) w = vk_lw(c);
    if (h > vk_lh(c)) h = vk_lh(c);
    if (!w || !h) return 0;
    const u32 lw = w, lh = h;            /* the guest's size; w,h become the image's */
    w = vk_rs(w); h = vk_rs(h);
    if (w > c->w) w = c->w;
    if (h > c->h) h = c->h;
    u32 snap = c->surface;
    vk_eng_obj* t = vk_eng_get(snap, VK_ENG_TEXTURE);
    if (!t || t->w != w || t->h != h || t->fmt != c->fmt) {
        if (t) vk_eng_release(user, snap);
        c->surface = 0;
        snap = vk_eng_alloc();
        if (!snap) return 0;
        c = vk_eng_get(color, VK_ENG_COLOR);
        t = &s_eobj[snap];
        s_img_why[3]++;
        if (vk_create_image(&t->im, c->fmt, w, h,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT)) {
            vk_destroy_image(&t->im); memset(t, 0, sizeof *t); return 0;
        }
        t->kind = VK_ENG_TEXTURE; t->fmt = c->fmt; t->bpp = c->bpp; t->w = w; t->h = h;
        t->lw = lw; t->lh = lh;
        c->surface = snap;
        if (vk_begin()) return 0;
        vk_barrier_image(t->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
        if (vk_defer()) return 0;
    }
    t->lw = lw; t->lh = lh;
    if (vk_begin()) return 0;
    vk_barrier_image(c->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image(t->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkImageCopy rg = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                       .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                       .extent = { w, h, 1 } };
    pvkCmdCopyImage(s_vk.cmd, c->im.img, VK_IMAGE_LAYOUT_GENERAL, t->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    return vk_defer() ? 0 : snap;
}

static u32 vk_eng_depth_snapshot_sized(void* user, u32 depth, u32 w, u32 h, u32 tw, u32 th)
{
    s_sync_calls[5]++;
    vk_eng_obj* z = vk_eng_get(depth, VK_ENG_DEPTH);
    if (!z) return 0;
    if (s_vk.eng_depth_format != VK_FORMAT_D32_SFLOAT && s_vk.eng_depth_format != VK_FORMAT_D32_SFLOAT_S8_UINT) {
        static int warned_fmt;
        if (!warned_fmt) { VK_LOG("engine: depth snapshots need D32_SFLOAT, this device uses %d\n",
                                  (int)s_vk.eng_depth_format); warned_fmt = 1; }
        return 0;
    }
    if (!tw || !th) { tw = w; th = h; }
    if (tw > 4096u || th > 4096u) return 0;
    if (w > vk_lw(z)) w = vk_lw(z);
    if (h > vk_lh(z)) h = vk_lh(z);
    if (w > tw) w = tw;
    if (h > th) h = th;
    if (!w || !h) return 0;
    /* From here w,h and tw,th are the images' sizes (the depth target's scale). */
    const u32 ltw = tw, lth = th;
    w = vk_os(z, w); h = vk_os(z, h); tw = vk_os(z, tw); th = vk_os(z, th);
    if (w > z->w) w = z->w;
    if (h > z->h) h = z->h;
    if (w > tw) w = tw;
    if (h > th) h = th;

    u32 snap = z->surface;
    vk_eng_obj* t = vk_eng_get(snap, VK_ENG_TEXTURE);
    if (t && (t->w != tw || t->h != th)) {
        /* Read at another size than last time (a depth buffer two passes use
         * at different sizes): swap in the spare if it is that size, and keep
         * this one as the spare -- not a release and a create each time. */
        const u32 sp = z->spare;
        vk_eng_obj* st = vk_eng_get(sp, VK_ENG_TEXTURE);
        z->spare = snap;
        z->surface = 0;
        if (st && st->w == tw && st->h == th) {
            z->surface = snap = sp; t = st;
            t->lw = ltw; t->lh = lth; t->sc = z->sc;
        } else {
            if (st) vk_eng_release(user, sp);
            z = vk_eng_get(depth, VK_ENG_DEPTH);
            t = NULL;
        }
    }
    if (!t || t->w != tw || t->h != th) {
        if (t) vk_eng_release(user, snap);
        z->surface = 0;
        snap = vk_eng_alloc();
        if (!snap) return 0;
        t = &s_eobj[snap];
        s_img_why[4]++;
        if (vk_create_image(&t->im, VK_FORMAT_R32_SFLOAT, tw, th,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT)) {
            vk_destroy_image(&t->im); memset(t, 0, sizeof *t); return 0;
        }
        t->kind = VK_ENG_TEXTURE; t->fmt = VK_FORMAT_R32_SFLOAT; t->bpp = 4; t->w = tw; t->h = th;
        t->lw = ltw; t->lh = lth; t->sc = z->sc;
        /* A depth texture read without a compare gives its depth in R, G and
         * B on the RSX (the units' remap selects that one channel for all
         * three). A one-channel R32F view gave 0 in G and B: A Crack in
         * Time's light composite reads .z of the hero's shadow depth, got 0,
         * and shaded the hero's whole shadow volume uniformly -- a dark
         * square on the ground. */
        { const VkComponentMapping rrr1 = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
                                            VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ONE };
          VkImageView v = VK_NULL_HANDLE;
          if (!vk_eng_make_view(t->im.img, VK_FORMAT_R32_SFLOAT, 1, rrr1, &v)) {
              pvkDestroyImageView(s_vk.device, t->im.view, NULL);   /* new: no GPU use yet */
              t->im.view = v;
          } }
        z = vk_eng_get(depth, VK_ENG_DEPTH);          /* same slot; reload for clarity */
        z->surface = snap;
        if (vk_begin()) return 0;
        vk_barrier_image(t->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
        if (vk_defer()) return 0;
    }
    t->sc = z->sc;

    /* One scratch buffer for every snapshot, in GPU memory, grown on demand:
     * a host buffer per snapshot was four allocations a frame and sent each
     * depth buffer over the bus twice. Snapshots are ordered by vk_begin's
     * full barrier, so reusing it between them is safe. */
    static vk_buffer s_zscratch;
    const VkDeviceSize need = (VkDeviceSize)w * h * 4u;
    if (s_zscratch.size < need) {
        vk_destroy_buffer(&s_zscratch);          /* deferred until the GPU is done */
        VkDeviceSize sz = 4u << 20;
        while (sz < need) sz *= 2;
        if (vk_create_device_buffer(&s_zscratch, sz,
                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
            vk_destroy_buffer(&s_zscratch); return 0;
        }
    }
    vk_buffer st = s_zscratch;
    int rc = vk_begin();
    if (!rc) {
        vk_barrier_image(z->im.img, s_vk.eng_depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
        vk_barrier_image(t->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        if (w < tw || h < th) {                    /* outside the drawn area: far plane */
            const VkClearColorValue far_ = { .float32 = { 1.0f, 1.0f, 1.0f, 1.0f } };
            const VkImageSubresourceRange cr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            pvkCmdClearColorImage(s_vk.cmd, t->im.img, VK_IMAGE_LAYOUT_GENERAL, &far_, 1, &cr);
            vk_full_barrier();
        }
        VkBufferImageCopy out = { .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
                                  .imageExtent = { w, h, 1 } };
        pvkCmdCopyImageToBuffer(s_vk.cmd, z->im.img, VK_IMAGE_LAYOUT_GENERAL, st.buf, 1, &out);
        VkBufferMemoryBarrier bb = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = st.buf, .offset = 0, .size = VK_WHOLE_SIZE };
        pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 0, NULL, 1, &bb, 0, NULL);
        VkBufferImageCopy in = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                 .imageExtent = { w, h, 1 } };
        pvkCmdCopyBufferToImage(s_vk.cmd, st.buf, t->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &in);
        rc = vk_defer();
    }
    /* RSX_VK_ZSNAP_DUMP=<dir>[,<skip>]: after <skip> snapshots (default
     * 20000), the next one of each guest size at the image's own resolution,
     * as z_<guest>_<image>.pgm (0 = near, 255 = far) -- what a
     * shadow map holds at RACACIT_SHADOW_RES. 16 files at most. */
    { static const char* dir = (const char*)-1; static long skip; static unsigned long long calls;
      static u32 done[16][2]; static u32 ndone;
      if (dir == (const char*)-1) { dir = getenv("RSX_VK_ZSNAP_DUMP");
                                    const char* c = dir ? strchr(dir, ',') : NULL; skip = c ? atol(c + 1) : 20000; }
      if (!rc && dir && ++calls > (unsigned long long)skip && ndone < 16) {
          int seen = 0;
          for (u32 i = 0; i < ndone; i++) if (done[i][0] == ltw && done[i][1] == lth) seen = 1;
          vk_buffer hb = {0};
          if (!seen && !vk_create_host_buffer(&hb, (VkDeviceSize)w * h * 4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT) && !vk_begin()) {
              const VkBufferImageCopy rb = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                             .imageExtent = { w, h, 1 } };
              pvkCmdCopyImageToBuffer(s_vk.cmd, t->im.img, VK_IMAGE_LAYOUT_GENERAL, hb.buf, 1, &rb);
              vk_full_barrier();
              if (!vk_submit_and_wait() && !vk_invalidate(&hb)) {
                  char path[600], d[512]; snprintf(d, sizeof d, "%s", dir);
                  { char* c = strchr(d, ','); if (c) *c = 0; }
                  snprintf(path, sizeof path, "%s/z_%ux%u_%ux%u.pgm", d, ltw, lth, w, h);
                  FILE* f = fopen(path, "wb");
                  if (f) { fprintf(f, "P5 %u %u 255%c", w, h, 10);
                           const float* px = (const float*)hb.ptr;
                           for (u64 i = 0; i < (u64)w * h; i++) { const float v = px[i] < 0 ? 0 : px[i] > 1 ? 1 : px[i];
                                                                   fputc((int)(v * 255.0f + 0.5f), f); }
                           fclose(f); }
                  done[ndone][0] = ltw; done[ndone][1] = lth; ndone++;
              }
          }
          vk_destroy_buffer(&hb);
      } }
    return rc ? 0 : snap;
}
static u32 vk_eng_depth_snapshot(void* user, u32 depth, u32 w, u32 h)
{
    return vk_eng_depth_snapshot_sized(user, depth, w, h, w, h);
}

/* A scaled depth copy where the depth format cannot be blitted -- every Radeon
 * reports no BLIT for D32_SFLOAT_S8_UINT, and the copy was skipped there. That
 * copy is the depth of A Crack in Time's half-res effects pass, which then ran
 * against an empty depth buffer: fire, smoke and sparks drawn through every
 * wall, smoke cut into bands (GitHub #3, #4, #6, all on AMD). Done instead with
 * operations every Vulkan driver must support: depth aspect -> buffer -> an
 * R32F image, a nearest blit of that to the new size (R32F blits are
 * mandatory), and back through the buffer into the destination's depth aspect.
 * Scratch images and buffer grow on demand; ordered by full barriers. */
static int vk_eng_depth_blit_ok(void)
{
    static int s_can = -1;
    if (s_can < 0) {
        VkFormatProperties fp;
        pvkGetPhysicalDeviceFormatProperties(s_vk.phys, s_vk.eng_depth_format, &fp);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
        s_can = (fp.optimalTilingFeatures & need) == need;
        if (!s_can) VK_LOG("engine: depth format %d cannot be blitted; scaled depth copies go through R32F%c",
                           (int)s_vk.eng_depth_format, 10);
    }
    return s_can;
}
static int vk_eng_blit_depth_staged(vk_eng_obj* s, u32 sx, u32 sy, u32 sw, u32 sh,
                                    vk_eng_obj* d, u32 dx, u32 dy, u32 dw, u32 dh)
{
    static vk_image a, b; static u32 aw, ah, bw, bh;
    static vk_buffer buf;
    const VkImageUsageFlags use = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (aw < sw || ah < sh) {
        vk_destroy_image(&a); aw = ah = 0;
        if (vk_create_image(&a, VK_FORMAT_R32_SFLOAT, sw, sh, use, VK_IMAGE_ASPECT_COLOR_BIT)) { vk_destroy_image(&a); return 0; }
        aw = sw; ah = sh;
    }
    if (bw < dw || bh < dh) {
        vk_destroy_image(&b); bw = bh = 0;
        if (vk_create_image(&b, VK_FORMAT_R32_SFLOAT, dw, dh, use, VK_IMAGE_ASPECT_COLOR_BIT)) { vk_destroy_image(&b); return 0; }
        bw = dw; bh = dh;
    }
    const VkDeviceSize need = (VkDeviceSize)((u64)sw * sh > (u64)dw * dh ? (u64)sw * sh : (u64)dw * dh) * 4u;
    if (buf.size < need) {
        vk_destroy_buffer(&buf);
        VkDeviceSize sz = 4u << 20;
        while (sz < need) sz *= 2;
        if (vk_create_device_buffer(&buf, sz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
            vk_destroy_buffer(&buf); return 0;
        }
    }
    if (vk_begin()) return 0;
    vk_barrier_image(s->im.img, s_vk.eng_depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image(d->im.img, s_vk.eng_depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image(a.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    vk_barrier_image(b.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    const VkBufferImageCopy c1 = { .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
                                   .imageOffset = { (int32_t)sx, (int32_t)sy, 0 }, .imageExtent = { sw, sh, 1 } };
    pvkCmdCopyImageToBuffer(s_vk.cmd, s->im.img, VK_IMAGE_LAYOUT_GENERAL, buf.buf, 1, &c1);
    vk_full_barrier();
    const VkBufferImageCopy c2 = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                   .imageExtent = { sw, sh, 1 } };
    pvkCmdCopyBufferToImage(s_vk.cmd, buf.buf, a.img, VK_IMAGE_LAYOUT_GENERAL, 1, &c2);
    vk_full_barrier();
    const VkImageBlit bl = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets = { { 0, 0, 0 }, { (int32_t)sw, (int32_t)sh, 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = { { 0, 0, 0 }, { (int32_t)dw, (int32_t)dh, 1 } } };
    pvkCmdBlitImage(s_vk.cmd, a.img, VK_IMAGE_LAYOUT_GENERAL, b.img, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_NEAREST);
    vk_full_barrier();
    const VkBufferImageCopy c3 = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                   .imageExtent = { dw, dh, 1 } };
    pvkCmdCopyImageToBuffer(s_vk.cmd, b.img, VK_IMAGE_LAYOUT_GENERAL, buf.buf, 1, &c3);
    vk_full_barrier();
    const VkBufferImageCopy c4 = { .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
                                   .imageOffset = { (int32_t)dx, (int32_t)dy, 0 }, .imageExtent = { dw, dh, 1 } };
    pvkCmdCopyBufferToImage(s_vk.cmd, buf.buf, d->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &c4);
    { static unsigned long long n; if (!n++) VK_LOG("engine: scaled depth copy through R32F (%ux%u -> %ux%u)%c", sw, sh, dw, dh, 10); }
    return vk_defer() ? 0 : 1;
}

/* NV3089 scaled-image blit between GPU targets, nearest-filtered as the RSX
 * point-samples. A Crack in Time builds the depth buffer of its half-res
 * effects pass this way (960x704 depth -> 512x352): done in guest memory only,
 * that pass ran against an empty depth buffer. */
static int vk_eng_blit(void* user, u32 src, u32 sx, u32 sy, u32 sw, u32 sh,
                       u32 dst, u32 dx, u32 dy, u32 dw, u32 dh)
{
    s_sync_calls[10]++;
    (void)user;
    vk_eng_obj* s = vk_eng_get(src, VK_ENG_DEPTH);
    vk_eng_obj* d = s ? vk_eng_get(dst, VK_ENG_DEPTH) : NULL;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    static int fails;
    #define VK_BLIT_FAIL(why) do { if (fails < 12) { fails++; VK_LOG("engine: blit %u -> %u failed: %s (src %u,%u %ux%u of %ux%u, dst %u,%u %ux%u of %ux%u)%c", \
        src, dst, why, sx, sy, sw, sh, s ? vk_lw(s) : 0u, s ? vk_lh(s) : 0u, dx, dy, dw, dh, d ? vk_lw(d) : 0u, d ? vk_lh(d) : 0u, 10); } return 0; } while (0)
    if (!s || !d) {
        const vk_eng_obj* ds = s; (void)ds;
        s = vk_eng_get(src, VK_ENG_COLOR);
        d = s ? vk_eng_get(dst, VK_ENG_COLOR) : NULL;
        aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        if (!s) VK_BLIT_FAIL("source is neither a depth nor a colour target");
        if (!d) VK_BLIT_FAIL("destination is not a target of the same kind");
        if (s->fmt != d->fmt) VK_BLIT_FAIL("formats differ");
    }
    if (!sw || !sh || !dw || !dh || sx + sw > vk_lw(s) || sy + sh > vk_lh(s) ||
        dx + dw > vk_lw(d) || dy + dh > vk_lh(d)) VK_BLIT_FAIL("rectangle outside a target");
    /* Guest rectangles to image rectangles (each at its own target's scale). */
    { const u32 sx1 = vk_os(s, sx + sw), sy1 = vk_os(s, sy + sh), dx1 = vk_os(d, dx + dw), dy1 = vk_os(d, dy + dh);
      sx = vk_os(s, sx); sy = vk_os(s, sy); dx = vk_os(d, dx); dy = vk_os(d, dy);
      sw = sx1 - sx; sh = sy1 - sy; dw = dx1 - dx; dh = dy1 - dy; }
    if (aspect == VK_IMAGE_ASPECT_DEPTH_BIT && !vk_eng_depth_blit_ok())
        return vk_eng_blit_depth_staged(s, sx, sy, sw, sh, d, dx, dy, dw, dh);
    if (vk_begin()) return 0;
    const VkImageAspectFlags bar = (aspect == VK_IMAGE_ASPECT_DEPTH_BIT) ? s_vk.eng_depth_aspect : aspect;
    vk_barrier_image(s->im.img, bar, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image(d->im.img, bar, VK_IMAGE_LAYOUT_GENERAL);
    VkImageBlit b = {
        .srcSubresource = { aspect, 0, 0, 1 },
        .srcOffsets = { { (int32_t)sx, (int32_t)sy, 0 }, { (int32_t)(sx + sw), (int32_t)(sy + sh), 1 } },
        .dstSubresource = { aspect, 0, 0, 1 },
        .dstOffsets = { { (int32_t)dx, (int32_t)dy, 0 }, { (int32_t)(dx + dw), (int32_t)(dy + dh), 1 } },
    };
    pvkCmdBlitImage(s_vk.cmd, s->im.img, VK_IMAGE_LAYOUT_GENERAL, d->im.img, VK_IMAGE_LAYOUT_GENERAL,
                    1, &b, VK_FILTER_NEAREST);
    return vk_defer() ? 0 : 1;
}

/* ---- E2: pipelines and draws ---------------------------------------------
 * A pipeline handle carries the two translated modules and the render state
 * the engine keyed it on. Topology is not part of that key (Metal sets it on
 * the encoder), but Vulkan 1.0 bakes it into the pipeline, so each handle
 * builds its VkPipeline per topology on first use.
 *
 * Bindings are the translator's contract (rsx_shader_spirv.h), and the set
 * layout, pipeline layout, descriptor set and constant UBO are the ones the
 * vtable guest path already creates. Vertex inputs are the engine's compact
 * layout: slot n is a float4 at n*16, at location n, as Metal reads it.
 * Known gaps, stated rather than guessed: vertex-texture units (their
 * samplers s0..s3 would alias the pixel samplers' binding), FP16 and other
 * non-RGBA8 colour targets, depth-only passes, and stencil (the depth format
 * has no stencil aspect yet, so the test is inert). */
/* As many as the engine will ask for (ENG_MAX_PIPELINES). At 512 a level
 * filled the table and every later pipeline failed without a word -- in A
 * Crack in Time the resolve and composite passes, so the scene was rendered
 * into its 960x704 target and the display stayed black. */
#define VK_ENG_MAX_PIPE   (8192 + 256)   /* the engine retires its own at 8192 (ENG_MAX_PIPELINES); the rest is its helpers' */
#define VK_ENG_TOPOLOGIES 5
#define VK_ENG_FB_CACHE   64

typedef struct vk_eng_pipe {
    int                 used;
    VkShaderModule      vs, fs;
    u64                 vs_hash, fs_hash;   /* their SPIR-V (vk_spv_hash), for pipelinelist.bin */
    u32                 vs_words, fs_words;
    rsx_be_render_state rs;
    u32                 nslots, stride, rt_count;
    u32                 rt_fi;              /* colour format: vk_eng_rt_fi */
    u32                 wpos;               /* its pixel program reads WPOS (baked at the scene's scale) */
    VkPipeline          variant[VK_ENG_TOPOLOGIES];
    /* vk_eng_variant's background builds: 0 none, 1 queued or building,
     * 2 done (ready holds it, or NULL when it failed). */
    SDL_atomic_t        pend[VK_ENG_TOPOLOGIES];
    VkPipeline          ready[VK_ENG_TOPOLOGIES];
    u32                 skipped[VK_ENG_TOPOLOGIES], build_ms[VK_ENG_TOPOLOGIES];
} vk_eng_pipe;

static vk_eng_pipe s_epipe[VK_ENG_MAX_PIPE];
/* The background pipeline builder (vk_eng_variant). */
#define VK_AP_QUEUE 128
static struct {
    SDL_Thread* th; SDL_mutex* mu; SDL_cond* cv; int stop, off;
    struct { u32 pipe, topo; VkRenderPass rp; } q[VK_AP_QUEUE];
    u32 head, tail;
    u32 built, logged, merged;
    SDL_atomic_t active;   /* builds queued or running */
    /* Its own pipeline cache: the driver locks a cache for a whole compile,
     * and the render thread's quick "is it cached" check on the main one
     * waited out every background compile (first test: 195 and 591 ms
     * stalls matching two compiles). Merged into the main one before that
     * is saved (vk_pcache_save). */
    VkPipelineCache cache;
} s_ap;

/* vk_pcache_save: the builder's pipelines into the main cache -- its
 * destination, so not while the pre-warm builds into it. */
static void vk_ap_merge(void)
{
    if (!s_ap.cache || s_ap.merged == s_ap.built || s_pw_running) return;
    if (pvkMergePipelineCaches(s_vk.device, s_g.pcache, 1, &s_ap.cache) == VK_SUCCESS) s_ap.merged = s_ap.built;
}

static struct {
    VkRenderPass rp[2][RSX_BE_MAX_COLOR_TARGETS + 1];   /* by colour format, colour count */
    VkRenderPass rp_zonly[2];                            /* by colour format: vk_eng_make_render_pass */
    struct { u32 key[RSX_BE_MAX_COLOR_TARGETS + 1]; VkFramebuffer fb; } fb[VK_ENG_FB_CACHE];
    u32          fb_count;
    vk_image     scratch_depth;                          /* when no zeta is bound */
    u32          scratch_w, scratch_h;
    u32          pipeline;
    u32          vs_bytes, ps_bytes;
    u32          tex[RSX_BE_MAX_TEXTURES];
    rsx_be_sampler_desc samp[RSX_BE_MAX_TEXTURES];
    u32          tex_mask;
    float        vp[4];
    u32          sc[4];
    int          have_vp, have_sc;
    u32          stencil_ref;
    vk_buffer    indices;
    u32          warned;
    u8*          vs_shadow;   /* (unused: see vs_src) */
    const u8*    vs_src;      /* batched mode: the engine's constant block, copied
                               * into the arena per draw. The engine's buffers
                               * persist and are rebound whenever they change, so
                               * a pointer saves a 7.5 KB copy per draw. */
    u8*          ps_shadow;
    /* Reuse between consecutive draws of one command buffer: a constant
     * block, the descriptor set, the bound pipeline and dynamic state stay
     * as they were unless something changed (all of it per draw was ~15% of
     * the RSX thread in Zolar Forest). */
    int          vs_dirty, ps_dirty, tex_dirty;
    u32          reuse_epoch, last_obj_gen;
    VkDeviceSize last_uvo, last_upo;
    u32          last_vsb, last_psb;
    VkDescriptorSet last_set, bound_set;
    VkDeviceSize bound_uvo, bound_upo;
    VkPipeline   last_pipe;
    u32          dyn_epoch, dyn_w, dyn_h, dyn_ref, dyn_have_vp, dyn_have_sc;
    float        dbias_units, dbias_factor, dyn_dbu, dyn_dbf;   /* polygon offset */
    float        blend_c[4], dyn_blend_c[4];   /* SET_BLEND_COLOR, for the pipelines that use it */
    int          blend_c_on;           /* the draw's pipeline blends with it (vk_rs_blend_const) */
    float        dyn_vp[4];
    u32          dyn_sc[4];
    float        fb_sc, dyn_fbsc;      /* the draw's targets' scale (s_rs, or a shadow map's) */
    u32          zrt[2];               /* by colour format: the colour image a depth-only
                                          pass into a shadow map draws with (vk_eng_draw) */
} s_e2;

/* One sampler per distinct guest sampler state (filters, wraps, LOD range,
 * border class), found by hash. v62 kept 64 and gave every state after the
 * 64th the null backend's nearest + repeat sampler, so the longer a session
 * ran -- a heavy fight brings new effects -- the more textures came out as
 * hard-edged texel squares tiled past their edges: the owner's "boxes over
 * enemies and explosions". Vulkan allows 4000 samplers (NVIDIA's limit). */
#define VK_ENG_SAMP_CACHE 1024
#define VK_ENG_SAMP_HASH  2048
static struct { u64 key; VkSampler s; } s_esamp[VK_ENG_SAMP_CACHE];
static u32 s_esamp_count;
static u16 s_esamp_hash[VK_ENG_SAMP_HASH];   /* s_esamp index + 1; 0 = empty */
static u32 vk_esamp_slot(u64 key)
{
    u64 h = key * 0x9E3779B97F4A7C15ull;
    return (u32)(h >> 53) & (VK_ENG_SAMP_HASH - 1u);
}

/* The texture-filter options (anisotropy, trilinear) for the samplers built
 * from here on: the cached ones are retired -- destroyed once the GPU is done
 * with the batches that may use them -- and descriptor sets rebuilt. The
 * settings menu changes these live. */
static void vk_apply_filter_options(const rsx_host_options* o)
{
    const float a = (o->aniso >= 2 && s_vk.max_aniso >= 2.0f) ? (float)o->aniso : 0.0f;
    s_vk.aniso = a > s_vk.max_aniso ? s_vk.max_aniso : a;
    s_vk.trilinear = o->trilinear;
    for (u32 i = 0; i < s_esamp_count; i++)
        if (!vk_bt_push(6, (uint64_t)(uintptr_t)s_esamp[i].s, 0, 0))
            pvkDestroySampler(s_vk.device, s_esamp[i].s, NULL);
    s_esamp_count = 0;
    memset(s_esamp_hash, 0, sizeof s_esamp_hash);
    s_e2.tex_dirty = 1;
    VK_LOG("texture filtering: anisotropic %.0fx, trilinear %s\n", (double)s_vk.aniso, s_vk.trilinear ? "on" : "off");
}

/* Only the framebuffers with `handle` among their attachments. Releasing a
 * target dropped every framebuffer -- and the open render pass -- and A
 * Crack in Time releases a few a frame (depth-as-colour targets and depth
 * snapshots follow the size a depth buffer is used at), so ~20 framebuffers
 * were rebuilt every frame in Axiom City. */
static void vk_eng_drop_framebuffers_using(u32 handle)
{
    u32 keep = 0;
    for (u32 i = 0; i < s_e2.fb_count; i++) {
        int uses = 0;
        for (u32 k = 0; k <= RSX_BE_MAX_COLOR_TARGETS; k++) if (s_e2.fb[i].key[k] == handle) uses = 1;
        if (!uses) { if (keep != i) s_e2.fb[keep] = s_e2.fb[i]; keep++; continue; }
        if (s_e2.fb[i].fb == s_bt.fb) {               /* the open pass draws into it */
            if (s_bt.recording) vk_bt_end_pass();
            s_bt.fb = VK_NULL_HANDLE;
        }
        if (s_e2.fb[i].fb && !vk_bt_defer_framebuffer(s_e2.fb[i].fb))
            pvkDestroyFramebuffer(s_vk.device, s_e2.fb[i].fb, NULL);
    }
    s_e2.fb_count = keep;
}

static void vk_eng_drop_framebuffers(void)
{
    if (s_bt.recording) vk_bt_end_pass();      /* the open pass may be using one */
    for (u32 i = 0; i < s_e2.fb_count; i++)
        if (s_e2.fb[i].fb && !vk_bt_defer_framebuffer(s_e2.fb[i].fb))
            pvkDestroyFramebuffer(s_vk.device, s_e2.fb[i].fb, NULL);
    s_e2.fb_count = 0;
    s_bt.fb = VK_NULL_HANDLE;
}

/* Colour target formats with a render pass of their own: 0 RGBA8, 1 RGBA16F
 * (SET_SURFACE_FORMAT 0xB -- A Crack in Time draws its pause-menu backdrop
 * into a 240x176 one; without it the menu opened on the magenta "missing
 * texture" for a frame). -1 for any other. */
static int vk_eng_rt_fi(VkFormat f)
{
    return f == VK_FORMAT_R8G8B8A8_UNORM ? 0 : f == VK_FORMAT_R16G16B16A16_SFLOAT ? 1 : -1;
}

/* The render pass for `n` colour attachments of format `fi` plus depth,
 * LOAD/STORE in GENERAL like the vtable path's: clears are separate
 * operations. */
/* zonly: the colour attachments are neither loaded nor stored -- a depth-only
 * pass into a shadow map with the scratch colour image (vk_eng_zonly_color),
 * which nothing reads, so no pass moves its pixels in or out of memory.
 * Compatible with the LOAD/STORE pass (load and store ops do not count), so
 * the same pipelines and framebuffers serve both. */
static VkRenderPass vk_eng_make_render_pass(u32 n, u32 fi, int zonly)
{
    VkAttachmentDescription att[RSX_BE_MAX_COLOR_TARGETS + 1];
    VkAttachmentReference   cref[RSX_BE_MAX_COLOR_TARGETS];
    for (u32 i = 0; i <= n; i++) {
        const int discard = zonly && i < n;
        att[i] = (VkAttachmentDescription){
            .format = (i < n) ? (fi ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM) : s_vk.eng_depth_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = discard ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = discard ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL };
        if (i < n) cref[i] = (VkAttachmentReference){ i, VK_IMAGE_LAYOUT_GENERAL };
    }
    VkAttachmentReference dref = { n, VK_IMAGE_LAYOUT_GENERAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 .colorAttachmentCount = n, .pColorAttachments = cref,
                                 .pDepthStencilAttachment = &dref };
    VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                   .attachmentCount = n + 1, .pAttachments = att,
                                   .subpassCount = 1, .pSubpasses = &sub };
    VkRenderPass rp = VK_NULL_HANDLE;
    if (pvkCreateRenderPass(s_vk.device, &rci, NULL, &rp) != VK_SUCCESS) rp = VK_NULL_HANDLE;
    return rp;
}

static VkRenderPass vk_eng_render_pass(u32 n, u32 fi)
{
    if (n < 1 || n > RSX_BE_MAX_COLOR_TARGETS || fi > 1) return VK_NULL_HANDLE;
    if (!s_e2.rp[fi][n]) s_e2.rp[fi][n] = vk_eng_make_render_pass(n, fi, 0);
    return s_e2.rp[fi][n];
}
static VkRenderPass vk_eng_render_pass_zonly(u32 fi)
{
    if (fi > 1) return VK_NULL_HANDLE;
    if (!s_e2.rp_zonly[fi]) s_e2.rp_zonly[fi] = vk_eng_make_render_pass(1, fi, 1);
    return s_e2.rp_zonly[fi];
}

/* Tables carried from the Metal backend, itself copied from the D3D12 live
 * draw engine a title has shipped on. */
static VkBlendFactor vk_gcm_blend_factor(u32 f)
{
    switch (f) {
    case 0x0000: return VK_BLEND_FACTOR_ZERO;
    case 0x0001: return VK_BLEND_FACTOR_ONE;
    case 0x0300: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x0301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x0302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x0303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x0304: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x0305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x0306: return VK_BLEND_FACTOR_DST_COLOR;
    case 0x0307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x0308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default:     return VK_BLEND_FACTOR_ONE;
    }
}
static VkBlendOp vk_gcm_blend_op(u32 e)
{
    switch (e) {
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default:     return VK_BLEND_OP_ADD;                  /* FUNC_ADD 0x8006 */
    }
}
static VkCompareOp vk_gcm_compare(u32 f)
{
    return (f >= 0x0200u && f <= 0x0206u) ? (VkCompareOp)(f - 0x0200u) : VK_COMPARE_OP_ALWAYS;
}
static VkStencilOp vk_gcm_stencil_op(u32 op)
{
    switch (op) {
    case 0x0000: return VK_STENCIL_OP_ZERO;
    case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150A: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default:     return VK_STENCIL_OP_KEEP;               /* GL_KEEP 0x1E00 */
    }
}
static VkStencilOpState vk_gcm_stencil_face(u32 func, u32 fail, u32 zfail, u32 zpass,
                                            u32 cmp_mask, u32 write_mask)
{
    VkStencilOpState s = { .failOp = vk_gcm_stencil_op(fail), .passOp = vk_gcm_stencil_op(zpass),
                           .depthFailOp = vk_gcm_stencil_op(zfail),
                           .compareOp = vk_gcm_compare(func),
                           .compareMask = cmp_mask & 0xFFu, .writeMask = write_mask & 0xFFu,
                           .reference = 0 };                /* dynamic */
    return s;
}

/* RSX_ENG_STATS: shader translations / engine pipeline variants built, and
 * the render-thread time they took. */
static unsigned long long s_vkt_spv, s_vkt_spv_us, s_vkt_var, s_vkt_var_us;

/* A SPIR-V program's name in pipelinelist.bin: FNV-1a over its words. */
static u64 vk_spv_hash(const u32* w, u32 n)
{
    u64 h = 1469598103934665603ull ^ n;
    for (u32 i = 0; i < n; i++) { h ^= w[i]; h *= 1099511628211ull; }
    return h;
}

static int vk_eng_translate(const char* hlsl, int stage, VkShaderModule* out, u64* hash, u32* nwords)
{
    u32 words = 0;
    const unsigned long long _t0 = ps3_qpc_us();
    s_vkt_spv++;
    const int bad = rsx_hlsl_to_spirv(hlsl, stage, s_g.spv, VK_SPV_WORDS, &words, s_g.log, sizeof s_g.log) || !words;
    s_vkt_spv_us += ps3_qpc_us() - _t0;
    if (bad) {
        VK_LOG("engine: %s translation failed: %s\n",
               stage == RSX_SHADER_STAGE_VERTEX ? "vertex" : "fragment", s_g.log);
        return -1;
    }
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = (size_t)words * 4u, .pCode = s_g.spv };
    *hash = vk_spv_hash(s_g.spv, words);
    *nwords = words;
    return pvkCreateShaderModule(s_vk.device, &ci, NULL, out) == VK_SUCCESS ? 0 : -1;
}

static u32 vk_eng_pipeline_create(void* user, const char* vs_hlsl, const char* ps_hlsl,
                                  const rsx_be_render_state* rs, const rsx_vertex_layout_plan* layout,
                                  u32 vertex_stride, rsx_be_format rt_fmt, u32 rt_count)
{
    s_sync_calls[7]++;
    (void)user;
    /* Same gate as Metal: no guest programs, no engine pipelines. */
    if (!s_g.on || !vs_hlsl || !ps_hlsl || !rs || !layout || !vertex_stride) return 0;
    /* Vertex textures declare t16..t19 / s0..s3 bindings the shared set
     * layout does not have; a pipeline reading an undeclared descriptor
     * faults some drivers outright (llvmpipe). Drop those draws instead. */
    if (strstr(vs_hlsl, "rsx_vtex")) {
        if (!(s_e2.warned & 64u)) { VK_LOG("engine: vertex textures not supported yet; draws dropped\n"); s_e2.warned |= 64u; }
        return 0;
    }
    if (rt_fmt != RSX_BE_FMT_R8G8B8A8 && rt_fmt != RSX_BE_FMT_R16G16B16A16F) {
        if (!(s_e2.warned & 1u)) { VK_LOG("engine: colour format %d not supported yet\n", (int)rt_fmt); s_e2.warned |= 1u; }
        return 0;
    }
    if (!rt_count) rt_count = 1;
    if (rt_count > RSX_BE_MAX_COLOR_TARGETS) rt_count = RSX_BE_MAX_COLOR_TARGETS;
    u32 slot = VK_ENG_MAX_PIPE;
    for (u32 i = 0; i < VK_ENG_MAX_PIPE; i++) if (!s_epipe[i].used) { slot = i; break; }
    if (slot == VK_ENG_MAX_PIPE) {
        if (!(s_e2.warned & 128u)) { VK_LOG("engine: pipeline table full (%d); draws dropped%c", VK_ENG_MAX_PIPE, 10); s_e2.warned |= 128u; }
        return 0;
    }
    vk_eng_pipe* P = &s_epipe[slot];
    memset(P, 0, sizeof *P);
    if (vk_eng_translate(vs_hlsl, RSX_SHADER_STAGE_VERTEX, &P->vs, &P->vs_hash, &P->vs_words)) return 0;
    if (vk_eng_translate(ps_hlsl, RSX_SHADER_STAGE_FRAGMENT, &P->fs, &P->fs_hash, &P->fs_words)) {
        pvkDestroyShaderModule(s_vk.device, P->vs, NULL); memset(P, 0, sizeof *P); return 0;
    }
    P->rs = *rs;
    P->nslots = layout->count;
    P->stride = vertex_stride;
    P->rt_count = rt_count;
    P->rt_fi = rt_fmt == RSX_BE_FMT_R16G16B16A16F ? 1u : 0u;
    P->wpos = strstr(ps_hlsl, "input.position") != NULL;
    P->used = 1;
    return slot + 1u;
}

static void vk_eng_pipeline_release(void* user, u32 pipeline)
{
    (void)user;
    if (!pipeline || pipeline > VK_ENG_MAX_PIPE || !s_epipe[pipeline - 1].used) return;
    vk_eng_pipe* P = &s_epipe[pipeline - 1];
    /* A background build of one of its variants uses its shader modules:
     * wait for it (rare: the engine retires pipelines it has not used for a
     * while). A finished one was never drawn with: destroyed now. */
    for (u32 t = 0; t < VK_ENG_TOPOLOGIES; t++) {
        while (SDL_AtomicGet(&P->pend[t]) == 1 && s_ap.th) SDL_Delay(1);
        if (SDL_AtomicGet(&P->pend[t]) == 2) { SDL_MemoryBarrierAcquire(); if (P->ready[t]) pvkDestroyPipeline(s_vk.device, P->ready[t], NULL); }
    }
    /* The engine retires pipelines while it runs (its table is LRU): a batch
     * still in flight may draw with one, so it goes once that batch is done.
     * Shader modules are not referenced by command buffers. */
    for (u32 t = 0; t < VK_ENG_TOPOLOGIES; t++)
        if (P->variant[t] && !vk_bt_push(7, (uint64_t)(uintptr_t)P->variant[t], 0, 0))
            pvkDestroyPipeline(s_vk.device, P->variant[t], NULL);
    s_e2.last_pipe = VK_NULL_HANDLE;
    if (P->vs) pvkDestroyShaderModule(s_vk.device, P->vs, NULL);
    if (P->fs) pvkDestroyShaderModule(s_vk.device, P->fs, NULL);
    memset(P, 0, sizeof *P);
    if (s_e2.pipeline == pipeline) s_e2.pipeline = 0;
}

/* A game pipeline variant as the Vulkan state it is built from: every value
 * vk_vdesc_build puts into its VkGraphicsPipelineCreateInfo and no handle, so
 * it means the same to any driver -- the record pipelinelist.bin keeps. The
 * shaders are named by their SPIR-V (hash and size), as RSX_SHADER_CACHE's
 * file holds them. Change kPlistMagic when a field changes meaning. */
typedef struct vk_vdesc {
    u64 vs_hash, fs_hash;
    u32 vs_words, fs_words;
    u32 topology, nslots, stride, rt_count, rt_fi;
    u32 cull, front_face, depth_bias, depth_clamp;
    u32 depth_test, depth_write, depth_cmp, stencil_test;
    u32 stencil[2][6];        /* front, back: fail, pass, depth-fail, compare, compare mask, write mask */
    u32 blend, blend_f[6];    /* src/dst colour factor, colour op, src/dst alpha factor, alpha op */
    u32 write_mask;
    u32 check;                /* vk_fnv1a32 of everything before it */
} vk_vdesc;

static void vk_vdesc_stencil(u32 out[6], VkStencilOpState s)
{
    out[0] = s.failOp;    out[1] = s.passOp;      out[2] = s.depthFailOp;
    out[3] = s.compareOp; out[4] = s.compareMask; out[5] = s.writeMask;
}

/* What pipeline P drawing topology `topo` is built from. */
static void vk_vdesc_from(vk_vdesc* d, const vk_eng_pipe* P, VkPrimitiveTopology topo)
{
    const rsx_be_render_state* rs = &P->rs;
    memset(d, 0, sizeof *d);
    d->vs_hash = P->vs_hash; d->vs_words = P->vs_words;
    d->fs_hash = P->fs_hash; d->fs_words = P->fs_words;
    d->topology = (u32)topo;
    d->nslots = P->nslots; d->stride = P->stride;
    d->rt_count = P->rt_count; d->rt_fi = P->rt_fi;
    /* CULL_FACE FRONT 0x0404 / BACK 0x0405 / FRONT_AND_BACK 0x0408 (front, as
     * in D3D12 and Metal); FRONT_FACE CCW 0x0901 maps directly: both APIs
     * judge winding on screen, and the Y flip in the vertex stage makes this
     * screen the same one D3D's viewport produces. */
    static int _co = -1; if (_co < 0) _co = getenv("CULL_OFF") ? 1 : 0;
    d->cull = (_co > 0 || !rs->cull_enable) ? VK_CULL_MODE_NONE
            : (rs->cull_face == 0x0404u || rs->cull_face == 0x0408u) ? VK_CULL_MODE_FRONT_BIT
                                                                     : VK_CULL_MODE_BACK_BIT;
    static int _cfl = -1; if (_cfl < 0) _cfl = getenv("CULL_FLIP") ? 1 : 0;
    const int ccw = ((rs->front_face == 0x0901u) ? 1 : 0) ^ (_cfl > 0 ? 1 : 0);
    d->front_face = ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    d->depth_bias = rs->poly_offset_fill ? 1u : 0u;              /* values: dynamic */
    { static int force = -1; if (force < 0) force = getenv("RSX_DEPTH_CLAMP") ? 1 : 0;
      d->depth_clamp = (force && s_vk.depth_clamp_ok) ? 1u : 0u; }
    d->depth_test  = rs->depth_test ? 1u : 0u;
    d->depth_write = (rs->depth_test && rs->depth_write) ? 1u : 0u;
    d->depth_cmp   = (u32)vk_gcm_compare(rs->depth_func);
    d->stencil_test = rs->stencil_enable ? 1u : 0u;
    const VkStencilOpState front = vk_gcm_stencil_face(rs->s_func, rs->s_fail, rs->s_zfail, rs->s_zpass,
                                                       rs->s_func_mask, rs->s_write_mask);
    vk_vdesc_stencil(d->stencil[0], front);
    vk_vdesc_stencil(d->stencil[1], rs->stencil_two_sided
        ? vk_gcm_stencil_face(rs->bs_func, rs->bs_fail, rs->bs_zfail, rs->bs_zpass,
                              rs->s_func_mask, rs->s_write_mask)
        : front);
    d->blend = rs->blend_enable ? 1u : 0u;
    d->blend_f[0] = (u32)vk_gcm_blend_factor(rs->sf_rgb);
    d->blend_f[1] = (u32)vk_gcm_blend_factor(rs->df_rgb);
    d->blend_f[2] = (u32)vk_gcm_blend_op(rs->eq_rgb);
    d->blend_f[3] = (u32)vk_gcm_blend_factor(rs->sf_a);
    d->blend_f[4] = (u32)vk_gcm_blend_factor(rs->df_a);
    d->blend_f[5] = (u32)vk_gcm_blend_op(rs->eq_a);
    /* nv40 COLOR_MASK: B [0:7], G [8:15], R [16:23], A [24:31], any bit set.
     * Every attachment takes target A's blend and mask (see rsx_draw_engine.h). */
    if ((rs->color_mask >> 16) & 0xFFu) d->write_mask |= VK_COLOR_COMPONENT_R_BIT;
    if ((rs->color_mask >>  8) & 0xFFu) d->write_mask |= VK_COLOR_COMPONENT_G_BIT;
    if ((rs->color_mask >>  0) & 0xFFu) d->write_mask |= VK_COLOR_COMPONENT_B_BIT;
    if ((rs->color_mask >> 24) & 0xFFu) d->write_mask |= VK_COLOR_COMPONENT_A_BIT;
    d->check = vk_fnv1a32((const u8*)d, (u32)offsetof(vk_vdesc, check));
}

static int vk_factor_is_const(u32 f)
{
    return f >= (u32)VK_BLEND_FACTOR_CONSTANT_COLOR && f <= (u32)VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
}
static int vk_rs_blend_const(const rsx_be_render_state* rs)
{
    return rs->blend_enable && (vk_factor_is_const((u32)vk_gcm_blend_factor(rs->sf_rgb)) || vk_factor_is_const((u32)vk_gcm_blend_factor(rs->df_rgb)) ||
                                vk_factor_is_const((u32)vk_gcm_blend_factor(rs->sf_a)) || vk_factor_is_const((u32)vk_gcm_blend_factor(rs->df_a)));
}

/* The pipeline, into the shared pipeline cache. Any thread: the cache is
 * internally synchronised for pipeline creation. */
static VkResult vk_vdesc_build(const vk_vdesc* d, VkShaderModule vs, VkShaderModule fs,
                               VkRenderPass rp, VkPipeline* out, VkPipelineCreateFlags flags, VkPipelineCache pc)
{
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
    /* RSX_PIPELINE_SALT=<n> (tests only): a specialization constant no shader
     * declares, so the driver's own shader cache (NVIDIA: DXCache) misses as
     * after a driver update; it changes nothing drawn. */
    const VkSpecializationMapEntry salt_e = { 0x7FFFu, 0, 4 };
    const VkSpecializationInfo salt_i = { 1, &salt_e, 4, &s_g.pcache_salt };
    if (s_g.pcache_salt) st[0].pSpecializationInfo = st[1].pSpecializationInfo = &salt_i;
    VkVertexInputBindingDescription vb = { 0, d->stride, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[VK_GUEST_ATTRS];
    for (u32 s = 0; s < d->nslots; s++)
        va[s] = (VkVertexInputAttributeDescription){ s, 0, VK_FORMAT_R32G32B32A32_SFLOAT, s * 16u };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
        .vertexAttributeDescriptionCount = d->nslots, .pVertexAttributeDescriptions = va };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = (VkPrimitiveTopology)d->topology };
    VkPipelineViewportStateCreateInfo vps = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo ras = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = d->depth_clamp, .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = d->cull, .frontFace = (VkFrontFace)d->front_face,
        .depthBiasEnable = d->depth_bias, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkStencilOpState sf[2];
    for (u32 k = 0; k < 2; k++)
        sf[k] = (VkStencilOpState){ (VkStencilOp)d->stencil[k][0], (VkStencilOp)d->stencil[k][1],
                                    (VkStencilOp)d->stencil[k][2], (VkCompareOp)d->stencil[k][3],
                                    d->stencil[k][4], d->stencil[k][5], 0 };   /* reference: dynamic */
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = d->depth_test, .depthWriteEnable = d->depth_write,
        .depthCompareOp = (VkCompareOp)d->depth_cmp,
        .stencilTestEnable = d->stencil_test, .front = sf[0], .back = sf[1] };
    VkPipelineColorBlendAttachmentState cba[RSX_BE_MAX_COLOR_TARGETS];
    for (u32 r = 0; r < d->rt_count; r++)
        cba[r] = (VkPipelineColorBlendAttachmentState){
            .blendEnable = d->blend,
            .srcColorBlendFactor = (VkBlendFactor)d->blend_f[0],
            .dstColorBlendFactor = (VkBlendFactor)d->blend_f[1],
            .colorBlendOp = (VkBlendOp)d->blend_f[2],
            .srcAlphaBlendFactor = (VkBlendFactor)d->blend_f[3],
            .dstAlphaBlendFactor = (VkBlendFactor)d->blend_f[4],
            .alphaBlendOp = (VkBlendOp)d->blend_f[5],
            .colorWriteMask = d->write_mask };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = d->rt_count, .pAttachments = cba };
    /* The blend constant is dynamic only where a factor uses it: every other
     * pipeline stays as it was, and so does its entry in the players' caches. */
    VkDynamicState dyn[5] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                              VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_BIAS,
                              VK_DYNAMIC_STATE_BLEND_CONSTANTS };
    const int blend_c = d->blend && (vk_factor_is_const(d->blend_f[0]) || vk_factor_is_const(d->blend_f[1]) ||
                                     vk_factor_is_const(d->blend_f[3]) || vk_factor_is_const(d->blend_f[4]));
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = blend_c ? 5u : 4u, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .flags = flags,
        .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &ras, .pMultisampleState = &ms,
        .pDepthStencilState = &ds, .pColorBlendState = &cb, .pDynamicState = &dy,
        .layout = s_g.pipe_layout, .renderPass = rp, .subpass = 0 };
    return pvkCreateGraphicsPipelines(s_vk.device, pc, 1, &gci, NULL, out);
}

/* ---- pipelinelist.bin and the pipeline pre-warm ----------------------------
 * RSX_PIPELINE_LIST=<file> (main.cpp: pipelinelist.bin next to the exe): every
 * engine pipeline variant built, once each, appended as it first appears
 * ("RSXPLS01", u32 record size, u32 0, then vk_vdesc records).
 * When the pipeline cache gave nothing to start from -- missing, refused, or
 * written by another driver (vk_pcache_init) -- a low-priority thread builds
 * the listed variants again into it and destroys each pipeline right away.
 * After a graphics driver update every area otherwise stalled once, 0.2-1.7 s
 * at a time, while the new driver compiled its pipelines at first use (owner,
 * 2026-10-20; HANDOFF "Release v50"). Nothing waits for the thread: two
 * threads may create pipelines into one cache (only vkMergePipelineCaches'
 * destination needs external synchronisation), and it reads the shaders from
 * RSX_SHADER_CACHE's file on a handle of its own, not through the SPIR-V
 * cache's lock. RSX_PIPELINE_PREWARM=0: never; =1: whenever there is a list
 * (with a warm cache every build is a hit). A list over VK_PLIST_MAX records
 * keeps its newest half at the next start.
 * The u32 after the record size is 0 in a list written while CONSTANT_ALPHA
 * factors were read as ONE: its normal-pass records (ONE,ZERO; ONE,ZERO) become
 * what those draws build now, once, and the pre-warm runs that one time even with
 * a warm cache, or each would stall at first use after the update. */
#define VK_PLIST_MAX 65536u
static const char kPlistMagic[8] = { 'R', 'S', 'X', 'P', 'L', 'S', '0', '1' };

static struct {
    FILE* f;                  /* appending; NULL: no list */
    u64*  seen;               /* records' vk_vdesc_key, open addressing, 0 = empty */
    u32   cap, n;
} s_pl;

static struct {
    SDL_Thread*  thread;
    SDL_atomic_t stop;
    vk_vdesc*    recs;        /* the list as loaded; the thread frees it */
    u32          n;
} s_pw;

static u64 vk_vdesc_key(const vk_vdesc* d)
{
    const u64 k = vk_fnv1a64(d, offsetof(vk_vdesc, check), 1469598103934665603ull);
    return k ? k : 1u;
}

/* Values only vk_vdesc_from produces: a record from the file goes to the
 * driver, and its check only catches a torn write. */
static int vk_vdesc_ok(const vk_vdesc* d)
{
    if (d->check != vk_fnv1a32((const u8*)d, (u32)offsetof(vk_vdesc, check))) return 0;
    if (!d->vs_words || !d->fs_words || d->vs_words > VK_SPV_WORDS || d->fs_words > VK_SPV_WORDS) return 0;
    if (d->topology > VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || d->nslots > VK_GUEST_ATTRS || !d->stride) return 0;
    if (d->rt_count < 1 || d->rt_count > RSX_BE_MAX_COLOR_TARGETS || d->rt_fi > 1) return 0;
    if (d->cull > VK_CULL_MODE_FRONT_AND_BACK || d->front_face > 1 || d->depth_bias > 1) return 0;
    if (d->depth_clamp > 1 || (d->depth_clamp && !s_vk.depth_clamp_ok)) return 0;
    if (d->depth_test > 1 || d->depth_write > 1 || d->depth_cmp > VK_COMPARE_OP_ALWAYS || d->stencil_test > 1) return 0;
    for (u32 k = 0; k < 2; k++) {
        for (u32 i = 0; i < 3; i++) if (d->stencil[k][i] > VK_STENCIL_OP_DECREMENT_AND_WRAP) return 0;
        if (d->stencil[k][3] > VK_COMPARE_OP_ALWAYS || d->stencil[k][4] > 0xFFu || d->stencil[k][5] > 0xFFu) return 0;
    }
    if (d->blend > 1 || d->write_mask > 0xFu) return 0;
    for (u32 i = 0; i < 6; i++)
        if (d->blend_f[i] > (i % 3u == 2u ? (u32)VK_BLEND_OP_MAX : (u32)VK_BLEND_FACTOR_SRC_ALPHA_SATURATE)) return 0;
    return 1;
}

/* 1 when the record is on the list already; otherwise it is now. */
static int vk_plist_seen(u64 k)
{
    if (2u * (s_pl.n + 1u) > s_pl.cap) {
        const u32 cap = s_pl.cap ? s_pl.cap * 2u : 4096u;
        u64* t = (u64*)calloc(cap, sizeof *t);
        if (!t) return 1;                                 /* no room: do not append */
        for (u32 i = 0; i < s_pl.cap; i++) {
            if (!s_pl.seen[i]) continue;
            u32 j = (u32)(s_pl.seen[i] ^ (s_pl.seen[i] >> 32)) & (cap - 1u);
            while (t[j]) j = (j + 1u) & (cap - 1u);
            t[j] = s_pl.seen[i];
        }
        free(s_pl.seen); s_pl.seen = t; s_pl.cap = cap;
    }
    u32 j = (u32)(k ^ (k >> 32)) & (s_pl.cap - 1u);
    while (s_pl.seen[j]) { if (s_pl.seen[j] == k) return 1; j = (j + 1u) & (s_pl.cap - 1u); }
    s_pl.seen[j] = k; s_pl.n++;
    return 0;
}

/* Render thread, after a variant was built. */
static void vk_plist_add(const vk_vdesc* d)
{
    if (!s_pl.f || s_pl.n >= 4u * VK_PLIST_MAX || vk_plist_seen(vk_vdesc_key(d))) return;
    if (fwrite(d, sizeof *d, 1, s_pl.f) != 1 || fflush(s_pl.f) != 0) {
        VK_LOG("pipeline list: write failed; no more variants recorded this run\n");
        fclose(s_pl.f); s_pl.f = NULL;
    }
}

typedef struct { u64 hash; u32 words; long off; } vk_pw_spv;   /* off: its shader-cache record, -1 none */

static int vk_pw_spv_cmp(const void* a, const void* b)
{
    const vk_pw_spv* x = (const vk_pw_spv*)a;
    const vk_pw_spv* y = (const vk_pw_spv*)b;
    if (x->hash != y->hash) return x->hash < y->hash ? -1 : 1;
    return x->words < y->words ? -1 : x->words > y->words;
}

static int vk_pw_u32_cmp(const void* a, const void* b)
{
    const u32 x = *(const u32*)a, y = *(const u32*)b;
    return x < y ? -1 : x > y;
}

static vk_pw_spv* vk_pw_find(vk_pw_spv* v, u32 n, u64 hash, u32 words)
{
    const vk_pw_spv k = { hash, words, -1 };
    return (vk_pw_spv*)bsearch(&k, v, n, sizeof *v, vk_pw_spv_cmp);
}

/* RSX_SHADER_CACHE's file, walked for the programs in `sp` (sorted; `wc` their
 * sorted sizes, a cheap first test: most records are skipped unread). Their
 * offsets go into sp[].off; returns the file, open, or NULL. */
static FILE* vk_pw_find_programs(vk_pw_spv* sp, u32 ns, const u32* wc, u32 nw, u32* buf, u32* found)
{
    /* The SPIR-V cache rewrites its file at its first use when the file is
     * torn or over its cap -- and on Windows cannot while another handle has
     * it open -- so the walk waits for that first translation. */
    for (;;) {
        unsigned long h = 0, m = 0;
        rsx_hlsl_to_spirv_cache_stats(&h, &m);
        if (h + m || SDL_AtomicGet(&s_pw.stop)) break;
        SDL_Delay(20);
    }
    const char* path = getenv("RSX_SHADER_CACHE");
    FILE* f = (path && *path) ? fopen(path, "rb") : NULL;
    char magic[8];
    if (!f || fread(magic, 1, 8, f) != 8 || memcmp(magic, "RSXSPV01", 8) != 0) {
        if (f) fclose(f);
        return NULL;
    }
    /* Records { u64 key[2], u32 words, u32 check, u32 spirv[words] }
     * (rsx_shader_spirv.cpp); a torn or growing tail ends the walk. */
    for (long off = 8; !SDL_AtomicGet(&s_pw.stop); ) {
        u32 h[6];
        if (fread(h, 4, 6, f) != 6 || h[5] != (h[4] ^ 0x5350565Au) || !h[4]) break;
        const u32 w = h[4];
        if (w <= VK_SPV_WORDS && bsearch(&w, wc, nw, sizeof *wc, vk_pw_u32_cmp)) {
            if (fread(buf, 4, w, f) != w) break;
            vk_pw_spv* s = vk_pw_find(sp, ns, vk_spv_hash(buf, w), w);
            if (s && s->off < 0) { s->off = off; (*found)++; }
        } else if (fseek(f, (long)w * 4L, SEEK_CUR) != 0) break;
        off += 24L + (long)w * 4L;
    }
    return f;
}

/* A module of one program's words, read back from the shader cache. */
static VkShaderModule vk_pw_module(FILE* f, const vk_pw_spv* s, u32* buf)
{
    if (fseek(f, s->off + 24L, SEEK_SET) != 0 || fread(buf, 4, s->words, f) != s->words ||
        vk_spv_hash(buf, s->words) != s->hash) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = (size_t)s->words * 4u, .pCode = buf };
    VkShaderModule m = VK_NULL_HANDLE;
    return pvkCreateShaderModule(s_vk.device, &ci, NULL, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

static int SDLCALL vk_prewarm_thread(void* arg)
{
    (void)arg;
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);
    const unsigned long long t0 = ps3_qpc_us();
    const vk_vdesc* recs = s_pw.recs;
    const u32 n = s_pw.n;
    u32 ns = 0, nw = 0, found = 0, built = 0, absent = 0, failed = 0;
    vk_pw_spv* sp = (vk_pw_spv*)malloc((size_t)n * 2u * sizeof *sp);
    u32* wc  = (u32*)malloc((size_t)n * 2u * sizeof *wc);
    u32* buf = (u32*)malloc(VK_SPV_WORDS * sizeof *buf);
    FILE* f = NULL;
    if (sp && wc && buf) {
        for (u32 i = 0; i < n; i++) {
            sp[ns++] = (vk_pw_spv){ recs[i].vs_hash, recs[i].vs_words, -1 };
            sp[ns++] = (vk_pw_spv){ recs[i].fs_hash, recs[i].fs_words, -1 };
        }
        qsort(sp, ns, sizeof *sp, vk_pw_spv_cmp);
        u32 k = 0;
        for (u32 i = 0; i < ns; i++) if (!k || vk_pw_spv_cmp(&sp[i], &sp[k - 1])) sp[k++] = sp[i];
        ns = k;
        for (u32 i = 0; i < ns; i++) wc[i] = sp[i].words;
        qsort(wc, ns, sizeof *wc, vk_pw_u32_cmp);
        for (u32 i = 0; i < ns; i++) if (!nw || wc[i] != wc[nw - 1]) wc[nw++] = wc[i];
        f = vk_pw_find_programs(sp, ns, wc, nw, buf, &found);
        if (!f) VK_LOG("pipeline pre-warm: no shader cache file to read the programs from\n");
    }
    if (f) {
        VK_LOG("pipeline pre-warm: %u of the %u programs found in the shader cache (%.1f s)\n",
               found, ns, (ps3_qpc_us() - t0) / 1e6);
        /* In list order (first seen first); a module carries over while
         * consecutive records share it. */
        VkRenderPass rp[2][RSX_BE_MAX_COLOR_TARGETS + 1];
        memset(rp, 0, sizeof rp);
        VkShaderModule vm = VK_NULL_HANDLE, fm = VK_NULL_HANDLE;
        const vk_pw_spv* vcur = NULL;
        const vk_pw_spv* fcur = NULL;
        unsigned long long tlog = ps3_qpc_us();
        for (u32 i = 0; i < n && !SDL_AtomicGet(&s_pw.stop); i++) {
            const vk_vdesc* d = &recs[i];
            const vk_pw_spv* vs = vk_pw_find(sp, ns, d->vs_hash, d->vs_words);
            const vk_pw_spv* fs = vk_pw_find(sp, ns, d->fs_hash, d->fs_words);
            if (!vs || vs->off < 0 || !fs || fs->off < 0) { absent++; continue; }
            if (vs != vcur) {
                if (vm) pvkDestroyShaderModule(s_vk.device, vm, NULL);
                vm = vk_pw_module(f, vs, buf); vcur = vs;
            }
            if (fs != fcur) {
                if (fm) pvkDestroyShaderModule(s_vk.device, fm, NULL);
                fm = vk_pw_module(f, fs, buf); fcur = fs;
            }
            VkRenderPass* r = &rp[d->rt_fi][d->rt_count];
            if (!*r) *r = vk_eng_make_render_pass(d->rt_count, d->rt_fi, 0);
            VkPipeline p = VK_NULL_HANDLE;
            if (!vm || !fm || !*r || vk_vdesc_build(d, vm, fm, *r, &p, 0, s_g.pcache) != VK_SUCCESS) { failed++; continue; }
            pvkDestroyPipeline(s_vk.device, p, NULL);
            if ((++built & 63u) == 0) SDL_AtomicSet(&s_pw_dirty, 1);
            const unsigned long long now = ps3_qpc_us();
            if (now - tlog >= 5000000ull) {
                tlog = now;
                VK_LOG("pipeline pre-warm: %u of %u built (%.1f s)\n", built, n, (now - t0) / 1e6);
            }
        }
        if (vm) pvkDestroyShaderModule(s_vk.device, vm, NULL);
        if (fm) pvkDestroyShaderModule(s_vk.device, fm, NULL);
        for (u32 fi = 0; fi < 2; fi++)
            for (u32 c = 0; c <= RSX_BE_MAX_COLOR_TARGETS; c++)
                if (rp[fi][c]) pvkDestroyRenderPass(s_vk.device, rp[fi][c], NULL);
        fclose(f);
    }
    VK_LOG("pipeline pre-warm: %s, %u of %u variants built in %.1f s (%u without their programs, %u failed)\n",
           SDL_AtomicGet(&s_pw.stop) ? "stopped" : "done", built, n, (ps3_qpc_us() - t0) / 1e6, absent, failed);
    if (built) SDL_AtomicSet(&s_pw_dirty, 1);
    free(sp); free(wc); free(buf);
    free(s_pw.recs); s_pw.recs = NULL;
    s_pw_running = 0;
    return 0;
}

/* vk_pcache_init: load the list (dropping a torn tail, duplicates, and all
 * but the newest half when it is over VK_PLIST_MAX), keep it open for
 * appending, and start the pre-warm when the cache needs rebuilding. */
static void vk_plist_open(int rebuild)
{
    const char* path = getenv("RSX_PIPELINE_LIST");
    if (!path || !*path) return;
    vk_vdesc* recs = NULL;
    u32 n = 0;
    int rewrite = 1;                                         /* no usable file: start one */
    u32 upgraded = 0;
    FILE* f = fopen(path, "rb");
    if (f) {
        char magic[8]; u32 hdr[2];
        fseek(f, 0, SEEK_END); const long len = ftell(f); fseek(f, 0, SEEK_SET);
        if (len >= 16 && fread(magic, 1, 8, f) == 8 && fread(hdr, 4, 2, f) == 2 &&
            !memcmp(magic, kPlistMagic, 8) && hdr[0] == (u32)sizeof(vk_vdesc)) {
            const u32 total = (u32)((len - 16) / (long)sizeof(vk_vdesc));
            rewrite = (len - 16) % (long)sizeof(vk_vdesc) != 0 || hdr[1] == 0u;   /* a torn tail, an old list */
            recs = (vk_vdesc*)malloc((size_t)(total ? total : 1u) * sizeof *recs);
            u32 i = 0;
            for (; recs && i < total && fread(&recs[n], sizeof *recs, 1, f) == 1; i++) {
                vk_vdesc* r = &recs[n];
                if (hdr[1] == 0u && vk_vdesc_ok(r) && r->blend && r->blend_f[0] == VK_BLEND_FACTOR_ONE && r->blend_f[1] == VK_BLEND_FACTOR_ZERO &&
                    r->blend_f[3] == VK_BLEND_FACTOR_ONE && r->blend_f[4] == VK_BLEND_FACTOR_ZERO) {
                    r->blend_f[3] = VK_BLEND_FACTOR_CONSTANT_ALPHA;
                    r->check = vk_fnv1a32((const u8*)r, (u32)offsetof(vk_vdesc, check));
                    upgraded++;
                }
                if (!vk_vdesc_ok(&recs[n]) || vk_plist_seen(vk_vdesc_key(&recs[n]))) rewrite = 1;
                else n++;
            }
            if (i != total) rewrite = 1;
        }
        fclose(f);
    }
    if (n > VK_PLIST_MAX) {                                  /* keep the newest half */
        const u32 drop = n - VK_PLIST_MAX / 2u;
        memmove(recs, recs + drop, (size_t)(n - drop) * sizeof *recs);
        n -= drop;
        free(s_pl.seen); s_pl.seen = NULL; s_pl.cap = s_pl.n = 0;
        for (u32 i = 0; i < n; i++) vk_plist_seen(vk_vdesc_key(&recs[i]));
        rewrite = 1;
    }
    if (rewrite) {                                           /* through <file>.tmp */
        char tmp[1100];
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        FILE* o = fopen(tmp, "wb");
        int ok = o != NULL;
        if (o) {
            const u32 hdr[2] = { (u32)sizeof(vk_vdesc), 1u };
            ok = fwrite(kPlistMagic, 1, 8, o) == 8 && fwrite(hdr, 4, 2, o) == 2 &&
                 (n == 0 || fwrite(recs, sizeof *recs, n, o) == n);
            ok = fclose(o) == 0 && ok;
        }
        remove(path);
        if (!ok || rename(tmp, path) != 0) {
            remove(tmp);
            VK_LOG("pipeline list: cannot write %s; variants are not recorded\n", path);
            free(recs);
            return;
        }
    }
    s_pl.f = fopen(path, "ab");
    VK_LOG("pipeline list: %s, %u variants%s\n", path, n, s_pl.f ? "" : " (cannot append)");
    if (upgraded) { VK_LOG("pipeline list: %u variants take the blend constant now%c", upgraded, 10); rebuild = 1; }
    const char* pw = getenv("RSX_PIPELINE_PREWARM");
    if (pw && *pw) rebuild = pw[0] != '0';
    if (rebuild && n) {
        s_pw.recs = recs; s_pw.n = n;
        SDL_AtomicSet(&s_pw.stop, 0);
        s_pw_running = 1;
        s_pw.thread = SDL_CreateThread(vk_prewarm_thread, "pipeline-prewarm", NULL);
        if (s_pw.thread) {
            VK_LOG("pipeline pre-warm: rebuilding the %u listed variants in the background\n", n);
            return;
        }
        s_pw.recs = NULL; s_pw.n = 0; s_pw_running = 0;
    } else if (rebuild) {
        VK_LOG("pipeline pre-warm: nothing listed yet; expect shader-compile stutter the first time each area is seen\n");
    }
    free(recs);
}

/* vk_guest_shutdown: the pre-warm stops after the pipeline it is building. */
static void vk_plist_close(void)
{
    if (s_pw.thread) {
        SDL_AtomicSet(&s_pw.stop, 1);
        SDL_WaitThread(s_pw.thread, NULL);
        s_pw.thread = NULL;
    }
    if (s_pl.f) fclose(s_pl.f);
    free(s_pl.seen);
    memset(&s_pl, 0, sizeof s_pl);
}

static const VkPrimitiveTopology k_vk_topo[VK_ENG_TOPOLOGIES] = {
    VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
    VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };

/* ---- New pipelines, compiled off the render thread -------------------------
 * A variant is first asked for with FAIL_ON_PIPELINE_COMPILE_REQUIRED
 * (VK_EXT_pipeline_creation_cache_control): one the pipeline cache already
 * has is made at once, as before. One it lacks needs the driver's compiler
 * -- a single new effect or material froze the game 140-210 ms on the
 * owner's RTX 3070 Ti (v66 log) -- so it goes to this thread, and the draws
 * that need it are skipped until it is ready (that one object or effect
 * missing for a few frames instead of everything stopping). Pipelines made
 * here land in pipelinelist.bin and the pipeline cache like any other, so the
 * next run has them. RACACIT_ASYNC_PIPELINES=0: compile on the render
 * thread, as before; also when the queue is full. */

static int SDLCALL vk_ap_thread(void* arg)
{
    (void)arg;
    for (;;) {
        SDL_LockMutex(s_ap.mu);
        while (s_ap.head == s_ap.tail && !s_ap.stop) SDL_CondWait(s_ap.cv, s_ap.mu);
        if (s_ap.stop) { SDL_UnlockMutex(s_ap.mu); break; }
        const u32 pi = s_ap.q[s_ap.head % VK_AP_QUEUE].pipe, t = s_ap.q[s_ap.head % VK_AP_QUEUE].topo;
        const VkRenderPass rp = s_ap.q[s_ap.head % VK_AP_QUEUE].rp;
        s_ap.head++;
        SDL_UnlockMutex(s_ap.mu);
        vk_eng_pipe* P = &s_epipe[pi];
        vk_vdesc d;
        vk_vdesc_from(&d, P, k_vk_topo[t]);
        const unsigned long long t0 = ps3_qpc_us();
        VkPipeline out = VK_NULL_HANDLE;
        if (vk_vdesc_build(&d, P->vs, P->fs, rp, &out, 0, s_ap.cache) != VK_SUCCESS) out = VK_NULL_HANDLE;
        P->build_ms[t] = (u32)((ps3_qpc_us() - t0) / 1000u);
        P->ready[t] = out;
        SDL_MemoryBarrierRelease();
        SDL_AtomicSet(&P->pend[t], 2);
        SDL_AtomicAdd(&s_ap.active, -1);
    }
    return 0;
}

/* Queue a background build (render thread). 0 when it cannot. */
static int vk_ap_queue(u32 pipe_index, u32 t, VkRenderPass rp)
{
    if (s_ap.off) return 0;
    if (!s_ap.th) {
        static int env = -1;
        if (env < 0) { const char* e = getenv("RACACIT_ASYNC_PIPELINES"); env = !(e && *e == '0'); }
        if (!env) { s_ap.off = 1; return 0; }
        s_ap.mu = SDL_CreateMutex(); s_ap.cv = SDL_CreateCond();
        VkPipelineCacheCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
        if (pvkCreatePipelineCache(s_vk.device, &ci, NULL, &s_ap.cache) != VK_SUCCESS) s_ap.cache = VK_NULL_HANDLE;
        if (s_ap.mu && s_ap.cv && s_ap.cache) s_ap.th = SDL_CreateThread(vk_ap_thread, "pipeline-build", NULL);
        if (!s_ap.th) { s_ap.off = 1; return 0; }
    }
    SDL_LockMutex(s_ap.mu);
    const int ok = s_ap.tail - s_ap.head < VK_AP_QUEUE;
    if (ok) { SDL_AtomicAdd(&s_ap.active, 1);
              s_ap.q[s_ap.tail % VK_AP_QUEUE].pipe = pipe_index; s_ap.q[s_ap.tail % VK_AP_QUEUE].topo = t;
              s_ap.q[s_ap.tail % VK_AP_QUEUE].rp = rp; s_ap.tail++; SDL_CondSignal(s_ap.cv); }
    SDL_UnlockMutex(s_ap.mu);
    return ok;
}

/* vk_eng2_shutdown: the builder finishes the pipeline it is on and stops. */
static void vk_ap_stop(void)
{
    if (!s_ap.th) return;
    SDL_LockMutex(s_ap.mu); s_ap.stop = 1; SDL_CondSignal(s_ap.cv); SDL_UnlockMutex(s_ap.mu);
    SDL_WaitThread(s_ap.th, NULL);
    s_ap.th = NULL; s_ap.off = 1;
}

static VkPipeline vk_eng_variant(vk_eng_pipe* P, u32 topo_idx)
{
    if (P->variant[topo_idx]) return P->variant[topo_idx];
    const int pend = SDL_AtomicGet(&P->pend[topo_idx]);
    if (pend == 1) { P->skipped[topo_idx]++; return VK_NULL_HANDLE; }   /* still compiling */
    if (pend == 2) {                                                    /* the builder is done */
        SDL_MemoryBarrierAcquire();
        P->variant[topo_idx] = P->ready[topo_idx]; P->ready[topo_idx] = VK_NULL_HANDLE;
        SDL_AtomicSet(&P->pend[topo_idx], 0);
        s_ap.built++;
        if (P->variant[topo_idx]) {
            vk_vdesc d; vk_vdesc_from(&d, P, k_vk_topo[topo_idx]);
            vk_plist_add(&d);
            s_g.pcache_dirty++;
        } else VK_LOG("engine: vkCreateGraphicsPipelines failed (background)\n");
        if (s_ap.logged < 40 || (s_ap.built & 63u) == 0) { s_ap.logged++;
            VK_LOG("new pipeline compiled in the background in %u ms, %u draws skipped meanwhile (%u so far)\n",
                   P->build_ms[topo_idx], P->skipped[topo_idx], s_ap.built); }
        return P->variant[topo_idx];
    }
    VkRenderPass rp = vk_eng_render_pass(P->rt_count, P->rt_fi);
    if (!rp || P->nslots > VK_GUEST_ATTRS) return VK_NULL_HANDLE;
    vk_vdesc d;
    vk_vdesc_from(&d, P, k_vk_topo[topo_idx]);
    const unsigned long long _t0 = ps3_qpc_us();
    /* While the builder compiles, the render thread makes no pipeline at all:
     * the driver runs one creation at a time, and even the quick cache check
     * below waited out a whole background compile (second test: a 166 ms
     * stall behind a 164 ms compile). The new one joins the queue; a cached
     * one is made there within a frame or two. */
    if (s_vk_pcc && s_ap.th && SDL_AtomicGet(&s_ap.active) > 0 && vk_ap_queue((u32)(P - s_epipe), topo_idx, rp)) {
        P->skipped[topo_idx] = 1; P->build_ms[topo_idx] = 0;
        SDL_AtomicSet(&P->pend[topo_idx], 1);
        return VK_NULL_HANDLE;
    }
    if (s_vk_pcc && !s_ap.off) {
        /* In the pipeline cache: made now. Not: compiled in the background. */
        const VkResult rc = vk_vdesc_build(&d, P->vs, P->fs, rp, &P->variant[topo_idx],
                                           VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_EXT, s_g.pcache);
        if (rc == VK_SUCCESS && P->variant[topo_idx]) {
            s_vkt_var++; s_vkt_var_us += ps3_qpc_us() - _t0;
            vk_plist_add(&d);
            return P->variant[topo_idx];
        }
        P->variant[topo_idx] = VK_NULL_HANDLE;
        if (rc == VK_PIPELINE_COMPILE_REQUIRED_EXT && vk_ap_queue((u32)(P - s_epipe), topo_idx, rp)) {
            P->skipped[topo_idx] = 1; P->build_ms[topo_idx] = 0;
            SDL_AtomicSet(&P->pend[topo_idx], 1);
            return VK_NULL_HANDLE;
        }
    }
    const VkResult r = vk_vdesc_build(&d, P->vs, P->fs, rp, &P->variant[topo_idx], 0, s_g.pcache);
    s_vkt_var++; s_vkt_var_us += ps3_qpc_us() - _t0;
    s_g.pcache_dirty++;
    if (r != VK_SUCCESS) {
        VK_LOG("engine: vkCreateGraphicsPipelines failed\n");
        P->variant[topo_idx] = VK_NULL_HANDLE;
    } else {
        vk_plist_add(&d);
    }
    return P->variant[topo_idx];
}

static void vk_eng_bind_pipeline(void* user, u32 pipeline) { (void)user; s_e2.pipeline = pipeline; }

static void vk_eng2_shutdown(void* user)
{
    vk_ap_stop();
    for (u32 i = 1; i <= VK_ENG_MAX_PIPE; i++) vk_eng_pipeline_release(user, i);
    vk_eng_drop_framebuffers();
    for (u32 fi = 0; fi < 2; fi++)
        for (u32 n = 0; n <= RSX_BE_MAX_COLOR_TARGETS; n++)
            if (s_e2.rp[fi][n]) pvkDestroyRenderPass(s_vk.device, s_e2.rp[fi][n], NULL);
    for (u32 fi = 0; fi < 2; fi++)
        if (s_e2.rp_zonly[fi]) pvkDestroyRenderPass(s_vk.device, s_e2.rp_zonly[fi], NULL);
    vk_destroy_image(&s_e2.scratch_depth);
    vk_destroy_buffer(&s_e2.indices);
    memset(&s_e2, 0, sizeof s_e2);
    for (u32 i = 0; i < s_esamp_count; i++) pvkDestroySampler(s_vk.device, s_esamp[i].s, NULL);
    s_esamp_count = 0;
    memset(s_esamp_hash, 0, sizeof s_esamp_hash);
}

/* Constants go straight into the shared UBO: vertex at 0, pixel at ps_off.
 * Every operation is waited on, so overwriting per draw is safe. */
static void vk_eng_bind_vs_constants(void* user, const void* data, u32 bytes)
{
    (void)user;
    if (!s_g.on || !data) { s_e2.vs_bytes = 0; return; }
    const u32 cap = (u32)s_g.ps_off;
    if (bytes > cap) { bytes = cap; if (!(s_e2.warned & 2u)) { VK_LOG("engine: VS constants truncated\n"); s_e2.warned |= 2u; } }
    if (s_bt.ready) {
        s_e2.vs_src = (const u8*)data;
    } else memcpy(s_g.ubo.ptr, data, bytes);
    s_e2.vs_bytes = bytes;
    s_e2.vs_dirty = 1;
}
static void vk_eng_bind_ps_constants(void* user, const void* data, u32 bytes)
{
    (void)user;
    if (!s_g.on || !data) { s_e2.ps_bytes = 0; return; }
    const u32 cap = (u32)(s_g.ubo.size - s_g.ps_off);
    if (bytes > cap) { bytes = cap; if (!(s_e2.warned & 4u)) { VK_LOG("engine: PS constants truncated\n"); s_e2.warned |= 4u; } }
    if (s_bt.ready) {
        if (!s_e2.ps_shadow) s_e2.ps_shadow = (u8*)malloc(cap ? cap : 16);
        if (s_e2.ps_shadow) {
            /* Small: compare first, so an unchanged block keeps its copy. */
            if (bytes != s_e2.ps_bytes || memcmp(s_e2.ps_shadow, data, bytes)) {
                memcpy(s_e2.ps_shadow, data, bytes);
                s_e2.ps_dirty = 1;
            }
        }
    } else memcpy((u8*)s_g.ubo.ptr + s_g.ps_off, data, bytes);
    s_e2.ps_bytes = bytes;
}
static void vk_eng_bind_textures(void* user, const u32* textures,
                                 const rsx_be_sampler_desc* samplers, u32 mask)
{
    (void)user;
    if (mask != s_e2.tex_mask) s_e2.tex_dirty = 1;
    for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
        const int on = (mask >> u) & 1u;
        const u32 t = on ? textures[u] : 0;
        if (t && t < VK_ENG_MAX_OBJ && s_eobj[t].lazy) (void)vk_eng_color_back(&s_eobj[t]);
        if (t != s_e2.tex[u]) { s_e2.tex[u] = t; s_e2.tex_dirty = 1; }
        if (on && samplers && memcmp(&s_e2.samp[u], &samplers[u], sizeof samplers[u])) {
            s_e2.samp[u] = samplers[u]; s_e2.tex_dirty = 1;
        }
    }
    s_e2.tex_mask = mask;
}
static void vk_eng_bind_vertex_textures(void* user, const u32* textures,
                                        const rsx_be_sampler_desc* samplers, u32 mask)
{
    (void)user; (void)textures; (void)samplers;
    if (mask && !(s_e2.warned & 8u)) { VK_LOG("engine: vertex textures not supported yet\n"); s_e2.warned |= 8u; }
}
static void vk_eng_set_viewport(void* user, float x, float y, float w, float h)
{
    (void)user;
    s_e2.vp[0] = x; s_e2.vp[1] = y; s_e2.vp[2] = w; s_e2.vp[3] = h; s_e2.have_vp = 1;
}
static void vk_eng_set_scissor(void* user, u32 x, u32 y, u32 w, u32 h)
{
    (void)user;
    s_e2.sc[0] = x; s_e2.sc[1] = y; s_e2.sc[2] = w; s_e2.sc[3] = h; s_e2.have_sc = 1;
}
static void vk_eng_set_stencil_ref(void* user, u32 ref) { (void)user; s_e2.stencil_ref = ref; }
static void vk_eng_set_blend_color(void* user, const float* rgba) { (void)user; memcpy(s_e2.blend_c, rgba, sizeof s_e2.blend_c); }
static void vk_eng_set_depth_bias(void* user, float units, float factor)
{
    (void)user; s_e2.dbias_units = units; s_e2.dbias_factor = factor;
}

static VkFramebuffer vk_eng_framebuffer(VkRenderPass rp, const u32* rt, u32 n, VkImageView dview,
                                        u32 dkey, u32 w, u32 h)
{
    u32 key[RSX_BE_MAX_COLOR_TARGETS + 1] = { 0 };
    for (u32 i = 0; i < n; i++) key[i] = rt[i];
    key[RSX_BE_MAX_COLOR_TARGETS] = dkey;
    for (u32 i = 0; i < s_e2.fb_count; i++)
        if (!memcmp(s_e2.fb[i].key, key, sizeof key)) return s_e2.fb[i].fb;
    if (s_e2.fb_count >= VK_ENG_FB_CACHE) vk_eng_drop_framebuffers();
    VkImageView views[RSX_BE_MAX_COLOR_TARGETS + 1];
    for (u32 i = 0; i < n; i++) views[i] = s_eobj[rt[i]].im.view;
    views[n] = dview;
    VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                    .renderPass = rp, .attachmentCount = n + 1, .pAttachments = views,
                                    .width = w, .height = h, .layers = 1 };
    VkFramebuffer fb = VK_NULL_HANDLE;
    if ((s_vkc[2]++, pvkCreateFramebuffer(s_vk.device, &fci, NULL, &fb)) != VK_SUCCESS) return VK_NULL_HANDLE;
    memcpy(s_e2.fb[s_e2.fb_count].key, key, sizeof key);
    s_e2.fb[s_e2.fb_count++].fb = fb;
    return fb;
}

/* Depth when the draw names no zeta: one scratch image, grown as needed and
 * cleared to the far plane when (re)created. Key 0xFFFFFFFF in the cache. */
static VkImageView vk_eng_scratch_depth(u32 w, u32 h)
{
    if (s_e2.scratch_depth.view && s_e2.scratch_w >= w && s_e2.scratch_h >= h)
        return s_e2.scratch_depth.view;
    vk_eng_drop_framebuffers();
    vk_destroy_image(&s_e2.scratch_depth);
    if (vk_create_image(&s_e2.scratch_depth, s_vk.eng_depth_format, w, h,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        s_vk.eng_depth_aspect) || vk_begin())
        return VK_NULL_HANDLE;
    vk_barrier_image(s_e2.scratch_depth.img, s_vk.eng_depth_aspect, VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearDepthStencilValue far_plane = { 1.0f, 0 };
    VkImageSubresourceRange r = { s_vk.eng_depth_aspect, 0, 1, 0, 1 };
    pvkCmdClearDepthStencilImage(s_vk.cmd, s_e2.scratch_depth.img, VK_IMAGE_LAYOUT_GENERAL, &far_plane, 1, &r);
    if (vk_submit_and_wait()) return VK_NULL_HANDLE;
    s_e2.scratch_w = w; s_e2.scratch_h = h;
    return s_e2.scratch_depth.view;
}

/* The colour image a depth-only pass into a shadow map draws with: the pass
 * writes no colour, but every render pass here has a colour attachment, and
 * the pass's own colour target is at the scene's scale, not the map's. One per
 * colour format, grown to the largest map; made like a colour target, so the
 * recycle bin can hand its image to one. */
static u32 vk_eng_zonly_color(u32 fi, VkFormat fmt, u32 w, u32 h)
{
    vk_eng_obj* o = vk_eng_get(s_e2.zrt[fi], VK_ENG_COLOR);
    if (o && o->fmt == fmt && o->w >= w && o->h >= h) return s_e2.zrt[fi];
    if (o) {
        if (o->w > w) w = o->w;
        if (o->h > h) h = o->h;
        vk_eng_release(NULL, s_e2.zrt[fi]);
    }
    s_e2.zrt[fi] = 0;
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    o = &s_eobj[hd];
    if (!vk_rbin_take(VK_ENG_COLOR, fmt, w, h, &o->im) &&
        vk_create_image(&o->im, fmt, w, h,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT)) {
        vk_destroy_image(&o->im); memset(o, 0, sizeof *o); return 0;
    }
    o->kind = VK_ENG_COLOR; o->fmt = fmt; o->bpp = fmt == VK_FORMAT_R16G16B16A16_SFLOAT ? 8u : 4u;
    o->w = o->lw = w; o->h = o->lh = h;
    if (vk_begin()) { vk_eng_release(NULL, hd); return 0; }
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    s_e2.zrt[fi] = hd;
    return hd;
}

/* A shadow-scale depth target that a pass also draws colour into (or reads
 * WPOS in, or binds with several colour targets) cannot be drawn at its own
 * scale: it goes back to the scene's for good, its depth stretched down onto
 * a new image. Its stencil restarts at 0 (the shadow passes leave it alone). */
static int vk_eng_depth_demote(u32 handle, u32 why_mask, u32 why_n, u32 why_wpos)
{
    vk_eng_obj* z = vk_eng_get(handle, VK_ENG_DEPTH);
    if (!z || !(z->sc > 0.0f)) return 0;
    const u32 pw = vk_rs(z->lw), ph = vk_rs(z->lh);
    vk_eng_obj nd = *z;
    memset(&nd.im, 0, sizeof nd.im);
    if (!vk_rbin_take(VK_ENG_DEPTH, z->fmt, pw, ph, &nd.im) &&
        vk_create_image(&nd.im, z->fmt, pw, ph,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        s_vk.eng_depth_aspect)) {
        vk_destroy_image(&nd.im); return -1;
    }
    nd.w = pw; nd.h = ph; nd.sc = 0.0f;
    if (vk_begin()) { vk_destroy_image(&nd.im); return -1; }
    vk_barrier_image(nd.im.img, s_vk.eng_depth_aspect, VK_IMAGE_LAYOUT_UNDEFINED);
    { const VkClearDepthStencilValue far_plane = { 1.0f, 0 };
      const VkImageSubresourceRange r = { s_vk.eng_depth_aspect, 0, 1, 0, 1 };
      pvkCmdClearDepthStencilImage(s_vk.cmd, nd.im.img, VK_IMAGE_LAYOUT_GENERAL, &far_plane, 1, &r); }
    vk_full_barrier();
    int ok;
    if (vk_eng_depth_blit_ok()) {
        vk_barrier_image(z->im.img, s_vk.eng_depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
        const VkImageBlit b = {
            .srcSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
            .srcOffsets = { { 0, 0, 0 }, { (int32_t)z->w, (int32_t)z->h, 1 } },
            .dstSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
            .dstOffsets = { { 0, 0, 0 }, { (int32_t)pw, (int32_t)ph, 1 } } };
        pvkCmdBlitImage(s_vk.cmd, z->im.img, VK_IMAGE_LAYOUT_GENERAL, nd.im.img, VK_IMAGE_LAYOUT_GENERAL,
                        1, &b, VK_FILTER_NEAREST);
        ok = !vk_defer();
    } else {
        ok = vk_eng_blit_depth_staged(z, 0, 0, z->w, z->h, &nd, 0, 0, pw, ph);
    }
    if (!ok) { vk_destroy_image(&nd.im); return -1; }
    vk_eng_drop_framebuffers_using(handle);
    if (!vk_rbin_park(VK_ENG_DEPTH, z->fmt, z->w, z->h, 8u, &z->im)) vk_destroy_image(&z->im);
    if (s_zhi_demoted++ < 8)
        VK_LOG("engine: shadow-scale depth %ux%u (x%.2f) back to the scene's scale: drawn with colour mask %08X, %u targets%s%c",
               z->lw, z->lh, (double)z->sc, why_mask, why_n, why_wpos ? ", WPOS read" : "", 10);
    z->im = nd.im; z->w = pw; z->h = ph; z->sc = 0.0f;
    return 0;
}

static int vk_eng_ensure_indices(VkDeviceSize bytes)
{
    if (s_e2.indices.buf && s_e2.indices.size >= bytes) return 0;
    VkDeviceSize size = s_e2.indices.size ? s_e2.indices.size : 64 * 1024;
    while (size < bytes) size *= 2;
    vk_destroy_buffer(&s_e2.indices);
    if (vk_create_host_buffer(&s_e2.indices, size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) return -1;
    return s_e2.indices.coherent ? 0 : -1;
}

/* rsx_be_sampler_desc is the registers decoded the D3D12 way; the wrap codes
 * stay the guest's 1..8, which vk_gcm_wrap already translates. */
static VkSampler vk_eng_sampler(const rsx_be_sampler_desc* d)
{
    u32 lo, hi;
    memcpy(&lo, &d->min_lod, 4); memcpy(&hi, &d->max_lod, 4);
    const u64 key = (u64)d->min_linear | ((u64)d->mag_linear << 1) | ((u64)d->mip_linear << 2)
                  | ((u64)d->mip_present << 3) | ((u64)(d->wrap_s & 0xF) << 4)
                  | ((u64)(d->wrap_t & 0xF) << 8) | ((u64)(d->wrap_r & 0xF) << 12)
                  | ((u64)(lo >> 16) << 16) | ((u64)(hi >> 16) << 32);
    /* The unit's border colour, when core Vulkan has it: a shadow map read
     * past its edge must see the colour the title set there (A Crack in
     * Time: white, the far plane). Transparent black made every lookup
     * outside Ratchet's shadow map a shadow -- a solid slab. Any other colour
     * (bc 3) cannot be had without VK_EXT_custom_border_color: the texture's
     * own edge stands in for it. Snapping it to white tinted the edges of
     * every Bink movie magenta -- their colour planes' border is a neutral
     * grey -- a 1-pixel frame round the picture, wider at higher resolutions
     * (owner, v83). ARGB in the register. */
    const u32 br = (d->border >> 16) & 0xFFu, bg = (d->border >> 8) & 0xFFu, bb = d->border & 0xFFu, ba = d->border >> 24;
    const int b_white = br >= 0xF0u && bg >= 0xF0u && bb >= 0xF0u, b_black = br <= 0x0Fu && bg <= 0x0Fu && bb <= 0x0Fu;
    const u32 bc = b_white ? 2u : b_black ? (ba >= 0x80u ? 1u : 0u) : 3u;   /* white whatever its alpha, as before */
    const VkBorderColor border = bc == 2u ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                               : bc == 1u ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
                                          : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    const u64 key2 = key ^ ((u64)bc << 48);
    u32 h = vk_esamp_slot(key2);
    for (; s_esamp_hash[h]; h = (h + 1u) & (VK_ENG_SAMP_HASH - 1u))
        if (s_esamp[s_esamp_hash[h] - 1u].key == key2) return s_esamp[s_esamp_hash[h] - 1u].s;
    if (s_esamp_count >= VK_ENG_SAMP_CACHE) {
        /* Full (never seen): start over -- the samplers are destroyed once
         * the batches that may use them are done, as for a filter change. */
        VK_LOG("sampler cache full (%u states): rebuilt%c", s_esamp_count, 10);
        for (u32 i = 0; i < s_esamp_count; i++)
            if (!vk_bt_push(6, (uint64_t)(uintptr_t)s_esamp[i].s, 0, 0))
                pvkDestroySampler(s_vk.device, s_esamp[i].s, NULL);
        s_esamp_count = 0;
        memset(s_esamp_hash, 0, sizeof s_esamp_hash);
        s_e2.tex_dirty = 1;
        h = vk_esamp_slot(key2);
    }
    float min_lod = d->min_lod, max_lod = d->mip_present ? d->max_lod : d->min_lod;
    if (max_lod < min_lod) max_lod = min_lod;
    /* Quality options apply to filtered, mipmapped textures only -- the
     * world's surfaces. Render targets, look-up tables and point-sampled data
     * are read exactly as the title asked. */
    const int mipped = d->min_linear && d->mip_present && max_lod > min_lod;
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = d->mag_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = d->min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = (d->mip_linear || (mipped && s_vk.trilinear)) ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                                    : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = vk_gcm_wrap_bc(d->wrap_s, bc), .addressModeV = vk_gcm_wrap_bc(d->wrap_t, bc),
        .addressModeW = vk_gcm_wrap_bc(d->wrap_r, bc),
        .anisotropyEnable = (mipped && s_vk.aniso > 1.0f) ? VK_TRUE : VK_FALSE,
        .maxAnisotropy = (mipped && s_vk.aniso > 1.0f) ? s_vk.aniso : 1.0f,
        .minLod = min_lod, .maxLod = max_lod,
        .borderColor = border };
    VkSampler smp;
    if ((s_vkc[3]++, pvkCreateSampler(s_vk.device, &sci, NULL, &smp)) != VK_SUCCESS) return s_vk.sampler;
    s_esamp[s_esamp_count].key = key2;
    s_esamp[s_esamp_count++].s = smp;
    s_esamp_hash[h] = (u16)s_esamp_count;
    { static u32 said = 32;   /* how many states the title uses: 64, 128, ... */
      if (s_esamp_count >= said * 2u) { said *= 2u; VK_LOG("sampler states: %u distinct%c", s_esamp_count, 10); } }
    return smp;
}

/* The view a bound engine handle samples through: an uploaded texture, a
 * colour target (bound directly when surface_view was not asked), or a view. */
static VkImageView vk_eng_sample_view(u32 handle)
{
    if (!handle || handle >= VK_ENG_MAX_OBJ) return VK_NULL_HANDLE;
    const vk_eng_obj* o = &s_eobj[handle];
    return (o->kind == VK_ENG_TEXTURE || o->kind == VK_ENG_COLOR || o->kind == VK_ENG_VIEW)
               ? o->im.view : VK_NULL_HANDLE;
}

static void vk_eng_write_descriptors_to(VkDescriptorSet set, const VkDescriptorBufferInfo bi[2]);
static void vk_eng_write_descriptors(void)
{
    VkDescriptorBufferInfo bi[2] = {
        { s_g.ubo.buf, 0, s_e2.vs_bytes ? s_e2.vs_bytes : 16u },
        { s_g.ubo.buf, s_g.ps_off, s_e2.ps_bytes ? s_e2.ps_bytes : 16u },
    };
    vk_eng_write_descriptors_to(s_g.set, bi);
}
static void vk_eng_write_descriptors_to(VkDescriptorSet set, const VkDescriptorBufferInfo bi[2])
{
    VkDescriptorImageInfo ii[RSX_MAX_TEXTURES], si[RSX_MAX_TEXTURES], ci[RSX_MAX_TEXTURES];
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        VkImageView v = ((s_e2.tex_mask >> u) & 1u) ? vk_eng_sample_view(s_e2.tex[u]) : VK_NULL_HANDLE;
        const int is_cube = v && s_eobj[s_e2.tex[u]].kind == VK_ENG_TEXTURE && s_eobj[s_e2.tex[u]].cube;
        const u32 nsamp0 = s_esamp_count;
        VkSampler smp = v ? vk_eng_sampler(&s_e2.samp[u]) : s_vk.sampler;
        { static int sl = -1; if (sl < 0) { const char* e = getenv("RSX_VK_SAMPLER_LOG"); sl = e && *e == '1'; }
          if (sl && s_esamp_count > nsamp0) {   /* a new sampler state: what it samples */
              const rsx_be_sampler_desc* d = &s_e2.samp[u]; const vk_eng_obj* o = &s_eobj[s_e2.tex[u]];
              VK_LOG("sampler state #%u: unit %u min %u mag %u mip %u/%u wrap %u,%u,%u lod %.2f-%.2f border %08X | %s %ux%u fmt %d rsx_fmt 0x%X%c",
                     s_esamp_count, u, d->min_linear, d->mag_linear, d->mip_present, d->mip_linear, d->wrap_s, d->wrap_t, d->wrap_r,
                     (double)d->min_lod, (double)d->max_lod, d->border,
                     o->kind == VK_ENG_TEXTURE ? "texture" : o->kind == VK_ENG_COLOR ? "target" : o->kind == VK_ENG_VIEW ? "view" : "other",
                     o->lw ? o->lw : o->w, o->lh ? o->lh : o->h, (int)o->fmt, o->rsx_fmt, 10); } }
        ii[u] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, (v && !is_cube) ? v : s_vk.dummy.view,
                                         VK_IMAGE_LAYOUT_GENERAL };
        ci[u] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, is_cube ? v : s_vk.dummy_cube.view,
                                         VK_IMAGE_LAYOUT_GENERAL };
        si[u] = (VkDescriptorImageInfo){ smp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    }
    VkWriteDescriptorSet w[5] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = RSX_SPIRV_VPCONST_BINDING, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .pBufferInfo = &bi[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = RSX_SPIRV_PSCONST_BINDING, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .pBufferInfo = &bi[1] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = RSX_SPIRV_TEXTURE_BINDING, .descriptorCount = RSX_MAX_TEXTURES,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = ii },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = RSX_SPIRV_SAMPLER_BINDING, .descriptorCount = RSX_MAX_TEXTURES,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = si },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = VK_CUBE_BINDING, .descriptorCount = RSX_MAX_TEXTURES,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = ci },
    };
    pvkUpdateDescriptorSets(s_vk.device, 5, w, 0, NULL);
}

/* Viewport, scissor and stencil reference for the bound target size, at the
 * bound targets' scale (s_e2.fb_sc). */
static inline u32 vk_fbs(u32 v)
{
    const float s = s_e2.fb_sc > 0.0f ? s_e2.fb_sc : s_rs;
    return s == 1.0f ? v : (u32)((float)v * s + 0.5f);
}
static void vk_eng_set_dynamic(u32 w, u32 h)
{
    const float fs = s_e2.fb_sc > 0.0f ? s_e2.fb_sc : s_rs;
    VkViewport vp = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
    if (s_e2.have_vp && s_e2.vp[2] > 0.0f && s_e2.vp[3] > 0.0f) {   /* guest units -> the image's */
        vp.x = s_e2.vp[0] * fs; vp.y = s_e2.vp[1] * fs;
        vp.width = s_e2.vp[2] * fs; vp.height = s_e2.vp[3] * fs;
    }
    VkRect2D sc = { { 0, 0 }, { w, h } };
    if (s_e2.have_sc) {                       /* clamped: Vulkan rejects negative or over-size */
        const u32 gx0 = vk_fbs(s_e2.sc[0]), gy0 = vk_fbs(s_e2.sc[1]);
        u32 x0 = gx0 < w ? gx0 : w, y0 = gy0 < h ? gy0 : h;
        u32 x1 = vk_fbs(s_e2.sc[0] + s_e2.sc[2]), y1 = vk_fbs(s_e2.sc[1] + s_e2.sc[3]);
        if (x1 > w) x1 = w;
        if (y1 > h) y1 = h;
        sc.offset.x = (int32_t)x0; sc.offset.y = (int32_t)y0;
        sc.extent.width = x1 > x0 ? x1 - x0 : 0; sc.extent.height = y1 > y0 ? y1 - y0 : 0;
    }
    pvkCmdSetViewport(s_vk.cmd, 0, 1, &vp);
    pvkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
    pvkCmdSetStencilReference(s_vk.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, s_e2.stencil_ref & 0xFFu);
    /* GL's polygon offset and Vulkan's depth bias are the same formula (units
     * times the format's resolvable step, plus factor times the slope). The
     * slope is per PIXEL, so at an internal resolution of N each pixel spans
     * 1/N of the depth the guest's did: the factor scales with it. */
    pvkCmdSetDepthBias(s_vk.cmd, s_e2.dbias_units, 0.0f, s_e2.dbias_factor * fs);
}

/* Batched draw: per-draw data from the arena, a fresh descriptor set, and the
 * render pass kept open across consecutive draws into the same framebuffer. */
/* ---- persistent geometry (rsx_draw_backend.geom_*) ----------------------
 * One host-visible heap; blocks in power-of-two size classes from 256 bytes,
 * each class with its own free list, a block parked on the batch's garbage
 * list until the GPU has finished with it. Drawing cached geometry from here
 * copies nothing; copying it into the batch arena every draw was ~10% of the
 * RSX thread in Zolar Forest. */
#define VK_GEO_HEAP   (1024ull << 20)   /* blocks round up to powers of two: the engine's 480 MB of budgets need up to twice that */
#define VK_GEO_MAX    65536u
#define VK_GEO_CLASSES 23u
typedef struct { VkDeviceSize off, ioff; u32 cls, used, ibytes, next; } vk_geo_blk;
static vk_buffer  s_geo_heap;
static int        s_geo_state;              /* 0 untried, 1 on, -1 unavailable */
static VkDeviceSize s_geo_top;              /* bytes in use (stats) */
static vk_geo_blk s_geo[VK_GEO_MAX];
static u32        s_geo_nh = 1;                    /* next never-used handle */
/* A buddy allocator: block sizes 256 << class, each aligned to its size; a
 * freed block merges with its free buddy. The first version carved the heap
 * into size classes for good -- a block freed in one class was reusable only
 * in that class -- so as the mix of geometry changed the heap filled ("512 MB
 * used") with most of it free in the wrong sizes, and every later upload was
 * refused (copied per draw: the frame-rate cost of long sessions). */
static u64*       s_geo_bits[VK_GEO_CLASSES];     /* free blocks, one bit each */
static u32        s_geo_nfree[VK_GEO_CLASSES], s_geo_hint[VK_GEO_CLASSES];
static u32        s_geo_hstk[VK_GEO_MAX], s_geo_nhstk;   /* recycled handles */
#define VK_GEO_SHIFT(c) (8u + (c))

static int vk_geo_bit(u32 c, u64 i) { return (int)((s_geo_bits[c][i >> 6] >> (i & 63u)) & 1u); }
static void vk_geo_set(u32 c, u64 i, int on)
{
    if (on) { s_geo_bits[c][i >> 6] |= 1ull << (i & 63u); s_geo_nfree[c]++; }
    else    { s_geo_bits[c][i >> 6] &= ~(1ull << (i & 63u)); s_geo_nfree[c]--; }
}
static int vk_geo_init(VkDeviceSize lim)
{
    for (u32 c = 0; c < VK_GEO_CLASSES; c++) {
        const u64 n = (VK_GEO_HEAP >> VK_GEO_SHIFT(c)) + 1u;
        s_geo_bits[c] = (u64*)calloc((size_t)((n + 63u) / 64u), 8);
        if (!s_geo_bits[c]) return -1;
    }
    VkDeviceSize off = 0;                  /* the usable span as aligned free blocks, largest first */
    for (int c = (int)VK_GEO_CLASSES - 1; c >= 0; c--) {
        const VkDeviceSize sz = 256ull << c;
        while (off + sz <= lim && off % sz == 0) { vk_geo_set((u32)c, off >> VK_GEO_SHIFT(c), 1); off += sz; }
    }
    return 0;
}
/* A free block of class `cls` (splitting a larger one); its offset, or -1. */
static long long vk_geo_take(u32 cls)
{
    u32 k = cls;
    while (k < VK_GEO_CLASSES && !s_geo_nfree[k]) k++;
    if (k >= VK_GEO_CLASSES) return -1;
    const u64 words = ((VK_GEO_HEAP >> VK_GEO_SHIFT(k)) + 64u) / 64u;
    u64 idx = ~0ull;
    for (u64 w = 0; w < words; w++) {
        const u64 wi = (s_geo_hint[k] + w) % words;
        if (s_geo_bits[k][wi]) { idx = wi * 64u + (u64)__builtin_ctzll(s_geo_bits[k][wi]); s_geo_hint[k] = (u32)wi; break; }
    }
    if (idx == ~0ull) return -1;
    vk_geo_set(k, idx, 0);
    while (k > cls) { k--; idx <<= 1; vk_geo_set(k, idx + 1u, 1); }   /* keep the lower half */
    return (long long)(idx << VK_GEO_SHIFT(cls));
}
static void vk_geo_give(VkDeviceSize off, u32 cls)
{
    u64 idx = off >> VK_GEO_SHIFT(cls);
    while (cls + 1u < VK_GEO_CLASSES && vk_geo_bit(cls, idx ^ 1u)) {
        vk_geo_set(cls, idx ^ 1u, 0);
        idx >>= 1; cls++;
    }
    vk_geo_set(cls, idx, 1);
}

static void vk_geo_free_now(u32 h)
{
    if (!h || h >= VK_GEO_MAX || !s_geo[h].used) return;
    s_geo[h].used = 0;
    vk_geo_give(s_geo[h].off, s_geo[h].cls);
    s_geo_top -= 256ull << s_geo[h].cls;
    s_geo_hstk[s_geo_nhstk++] = h;
}

static u32 vk_eng_geom_upload(void* user, const void* vertices, u32 vbytes, const u32* indices, u32 ibytes)
{
    s_sync_calls[8]++;
    (void)user;
    if (!s_bt.ready || !vertices || !vbytes) return 0;
    if (s_geo_state == 0) {
        s_geo_state = vk_create_host_buffer(&s_geo_heap, VK_GEO_HEAP,
                                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT) == 0 &&
                      s_geo_heap.coherent ? 1 : -1;
        if (s_geo_state > 0) {
            /* RSX_VK_GEO_MB=<n>: a smaller geometry heap (testing it running full). */
            const char* e = getenv("RSX_VK_GEO_MB");
            unsigned long long lim = e ? (unsigned long long)atoi(e) << 20 : 0;
            if (!lim || lim > VK_GEO_HEAP) lim = VK_GEO_HEAP;
            if (vk_geo_init(lim)) s_geo_state = -1;
        }
        if (s_geo_state < 0) { vk_destroy_buffer(&s_geo_heap); VK_LOG("engine: no geometry heap; cached geometry is copied per draw%c", 10); }
    }
    if (s_geo_state < 0) return 0;
    const VkDeviceSize ioff_rel = ((VkDeviceSize)vbytes + 15u) & ~(VkDeviceSize)15u;
    const VkDeviceSize need = ioff_rel + ibytes;
    u32 cls = 0;
    while (cls < VK_GEO_CLASSES && (256ull << cls) < need) cls++;
    if (cls >= VK_GEO_CLASSES) return 0;
    u32 h = 0;
    const long long off = (s_geo_nhstk || s_geo_nh < VK_GEO_MAX) ? vk_geo_take(cls) : -1;
    if (off < 0) {
        static unsigned long long fails;
        ++fails;
        if ((fails & (fails - 1)) == 0)
            VK_LOG("engine: geometry heap full (%llu MB in use, class %u) -- %llu uploads refused so far%c",
                   (unsigned long long)(s_geo_top >> 20), cls, fails, 10);
        return 0;
    }
    h = s_geo_nhstk ? s_geo_hstk[--s_geo_nhstk] : s_geo_nh++;
    s_geo[h].off = (VkDeviceSize)off; s_geo[h].cls = cls;
    s_geo_top += 256ull << cls;
    s_geo[h].used = 1; s_geo[h].ibytes = ibytes; s_geo[h].ioff = s_geo[h].off + ioff_rel;
    memcpy((u8*)s_geo_heap.ptr + s_geo[h].off, vertices, vbytes);
    if (ibytes && indices) memcpy((u8*)s_geo_heap.ptr + s_geo[h].ioff, indices, ibytes);
    return h;
}

static void vk_eng_geom_release(void* user, u32 h)
{
    (void)user;
    if (!h || h >= VK_GEO_MAX || !s_geo[h].used) return;
    if (!vk_bt_push(5, h, 0, 0)) vk_geo_free_now(h);   /* nothing in flight: now */
}

/* A descriptor set for the bound textures and samplers and these block sizes
 * (their offsets are dynamic): one per combination per batch, from a small
 * cache cleared with the batch's descriptor pool. A fresh set written for
 * every draw was ~6% of the RSX thread. VK_NULL_HANDLE = pool exhausted. */
#define VK_SETC 2048u
static struct { u64 key; VkDescriptorSet set; } s_setc[VK_SETC];
static u32 s_setc_epoch;
static VkDescriptorSet vk_eng_set_for(vk_bt_slot* sl, u32 vsb, u32 psb)
{
    if (s_setc_epoch != s_bt.epoch) { memset(s_setc, 0, sizeof s_setc); s_setc_epoch = s_bt.epoch; }
    u64 h = 1469598103934665603ull;
    #define VK_MIX(v) (h = (h ^ (u64)(v)) * 1099511628211ull)
    VK_MIX(s_e2.tex_mask); VK_MIX(vsb); VK_MIX(psb); VK_MIX(s_eobj_gen);
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        if (!((s_e2.tex_mask >> u) & 1u)) continue;
        VK_MIX(s_e2.tex[u]);
        const u32* sw = (const u32*)&s_e2.samp[u];
        for (u32 k = 0; k < sizeof s_e2.samp[u] / 4u; k++) VK_MIX(sw[k]);
    }
    #undef VK_MIX
    h |= 1u;
    u32 i = (u32)(h ^ (h >> 32)) & (VK_SETC - 1u);
    for (u32 probe = 0; probe < 8; probe++, i = (i + 1u) & (VK_SETC - 1u)) {
        if (s_setc[i].key == h) return s_setc[i].set;
        if (!s_setc[i].key) break;
    }
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = sl->pool, .descriptorSetCount = 1,
                                        .pSetLayouts = &s_g.set_layout };
    if (pvkAllocateDescriptorSets(s_vk.device, &dai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkDescriptorBufferInfo bi[2] = { { sl->arena.buf, 0, vsb }, { sl->arena.buf, 0, psb } };
    vk_eng_write_descriptors_to(set, bi);
    if (!s_setc[i].key) { s_setc[i].key = h; s_setc[i].set = set; }
    return set;
}

static int s_draw_geom_vi;        /* vk_eng_draw_cached_vi: the block holds vertices only */
static int s_draw_vofs;           /* ... and the draw's base vertex */
static void vk_eng_draw_batched(VkPipeline pipe, VkRenderPass rp, VkFramebuffer fb, u32 w, u32 h,
                                u32 n, const void* vertices, VkDeviceSize vbytes,
                                const u32* indices, u32 index_count, u32 vertex_count, u32 geom)
{
    const int gidx = geom && !s_draw_geom_vi;      /* the indices are in the block too */
    const VkDeviceSize ibytes = gidx ? (index_count ? (VkDeviceSize)index_count * 4u : 0)
                                     : (indices && index_count) ? (VkDeviceSize)index_count * 4u : 0;
    const u32 vsb = s_e2.vs_bytes ? s_e2.vs_bytes : 16u, psb = s_e2.ps_bytes ? s_e2.ps_bytes : 16u;
#ifdef __ANDROID__
    /* Android: hand the GPU what is recorded so far at a render-pass switch once
     * the batch holds RACACIT_GPU_SUBMIT_DRAWS draws (default 1024; 0 = only at
     * the present). A frame used to go to the GPU whole at the present: the GPU
     * idled while the CPU recorded it, then the present waited while the GPU drew
     * it -- the two added up, ~30 ms a frame on a phone, where a desktop GPU hides
     * it. Done before this draw takes anything from the batch. */
    {
        static int s_every = -1;
        if (s_every < 0) { const char* e = getenv("RACACIT_GPU_SUBMIT_DRAWS"); s_every = e && *e ? atoi(e) : 1024; }
        if (s_every > 0 && s_bt.recording && s_bt_draws >= (u32)s_every &&
            (!s_bt.in_pass || s_bt.rp != rp || s_bt.fb != fb || s_bt.w != w || s_bt.h != h))
            vk_bt_submit(0, VK_NULL_HANDLE, VK_NULL_HANDLE);
        s_bt_draws++;
    }
#endif
    for (int attempt = 0; attempt < 2; attempt++) {
        if (vk_bt_open()) return;
        vk_bt_slot* sl = &s_bt.s[s_bt.cur];
        const VkDeviceSize mark = sl->used;
        VkDeviceSize vo = 0, io = 0, uvo = 0, upo = 0;
        u8* vp_ = geom ? (vo = s_geo[geom].off, (u8*)1) : (u8*)vk_bt_alloc(vbytes, 16, &vo);
        u8* ip_ = (gidx || !ibytes) ? (io = gidx ? s_geo[geom].ioff : 0, (u8*)1) : (u8*)vk_bt_alloc(ibytes, 4, &io);
        if (s_vk_foreign_bind) { s_vk_foreign_bind = 0; s_e2.reuse_epoch = 0; s_e2.dyn_epoch = 0; }
        const int same = s_e2.reuse_epoch == s_bt.epoch;
        const int vs_keep = same && !s_e2.vs_dirty && s_e2.last_vsb == vsb;
        const int ps_keep = same && !s_e2.ps_dirty && s_e2.last_psb == psb;
        u8* uv = vs_keep ? (uvo = s_e2.last_uvo, (u8*)1) : (u8*)vk_bt_alloc(vsb, s_bt.ubo_align, &uvo);
        u8* up = ps_keep ? (upo = s_e2.last_upo, (u8*)1) : (u8*)vk_bt_alloc(psb, s_bt.ubo_align, &upo);
        const int set_keep = same && !s_e2.tex_dirty && s_e2.last_set && s_e2.last_vsb == vsb &&
                             s_e2.last_psb == psb && s_e2.last_obj_gen == s_eobj_gen;
        VkDescriptorSet set = set_keep ? s_e2.last_set : VK_NULL_HANDLE;
        if (!set_keep && vp_ && ip_ && uv && up) set = vk_eng_set_for(sl, vsb, psb);
        if (!vp_ || !ip_ || !uv || !up) set = VK_NULL_HANDLE;   /* arena full: submit and retry */
        if (!set) {
            /* Arena or pool exhausted: hand this batch to the GPU, retry once. */
            sl->used = mark;
            if (attempt == 0 && vk_bt_submit(0, VK_NULL_HANDLE, VK_NULL_HANDLE) == 0) continue;
            return;
        }
        if (!geom) memcpy(vp_, vertices, (size_t)vbytes);
        if (ibytes && !gidx) memcpy(ip_, indices, (size_t)ibytes);
        const VkBuffer vbuf = geom ? s_geo_heap.buf : sl->arena.buf;
        if (!vs_keep) { if (s_e2.vs_bytes && s_e2.vs_src) memcpy(uv, s_e2.vs_src, s_e2.vs_bytes); else memset(uv, 0, vsb); }
        if (!ps_keep) { if (s_e2.ps_bytes && s_e2.ps_shadow) memcpy(up, s_e2.ps_shadow, s_e2.ps_bytes); else memset(up, 0, psb); }
        s_e2.reuse_epoch = s_bt.epoch; s_e2.last_obj_gen = s_eobj_gen;
        s_e2.vs_dirty = s_e2.ps_dirty = s_e2.tex_dirty = 0;
        s_e2.last_uvo = uvo; s_e2.last_upo = upo; s_e2.last_vsb = vsb; s_e2.last_psb = psb;
        s_e2.last_set = set;

        if (!s_bt.in_pass || s_bt.rp != rp || s_bt.fb != fb || s_bt.w != w || s_bt.h != h) {
            vk_bt_end_pass();
            vk_full_barrier();
            VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                          .renderPass = rp, .framebuffer = fb,
                                          .renderArea = { { 0, 0 }, { w, h } } };
            pvkCmdBeginRenderPass(s_vk.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
            s_bt.in_pass = 1; s_bt.rp = rp; s_bt.fb = fb; s_bt.w = w; s_bt.h = h;
        }
        (void)n;
        const int dyn_same = s_e2.dyn_epoch == s_bt.epoch;
        const int rebound = !dyn_same || s_e2.last_pipe != pipe;
        if (rebound) {
            pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            s_e2.last_pipe = pipe;
        }
        /* Binding a pipeline without the dynamic state resets the command
         * buffer's constant, so it is set again after any bind. */
        if (s_e2.blend_c_on && (rebound || memcmp(s_e2.dyn_blend_c, s_e2.blend_c, sizeof s_e2.blend_c))) {
            pvkCmdSetBlendConstants(s_vk.cmd, s_e2.blend_c);
            memcpy(s_e2.dyn_blend_c, s_e2.blend_c, sizeof s_e2.blend_c);
        }
        if (!dyn_same || s_e2.dyn_w != w || s_e2.dyn_h != h || s_e2.dyn_ref != s_e2.stencil_ref ||
            s_e2.dyn_fbsc != s_e2.fb_sc ||
            s_e2.dyn_dbu != s_e2.dbias_units || s_e2.dyn_dbf != s_e2.dbias_factor ||
            s_e2.dyn_have_vp != (u32)s_e2.have_vp || s_e2.dyn_have_sc != (u32)s_e2.have_sc ||
            memcmp(s_e2.dyn_vp, s_e2.vp, sizeof s_e2.vp) || memcmp(s_e2.dyn_sc, s_e2.sc, sizeof s_e2.sc)) {
            vk_eng_set_dynamic(w, h);
            s_e2.dyn_w = w; s_e2.dyn_h = h; s_e2.dyn_ref = s_e2.stencil_ref; s_e2.dyn_fbsc = s_e2.fb_sc;
            s_e2.dyn_dbu = s_e2.dbias_units; s_e2.dyn_dbf = s_e2.dbias_factor;
            s_e2.dyn_have_vp = (u32)s_e2.have_vp; s_e2.dyn_have_sc = (u32)s_e2.have_sc;
            memcpy(s_e2.dyn_vp, s_e2.vp, sizeof s_e2.vp); memcpy(s_e2.dyn_sc, s_e2.sc, sizeof s_e2.sc);
        }
        s_e2.dyn_epoch = s_bt.epoch;
        if (!dyn_same || set != s_e2.bound_set || uvo != s_e2.bound_uvo || upo != s_e2.bound_upo) {
            const uint32_t offs[2] = { (uint32_t)uvo, (uint32_t)upo };
            pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_g.pipe_layout,
                                     0, 1, &set, 2, offs);
            s_e2.bound_set = set; s_e2.bound_uvo = uvo; s_e2.bound_upo = upo;
        }
        pvkCmdBindVertexBuffers(s_vk.cmd, 0, 1, &vbuf, &vo);
        if (ibytes) {
            pvkCmdBindIndexBuffer(s_vk.cmd, gidx ? s_geo_heap.buf : sl->arena.buf, io, VK_INDEX_TYPE_UINT32);
            pvkCmdDrawIndexed(s_vk.cmd, index_count, 1, 0, s_draw_geom_vi ? s_draw_vofs : 0, 0);
        } else {
            pvkCmdDraw(s_vk.cmd, vertex_count, 1, 0, 0);
        }
        return;
    }
}

/* RSX_ENG_STATS=1: which way each engine draw left vk_eng_draw (printed with
 * the engine's own line). Most exits here are silent. */
static u64 s_vk_draw_exit[9];
#define VK_DRAW_EXIT(i) (s_vk_draw_exit[i]++)
void rsx_vulkan_engine_stats_report(void)
{
    static const char* name[9] = { "no-pipeline-or-verts", "bad-topology", "stride-mismatch", "rt-count-mismatch",
                                   "rt-missing", "no-depth-view", "no-pipe-rp-fb", "drawn-batched", "drawn" };
    fprintf(stderr, "[RSX vulkan] engine draws:");
    for (int i = 0; i < 9; i++) { fprintf(stderr, " %s=%llu", name[i], (unsigned long long)s_vk_draw_exit[i]); s_vk_draw_exit[i] = 0; }
    fprintf(stderr, "%c", 10);
    fprintf(stderr, "[RSX vulkan] GPU waits %llu (%.1f ms), submits %llu (%.1f ms)%c", s_vkt_waits, s_vkt_wait_us / 1000.0,
            s_vkt_submits, s_vkt_submit_us / 1000.0, 10);
    s_vkt_waits = s_vkt_wait_us = s_vkt_submits = s_vkt_submit_us = 0;
    fprintf(stderr, "[RSX vulkan] shader translations %llu (%.1f ms), pipeline variants %llu (%.1f ms)%c",
            s_vkt_spv, s_vkt_spv_us / 1000.0, s_vkt_var, s_vkt_var_us / 1000.0, 10);
    s_vkt_spv = s_vkt_spv_us = s_vkt_var = s_vkt_var_us = 0;
    { unsigned long h = 0, m = 0; rsx_hlsl_to_spirv_cache_stats(&h, &m);
      fprintf(stderr, "[RSX vulkan] spirv cache since start: %lu hits, %lu translated%c", h, m, 10); }
    fprintf(stderr, "[RSX vulkan] created: images %llu memory %llu framebuffers %llu samplers %llu views %llu pipelines %llu buffers %llu%c",
            s_vkc[0], s_vkc[1], s_vkc[2], s_vkc[3], s_vkc[4], s_vkc[5], s_vkc[6], 10);
    fprintf(stderr, "[RSX vulkan] images by purpose: colour %llu depth %llu texture %llu colour-snap %llu depth-snap %llu%c",
            s_img_why[0], s_img_why[1], s_img_why[2], s_img_why[3], s_img_why[4], 10);
    memset(s_img_why, 0, sizeof s_img_why);
    fprintf(stderr, "[RSX vulkan] sync calls: submit %llu texcreate %llu colorcreate %llu view %llu depthcreate %llu dsnap %llu csnap %llu pipe %llu geom %llu readback %llu blit %llu%c",
            s_sync_calls[0], s_sync_calls[1], s_sync_calls[2], s_sync_calls[3], s_sync_calls[4], s_sync_calls[5],
            s_sync_calls[6], s_sync_calls[7], s_sync_calls[8], s_sync_calls[9], s_sync_calls[10], 10);
    memset(s_sync_calls, 0, sizeof s_sync_calls);
    memset(s_vkc, 0, sizeof s_vkc);
}

static u32 s_draw_geom;          /* vk_eng_draw_cached's block for the vk_eng_draw it calls */
static void vk_eng_draw(void* user, rsx_topology topology, const void* vertices, u32 vertex_count,
                        u32 stride, const u32* indices, u32 index_count)
{
    (void)user;
    if (!s_e2.pipeline || !vertices || !vertex_count || !stride) { VK_DRAW_EXIT(0); return; }
    if (topology < RSX_TOPOLOGY_POINTS || topology > RSX_TOPOLOGY_TRIANGLE_STRIP) { VK_DRAW_EXIT(1); return; }
    vk_eng_pipe* P = &s_epipe[s_e2.pipeline - 1];
    if (!P->used || stride != P->stride) { VK_DRAW_EXIT(2); return; }

    /* Targets: every member of a set is target A's size (engine contract). */
    const u32 n = s_eng_bound.nrt;
    if (n < 1 || n != P->rt_count) {
        if (!(s_e2.warned & 16u)) { VK_LOG("engine: draw with %u colour targets for a %u-target pipeline skipped\n", n, P->rt_count); s_e2.warned |= 16u; }
        VK_DRAW_EXIT(3);
        return;
    }
    /* Read without giving it an image: a depth-only pass into a shadow map never uses it. */
    vk_eng_obj* rt0 = s_eng_bound.rt[0] && s_eng_bound.rt[0] < VK_ENG_MAX_OBJ ? &s_eobj[s_eng_bound.rt[0]] : NULL;
    if (!rt0 || rt0->kind != VK_ENG_COLOR) { VK_DRAW_EXIT(4); return; }
    for (u32 i = 1; i < n; i++)
        if (!vk_eng_get(s_eng_bound.rt[i], VK_ENG_COLOR)) { VK_DRAW_EXIT(4); return; }
    /* The bound targets' format must be the one the pipeline was built for. */
    const int fi = vk_eng_rt_fi(rt0->fmt);
    if (fi < 0 || (u32)fi != P->rt_fi) {
        if (!(s_e2.warned & 256u)) { VK_LOG("engine: draw into a colour format its pipeline was not built for; skipped\n"); s_e2.warned |= 256u; }
        VK_DRAW_EXIT(3);
        return;
    }
    u32 w = rt0->w, h = rt0->h, fbw = w, fbh = h, zrt = 0;
    const u32* rts = s_eng_bound.rt;
    vk_eng_obj* zo = vk_eng_get(s_eng_bound.depth, VK_ENG_DEPTH);
    s_e2.fb_sc = 0.0f;
    if (zo && zo->sc > 0.0f) {
        /* A shadow map (RACACIT_SHADOW_RES): a depth-only pass draws at its
         * scale, the guest's area of the pass's colour target over it. */
        if (n == 1 && !P->rs.color_mask && !P->wpos)
            zrt = vk_eng_zonly_color((u32)fi, rt0->fmt, zo->w, zo->h);
        if (zrt) {
            w = vk_os(zo, vk_lw(rt0)); h = vk_os(zo, vk_lh(rt0));
            if (w > zo->w) w = zo->w;
            if (h > zo->h) h = zo->h;
            fbw = zo->w; fbh = zo->h;
            rts = &zrt;
            s_e2.fb_sc = zo->sc;
        } else if (P->rs.depth_test || P->rs.stencil_enable) {
            vk_eng_depth_demote(s_eng_bound.depth, P->rs.color_mask, n, P->wpos);
        }
        /* Neither depth nor stencil test: the pass never touches the buffer
         * (A Crack in Time leaves its zeta on the sun map's address while it
         * draws a 256x176 bloom level there), so the scratch depth stands in. */
    }
    if (!zrt && !vk_eng_get(s_eng_bound.rt[0], VK_ENG_COLOR)) { VK_DRAW_EXIT(4); return; }
    /* A depth target is bound only at the scale the pass draws at (a shadow
     * map whose demotion failed falls back to the scratch depth). */
    VkImageView dview = (zo && zo->sc == s_e2.fb_sc && zo->w >= w && zo->h >= h) ? zo->im.view
                                                                               : vk_eng_scratch_depth(w, h);
    if (!dview) { VK_DRAW_EXIT(5); return; }
    const u32 dkey = (zo && dview == zo->im.view) ? s_eng_bound.depth : 0xFFFFFFFFu;

    VkPipeline pipe = vk_eng_variant(P, (u32)topology - 1u);
    s_e2.blend_c_on = vk_rs_blend_const(&P->rs);
    VkRenderPass rp = zrt ? vk_eng_render_pass_zonly((u32)fi) : vk_eng_render_pass(n, (u32)fi);
    VkFramebuffer fb = vk_eng_framebuffer(rp, rts, n, dview, dkey, fbw, fbh);
    if (!pipe || !rp || !fb) {
        if (rt0 && rt0->w == 1280 && rt0->h == 720) {
            static u32 s_vk_drop6 = 0;
            if (s_vk_drop6++ < 30) VK_LOG("[vk-disp-drop] exit 6 on 1280x720: pipe=%p rp=%p fb=%p\n", (void*)pipe, (void*)rp, (void*)fb);
        }
        VK_DRAW_EXIT(6); return;
    }

    if (rt0 && rt0->w == 1280 && rt0->h == 720) {
        static u32 s_vk_ok_cnt = 0;
        if (s_vk_ok_cnt < 30) {
            s_vk_ok_cnt++;
            VK_LOG("[vk-disp-ok] #%u: rt0=%u fmt=%d fi=%d n=%u pipe=%p rp=%p fb=%p dview=%p zo=%p batched=%d\n",
                   s_vk_ok_cnt, s_eng_bound.rt[0], rt0->fmt, fi, n, (void*)pipe, (void*)rp, (void*)fb, (void*)dview, (void*)zo, s_bt.ready);
        }
    }

    const VkDeviceSize vbytes = (VkDeviceSize)vertex_count * stride;
    VK_DRAW_EXIT(s_bt.ready ? 7 : 8);
    if (s_bt.ready) { vk_eng_draw_batched(pipe, rp, fb, w, h, n, vertices, vbytes, indices, index_count, vertex_count, s_draw_geom); return; }
    if (vk_ensure_vertex_buffer(vbytes)) return;
    memcpy(s_vk.vertices.ptr, vertices, (size_t)vbytes);
    if (indices && index_count) {
        if (vk_eng_ensure_indices((VkDeviceSize)index_count * 4u)) return;
        memcpy(s_e2.indices.ptr, indices, (size_t)index_count * 4u);
    }
    vk_eng_write_descriptors();

    if (vk_begin()) return;
    /* Everything submitted before is complete (each submission is waited
     * on); this orders memory for the surfaces this draw touches. */
    for (u32 i = 0; i < n; i++)
        vk_barrier_image(s_eobj[rts[i]].im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = rp, .framebuffer = fb,
                                  .renderArea = { { 0, 0 }, { w, h } } };
    pvkCmdBeginRenderPass(s_vk.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    s_vk_foreign_bind = 1;   /* the batched path's cached binds are gone */
    vk_eng_set_dynamic(w, h);
    if (s_e2.blend_c_on) pvkCmdSetBlendConstants(s_vk.cmd, s_e2.blend_c);
    { static const uint32_t zero_offs[2] = { 0, 0 };
      pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_g.pipe_layout,
                               0, 1, &s_g.set, 2, zero_offs); }
    VkDeviceSize off = 0;
    pvkCmdBindVertexBuffers(s_vk.cmd, 0, 1, &s_vk.vertices.buf, &off);
    if (indices && index_count) {
        pvkCmdBindIndexBuffer(s_vk.cmd, s_e2.indices.buf, 0, VK_INDEX_TYPE_UINT32);
        pvkCmdDrawIndexed(s_vk.cmd, index_count, 1, 0, 0, 0);
    } else {
        pvkCmdDraw(s_vk.cmd, vertex_count, 1, 0, 0);
    }
    pvkCmdEndRenderPass(s_vk.cmd);
    vk_submit_and_wait();
}

/* rsx_draw_backend.draw_cached: vk_eng_draw with the vertices and indices
 * already in the geometry heap. */
static void vk_eng_draw_cached(void* user, rsx_topology topology, u32 geom, u32 vertex_count,
                               u32 stride, u32 index_count)
{
    if (!geom || geom >= VK_GEO_MAX || !s_geo[geom].used || !s_bt.ready) return;
    s_draw_geom = geom;
    vk_eng_draw(user, topology, (const void*)&s_geo[geom], vertex_count, stride, NULL, index_count);
    s_draw_geom = 0;
}

/* rsx_draw_backend.draw_cached_vi: the vertices in the geometry heap, the
 * indices this draw's own (copied into the batch arena like any draw's). */
static void vk_eng_draw_cached_vi(void* user, rsx_topology topology, u32 geom, u32 vertex_count,
                                  u32 stride, const u32* indices, u32 index_count, int vertex_offset)
{
    if (!geom || geom >= VK_GEO_MAX || !s_geo[geom].used || !s_bt.ready || !indices || !index_count) return;
    s_draw_geom = geom; s_draw_geom_vi = 1; s_draw_vofs = vertex_offset;
    vk_eng_draw(user, topology, (const void*)&s_geo[geom], vertex_count, stride, indices, index_count);
    s_draw_geom = 0; s_draw_geom_vi = 0; s_draw_vofs = 0;
}

static const rsx_draw_backend s_vk_engine_backend = {
    .user                 = NULL,
    .init                 = vk_eng_init,
    .shutdown             = vk_eng_shutdown,
    .submit_and_wait      = vk_eng_submit_and_wait,
    .texture_create       = vk_eng_texture_create,
    .texture_upload       = vk_eng_texture_upload,
    .texture_release      = vk_eng_release,
    .color_target_create  = vk_eng_color_target_create,
    .color_target_release = vk_eng_release,
    .surface_view         = vk_eng_surface_view,
    .depth_target_create  = vk_eng_depth_target_create,
    .depth_target_release = vk_eng_release,
    .depth_snapshot       = vk_eng_depth_snapshot,
    .depth_snapshot_sized = vk_eng_depth_snapshot_sized,
    .geom_upload          = vk_eng_geom_upload,
    .geom_release         = vk_eng_geom_release,
    .draw_cached          = vk_eng_draw_cached,
    .draw_cached_vi       = vk_eng_draw_cached_vi,
    .color_snapshot       = vk_eng_color_snapshot,
    .blit                 = vk_eng_blit,
    .pipeline_create      = vk_eng_pipeline_create,
    .pipeline_release     = vk_eng_pipeline_release,
    .bind_targets         = vk_eng_bind_targets,
    .bind_pipeline        = vk_eng_bind_pipeline,
    .bind_vs_constants    = vk_eng_bind_vs_constants,
    .bind_ps_constants    = vk_eng_bind_ps_constants,
    .bind_textures        = vk_eng_bind_textures,
    .bind_vertex_textures = vk_eng_bind_vertex_textures,
    .set_viewport         = vk_eng_set_viewport,
    .set_scissor          = vk_eng_set_scissor,
    .set_stencil_ref      = vk_eng_set_stencil_ref,
    .set_depth_bias       = vk_eng_set_depth_bias,
    .set_blend_color      = vk_eng_set_blend_color,
    .draw                 = vk_eng_draw,
    .clear_color          = vk_eng_clear_color,
    .clear_depth_stencil  = vk_eng_clear_depth_stencil,
    .present              = vk_eng_present,
    .readback             = vk_eng_readback,
};

static rsx_backend s_vulkan_backend = {
    .userdata           = &s_vk,
    .set_render_target  = vk_cb_track_state,
    .set_vertex_attribs = vk_cb_track_state,
    .set_depth_stencil  = vk_cb_track_state,
    .clear              = vk_cb_clear,
    .draw_arrays        = vk_cb_draw_arrays,
    .draw_indexed       = vk_cb_draw_indexed,
    .bind_texture       = vk_cb_bind_texture,
    .present            = vk_cb_present,
};

/* ---------------------------------------------------------------------------
 * Public entry points
 * -------------------------------------------------------------------------*/
static int vk_init_all(u32 width, u32 height, const char* title)
{
    s_vk.width  = width  ? width  : 1280;
    s_vk.height = height ? height : 720;
    s_vk.dump_path = getenv("PS3RECOMP_VK_DUMP");

    vk_window_open(title);           /* no-op unless PS3RECOMP_VK_WINDOW is set */
    /* The window opened at the guest's size; the image the frame is composed
     * in follows the internal resolution, so a higher one reaches the screen
     * (the swapchain blit fits it to the window). */
    s_vk.guest_w = s_vk.width; s_vk.guest_h = s_vk.height;
    { const char* e = getenv("RACACIT_RES_SCALE");
      float sc = e ? (float)atof(e) : 1.0f;
      if (!(sc >= 0.25f)) sc = 1.0f;
      if (sc > 4.0f) sc = 4.0f;
      s_rs = sc;
      extern float rsx_fp_wpos_scale;
      rsx_fp_wpos_scale = sc;
      extern float g_rsx_res_scale;      /* the engine's memory budgets count our bytes */
      g_rsx_res_scale = sc;
      if (sc != 1.0f) {
          s_vk.width = vk_rs(s_vk.width); s_vk.height = vk_rs(s_vk.height);
          VK_LOG("internal resolution x%.2f (display image %ux%u)\n", (double)sc, s_vk.width, s_vk.height);
      } }
    { unsigned u = 0, b = 0;             /* the engine's texture budget follows the card's */
      extern unsigned g_rsx_vram_budget_mb;
      if (!vk_vram_mb(&u, &b) && b) g_rsx_vram_budget_mb = b; }
    if (vk_load_library()) return -1;

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .pApplicationName = "ps3recomp", .pEngineName = "ps3recomp RSX",
                              .apiVersion = VK_API_VERSION_1_0 };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                .pApplicationInfo = &app };
    if (s_vk.windowed) {
        ci.enabledExtensionCount   = s_vk.inst_ext_count;
        ci.ppEnabledExtensionNames = s_vk.inst_exts;
    }
    VkResult ir = pvkCreateInstance(&ci, NULL, &s_vk.instance);
    if (ir != VK_SUCCESS && s_vk.windowed) {
        vk_window_drop("vkCreateInstance with the surface extensions failed");
        ci.enabledExtensionCount = 0;
        ci.ppEnabledExtensionNames = NULL;
        ir = pvkCreateInstance(&ci, NULL, &s_vk.instance);
    }
    if (ir != VK_SUCCESS) { VK_LOG("vkCreateInstance failed (VkResult %d)\n", (int)ir); return -1; }
    if (vk_load_instance_functions()) return -1;
    if (s_vk.windowed) {
        if (vk_load_wsi_instance_functions()) {
            vk_window_drop("the driver has no surface functions");
        } else if (!SDL_Vulkan_CreateSurface(s_vk.window, s_vk.instance, &s_vk.surface)) {
            char why[256];
            snprintf(why, sizeof why, "SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
            vk_window_drop(why);
        }
    }
    if (vk_pick_device()) {
        if (!s_vk.windowed) return -1;
        vk_window_drop("no device can present to the window");
        if (vk_pick_device()) return -1;
    }
    /* RACACIT_SHADOW_RES: shadow maps at this multiple of the scene's
     * resolution (1 = the scene's, at most 4; vk_os). Default 2 on a dedicated
     * card, where it is next to free (RTX 3070 Ti at 2x: ~0.03 ms a frame), and
     * 1 on an integrated GPU, which shares memory bandwidth with the CPU (the
     * dev box's AMD Radeon integrated GPU at 1x: 60 -> 53 fps in Zolar Forest). */
    { VkPhysicalDeviceProperties dp;
      pvkGetPhysicalDeviceProperties(s_vk.phys, &dp);
      const int igpu = dp.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
      const char* se = getenv("RACACIT_SHADOW_RES");
      /* Under 5 GB of budget (a 4 GB card) they stay at the scene's too: the maps cost
       * the card fill rate and memory it has little of. */
      extern unsigned g_rsx_vram_budget_mb;
      const int small_card = g_rsx_vram_budget_mb && g_rsx_vram_budget_mb < 5000u;
      float sr = se && *se ? (float)atof(se) : (igpu || small_card) ? 1.0f : 2.0f;
      if (!(sr >= 1.0f)) sr = 1.0f;
      if (sr > 4.0f) sr = 4.0f;
      extern float g_rsx_shadow_scale;
      g_rsx_shadow_scale = sr > 1.0f ? s_rs * sr : 0.0f;
      if (sr > 1.0f)
          VK_LOG("shadow maps at x%.2f the scene's resolution (x%.2f; 128x128 -> %u, 2048 a side at most)%c",
                 (double)sr, (double)(s_rs * sr), (u32)(128.0f * s_rs * sr + 0.5f), 10);
      else
          VK_LOG("shadow maps at the scene's resolution (%s)%c",
                 se && *se ? "RACACIT_SHADOW_RES" : igpu ? "integrated GPU; RACACIT_SHADOW_RES=2 for sharper shadows" : "under 5 GB of video memory; RACACIT_SHADOW_RES=2 for sharper shadows", 10); }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = s_vk.queue_family,
                                    .queueCount = 1, .pQueuePriorities = &prio };
    const char* dev_exts[4]; u32 n_dev_exts = 0;
    if (s_vk.windowed) dev_exts[n_dev_exts++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    const int fault_ext = vk_has_device_extension(s_vk.phys, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
    VkPhysicalDeviceFaultFeaturesEXT fault_feat = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT,
                                                    .deviceFault = VK_TRUE };
    /* "Only from the pipeline cache" pipeline creation (vk_eng_variant: a
     * real compile goes to a background thread). */
    const int pcc_ext = vk_has_device_extension(s_vk.phys, VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME);
    VkPhysicalDevicePipelineCreationCacheControlFeaturesEXT pcc_feat = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES_EXT,
        .pipelineCreationCacheControl = VK_TRUE };
    /* DXT (BC1-3) is how PS3 titles ship most textures; Vulkan gates it
     * behind a feature, enabled only when the device reports it. */
    VkPhysicalDeviceFeatures have, want;
    memset(&want, 0, sizeof want);
    pvkGetPhysicalDeviceFeatures(s_vk.phys, &have);
    want.textureCompressionBC = have.textureCompressionBC;
    /* Guest vertex/index streams are sized from guest state the backend
     * cannot fully validate, and a driver that faults on an out-of-range
     * fetch takes the process with it (llvmpipe does). robustBufferAccess is
     * mandatory-to-support in Vulkan 1.0, so this only turns it on. */
    want.robustBufferAccess = have.robustBufferAccess;
    want.depthClamp = have.depthClamp;
    /* RACACIT_ANISO=2..16: anisotropic filtering on mipmapped textures, which
     * the RSX did not have -- ground and walls at a glancing angle stay sharp
     * instead of blurring into their smallest mips. */
    { rsx_host_options o; rsx_host_ui_options(&o);
      if (have.samplerAnisotropy) {
          VkPhysicalDeviceProperties pp; pvkGetPhysicalDeviceProperties(s_vk.phys, &pp);
          want.samplerAnisotropy = VK_TRUE;
          s_vk.max_aniso = pp.limits.maxSamplerAnisotropy;
      }
      vk_apply_filter_options(&o); }
    s_vk.bc_ok = have.textureCompressionBC ? 1 : 0;
    s_vk.depth_clamp_ok = have.depthClamp ? 1 : 0;
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                               .pEnabledFeatures = &want };
    if (fault_ext || pcc_ext) {               /* refused: created again without them */
        u32 ne = n_dev_exts; const void* chain = NULL;
        if (pcc_ext) { dev_exts[ne++] = VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME; pcc_feat.pNext = (void*)chain; chain = &pcc_feat; }
        if (fault_ext) { dev_exts[ne++] = VK_EXT_DEVICE_FAULT_EXTENSION_NAME; fault_feat.pNext = (void*)chain; chain = &fault_feat; }
        dci.enabledExtensionCount = ne; dci.ppEnabledExtensionNames = dev_exts;
        dci.pNext = chain;
        if (pvkCreateDevice(s_vk.phys, &dci, NULL, &s_vk.device) != VK_SUCCESS) s_vk.device = VK_NULL_HANDLE;
        else s_vk_pcc = pcc_ext;
    }
    if (!s_vk.device) {
        dci.pNext = NULL;
        dci.enabledExtensionCount = n_dev_exts; dci.ppEnabledExtensionNames = n_dev_exts ? dev_exts : NULL;
        VK_CHECK(pvkCreateDevice(s_vk.phys, &dci, NULL, &s_vk.device), "vkCreateDevice");
    } else {
        if (fault_ext) {
            pvkGetDeviceFaultInfoEXT = (PFN_vkGetDeviceFaultInfoEXT)pvkGetDeviceProcAddr(s_vk.device, "vkGetDeviceFaultInfoEXT");
            VK_LOG("device fault reporting on (VK_EXT_device_fault)\n");
        }
        if (s_vk_pcc) VK_LOG("new pipelines compile in the background (VK_EXT_pipeline_creation_cache_control)\n");
    }
    if (vk_load_device_functions()) return -1;
    if (s_vk.windowed && vk_load_wsi_device_functions())
        vk_window_drop("the driver has no swapchain functions");
    pvkGetDeviceQueue(s_vk.device, s_vk.queue_family, 0, &s_vk.queue);

    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                    .queueFamilyIndex = s_vk.queue_family };
    VK_CHECK(pvkCreateCommandPool(s_vk.device, &pci, NULL, &s_vk.pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = s_vk.pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1 };
    VK_CHECK(pvkAllocateCommandBuffers(s_vk.device, &cai, &s_vk.cmd), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VK_CHECK(pvkCreateFence(s_vk.device, &fci, NULL, &s_vk.fence), "vkCreateFence");
    /* PS3RECOMP_VK_SYNC=1: the old submit-and-wait-per-operation mode. */
    if (!vk_env_on("PS3RECOMP_VK_SYNC") && vk_bt_init()) return -1;

    if (vk_pick_depth_format()) return -1;
    vk_pick_eng_depth_format();
    if (vk_create_image(&s_vk.color, VK_FORMAT_R8G8B8A8_UNORM, s_vk.width, s_vk.height,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT)) return -1;
    if (vk_create_image(&s_vk.depth, s_vk.depth_format, s_vk.width, s_vk.height,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT)) return -1;
    if (vk_create_host_buffer(&s_vk.readback, (VkDeviceSize)s_vk.width * s_vk.height * 4u,
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return -1;
    if (vk_create_render_pass()) return -1;
    if (vk_create_pipeline_objects()) return -1;
    if (vk_guest_init()) {
        VK_LOG("guest programs: setup failed, fixed-function path only\n");
        vk_guest_shutdown();
    }

    /* Both targets UNDEFINED -> GENERAL once. Colour starts opaque black so a
     * present before the first clear reads defined memory; depth starts at
     * the FAR plane, not zero -- zero is the near plane, and a guest that
     * enables LESS before its first depth clear would draw nothing (the
     * null backend makes the same choice). */
    if (vk_begin()) return -1;
    vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    vk_barrier_image(s_vk.depth.img, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearColorValue black = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };
    VkClearDepthStencilValue far_plane = { 1.0f, 0 };
    VkImageSubresourceRange cr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageSubresourceRange dr = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
    pvkCmdClearColorImage(s_vk.cmd, s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &cr);
    pvkCmdClearDepthStencilImage(s_vk.cmd, s_vk.depth.img, VK_IMAGE_LAYOUT_GENERAL, &far_plane, 1, &dr);
    if (vk_submit_and_wait()) return -1;

    if (s_vk.windowed) {
        VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (pvkCreateSemaphore(s_vk.device, &semi, NULL, &s_vk.sem_acquire) != VK_SUCCESS ||
            vk_create_swapchain())
            vk_window_drop("the swapchain could not be created");
    }
    return 0;
}

int rsx_vulkan_backend_init(u32 width, u32 height, const char* title)
{
    memset(&s_vk, 0, sizeof s_vk);
    if (vk_init_all(width, height, title)) {
        rsx_vulkan_backend_shutdown();
        return -1;
    }
    /* One path at a time: the FIFO walker feeds both the rsx_state vtable
     * and the register-file engine, so registering both would record every
     * draw twice (the rule the Metal backend's init follows).
     *
     * With guest programs on, the engine is the default -- as it became
     * Metal's once every host mode passed through it. It builds every
     * pipeline from the guest's programs, so without them it has nothing to
     * draw with, and the vtable's fixed-function path stays the default.
     * Either way PS3RECOMP_RSX_ENGINE=dispatch|vtable overrides. */
    rsx_draw_engine_set_backend(rsx_be_proxy_wrap(&s_vk_engine_backend));
    rsx_draw_engine_set_default(s_g.on ? 1 : 0);
    if (rsx_draw_engine_enabled() && rsx_draw_engine_init(s_vk.guest_w, s_vk.guest_h) == 0)
        s_vk.eng_active = 1;
    else
        rsx_set_backend(&s_vulkan_backend);
    VK_LOG("%s path\n", s_vk.eng_active ? "register-file draw engine" : "vtable");
    if (s_vk.windowed) rsx_host_ui_attach(s_vk.window, s_vk.max_aniso, s_rs);
    VK_LOG("offscreen target %ux%u ready, depth format %d (%s)\n",
           s_vk.width, s_vk.height, (int)s_vk.depth_format, title ? title : "");
    return 0;
}

void rsx_vulkan_backend_shutdown(void)
{
    if (rsx_get_backend() == &s_vulkan_backend) rsx_set_backend(NULL);
    if (s_vk.eng_active) { rsx_draw_engine_shutdown(); s_vk.eng_active = 0; }
    rsx_draw_engine_set_backend(NULL);
    if (s_vk.windowed && s_vk.swapchain && s_vk.presented && s_vk.hold_seconds > 0.0) {
        VK_LOG("holding the last frame on screen for %.1f s "
               "(Esc or closing the window ends it early)\n", s_vk.hold_seconds);
        const Uint32 end = SDL_GetTicks() + (Uint32)(s_vk.hold_seconds * 1000.0);
        while (s_vk.windowed && !SDL_TICKS_PASSED(SDL_GetTicks(), end)) {
            if (rsx_vulkan_backend_pump_messages() != 0) break;
            vk_present_window();         /* FIFO: paced by the display */
        }
    }
    if (s_vk.windowed || s_vk.window) vk_window_drop(NULL);
    if (s_vk.device) {
        vk_bt_shutdown();
        if (pvkDeviceWaitIdle) pvkDeviceWaitIdle(s_vk.device);
        vk_pp_shutdown();
        vk_guest_shutdown();
        for (int i = 0; i < VK_PIPELINE_SLOTS; i++)
            if (s_vk.pipelines[i]) pvkDestroyPipeline(s_vk.device, s_vk.pipelines[i], NULL);
        if (s_vk.pipe_layout) pvkDestroyPipelineLayout(s_vk.device, s_vk.pipe_layout, NULL);
        if (s_vk.desc_pool)   pvkDestroyDescriptorPool(s_vk.device, s_vk.desc_pool, NULL);
        if (s_vk.set_layout)  pvkDestroyDescriptorSetLayout(s_vk.device, s_vk.set_layout, NULL);
        if (s_vk.sampler)     pvkDestroySampler(s_vk.device, s_vk.sampler, NULL);
        for (u32 i = 0; i < s_vk.samp_count; i++)
            pvkDestroySampler(s_vk.device, s_vk.samp_cache[i].s, NULL);
        if (s_vk.vs)          pvkDestroyShaderModule(s_vk.device, s_vk.vs, NULL);
        if (s_vk.fs)          pvkDestroyShaderModule(s_vk.device, s_vk.fs, NULL);
        if (s_vk.framebuffer) pvkDestroyFramebuffer(s_vk.device, s_vk.framebuffer, NULL);
        if (s_vk.render_pass) pvkDestroyRenderPass(s_vk.device, s_vk.render_pass, NULL);
        for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) vk_destroy_image(&s_vk.tex[u]);
        vk_destroy_image(&s_vk.dummy);
        vk_destroy_image(&s_vk.depth);
        vk_destroy_image(&s_vk.color);
        vk_destroy_buffer(&s_vk.vertices);
        vk_destroy_buffer(&s_vk.readback);
        vk_pool_shutdown();
        if (s_vk.fence) pvkDestroyFence(s_vk.device, s_vk.fence, NULL);
        if (s_vk.pool)  pvkDestroyCommandPool(s_vk.device, s_vk.pool, NULL);
        pvkDestroyDevice(s_vk.device, NULL);
    }
    if (s_vk.instance && pvkDestroyInstance) pvkDestroyInstance(s_vk.instance, NULL);
    if (s_vk.lib) dlclose(s_vk.lib);
    free(s_vk.cpu_verts);
    memset(&s_vk, 0, sizeof s_vk);
}

int rsx_vulkan_backend_pump_messages(void)
{
    if (!s_vk.windowed) return 0;
    int quit = 0;
    SDL_Event e;
    rsx_host_ui_poll_script();
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) quit = 1;
        else if (rsx_host_ui_event(&e)) quit = 1;     /* "Quit game" in the menu */
    }
    return quit ? -1 : 0;
}

void rsx_vulkan_backend_present(void)
{
    if (s_vk.eng_active) rsx_draw_engine_present();
    else                 vk_cb_present(&s_vk, 0);
}

u32 rsx_vulkan_backend_debug_color(void) { return s_vk.clear_argb; }

u32 rsx_vulkan_backend_readback_center(void)
{
    if (s_vk.eng_active) return rsx_draw_engine_readback_center();
    return s_vk.presented ? (0xFF000000u | s_vk.last_center) : 0u;
}

u32 rsx_vulkan_backend_guest_draws(void)
{
    return s_vk.eng_active ? rsx_draw_engine_guest_draws() : s_g.last_draws;
}


