/* Somniel M0: open an SDL3 window, bring up Vulkan and clear the screen every
 * frame. Proves the toolchain, the Vulkan path and the debug plumbing. */

#include <vulkan/vulkan.h>
#include "volk.h" /* Vulkan calls go through volk pointers (gpu_driver.c) */

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_vulkan.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include "debug_log.h"
#include "telemetry.h"
#include "debug_socket.h"
#include "guest.h"
#include "ipc.h"
#include "nvn/vkr.h"
#include "overlay.h"
#include "thread.h"
void som_audio_pause_all(int on); /* services_audio.c */

extern float som_gfx_color[4];
extern uint64_t som_gfx_presents;
static bool g_spike_mode; /* draw a dark screen while the guest runs */

#define FRAMES_IN_FLIGHT 2
#define MAX_IMAGES 8

typedef struct Gfx {
    SDL_Window *window;
    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice phys;
    VkDevice dev;
    uint32_t family;
    VkQueue queue;
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkExtent2D extent;
    uint32_t image_count;
    VkImage images[MAX_IMAGES];
    VkSemaphore render_done[MAX_IMAGES];
    VkCommandPool pool;
    VkCommandBuffer cmd[FRAMES_IN_FLIGHT];
    VkSemaphore acquire_sem[FRAMES_IN_FLIGHT];
    VkFence fence[FRAMES_IN_FLIGHT];
    uint32_t frame;
} Gfx;

static bool vk_ok(VkResult r, const char *what) {
    if (r != VK_SUCCESS) {
        som_log("Vulkan error %d in %s", (int)r, what);
        return false;
    }
    return true;
}

static void destroy_swapchain(Gfx *g) {
    if (!g->dev) return;
    som_log("destroy_swapchain: wait idle");
    pthread_mutex_lock(&g_vkr.qmu); /* the renderer thread submits to the same queue */
    vkDeviceWaitIdle(g->dev);
    pthread_mutex_unlock(&g_vkr.qmu);
    som_log("destroy_swapchain: idle");
    for (uint32_t i = 0; i < g->image_count; i++) {
        if (g->render_done[i]) vkDestroySemaphore(g->dev, g->render_done[i], NULL);
        g->render_done[i] = VK_NULL_HANDLE;
    }
    if (g->swapchain) vkDestroySwapchainKHR(g->dev, g->swapchain, NULL);
    g->swapchain = VK_NULL_HANDLE;
    g->image_count = 0;
    som_log("destroy_swapchain: done");
}

static bool create_swapchain(Gfx *g) {
    VkSurfaceCapabilitiesKHR caps;
    if (!vk_ok(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g->phys, g->surface, &caps), "surface caps")) return false;

    uint32_t nfmt = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g->phys, g->surface, &nfmt, NULL);
    VkSurfaceFormatKHR fmts[32];
    if (nfmt > 32) nfmt = 32;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g->phys, g->surface, &nfmt, fmts);
    /* The game presents an sRGB image (R8G8B8A8_SRGB). The window blit decodes it to linear, so the swapchain must be
     * sRGB too to encode it back; into a UNORM swapchain every frame came out much darker. */
    VkSurfaceFormatKHR chosen = fmts[0];
    int rank = 0;
    for (uint32_t i = 0; i < nfmt; i++) {
        VkFormat f = fmts[i].format;
        int r = (f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB) ? 2
              : (f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_B8G8R8A8_UNORM) ? 1 : 0;
        if (r > rank) rank = r, chosen = fmts[i];
    }
    som_log("swapchain format %d (%s)", (int)chosen.format, rank == 2 ? "sRGB" : "not sRGB");

    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(g->window, &w, &h);
        ext.width = (uint32_t)w;
        ext.height = (uint32_t)h;
    }
    if (ext.width == 0 || ext.height == 0) return false;

    uint32_t want = caps.minImageCount + 1;
    if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;
    if (want > MAX_IMAGES) want = MAX_IMAGES;

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;

    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = g->surface;
    ci.minImageCount = want;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    /* The RP6 panel is natively portrait, so currentTransform is ROTATE_90: using it as preTransform means the app
     * must render pre-rotated, and the unrotated blit came out stretched and flipped. Ask for IDENTITY and let the
     * compositor rotate (one extra composition pass). */
    ci.preTransform = caps.currentTransform;
    if ((caps.currentTransform & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)) &&
        (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)) {
        /* currentExtent is already the landscape window size here, keep it */
        ci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    }
    som_log("swapchain %ux%u transform cur 0x%x used 0x%x", ext.width, ext.height, (unsigned)caps.currentTransform,
            (unsigned)ci.preTransform);
    ci.compositeAlpha = alpha;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    if (!vk_ok(vkCreateSwapchainKHR(g->dev, &ci, NULL, &g->swapchain), "vkCreateSwapchainKHR")) return false;

    g->format = chosen.format;
    g->extent = ext;
    g->image_count = MAX_IMAGES;
    vkGetSwapchainImagesKHR(g->dev, g->swapchain, &g->image_count, g->images);

    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < g->image_count; i++)
        if (!vk_ok(vkCreateSemaphore(g->dev, &si, NULL, &g->render_done[i]), "render semaphore")) return false;

    som_log("swapchain: %ux%u, %u images, format %d", ext.width, ext.height, g->image_count, (int)chosen.format);
    return true;
}

/* The surface is made with our own Vulkan (volk, possibly a custom driver), not SDL's copy of the system loader. */
static bool make_surface(Gfx *g) {
    struct ANativeWindow *nw = SDL_GetPointerProperty(SDL_GetWindowProperties(g->window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, NULL);
    if (!nw) {
        som_log("no Android window for the surface");
        return false;
    }
    VkAndroidSurfaceCreateInfoKHR ci = {VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    ci.window = nw;
    return vk_ok(vkCreateAndroidSurfaceKHR(g->instance, &ci, NULL, &g->surface), "vkCreateAndroidSurfaceKHR");
}

static bool create_surface_and_swapchain(Gfx *g) {
    if (!make_surface(g)) return false;
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(g->phys, g->family, g->surface, &supported);
    if (!supported) {
        som_log("queue family cannot present to the new surface");
        return false;
    }
    return create_swapchain(g);
}

static void destroy_surface(Gfx *g) {
    destroy_swapchain(g);
    if (g->surface) vkDestroySurfaceKHR(g->instance, g->surface, NULL);
    g->surface = VK_NULL_HANDLE;
    som_log("destroy_surface: done");
}

/* Android destroys the native window as soon as the activity pauses, while SDL is still delivering the event: the
 * surface must be gone before the watch below returns, and never be used by the render loop at the same time. */
static pthread_mutex_t g_gfx_mu = PTHREAD_MUTEX_INITIALIZER;
static Gfx *g_gfx;
static volatile bool g_background;
static volatile bool g_want_surface; /* set on foreground: the render loop rebuilds the surface */

static bool lifecycle_watch(void *ud, SDL_Event *e) {
    (void)ud;
    /* SDL hands the lifecycle events to this watch but they never reach SDL_PollEvent in the main loop, so the
     * whole background/foreground state machine lives here. */
    if (e->type == SDL_EVENT_WILL_ENTER_BACKGROUND && g_gfx) {
        pthread_mutex_lock(&g_gfx_mu);
        som_log("entering background (watch)");
        g_background = true;
        g_want_surface = false;
        destroy_surface(g_gfx);
        pthread_mutex_unlock(&g_gfx_mu);
        if (g_spike_mode) { /* the guest only exists in game mode */
            som_guest_pause_all(1);
            som_audio_pause_all(1);
        }
    } else if (e->type == SDL_EVENT_DID_ENTER_FOREGROUND && g_gfx) {
        som_log("entering foreground (watch)");
        if (g_spike_mode) {
            som_audio_pause_all(0);
            som_guest_pause_all(0);
        }
        g_want_surface = true;
    }
    return true;
}

PFN_vkGetInstanceProcAddr gpu_driver_open(void); /* gpu_driver.c */

static bool init_vulkan(Gfx *g) {
    PFN_vkGetInstanceProcAddr gipa = gpu_driver_open();
    if (!gipa) return false;
    volkInitializeCustom(gipa);
    static const char *exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};

    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Somniel";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = exts;
    if (!vk_ok(vkCreateInstance(&ici, NULL, &g->instance), "vkCreateInstance")) return false;
    volkLoadInstance(g->instance);

    if (!make_surface(g)) return false;

    uint32_t nphys = 0;
    vkEnumeratePhysicalDevices(g->instance, &nphys, NULL);
    if (!nphys) {
        som_log("no Vulkan physical devices");
        return false;
    }
    VkPhysicalDevice phys[8];
    if (nphys > 8) nphys = 8;
    vkEnumeratePhysicalDevices(g->instance, &nphys, phys);

    g->phys = VK_NULL_HANDLE;
    for (uint32_t d = 0; d < nphys && !g->phys; d++) {
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys[d], &nq, NULL);
        VkQueueFamilyProperties qp[16];
        if (nq > 16) nq = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(phys[d], &nq, qp);
        for (uint32_t q = 0; q < nq; q++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(phys[d], q, g->surface, &present);
            if ((qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                g->phys = phys[d];
                g->family = q;
                break;
            }
        }
    }
    if (!g->phys) {
        som_log("no graphics+present queue family");
        return false;
    }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g->phys, &props);
    som_log("GPU: %s, Vulkan %u.%u.%u, driver 0x%x, vendor 0x%x", props.deviceName,
            VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
            VK_VERSION_PATCH(props.apiVersion), props.driverVersion, props.vendorID);
    tel_set_gpu(props.deviceName, props.driverVersion, props.vendorID, VK_VERSION_MAJOR(props.apiVersion),
                VK_VERSION_MINOR(props.apiVersion));

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = g->family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    uint32_t next_dev = 0;
    vkEnumerateDeviceExtensionProperties(g->phys, NULL, &next_dev, NULL);
    VkExtensionProperties *dprops = calloc(next_dev ? next_dev : 1, sizeof *dprops);
    vkEnumerateDeviceExtensionProperties(g->phys, NULL, &next_dev, dprops);
    bool has_dcc = false, has_pcc = false, has_dem = false;
    for (uint32_t i = 0; i < next_dev; i++) {
        if (!strcmp(dprops[i].extensionName, VK_EXT_SHADER_DEMOTE_TO_HELPER_INVOCATION_EXTENSION_NAME)) has_dem = true;
        if (!strcmp(dprops[i].extensionName, VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME)) has_dcc = true;
        if (!strcmp(dprops[i].extensionName, VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME)) has_pcc = true;
    }
    free(dprops);
    VkPhysicalDevicePipelineCreationCacheControlFeaturesEXT pccf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES_EXT};
    if (has_pcc) {
        VkPhysicalDeviceFeatures2 q = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &pccf};
        vkGetPhysicalDeviceFeatures2(g->phys, &q);
        has_pcc = pccf.pipelineCreationCacheControl;
        pccf.pNext = NULL;
    }
    /* Translated fragment shaders demote a discarded pixel to a helper invocation (the translator's SpirvGenerator):
     * the pixels beside it keep valid derivatives, as on the console. */
    VkPhysicalDeviceShaderDemoteToHelperInvocationFeaturesEXT demf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES_EXT};
    if (has_dem) {
        VkPhysicalDeviceFeatures2 q = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &demf};
        vkGetPhysicalDeviceFeatures2(g->phys, &q);
        has_dem = demf.shaderDemoteToHelperInvocation;
        demf.pNext = NULL;
    }
    som_log("Vulkan device: shader demote to helper invocation %s", has_dem ? "on" : "UNSUPPORTED (shaders that discard will not load)");
    const char *dev_exts[4] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    uint32_t n_exts = 1;
    if (has_dem) dev_exts[n_exts++] = VK_EXT_SHADER_DEMOTE_TO_HELPER_INVOCATION_EXTENSION_NAME;
    if (has_dcc) dev_exts[n_exts++] = VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME;
    if (has_pcc) dev_exts[n_exts++] = VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME;
    VkPhysicalDeviceDepthClipControlFeaturesEXT dccf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
    dccf.depthClipControl = VK_TRUE;
    dccf.pNext = has_pcc ? &pccf : NULL;
    VkPhysicalDeviceFeatures pf;
    vkGetPhysicalDeviceFeatures(g->phys, &pf);
    VkPhysicalDeviceFeatures want = {0};
    /* <data>/no_robust: experiment, without bounds checks on buffer reads (a cost on every vertex/buffer fetch) */
    {
        char rp[600];
        const char *ext = SDL_GetAndroidExternalStoragePath();
        snprintf(rp, sizeof rp, "%s/no_robust", ext ? ext : ".");
        FILE *nf = fopen(rp, "r");
        want.robustBufferAccess = nf ? VK_FALSE : pf.robustBufferAccess;
        if (nf) fclose(nf);
        som_log("Vulkan device: robustBufferAccess %s", want.robustBufferAccess ? "on" : "off");
    }
    want.independentBlend = pf.independentBlend;
    want.depthClamp = pf.depthClamp;
    want.fillModeNonSolid = pf.fillModeNonSolid;
    want.samplerAnisotropy = pf.samplerAnisotropy;
    want.textureCompressionBC = pf.textureCompressionBC;
    want.textureCompressionASTC_LDR = pf.textureCompressionASTC_LDR;
    want.dualSrcBlend = pf.dualSrcBlend;
    want.imageCubeArray = pf.imageCubeArray;
    /* translated shaders use these (validation: ImageGatherExtended, clip/cull distances, int16/float64 never) */
    want.shaderImageGatherExtended = pf.shaderImageGatherExtended;
    want.shaderClipDistance = pf.shaderClipDistance;
    want.shaderCullDistance = pf.shaderCullDistance;
    want.multiDrawIndirect = pf.multiDrawIndirect;
    want.drawIndirectFirstInstance = pf.drawIndirectFirstInstance; /* occlusion culling (vkr.c): indirect draws with a base instance */
    want.shaderStorageImageExtendedFormats = pf.shaderStorageImageExtendedFormats;
    want.fragmentStoresAndAtomics = pf.fragmentStoresAndAtomics;
    want.vertexPipelineStoresAndAtomics = pf.vertexPipelineStoresAndAtomics;
    want.geometryShader = pf.geometryShader;
    /* translated vertex shaders declare DrawParameters (base vertex / instance) */
    VkPhysicalDeviceShaderDrawParametersFeatures sdp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &sdp};
    vkGetPhysicalDeviceFeatures2(g->phys, &f2);
    sdp.pNext = has_dcc ? (void *)&dccf : has_pcc ? (void *)&pccf : NULL;
    if (has_dem) demf.pNext = sdp.pNext, sdp.pNext = &demf;
    som_log("Vulkan device: shaderDrawParameters %s", sdp.shaderDrawParameters ? "on" : "unsupported");
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &sdp;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = n_exts;
    dci.ppEnabledExtensionNames = dev_exts;
    dci.pEnabledFeatures = &want;
    if (!vk_ok(vkCreateDevice(g->phys, &dci, NULL, &g->dev), "vkCreateDevice")) return false;
    volkLoadDevice(g->dev); /* direct device calls, without the loader's dispatch */
    som_log("Vulkan device: depth clip control %s, pipeline cache control %s", has_dcc ? "on" : "off", has_pcc ? "on" : "off");
    g_vkr.pipe_cache_control = has_pcc;
    vkGetDeviceQueue(g->dev, g->family, 0, &g->queue);
    pthread_mutex_init(&g_vkr.qmu, NULL);
    g_vkr.instance = g->instance, g_vkr.phys = g->phys, g_vkr.dev = g->dev, g_vkr.family = g->family, g_vkr.queue = g->queue;
    g_vkr.depth_clip_control = has_dcc;
    {
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(g->phys, &pp);
        g_vkr.max_aniso = pf.samplerAnisotropy ? pp.limits.maxSamplerAnisotropy : 1.0f;
        g_vkr.sbuf_align = (uint32_t)pp.limits.minStorageBufferOffsetAlignment;
        VkPhysicalDeviceSubgroupProperties sgp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        VkPhysicalDeviceProperties2 pp2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &sgp};
        vkGetPhysicalDeviceProperties2(g->phys, &pp2);
        g_vkr.subgroup_size = sgp.subgroupSize;
        g_vkr.clip_distance = pf.shaderClipDistance;
        som_log("Vulkan device: %s, api %u.%u.%u, subgroup size %u, storage buffer alignment %u, BC %s, clip distance %s", pp.deviceName,
                VK_API_VERSION_MAJOR(pp.apiVersion), VK_API_VERSION_MINOR(pp.apiVersion), VK_API_VERSION_PATCH(pp.apiVersion),
                g_vkr.subgroup_size, g_vkr.sbuf_align, pf.textureCompressionBC ? "yes" : "NO", pf.shaderClipDistance ? "yes" : "NO");
    }
    __atomic_store_n(&g_vkr.ready, 1, __ATOMIC_RELEASE);

    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g->family;
    if (!vk_ok(vkCreateCommandPool(g->dev, &pci, NULL, &g->pool), "command pool")) return false;
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = g->pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = FRAMES_IN_FLIGHT;
    if (!vk_ok(vkAllocateCommandBuffers(g->dev, &cai, g->cmd), "command buffers")) return false;

    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        if (!vk_ok(vkCreateSemaphore(g->dev, &si, NULL, &g->acquire_sem[i]), "acquire semaphore")) return false;
        if (!vk_ok(vkCreateFence(g->dev, &fi, NULL, &g->fence[i]), "fence")) return false;
    }
    return create_swapchain(g);
}

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
                          VkAccessFlags src_access, VkAccessFlags dst_access,
                          VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* Returns false when the swapchain must be rebuilt. */
static bool draw_frame(Gfx *g, double t) {
    uint32_t f = g->frame % FRAMES_IN_FLIGHT;
    vkWaitForFences(g->dev, 1, &g->fence[f], VK_TRUE, UINT64_MAX);

    uint32_t idx;
    VkResult r = vkAcquireNextImageKHR(g->dev, g->swapchain, UINT64_MAX, g->acquire_sem[f], VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR) return false;
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        vk_ok(r, "acquire");
        return false;
    }
    vkResetFences(g->dev, 1, &g->fence[f]);

    VkCommandBuffer cb = g->cmd[f];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    image_barrier(cb, g->images[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue col = {{0.5f + 0.5f * (float)sin(t * 1.0), 0.5f + 0.5f * (float)sin(t * 1.3 + 2.0),
                              0.5f + 0.5f * (float)sin(t * 1.7 + 4.0), 1.0f}};
    if (g_spike_mode && __atomic_load_n(&som_gfx_presents, __ATOMIC_RELAXED)) {
        /* the game is presenting: show the colour it last cleared the presented texture to (skeleton display) */
        col = (VkClearColorValue){{som_gfx_color[0], som_gfx_color[1], som_gfx_color[2], 1.0f}};
        extern int som_keep_aspect(void);
        if (som_keep_aspect()) col = (VkClearColorValue){{0.0f, 0.0f, 0.0f, 1.0f}}; /* the letterbox bars */
    } else if (g_spike_mode && t < 120.0) {
        /* boot: navy behind the Java splash overlay (sRGB swapchain, so linear values of #1B2545), never the colour cycle */
        col = (VkClearColorValue){{0.011f, 0.018f, 0.060f, 1.0f}};
    } else if (g_spike_mode) {
        /* Burn-in care for OLED panels: true black (pixels off) while no real frames are shown, and every
         * CLEAN_PERIOD seconds a CLEAN_LENGTH second cycle of full-screen colours to even out pixel wear. */
        const double CLEAN_PERIOD = 1200.0, CLEAN_LENGTH = 30.0;
        double cyc = fmod(t, CLEAN_PERIOD);
        if (cyc < CLEAN_LENGTH) {
            static const float k[6][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 1}, {1, 1, 0}, {0, 1, 1}};
            const float *s = k[(int)(cyc / 5.0) % 6];
            col = (VkClearColorValue){{s[0], s[1], s[2], 1.0f}};
        } else {
            col = (VkClearColorValue){{0.0f, 0.0f, 0.0f, 1.0f}};
        }
    }
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, g->images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &col, 1, &range);
    if (g_spike_mode && vkr_blit_present(cb, g->images[idx], g->extent)) overlay_draw(cb, g->images[idx], g->format, g->extent, f);
    image_barrier(cb, g->images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    vkEndCommandBuffer(cb);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo sub = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    sub.waitSemaphoreCount = 1;
    sub.pWaitSemaphores = &g->acquire_sem[f];
    sub.pWaitDstStageMask = &wait_stage;
    sub.commandBufferCount = 1;
    sub.pCommandBuffers = &cb;
    sub.signalSemaphoreCount = 1;
    sub.pSignalSemaphores = &g->render_done[idx];
    static uint64_t wl_sub_ns, wl_pres_ns, wl_n, wl_t0;
    struct timespec wa, wb, wc, wp;
    clock_gettime(CLOCK_MONOTONIC, &wa);
    pthread_mutex_lock(&g_vkr.qmu);
    VkResult sr = vkQueueSubmit(g->queue, 1, &sub, g->fence[f]);
    pthread_mutex_unlock(&g_vkr.qmu);
    clock_gettime(CLOCK_MONOTONIC, &wb);
    if (!vk_ok(sr, "vkQueueSubmit")) return false;
    {   /* Mali: wait for the copy to finish before presenting, without the queue lock. The Mali driver otherwise waits
         * for it inside vkQueuePresentKHR (20-27 ms on the Galaxy Tab A11+) with the lock held; the next copy queued late
         * and frames reached the screen in pairs, one refresh apart, then a long gap. Adreno: unchanged. */
        extern int som_gpu_is_mali(void);
        static int wait_copy = -1;
        if (wait_copy < 0) wait_copy = som_gpu_is_mali();
        if (g_spike_mode && wait_copy) vkWaitForFences(g->dev, 1, &g->fence[f], VK_TRUE, 200000000ull);
    }
    clock_gettime(CLOCK_MONOTONIC, &wp);

    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g->render_done[idx];
    pi.swapchainCount = 1;
    pi.pSwapchains = &g->swapchain;
    pi.pImageIndices = &idx;
    pthread_mutex_lock(&g_vkr.qmu);
    r = vkQueuePresentKHR(g->queue, &pi);
    pthread_mutex_unlock(&g_vkr.qmu);
    clock_gettime(CLOCK_MONOTONIC, &wc);
    {   /* how long the window thread holds the queue lock (the renderer's submit waits for it) */
        uint64_t a = (uint64_t)wa.tv_sec * 1000000000ull + (uint64_t)wa.tv_nsec, b = (uint64_t)wb.tv_sec * 1000000000ull + (uint64_t)wb.tv_nsec;
        uint64_t cc = (uint64_t)wc.tv_sec * 1000000000ull + (uint64_t)wc.tv_nsec;
        uint64_t pp = (uint64_t)wp.tv_sec * 1000000000ull + (uint64_t)wp.tv_nsec;
        wl_sub_ns += b - a, wl_pres_ns += cc - pp, wl_n++;
        if (!wl_t0) wl_t0 = cc;
        if (cc - wl_t0 >= 5000000000ull) {
            som_log("window: %.1f presents/s, queue lock held per present: submit %.2f ms, present %.2f ms", wl_n / ((cc - wl_t0) / 1e9),
                    wl_sub_ns / 1e6 / wl_n, wl_pres_ns / 1e6 / wl_n);
            wl_sub_ns = wl_pres_ns = wl_n = 0, wl_t0 = cc;
        }
    }
    g->frame++;
    /* SUBOPTIMAL is expected every frame: we present with IDENTITY on a rotated panel on purpose */
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR) return false;
    return r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
}

/* ---- input: SDL gamepads (and a keyboard fallback) -> the game's one Switch controller (hooks_hid.c) ----
 * Face buttons go by position, as on a Switch: right = A, bottom = B, top = X, left = Y. */
void som_hid_set_pad(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);
void som_hid_touch(int64_t finger, float nx, float ny, int phase, int64_t now_ns); /* hooks_hid.c */

enum {
    NP_A = 1 << 0, NP_B = 1 << 1, NP_X = 1 << 2, NP_Y = 1 << 3, NP_LS = 1 << 4, NP_RS = 1 << 5, NP_L = 1 << 6, NP_R = 1 << 7,
    NP_ZL = 1 << 8, NP_ZR = 1 << 9, NP_PLUS = 1 << 10, NP_MINUS = 1 << 11, NP_LEFT = 1 << 12, NP_UP = 1 << 13,
    NP_RIGHT = 1 << 14, NP_DOWN = 1 << 15, NP_LS_LEFT = 1 << 16, NP_LS_UP = 1 << 17, NP_LS_RIGHT = 1 << 18,
    NP_LS_DOWN = 1 << 19, NP_RS_LEFT = 1 << 20, NP_RS_UP = 1 << 21, NP_RS_RIGHT = 1 << 22, NP_RS_DOWN = 1 << 23
};

static SDL_Gamepad *g_pads[4];
static uint64_t g_key_buttons; /* from the keyboard fallback */
/* adb input keyevent sends down and up together: hold every key at least 100 ms so the game's polling sees it */
static uint64_t g_key_release;
static Uint64 g_key_down_at[24];

static void input_event(const SDL_Event *e) {
    if (e->type == SDL_EVENT_GAMEPAD_ADDED) {
        SDL_Gamepad *p = SDL_OpenGamepad(e->gdevice.which);
        for (int i = 0; p && i < 4; i++)
            if (!g_pads[i]) {
                g_pads[i] = p;
                break;
            }
        som_log("input: gamepad added: %s", p ? SDL_GetGamepadName(p) : SDL_GetError());
        return;
    }
    if (e->type == SDL_EVENT_GAMEPAD_REMOVED) {
        for (int i = 0; i < 4; i++)
            if (g_pads[i] && SDL_GetGamepadID(g_pads[i]) == e->gdevice.which) {
                som_log("input: gamepad removed: %s", SDL_GetGamepadName(g_pads[i]));
                SDL_CloseGamepad(g_pads[i]);
                g_pads[i] = NULL;
            }
        return;
    }
    uint64_t bit = 0;
    switch (e->key.key) {
    case SDLK_RETURN: case SDLK_SPACE: case SDLK_Z: bit = NP_A; break;
    case SDLK_ESCAPE: case SDLK_BACKSPACE: case SDLK_X: case SDLK_AC_BACK: bit = NP_B; break;
    case SDLK_C: bit = NP_X; break;
    case SDLK_V: bit = NP_Y; break;
    case SDLK_Q: bit = NP_L; break;
    case SDLK_E: bit = NP_R; break;
    case SDLK_UP: bit = NP_UP; break;
    case SDLK_DOWN: bit = NP_DOWN; break;
    case SDLK_LEFT: bit = NP_LEFT; break;
    case SDLK_RIGHT: bit = NP_RIGHT; break;
    case SDLK_TAB: bit = NP_PLUS; break;
    case SDLK_MINUS: bit = NP_MINUS; break;
    default: break;
    }
    if (!bit) return;
    int k = __builtin_ctzll(bit);
    if (e->type == SDL_EVENT_KEY_DOWN) {
        g_key_buttons |= bit;
        g_key_release &= ~bit;
        g_key_down_at[k] = SDL_GetTicks();
    } else {
        g_key_release |= bit; /* released by input_update once it was held long enough to be seen */
    }
}

/* Scripted input for automated testing: write <external files>/input.txt (adb push) and the app plays it once, from
 * the top, whenever its modification time changes. One step per line: a button name (A B X Y L R ZL ZR PLUS MINUS UP
 * DOWN LEFT RIGHT) with an optional hold time in ms (default 200; a 200 ms gap follows: the game polls once a frame), or "wait <ms>". */
#include <sys/stat.h>
/* sticks: LS_UP LS_DOWN LS_LEFT LS_RIGHT and RS_* push that stick fully for the hold time (camera, walking) */
typedef struct { uint64_t bits; uint32_t ms; int8_t lx, ly, rx, ry; } InStep;
static InStep g_script[512];
static int g_nscript, g_script_pos = -1;
static Uint64 g_step_end;

static void script_poll(void) {
    static Uint64 next_check;
    static time_t mtime;
    Uint64 now = SDL_GetTicks();
    if (now >= next_check) {
        next_check = now + 500;
        const char *ext = SDL_GetAndroidExternalStoragePath();
        char p[600];
        struct stat st;
        snprintf(p, sizeof p, "%s/input.txt", ext ? ext : ".");
        if (!stat(p, &st) && st.st_mtime != mtime) {
            int first = !mtime;
            mtime = st.st_mtime;
            if (!(first && time(NULL) - st.st_mtime > 10)) { /* ignore a script left from an earlier run */
                FILE *f = fopen(p, "r");
                static const struct { const char *n; uint64_t b; } names[] = {
                    {"A", NP_A}, {"B", NP_B}, {"X", NP_X}, {"Y", NP_Y}, {"L", NP_L}, {"R", NP_R}, {"ZL", NP_ZL}, {"ZR", NP_ZR},
                    {"PLUS", NP_PLUS}, {"MINUS", NP_MINUS}, {"UP", NP_UP}, {"DOWN", NP_DOWN}, {"LEFT", NP_LEFT}, {"RIGHT", NP_RIGHT}};
                char w[32];
                unsigned ms;
                g_nscript = 0;
                while (f && g_nscript < 510 && fscanf(f, "%31s", w) == 1) {
                    if (!strcmp(w, "wait")) {
                        if (fscanf(f, "%u", &ms) == 1) g_script[g_nscript++] = (InStep){0, ms};
                        continue;
                    }
                    uint64_t b = 0;
                    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
                        if (!strcmp(w, names[i].n)) b = names[i].b;
                    InStep st = {b, 0, 0, 0, 0, 0};
                    if ((w[0] == 'L' || w[0] == 'R') && w[1] == 'S' && w[2] == '_') {
                        int8_t *x = w[0] == 'L' ? &st.lx : &st.rx, *y = w[0] == 'L' ? &st.ly : &st.ry;
                        if (!strcmp(w + 3, "UP")) *y = 1;
                        else if (!strcmp(w + 3, "DOWN")) *y = -1;
                        else if (!strcmp(w + 3, "LEFT")) *x = -1;
                        else if (!strcmp(w + 3, "RIGHT")) *x = 1;
                    }
                    long pos = f ? ftell(f) : 0;
                    if (fscanf(f, "%u", &ms) != 1) ms = 200, fseek(f, pos, SEEK_SET);
                    st.ms = ms;
                    if (b || st.lx || st.ly || st.rx || st.ry) g_script[g_nscript++] = st, g_script[g_nscript++] = (InStep){0, 200};
                }
                if (f) fclose(f);
                g_script_pos = g_nscript ? 0 : -1;
                g_step_end = now + (g_nscript ? g_script[0].ms : 0);
                som_log("input: playing a %d-step script", g_nscript);
            }
        }
    }
    while (g_script_pos >= 0 && now >= g_step_end) {
        if (++g_script_pos >= g_nscript) {
            g_script_pos = -1;
            som_log("input: script done");
            break;
        }
        g_step_end = now + g_script[g_script_pos].ms;
    }
}

/* On-screen controls (TouchControls.java): written from the UI thread, read once per frame. A tap shorter than a frame
 * is latched so the game's polling still sees it. */
static volatile uint64_t g_touch_buttons, g_touch_latch;
static volatile int32_t g_touch_axes[4];

void som_touch_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    g_touch_buttons = buttons;
    g_touch_latch |= buttons;
    g_touch_axes[0] = lx; g_touch_axes[1] = ly; g_touch_axes[2] = rx; g_touch_axes[3] = ry;
}

static int32_t stick(int16_t v, int invert) {
    int32_t s = invert ? -(int32_t)v : (int32_t)v;
    if (s < -32767) s = -32767;
    if (s > 32767) s = 32767;
    return (s > -2500 && s < 2500) ? 0 : s; /* small dead zone: Switch sticks rest at exactly 0 */
}

/* Face buttons. Android reports them by their printed label (KEYCODE_BUTTON_A is the button marked A), and SDL files
 * BUTTON_A under SOUTH, B under EAST, X under WEST, Y under NORTH. Handhelds with a Nintendo layout (Retroid Pocket 6
 * set to Nintendo, AYN Thor) send the keycode of the label, so mapping by label puts every face button where its
 * label says. A controller that reports positions instead (an Xbox pad wanted in the Switch positions) can opt in
 * with a file <data>/pad_layout whose first word is "positional"; it is read once at start. */
static int face_positional(void) {
    static int v = -1;
    if (v < 0) {
        char p[600], w[16] = "";
        const char *ext = SDL_GetAndroidExternalStoragePath();
        snprintf(p, sizeof p, "%s/pad_layout", ext ? ext : ".");
        FILE *f = fopen(p, "r");
        if (f) { if (fscanf(f, "%15s", w) != 1) w[0] = 0; fclose(f); }
        v = !strcmp(w, "positional");
        som_log("input: face buttons mapped by %s", v ? "position (pad_layout)" : "label");
    }
    return v;
}

extern volatile int g_sidebar_open; /* sidebar.c */
extern volatile int g_text_input_open; /* hooks_swkbd.c */

static void input_update(void) {
    static const struct { SDL_GamepadButton b; uint64_t np; } face_label[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, NP_A}, {SDL_GAMEPAD_BUTTON_EAST, NP_B}, {SDL_GAMEPAD_BUTTON_WEST, NP_X},
        {SDL_GAMEPAD_BUTTON_NORTH, NP_Y},
    }, face_pos[] = {
        {SDL_GAMEPAD_BUTTON_EAST, NP_A}, {SDL_GAMEPAD_BUTTON_SOUTH, NP_B}, {SDL_GAMEPAD_BUTTON_NORTH, NP_X},
        {SDL_GAMEPAD_BUTTON_WEST, NP_Y},
    };
    static const struct { SDL_GamepadButton b; uint64_t np; } map[] = {
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, NP_LS}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, NP_RS},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, NP_L}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, NP_R},
        {SDL_GAMEPAD_BUTTON_START, NP_PLUS}, {SDL_GAMEPAD_BUTTON_BACK, NP_MINUS},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, NP_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_UP, NP_UP},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, NP_RIGHT}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, NP_DOWN},
    };
    for (int k = 0; k < 24; k++)
        if ((g_key_release >> k & 1) && SDL_GetTicks() - g_key_down_at[k] >= 100) {
            g_key_buttons &= ~(1ull << k);
            g_key_release &= ~(1ull << k);
        }
    script_poll();
    uint64_t buttons = g_key_buttons | (g_script_pos >= 0 ? g_script[g_script_pos].bits : 0);
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    for (int i = 0; i < 4; i++) {
        SDL_Gamepad *p = g_pads[i];
        if (!p) continue;
        for (size_t k = 0; k < sizeof map / sizeof map[0]; k++)
            if (SDL_GetGamepadButton(p, map[k].b)) buttons |= map[k].np;
        for (size_t k = 0; k < 4; k++) {
            SDL_GamepadButton fb = face_positional() ? face_pos[k].b : face_label[k].b;
            if (SDL_GetGamepadButton(p, fb)) buttons |= face_label[k].np;
        }
        if (SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) buttons |= NP_ZL;
        if (SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) buttons |= NP_ZR;
        int32_t a = stick(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX), 0), b = stick(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY), 1);
        int32_t c = stick(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTX), 0), d = stick(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTY), 1);
        if (!lx && !ly) lx = a, ly = b;
        if (!rx && !ry) rx = c, ry = d;
    }
    {
        uint64_t tb = g_touch_buttons | g_touch_latch;
        g_touch_latch = g_touch_buttons; /* a tap is kept for the frame it was pressed in */
        buttons |= tb;
        if (!lx && !ly) lx = g_touch_axes[0], ly = g_touch_axes[1];
        if (!rx && !ry) rx = g_touch_axes[2], ry = g_touch_axes[3];
    }
    if (g_script_pos >= 0) {
        const InStep *s = &g_script[g_script_pos];
        if (s->lx || s->ly) lx = s->lx * 32767, ly = s->ly * 32767;
        if (s->rx || s->ry) rx = s->rx * 32767, ry = s->ry * 32767;
    }
    const int32_t t = 16384; /* the stick-as-direction bits, at half travel */
    if (lx < -t) buttons |= NP_LS_LEFT;
    if (lx > t) buttons |= NP_LS_RIGHT;
    if (ly > t) buttons |= NP_LS_UP;
    if (ly < -t) buttons |= NP_LS_DOWN;
    if (rx < -t) buttons |= NP_RS_LEFT;
    if (rx > t) buttons |= NP_RS_RIGHT;
    if (ry > t) buttons |= NP_RS_UP;
    if (ry < -t) buttons |= NP_RS_DOWN;
    /* Debug: holding both stick clicks (L3 + R3) for half a second starts a frame capture (touches <data>/dump_rt,
     * which vkr.c watches), so a broken view can be captured from the controller while it is on screen. */
    {
        static uint64_t held_since;
        uint64_t nowt = SDL_GetTicks();
        if ((buttons & (NP_LS | NP_RS)) == (NP_LS | NP_RS)) {
            if (!held_since) held_since = nowt;
            else if (held_since != 1 && nowt - held_since >= 500) {
                char p[600];
                const char *ext = SDL_GetAndroidExternalStoragePath();
                snprintf(p, sizeof p, "%s/dump_rt", ext ? ext : ".");
                FILE *f = fopen(p, "w");
                if (f) fclose(f);
                utime(p, NULL);
                som_log("input: L3+R3 held: frame capture requested");
                held_since = 1; /* once per hold */
            }
        } else held_since = 0;
    }
    static uint64_t last = ~0ull;
    if (buttons != last) {
        static int n;
        if (n++ < 30) som_log("input: buttons %#llx", (unsigned long long)buttons);
        last = buttons;
    }
    if (g_sidebar_open || g_text_input_open) buttons = 0, lx = ly = rx = ry = 0; /* the sidebar has the controls; the game sees neutral */
    som_hid_set_pad(buttons, lx, ly, rx, ry);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    Gfx g;
    memset(&g, 0, sizeof g);

    {   /* <external files>/log_external: write somniel.log (and the other logs) next to the saves, where adb can read them
         * on a release build (no run-as) */
        const char *log_dir = SDL_GetAndroidInternalStoragePath(), *e = SDL_GetAndroidExternalStoragePath();
        char f[512];
        if (e && snprintf(f, sizeof f, "%s/log_external", e) > 0 && access(f, F_OK) == 0) log_dir = e;
        som_log_init(log_dir);
    }
    som_log("Somniel M0 starting (built %s %s)", __DATE__, __TIME__);
    som_debug_socket_start();

    /* M1 spike: if <external files>/spike.on exists, run the NSOs in <external files>/exefs */
    const char *ext = SDL_GetAndroidExternalStoragePath();
    if (ext) {
        char flag[512], exefs[512], gdir[400] = "";
        snprintf(flag, sizeof flag, "%s/spike.on", ext);
        services_set_data_dir(ext); /* saves, flags and caches stay here whatever volume the game files are on */
        {   /* <ext>/game_dir: the folder holding exefs/ and romfs.bin when they were installed elsewhere (an SD card) */
            char gp[512];
            snprintf(gp, sizeof gp, "%s/game_dir", ext);
            FILE *gf = fopen(gp, "r");
            if (gf) {
                if (!fgets(gdir, sizeof gdir, gf)) gdir[0] = 0;
                fclose(gf);
                gdir[strcspn(gdir, "\r\n")] = 0;
            }
            char probe[600];
            snprintf(probe, sizeof probe, "%s/exefs/main", gdir);
            if (gdir[0] && access(probe, R_OK) == 0) services_set_game_dir(gdir);
            else if (gdir[0]) som_log("game_dir %s has no exefs/main (card removed?): using %s", gdir, ext);
        }
        snprintf(exefs, sizeof exefs, "%s/exefs", som_game_dir());
        FILE *f = fopen(flag, "rb");
        if (f) {
            fclose(f);
            g_spike_mode = true;
            if (spike_start(exefs) != 0) som_log("spike failed to start");
        }
    }

    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1"); /* Back is the game's B, not "close the app" */
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
        som_log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    g.window = SDL_CreateWindow("Somniel", 1920, 1080, SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN);
    if (!g.window) {
        som_log("SDL_CreateWindow failed: %s", SDL_GetError());
        return 1;
    }
    if (!init_vulkan(&g)) return 1;
    g_gfx = &g;
    SDL_AddEventWatch(lifecycle_watch, NULL);

    bool running = true;
    Uint64 freq = SDL_GetPerformanceFrequency();
    Uint64 start = SDL_GetPerformanceCounter(), last = start, win_start = start;
    unsigned win_frames = 0;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
            case SDL_EVENT_GAMEPAD_REMOVED:
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                input_event(&e);
                break;
            case SDL_EVENT_FINGER_DOWN:
            case SDL_EVENT_FINGER_MOTION:
            case SDL_EVENT_FINGER_UP:
            case SDL_EVENT_FINGER_CANCELED:
            {   /* screen fractions to the picture's own (letterboxed by the Aspect ratio option: vkr_blit_present) */
                extern void vkr_present_rect(float r[4]);
                float r[4];
                vkr_present_rect(r);
                float tx = r[2] > 0 ? (e.tfinger.x - r[0]) / r[2] : e.tfinger.x, ty = r[3] > 0 ? (e.tfinger.y - r[1]) / r[3] : e.tfinger.y;
                tx = tx < 0 ? 0 : tx > 1 ? 1 : tx;
                ty = ty < 0 ? 0 : ty > 1 ? 1 : ty;
                som_hid_touch((int64_t)e.tfinger.fingerID, tx, ty,
                              e.type == SDL_EVENT_FINGER_DOWN ? 0 : e.type == SDL_EVENT_FINGER_MOTION ? 1 : 2,
                              (int64_t)SDL_GetTicksNS());
            }
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                if (!g_background && g.swapchain) {
                    pthread_mutex_lock(&g_gfx_mu);
                    destroy_swapchain(&g);
                    create_swapchain(&g);
                    pthread_mutex_unlock(&g_gfx_mu);
                }
                break;
            default:
                break;
            }
        }
        input_update();
        if (g_want_surface) { /* set by the lifecycle watch; retried every pass until the new native window is usable */
            pthread_mutex_lock(&g_gfx_mu);
            destroy_surface(&g);
            if (create_surface_and_swapchain(&g)) {
                g_background = g_want_surface = false;
                som_log("foreground: surface rebuilt");
            }
            pthread_mutex_unlock(&g_gfx_mu);
        }
        if (g_background) {
            SDL_Delay(50);
            continue;
        }
        if (!g.swapchain) { /* e.g. a resize raced the window going away: keep retrying */
            pthread_mutex_lock(&g_gfx_mu);
            if (!g_background && !create_swapchain(&g)) SDL_Delay(50);
            pthread_mutex_unlock(&g_gfx_mu);
            continue;
        }

        Uint64 now = SDL_GetPerformanceCounter();
        {   /* Present only when the game has a new frame (or every 100 ms, for the overlay). vkQueuePresentKHR blocks
             * ~17 ms on the Mali-G68 while holding the queue lock the renderer needs for its own submit: presenting the
             * same image at the display rate (~50 times a second) kept the renderer waiting ~16 ms per game frame. */
            static uint64_t drawn_presents;
            static Uint64 drawn_at;
            uint64_t p = __atomic_load_n(&som_gfx_presents, __ATOMIC_RELAXED);
            if (g_spike_mode && p && p == drawn_presents && now - drawn_at < freq / 10) {
                SDL_Delay(2);
                continue;
            }
            drawn_presents = p, drawn_at = now;
        }
        double t = (double)(now - start) / (double)freq;
        Uint64 cpu0 = now;
        pthread_mutex_lock(&g_gfx_mu);
        bool ok = !g_background && g.swapchain && draw_frame(&g, t);
        pthread_mutex_unlock(&g_gfx_mu);
        Uint64 after = SDL_GetPerformanceCounter();
        if (!ok) {
            pthread_mutex_lock(&g_gfx_mu);
            if (!g_background) {
                destroy_swapchain(&g);
                if (!create_swapchain(&g)) SDL_Delay(50);
            }
            pthread_mutex_unlock(&g_gfx_mu);
            continue;
        }

        double frame_ms = (double)(after - last) * 1000.0 / (double)freq;
        double cpu_ms = (double)(after - cpu0) * 1000.0 / (double)freq;
        last = after;
        som_perf_frame(frame_ms, cpu_ms, 0.0);
        g_som_stats.last_frame_ms = frame_ms;
        g_som_stats.frames++;
        win_frames++;
        double win_s = (double)(after - win_start) / (double)freq;
        if (win_s >= 1.0) {
            g_som_stats.avg_fps = win_frames / win_s;
            som_log("fps %.1f (last frame %.2f ms)", g_som_stats.avg_fps, frame_ms);
            win_frames = 0;
            win_start = after;
        }
    }

    destroy_surface(&g);
    som_log("Somniel M0 exiting");
    som_log_shutdown();
    SDL_Quit();
    return 0;
}


