/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * Offline stream-shape probe for transfer command buffers. It records one
 * command buffer of transfers, submits it once and waits; it never checks GPU
 * results. Run it on the asahi drm-shim with ASAHI_SHIM_SUBMIT_LOG and
 * ASAHI_SHIM_SUBMIT_LOG_CDM_BYTES=1 (and, for G17G,
 * ASAHI_SHIM_G17G_ACCEPT_SUBMIT=1): the shim then logs every command each
 * submit carries and the bytes of every compute control stream. The setup
 * work goes to the log named by HK_CDM_SETUP_LOG; only the measured command
 * buffer's submits reach ASAHI_SHIM_SUBMIT_LOG. run-cdm-stream-shape.py
 * drives it.
 *
 *    hk_cdm_stream_shape ICD.so OPS [barrier|nobarrier]
 *
 * OPS is a string of: F fill (16-byte aligned), C buffer copy,
 * K clear colour image, U buffer update.
 *
 * It loads the ICD directly (vk_icdGetInstanceProcAddr), so no loader or
 * other driver takes part.
 *
 * HK_CDM_FENCE_WAIT=1 instead submits the measured command buffer with a
 * fence and prints vkWaitForFences' result ("WAIT result=N").
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define CHECK(call)                                                            \
   do {                                                                        \
      VkResult r_ = (call);                                                    \
      if (r_ != VK_SUCCESS) {                                                  \
         fprintf(stderr, "FAIL %s: %d\n", #call, r_);                          \
         exit(2);                                                              \
      }                                                                        \
   } while (0)

typedef VkResult(VKAPI_PTR *PFN_negotiate)(uint32_t *);

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
static VkInstance instance;
static VkDevice device;

#define IFN(name) PFN_vk##name name = (PFN_vk##name)gipa(instance, "vk" #name)
#define DFN(name) PFN_vk##name name = (PFN_vk##name)gdpa(device, "vk" #name)

static uint32_t
memory_type(VkPhysicalDevice pdev, uint32_t bits, VkMemoryPropertyFlags want)
{
   IFN(GetPhysicalDeviceMemoryProperties);
   VkPhysicalDeviceMemoryProperties props;
   GetPhysicalDeviceMemoryProperties(pdev, &props);
   for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
      if ((bits & (1u << i)) &&
          (props.memoryTypes[i].propertyFlags & want) == want)
         return i;
   }
   fprintf(stderr, "FAIL no memory type for bits 0x%x\n", bits);
   exit(2);
}

int
main(int argc, char **argv)
{
   if (argc < 3) {
      fprintf(stderr, "usage: %s ICD.so OPS [barrier|nobarrier]\n", argv[0]);
      return 2;
   }
   const char *ops = argv[2];
   const bool barriers = argc > 3 && !strcmp(argv[3], "barrier");
   const char *measured_log = getenv("ASAHI_SHIM_SUBMIT_LOG");
   const char *setup_log = getenv("HK_CDM_SETUP_LOG");
   if (!measured_log || !setup_log) {
      fprintf(stderr, "FAIL set ASAHI_SHIM_SUBMIT_LOG and HK_CDM_SETUP_LOG\n");
      return 2;
   }
   char measured[4096];
   snprintf(measured, sizeof(measured), "%s", measured_log);
   /* Everything until the measured submit is setup. */
   setenv("ASAHI_SHIM_SUBMIT_LOG", setup_log, 1);

   void *icd = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
   if (!icd) {
      fprintf(stderr, "FAIL dlopen %s: %s\n", argv[1], dlerror());
      return 2;
   }
   PFN_negotiate negotiate =
      (PFN_negotiate)dlsym(icd, "vk_icdNegotiateLoaderICDInterfaceVersion");
   gipa = (PFN_vkGetInstanceProcAddr)dlsym(icd, "vk_icdGetInstanceProcAddr");
   if (!negotiate || !gipa) {
      fprintf(stderr, "FAIL ICD entry points missing\n");
      return 2;
   }
   uint32_t version = 5;
   CHECK(negotiate(&version));

   PFN_vkCreateInstance CreateInstance =
      (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   const VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "hk_cdm_stream_shape",
      .apiVersion = VK_API_VERSION_1_3,
   };
   const VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   CHECK(CreateInstance(&ici, NULL, &instance));

   IFN(EnumeratePhysicalDevices);
   uint32_t count = 1;
   VkPhysicalDevice pdev;
   VkResult r = EnumeratePhysicalDevices(instance, &count, &pdev);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || count == 0) {
      fprintf(stderr, "FAIL no physical device (%d)\n", r);
      return 2;
   }

   const float priority = 1.0f;
   const VkDeviceQueueCreateInfo qci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0,
      .queueCount = 1,
      .pQueuePriorities = &priority,
   };
   const VkPhysicalDeviceVulkan13Features f13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .synchronization2 = VK_TRUE,
   };
   const VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = &f13,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &qci,
   };
   IFN(CreateDevice);
   CHECK(CreateDevice(pdev, &dci, NULL, &device));
   gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");

   DFN(GetDeviceQueue);
   DFN(CreateBuffer);
   DFN(GetBufferMemoryRequirements);
   DFN(AllocateMemory);
   DFN(BindBufferMemory);
   DFN(CreateImage);
   DFN(GetImageMemoryRequirements);
   DFN(BindImageMemory);
   DFN(CreateCommandPool);
   DFN(AllocateCommandBuffers);
   DFN(BeginCommandBuffer);
   DFN(EndCommandBuffer);
   DFN(CmdFillBuffer);
   DFN(CmdCopyBuffer);
   DFN(CmdUpdateBuffer);
   DFN(CmdClearColorImage);
   DFN(CmdPipelineBarrier2);
   DFN(QueueSubmit);
   DFN(QueueWaitIdle);

   VkQueue queue;
   GetDeviceQueue(device, 0, 0, &queue);

   /* Two 64 KiB buffers. */
   VkBuffer buf[2];
   for (unsigned i = 0; i < 2; ++i) {
      const VkBufferCreateInfo bci = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
         .size = 65536,
         .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                  VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      };
      CHECK(CreateBuffer(device, &bci, NULL, &buf[i]));
      VkMemoryRequirements req;
      GetBufferMemoryRequirements(device, buf[i], &req);
      const VkMemoryAllocateInfo mai = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = req.size,
         .memoryTypeIndex = memory_type(pdev, req.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT),
      };
      VkDeviceMemory mem;
      CHECK(AllocateMemory(device, &mai, NULL, &mem));
      CHECK(BindBufferMemory(device, buf[i], mem, 0));
   }

   /* A 64x64 RGBA8 image in GENERAL for the clears. */
   const VkImageCreateInfo ici2 = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = {64, 64, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
   };
   VkImage image;
   CHECK(CreateImage(device, &ici2, NULL, &image));
   {
      VkMemoryRequirements req;
      GetImageMemoryRequirements(device, image, &req);
      const VkMemoryAllocateInfo mai = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = req.size,
         .memoryTypeIndex = memory_type(pdev, req.memoryTypeBits, 0),
      };
      VkDeviceMemory mem;
      CHECK(AllocateMemory(device, &mai, NULL, &mem));
      CHECK(BindImageMemory(device, image, mem, 0));
   }

   const VkCommandPoolCreateInfo pci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = 0,
   };
   VkCommandPool pool;
   CHECK(CreateCommandPool(device, &pci, NULL, &pool));
   VkCommandBuffer cbs[2];
   const VkCommandBufferAllocateInfo cai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 2,
   };
   CHECK(AllocateCommandBuffers(device, &cai, cbs));
   const VkCommandBufferBeginInfo begin = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   const VkImageSubresourceRange range = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .levelCount = 1,
      .layerCount = 1,
   };

   /* Setup: the image to GENERAL. */
   CHECK(BeginCommandBuffer(cbs[0], &begin));
   {
      const VkImageMemoryBarrier2 imb = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
         .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
         .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
         .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_GENERAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = image,
         .subresourceRange = range,
      };
      const VkDependencyInfo dep = {
         .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
         .imageMemoryBarrierCount = 1,
         .pImageMemoryBarriers = &imb,
      };
      CmdPipelineBarrier2(cbs[0], &dep);
   }
   CHECK(EndCommandBuffer(cbs[0]));
   VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cbs[0],
   };
   CHECK(QueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
   CHECK(QueueWaitIdle(queue));

   /* The measured command buffer. */
   CHECK(BeginCommandBuffer(cbs[1], &begin));
   const VkMemoryBarrier2 mb = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
      .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
      .dstAccessMask =
         VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
   };
   const VkDependencyInfo dep = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .memoryBarrierCount = 1,
      .pMemoryBarriers = &mb,
   };
   const uint32_t words[64] = {0x11111111, 0x22222222, 0x33333333};
   for (unsigned i = 0; ops[i]; ++i) {
      if (i && barriers)
         CmdPipelineBarrier2(cbs[1], &dep);
      const VkDeviceSize at = 1024 * (VkDeviceSize)i;
      switch (ops[i]) {
      case 'F':
         CmdFillBuffer(cbs[1], buf[1], at, 256, 0xa5a50000u + i);
         break;
      case 'C': {
         const VkBufferCopy copy = {.srcOffset = at, .dstOffset = at,
                                    .size = 256};
         CmdCopyBuffer(cbs[1], buf[0], buf[1], 1, &copy);
         break;
      }
      case 'K': {
         const VkClearColorValue colour = {.uint32 = {i, 0, 0, 0xff}};
         CmdClearColorImage(cbs[1], image, VK_IMAGE_LAYOUT_GENERAL, &colour,
                            1, &range);
         break;
      }
      case 'U':
         CmdUpdateBuffer(cbs[1], buf[1], at, sizeof(words), words);
         break;
      default:
         fprintf(stderr, "FAIL unknown op %c\n", ops[i]);
         return 2;
      }
   }
   CHECK(EndCommandBuffer(cbs[1]));

   setenv("ASAHI_SHIM_SUBMIT_LOG", measured, 1);
   si.pCommandBuffers = &cbs[1];
   if (getenv("HK_CDM_FENCE_WAIT")) {
      /* The submit's fence signals (the shim retires every accepted submit);
       * print what vkWaitForFences makes of it. With a sticky VM error
       * published through VM_STATUS the driver reports the device lost. */
      DFN(CreateFence);
      DFN(WaitForFences);
      const VkFenceCreateInfo fci = {
         .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
      };
      VkFence fence;
      CHECK(CreateFence(device, &fci, NULL, &fence));
      /* HK_CDM_VM_ERROR=<errno>: the shim publishes that sticky VM error
       * from this submit on, as a kernel that refused it and still
       * signalled its fence would (setup above ran without it). */
      const char *vm_error = getenv("HK_CDM_VM_ERROR");
      if (vm_error)
         setenv("ASAHI_SHIM_VM_STATUS_ERROR", vm_error, 1);
      CHECK(QueueSubmit(queue, 1, &si, fence));
      const VkResult waited =
         WaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull);
      printf("WAIT result=%d\n", waited);
      fflush(stdout);
      _Exit(0);
   }
   CHECK(QueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
   CHECK(QueueWaitIdle(queue));
   setenv("ASAHI_SHIM_SUBMIT_LOG", setup_log, 1);
   printf("DONE ops=%s barriers=%u\n", ops, barriers);
   fflush(stdout);
   /* Teardown is not part of the measurement; skip it. */
   _Exit(0);
}
