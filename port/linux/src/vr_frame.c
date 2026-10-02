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

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct
{
	int initialized, active;
	struct halo_xr_info info;
	GLuint framebuffer;
	/* the runtime's frame begun and not yet ended, and its state */
	int frame_begun;
	struct halo_xr_frame frame;
	/* the eye render size (vr.resolution_scale) */
	int eye_size;
	int srgb_write_control;
	/* the flat screen (vr.screen_distance, vr.screen_width), metres */
	float screen_distance, screen_width;
	/* the HUD's layer, head-locked (vr.hud_distance, vr.hud_width) */
	float hud_distance, hud_width;
	int stereo_enabled;
	/* world units per metre (vr.world_scale) */
	float units_per_metre;
	/* this frame is drawn in stereo; which eyes are in their images */
	int stereo;
	unsigned int eyes_resolved;
	/* the view's heading (radians about Halo's +z); 0 before the head first
	aims or after a recentre */
	float heading, last_aim_yaw;
	int heading_valid, aiming, aiming_last_frame;
	/* vr.snap_turn (radians, 0 for smooth), vr.smooth_turn_speed (radians a second) */
	float snap_turn, smooth_turn_speed;
	int snap_armed, recentre_held;
	/* diagnostics (vr.force_render, vr.diag_yaw, vr.dump_frame) */
	int force_render;
	float diag_yaw;
	long dump_frame;
	long stereo_frames;
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
	{
		double scale = config_real("vr.resolution_scale");

		if (scale < 0.5) scale = 0.5;
		if (scale > 1.5) scale = 1.5;
		vr.eye_size = (int)(vr.info.width[HALO_XR_SWAPCHAIN_LEFT] * scale) & ~1;
	}
	/* the swapchains are sRGB and the game's picture is gamma-encoded
	already: written without conversion, the compositor shows it as it is */
	vr.srgb_write_control = host_gl_has_extension("GL_EXT_sRGB_write_control");
	vr.screen_distance = (float)config_real("vr.screen_distance");
	vr.screen_width = (float)config_real("vr.screen_width");
	vr.hud_distance = (float)config_real("vr.hud_distance");
	vr.hud_width = (float)config_real("vr.hud_width");
	vr.stereo_enabled = config_boolean("vr.stereo");
	vr.units_per_metre = (float)config_real("vr.world_scale");
	vr.snap_turn = (float)config_real("vr.snap_turn") * 0.017453293f;
	vr.smooth_turn_speed = (float)config_real("vr.smooth_turn_speed") * 0.017453293f;
	vr.snap_armed = 1;
	vr.force_render = config_boolean("vr.force_render");
	vr.diag_yaw = (float)config_real("vr.diag_yaw") * 0.017453293f;
	vr.dump_frame = config_integer("vr.dump_frame");
	if (vr.units_per_metre <= 0.0f)
		vr.units_per_metre = 1.0f / 3.048f;
	platform_log("vr: drawing %dx%d per eye; GL_EXT_sRGB_write_control %s", vr.eye_size, vr.eye_size,
		vr.srgb_write_control ? "present" : "absent");
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

int vr_screen_scale(float scale[2])
{
	if (!vr.active)
		return 0;
	/* the eye's square from the 640x480 screen: the frustum takes the
	aspect back out (vr_camera.c) */
	scale[0] = (float)vr.eye_size / 640.0f;
	scale[1] = (float)vr.eye_size / 480.0f;
	return 1;
}

int vr_controller(unsigned int *buttons, float trigger[2], float thumb[4])
{
	if (!vr.active || !(vr.frame.flags & HALO_XR_FRAME_FOCUSED))
		return 0;
	*buttons = vr.frame.buttons;
	memcpy(trigger, vr.frame.trigger, sizeof(vr.frame.trigger));
	memcpy(thumb, vr.frame.thumb, sizeof(vr.frame.thumb));
	if (vr.aiming_last_frame)
	{
		/* the right stick turns the heading (vr_aim), the head looks */
		thumb[2] = thumb[3] = 0.0f;
		/* both sticks pressed together recentre (vr_aim) */
		if ((*buttons & (HALO_XR_BUTTON_LEFT_THUMB | HALO_XR_BUTTON_RIGHT_THUMB)) ==
			(HALO_XR_BUTTON_LEFT_THUMB | HALO_XR_BUTTON_RIGHT_THUMB))
		{
			*buttons &= ~(HALO_XR_BUTTON_LEFT_THUMB | HALO_XR_BUTTON_RIGHT_THUMB);
		}
	}
	return 1;
}

/* begins the runtime's next frame unless one is begun; 0 when the session
is not running (the host polled and slept) */
static int frame_begin(void)
{
	if (vr.frame_begun)
		return 1;
	if (!host_xr_begin_frame(&vr.frame))
	{
		if (vr.frame.flags & HALO_XR_FRAME_EXIT)
		{
			platform_log("vr: the runtime ended the session; exiting");
			exit(0);
		}
		return 0;
	}
	vr.frame_begun = 1;
	return 1;
}

static void frame_end(const struct halo_xr_layers *layers)
{
	if (!vr.frame_begun)
		return;
	host_xr_end_frame(layers);
	vr.frame_begun = 0;
}

static void dump_image(unsigned int which, int index, const char *name);

/* copies framebuffer `source` (row 0 at the top) into the acquired image
of a swapchain, filling it */
static int copy_to_swapchain(unsigned int which, GLuint source, int width, int height)
{
	static const char *const names[] = { "vr-eye0.bmp", "vr-eye1.bmp", "vr-hud.bmp" };
	int index = host_xr_acquire(which);

	if (index < 0)
		return 0;
	glBindFramebuffer(GL_READ_FRAMEBUFFER, source);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, vr.framebuffer);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
		vr.info.images[which][index], 0);
	glDisable(GL_SCISSOR_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	if (vr.srgb_write_control)
		glDisable(GL_FRAMEBUFFER_SRGB_EXT);
	/* OpenXR images have row 0 at the bottom */
	glBlitFramebuffer(0, 0, width, height, 0, (GLint)vr.info.height[which], (GLint)vr.info.width[which], 0,
		GL_COLOR_BUFFER_BIT, GL_LINEAR);
	if (vr.srgb_write_control)
		glEnable(GL_FRAMEBUFFER_SRGB_EXT);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	/* vr.dump_frame: the stereo frame's images, as they are shown */
	if (vr.stereo && vr.dump_frame > 0 && vr.stereo_frames == vr.dump_frame)
		dump_image(which, index, names[which]);
	host_xr_release(which);
	return 1;
}

/* ---------- stereo */

static void rotate(const float q[4], const float v[3], float out[3]);

/* vr.force_render: a head 1.6 m up looking ahead (turned by vr.diag_yaw),
for runs with the headset off (in standby the runtime shows nothing and
tracks nothing) */
static void synthesize_views(void)
{
	static const float fov[2][4] = { { -1.028f, 0.885f, 0.864f, -1.048f }, { -0.885f, 1.028f, 0.864f, -1.048f } };
	float yaw[4] = { 0.0f, sinf(vr.diag_yaw * 0.5f), 0.0f, cosf(vr.diag_yaw * 0.5f) };
	int eye;

	memset(&vr.frame.head, 0, sizeof(vr.frame.head));
	memcpy(vr.frame.head.orientation, yaw, sizeof(yaw));
	for (eye = 0; eye < 2; eye++)
	{
		float offset[3] = { eye ? 0.032f : -0.032f, 0.0f, 0.0f };

		rotate(yaw, offset, vr.frame.eye[eye].position);
		memcpy(vr.frame.eye[eye].orientation, yaw, sizeof(yaw));
		memcpy(vr.frame.fov[eye], fov[eye], sizeof(fov[eye]));
	}
	vr.frame.flags |= HALO_XR_FRAME_SHOULD_RENDER | HALO_XR_FRAME_VIEWS_VALID;
}

int vr_stereo_begin(void)
{
	const unsigned int needed = HALO_XR_FRAME_SHOULD_RENDER | HALO_XR_FRAME_VIEWS_VALID;

	if (!vr.active || !frame_begin())
		return 0;
	if (vr.force_render && ((vr.frame.flags & needed) != needed || vr.diag_yaw != 0.0f))
		synthesize_views();
	vr.stereo = vr.stereo_enabled && (vr.frame.flags & needed) == needed;
	return vr.stereo;
}

/* q * v for a unit quaternion (x, y, z, w) */
static void rotate(const float q[4], const float v[3], float out[3])
{
	float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
	float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
	float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);

	out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
	out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
	out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

/* an OpenXR LOCAL vector (+x right, +y up, -z forward) in Halo's axes for
a viewer whose heading is (cosine, sine) */
static void to_halo(const float v[3], float cosine, float sine, float out[3])
{
	float forward = -v[2], left = -v[0], up = v[1];

	out[0] = forward * cosine - left * sine;
	out[1] = forward * sine + left * cosine;
	out[2] = up;
}

/* how far the head may move the view from the game's camera, in metres:
about as far as the body could lean (the game keeps only its own camera
out of walls) */
#define HEAD_REACH 0.35f

/* the head's offset from the recentred origin, limited to HEAD_REACH */
static void head_offset(float out[3])
{
	const float *head = vr.frame.head.position;
	float length = sqrtf(head[0] * head[0] + head[1] * head[1] + head[2] * head[2]);
	float scale = length > HEAD_REACH ? HEAD_REACH / length : 1.0f;

	out[0] = head[0] * scale;
	out[1] = head[1] * scale;
	out[2] = head[2] * scale;
}

/* a view at `offset` (LOCAL metres) turned by `orientation`, for a viewer
at `position` with the heading of `forward` */
static void view(const float offset[3], const float orientation[4], const float position[3],
	const float forward[3], float out_position[3], float out_forward[3], float out_up[3])
{
	static const float xr_forward[3] = { 0.0f, 0.0f, -1.0f }, xr_up[3] = { 0.0f, 1.0f, 0.0f };
	float heading = sqrtf(forward[0] * forward[0] + forward[1] * forward[1]);
	float cosine = 1.0f, sine = 0.0f;
	float halo[3], local[3];

	if (heading > 1e-4f)
	{
		cosine = forward[0] / heading;
		sine = forward[1] / heading;
	}
	to_halo(offset, cosine, sine, halo);
	out_position[0] = position[0] + halo[0] * vr.units_per_metre;
	out_position[1] = position[1] + halo[1] * vr.units_per_metre;
	out_position[2] = position[2] + halo[2] * vr.units_per_metre;
	rotate(orientation, xr_forward, local);
	to_halo(local, cosine, sine, out_forward);
	rotate(orientation, xr_up, local);
	to_halo(local, cosine, sine, out_up);
}

int vr_eye_view(int eye, const float position[3], const float forward[3], float aspect,
	float out_position[3], float out_forward[3], float out_up[3], float bounds[4])
{
	const struct halo_xr_pose *pose;
	float offset[3];
	int axis;

	if (!vr.stereo || eye < 0 || eye > 1)
		return 0;
	pose = &vr.frame.eye[eye];
	/* the head limited, the eye's own offset from it kept whole */
	head_offset(offset);
	for (axis = 0; axis < 3; axis++)
		offset[axis] += pose->position[axis] - vr.frame.head.position[axis];
	view(offset, pose->orientation, position, forward, out_position, out_forward, out_up);
	/* render_camera_build_frustum: x spans tangent * aspect, y the tangent */
	bounds[0] = tanf(vr.frame.fov[eye][0]) / aspect;
	bounds[1] = tanf(vr.frame.fov[eye][1]) / aspect;
	bounds[2] = tanf(vr.frame.fov[eye][3]);
	bounds[3] = tanf(vr.frame.fov[eye][2]);
	return 1;
}

/* the head's forward in Halo's axes at heading 0 */
static void head_forward(float out[3])
{
	static const float xr_forward[3] = { 0.0f, 0.0f, -1.0f };
	float local[3];

	rotate(vr.frame.head.orientation, xr_forward, local);
	to_halo(local, 1.0f, 0.0f, out);
}

static float wrap_angle(float angle)
{
	while (angle > 3.14159265f)
		angle -= 6.28318531f;
	while (angle < -3.14159265f)
		angle += 6.28318531f;
	return angle;
}

/* the right stick: snap turns, or smooth ones at vr.smooth_turn_speed */
static void turn(void)
{
	float x = vr.frame.thumb[2];
	double seconds = vr.frame.predicted_display_period * 1e-9;

	if (!(vr.frame.flags & HALO_XR_FRAME_FOCUSED))
		return;
	if (vr.snap_turn > 0.0f)
	{
		if (vr.snap_armed && (x > 0.7f || x < -0.7f))
		{
			/* pushed right turns right: yaw grows to the left */
			vr.heading = wrap_angle(vr.heading + (x > 0.0f ? -vr.snap_turn : vr.snap_turn));
			vr.snap_armed = 0;
		}
		else if (x < 0.3f && x > -0.3f)
		{
			vr.snap_armed = 1;
		}
	}
	else if (x > 0.15f || x < -0.15f)
	{
		vr.heading = wrap_angle(vr.heading - x * vr.smooth_turn_speed * (float)seconds);
	}
}

int vr_aim(float game_yaw, float out_forward[3])
{
	const unsigned int needed = HALO_XR_FRAME_SHOULD_RENDER | HALO_XR_FRAME_VIEWS_VALID;
	unsigned int both = HALO_XR_BUTTON_LEFT_THUMB | HALO_XR_BUTTON_RIGHT_THUMB;
	float head[3], head_yaw, cosine, sine;

	vr.aiming = 0;
	if (!vr.active || !vr.stereo_enabled || !frame_begin())
		return 0;
	if (vr.force_render && ((vr.frame.flags & needed) != needed || vr.diag_yaw != 0.0f))
		synthesize_views();
	if ((vr.frame.flags & needed) != needed)
		return 0;
	/* both sticks pressed: the way the player faces becomes forward */
	if ((vr.frame.buttons & both) == both)
	{
		if (!vr.recentre_held)
		{
			host_xr_recenter();
			vr.heading_valid = 0;
		}
		vr.recentre_held = 1;
	}
	else
	{
		vr.recentre_held = 0;
	}
	head_forward(head);
	head_yaw = atan2f(head[1], head[0]);
	if (vr.frame.flags & HALO_XR_FRAME_RECENTRED)
		vr.heading_valid = 0;
	/* the game turned the player itself: the heading follows */
	if (!vr.heading_valid || !vr.aiming_last_frame ||
		fabsf(wrap_angle(game_yaw - vr.last_aim_yaw)) > 0.01f)
	{
		vr.heading = wrap_angle(game_yaw - head_yaw);
		vr.heading_valid = 1;
	}
	turn();
	/* the game limits its pitch short of straight up or down (85.5
	degrees): so does the aim, keeping its heading */
	{
		float horizontal = sqrtf(head[0] * head[0] + head[1] * head[1]);
		const float limit = 0.08f; /* cos(85.4 degrees) */

		if (horizontal < limit)
		{
			float x = horizontal > 1e-6f ? head[0] / horizontal : 1.0f;
			float y = horizontal > 1e-6f ? head[1] / horizontal : 0.0f;

			head[0] = x * limit;
			head[1] = y * limit;
			head[2] = head[2] < 0.0f ? -sqrtf(1.0f - limit * limit) : sqrtf(1.0f - limit * limit);
		}
	}
	cosine = cosf(vr.heading);
	sine = sinf(vr.heading);
	out_forward[0] = head[0] * cosine - head[1] * sine;
	out_forward[1] = head[0] * sine + head[1] * cosine;
	out_forward[2] = head[2];
	vr.last_aim_yaw = atan2f(out_forward[1], out_forward[0]);
	vr.aiming = 1;
	return 1;
}

int vr_aiming(void)
{
	return vr.aiming || vr.aiming_last_frame;
}

int vr_heading_forward(float out_forward[3])
{
	if (!vr.heading_valid)
		return 0;
	out_forward[0] = cosf(vr.heading);
	out_forward[1] = sinf(vr.heading);
	out_forward[2] = 0.0f;
	return 1;
}

int vr_head_view(const float position[3], const float forward[3],
	float out_position[3], float out_forward[3], float out_up[3])
{
	float offset[3];

	if (!vr.stereo)
		return 0;
	head_offset(offset);
	view(offset, vr.frame.head.orientation, position, forward, out_position, out_forward, out_up);
	return 1;
}

void vr_resolve_eye(int eye, unsigned int source, int width, int height)
{
	if (!vr.stereo || eye < 0 || eye > 1)
		return;
	if (copy_to_swapchain((unsigned int)eye, source, width, height))
		vr.eyes_resolved |= 1u << eye;
}

/* writes a swapchain's acquired image (or the HUD's, with its alpha) as a
BMP into the data folder */
static void dump_image(unsigned int which, int index, const char *name)
{
	const char *directory = getenv("HALO_DATA_ROOT");
	int width = (int)vr.info.width[which], height = (int)vr.info.height[which];
	unsigned char header[54] = { 'B', 'M' };
	unsigned int size = (unsigned int)(width * height * 4);
	unsigned char *pixels = malloc(size);
	unsigned int pixel, transparent = 0;
	char path[512];
	FILE *file;

	if (!pixels)
		return;
	glBindFramebuffer(GL_READ_FRAMEBUFFER, vr.framebuffer);
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, vr.info.images[which][index], 0);
	glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	for (pixel = 0; pixel < (unsigned int)(width * height); pixel++)
	{
		unsigned char red = pixels[pixel * 4];

		pixels[pixel * 4] = pixels[pixel * 4 + 2];
		pixels[pixel * 4 + 2] = red;
		if (pixels[pixel * 4 + 3] == 0)
			transparent++;
		if (which != HALO_XR_SWAPCHAIN_QUAD)
			pixels[pixel * 4 + 3] = 0xff;
	}
	snprintf(path, sizeof(path), "%s/%s", directory ? directory : ".", name);
	file = fopen(path, "wb");
	if (file)
	{
		*(unsigned int *)(header + 2) = 54 + size;
		*(unsigned int *)(header + 10) = 54;
		*(unsigned int *)(header + 14) = 40;
		*(int *)(header + 18) = width;
		*(int *)(header + 22) = height; /* bottom-up, as GL reads */
		*(unsigned short *)(header + 26) = 1;
		*(unsigned short *)(header + 28) = 32;
		*(unsigned int *)(header + 34) = size;
		fwrite(header, 1, sizeof(header), file);
		fwrite(pixels, 1, size, file);
		fclose(file);
		platform_log("vr: wrote %s (%dx%d, %.1f%% transparent)", path, width, height,
			100.0 * transparent / (width * height));
	}
	free(pixels);
}

void vr_present(unsigned int source, int width, int height)
{
	struct halo_xr_layers layers;

	if (!vr.active || !frame_begin())
		return;
	memset(&layers, 0, sizeof(layers));
	if (vr.stereo)
	{
		/* the eyes are in their images; what is left in the back buffer is
		the HUD on a transparent ground, which rides ahead of the head */
		if (vr.eyes_resolved == 3)
			layers.flags |= HALO_XR_LAYER_PROJECTION;
		if (copy_to_swapchain(HALO_XR_SWAPCHAIN_QUAD, source, width, height))
		{
			layers.flags |= HALO_XR_LAYER_QUAD | HALO_XR_LAYER_QUAD_HEAD_LOCKED | HALO_XR_LAYER_QUAD_ALPHA;
			layers.quad_pose.position[2] = -vr.hud_distance;
			layers.quad_pose.orientation[3] = 1.0f;
			layers.quad_size[0] = vr.hud_width;
			layers.quad_size[1] = vr.hud_width * 0.75f;
		}
	}
	else if ((vr.frame.flags & HALO_XR_FRAME_SHOULD_RENDER) &&
		copy_to_swapchain(HALO_XR_SWAPCHAIN_QUAD, source, width, height))
	{
		/* a screen floating ahead at eye height, opaque */
		layers.flags = HALO_XR_LAYER_QUAD;
		layers.quad_pose.position[2] = -vr.screen_distance;
		layers.quad_pose.orientation[3] = 1.0f;
		layers.quad_size[0] = vr.screen_width;
		layers.quad_size[1] = vr.screen_width * 0.75f;
	}
	frame_end(&layers);
	if (vr.stereo)
		vr.stereo_frames++;
	vr.stereo = 0;
	vr.eyes_resolved = 0;
	vr.aiming_last_frame = vr.aiming;
	vr.aiming = 0;
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
