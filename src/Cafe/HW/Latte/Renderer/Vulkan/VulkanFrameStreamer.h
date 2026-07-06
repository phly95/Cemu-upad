#pragma once

#ifdef ENABLE_VULKAN

#include <string>
#include <array>
#include <atomic>
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"

#ifdef HAVE_GSTREAMER
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/allocators/gstdmabuf.h>
#endif

class VulkanFrameStreamer
{
public:
	VulkanFrameStreamer(VkDevice device, VkPhysicalDevice physicalDevice, VkInstance instance,
					   uint32 width, uint32 height, VkFormat format = VK_FORMAT_R8G8B8A8_UNORM);
	~VulkanFrameStreamer();

	VulkanFrameStreamer(const VulkanFrameStreamer&) = delete;
	VulkanFrameStreamer& operator=(const VulkanFrameStreamer&) = delete;

	bool IsSupported() const { return m_supported; }
	bool IsActive() const { return m_active; }

	void Start(const std::string& targetIP, uint16 targetPort, uint32 bitrateKbps, uint32 qp);
	void Stop();

	// Record a blit from source image to the streamer's internal buffer
	bool RecordBlit(VkCommandBuffer cmdbuf, VkImage source,
					uint32 srcWidth, uint32 srcHeight,
					uint32 srcOffsetX = 0, uint32 srcOffsetY = 0);
	// Push the current frame to GStreamer (call after RecordBlit)
	void PushFrame();

	uint32 GetWidth() const { return m_width; }
	uint32 GetHeight() const { return m_height; }
	VkImage GetImage() const { return m_frames[m_writeIndex].image; }

private:
	struct FrameResources {
		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkImageView imageView = VK_NULL_HANDLE;
		int cachedFd = -1;
		uint32 stride = 0;
	};

	bool CreateFrameResources(FrameResources& frame);
	void DestroyFrameResources(FrameResources& frame);
	int ExportDmaBuf(FrameResources& frame);

#ifdef HAVE_GSTREAMER
	void InitGstPipeline(const std::string& targetIP, uint16 targetPort, uint32 bitrateKbps, uint32 qp);
	void CleanupGstPipeline();
#endif

	VkDevice m_device = VK_NULL_HANDLE;
	VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
	VkInstance m_instance = VK_NULL_HANDLE;

	// Extension function pointers loaded at runtime
	using PFN_vkGetImageSubresourceLayout_t = void(*)(VkDevice, VkImage, const VkImageSubresource*, VkSubresourceLayout*);
	using PFN_vkGetMemoryFdKHR_t = VkResult(*)(VkDevice, const VkMemoryGetFdInfoKHR*, int*);
	using PFN_vkGetImageDrmFormatModifierPropertiesEXT_t = VkResult(*)(VkDevice, VkImage, VkImageDrmFormatModifierPropertiesEXT*);

	PFN_vkGetImageSubresourceLayout_t m_vkGetImageSubresourceLayout = nullptr;
	PFN_vkGetMemoryFdKHR_t m_vkGetMemoryFdKHR = nullptr;
	PFN_vkGetImageDrmFormatModifierPropertiesEXT_t m_vkGetImageDrmFormatModifier = nullptr;

	VkFormat m_format = VK_FORMAT_R8G8B8A8_UNORM;
	uint32 m_width = 0;
	uint32 m_height = 0;
	bool m_supported = false;
	bool m_active = false;
	uint64 m_drmModifier = 0;

	static constexpr size_t NUM_FRAMES = 3;
	std::array<FrameResources, NUM_FRAMES> m_frames{};
	uint32 m_writeIndex = 0;
	uint64 m_frameCount = 0;

#ifdef HAVE_GSTREAMER
	GstElement* m_pipeline = nullptr;
	GstElement* m_appsrc = nullptr;
	GstAllocator* m_allocator = nullptr;
#endif
};

#endif // ENABLE_VULKAN
