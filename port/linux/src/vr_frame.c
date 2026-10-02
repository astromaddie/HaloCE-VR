/*
VR_FRAME.C

The game loop's side of the OpenXR session (vr.h). The host
(port/android/host/host_xr.c) waits for, begins and ends the runtime's
frames; this side decides what each frame shows and draws it into the
swapchain images, whose GL texture names work in this context as they are.
*/

#ifdef HALO_VR

#include "platform.h"
#include "gl.h"
#include "port_config.h"
#include "vr.h"

#include "guest_host.h"
#include "halo_android_abi.h"

#include <string.h>

static struct
{
	int initialized, active;
	struct halo_xr_info info;
	GLuint framebuffer;
} vr;

int vr_active(void)
{
	return vr.active;
}

void vr_initialize(void)
{
	if (vr.initialized)
		return;
	vr.initialized = 1;
	if (!config_boolean("vr.enabled"))
	{
		platform_log("vr: off (vr.enabled)");
		return;
	}
	/* the flat screen and HUD layer: the game's 640x480 at twice the size */
	if (host_xr_init(&vr.info, 1280, 960) != 0)
	{
		platform_log("vr: OpenXR is unavailable; playing on the flat screen");
		return;
	}
	glGenFramebuffers(1, &vr.framebuffer);
	vr.active = 1;
	platform_log("vr: %s on %s, eyes %ux%u, %u images", vr.info.runtime, vr.info.system,
		vr.info.width[0], vr.info.height[0], vr.info.image_count[0]);
}

/* clears the acquired image of a swapchain */
static void clear_swapchain(unsigned int which, float red, float green, float blue, float alpha)
{
	int index = host_xr_acquire(which);

	if (index < 0)
		return;
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, vr.framebuffer);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
		vr.info.images[which][index], 0);
	glViewport(0, 0, (GLsizei)vr.info.width[which], (GLsizei)vr.info.height[which]);
	glDisable(GL_SCISSOR_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glClearColor(red, green, blue, alpha);
	glClear(GL_COLOR_BUFFER_BIT);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	host_xr_release(which);
}

void vr_probe(void)
{
	double seconds = config_real("vr.probe_seconds");
	long long start = 0, now = 0, frames = 0, rendered = 0;
	struct halo_xr_frame frame;

	if (!vr.active || seconds <= 0.0)
		return;
	platform_log("vr: probe for %.0f s (dim red left eye, dim blue right eye, grey panel ahead)", seconds);
	while (!start || (now - start) < (long long)(seconds * 1e9))
	{
		struct halo_xr_layers layers;

		if (!host_xr_begin_frame(&frame))
		{
			if (frame.flags & HALO_XR_FRAME_EXIT)
				break;
			continue;
		}
		now = frame.predicted_display_time;
		if (!start)
			start = now;
		frames++;
		memset(&layers, 0, sizeof(layers));
		if (frame.flags & HALO_XR_FRAME_SHOULD_RENDER)
		{
			/* dim: full-intensity colours fill the view uncomfortably */
			clear_swapchain(HALO_XR_SWAPCHAIN_LEFT, 0.18f, 0.03f, 0.03f, 1.0f);
			clear_swapchain(HALO_XR_SWAPCHAIN_RIGHT, 0.03f, 0.03f, 0.18f, 1.0f);
			clear_swapchain(HALO_XR_SWAPCHAIN_QUAD, 0.15f, 0.15f, 0.15f, 1.0f);
			layers.flags = HALO_XR_LAYER_PROJECTION | HALO_XR_LAYER_QUAD;
			/* 1 m wide, 2 m ahead at eye height */
			layers.quad_pose.position[2] = -2.0f;
			layers.quad_pose.orientation[3] = 1.0f;
			layers.quad_size[0] = 1.0f;
			layers.quad_size[1] = 0.75f;
			rendered++;
		}
		if ((frames % 360) == 1)
		{
			platform_log("vr: probe frame %lld: state %u flags 0x%x head (%.2f %.2f %.2f) "
				"fov L %.3f %.3f %.3f %.3f buttons 0x%x thumbs %.2f %.2f %.2f %.2f hands %u %u",
				frames, frame.session_state, frame.flags, frame.head.position[0], frame.head.position[1],
				frame.head.position[2], frame.fov[0][0], frame.fov[0][1], frame.fov[0][2], frame.fov[0][3],
				frame.buttons, frame.thumb[0], frame.thumb[1], frame.thumb[2], frame.thumb[3],
				frame.hand_valid[0], frame.hand_valid[1]);
		}
		host_xr_end_frame(&layers);
	}
	platform_log("vr: probe done: %lld frames, %lld rendered, %.1f frames/s", frames, rendered,
		now > start ? frames / ((now - start) * 1e-9) : 0.0);
}

#endif /* HALO_VR */
