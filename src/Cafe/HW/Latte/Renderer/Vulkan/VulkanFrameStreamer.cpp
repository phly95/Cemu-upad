#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanFrameStreamer.h"

#ifdef ENABLE_VULKAN

#include <unistd.h>
#include <cstring>

#include "config/CemuConfig.h"

#ifdef HAVE_GSTREAMER
#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#include <gst/video/gstvideometa.h>
#endif

VulkanFrameStreamer::VulkanFrameStreamer(VkDevice device, VkPhysicalDevice physicalDevice,
										 VkInstance instance, uint32 width, uint32 height,
										 VkFormat format, const std::string& name)
	: m_device(device), m_physicalDevice(physicalDevice), m_instance(instance),
	  m_format(format), m_width(width), m_height(height), m_name(name)
{
	// Log physical device info
	{
		VkPhysicalDeviceProperties props{};
		vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
		cemuLog_log(LogType::Force, "VulkanFrameStreamer [{}]: Creating ({}x{}). Physical device: {} (vendor=0x{:04x})",
					m_name, m_width, m_height, props.deviceName, props.vendorID);
	}

	// Load extension function pointers
	m_vkGetImageSubresourceLayout = reinterpret_cast<PFN_vkGetImageSubresourceLayout_t>(
		vkGetDeviceProcAddr(m_device, "vkGetImageSubresourceLayoutEXT"));
	if (!m_vkGetImageSubresourceLayout)
		m_vkGetImageSubresourceLayout = reinterpret_cast<PFN_vkGetImageSubresourceLayout_t>(
			vkGetDeviceProcAddr(m_device, "vkGetImageSubresourceLayout"));

	m_vkGetMemoryFdKHR = reinterpret_cast<PFN_vkGetMemoryFdKHR_t>(
		vkGetDeviceProcAddr(m_device, "vkGetMemoryFdKHR"));

	m_vkGetImageDrmFormatModifier = reinterpret_cast<PFN_vkGetImageDrmFormatModifierPropertiesEXT_t>(
		vkGetDeviceProcAddr(m_device, "vkGetImageDrmFormatModifierPropertiesEXT"));

	if (!m_vkGetMemoryFdKHR)
	{
		cemuLog_log(LogType::Force, "VulkanFrameStreamer [{}]: vkGetMemoryFdKHR not available, streaming disabled", m_name);
		return;
	}

	bool allOk = true;
	for (size_t i = 0; i < NUM_FRAMES; ++i)
	{
		if (!CreateFrameResources(m_frames[i]))
		{
			allOk = false;
			break;
		}
	}

	if (!allOk)
	{
		for (size_t i = 0; i < NUM_FRAMES; ++i)
			DestroyFrameResources(m_frames[i]);
		return;
	}

	m_supported = true;
	cemuLog_log(LogType::Force, "VulkanFrameStreamer [{}]: Initialized ({}x{}, modifier=0x{:x})",
				m_name, m_width, m_height, m_drmModifier);
}

VulkanFrameStreamer::~VulkanFrameStreamer()
{
	Stop();
	for (size_t i = 0; i < NUM_FRAMES; ++i)
		DestroyFrameResources(m_frames[i]);
}

void VulkanFrameStreamer::Start(const std::string& targetIP, uint16 targetPort,
								uint32 bitrateKbps, uint32 qp)
{
	if (!m_supported)
	{
		cemuLog_log(LogType::Force, "VulkanFrameStreamer [{}]: Not supported, cannot start", m_name);
		return;
	}

	if (m_active)
		Stop();

#ifdef HAVE_GSTREAMER
	InitGstPipeline(targetIP, targetPort, bitrateKbps, qp);
	m_active = (m_pipeline != nullptr);
	if (m_active)
		cemuLog_log(LogType::Force, "VulkanFrameStreamer [{}]: Streaming to {}:{} at {} kbps, QP={}",
					m_name, targetIP, targetPort, bitrateKbps, qp);
#else
	cemuLog_log(LogType::Force, "VulkanFrameStreamer [{}]: GStreamer not available", m_name);
#endif
}

void VulkanFrameStreamer::Stop()
{
#ifdef HAVE_GSTREAMER
	CleanupGstPipeline();
#endif
	m_active = false;
}

bool VulkanFrameStreamer::CreateFrameResources(FrameResources& frame)
{
	// Create image with external memory support
	VkExternalMemoryImageCreateInfo externalInfo{};
	externalInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
	externalInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

	VkImageCreateInfo imageInfo{};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.pNext = &externalInfo;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = m_format;
	imageInfo.extent = {m_width, m_height, 1};
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling = VK_IMAGE_TILING_LINEAR;
	imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	if (vkCreateImage(m_device, &imageInfo, nullptr, &frame.image) != VK_SUCCESS)
	{
		cemuLog_log(LogType::Force, "VulkanFrameStreamer [{}]: Failed to create image", m_name);
		return false;
	}

	// Find suitable memory type (prefer DEVICE_LOCAL | HOST_VISIBLE for DMA-BUF export)
	VkMemoryRequirements memReqs{};
	vkGetImageMemoryRequirements(m_device, frame.image, &memReqs);

	VkPhysicalDeviceMemoryProperties memProps{};
	vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProps);

	uint32 memoryTypeIndex = 0;
	bool found = false;
	for (uint32 i = 0; i < memProps.memoryTypeCount; ++i)
	{
		if ((memReqs.memoryTypeBits & (1 << i)) &&
			(memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
		{
			memoryTypeIndex = i;
			found = true;
			if (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
				break;
		}
	}
	if (!found)
	{
		cemuLog_log(LogType::Force, "VulkanFrameStreamer [{}]: Failed to find suitable memory type", m_name);
		vkDestroyImage(m_device, frame.image, nullptr);
		frame.image = VK_NULL_HANDLE;
		return false;
	}

	// Allocate with export support
	VkExportMemoryAllocateInfo exportAllocInfo{};
	exportAllocInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
	exportAllocInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

	VkMemoryAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.pNext = &exportAllocInfo;
	allocInfo.allocationSize = memReqs.size;
	allocInfo.memoryTypeIndex = memoryTypeIndex;

	if (vkAllocateMemory(m_device, &allocInfo, nullptr, &frame.memory) != VK_SUCCESS)
	{
		cemuLog_log(LogType::Force, "VulkanFrameStreamer [{}]: Failed to allocate exportable memory", m_name);
		vkDestroyImage(m_device, frame.image, nullptr);
		frame.image = VK_NULL_HANDLE;
		return false;
	}

	vkBindImageMemory(m_device, frame.image, frame.memory, 0);

	// Get DRM format modifier
	if (m_vkGetImageDrmFormatModifier)
	{
		VkImageDrmFormatModifierPropertiesEXT modifierProps{};
		modifierProps.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
		if (m_vkGetImageDrmFormatModifier(m_device, frame.image, &modifierProps) == VK_SUCCESS)
			m_drmModifier = modifierProps.drmFormatModifier;
	}

	// Get stride from subresource layout
	if (m_vkGetImageSubresourceLayout)
	{
		VkImageSubresource subres{};
		subres.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		VkSubresourceLayout layout{};
		m_vkGetImageSubresourceLayout(m_device, frame.image, &subres, &layout);
		frame.stride = static_cast<uint32>(layout.rowPitch);
	}
	else
	{
		frame.stride = m_width * 4;
	}

	const uint32 expectedStride = m_width * 4;
	if (frame.stride != expectedStride)
	{
		cemuLog_log(LogType::Force, "VulkanFrameStreamer [{}]: STRIDE MISMATCH! rowPitch={} expected {} (width*4). "
					"GStreamer needs GstVideoMeta for non-tight stride!",
					m_name, frame.stride, expectedStride);
	}

	// Create image view
	VkImageViewCreateInfo viewInfo{};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = frame.image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = m_format;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.baseMipLevel = 0;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.baseArrayLayer = 0;
	viewInfo.subresourceRange.layerCount = 1;

	if (vkCreateImageView(m_device, &viewInfo, nullptr, &frame.imageView) != VK_SUCCESS)
	{
		cemuLog_log(LogType::Force, "VulkanFrameStreamer [{}]: Failed to create image view", m_name);
		vkDestroyImage(m_device, frame.image, nullptr);
		frame.image = VK_NULL_HANDLE;
		vkFreeMemory(m_device, frame.memory, nullptr);
		frame.memory = VK_NULL_HANDLE;
		return false;
	}

	// Create HOST_CACHED staging buffer for fast CPU reads.
	// Linear VkImages cannot use HOST_CACHED memory on RADV, but VkBuffers can.
	// The GPU copies the image to this buffer via vkCmdCopyImageToBuffer,
	// then the CPU reads from cached (write-back) memory instead of write-combining.
	const gsize stagingSize = frame.stride * m_height;
	{
		VkBufferCreateInfo bufInfo{};
		bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufInfo.size = stagingSize;
		bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		if (vkCreateBuffer(m_device, &bufInfo, nullptr, &frame.stagingBuffer) != VK_SUCCESS)
		{
			cemuLog_log(LogType::Force, "VulkanFrameStreamer [{}]: Failed to create staging buffer", m_name);
			return true; // non-fatal, fall back to DMA-BUF path
		}

		VkMemoryRequirements bufMemReqs{};
		vkGetBufferMemoryRequirements(m_device, frame.stagingBuffer, &bufMemReqs);

		// Find HOST_CACHED | HOST_VISIBLE memory type
		uint32 stagingMemType = UINT32_MAX;
		for (uint32 i = 0; i < memProps.memoryTypeCount; ++i)
		{
			if ((bufMemReqs.memoryTypeBits & (1 << i)) &&
				(memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
				(memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
			{
				stagingMemType = i;
				break;
			}
		}
		// Fallback: HOST_VISIBLE | HOST_COHERENT if no HOST_CACHED
		if (stagingMemType == UINT32_MAX)
		{
			for (uint32 i = 0; i < memProps.memoryTypeCount; ++i)
			{
				if ((bufMemReqs.memoryTypeBits & (1 << i)) &&
					(memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
				{
					stagingMemType = i;
					break;
				}
			}
		}

		if (stagingMemType == UINT32_MAX)
		{
			cemuLog_log(LogType::Force, "VulkanFrameStreamer [{}]: Failed to find staging buffer memory type", m_name);
			vkDestroyBuffer(m_device, frame.stagingBuffer, nullptr);
			frame.stagingBuffer = VK_NULL_HANDLE;
			return true; // non-fatal
		}

		VkMemoryAllocateInfo stagingAllocInfo{};
		stagingAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		stagingAllocInfo.allocationSize = bufMemReqs.size;
		stagingAllocInfo.memoryTypeIndex = stagingMemType;

		if (vkAllocateMemory(m_device, &stagingAllocInfo, nullptr, &frame.stagingMemory) != VK_SUCCESS)
		{
			cemuLog_log(LogType::Force, "VulkanFrameStreamer [{}]: Failed to allocate staging memory", m_name);
			vkDestroyBuffer(m_device, frame.stagingBuffer, nullptr);
			frame.stagingBuffer = VK_NULL_HANDLE;
			return true; // non-fatal
		}

		vkBindBufferMemory(m_device, frame.stagingBuffer, frame.stagingMemory, 0);

		// Persistently map the staging buffer
		if (vkMapMemory(m_device, frame.stagingMemory, 0, stagingSize, 0, &frame.stagingMappedPtr) != VK_SUCCESS)
		{
			cemuLog_log(LogType::Force, "VulkanFrameStreamer [{}]: Failed to map staging memory", m_name);
			vkDestroyBuffer(m_device, frame.stagingBuffer, nullptr);
			vkFreeMemory(m_device, frame.stagingMemory, nullptr);
			frame.stagingBuffer = VK_NULL_HANDLE;
			frame.stagingMemory = VK_NULL_HANDLE;
			return true; // non-fatal
		}

		cemuLog_log(LogType::Force, "VulkanFrameStreamer [{}]: Buffer created ({}x{}, modifier=0x{:x}, stride={})",
					m_name, m_width, m_height, m_drmModifier, frame.stride);
	}

	return true;
}

void VulkanFrameStreamer::DestroyFrameResources(FrameResources& frame)
{
	if (frame.cachedFd >= 0)
	{
		close(frame.cachedFd);
		frame.cachedFd = -1;
	}
	if (frame.stagingMappedPtr)
	{
		vkUnmapMemory(m_device, frame.stagingMemory);
		frame.stagingMappedPtr = nullptr;
	}
	if (frame.stagingBuffer)
	{
		vkDestroyBuffer(m_device, frame.stagingBuffer, nullptr);
		frame.stagingBuffer = VK_NULL_HANDLE;
	}
	if (frame.stagingMemory)
	{
		vkFreeMemory(m_device, frame.stagingMemory, nullptr);
		frame.stagingMemory = VK_NULL_HANDLE;
	}
	if (frame.imageView)
	{
		vkDestroyImageView(m_device, frame.imageView, nullptr);
		frame.imageView = VK_NULL_HANDLE;
	}
	if (frame.image)
	{
		vkDestroyImage(m_device, frame.image, nullptr);
		frame.image = VK_NULL_HANDLE;
	}
	if (frame.memory)
	{
		vkFreeMemory(m_device, frame.memory, nullptr);
		frame.memory = VK_NULL_HANDLE;
	}
}

int VulkanFrameStreamer::ExportDmaBuf(FrameResources& frame)
{
	if (!frame.memory || !m_vkGetMemoryFdKHR)
		return -1;

	if (frame.cachedFd >= 0)
		return dup(frame.cachedFd);

	VkMemoryGetFdInfoKHR fdInfo{};
	fdInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
	fdInfo.memory = frame.memory;
	fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

	if (m_vkGetMemoryFdKHR(m_device, &fdInfo, &frame.cachedFd) != VK_SUCCESS)
	{
		cemuLog_log(LogType::Force, "VulkanFrameStreamer [{}]: Failed to export DMA-BUF fd", m_name);
		return -1;
	}

	return dup(frame.cachedFd);
}

static void MakeImageBarrier(VkImageMemoryBarrier& barrier, VkImage image,
							  VkImageLayout oldLayout, VkImageLayout newLayout,
							  VkAccessFlags srcAccess, VkAccessFlags dstAccess)
{
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = srcAccess;
	barrier.dstAccessMask = dstAccess;
	barrier.oldLayout = oldLayout;
	barrier.newLayout = newLayout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount = 1;
}

bool VulkanFrameStreamer::RecordBlit(VkCommandBuffer cmdbuf, VkImage source,
									 uint32 srcWidth, uint32 srcHeight,
									 uint32 srcOffsetX, uint32 srcOffsetY,
									 VkImageLayout srcLayout)
{
	FrameResources& frame = m_frames[m_writeIndex];
	if (!frame.image)
		return false;

	// Transition source to transfer src
	VkImageMemoryBarrier srcBarrier{};
	MakeImageBarrier(srcBarrier, source,
					 srcLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					 VK_ACCESS_MEMORY_READ_BIT,
					 VK_ACCESS_TRANSFER_READ_BIT);
	vkCmdPipelineBarrier(cmdbuf,
						 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
						 VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
						 0, nullptr, 0, nullptr, 1, &srcBarrier);

	// Transition destination to transfer dst
	VkImageMemoryBarrier dstBarrier{};
	MakeImageBarrier(dstBarrier, frame.image,
					 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					 VK_ACCESS_NONE, VK_ACCESS_TRANSFER_WRITE_BIT);
	vkCmdPipelineBarrier(cmdbuf,
						 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
						 VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
						 0, nullptr, 0, nullptr, 1, &dstBarrier);

	// Blit
	VkImageBlit blit{};
	blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.srcSubresource.mipLevel = 0;
	blit.srcSubresource.baseArrayLayer = 0;
	blit.srcSubresource.layerCount = 1;
	blit.srcOffsets[0] = {(sint32)srcOffsetX, (sint32)srcOffsetY, 0};
	blit.srcOffsets[1] = {(sint32)(srcOffsetX + srcWidth), (sint32)(srcOffsetY + srcHeight), 1};
	blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.dstSubresource.mipLevel = 0;
	blit.dstSubresource.baseArrayLayer = 0;
	blit.dstSubresource.layerCount = 1;
	blit.dstOffsets[0] = {0, 0, 0};
	blit.dstOffsets[1] = {(sint32)m_width, (sint32)m_height, 1};

	vkCmdBlitImage(cmdbuf,
				   source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				   frame.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				   1, &blit, VK_FILTER_LINEAR);

	// GPU copy: LINEAR image → HOST_CACHED staging buffer
	if (frame.stagingBuffer)
	{
		VkBufferImageCopy copyRegion{};
		copyRegion.bufferOffset = 0;
		copyRegion.bufferRowLength = frame.stride / 4;
		copyRegion.bufferImageHeight = m_height;
		copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copyRegion.imageSubresource.mipLevel = 0;
		copyRegion.imageSubresource.baseArrayLayer = 0;
		copyRegion.imageSubresource.layerCount = 1;
		copyRegion.imageOffset = {0, 0, 0};
		copyRegion.imageExtent = {m_width, m_height, 1};

		vkCmdCopyImageToBuffer(cmdbuf,
							  frame.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
							  frame.stagingBuffer, 1, &copyRegion);
	}

	// Restore source layout
	VkImageMemoryBarrier srcRestore{};
	MakeImageBarrier(srcRestore, source,
					 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, srcLayout,
					 VK_ACCESS_TRANSFER_READ_BIT,
					 VK_ACCESS_MEMORY_READ_BIT);

	// Transition frame to general
	VkImageMemoryBarrier finalBarrier{};
	MakeImageBarrier(finalBarrier, frame.image,
					 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
					 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT);

	// Use array to ensure contiguous layout
	VkImageMemoryBarrier barriers[2] = {srcRestore, finalBarrier};
	vkCmdPipelineBarrier(cmdbuf,
						 VK_PIPELINE_STAGE_TRANSFER_BIT,
						 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
						 0, nullptr, 0, nullptr, 2, barriers);

	return true;
}

void VulkanFrameStreamer::PushFrame()
{
#ifdef HAVE_GSTREAMER
	if (!m_active || !m_appsrc || !m_allocator)
		return;

	const uint32 pushIndex = m_writeIndex;
	m_writeIndex = (m_writeIndex + 1) % NUM_FRAMES;

	FrameResources& frame = m_frames[pushIndex];
	const gsize bufSize = frame.stride * m_height;

	GstBuffer* buf = nullptr;

	if (frame.stagingMappedPtr)
	{
		// Fast path: copy from HOST_CACHED staging buffer (write-back cached, fast CPU reads)
		buf = gst_buffer_new_allocate(nullptr, bufSize, nullptr);
		if (!buf)
			return;
		GstMapInfo map;
		gst_buffer_map(buf, &map, GST_MAP_WRITE);
		memcpy(map.data, frame.stagingMappedPtr, bufSize);
		gst_buffer_unmap(buf, &map);
	}
	else
	{
		// Fallback: DMA-BUF export (write-combining memory, slow CPU reads)
		const int fd = ExportDmaBuf(frame);
		if (fd < 0)
			return;
		GstMemory* mem = gst_dmabuf_allocator_alloc(m_allocator, fd, bufSize);
		if (!mem)
		{
			close(fd);
			return;
		}
		buf = gst_buffer_new();
		if (!buf)
		{
			gst_memory_unref(mem);
			return;
		}
		gst_buffer_append_memory(buf, mem);
	}

	// GstVideoMeta so GStreamer knows the actual stride (may differ from width*4)
	gsize offsets[GST_VIDEO_MAX_PLANES] = {};
	gint strides[GST_VIDEO_MAX_PLANES] = {};
	offsets[0] = 0;
	strides[0] = static_cast<gint>(frame.stride);
	gst_buffer_add_video_meta_full(buf, GST_VIDEO_FRAME_FLAG_NONE,
		GST_VIDEO_FORMAT_RGBA, m_width, m_height, 1, offsets, strides);

	GST_BUFFER_PTS(buf) = gst_element_get_current_running_time(m_pipeline);
	GST_BUFFER_DTS(buf) = GST_BUFFER_PTS(buf);
	GST_BUFFER_DURATION(buf) = GST_CLOCK_TIME_NONE;

	GstFlowReturn ret;
	g_signal_emit_by_name(m_appsrc, "push-buffer", buf, &ret);
	gst_buffer_unref(buf);

	if (ret != GST_FLOW_OK)
		cemuLog_log(LogType::Force, "VulkanFrameStreamer [{}]: push failed: {}", m_name, (int)ret);
#endif
}

#ifdef HAVE_GSTREAMER

#include <filesystem>
#include <fstream>

// Auto-detect the DRM render node matching the Vulkan physical device's vendor ID.
static std::string FindMatchingDrmRenderNode(VkPhysicalDevice physicalDevice)
{
	VkPhysicalDeviceProperties props{};
	vkGetPhysicalDeviceProperties(physicalDevice, &props);

	const uint32 vendorID = props.vendorID;
	std::string expectedVendor;
	if (vendorID == 0x1002) expectedVendor = "0x1002"; // AMD
	else if (vendorID == 0x8086) expectedVendor = "0x8086"; // Intel
	else if (vendorID == 0x10de) expectedVendor = "0x10de"; // NVIDIA
	else return {};

	try
	{
		for (const auto& entry : std::filesystem::directory_iterator("/sys/class/drm"))
		{
			std::string name = entry.path().filename().string();
			if (name.find("renderD") != 0)
				continue;

			std::string vendorPath = entry.path().string() + "/device/vendor";
			std::ifstream vendorFile(vendorPath);
			if (!vendorFile.is_open())
				continue;

			std::string fileVendor;
			vendorFile >> fileVendor;
			if (fileVendor != expectedVendor)
				continue;

			std::string renderNode = "/dev/dri/" + name;
			if (std::filesystem::exists(renderNode))
				return renderNode;
		}
	}
	catch (...) {}

	return {};
}

void VulkanFrameStreamer::InitGstPipeline(const std::string& targetIP, uint16 targetPort,
										   uint32 bitrateKbps, uint32 qp)
{
	// Set GST_VAAPI_DRM_DEVICE BEFORE gst_init() so VAAPI plugin uses the correct device.
	const bool wasAlreadyInited = gst_is_initialized();

	CemuConfig& cfg = GetConfig();
	const auto encoder = static_cast<StreamingEncoder>(cfg.streaming_encoder.GetValue());
	const std::string gpuDevice = cfg.streaming_gpu_device.GetValue();

	if (!gpuDevice.empty())
	{
		g_setenv("GST_VAAPI_DRM_DEVICE", gpuDevice.c_str(), TRUE);
	}
	else
	{
		std::string autoDevice = FindMatchingDrmRenderNode(m_physicalDevice);
		if (!autoDevice.empty())
			g_setenv("GST_VAAPI_DRM_DEVICE", autoDevice.c_str(), TRUE);
	}

	if (!wasAlreadyInited)
		gst_init(nullptr, nullptr);

	std::string encDesc;
	switch (encoder)
	{
	case StreamingEncoder::VAAPI:
		encDesc = "vaapih264enc name=enc rate-control=cqp init-qp=" + std::to_string(qp) + " qp-ip=1";
		break;
	case StreamingEncoder::VAAPI_LowPower:
	{
		// vah264lpenc is not supported on AMD GPUs, fall back to normal vaapih264enc
		const char* drmDevice = g_getenv("GST_VAAPI_DRM_DEVICE");
		const bool isAmd = drmDevice && (strstr(drmDevice, "renderD") != nullptr);
		if (isAmd)
		{
			std::string vendorPath = std::string("/sys/class/drm/") +
				std::string(drmDevice).substr(std::string(drmDevice).rfind('/') + 1) + "/device/vendor";
			std::ifstream vFile(vendorPath);
			std::string vendor;
			if (vFile >> vendor && vendor == "0x1002")
			{
				encDesc = "vaapih264enc name=enc rate-control=cqp init-qp=" + std::to_string(qp) + " qp-ip=1";
				break;
			}
		}
		encDesc = "vah264lpenc name=enc rate-control=cqp init-qp=" + std::to_string(qp) + " qp-ip=1";
		break;
	}
	case StreamingEncoder::x264:
		encDesc = "x264enc name=enc tune=zerolatency speed-preset=ultrafast key-int-max=30 bitrate=" + std::to_string(bitrateKbps);
		break;
	case StreamingEncoder::OpenH264:
		encDesc = "openh264enc name=enc complexity=low bitrate=" + std::to_string(bitrateKbps);
		break;
	case StreamingEncoder::Auto:
	default:
	{
		GstElement* test = gst_element_factory_make("vaapih264enc", nullptr);
		if (test)
		{
			gst_object_unref(test);
			encDesc = "vaapih264enc name=enc rate-control=cqp init-qp=" + std::to_string(qp) + " qp-ip=1";
		}
		else
		{
			encDesc = "x264enc name=enc tune=zerolatency speed-preset=ultrafast key-int-max=30 bitrate=" + std::to_string(bitrateKbps);
		}
		break;
	}
	}

	std::string pipelineDesc =
		"appsrc name=src is-live=true format=3 block=false max-buffers=2 "
		"! queue name=q max-size-buffers=2 leaky=downstream "
		"! videoconvert name=vc ! " +
		encDesc +
		" ! h264parse name=parse "
		"! rtph264pay name=pay config-interval=1 pt=96 "
		"! udpsink name=sink host=" +
		targetIP + " port=" + std::to_string(targetPort) + " sync=false";

	cemuLog_log(LogType::Force, "VulkanFrameStreamer [{}]: Pipeline: {}", m_name, pipelineDesc);

	GError* error = nullptr;
	m_pipeline = gst_parse_launch(pipelineDesc.c_str(), &error);
	if (error)
	{
		cemuLog_log(LogType::Force, "VulkanFrameStreamer [{}]: Pipeline parse error: {}", m_name, error->message);
		g_error_free(error);
		m_pipeline = nullptr;
		return;
	}

	m_appsrc = gst_bin_get_by_name(GST_BIN(m_pipeline), "src");
	if (!m_appsrc)
	{
		cemuLog_log(LogType::Force, "VulkanFrameStreamer [{}]: Could not find appsrc", m_name);
		gst_object_unref(m_pipeline);
		m_pipeline = nullptr;
		return;
	}

	GstVideoInfo vinfo;
	gst_video_info_set_format(&vinfo, GST_VIDEO_FORMAT_RGBA, m_width, m_height);
	GstCaps* caps = gst_video_info_to_caps(&vinfo);
	g_object_set(m_appsrc, "caps", caps, nullptr);
	g_object_set(m_appsrc, "block", FALSE, "max-buffers", (guint)2, "leaky", 2 /* downstream */, nullptr);
	gst_caps_unref(caps);

	if (!m_allocator)
		m_allocator = gst_dmabuf_allocator_new();

	GstStateChangeReturn ret = gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
	if (ret == GST_STATE_CHANGE_FAILURE)
	{
		cemuLog_log(LogType::Force, "VulkanFrameStreamer [{}]: Failed to start pipeline", m_name);
		gst_object_unref(m_appsrc);
		m_appsrc = nullptr;
		gst_object_unref(m_pipeline);
		m_pipeline = nullptr;
		return;
	}

	cemuLog_log(LogType::Force, "VulkanFrameStreamer [{}]: Pipeline started ({}x{})", m_name, m_width, m_height);
}

void VulkanFrameStreamer::CleanupGstPipeline()
{
	if (m_pipeline)
		gst_element_set_state(m_pipeline, GST_STATE_NULL);
	if (m_appsrc)
	{
		gst_object_unref(m_appsrc);
		m_appsrc = nullptr;
	}
	if (m_pipeline)
	{
		gst_object_unref(m_pipeline);
		m_pipeline = nullptr;
	}
	if (m_allocator)
	{
		gst_object_unref(m_allocator);
		m_allocator = nullptr;
	}
}

#else

void VulkanFrameStreamer::InitGstPipeline(const std::string&, uint16, uint32, uint32) {}
void VulkanFrameStreamer::CleanupGstPipeline() {}

#endif // HAVE_GSTREAMER

#endif // ENABLE_VULKAN
