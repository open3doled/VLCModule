#define _GNU_SOURCE

/*
 * Minimal Open3D Vulkan direct-display flip probe.
 *
 * This intentionally avoids Wayland/X11 surfaces. It creates a VK_KHR_display
 * surface for a physical connector, clears swapchain images red/blue/black,
 * and presents at a requested cadence. It is a standalone hardware capability
 * probe for the future exclusive fullscreen Open3D presenter backend.
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <vulkan/vulkan.h>

typedef struct
{
    double hz;
    double target_flip_hz;
    unsigned seconds;
    const char *display_substr;
    const char *drm_path;
    bool bfi;
    bool list_only;
} open3d_probe_opts_t;

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void sleep_until_ns(uint64_t deadline)
{
    for (;;)
    {
        uint64_t now = mono_ns();
        if (now >= deadline)
            return;
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                               &(struct timespec) {
                                   .tv_sec = (time_t)(deadline / 1000000000ull),
                                   .tv_nsec = (long)(deadline % 1000000000ull),
                               },
                               NULL) != 0)
        {
            if (errno != EINTR)
                return;
        }
        return;
    }
}

static const char *vk_result_name(VkResult r)
{
    switch (r)
    {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_EVENT_SET: return "VK_EVENT_SET";
        case VK_EVENT_RESET: return "VK_EVENT_RESET";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        default: return "VK_UNKNOWN";
    }
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [--list] [--hz HZ] [--target-flip-hz HZ] [--seconds N]\n"
            "          [--display NAME_SUBSTR] [--drm /dev/dri/cardN] [--bfi]\n",
            argv0);
}

static bool parse_opts(int argc, char **argv, open3d_probe_opts_t *opts)
{
    *opts = (open3d_probe_opts_t) {
        .hz = 239.761,
        .target_flip_hz = 119.8805,
        .seconds = 8,
        .display_substr = NULL,
        .drm_path = "/dev/dri/card1",
        .bfi = false,
        .list_only = false,
    };
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--list"))
            opts->list_only = true;
        else if (!strcmp(argv[i], "--hz") && i + 1 < argc)
            opts->hz = atof(argv[++i]);
        else if (!strcmp(argv[i], "--target-flip-hz") && i + 1 < argc)
            opts->target_flip_hz = atof(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            opts->seconds = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--display") && i + 1 < argc)
            opts->display_substr = argv[++i];
        else if (!strcmp(argv[i], "--drm") && i + 1 < argc)
            opts->drm_path = argv[++i];
        else if (!strcmp(argv[i], "--bfi"))
            opts->bfi = true;
        else
        {
            usage(argv[0]);
            return false;
        }
    }
    return opts->hz > 1.0 && opts->target_flip_hz > 0.0 && opts->seconds > 0;
}

static VkResult submit_clear(VkDevice dev, VkQueue queue, VkCommandPool pool,
                             VkImage image, VkSemaphore wait_sem,
                             VkSemaphore signal_sem, VkFence fence,
                             VkClearColorValue color)
{
    VkCommandBufferAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkResult r = vkAllocateCommandBuffers(dev, &ai, &cmd);
    if (r != VK_SUCCESS)
        return r;

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &bi);

    VkImageMemoryBarrier to_transfer = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 1, &to_transfer);
    vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &color, 1, &to_transfer.subresourceRange);
    VkImageMemoryBarrier to_present = to_transfer;
    to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_present.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                         0, NULL, 0, NULL, 1, &to_present);
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &wait_sem,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &signal_sem,
    };
    r = vkQueueSubmit(queue, 1, &si, fence);
    if (r == VK_SUCCESS)
        r = vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    vkResetFences(dev, 1, &fence);
    vkFreeCommandBuffers(dev, pool, 1, &cmd);
    return r;
}

int main(int argc, char **argv)
{
    open3d_probe_opts_t opts;
    if (!parse_opts(argc, argv, &opts))
        return 2;

    const char *inst_exts[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_DISPLAY_EXTENSION_NAME,
        VK_EXT_DIRECT_MODE_DISPLAY_EXTENSION_NAME,
        VK_EXT_ACQUIRE_DRM_DISPLAY_EXTENSION_NAME,
    };
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "open3d-vk-display-flip-probe",
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledExtensionCount = sizeof(inst_exts) / sizeof(inst_exts[0]),
        .ppEnabledExtensionNames = inst_exts,
    };
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = vkCreateInstance(&ici, NULL, &inst);
    if (r != VK_SUCCESS)
    {
        fprintf(stderr, "vkCreateInstance failed: %s\n", vk_result_name(r));
        return 1;
    }

    PFN_vkGetPhysicalDeviceDisplayPropertiesKHR get_display_props =
        (PFN_vkGetPhysicalDeviceDisplayPropertiesKHR)
            vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceDisplayPropertiesKHR");
    PFN_vkGetDisplayModePropertiesKHR get_mode_props =
        (PFN_vkGetDisplayModePropertiesKHR)
            vkGetInstanceProcAddr(inst, "vkGetDisplayModePropertiesKHR");
    PFN_vkGetPhysicalDeviceDisplayPlanePropertiesKHR get_plane_props =
        (PFN_vkGetPhysicalDeviceDisplayPlanePropertiesKHR)
            vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceDisplayPlanePropertiesKHR");
    PFN_vkGetDisplayPlaneSupportedDisplaysKHR get_plane_displays =
        (PFN_vkGetDisplayPlaneSupportedDisplaysKHR)
            vkGetInstanceProcAddr(inst, "vkGetDisplayPlaneSupportedDisplaysKHR");
    PFN_vkCreateDisplayPlaneSurfaceKHR create_display_surface =
        (PFN_vkCreateDisplayPlaneSurfaceKHR)
            vkGetInstanceProcAddr(inst, "vkCreateDisplayPlaneSurfaceKHR");
    PFN_vkAcquireDrmDisplayEXT acquire_drm_display =
        (PFN_vkAcquireDrmDisplayEXT)
            vkGetInstanceProcAddr(inst, "vkAcquireDrmDisplayEXT");
    PFN_vkReleaseDisplayEXT release_display =
        (PFN_vkReleaseDisplayEXT)
            vkGetInstanceProcAddr(inst, "vkReleaseDisplayEXT");

    if (!get_display_props || !get_mode_props || !get_plane_props ||
        !get_plane_displays || !create_display_surface)
    {
        fprintf(stderr, "required VK_KHR_display entry points unavailable\n");
        return 1;
    }

    uint32_t phys_count = 0;
    vkEnumeratePhysicalDevices(inst, &phys_count, NULL);
    VkPhysicalDevice *phys_devs = calloc(phys_count, sizeof(*phys_devs));
    vkEnumeratePhysicalDevices(inst, &phys_count, phys_devs);

    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDisplayKHR display = VK_NULL_HANDLE;
    VkDisplayModeKHR mode = VK_NULL_HANDLE;
    VkExtent2D extent = {0, 0};
    uint32_t plane_index = UINT32_MAX;
    double best_score = 1e30;

    for (uint32_t pi = 0; pi < phys_count; ++pi)
    {
        uint32_t display_count = 0;
        r = get_display_props(phys_devs[pi], &display_count, NULL);
        if (r != VK_SUCCESS || display_count == 0)
            continue;
        VkDisplayPropertiesKHR *displays =
            calloc(display_count, sizeof(*displays));
        get_display_props(phys_devs[pi], &display_count, displays);

        for (uint32_t di = 0; di < display_count; ++di)
        {
            const char *name = displays[di].displayName ? displays[di].displayName : "";
            if (opts.display_substr != NULL && strstr(name, opts.display_substr) == NULL)
                continue;

            uint32_t mode_count = 0;
            get_mode_props(phys_devs[pi], displays[di].display, &mode_count, NULL);
            VkDisplayModePropertiesKHR *modes =
                calloc(mode_count, sizeof(*modes));
            get_mode_props(phys_devs[pi], displays[di].display, &mode_count, modes);
            for (uint32_t mi = 0; mi < mode_count; ++mi)
            {
                double hz = (double)modes[mi].parameters.refreshRate / 1000.0;
                double score = fabs(hz - opts.hz);
                if (modes[mi].parameters.visibleRegion.width == 0 ||
                    modes[mi].parameters.visibleRegion.height == 0)
                    score += 1000000.0;
                if (score < best_score)
                {
                    phys = phys_devs[pi];
                    display = displays[di].display;
                    mode = modes[mi].displayMode;
                    extent = modes[mi].parameters.visibleRegion;
                    best_score = score;
                    fprintf(stderr, "candidate display=%s mode=%ux%u %.6f Hz score=%.6f\n",
                            name, extent.width, extent.height, hz, score);
                }
            }
            free(modes);
        }
        free(displays);
    }

    if (opts.list_only)
    {
        free(phys_devs);
        vkDestroyInstance(inst, NULL);
        return 0;
    }

    if (phys == VK_NULL_HANDLE || display == VK_NULL_HANDLE || mode == VK_NULL_HANDLE)
    {
        fprintf(stderr, "no matching Vulkan display/mode found\n");
        return 1;
    }

    if (acquire_drm_display && opts.drm_path != NULL)
    {
        int fd = open(opts.drm_path, O_RDWR | O_CLOEXEC);
        if (fd >= 0)
        {
            r = acquire_drm_display(phys, fd, display);
            fprintf(stderr, "vkAcquireDrmDisplayEXT(%s): %s\n",
                    opts.drm_path, vk_result_name(r));
            if (r != VK_SUCCESS)
            {
                fprintf(stderr,
                        "note: acquire failure usually means the active compositor still owns this display;\n"
                        "      run from a dedicated VT/KMS session or use a compositor DRM lease.\n");
            }
            close(fd);
        }
        else
        {
            fprintf(stderr, "open(%s) failed: %s\n", opts.drm_path, strerror(errno));
        }
    }

    uint32_t plane_count = 0;
    get_plane_props(phys, &plane_count, NULL);
    VkDisplayPlanePropertiesKHR *planes = calloc(plane_count, sizeof(*planes));
    get_plane_props(phys, &plane_count, planes);
    for (uint32_t p = 0; p < plane_count; ++p)
    {
        uint32_t supported = 0;
        get_plane_displays(phys, p, &supported, NULL);
        VkDisplayKHR *supported_displays = calloc(supported, sizeof(*supported_displays));
        get_plane_displays(phys, p, &supported, supported_displays);
        for (uint32_t i = 0; i < supported; ++i)
        {
            if (supported_displays[i] == display)
            {
                plane_index = p;
                break;
            }
        }
        free(supported_displays);
        if (plane_index != UINT32_MAX)
            break;
    }
    free(planes);
    if (plane_index == UINT32_MAX)
    {
        fprintf(stderr, "no display plane supports selected display\n");
        return 1;
    }

    VkDisplaySurfaceCreateInfoKHR dsci = {
        .sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
        .displayMode = mode,
        .planeIndex = plane_index,
        .planeStackIndex = 0,
        .transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .globalAlpha = 1.0f,
        .alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
        .imageExtent = extent,
    };
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    r = create_display_surface(inst, &dsci, NULL, &surface);
    if (r != VK_SUCCESS)
    {
        fprintf(stderr, "vkCreateDisplayPlaneSurfaceKHR failed: %s\n",
                vk_result_name(r));
        return 1;
    }

    uint32_t q_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &q_count, NULL);
    VkQueueFamilyProperties *q_props = calloc(q_count, sizeof(*q_props));
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &q_count, q_props);
    uint32_t q_family = UINT32_MAX;
    for (uint32_t q = 0; q < q_count; ++q)
    {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(phys, q, surface, &present);
        if ((q_props[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
        {
            q_family = q;
            break;
        }
    }
    free(q_props);
    if (q_family == UINT32_MAX)
    {
        fprintf(stderr, "no graphics+present queue for display surface\n");
        return 1;
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = q_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const char *dev_exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = dev_exts,
    };
    VkDevice dev = VK_NULL_HANDLE;
    r = vkCreateDevice(phys, &dci, NULL, &dev);
    if (r != VK_SUCCESS)
    {
        fprintf(stderr, "vkCreateDevice failed: %s\n", vk_result_name(r));
        return 1;
    }
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(dev, q_family, 0, &queue);

    uint32_t fmt_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fmt_count, NULL);
    VkSurfaceFormatKHR *fmts = calloc(fmt_count, sizeof(*fmts));
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fmt_count, fmts);
    VkSurfaceFormatKHR fmt = fmts[0];
    for (uint32_t i = 0; i < fmt_count; ++i)
    {
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM ||
            fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM)
        {
            fmt = fmts[i];
            break;
        }
    }
    free(fmts);

    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps);
    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount != 0 && image_count > caps.maxImageCount)
        image_count = caps.maxImageCount;
    if (image_count < 2)
        image_count = 2;

    VkSwapchainCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = image_count,
        .imageFormat = fmt.format,
        .imageColorSpace = fmt.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
    };
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    r = vkCreateSwapchainKHR(dev, &sci, NULL, &swapchain);
    if (r != VK_SUCCESS)
    {
        fprintf(stderr, "vkCreateSwapchainKHR failed: %s\n", vk_result_name(r));
        fprintf(stderr,
                "note: direct-display swapchain creation cannot proceed while GNOME/Mutter owns the\n"
                "      primary monitor and does not advertise wp_drm_lease_device_v1.\n");
        return 1;
    }

    uint32_t swap_count = 0;
    vkGetSwapchainImagesKHR(dev, swapchain, &swap_count, NULL);
    VkImage *images = calloc(swap_count, sizeof(*images));
    vkGetSwapchainImagesKHR(dev, swapchain, &swap_count, images);

    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = q_family,
    };
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(dev, &cpci, NULL, &pool);
    VkSemaphoreCreateInfo sem_ci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fence_ci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkSemaphore acquire_sem = VK_NULL_HANDLE;
    VkSemaphore render_sem = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    vkCreateSemaphore(dev, &sem_ci, NULL, &acquire_sem);
    vkCreateSemaphore(dev, &sem_ci, NULL, &render_sem);
    vkCreateFence(dev, &fence_ci, NULL, &fence);

    fprintf(stderr,
            "presenting direct display extent=%ux%u presenter=%.6f target=%.6f seconds=%u bfi=%d images=%u\n",
            extent.width, extent.height, opts.hz, opts.target_flip_hz,
            opts.seconds, opts.bfi ? 1 : 0, swap_count);

    const uint64_t period_ns = (uint64_t)llround(1000000000.0 / opts.hz);
    const unsigned divider = opts.target_flip_hz > 0.0
                                 ? (unsigned)llround(opts.hz / opts.target_flip_hz)
                                 : 1;
    const uint64_t start = mono_ns() + 500000000ull;
    const uint64_t end = start + (uint64_t)opts.seconds * 1000000000ull;
    uint64_t tick = 0;
    uint64_t late = 0;
    uint64_t late_max_ns = 0;
    uint64_t present_fail = 0;
    uint64_t last_present_end = 0;
    uint64_t interval_max_ns = 0;

    while (mono_ns() < end)
    {
        const uint64_t deadline = start + tick * period_ns;
        sleep_until_ns(deadline);
        uint64_t now = mono_ns();
        if (now > deadline + period_ns / 2)
        {
            late++;
            uint64_t over = now - deadline;
            if (over > late_max_ns)
                late_max_ns = over;
        }

        uint32_t image_index = 0;
        r = vkAcquireNextImageKHR(dev, swapchain, UINT64_MAX, acquire_sem,
                                  VK_NULL_HANDLE, &image_index);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
        {
            fprintf(stderr, "vkAcquireNextImageKHR failed at tick=%llu: %s\n",
                    (unsigned long long)tick, vk_result_name(r));
            present_fail++;
            break;
        }

        bool black = opts.bfi && ((tick % 2u) != 0);
        bool right = divider <= 1 ? ((tick & 1u) != 0)
                                  : (((tick / divider) & 1u) != 0);
        VkClearColorValue color = black
                                      ? (VkClearColorValue){{0.f, 0.f, 0.f, 1.f}}
                                      : (right
                                             ? (VkClearColorValue){{0.f, 0.f, 1.f, 1.f}}
                                             : (VkClearColorValue){{1.f, 0.f, 0.f, 1.f}});
        r = submit_clear(dev, queue, pool, images[image_index], acquire_sem,
                         render_sem, fence, color);
        if (r != VK_SUCCESS)
        {
            fprintf(stderr, "submit_clear failed at tick=%llu: %s\n",
                    (unsigned long long)tick, vk_result_name(r));
            present_fail++;
            break;
        }

        VkPresentInfoKHR pi = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &render_sem,
            .swapchainCount = 1,
            .pSwapchains = &swapchain,
            .pImageIndices = &image_index,
        };
        r = vkQueuePresentKHR(queue, &pi);
        uint64_t present_end = mono_ns();
        if (last_present_end != 0)
        {
            uint64_t interval = present_end - last_present_end;
            if (interval > interval_max_ns)
                interval_max_ns = interval;
        }
        last_present_end = present_end;
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
        {
            fprintf(stderr, "vkQueuePresentKHR failed at tick=%llu: %s\n",
                    (unsigned long long)tick, vk_result_name(r));
            present_fail++;
            break;
        }
        if ((tick % (uint64_t)llround(opts.hz)) == 0 && tick != 0)
        {
            fprintf(stderr,
                    "tick=%llu late=%llu late_max=%.3fms interval_max=%.3fms fail=%llu\n",
                    (unsigned long long)tick,
                    (unsigned long long)late,
                    (double)late_max_ns / 1000000.0,
                    (double)interval_max_ns / 1000000.0,
                    (unsigned long long)present_fail);
        }
        tick++;
    }

    vkDeviceWaitIdle(dev);
    fprintf(stderr,
            "done ticks=%llu late=%llu late_max=%.3fms interval_max=%.3fms fail=%llu\n",
            (unsigned long long)tick,
            (unsigned long long)late,
            (double)late_max_ns / 1000000.0,
            (double)interval_max_ns / 1000000.0,
            (unsigned long long)present_fail);

    vkDestroyFence(dev, fence, NULL);
    vkDestroySemaphore(dev, render_sem, NULL);
    vkDestroySemaphore(dev, acquire_sem, NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    free(images);
    vkDestroySwapchainKHR(dev, swapchain, NULL);
    vkDestroyDevice(dev, NULL);
    if (release_display)
        release_display(phys, display);
    vkDestroySurfaceKHR(inst, surface, NULL);
    free(phys_devs);
    vkDestroyInstance(inst, NULL);
    return present_fail == 0 ? 0 : 1;
}
