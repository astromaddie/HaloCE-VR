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
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "game/game.h"
#include "objects/objects.h"
#include "physics/collisions.h"
#include "physics/collision_features.h"
#include "camera/director.h"
#include "units/units.h"
#include "models/model_animation_definitions.h"
#include "units/unit_definitions.h"
#include "tag_files/tag_groups.h"
#include "tag_files/tag_files.h"
#include "items/weapons.h"
#include "items/weapon_definitions.h"

#include "halo_vr.h"
#include "../src/vr.h"
#include "../src/port_config.h"

/* port/linux/src/platform.h (a variadic call needs its prototype in scope
on the Android guest's ABI) */
void platform_log(const char *format, ...);

int vr_render_pass = _vr_render_pass_none;

static struct
{
	boolean stereo, cinema;
	/* what each window of this frame is, and which eye's image it
	completes (NONE: none) */
	short pass_of_window[5];
	short eye_of_window[5];
	/* a cutscene eye's frustum turned in, in the bounds' units */
	real cinema_shift[2];
	real_rectangle2d eye_bounds[2];
	/* the HUD pass is drawn from the head (head_camera), over the panel's
	field */
	boolean hud_from_head;
	real_rectangle2d hud_bounds;
	/* the head's yaw (radians), for the motion sensor */
	boolean head_yaw_valid;
	real head_yaw;
	/* the scope's pass this frame: its sight (VR_SCOPE_*, 0 none), field and
	viewport */
	int scope_shape;
	real_rectangle2d scope_bounds;
	rectangle2d scope_viewport;
	/* the game's camera this frame, the head posed from it */
	struct render_camera head_camera;
	real_point3d game_camera_position;
	/* the camera the first-person weapon was posed from this frame */
	struct render_camera weapon_camera;
	boolean weapon_camera_valid;
	/* the local player's seat (vr_update_seat): in a vehicle, its kind, and
	the heading the view turns with (the vehicle's, plus the seat's own turn
	from it) */
	struct
	{
		boolean seated, driver, gunner;
		long unit_index, vehicle_index;
		short seat_index;
		real offset, heading;
	} seat;
	/* where the hand's shots start (vr_render_hand_origin) */
	boolean hand_origin_valid;
	long hand_origin_unit;
	real_point3d hand_origin;
} vr_render;

/* vr.vehicle_view and vr.vehicle_steering */
enum
{
	_vr_steering_stick,
	_vr_steering_head,
	_vr_steering_hand,
};

static boolean vr_first_person_vehicles(
	void)
{
	static int first_person = -1, generation = -1;

	/* (read again when the pause menu changes it) */
	if (first_person < 0 || generation != vr_settings_generation())
	{
		first_person = strcmp(config_string("vr.vehicle_view"), "chase") != 0;
		generation = vr_settings_generation();
	}
	return first_person && vr_active();
}

static int vr_vehicle_steering(
	void)
{
	static int steering = -1, generation = -1;

	if (steering < 0 || generation != vr_settings_generation())
	{
		generation = vr_settings_generation();
		char const *setting = config_string("vr.vehicle_steering");

		steering = !strcmp(setting, "head") ? _vr_steering_head :
			!strcmp(setting, "hand") ? _vr_steering_hand : _vr_steering_stick;
	}
	return steering;
}

static real vr_yaw(
	real_vector3d const *forward)
{
	return (real)atan2(forward->j, forward->i);
}

/* vr.diag_drive_seconds: this long into play, the local player is seated
as the driver of the nearest vehicle (for checking vehicles unattended;
debug.network_test_vehicle does the same in network tests) */
static void vr_diag_drive(
	long unit_index)
{
	static real seconds = -1.0f;
	static long first_tick = NONE;
	struct object_iterator vehicles;
	long nearest_index = NONE;
	real nearest_distance = 0.0f;
	short seat_index;

	if (seconds < 0.0f)
		seconds = (real)config_real("vr.diag_drive_seconds");
	if (seconds <= 0.0f || unit_index == NONE || object_get(unit_index)->object.parent_object_index != NONE)
		return;
	if (first_tick == NONE)
		first_tick = game_time_get();
	if (game_time_get() - first_tick < (long)(seconds * TICKS_PER_SECOND))
		return;
	seconds = 0.0f;
	object_iterator_new(&vehicles, _object_mask_vehicle, 0);
	while (object_iterator_next(&vehicles))
	{
		real distance = distance_squared3d(&object_get(unit_index)->object.position,
			&object_get(vehicles.index)->object.position);

		if (nearest_index == NONE || distance < nearest_distance)
		{
			nearest_index = vehicles.index;
			nearest_distance = distance;
		}
	}
	if (nearest_index == NONE)
	{
		platform_log("vr: diag drive: no vehicle");
		return;
	}
	for (seat_index = 0; seat_index < unit_definition_get(object_get(nearest_index)->definition_index)->unit.seats.count;
		seat_index++)
	{
		if (unit_seat_is_driver(nearest_index, seat_index) && unit_enter_seat(unit_index, nearest_index, seat_index))
		{
			platform_log("vr: diag drive: driving vehicle %lx", nearest_index);
			return;
		}
	}
	platform_log("vr: diag drive: cannot drive vehicle %lx", nearest_index);
}

/* the local player's seat this frame */
static void vr_update_seat(
	long unit_index)
{
	struct object_datum *unit = unit_index != NONE ? object_get(unit_index) : NULL;
	long vehicle_index = unit ? unit->object.parent_object_index : NONE;
	short seat_index = vehicle_index != NONE ? unit_get(unit_index)->unit.parent_seat_index : NONE;
	struct object_datum *vehicle;

	if (vehicle_index == NONE || seat_index == NONE)
	{
		vr_render.seat.seated = FALSE;
		vr_render.seat.vehicle_index = NONE;
		return;
	}
	vehicle = object_get(vehicle_index);
	if (!vr_render.seat.seated || vr_render.seat.vehicle_index != vehicle_index ||
		vr_render.seat.seat_index != seat_index || vr_render.seat.unit_index != unit_index)
	{
		unsigned long flags = 0;

		if (TEST_FLAG(_object_mask_unit, vehicle->object.type))
		{
			struct unit_seat *seat = TAG_BLOCK_GET_ELEMENT(
				&unit_definition_get(vehicle->definition_index)->unit.seats, seat_index, struct unit_seat);

			flags = seat->flags;
		}
		vr_render.seat.driver = TEST_FLAG(flags, _unit_seat_driver_bit);
		vr_render.seat.gunner = TEST_FLAG(flags, _unit_seat_gunner_bit);
		/* a driver or gunner faces the vehicle's way; a passenger's seat may
		face aside (the Pelican's face the aisle): its turn as it sits down */
		vr_render.seat.offset = vr_render.seat.driver || vr_render.seat.gunner ? 0.0f :
			vr_yaw(&unit->object.forward) - vr_yaw(&vehicle->object.forward);
		vr_render.seat.vehicle_index = vehicle_index;
		vr_render.seat.seat_index = seat_index;
		vr_render.seat.unit_index = unit_index;
	}
	vr_render.seat.seated = TRUE;
	/* the vehicle's heading only: its pitch and roll would tilt the horizon */
	vr_render.seat.heading = vr_yaw(&vehicle->object.forward) + vr_render.seat.offset;
}

/* the view rides in the seat (vr.vehicle_view "first_person") */
static boolean vr_seat_view(
	void)
{
	return vr_render.seat.seated && vr_first_person_vehicles();
}

/* the heading the eyes turn from: the seat's in a vehicle seen from it,
the headset's own while the head or hand aims, otherwise the game
camera's */
static void view_heading(
	struct render_camera const *camera,
	real_vector3d *heading)
{
	if (vr_seat_view())
	{
		heading->i = (real)cos(vr_render.seat.heading);
		heading->j = (real)sin(vr_render.seat.heading);
		heading->k = 0.0f;
	}
	else if (!vr_aiming() || !vr_heading_forward(heading->n))
	{
		*heading = camera->forward;
	}
}

/* where the eyes are placed from: in a vehicle seen from its seat, the
player's head there (the game's camera for a seat is the chase camera's
place); otherwise the game's camera */
static void view_anchor(
	struct render_camera const *camera,
	real_point3d *anchor)
{
	*anchor = camera->position;
	if (vr_seat_view())
	{
		struct object_marker marker;

		if (object_get_marker_by_name(vr_render.seat.unit_index, "head", &marker, 1))
			*anchor = marker.matrix.position;
	}
}

static void eye_camera(
	int eye,
	struct render_camera *camera,
	real_rectangle2d *bounds)
{
	real aspect = (real)(camera->viewport_bounds.x1 - camera->viewport_bounds.x0) /
		(real)(camera->viewport_bounds.y1 - camera->viewport_bounds.y0);
	real_point3d position, anchor;
	real_vector3d forward, up, heading;

	view_heading(camera, &heading);
	view_anchor(camera, &anchor);
	vr_eye_view(eye, anchor.n, heading.n, aspect, position.n, forward.n, up.n, bounds->n);
	camera->position = position;
	camera->forward = forward;
	camera->up = up;
	/* the bounds are tangents of a 90-degree field */
	camera->vertical_field_of_view = _pi * 0.5f;
}

/* a cutscene: each eye's view of it, then the console window (letterbox,
titles) over it, for the 3D screen */
static short cinema_windows(
	struct render_window *windows)
{
	struct render_window player = windows[0], console = windows[1];
	int eye;

	for (eye = 0; eye < 2; eye++)
	{
		struct render_camera *camera;
		real offset, convergence, aspect, tangent;
		real_vector3d right;

		windows[eye * 2] = player;
		windows[eye * 2 + 1] = console;
		camera = &windows[eye * 2].rasterizer_camera;
		vr_cinema_eye(eye, &offset, &convergence);
		cross_product3d(&camera->forward, &camera->up, &right);
		normalize3d(&right);
		camera->position.x += right.i * offset;
		camera->position.y += right.j * offset;
		camera->position.z += right.k * offset;
		windows[eye * 2].render_camera = *camera;
		/* render_camera_build_frustum: x spans its bounds times the
		viewport's aspect and the field's tangent */
		aspect = (real)(camera->viewport_bounds.x1 - camera->viewport_bounds.x0) /
			(real)(camera->viewport_bounds.y1 - camera->viewport_bounds.y0);
		tangent = (real)tan(camera->vertical_field_of_view * 0.5f);
		vr_render.cinema_shift[eye] = convergence / (aspect * tangent);
		vr_render.pass_of_window[eye * 2] = eye ? _vr_render_pass_cinema_right_eye : _vr_render_pass_cinema_left_eye;
		vr_render.eye_of_window[eye * 2 + 1] = (short)eye;
	}
	vr_render.cinema = TRUE;
	return 4;
}

/* the sight of the weapon the local player holds (vr.h's VR_SCOPE_*), by
its tag's name */
static int scope_shape(
	short local_player_index)
{
	long player_index = local_player_get_player_index(local_player_index);
	long unit_index = player_index != NONE ? player_get(player_index)->unit_index : NONE;
	long weapon_index;
	char const *name;

	if (unit_index == NONE)
		return 0;
	weapon_index = unit_inventory_get_weapon(unit_index, unit_get(unit_index)->unit.current_weapon_index);
	if (weapon_index == NONE)
		return 0;
	name = tag_get_name(weapon_get(weapon_index)->definition_index);
	return name && strstr(name, "sniper") ? VR_SCOPE_SNIPER :
		name && strstr(name, "rocket") ? VR_SCOPE_ROCKET : VR_SCOPE_ROUND;
}

/* the scope's window, while the hand aims a zoomed weapon: the player's
window seen along the gun at the game's zoomed field, in a square of the
target as large as the scope's image. Its sight shows the middle of that
field (the PC mod HaloCEVR's: a disc half the screen's height, or the sniper
rifle's wide rectangle), so the frustum is cut to it. FALSE for none. */
static boolean scope_window(
	struct render_window *window,
	struct render_window const *player)
{
	struct render_camera *camera = &window->rasterizer_camera;
	real_point3d position;
	real_vector3d forward, up, vector;
	struct collision_result collision;
	float scale[2];
	real tangent, half, aspect;
	int pixels, shape;
	short width, height;

	if (player_control_get_zoom_level(player->local_player_index) == NONE ||
		!(shape = scope_shape(player->local_player_index)) ||
		!vr_screen_scale(scale) || scale[0] <= 0.0f || scale[1] <= 0.0f ||
		!vr_scope_view(vr_render.game_camera_position.n, position.n, forward.n, up.n, &pixels))
	{
		return FALSE;
	}
	width = (short)MIN(640.0f, (real)pixels / scale[0] + 0.5f);
	height = (short)MIN(480.0f, (real)pixels / scale[1] + 0.5f);
	if (width < 16 || height < 16)
		return FALSE;
	/* out of walls, as the hand's shots are */
	vector_from_points3d(&vr_render.game_camera_position, &position, &vector);
	if (collision_test_vector(FLAG(_collision_test_structure_bit), &vr_render.game_camera_position, &vector,
		NONE, &collision))
	{
		position.x = vr_render.game_camera_position.x + vector.i * collision.t * 0.9f;
		position.y = vr_render.game_camera_position.y + vector.j * collision.t * 0.9f;
		position.z = vr_render.game_camera_position.z + vector.k * collision.t * 0.9f;
	}
	*window = *player;
	tangent = (real)tan(player->render_camera.vertical_field_of_view * 0.5f);
	half = tangent * (shape == VR_SCOPE_SNIPER ? 0.4033f : 0.5f);
	camera->position = position;
	camera->forward = forward;
	camera->up = up;
	camera->viewport_bounds.x0 = camera->viewport_bounds.y0 = 0;
	camera->viewport_bounds.x1 = width;
	camera->viewport_bounds.y1 = height;
	camera->window_bounds = camera->viewport_bounds;
	/* the bounds are tangents of a 90-degree field; x spans them times the
	viewport's aspect (render_camera_build_frustum) */
	camera->vertical_field_of_view = _pi * 0.5f;
	aspect = (real)width / (real)height;
	vr_render.scope_bounds.x0 = -half / aspect;
	vr_render.scope_bounds.x1 = half / aspect;
	vr_render.scope_bounds.y0 = -half;
	vr_render.scope_bounds.y1 = half;
	vr_render.scope_viewport = camera->viewport_bounds;
	vr_render.scope_shape = shape;
	window->render_camera = *camera;
	return TRUE;
}

short vr_render_windows(
	struct render_window *windows,
	short window_count)
{
	struct render_window player, console;
	int eye;

	vr_render.stereo = FALSE;
	vr_render.cinema = FALSE;
	vr_render.scope_shape = 0;
	for (eye = 0; eye < 5; eye++)
	{
		vr_render.pass_of_window[eye] = _vr_render_pass_none;
		vr_render.eye_of_window[eye] = NONE;
	}
	if (window_count == 2 &&
		windows[0].local_player_index != NONE &&
		!windows[0].console_window &&
		cinematic_in_progress() &&
		vr_cinema_begin())
	{
		return cinema_windows(windows);
	}
	if (window_count != 2 ||
		windows[0].local_player_index == NONE ||
		windows[0].console_window ||
		cinematic_in_progress() ||
		/* the main menu's scene stays behind its menus, on the flat screen */
		global_scenario_get()->type == _scenario_type_main_menu ||
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
	view_anchor(&player.render_camera, &vr_render.game_camera_position);
	vr_render.head_camera = player.render_camera;
	vr_render.hud_from_head = FALSE;
	{
		real_point3d position;
		real_vector3d forward, up;

		real_vector3d heading;

		view_heading(&player.render_camera, &heading);
		if (vr_head_view(vr_render.game_camera_position.n, heading.n, position.n, forward.n, up.n))
		{
			real horizontal = (real)sqrt(forward.i * forward.i + forward.j * forward.j);

			vr_render.head_camera.position = position;
			vr_render.head_camera.forward = forward;
			vr_render.head_camera.up = up;
			vr_render.hud_from_head = TRUE;
			/* (looking straight up or down, the last yaw holds) */
			if (horizontal > 0.2f)
			{
				vr_render.head_yaw = vr_yaw(&forward);
				vr_render.head_yaw_valid = TRUE;
			}
		}
	}
	/* the HUD, drawn from the head over its head-locked panel's field: its
	markers (nav points, friends' names) land on what they mark, and with
	the head aiming its crosshair is where the head looks */
	windows[2] = player;
	windows[3] = console;
	if (vr_render.hud_from_head)
	{
		struct render_camera *camera = &windows[2].rasterizer_camera;

		camera->position = vr_render.head_camera.position;
		camera->forward = vr_render.head_camera.forward;
		camera->up = vr_render.head_camera.up;
		camera->vertical_field_of_view = _pi * 0.5f;
		windows[2].render_camera = *camera;
		vr_hud_bounds((real)(camera->viewport_bounds.x1 - camera->viewport_bounds.x0) /
			(real)(camera->viewport_bounds.y1 - camera->viewport_bounds.y0), vr_render.hud_bounds.n);
	}
	/* the hand's reticle, where its aim meets the world */
	if (vr_hand_aiming())
	{
		real_point3d origin;
		real_vector3d direction, vector;
		struct collision_result collision;
		real distance = 128.0f;

		if (vr_hand_ray(vr_render.game_camera_position.n, origin.n, direction.n))
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
	vr_render.pass_of_window[0] = _vr_render_pass_left_eye;
	vr_render.pass_of_window[1] = _vr_render_pass_right_eye;
	vr_render.pass_of_window[2] = _vr_render_pass_hud;
	vr_render.eye_of_window[0] = 0;
	vr_render.eye_of_window[1] = 1;
	/* zoomed: the scope's view before the HUD (window 2, within
	MAXIMUM_WINDOWS for the rasterizer's per-window state) */
	{
		struct render_window scope;

		if (scope_window(&scope, &player))
		{
			windows[3] = windows[2];
			windows[4] = console;
			windows[2] = scope;
			vr_render.pass_of_window[2] = _vr_render_pass_scope;
			vr_render.pass_of_window[3] = _vr_render_pass_hud;
			return 5;
		}
	}
	return 4;
}

void vr_render_window_begin(
	short window_index)
{
	vr_render_pass = (vr_render.stereo || vr_render.cinema) && window_index >= 0 && window_index < 5 ?
		vr_render.pass_of_window[window_index] : _vr_render_pass_none;
	vr_pass_mark(vr_render_pass, 0);
}

void vr_render_window_end(
	short window_index)
{
	vr_pass_mark(vr_render_pass, 1);
	if ((vr_render.stereo || vr_render.cinema) && window_index >= 0 && window_index < 5 &&
		vr_render.eye_of_window[window_index] != NONE)
	{
		halo_vr_resolve_eye(vr_render.eye_of_window[window_index]);
	}
	if (VR_RENDER_SCOPE())
	{
		halo_vr_resolve_scope(vr_render.scope_viewport.x0, vr_render.scope_viewport.y0,
			vr_render.scope_viewport.x1, vr_render.scope_viewport.y1, vr_render.scope_shape);
	}
	vr_render_pass = _vr_render_pass_none;
}

void vr_render_frustum_bounds(
	real_rectangle2d *bounds)
{
	if (VR_RENDER_EYE())
	{
		*bounds = vr_render.eye_bounds[vr_render_pass];
	}
	else if (VR_RENDER_SCOPE())
	{
		*bounds = vr_render.scope_bounds;
	}
	else if (VR_RENDER_HUD() && vr_render.hud_from_head)
	{
		*bounds = vr_render.hud_bounds;
	}
	else if (vr_render_pass == _vr_render_pass_cinema_left_eye || vr_render_pass == _vr_render_pass_cinema_right_eye)
	{
		real shift = vr_render.cinema_shift[vr_render_pass == _vr_render_pass_cinema_right_eye];

		bounds->x0 += shift;
		bounds->x1 += shift;
	}
}

void vr_render_room_scale(
	long biped_index,
	real_point3d *position,
	real height,
	real width)
{
	static real diag_walk = -1.0f, asked = 0.0f, walked = 0.0f;
	static long diag_tick = NONE;
	short local_player_index = local_player_get_next(NONE);
	long player_index = local_player_index != NONE ? local_player_get_player_index(local_player_index) : NONE;
	struct collision_plane collisions[16];
	real_point3d clipped;
	real_vector3d step, clipped_velocity;
	float room_step[2];

	if (player_index == NONE || player_get(player_index)->unit_index != biped_index || !vr_active())
		return;
	/* only where the player could walk by the stick (a local game; no
	cutscene, script or death holding them) */
	if (!vr_aiming() || cinematic_in_progress() || game_connection() != _game_connection_local ||
		director_inhibited_input(local_player_index) ||
		object_get(biped_index)->object.parent_object_index != NONE ||
		TEST_FLAG(object_get(biped_index)->object.damage_flags, _object_dead_bit))
	{
		vr_room_hold();
		return;
	}
	if (!vr_room_step(room_step))
		return;
	step.i = room_step[0];
	step.j = room_step[1];
	step.k = 0.0f;
	/* (a second tick in a frame has the head where the first left it) */
	if (magnitude_squared3d(&step) < 1e-10f)
	{
		vr_room_moved();
		return;
	}
	/* the pill moved as walking moves it, sliding along what it meets */
	collision_move_pill(_collision_test_for_bipeds_living_flags, position, &step, height, width, biped_index,
		&clipped, &clipped_velocity, 16, collisions);
	vr_room_moved();
	/* vr.diag_walk_speed: how far the steps took the player each second */
	if (diag_walk < 0.0f)
		diag_walk = (real)config_real("vr.diag_walk_speed");
	if (diag_walk != 0.0f)
	{
		asked += magnitude3d(&step);
		walked += (real)sqrt((clipped.x - position->x) * (clipped.x - position->x) +
			(clipped.y - position->y) * (clipped.y - position->y));
		if (diag_tick == NONE || game_time_get() - diag_tick >= TICKS_PER_SECOND)
		{
			platform_log("vr: room-scale walked %.3f of %.3f units this second", walked, asked);
			diag_tick = game_time_get();
			asked = walked = 0.0f;
		}
	}
	*position = clipped;
}

int vr_render_motion_sensor_yaw(
	short local_player_index,
	real *yaw)
{
	if (local_player_index != local_player_get_next(NONE) || !vr_aiming() || !vr_render.head_yaw_valid)
		return FALSE;
	*yaw = vr_render.head_yaw;
	return TRUE;
}

int vr_render_unzoomed_view(
	void)
{
	return vr_render.stereo;
}

void vr_render_weapon_camera(
	struct render_camera *camera)
{
	real_point3d position;
	real_vector3d forward, up;

	/* in the hand when it aims, else with the head */
	vr_render.weapon_camera_valid = FALSE;
	if (vr_render.stereo &&
		vr_weapon_view(vr_render.game_camera_position.n, position.n, forward.n, up.n))
	{
		camera->position = position;
		camera->forward = forward;
		camera->up = up;
		vr_render.weapon_camera = *camera;
		vr_render.weapon_camera_valid = TRUE;
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

	if (local_player_index != local_player_get_next(NONE))
		return;
	if (cinematic_in_progress())
	{
		vr_room_hold();
		return;
	}
	player_index = local_player_get_player_index(local_player_index);
	unit_index = player_index != NONE ? player_get(player_index)->unit_index : NONE;
	vr_diag_drive(unit_index);
	vr_update_seat(unit_index);
	/* seated, the head leans from where the player stood (vr.roomscale) */
	if (vr_render.seat.seated)
		vr_room_hold();
	vr_set_zoom_level(player_control_get_zoom_level(local_player_index));
	seated = vr_render.seat.seated;
	angles = player_control_get_facing_angles(local_player_index);
	/* a driver steered by the stick (vr.vehicle_steering "stick"): the game
	takes the right stick as it would, and the head only looks */
	if (seated && vr_render.seat.driver && vr_vehicle_steering() == _vr_steering_stick)
	{
		vr_render.hand_origin_valid = FALSE;
		return;
	}
	{
		/* the hand aims on foot, and a driver's seat steered by it; the head
		in any other seat (a turret aims where the head looks) */
		boolean hand_may_aim = !seated || (vr_render.seat.driver && vr_vehicle_steering() == _vr_steering_hand);
		real heading = vr_render.seat.heading;

		if (!vr_aim(angles->yaw, seated, hand_may_aim, vr_seat_view() ? &heading : NULL, forward.n))
		{
			vr_render.hand_origin_valid = FALSE;
			return;
		}
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

/* the buzz of a shot, by weapon (its tag's name): strength and seconds */
static void vr_weapon_buzz(
	long weapon_index,
	real *amplitude,
	real *seconds)
{
	static struct { char const *name; real amplitude, seconds; } const buzzes[] =
	{
		{ "pistol", 0.6f, 0.06f },        /* the plasma pistol is matched first */
		{ "shotgun", 1.0f, 0.12f },
		{ "sniper", 1.0f, 0.12f },
		{ "rocket", 1.0f, 0.20f },
		{ "fuel rod", 0.9f, 0.15f },
		{ "flamethrower", 0.3f, 0.05f },
		{ "needler", 0.3f, 0.04f },
		{ "plasma rifle", 0.3f, 0.04f },
		{ "assault rifle", 0.35f, 0.04f },
	};
	char const *name = tag_get_name(weapon_get(weapon_index)->definition_index);
	int index;

	*amplitude = 0.5f;
	*seconds = 0.05f;
	if (name && strstr(name, "plasma pistol"))
	{
		*amplitude = 0.4f;
		return;
	}
	for (index = 0; name && index < (int)(sizeof(buzzes) / sizeof(buzzes[0])); index++)
	{
		if (strstr(name, buzzes[index].name))
		{
			*amplitude = buzzes[index].amplitude;
			*seconds = buzzes[index].seconds;
			return;
		}
	}
}

void vr_render_weapon_fired(
	long weapon_index,
	long player_index)
{
	static long last_tick = NONE, last_weapon = NONE;
	real amplitude, seconds;
	int hand;

	if (!vr_active() || player_index == NONE ||
		player_index != local_player_get_player_index(local_player_get_next(NONE)))
	{
		return;
	}
	/* a shotgun's pellets are one shot */
	if (last_tick == game_time_get() && last_weapon == weapon_index)
		return;
	last_tick = game_time_get();
	last_weapon = weapon_index;
	vr_weapon_buzz(weapon_index, &amplitude, &seconds);
	hand = vr_weapon_hand();
	vr_haptic(hand, amplitude, seconds);
	if (vr_two_handed())
		vr_haptic(1 - hand, amplitude * 0.6f, seconds);
}

/* vr.diag_zoom_seconds: this long into play the local player zooms in,
switching first to a weapon that zooms (for checking the scope
unattended) */
static unsigned long vr_diag_zoom(
	short local_player_index)
{
	static real seconds = -1.0f;
	static long next_tick = NONE;
	static int switches = 0;
	long player_index = local_player_get_player_index(local_player_index);
	long unit_index = player_index != NONE ? player_get(player_index)->unit_index : NONE;
	long weapon_index;

	if (seconds < 0.0f)
		seconds = (real)config_real("vr.diag_zoom_seconds");
	if (seconds <= 0.0f || unit_index == NONE || cinematic_in_progress())
		return 0;
	if (next_tick == NONE)
		next_tick = game_time_get() + (long)(seconds * TICKS_PER_SECOND);
	if (game_time_get() < next_tick)
		return 0;
	weapon_index = unit_inventory_get_weapon(unit_index, unit_get(unit_index)->unit.current_weapon_index);
	if (weapon_index != NONE && weapon_definition_get(weapon_get(weapon_index)->definition_index)->weapon.zoom_level_count > 0)
	{
		seconds = 0.0f;
		platform_log("vr: diag zoom: zooming %s", tag_get_name(weapon_get(weapon_index)->definition_index));
		return VR_RENDER_ACTION_ZOOM;
	}
	if (++switches > 3)
	{
		seconds = 0.0f;
		platform_log("vr: diag zoom: no weapon that zooms");
		return 0;
	}
	/* a weapon switch takes its animation's time */
	next_tick = game_time_get() + 2 * TICKS_PER_SECOND;
	return VR_RENDER_ACTION_SWITCH_WEAPON;
}

unsigned long vr_render_actions(
	short local_player_index)
{
	if (local_player_index != local_player_get_next(NONE) || !vr_active())
		return 0;
	return vr_take_actions() | vr_diag_zoom(local_player_index);
}

int vr_render_first_person_mirrored(
	void)
{
	return vr_render.stereo && vr_hand_aiming() && vr_weapon_hand() == 0;
}

int vr_render_first_person_vehicles(
	void)
{
	return vr_first_person_vehicles();
}

int vr_render_hide_first_person_weapon(
	void)
{
	/* driving or on a turret, the player's own gun is put away */
	return vr_seat_view() && (vr_render.seat.driver || vr_render.seat.gunner);
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

/* ---------- the first-person arms (vr.arms)

The first-person weapon's animation poses the gun and the arms holding it
from the camera; with the hand aiming, its camera is placed for the gun to
sit in the right hand, which carries the arms along with the gun. "ik"
gives the arms shoulders where the body is and solves each arm's upper arm
and forearm to reach its hand: the right one the gun's grip as animated,
the left one the left controller (or, held near the gun, its grip as
animated). "hidden" shows the gun alone; "animated" leaves the arms as the
animation has them. */

/* as first_person_weapons.c and model_animations.c lay it out */
struct vr_animation_graph_node
{
	char name[TAG_STRING_LENGTH+1];
	short next_sibling_node_index;
	short first_child_node_index;
	short parent_node_index;
	word pad;
	unsigned long flags;
	real_vector3d base_vector;
	real range;
	long pad1;
};

enum
{
	_vr_arm_upper,
	_vr_arm_fore,
	_vr_arm_hand,
	NUMBER_OF_VR_ARM_BONES
};

static short vr_find_node(
	struct animation_graph *graph,
	char const *side,
	char const *bone)
{
	short index;

	for (index = 0; index < graph->nodes.count; index++)
	{
		struct vr_animation_graph_node const *node =
			TAG_BLOCK_GET_ELEMENT(&graph->nodes, index, struct vr_animation_graph_node);

		if (strstr(node->name, side) && strstr(node->name, bone))
			return index;
	}
	return NONE;
}

static real vr_length(real_vector3d const *v)
{
	return (real)sqrt(v->i * v->i + v->j * v->j + v->k * v->k);
}

static void vr_point_minus(real_point3d const *a, real_point3d const *b, real_vector3d *out)
{
	out->i = a->x - b->x;
	out->j = a->y - b->y;
	out->k = a->z - b->z;
}

/* the rotation (as a matrix applied to column vectors) taking direction
a to direction b, the shortest way */
static void vr_rotation_between(
	real_vector3d const *a,
	real_vector3d const *b,
	real rotation[3][3])
{
	real_vector3d u = *a, v = *b, axis;
	real c, s, t, length;

	normalize3d(&u);
	normalize3d(&v);
	cross_product3d(&u, &v, &axis);
	s = vr_length(&axis);
	c = u.i * v.i + u.j * v.j + u.k * v.k;
	if (s < 1e-6f)
	{
		/* parallel (or opposite: then any axis across does) */
		memset(rotation, 0, sizeof(real) * 9);
		rotation[0][0] = rotation[1][1] = rotation[2][2] = c >= 0.0f ? 1.0f : -1.0f;
		return;
	}
	length = s;
	axis.i /= length;
	axis.j /= length;
	axis.k /= length;
	t = 1.0f - c;
	rotation[0][0] = c + axis.i * axis.i * t;
	rotation[0][1] = axis.i * axis.j * t - axis.k * s;
	rotation[0][2] = axis.i * axis.k * t + axis.j * s;
	rotation[1][0] = axis.j * axis.i * t + axis.k * s;
	rotation[1][1] = c + axis.j * axis.j * t;
	rotation[1][2] = axis.j * axis.k * t - axis.i * s;
	rotation[2][0] = axis.k * axis.i * t - axis.j * s;
	rotation[2][1] = axis.k * axis.j * t + axis.i * s;
	rotation[2][2] = c + axis.k * axis.k * t;
}

static void vr_rotate_vector(real rotation[3][3], real_vector3d const *in, real_vector3d *out)
{
	real_vector3d v = *in;

	out->i = rotation[0][0] * v.i + rotation[0][1] * v.j + rotation[0][2] * v.k;
	out->j = rotation[1][0] * v.i + rotation[1][1] * v.j + rotation[1][2] * v.k;
	out->k = rotation[2][0] * v.i + rotation[2][1] * v.j + rotation[2][2] * v.k;
}

static void vr_multiply_rotations(real a[3][3], real b[3][3], real out[3][3])
{
	real r[3][3];
	int row, column;

	for (row = 0; row < 3; row++)
		for (column = 0; column < 3; column++)
			r[row][column] = a[row][0] * b[0][column] + a[row][1] * b[1][column] + a[row][2] * b[2][column];
	memcpy(out, r, sizeof(r));
}

/* a node moved from `from` to `to` and turned by `rotation` about itself:
the matrix of it, or of a node it carries (whose old matrix is `node`) */
static void vr_carry(
	real_matrix4x3 *node,
	real_point3d const *from,
	real_point3d const *to,
	real rotation[3][3])
{
	real_vector3d offset;

	vr_point_minus(&node->position, from, &offset);
	vr_rotate_vector(rotation, &offset, &offset);
	node->position.x = to->x + offset.i;
	node->position.y = to->y + offset.j;
	node->position.z = to->z + offset.k;
	vr_rotate_vector(rotation, &node->forward, &node->forward);
	vr_rotate_vector(rotation, &node->left, &node->left);
	vr_rotate_vector(rotation, &node->up, &node->up);
}

/* one arm: the shoulder, and the hand's target; bones[] the arm's nodes */
static void vr_solve_arm(
	struct animation_graph *graph,
	real_matrix4x3 *matrices,
	short bones[NUMBER_OF_VR_ARM_BONES],
	real_point3d shoulder,
	real_point3d const *target,
	real_vector3d const *pole,
	boolean hand_stays)
{
	real_matrix4x3 old[NUMBER_OF_VR_ARM_BONES];
	real rotation[NUMBER_OF_VR_ARM_BONES][3][3];
	real_point3d moved_to[NUMBER_OF_VR_ARM_BONES];
	real_vector3d upper, fore, reach, direction, bend, old_direction;
	real upper_length, fore_length, distance, along, across;
	short moved[MAXIMUM_NODES_PER_ANIMATION];
	short index, bone;

	for (bone = 0; bone < NUMBER_OF_VR_ARM_BONES; bone++)
		old[bone] = matrices[bones[bone]];
	vr_point_minus(&old[_vr_arm_fore].position, &old[_vr_arm_upper].position, &upper);
	vr_point_minus(&old[_vr_arm_hand].position, &old[_vr_arm_fore].position, &fore);
	upper_length = vr_length(&upper);
	fore_length = vr_length(&fore);
	if (upper_length < 1e-4f || fore_length < 1e-4f)
		return;
	vr_point_minus(target, &shoulder, &reach);
	distance = vr_length(&reach);
	if (distance < 1e-4f)
		return;
	direction = reach;
	normalize3d(&direction);
	/* out of reach: the shoulder comes forward rather than the hand
	letting go */
	if (distance > (upper_length + fore_length) * 0.999f)
	{
		distance = (upper_length + fore_length) * 0.999f;
		shoulder.x = target->x - direction.i * distance;
		shoulder.y = target->y - direction.j * distance;
		shoulder.z = target->z - direction.k * distance;
	}
	if (distance < (real)fabs(upper_length - fore_length) + 1e-3f)
		distance = (real)fabs(upper_length - fore_length) + 1e-3f;
	/* the elbow: in the plane of the reach and the pole */
	{
		real dot = pole->i * direction.i + pole->j * direction.j + pole->k * direction.k;

		bend.i = pole->i - direction.i * dot;
		bend.j = pole->j - direction.j * dot;
		bend.k = pole->k - direction.k * dot;
		if (vr_length(&bend) < 1e-4f)
			return;
		normalize3d(&bend);
	}
	along = (upper_length * upper_length + distance * distance - fore_length * fore_length) / (2.0f * distance);
	across = upper_length * upper_length - along * along;
	across = across > 0.0f ? (real)sqrt(across) : 0.0f;
	moved_to[_vr_arm_upper] = shoulder;
	moved_to[_vr_arm_fore].x = shoulder.x + direction.i * along + bend.i * across;
	moved_to[_vr_arm_fore].y = shoulder.y + direction.j * along + bend.j * across;
	moved_to[_vr_arm_fore].z = shoulder.z + direction.k * along + bend.k * across;
	moved_to[_vr_arm_hand] = *target;

	/* each bone turned to point at the next joint, and the hand carried by
	the forearm's turn */
	vr_point_minus(&moved_to[_vr_arm_fore], &moved_to[_vr_arm_upper], &reach);
	vr_rotation_between(&upper, &reach, rotation[_vr_arm_upper]);
	vr_rotate_vector(rotation[_vr_arm_upper], &fore, &old_direction);
	vr_point_minus(&moved_to[_vr_arm_hand], &moved_to[_vr_arm_fore], &reach);
	vr_rotation_between(&old_direction, &reach, rotation[_vr_arm_fore]);
	vr_multiply_rotations(rotation[_vr_arm_fore], rotation[_vr_arm_upper], rotation[_vr_arm_fore]);
	if (hand_stays)
	{
		/* the hand holding the gun stays as the controller put it, and with
		it the gun (the hand's child) */
		memset(rotation[_vr_arm_hand], 0, sizeof(rotation[_vr_arm_hand]));
		rotation[_vr_arm_hand][0][0] = rotation[_vr_arm_hand][1][1] = rotation[_vr_arm_hand][2][2] = 1.0f;
		moved_to[_vr_arm_hand] = old[_vr_arm_hand].position;
	}
	else
	{
		memcpy(rotation[_vr_arm_hand], rotation[_vr_arm_fore], sizeof(rotation[_vr_arm_hand]));
	}

	/* every node under a moved bone goes with the nearest one above it;
	the graph lists parents before children */
	for (index = 0; index < graph->nodes.count && index < MAXIMUM_NODES_PER_ANIMATION; index++)
	{
		struct vr_animation_graph_node const *node =
			TAG_BLOCK_GET_ELEMENT(&graph->nodes, index, struct vr_animation_graph_node);

		moved[index] = NONE;
		for (bone = 0; bone < NUMBER_OF_VR_ARM_BONES; bone++)
		{
			if (bones[bone] == index)
				moved[index] = bone;
		}
		if (moved[index] == NONE && node->parent_node_index >= 0 && node->parent_node_index < index)
			moved[index] = moved[node->parent_node_index];
		if (moved[index] != NONE)
		{
			bone = moved[index];
			vr_carry(&matrices[index], &old[bone].position, &moved_to[bone], rotation[bone]);
		}
	}
}

void vr_render_first_person_ik(
	real_matrix4x3 *matrices,
	struct animation_graph *graph)
{
	static char const *const arms_setting_names[] = { "ik", "hidden", "animated" };
	static int arms = -1;
	static struct animation_graph *logged;
	short left[NUMBER_OF_VR_ARM_BONES], right[NUMBER_OF_VR_ARM_BONES];
	int side, bone;

	if (arms < 0)
	{
		char const *setting = config_string("vr.arms");

		for (arms = 0; arms < 3 && strcmp(setting, arms_setting_names[arms]); arms++)
			;
		if (arms == 3)
			arms = 0;
	}
	if (!vr_render.stereo || !vr_hand_aiming() || !graph)
		return;
	for (side = 0; side < 2; side++)
	{
		static char const *const bone_names[] = { "upperarm", "forearm", "wrist" };
		short *chain = side ? right : left;

		/* Halo's graphs name the hands' bones "wriste" (or "hand") */
		for (bone = 0; bone < NUMBER_OF_VR_ARM_BONES; bone++)
			chain[bone] = vr_find_node(graph, side ? "r " : "l ", bone_names[bone]);
		if (chain[_vr_arm_hand] == NONE)
			chain[_vr_arm_hand] = vr_find_node(graph, side ? "r " : "l ", "hand");
	}
	if (logged != graph)
	{
		short index;

		logged = graph;
		for (index = 0; index < graph->nodes.count; index++)
		{
			struct vr_animation_graph_node const *node =
				TAG_BLOCK_GET_ELEMENT(&graph->nodes, index, struct vr_animation_graph_node);

			platform_log("vr: first-person node %d '%s' parent %d", index, node->name, node->parent_node_index);
		}
		platform_log("vr: arms: left %d %d %d, right %d %d %d", left[0], left[1], left[2], right[0], right[1], right[2]);
	}
	for (side = 0; side < 2; side++)
	{
		short *chain = side ? right : left;

		for (bone = 0; bone < NUMBER_OF_VR_ARM_BONES; bone++)
		{
			if (chain[bone] == NONE)
				return;
		}
	}

	/* in the left hand: the whole model mirrored across the weapon
	camera's upright plane (its left axis the plane's normal), the grip
	then landing in the hand (vr_weapon_view takes the offset the other
	way); its triangles' winding turns over (halo_vr_mirror_winding) */
	if (vr_render_first_person_mirrored() && vr_render.weapon_camera_valid)
	{
		real_vector3d normal;
		real_point3d centre = vr_render.weapon_camera.position;
		short index;

		cross_product3d(&vr_render.weapon_camera.up, &vr_render.weapon_camera.forward, &normal);
		normalize3d(&normal);
		for (index = 0; index < graph->nodes.count && index < MAXIMUM_NODES_PER_ANIMATION; index++)
		{
			real_matrix4x3 *m = &matrices[index];
			real_vector3d *axes[3] = { &m->forward, &m->left, &m->up };
			real_vector3d offset;
			real along;
			int axis;

			vr_point_minus(&m->position, &centre, &offset);
			along = 2.0f * (offset.i * normal.i + offset.j * normal.j + offset.k * normal.k);
			m->position.x -= normal.i * along;
			m->position.y -= normal.j * along;
			m->position.z -= normal.k * along;
			for (axis = 0; axis < 3; axis++)
			{
				along = 2.0f * (axes[axis]->i * normal.i + axes[axis]->j * normal.j + axes[axis]->k * normal.k);
				axes[axis]->i -= normal.i * along;
				axes[axis]->j -= normal.j * along;
				axes[axis]->k -= normal.k * along;
			}
		}
	}

	if (arms == 2)
		return;
	if (arms == 1)
	{
		short gun = vr_find_node(graph, "frame", "gun");

		/* hidden: the arms' bones (and what they carry) shrunk to nothing,
		at the shoulder's place */
		short index;

		for (index = 0; index < graph->nodes.count && index < MAXIMUM_NODES_PER_ANIMATION; index++)
		{
			struct vr_animation_graph_node const *node =
				TAG_BLOCK_GET_ELEMENT(&graph->nodes, index, struct vr_animation_graph_node);
			boolean arm = FALSE;
			short at;

			/* an arm's bone, unless it is the gun's (the right hand's child)
			or under it */
			for (at = index; at >= 0 && at < graph->nodes.count && !arm;
				at = TAG_BLOCK_GET_ELEMENT(&graph->nodes, at, struct vr_animation_graph_node)->parent_node_index)
			{
				if (at == gun)
					break;
				arm = at == left[_vr_arm_upper] || at == right[_vr_arm_upper];
				if (TAG_BLOCK_GET_ELEMENT(&graph->nodes, at, struct vr_animation_graph_node)->parent_node_index >= at)
					break;
			}
			(void)node;
			if (arm)
				matrices[index].scale = 0.0f;
		}
		return;
	}

	{
		real units = vr_units_per_metre();
		real_point3d head = vr_render.head_camera.position, shoulder, target;
		real_vector3d forward, right_side, up = { 0.0f, 0.0f, 1.0f }, pole;

		if (!vr_heading_forward(forward.n))
			forward = vr_render.head_camera.forward;
		forward.k = 0.0f;
		normalize3d(&forward);
		right_side.i = forward.j;
		right_side.j = -forward.i;
		right_side.k = 0.0f;
		/* the model's right arm holds the gun; in the left hand the model is
		mirrored, so that arm is the left one and the other reaches the right
		controller */
		int weapon_hand = vr_weapon_hand();

		for (side = 0; side < 2; side++)
		{
			short *chain = side ? right : left;
			boolean gun_arm = side == 1;
			real outward = (gun_arm == (weapon_hand == 1)) ? 1.0f : -1.0f;
			int controller = gun_arm ? weapon_hand : 1 - weapon_hand;

			/* shoulders below and either side of the eyes, a little back */
			shoulder.x = head.x + (right_side.i * 0.17f * outward - forward.i * 0.06f) * units;
			shoulder.y = head.y + (right_side.j * 0.17f * outward - forward.j * 0.06f) * units;
			shoulder.z = head.z - 0.22f * units;
			/* elbows down, out and back */
			pole.i = right_side.i * 0.6f * outward - forward.i * 0.3f;
			pole.j = right_side.j * 0.6f * outward - forward.j * 0.3f;
			pole.k = -1.0f;
			target = matrices[chain[_vr_arm_hand]].position;
			if (!gun_arm)
			{
				/* the other hand on its controller, unless it is near the
				gun's grip for it */
				real_point3d hand;
				real_vector3d hand_forward, hand_up, gap;

				if (vr_hand_world(controller, vr_render.game_camera_position.n, hand.n, hand_forward.n, hand_up.n))
				{
					vr_point_minus(&hand, &target, &gap);
					if (vr_length(&gap) > 0.15f * units)
						target = hand;
				}
			}
			vr_solve_arm(graph, matrices, chain, shoulder, &target, &pole, gun_arm);
		}
	}
}

#endif /* HALO_VR */
