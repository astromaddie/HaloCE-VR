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
#include "game/players.h"
#include "game/game.h"
#include "objects/objects.h"
#include "physics/collisions.h"
#include "units/units.h"

#include "halo_vr.h"
#include "../src/vr.h"

int vr_render_pass = _vr_render_pass_none;

static struct
{
	boolean stereo;
	real_rectangle2d eye_bounds[2];
	/* the game's camera this frame, the head posed from it */
	struct render_camera head_camera;
	real_point3d game_camera_position;
	/* where the hand's shots start (vr_render_hand_origin) */
	boolean hand_origin_valid;
	long hand_origin_unit;
	real_point3d hand_origin;
} vr_render;

/* the heading the eyes turn from: the headset's own while the head aims,
otherwise the game camera's */
static void view_heading(
	struct render_camera const *camera,
	real_vector3d *heading)
{
	if (!vr_heading_forward(heading->n))
		*heading = camera->forward;
}

static void eye_camera(
	int eye,
	struct render_camera *camera,
	real_rectangle2d *bounds)
{
	real aspect = (real)(camera->viewport_bounds.x1 - camera->viewport_bounds.x0) /
		(real)(camera->viewport_bounds.y1 - camera->viewport_bounds.y0);
	real_point3d position;
	real_vector3d forward, up, heading;

	view_heading(camera, &heading);
	vr_eye_view(eye, camera->position.n, heading.n, aspect, position.n, forward.n, up.n, bounds->n);
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
	vr_render.game_camera_position = player.render_camera.position;
	vr_render.head_camera = player.render_camera;
	{
		real_point3d position;
		real_vector3d forward, up;

		real_vector3d heading;

		view_heading(&player.render_camera, &heading);
		if (vr_head_view(player.render_camera.position.n, heading.n, position.n, forward.n, up.n))
		{
			vr_render.head_camera.position = position;
			vr_render.head_camera.forward = forward;
			vr_render.head_camera.up = up;
		}
	}
	/* the hand's reticle, where its aim meets the world */
	if (vr_hand_aiming())
	{
		real_point3d origin;
		real_vector3d direction, vector;
		struct collision_result collision;
		real distance = 128.0f;

		if (vr_hand_ray(player.render_camera.position.n, origin.n, direction.n))
		{
			long player_index = local_player_get_player_index(player.local_player_index);
			long unit_index = player_index != NONE ? player_get(player_index)->unit_index : NONE;

			scale_vector3d(&direction, distance, &vector);
			if (collision_test_vector(_collision_test_for_projectiles_flags, &origin, &vector, unit_index,
				&collision))
			{
				distance *= collision.t;
			}
			vr_set_reticle(distance);
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
	real_point3d position;
	real_vector3d forward, up;

	/* in the hand when it aims, else with the head */
	if (vr_render.stereo &&
		vr_weapon_view(vr_render.game_camera_position.n, position.n, forward.n, up.n))
	{
		camera->position = position;
		camera->forward = forward;
		camera->up = up;
	}
	else if (vr_render.stereo)
	{
		camera->position = vr_render.head_camera.position;
		camera->forward = vr_render.head_camera.forward;
		camera->up = vr_render.head_camera.up;
	}
}

void vr_player_control_facing(
	short local_player_index)
{
	real_euler_angles2d const *angles;
	real_vector3d forward;
	long player_index, unit_index;
	boolean seated;

	if (local_player_index != local_player_get_next(NONE) || cinematic_in_progress())
		return;
	player_index = local_player_get_player_index(local_player_index);
	unit_index = player_index != NONE ? player_get(player_index)->unit_index : NONE;
	/* in a vehicle's seat (or a turret's) the head aims */
	seated = unit_index != NONE && object_get(unit_index)->object.parent_object_index != NONE;
	angles = player_control_get_facing_angles(local_player_index);
	if (!vr_aim(angles->yaw, seated, forward.n))
	{
		vr_render.hand_origin_valid = FALSE;
		return;
	}
	player_control_set_facing(local_player_index, &forward);

	/* where the hand's shots start: the hand, seen from the unit's eye,
	unless a wall is in between */
	vr_render.hand_origin_valid = FALSE;
	if (unit_index != NONE && vr_hand_aiming() && game_connection() == _game_connection_local)
	{
		real_point3d camera, origin;
		real_vector3d direction, vector;
		struct collision_result collision;

		unit_get_camera_position(unit_index, &camera);
		if (vr_hand_ray(camera.n, origin.n, direction.n))
		{
			vector_from_points3d(&camera, &origin, &vector);
			if (!collision_test_vector(FLAG(_collision_test_structure_bit), &camera, &vector, unit_index, &collision))
			{
				vr_render.hand_origin = origin;
				vr_render.hand_origin_unit = unit_index;
				vr_render.hand_origin_valid = TRUE;
			}
		}
	}
}

int vr_render_hand_aiming(
	void)
{
	return vr_hand_aiming();
}

int vr_render_hand_origin(
	long unit_index,
	real_point3d *origin)
{
	if (!vr_render.hand_origin_valid || unit_index != vr_render.hand_origin_unit)
		return FALSE;
	*origin = vr_render.hand_origin;
	return TRUE;
}

int vr_render_aiming(
	void)
{
	return vr_aiming();
}

#endif /* HALO_VR */
