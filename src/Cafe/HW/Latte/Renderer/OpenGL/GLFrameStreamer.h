#pragma once

#ifdef ENABLE_OPENGL

#include <string>
#include <atomic>
#include <vector>
#include <cstdint>

#include "glad/glad.h"

#ifdef HAVE_GSTREAMER
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#endif

class GLFrameStreamer
{
public:
	GLFrameStreamer();
	~GLFrameStreamer();

	GLFrameStreamer(const GLFrameStreamer&) = delete;
	GLFrameStreamer& operator=(const GLFrameStreamer&) = delete;

	bool IsSupported();
	bool IsActive() const { return m_active; }

	void Start(const std::string& targetIP, uint16 targetPort, uint32 bitrateKbps, uint32 qp);
	void Stop();

	// Push a frame from the current OpenGL context
	// texId: the OpenGL texture ID containing the frame
	// srcWidth/srcHeight: dimensions of the source texture
	bool PushFrame(GLuint texId, uint32 srcWidth, uint32 srcHeight);

	uint32 GetWidth() const { return m_width; }
	uint32 GetHeight() const { return m_height; }

private:
#ifdef HAVE_GSTREAMER
	void InitGstPipeline(const std::string& targetIP, uint16 targetPort, uint32 bitrateKbps, uint32 qp);
	void CleanupGstPipeline();
#endif

	bool m_active = false;
	uint32 m_width = 0;
	uint32 m_height = 0;

	GLuint m_fbo = 0;
	GLuint m_tex = 0;

	static constexpr int NUM_PBO_BUFFERS = 2;
	GLuint m_pbo[NUM_PBO_BUFFERS] = {};
	GLsync m_pboFence[NUM_PBO_BUFFERS] = {};
	int m_currentPBO = 0;
	bool m_pboPrimed = false;

#ifdef HAVE_GSTREAMER
	GstElement* m_pipeline = nullptr;
	GstElement* m_appsrc = nullptr;
	GstClockTime m_frameTimestamp = 0;
#endif
};

#endif // ENABLE_OPENGL
