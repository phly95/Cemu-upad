#include "Cafe/HW/Latte/Renderer/OpenGL/GLFrameStreamer.h"

#ifdef ENABLE_OPENGL

#include <cstring>
#include <vector>

#include "Common/Log.h"
#include "config/CemuConfig.h"

GLFrameStreamer::GLFrameStreamer() = default;

GLFrameStreamer::~GLFrameStreamer()
{
	Stop();
}

bool GLFrameStreamer::IsSupported()
{
#ifdef HAVE_GSTREAMER
	if (!gst_is_initialized())
	{
		GError* error = nullptr;
		if (!gst_init_check(nullptr, nullptr, &error))
		{
			if (error) g_error_free(error);
			return false;
		}
	}
	return true;
#else
	return false;
#endif
}

void GLFrameStreamer::Start(const std::string& targetIP, uint16 targetPort,
							uint32 bitrateKbps, uint32 qp)
{
	if (m_active) Stop();

	m_width = 1280;
	m_height = 720;

	glGenFramebuffers(1, &m_fbo);

	glGenTextures(1, &m_tex);
	glBindTexture(GL_TEXTURE_2D, m_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_width, m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glBindTexture(GL_TEXTURE_2D, 0);

	glGenBuffers(NUM_PBO_BUFFERS, m_pbo);
	for (int i = 0; i < NUM_PBO_BUFFERS; i++)
	{
		glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[i]);
		glBufferData(GL_PIXEL_PACK_BUFFER, m_width * m_height * 4, nullptr, GL_STREAM_READ);
	}
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	m_pboPrimed = false;
	m_currentPBO = 0;

#ifdef HAVE_GSTREAMER
	InitGstPipeline(targetIP, targetPort, bitrateKbps, qp);
	m_active = (m_pipeline != nullptr);
#else
	m_active = false;
#endif

	if (m_active)
		cemuLog_log(LogType::Force, "GLFrameStreamer: Streaming to {}:{} ({}x{})",
					targetIP.c_str(), targetPort, m_width, m_height);
}

void GLFrameStreamer::Stop()
{
	if (!m_active) return;

#ifdef HAVE_GSTREAMER
	CleanupGstPipeline();
#endif

	for (int i = 0; i < NUM_PBO_BUFFERS; i++)
	{
		if (m_pboFence[i])
		{
			glDeleteSync(m_pboFence[i]);
			m_pboFence[i] = nullptr;
		}
	}
	if (m_fbo) { glDeleteFramebuffers(1, &m_fbo); m_fbo = 0; }
	if (m_tex) { glDeleteTextures(1, &m_tex); m_tex = 0; }
	if (m_pbo[0] || m_pbo[1]) { glDeleteBuffers(NUM_PBO_BUFFERS, m_pbo); m_pbo[0] = m_pbo[1] = 0; }
	m_active = false;
	m_pboPrimed = false;
}

bool GLFrameStreamer::PushFrame(GLuint texId, uint32 srcWidth, uint32 srcHeight)
{
	if (!m_active)
		return false;

	if (!glIsTexture(texId))
	{
		cemuLog_log(LogType::Force, "GLFrameStreamer: Invalid texture id {}", texId);
		return false;
	}

	// Blit source texture to our internal texture at stream resolution
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_fbo);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_tex, 0);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);

	GLuint srcFbo;
	glGenFramebuffers(1, &srcFbo);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texId, 0);
	glReadBuffer(GL_COLOR_ATTACHMENT0);

	glBlitFramebuffer(0, 0, srcWidth, srcHeight, 0, 0, m_width, m_height,
					  GL_COLOR_BUFFER_BIT, GL_LINEAR);

	glDeleteFramebuffers(1, &srcFbo);

	// Read from target texture into PBO
	glBindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo);
	glReadBuffer(GL_COLOR_ATTACHMENT0);

	glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[m_currentPBO]);
	glReadPixels(0, 0, m_width, m_height, GL_RGBA, GL_UNSIGNED_BYTE, 0);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

	// Insert fence to track when this PBO readback completes
	if (m_pboFence[m_currentPBO])
		glDeleteSync(m_pboFence[m_currentPBO]);
	m_pboFence[m_currentPBO] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

#ifdef HAVE_GSTREAMER
	// Map previous frame's PBO and push to GStreamer
	if (m_pboPrimed && m_pipeline && m_appsrc)
	{
		int readPBO = (m_currentPBO + 1) % NUM_PBO_BUFFERS;

		// Wait for this PBO's readback to complete on the GPU
		if (m_pboFence[readPBO])
		{
			glClientWaitSync(m_pboFence[readPBO], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
			glDeleteSync(m_pboFence[readPBO]);
			m_pboFence[readPBO] = nullptr;
		}

		glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[readPBO]);
		GLubyte* pixels = (GLubyte*)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0,
							m_width * m_height * 4, GL_MAP_READ_BIT);
		if (!pixels)
		{
			glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
			m_currentPBO = (m_currentPBO + 1) % NUM_PBO_BUFFERS;
			return false;
		}

		std::vector<uint8> frame(m_width * m_height * 4);
		memcpy(frame.data(), pixels, m_width * m_height * 4);

		glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

		GstBuffer* buf = gst_buffer_new_allocate(nullptr, m_width * m_height * 4, nullptr);
		if (!buf)
		{
			m_currentPBO = (m_currentPBO + 1) % NUM_PBO_BUFFERS;
			return false;
		}

		GstMapInfo map;
		gst_buffer_map(buf, &map, GST_MAP_WRITE);
		memcpy(map.data, frame.data(), m_width * m_height * 4);
		gst_buffer_unmap(buf, &map);

		GST_BUFFER_PTS(buf) = m_frameTimestamp;
		GST_BUFFER_DTS(buf) = m_frameTimestamp;
		GST_BUFFER_DURATION(buf) = gst_util_uint64_scale_int(1, GST_SECOND, 60);
		m_frameTimestamp += GST_BUFFER_DURATION(buf);

		GstFlowReturn ret;
		g_signal_emit_by_name(m_appsrc, "push-buffer", buf, &ret);
		gst_buffer_unref(buf);

		if (ret != GST_FLOW_OK)
		{
			cemuLog_log(LogType::Force, "GLFrameStreamer: Push failed: {}", (int)ret);
			m_currentPBO = (m_currentPBO + 1) % NUM_PBO_BUFFERS;
			return false;
		}
	}
#endif

	m_currentPBO = (m_currentPBO + 1) % NUM_PBO_BUFFERS;
	m_pboPrimed = true;
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	return true;
}

#ifdef HAVE_GSTREAMER

void GLFrameStreamer::InitGstPipeline(const std::string& targetIP, uint16 targetPort,
									   uint32 bitrateKbps, uint32 qp)
{
	if (!gst_is_initialized())
	{
		CemuConfig& cfg = GetConfig();
		const std::string gpuDevice = cfg.streaming_gpu_device.GetValue();
		if (!gpuDevice.empty())
			g_setenv("GST_VAAPI_DRM_DEVICE", gpuDevice.c_str(), TRUE);
		gst_init(nullptr, nullptr);
	}

	CemuConfig& cfg = GetConfig();
	const auto encoder = static_cast<StreamingEncoder>(cfg.streaming_encoder.GetValue());

	std::string encDesc;
	switch (encoder)
	{
	case StreamingEncoder::VAAPI:
		encDesc = "vaapih264enc rate-control=cqp init-qp=" + std::to_string(qp) + " qp-ip=1";
		break;
	case StreamingEncoder::VAAPI_LowPower:
		encDesc = "vah264lpenc rate-control=cqp init-qp=" + std::to_string(qp) + " qp-ip=1";
		break;
	case StreamingEncoder::x264:
		encDesc = "x264enc tune=zerolatency speed-preset=ultrafast key-int-max=30 bitrate=" + std::to_string(bitrateKbps);
		break;
	case StreamingEncoder::OpenH264:
		encDesc = "openh264enc complexity=low bitrate=" + std::to_string(bitrateKbps);
		break;
	case StreamingEncoder::Auto:
	default:
	{
		GstElement* test = gst_element_factory_make("vaapih264enc", nullptr);
		if (test)
		{
			gst_object_unref(test);
			encDesc = "vaapih264enc rate-control=cqp init-qp=" + std::to_string(qp) + " qp-ip=1";
		}
		else
		{
			encDesc = "x264enc tune=zerolatency speed-preset=ultrafast key-int-max=30 bitrate=" + std::to_string(bitrateKbps);
		}
		break;
	}
	}

	std::string pipelineDesc =
		"appsrc name=src is-live=true format=3 "
		"! videoconvert ! " +
		encDesc +
		" ! h264parse "
		"! rtph264pay config-interval=1 pt=96 "
		"! udpsink host=" +
		targetIP + " port=" + std::to_string(targetPort);

	cemuLog_log(LogType::Force, "GLFrameStreamer: Pipeline: {}", pipelineDesc);

	GError* error = nullptr;
	m_pipeline = gst_parse_launch(pipelineDesc.c_str(), &error);
	if (error)
	{
		cemuLog_log(LogType::Force, "GLFrameStreamer: Pipeline parse error: {}", error->message);
		g_error_free(error);
		m_pipeline = nullptr;
		return;
	}

	m_appsrc = gst_bin_get_by_name(GST_BIN(m_pipeline), "src");
	if (!m_appsrc)
	{
		cemuLog_log(LogType::Force, "GLFrameStreamer: Could not find appsrc");
		gst_object_unref(m_pipeline);
		m_pipeline = nullptr;
		return;
	}

	GstVideoInfo vinfo;
	gst_video_info_set_format(&vinfo, GST_VIDEO_FORMAT_RGBA, m_width, m_height);
	GstCaps* caps = gst_video_info_to_caps(&vinfo);
	g_object_set(m_appsrc, "caps", caps, nullptr);
	gst_caps_unref(caps);

	GstStateChangeReturn ret = gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
	if (ret == GST_STATE_CHANGE_FAILURE)
	{
		cemuLog_log(LogType::Force, "GLFrameStreamer: Failed to start pipeline");
		gst_object_unref(m_appsrc);
		m_appsrc = nullptr;
		gst_object_unref(m_pipeline);
		m_pipeline = nullptr;
		return;
	}

	m_frameTimestamp = 0;
	cemuLog_log(LogType::Force, "GLFrameStreamer: Pipeline started ({}x{})", m_width, m_height);
}

void GLFrameStreamer::CleanupGstPipeline()
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
}

#endif // HAVE_GSTREAMER

#endif // ENABLE_OPENGL
