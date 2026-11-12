#pragma once
#include <vulkan/vulkan.h>
#include "volk.h" /* Vulkan calls go through volk pointers (gpu_driver.c) */

/* The monitor level, set from the sidebar: 0 off; 1 the FPS; 2 FPS, frame time and a frame-time graph; 3 adds the CPU
 * and GPU temperatures; 4 adds each CPU core's clock and the GPU clock. Level 2 until set. A plain int read once per frame. */
void overlay_set_level(int level);

/* Draw the frame-rate overlay into dst (layout TRANSFER_DST_OPTIMAL), after the game's frame was blitted there.
 * slot: the window loop's frame-in-flight index (each slot has its own staging buffer). */
void overlay_draw(VkCommandBuffer cb, VkImage dst, VkFormat fmt, VkExtent2D ext, uint32_t slot);
