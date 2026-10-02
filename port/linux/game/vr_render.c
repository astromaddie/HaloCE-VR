/*
VR_RENDER.C

Stereo frames in the VR build (halo_vr.h): the eyes' windows and cameras,
built from the game's camera and the headset's pose (port/linux/src/vr.h).
*/

#ifdef HALO_VR

#include "cseries.h"
#include "math/real_math.h"
#include "cutscene/cinematics.h"
#include "render/render_cameras.h"

#include "halo_vr.h"
#include "../src/vr.h"

int vr_render_pass = _vr_render_pass_none;

static struct
{
	boolean stereo;
	real_rectangle2d eye_bounds[2];
	/* the game's camera this frame, the head posed from it */
	struct render_camera head_camera;
} vr_render;

static void eye_camera(
	int eye,
	struct render_camera *camera,
	real_rectangle2d *bounds)
{
	real aspect = (real)(camera->viewport_bounds.x1 - camera->viewport_bounds.x0) /
		(real)(camera->viewport_bounds.y1 - camera->viewport_bounds.y0);
	real_point3d position;
	real_vector3d forward, up;

	vr_eye_view(eye, camera->position.n, camera->forward.n, aspect, position.n, forward.n, up.n, bounds->n);
	camera->position = position;
	camera->forward = forward;
	camera->up = up;
	/* the bounds are tangents of a 90-degree field */
	camera->vertical_field_of_view = _pi * 0.5f;
}

short vr_render_windows(
	struct render_window *windows,
	short window_count)
{
	struct render_window player, console;
	int eye;

	vr_render.stereo = FALSE;
	if (window_count != 2 ||
		windows[0].local_player_index == NONE ||
		windows[0].console_window ||
		cinematic_in_progress() ||
		!vr_stereo_begin())
	{
		return window_count;
	}
	player = windows[0];
	console = windows[1];
	for (eye = 0; eye < 2; eye++)
	{
		windows[eye] = player;
		eye_camera(eye, &windows[eye].rasterizer_camera, &vr_render.eye_bounds[eye]);
		windows[eye].render_camera = windows[eye].rasterizer_camera;
	}
	/* the HUD keeps the game's camera: its reticle and markers line up
	with the view ahead */
	windows[2] = player;
	windows[3] = console;
	vr_render.head_camera = player.render_camera;
	{
		real_point3d position;
		real_vector3d forward, up;

		if (vr_head_view(player.render_camera.position.n, player.render_camera.forward.n,
			position.n, forward.n, up.n))
		{
			vr_render.head_camera.position = position;
			vr_render.head_camera.forward = forward;
			vr_render.head_camera.up = up;
		}
	}
	vr_render.stereo = TRUE;
	return 4;
}

void vr_render_window_begin(
	short window_index)
{
	vr_render_pass = vr_render.stereo && window_index <= _vr_render_pass_hud ?
		window_index : _vr_render_pass_none;
}

void vr_render_window_end(
	short window_index)
{
	if (vr_render.stereo && window_index <= _vr_render_pass_right_eye)
		halo_vr_resolve_eye(window_index);
	vr_render_pass = _vr_render_pass_none;
}

void vr_render_frustum_bounds(
	real_rectangle2d *bounds)
{
	if (VR_RENDER_EYE())
		*bounds = vr_render.eye_bounds[vr_render_pass];
}

void vr_render_weapon_camera(
	struct render_camera *camera)
{
	if (vr_render.stereo)
	{
		camera->position = vr_render.head_camera.position;
		camera->forward = vr_render.head_camera.forward;
		camera->up = vr_render.head_camera.up;
	}
}

#endif /* HALO_VR */
