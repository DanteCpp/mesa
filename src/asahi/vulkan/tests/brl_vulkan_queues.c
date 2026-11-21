/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * Queue families and their queues, under the no-op DRM shim. Usage:
 *
 *    brl-vulkan-queues FAMILIES QUEUES
 *
 * The device must advertise FAMILIES queue families, the first one universal
 * (graphics, compute and transfer) with QUEUES queues: more than one where
 * the kernel reports INDEPENDENT_QUEUES, otherwise one. The test creates a
 * device with every queue of that family, submits a fill on each in a
 * timeline chain (queue i waits for queue i - 1), and checks with the shim's
 * queue counters that every queue submitted on a kernel queue of its own and
 * that destroying the device destroys them all.
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>

#define MAX_QUEUES 64

#define CHECK(call)                                                            \
   do {                                                                        \
      VkResult r_ = (call);                                                    \
      if (r_ != VK_SUCCESS) {                                                  \
         fprintf(stderr, "%s: %d\n", #call, r_);                               \
         exit(2);                                                              \
      }                                                                        \
   } while (0)

static unsigned failures, checks;

#define EXPECT(cond, ...)                                                      \
   do {                                                                        \
      checks++;                                                                \
      if (!(cond)) {                                                           \
         fprintf(stderr, "FAIL: " __VA_ARGS__);                                \
         fprintf(stderr, "\n");                                                \
         failures++;                                                           \
      }                                                                        \
   } while (0)

typedef void (*queue_counts_fn)(uint32_t *live, uint32_t *submitted);

static void
queue_counts(queue_counts_fn fn, uint32_t *live, uint32_t *submitted)
{
   *live = *submitted = 0;
   if (fn)
      fn(live, submitted);
}

int
main(int argc, char **argv)
{
   if (argc != 3) {
      fprintf(stderr, "usage: %s FAMILIES QUEUES\n", argv[0]);
      return 2;
   }
   const uint32_t want_families = strtoul(argv[1], NULL, 0);
   const uint32_t want_queues = strtoul(argv[2], NULL, 0);
   const queue_counts_fn counts =
      (queue_counts_fn)dlsym(RTLD_DEFAULT, "asahi_noop_queue_counts");
   if (!counts) {
      fprintf(stderr, "asahi_noop_queue_counts not found: run under the shim\n");
      return 2;
   }

   const VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .apiVersion = VK_API_VERSION_1_3,
   };
   const VkInstanceCreateInfo instance_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   VkInstance instance;
   CHECK(vkCreateInstance(&instance_info, NULL, &instance));
   uint32_t pdev_count = 1;
   VkPhysicalDevice pdev;
   VkResult enumerated = vkEnumeratePhysicalDevices(instance, &pdev_count, &pdev);
   if ((enumerated != VK_SUCCESS && enumerated != VK_INCOMPLETE) || !pdev_count) {
      fprintf(stderr, "no physical device: %d\n", enumerated);
      return 2;
   }

   uint32_t family_count = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(pdev, &family_count, NULL);
   VkQueueFamilyProperties families[8];
   if (family_count > 8)
      family_count = 8;
   vkGetPhysicalDeviceQueueFamilyProperties(pdev, &family_count, families);
   EXPECT(family_count == want_families, "%u queue families, want %u",
          family_count, want_families);
   const VkQueueFlags universal =
      VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
   EXPECT((families[0].queueFlags & universal) == universal,
          "family 0 flags %#x are not universal", families[0].queueFlags);
   const uint32_t queue_count = families[0].queueCount;
   EXPECT(queue_count == want_queues, "family 0 has %u queues, want %u",
          queue_count, want_queues);
   if (!queue_count || queue_count > MAX_QUEUES) {
      fprintf(stderr, "unusable queue count %u\n", queue_count);
      return 1;
   }

   uint32_t live_before, submitted_before;
   queue_counts(counts, &live_before, &submitted_before);

   float priorities[MAX_QUEUES];
   for (uint32_t i = 0; i < queue_count; ++i)
      priorities[i] = 1.0f;
   const VkDeviceQueueCreateInfo queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0,
      .queueCount = queue_count,
      .pQueuePriorities = priorities,
   };
   VkPhysicalDeviceVulkan12Features vk12 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
      .timelineSemaphore = VK_TRUE,
   };
   const VkDeviceCreateInfo device_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = &vk12,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
   };
   VkDevice dev;
   CHECK(vkCreateDevice(pdev, &device_info, NULL, &dev));

   VkQueue queues[MAX_QUEUES];
   for (uint32_t i = 0; i < queue_count; ++i) {
      vkGetDeviceQueue(dev, 0, i, &queues[i]);
      EXPECT(queues[i] != VK_NULL_HANDLE, "queue %u is null", i);
      for (uint32_t j = 0; j < i; ++j)
         EXPECT(queues[i] != queues[j], "queues %u and %u are one queue", j, i);
   }

   const VkDeviceSize slot = 256;
   const VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = slot * queue_count,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
   };
   VkBuffer buffer;
   CHECK(vkCreateBuffer(dev, &buffer_info, NULL, &buffer));
   VkMemoryRequirements requirements;
   vkGetBufferMemoryRequirements(dev, buffer, &requirements);
   uint32_t type = 0;
   while (type < 32 && !(requirements.memoryTypeBits & (1u << type)))
      type++;
   const VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = type,
   };
   VkDeviceMemory memory;
   CHECK(vkAllocateMemory(dev, &alloc_info, NULL, &memory));
   CHECK(vkBindBufferMemory(dev, buffer, memory, 0));

   const VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = 0,
   };
   VkCommandPool pool;
   CHECK(vkCreateCommandPool(dev, &pool_info, NULL, &pool));
   VkCommandBuffer cmds[MAX_QUEUES];
   const VkCommandBufferAllocateInfo cmd_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = queue_count,
   };
   CHECK(vkAllocateCommandBuffers(dev, &cmd_info, cmds));
   for (uint32_t i = 0; i < queue_count; ++i) {
      const VkCommandBufferBeginInfo begin = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      };
      CHECK(vkBeginCommandBuffer(cmds[i], &begin));
      vkCmdFillBuffer(cmds[i], buffer, slot * i, slot, i + 1);
      CHECK(vkEndCommandBuffer(cmds[i]));
   }

   const VkSemaphoreTypeCreateInfo timeline_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
   };
   const VkSemaphoreCreateInfo semaphore_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
      .pNext = &timeline_info,
   };
   VkSemaphore timeline;
   CHECK(vkCreateSemaphore(dev, &semaphore_info, NULL, &timeline));

   for (uint32_t i = 0; i < queue_count; ++i) {
      const uint64_t wait_value = i, signal_value = i + 1;
      const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
      const VkTimelineSemaphoreSubmitInfo values = {
         .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
         .waitSemaphoreValueCount = i ? 1 : 0,
         .pWaitSemaphoreValues = &wait_value,
         .signalSemaphoreValueCount = 1,
         .pSignalSemaphoreValues = &signal_value,
      };
      const VkSubmitInfo submit = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .pNext = &values,
         .waitSemaphoreCount = i ? 1 : 0,
         .pWaitSemaphores = &timeline,
         .pWaitDstStageMask = &stage,
         .commandBufferCount = 1,
         .pCommandBuffers = &cmds[i],
         .signalSemaphoreCount = 1,
         .pSignalSemaphores = &timeline,
      };
      CHECK(vkQueueSubmit(queues[i], 1, &submit, VK_NULL_HANDLE));
   }
   const uint64_t last = queue_count;
   const VkSemaphoreWaitInfo wait = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
      .semaphoreCount = 1,
      .pSemaphores = &timeline,
      .pValues = &last,
   };
   CHECK(vkWaitSemaphores(dev, &wait, 5000000000ull));
   for (uint32_t i = 0; i < queue_count; ++i)
      CHECK(vkQueueWaitIdle(queues[i]));
   CHECK(vkDeviceWaitIdle(dev));

   uint32_t live, submitted;
   queue_counts(counts, &live, &submitted);
   EXPECT(submitted - submitted_before == queue_count,
          "%u kernel queues submitted work for %u API queues",
          submitted - submitted_before, queue_count);
   EXPECT(live - live_before >= queue_count,
          "%u live kernel queues for %u API queues", live - live_before,
          queue_count);

   vkDestroySemaphore(dev, timeline, NULL);
   vkDestroyCommandPool(dev, pool, NULL);
   vkDestroyBuffer(dev, buffer, NULL);
   vkFreeMemory(dev, memory, NULL);
   vkDestroyDevice(dev, NULL);
   queue_counts(counts, &live, &submitted);
   EXPECT(live == live_before, "%u kernel queues outlive the device",
          live - live_before);
   vkDestroyInstance(instance, NULL);

   printf("%u/%u checks passed: %u families, %u queues on family 0\n",
          checks - failures, checks, family_count, queue_count);
   return failures ? 1 : 0;
}
