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
#include "models/model_definitions.h"
#include "objects/object_definitions.h"

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
	/* the HUD pass is drawn from the head's place, turned as its panel is
	(vr_hud_view), over the panel's field */
	boolean hud_from_head;
	real_rectangle2d hud_bounds;
	real_point3d hud_position;
	real_vector3d hud_forward, hud_up;
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
	/* the game's camera of the frame, as interpolated (vr.diag_motion) */
	real_point3d diag_camera;
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
in the nearest vehicle whose tag name has vr.diag_vehicle in it (any, if
empty), as its driver or with vr.diag_seat "gunner" its gunner (for checking
vehicles unattended; debug.network_test_vehicle does the same in network
tests). The map's vehicles are logged. */
static void vr_diag_drive(
	long unit_index)
{
	static real seconds = -1.0f;
	static long first_tick = NONE;
	struct object_iterator vehicles;
	long nearest_index = NONE;
	real nearest_distance = 0.0f;
	short seat_index;
	char const *wanted;
	boolean gunner;

	if (seconds < 0.0f)
		seconds = (real)config_real("vr.diag_drive_seconds");
	if (seconds <= 0.0f || unit_index == NONE || object_get(unit_index)->object.parent_object_index != NONE)
		return;
	if (first_tick == NONE)
		first_tick = game_time_get();
	if (game_time_get() - first_tick < (long)(seconds * TICKS_PER_SECOND))
		return;
	seconds = 0.0f;
	wanted = config_string("vr.diag_vehicle");
	gunner = !strcmp(config_string("vr.diag_seat"), "gunner");
	object_iterator_new(&vehicles, _object_mask_vehicle, 0);
	while (object_iterator_next(&vehicles))
	{
		real distance = distance_squared3d(&object_get(unit_index)->object.position,
			&object_get(vehicles.index)->object.position);
		char const *name = tag_get_name(object_get(vehicles.index)->definition_index);

		platform_log("vr: diag drive: vehicle %lx '%s', %.1f units away", vehicles.index, name ? name : "?",
			(real)sqrt(distance));
		if (*wanted && (!name || !strstr(name, wanted)))
			continue;
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
	/* (a vehicle far off is brought beside the player first: seated from
	afar, the player did not stay in it) */
	if (nearest_distance > 2.0f * 2.0f)
	{
		struct object_datum *player = object_get(unit_index);
		real_point3d beside = player->object.position;
		real_vector3d forward = player->object.forward;

		forward.k = 0.0f;
		if (magnitude_squared3d(&forward) > 1e-6f)
			normalize3d(&forward);
		beside.x += forward.i * 0.8f;
		beside.y += forward.j * 0.8f;
		beside.z += 0.1f;
		object_set_position(nearest_index, &beside, &forward, global_up3d);
		platform_log("vr: diag drive: brought vehicle %lx beside the player", nearest_index);
	}
	for (seat_index = 0; seat_index < unit_definition_get(object_get(nearest_index)->definition_index)->unit.seats.count;
		seat_index++)
	{
		if ((gunner ? unit_seat_is_gunner(nearest_index, seat_index) : unit_seat_is_driver(nearest_index, seat_index)) &&
			unit_enter_seat(unit_index, nearest_index, seat_index))
		{
			platform_log("vr: diag drive: %s of vehicle %lx", gunner ? "gunner" : "driver", nearest_index);
			return;
		}
	}
	platform_log("vr: diag drive: cannot take a %s's seat in vehicle %lx", gunner ? "gunner" : "driver", nearest_index);
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
	if (window_count >= 1)
		vr_render.diag_camera = windows[0].render_camera.position;
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
			vr_render.hud_from_head = vr_hud_view(vr_render.game_camera_position.n, heading.n,
				vr_render.hud_position.n, vr_render.hud_forward.n, vr_render.hud_up.n);
			/* (looking straight up or down, the last yaw holds) */
			if (horizontal > 0.2f)
			{
				vr_render.head_yaw = vr_yaw(&forward);
				vr_render.head_yaw_valid = TRUE;
			}
		}
	}
	/* the HUD, drawn from the head's place over its panel's field: its
	markers (nav points, friends' names) land on what they mark, and with
	the head aiming its crosshair is where the head looks */
	windows[2] = player;
	windows[3] = console;
	if (vr_render.hud_from_head)
	{
		struct render_camera *camera = &windows[2].rasterizer_camera;

		camera->position = vr_render.hud_position;
		camera->forward = vr_render.hud_forward;
		camera->up = vr_render.hud_up;
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
		/* parallel: none; opposite: a half turn about any axis across them
		(not the negated identity, a reflection, which turns a limb inside
		out) */
		memset(rotation, 0, sizeof(real) * 9);
		if (c >= 0.0f)
		{
			rotation[0][0] = rotation[1][1] = rotation[2][2] = 1.0f;
			return;
		}
		{
			real_vector3d across = fabsf(u.i) < 0.9f ? *global_forward3d : *global_left3d, a;
			int row, column;

			cross_product3d(&u, &across, &a);
			normalize3d(&a);
			for (row = 0; row < 3; row++)
			{
				for (column = 0; column < 3; column++)
					rotation[row][column] = 2.0f * a.n[row] * a.n[column] - (row == column ? 1.0f : 0.0f);
			}
		}
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

/* ---------- the free hand's fingers

The animation poses the other hand for the gun (or leaves it flat). Off the
gun it is posed here: each finger bent at its two joints (low, mid) toward
the palm, a little at rest, into a fist as the controller's grip closes; the
index follows the trigger. The palm's side is across the knuckles from the
wrist (Halo's axes: +x forward, +y left, +z up), the other way for a mirrored
hand. */

enum
{
	_vr_finger_thumb,
	_vr_finger_index,
	_vr_finger_middle,
	_vr_finger_ring,
	_vr_finger_pinky,
	NUMBER_OF_VR_FINGERS
};

/* a finger's joint ("low", "mid", "tip") of the side's hand, or NONE */
static short vr_find_finger(
	struct animation_graph *graph,
	char const *side,
	char const *finger,
	char const *joint)
{
	short index;

	for (index = 0; index < graph->nodes.count && index < MAXIMUM_NODES_PER_ANIMATION; index++)
	{
		struct vr_animation_graph_node const *node =
			TAG_BLOCK_GET_ELEMENT(&graph->nodes, index, struct vr_animation_graph_node);

		if (strstr(node->name, side) && strstr(node->name, finger) && strstr(node->name, joint))
			return index;
	}
	return NONE;
}

/* turns node `joint` and all it carries about the joint, its bone (toward
`child`) bent by `angle` radians toward `palm` */
static void vr_bend_joint(
	struct animation_graph *graph,
	real_matrix4x3 *matrices,
	short joint,
	short child,
	real_vector3d const *palm,
	real angle)
{
	real_vector3d bone, toward, bent;
	real rotation[3][3], along, length;
	real_point3d pivot = matrices[joint].position;
	short index;

	vr_point_minus(&matrices[child].position, &pivot, &bone);
	length = vr_length(&bone);
	if (length < 1e-5f || angle == 0.0f)
		return;
	bone.i /= length;
	bone.j /= length;
	bone.k /= length;
	/* the palm's direction square to the bone */
	along = palm->i * bone.i + palm->j * bone.j + palm->k * bone.k;
	toward.i = palm->i - bone.i * along;
	toward.j = palm->j - bone.j * along;
	toward.k = palm->k - bone.k * along;
	if (vr_length(&toward) < 1e-4f)
		return;
	normalize3d(&toward);
	bent.i = bone.i * (real)cos(angle) + toward.i * (real)sin(angle);
	bent.j = bone.j * (real)cos(angle) + toward.j * (real)sin(angle);
	bent.k = bone.k * (real)cos(angle) + toward.k * (real)sin(angle);
	vr_rotation_between(&bone, &bent, rotation);
	/* the joint and the nodes under it (the graph lists parents first) */
	for (index = joint; index < graph->nodes.count && index < MAXIMUM_NODES_PER_ANIMATION; index++)
	{
		short at = index;

		while (at != joint && at > joint)
			at = TAG_BLOCK_GET_ELEMENT(&graph->nodes, at, struct vr_animation_graph_node)->parent_node_index;
		if (at == joint)
			vr_carry(&matrices[index], &pivot, &pivot, rotation);
	}
}

static char const *const vr_finger_names[NUMBER_OF_VR_FINGERS] = { "thumb", "index", "middle", "ring", "pinky" };

/* the side's finger joints (low, mid, tip of each); FALSE if any is missing */
static boolean vr_find_fingers(
	struct animation_graph *graph,
	char const *side,
	short joints[NUMBER_OF_VR_FINGERS][3])
{
	int finger;

	for (finger = 0; finger < NUMBER_OF_VR_FINGERS; finger++)
	{
		joints[finger][0] = vr_find_finger(graph, side, vr_finger_names[finger], "low");
		joints[finger][1] = vr_find_finger(graph, side, vr_finger_names[finger], "mid");
		joints[finger][2] = vr_find_finger(graph, side, vr_finger_names[finger], "tip");
		if (joints[finger][0] == NONE || joints[finger][1] == NONE || joints[finger][2] == NONE)
			return FALSE;
	}
	return TRUE;
}

/* the hand's length (wrist to the middle knuckle) and the way its palm
faces: across the knuckles (pinky to index) from that line, the other way
for a right hand (or a mirrored left) */
static boolean vr_hand_frame(
	real_matrix4x3 const *matrices,
	short wrist,
	short joints[NUMBER_OF_VR_FINGERS][3],
	char const *side,
	boolean mirrored,
	real_vector3d *along,
	real_vector3d *palm)
{
	real_vector3d across;

	vr_point_minus(&matrices[joints[_vr_finger_middle][0]].position, &matrices[wrist].position, along);
	vr_point_minus(&matrices[joints[_vr_finger_index][0]].position, &matrices[joints[_vr_finger_pinky][0]].position, &across);
	cross_product3d(along, &across, palm);
	if (vr_length(palm) < 1e-6f || vr_length(along) < 1e-6f)
		return FALSE;
	normalize3d(along);
	normalize3d(palm);
	if ((strcmp(side, " r ") == 0) != (mirrored != FALSE))
	{
		palm->i = -palm->i;
		palm->j = -palm->j;
		palm->k = -palm->k;
	}
	return TRUE;
}

/* an orthonormal frame's columns from a direction and a second one turned
square to it */
static void vr_frame_of(real_vector3d const *first, real_vector3d const *second, real frame[3][3])
{
	real_vector3d a = *first, b = *second, c;
	real along = a.i * b.i + a.j * b.j + a.k * b.k;

	b.i -= a.i * along;
	b.j -= a.j * along;
	b.k -= a.k * along;
	normalize3d(&b);
	cross_product3d(&a, &b, &c);
	frame[0][0] = a.i; frame[0][1] = b.i; frame[0][2] = c.i;
	frame[1][0] = a.j; frame[1][1] = b.j; frame[1][2] = c.j;
	frame[2][0] = a.k; frame[2][1] = b.k; frame[2][2] = c.k;
}

/* The free hand, relaxed: its length mostly along the forearm's line (a
straight wrist; following the controller's forward bent it limp), turned
a little toward the grip's forward, and rolled as the controller is, its
palm the way the player's faces (OpenXR's grip +x points to the player's
right for either hand: out of the left palm, into the back of the right).
The whole hand turns about the wrist, its fingers with it. */
#define VR_FREE_HAND_FOLLOWS_GRIP 0.25f

static void vr_orient_free_hand(
	struct animation_graph *graph,
	real_matrix4x3 *matrices,
	short forearm,
	short wrist,
	char const *side,
	boolean mirrored,
	int controller,
	real_vector3d const *hand_forward,
	real_vector3d const *hand_up)
{
	short joints[NUMBER_OF_VR_FINGERS][3];
	real_vector3d along, palm, right, palm_wanted, arm, along_wanted, grip_forward = *hand_forward;
	real model[3][3], wanted[3][3], rotation[3][3];
	real_point3d pivot = matrices[wrist].position;
	int row, column;
	short index;

	if (!vr_find_fingers(graph, side, joints) ||
		!vr_hand_frame(matrices, wrist, joints, side, mirrored, &along, &palm))
	{
		return;
	}
	cross_product3d(hand_forward, hand_up, &right);
	if (vr_length(&right) < 1e-6f)
		return;
	normalize3d(&right);
	palm_wanted = right;
	if (controller == 1)
	{
		palm_wanted.i = -palm_wanted.i;
		palm_wanted.j = -palm_wanted.j;
		palm_wanted.k = -palm_wanted.k;
	}
	vr_point_minus(&matrices[wrist].position, &matrices[forearm].position, &arm);
	if (vr_length(&arm) < 1e-6f)
		return;
	normalize3d(&arm);
	normalize3d(&grip_forward);
	along_wanted.i = arm.i * (1.0f - VR_FREE_HAND_FOLLOWS_GRIP) + grip_forward.i * VR_FREE_HAND_FOLLOWS_GRIP;
	along_wanted.j = arm.j * (1.0f - VR_FREE_HAND_FOLLOWS_GRIP) + grip_forward.j * VR_FREE_HAND_FOLLOWS_GRIP;
	along_wanted.k = arm.k * (1.0f - VR_FREE_HAND_FOLLOWS_GRIP) + grip_forward.k * VR_FREE_HAND_FOLLOWS_GRIP;
	if (vr_length(&along_wanted) < 1e-6f)
		return;
	normalize3d(&along_wanted);
	vr_frame_of(&along, &palm, model);
	vr_frame_of(&along_wanted, &palm_wanted, wanted);
	/* wanted * model^T */
	for (row = 0; row < 3; row++)
	{
		for (column = 0; column < 3; column++)
		{
			rotation[row][column] = wanted[row][0] * model[column][0] + wanted[row][1] * model[column][1] +
				wanted[row][2] * model[column][2];
		}
	}
	for (index = wrist; index < graph->nodes.count && index < MAXIMUM_NODES_PER_ANIMATION; index++)
	{
		short at = index;

		while (at > wrist)
			at = TAG_BLOCK_GET_ELEMENT(&graph->nodes, at, struct vr_animation_graph_node)->parent_node_index;
		if (at == wrist)
			vr_carry(&matrices[index], &pivot, &pivot, rotation);
	}
}

static void vr_pose_free_hand(
	struct animation_graph *graph,
	real_matrix4x3 *matrices,
	short wrist,
	char const *side,
	boolean mirrored,
	int controller)
{
	/* degrees each joint is bent toward the palm (from the bone before it:
	for a low joint the hand's length), at rest and in a fist; the fingers
	are set to these, opened from the animation's half-closed grip. The
	thumb, whose base points aside, is bent by these on top of the
	animation's instead */
	static const real relaxed[NUMBER_OF_VR_FINGERS][2] = { { 5, 8 }, { 14, 18 }, { 16, 22 }, { 20, 26 }, { 24, 30 } };
	static const real fist[NUMBER_OF_VR_FINGERS][2] = { { 40, 45 }, { 80, 90 }, { 85, 95 }, { 85, 95 }, { 85, 95 } };
	short joints[NUMBER_OF_VR_FINGERS][3];
	real_vector3d along, palm;
	float grip, trigger;
	int finger, joint;

	if (!vr_find_fingers(graph, side, joints) ||
		!vr_hand_frame(matrices, wrist, joints, side, mirrored, &along, &palm))
	{
		return;
	}
	vr_hand_controls(controller, &grip, &trigger);
	for (finger = 0; finger < NUMBER_OF_VR_FINGERS; finger++)
	{
		real closed = finger == _vr_finger_index && trigger > grip ? trigger * 0.6f : grip;

		for (joint = 0; joint < 2; joint++)
		{
			real wanted = (relaxed[finger][joint] + (fist[finger][joint] - relaxed[finger][joint]) * closed) *
				(real)(3.14159265 / 180.0);
			real bend = wanted;

			if (finger != _vr_finger_thumb)
			{
				/* the joint's bend now: its bone against the one before,
				toward the palm */
				real_vector3d before, bone;
				short previous = joint ? joints[finger][0] : NONE;

				if (previous == NONE)
					before = along;
				else
					vr_point_minus(&matrices[joints[finger][1]].position, &matrices[previous].position, &before);
				vr_point_minus(&matrices[joints[finger][joint + 1]].position, &matrices[joints[finger][joint]].position, &bone);
				if (vr_length(&before) > 1e-6f && vr_length(&bone) > 1e-6f)
				{
					real ahead, toward;

					normalize3d(&before);
					normalize3d(&bone);
					ahead = bone.i * before.i + bone.j * before.j + bone.k * before.k;
					toward = bone.i * palm.i + bone.j * palm.j + bone.k * palm.k;
					bend = wanted - (real)atan2(toward, ahead);
				}
			}
			vr_bend_joint(graph, matrices, joints[finger][joint], joints[finger][joint + 1], &palm, bend);
		}
	}
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

/* the head's yaw as a level direction (Halo's axes). Looking straight
down, its forward has no yaw left: its up (then pointing ahead) gives it */
static void vr_head_yaw_forward(
	real_vector3d *forward)
{
	*forward = vr_render.head_camera.forward;
	forward->k = 0.0f;
	if (magnitude_squared3d(forward) < 0.25f)
	{
		real toward = vr_render.head_camera.forward.k < 0.0f ? 1.0f : -1.0f;

		forward->i += vr_render.head_camera.up.i * toward;
		forward->j += vr_render.head_camera.up.j * toward;
	}
	if (magnitude_squared3d(forward) < 1e-6f && !vr_heading_forward(forward->n))
		*forward = vr_render.head_camera.forward;
	forward->k = 0.0f;
	normalize3d(forward);
}

/* vr.arms: 0 "ik", 1 "hidden", 2 "animated" */
static int vr_render_arms_setting(
	void)
{
	static char const *const arms_setting_names[] = { "ik", "hidden", "animated" };
	static int arms = -1;

	if (arms < 0)
	{
		char const *setting = config_string("vr.arms");

		for (arms = 0; arms < 3 && strcmp(setting, arms_setting_names[arms]); arms++)
			;
		if (arms == 3)
			arms = 0;
	}
	return arms;
}

int vr_render_first_person_arms_reach(
	void)
{
	return vr_render.stereo && vr_hand_aiming() && vr_render_arms_setting() == 0;
}

void vr_render_first_person_ik(
	real_matrix4x3 *matrices,
	struct animation_graph *graph)
{
	static struct animation_graph *logged;
	short left[NUMBER_OF_VR_ARM_BONES], right[NUMBER_OF_VR_ARM_BONES];
	int side, bone, arms = vr_render_arms_setting();

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

		/* The shoulders turn with the head (its yaw), and sit behind the
		eyes: the model's upper arms end partway, open, and a shoulder left
		where the body faces would bring that end into view as the head
		turns toward it. The hands stay on the controllers (the arms reach
		for them). */
		vr_head_yaw_forward(&forward);
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
			boolean gun_arm = side == 1, on_gun;
			real_vector3d free_forward = { 1.0f, 0.0f, 0.0f }, free_up = { 0.0f, 0.0f, 1.0f };
			real outward = (gun_arm == (weapon_hand == 1)) ? 1.0f : -1.0f;
			int controller = gun_arm ? weapon_hand : 1 - weapon_hand;

			/* shoulders below and either side of the eyes, behind them */
			shoulder.x = head.x + (right_side.i * 0.17f * outward - forward.i * 0.12f) * units;
			shoulder.y = head.y + (right_side.j * 0.17f * outward - forward.j * 0.12f) * units;
			shoulder.z = head.z - 0.22f * units;
			/* elbows down, out and back */
			pole.i = right_side.i * 0.6f * outward - forward.i * 0.3f;
			pole.j = right_side.j * 0.6f * outward - forward.j * 0.3f;
			pole.k = -1.0f;
			target = matrices[chain[_vr_arm_hand]].position;
			/* a hand on the gun keeps the grip the animation gave it, not the
			forearm's turn (which would twist its fingers off the gun) */
			on_gun = TRUE;
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
					{
						/* the grip pose is in the palm: the wrist a little
						behind it, along the hand */
						normalize3d(&hand_forward);
						target.x = hand.x - hand_forward.i * 0.07f * units;
						target.y = hand.y - hand_forward.j * 0.07f * units;
						target.z = hand.z - hand_forward.k * 0.07f * units;
						free_forward = hand_forward;
						free_up = hand_up;
						on_gun = FALSE;
					}
				}
			}
			vr_solve_arm(graph, matrices, chain, shoulder, &target, &pole, on_gun);
			/* the hand off the gun: held as the controller is, its fingers
			relaxed or closing with the grip */
			if (!on_gun)
			{
				boolean mirrored = vr_render_first_person_mirrored();
				char const *hand_side = side ? " r " : " l ";

				vr_orient_free_hand(graph, matrices, chain[_vr_arm_fore], chain[_vr_arm_hand], hand_side, mirrored,
					controller, &free_forward, &free_up);
				vr_pose_free_hand(graph, matrices, chain[_vr_arm_hand], hand_side, mirrored, controller);
			}
		}
	}
}

/* the game's camera of the frame (vr.diag_motion, vr_frame.c) */
void vr_render_diag_camera(
	float out[3])
{
	out[0] = vr_render.diag_camera.x;
	out[1] = vr_render.diag_camera.y;
	out[2] = vr_render.diag_camera.z;
}

/* ---------- the player's own body (vr.body)

Halo draws no body of the player's own in first person: render_object_list
skips the unit the camera is. With vr.body "legs" (the hips down) or
"torso" (the chest down) the eyes' passes draw it,
on foot and outside cutscenes, with no head, neck or arms (the first-person
arms are the player's) and without what it carries (its third-person
weapon): its bones' matrices are a copy (vr_render_body_nodes), turned to
the head's yaw and placed under the head, behind and below the eyes
(vr_body_back, vr_body_down). */

/* vr.body: 0 "off", 1 "legs" (the hips down), 2 "torso" (the chest down) */
enum
{
	_vr_body_off,
	_vr_body_legs,
	_vr_body_torso
};

/* metres the body sits behind and below the eyes. Halo's near plane is
some 19 cm out at this scale: with the torso, the collar and shoulder pads
(under the eyes) must stay beyond it or they are cut into shards; and too
far back, looking down shows the floor ahead of the feet. With the legs
alone nothing is near the eyes. */
static const real vr_body_back[3] = { 0.0f, 0.05f, 0.10f };
static const real vr_body_down[3] = { 0.0f, 0.0f, 0.15f };
#define VR_BODY_BACK_LOOKING_DOWN 0.25f

static int vr_render_body_setting(
	void)
{
	static int body = -1, generation = -1;

	/* (read again when the VR settings change it) */
	if (body < 0 || generation != vr_settings_generation())
	{
		generation = vr_settings_generation();
		char const *setting = config_string("vr.body");

		body = !strcmp(setting, "legs") ? _vr_body_legs : !strcmp(setting, "torso") ? _vr_body_torso : _vr_body_off;
	}
	return body;
}

/* the unit sits in the seat the view rides in (vr.vehicle_view
"first_person"; in the chase view the game draws it anyway) */
static boolean vr_body_seated(
	long object_index)
{
	return vr_render.seat.seated && vr_render.seat.unit_index == object_index && vr_render_first_person_vehicles();
}

boolean vr_render_body_wanted(
	long object_index)
{
	short local_player_index;
	long player_index;
	struct object_datum *object;

	if (!vr_render_body_setting() || !VR_RENDER_EYE() || !vr_render.stereo || cinematic_in_progress())
		return FALSE;
	local_player_index = local_player_get_next(NONE);
	player_index = local_player_index != NONE ? local_player_get_player_index(local_player_index) : NONE;
	if (player_index == NONE || player_get(player_index)->unit_index != object_index)
		return FALSE;
	object = object_get(object_index);
	/* in a seat (the vehicle's child), seen from it: the body as the game
	sits it there, its hands on the controls */
	if (object->object.parent_object_index != NONE)
		return vr_body_seated(object_index);
	/* on foot, only seen from inside it (a scripted or transition camera
	elsewhere shows the whole unit, as the game draws it) */
	return vr_aiming() && director_get_perspective(local_player_index) == _director_perspective_first_person;
}

/* the node, or one above it, is the head, a clavicle (the shoulder: with
the upper arm gone its triangles stretched into shards) or an upper arm, or
for the legs alone the spine (all above the hips), and on foot the neck.
In a seat only the head: the arms are on the vehicle's controls, and the
neck stays, the head shrunk into its base (vr_render_body_nodes) closing it
like a dimple (with the neck gone too, its opening looked into the body).
On foot the neck, kept, rose toward the eyes: there it goes */
static boolean vr_body_node_hidden(
	struct model *model,
	short node_index,
	boolean seated)
{
	boolean legs = !seated && vr_render_body_setting() == _vr_body_legs;

	short depth;

	for (depth = 0; node_index >= 0 && node_index < model->nodes.count && depth < MAXIMUM_NODES_PER_MODEL; depth++)
	{
		struct model_node const *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, node_index, struct model_node);

		if (strstr(node->name, "head") ||
			(!seated && (strstr(node->name, "neck") || strstr(node->name, "clavicle") ||
				strstr(node->name, "upperarm"))) ||
			(legs && strstr(node->name, "spine")))
			return TRUE;
		node_index = node->parent_node_index;
	}
	return FALSE;
}

/* the model's node named with `name` (" l upperarm"), or NONE */
static short vr_body_find_node(
	struct model *model,
	char const *name)
{
	short index;

	for (index = 0; index < model->nodes.count && index < MAXIMUM_NODES_PER_MODEL; index++)
	{
		if (strstr(TAG_BLOCK_GET_ELEMENT(&model->nodes, index, struct model_node)->name, name))
			return index;
	}
	return NONE;
}

/* the node is `root` or under it */
static boolean vr_body_under(
	struct model *model,
	short node_index,
	short root)
{
	short depth;

	for (depth = 0; node_index >= 0 && node_index < model->nodes.count && depth < MAXIMUM_NODES_PER_MODEL; depth++)
	{
		if (node_index == root)
			return TRUE;
		node_index = TAG_BLOCK_GET_ELEMENT(&model->nodes, node_index, struct model_node)->parent_node_index;
	}
	return FALSE;
}

/* turns `root` and all under it by `rotation` about `pivot` */
static void vr_body_turn(
	struct model *model,
	real_matrix4x3 *matrices,
	short node_count,
	short root,
	real_point3d const *pivot,
	real rotation[3][3])
{
	short index;

	for (index = 0; index < node_count; index++)
	{
		if (vr_body_under(model, index, root))
			vr_carry(&matrices[index], pivot, pivot, rotation);
	}
}

/* an arm (its upper arm, forearm and hand nodes) from where its shoulder
now is to the hand's place `target`, the elbow bent toward `pole` */
static void vr_body_reach(
	struct model *model,
	real_matrix4x3 *matrices,
	short node_count,
	short upper,
	short fore,
	short hand,
	real_point3d const *target,
	real_vector3d const *pole)
{
	real_point3d shoulder = matrices[upper].position, elbow;
	real_vector3d upper_bone, fore_bone, reach, direction, bend, now, wanted;
	real upper_length, fore_length, distance, along, across, dot, rotation[3][3];

	vr_point_minus(&matrices[fore].position, &shoulder, &upper_bone);
	vr_point_minus(&matrices[hand].position, &matrices[fore].position, &fore_bone);
	upper_length = vr_length(&upper_bone);
	fore_length = vr_length(&fore_bone);
	vr_point_minus(target, &shoulder, &reach);
	distance = vr_length(&reach);
	if (upper_length < 1e-5f || fore_length < 1e-5f || distance < 1e-5f)
		return;
	direction = reach;
	normalize3d(&direction);
	if (distance > (upper_length + fore_length) * 0.999f)
		distance = (upper_length + fore_length) * 0.999f;
	if (distance < (real)fabs(upper_length - fore_length) + 1e-4f)
		distance = (real)fabs(upper_length - fore_length) + 1e-4f;
	dot = pole->i * direction.i + pole->j * direction.j + pole->k * direction.k;
	bend.i = pole->i - direction.i * dot;
	bend.j = pole->j - direction.j * dot;
	bend.k = pole->k - direction.k * dot;
	if (vr_length(&bend) < 1e-5f)
		return;
	normalize3d(&bend);
	along = (upper_length * upper_length + distance * distance - fore_length * fore_length) / (2.0f * distance);
	across = upper_length * upper_length - along * along;
	across = across > 0.0f ? (real)sqrt(across) : 0.0f;
	elbow.x = shoulder.x + direction.i * along + bend.i * across;
	elbow.y = shoulder.y + direction.j * along + bend.j * across;
	elbow.z = shoulder.z + direction.k * along + bend.k * across;
	/* the upper arm to the elbow (carrying the rest), then the forearm to
	the hand's place */
	vr_point_minus(&elbow, &shoulder, &wanted);
	vr_rotation_between(&upper_bone, &wanted, rotation);
	vr_body_turn(model, matrices, node_count, upper, &shoulder, rotation);
	vr_point_minus(&matrices[hand].position, &matrices[fore].position, &now);
	vr_point_minus(target, &matrices[fore].position, &wanted);
	elbow = matrices[fore].position;
	vr_rotation_between(&now, &wanted, rotation);
	vr_body_turn(model, matrices, node_count, fore, &elbow, rotation);
}

/* In a seat the game's body holds the vehicle's controls, but the view
rides at its head: its shoulders sit inside Halo's near plane (some 19 cm
out) and were cut into flat shards across the view. The chest and all on
it go back and down (VR_SEAT_BODY_BACK, _DOWN, along the seat's facing),
each arm then reaches from its moved shoulder to where the animation had
its hand, its elbow bent as before, and the hands are put back exactly as
the animation has them: on the wheel, the handles, the controls. */
#define VR_SEAT_BODY_BACK 0.12f
#define VR_SEAT_BODY_DOWN 0.04f
/* the chest follows the head: turned by this share of the head's turn from
the body's way, up to the most, and leaned toward where the head has gone
(from its place at rest), up to the most (radians) */
#define VR_SEAT_CHEST_TURN 0.6f
#define VR_SEAT_CHEST_TURN_MOST 0.61f
#define VR_SEAT_CHEST_LEAN_MOST 0.44f

static void vr_body_seat_lean_back(
	struct model *model,
	real_matrix4x3 *matrices,
	short node_count)
{
	static char const *const arm_names[2][3] =
	{
		{ " l upperarm", " l forearm", " l hand" },
		{ " r upperarm", " r forearm", " r hand" },
	};
	real_matrix4x3 animated[MAXIMUM_NODES_PER_MODEL];
	short spine = vr_body_find_node(model, " spine");
	short arms[2][3];
	real units = vr_units_per_metre();
	real_vector3d back;
	int side, bone;
	short index;

	if (spine == NONE || node_count > MAXIMUM_NODES_PER_MODEL)
		return;
	for (side = 0; side < 2; side++)
	{
		for (bone = 0; bone < 3; bone++)
		{
			arms[side][bone] = vr_body_find_node(model, arm_names[side][bone]);
			if (arms[side][bone] == NONE)
				return;
		}
	}
	memcpy(animated, matrices, node_count * sizeof(real_matrix4x3));
	/* the way the body faces, from its shoulders (Halo's axes: +x forward,
	+y left, +z up: forward is left across up); a gunner's turns with the
	turret, apart from the vehicle */
	{
		real_vector3d left, facing, aside, head_forward, head_moved, leaning, chest_up;
		real turn, c, s, ahead, sideways, height, rotation[3][3], tilt[3][3], yaw[3][3];
		short neck = vr_body_find_node(model, " neck");
		real_point3d pivot = matrices[spine].position;

		vr_point_minus(&matrices[arms[0][0]].position, &matrices[arms[1][0]].position, &left);
		cross_product3d(&left, global_up3d, &facing);
		facing.k = 0.0f;
		if (magnitude_squared3d(&facing) < 1e-8f)
			return;
		normalize3d(&facing);
		cross_product3d(global_up3d, &facing, &aside);
		back.i = -facing.i * VR_SEAT_BODY_BACK * units;
		back.j = -facing.j * VR_SEAT_BODY_BACK * units;
		back.k = -VR_SEAT_BODY_DOWN * units;
		/* the chest turned and leaned after the head, about the spine's base */
		vr_head_yaw_forward(&head_forward);
		{
			/* (fading to none as the head looks behind, from 120 degrees: a
			look over the shoulder would flip it from one side to the other) */
			real angle = (real)atan2(facing.i * head_forward.j - facing.j * head_forward.i,
				facing.i * head_forward.i + facing.j * head_forward.j);
			real behind = (real)fabs(angle) > 2.0944f ? (3.14159265f - (real)fabs(angle)) / (3.14159265f - 2.0944f) : 1.0f;

			turn = PIN(angle * VR_SEAT_CHEST_TURN, -VR_SEAT_CHEST_TURN_MOST, VR_SEAT_CHEST_TURN_MOST) * PIN(behind, 0.0f, 1.0f);
		}
		c = (real)cos(turn);
		s = (real)sin(turn);
		memset(yaw, 0, sizeof(yaw));
		yaw[0][0] = c;
		yaw[0][1] = -s;
		yaw[1][0] = s;
		yaw[1][1] = c;
		yaw[2][2] = 1.0f;
		vr_point_minus(&vr_render.head_camera.position, &vr_render.game_camera_position, &head_moved);
		ahead = head_moved.i * facing.i + head_moved.j * facing.j;
		sideways = head_moved.i * aside.i + head_moved.j * aside.j;
		/* (the lean's lever: the spine's base to the neck) */
		height = 0.5f * units;
		if (neck != NONE)
		{
			real_vector3d spine_length;

			vr_point_minus(&matrices[neck].position, &pivot, &spine_length);
			if (vr_length(&spine_length) > 1e-4f)
				height = vr_length(&spine_length);
		}
		leaning.i = facing.i * ahead + aside.i * sideways;
		leaning.j = facing.j * ahead + aside.j * sideways;
		leaning.k = 0.0f;
		/* (at most VR_SEAT_CHEST_LEAN_MOST whichever way) */
		{
			real most = height * (real)tan(VR_SEAT_CHEST_LEAN_MOST), length = vr_length(&leaning);

			if (length > most)
			{
				leaning.i *= most / length;
				leaning.j *= most / length;
			}
		}
		chest_up.i = leaning.i;
		chest_up.j = leaning.j;
		chest_up.k = height;
		vr_rotation_between(global_up3d, &chest_up, tilt);
		/* (turned about the vertical first, then tilted toward the head's
		move as measured: yaw * tilt would turn the lean by the turn) */
		vr_multiply_rotations(tilt, yaw, rotation);
		vr_body_turn(model, matrices, node_count, spine, &pivot, rotation);
	}
	for (index = 0; index < node_count; index++)
	{
		if (vr_body_under(model, index, spine))
		{
			matrices[index].position.x += back.i;
			matrices[index].position.y += back.j;
			matrices[index].position.z += back.k;
		}
	}
	for (side = 0; side < 2; side++)
	{
		short upper = arms[side][0], fore = arms[side][1], hand = arms[side][2];
		real_vector3d pole;

		/* the elbow's side of the shoulder-to-hand line, as animated */
		pole.i = animated[fore].position.x - (animated[upper].position.x + animated[hand].position.x) * 0.5f;
		pole.j = animated[fore].position.y - (animated[upper].position.y + animated[hand].position.y) * 0.5f;
		pole.k = animated[fore].position.z - (animated[upper].position.z + animated[hand].position.z) * 0.5f;
		vr_body_reach(model, matrices, node_count, upper, fore, hand, &animated[hand].position, &pole);
		/* the hand (and its fingers) as the animation has it, where the
		forearm reached: on the controls, or short of them if out of reach
		(else it came off at the wrist) */
		{
			real_vector3d short_by;

			vr_point_minus(&matrices[hand].position, &animated[hand].position, &short_by);
			for (index = 0; index < node_count; index++)
			{
				if (vr_body_under(model, index, hand))
				{
					matrices[index] = animated[index];
					matrices[index].position.x += short_by.i;
					matrices[index].position.y += short_by.j;
					matrices[index].position.z += short_by.k;
				}
			}
		}
	}
}

void vr_render_body_nodes(
	long object_index,
	real_matrix4x3 *matrices,
	short node_count)
{
	static long logged_model = NONE;
	struct object_datum *object = object_get(object_index);
	long model_index = object_definition_get(object->definition_index)->object.model.index;
	struct model *model = model_index != NONE ? model_definition_get(model_index) : NULL;
	boolean seated = object->object.parent_object_index != NONE;
	real units = vr_units_per_metre();
	real_vector3d body_forward, head_forward, shift;
	real rotation[3][3], c, s, back;
	real_point3d pivot;
	short index;

	if (!model)
		return;
	if (logged_model != model_index)
	{
		logged_model = model_index;
		for (index = 0; index < model->nodes.count; index++)
		{
			struct model_node const *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, index, struct model_node);

			platform_log("vr: body node %d '%s' parent %d%s", index, node->name, node->parent_node_index,
				vr_body_node_hidden(model, index, seated) ? " (hidden)" : "");
		}
	}
	/* no head, neck or arms: each hidden bone shrunk to nothing at the
	nearest kept one above it (inside the chest), so that the collar and the
	shoulders fold in rather than stretch out to where the neck and arms
	were (spikes across the view) */
	for (index = 0; index < node_count && index < model->nodes.count; index++)
	{
		if (vr_body_node_hidden(model, index, seated))
		{
			short kept = TAG_BLOCK_GET_ELEMENT(&model->nodes, index, struct model_node)->parent_node_index;

			while (kept >= 0 && kept < node_count && vr_body_node_hidden(model, kept, seated))
				kept = TAG_BLOCK_GET_ELEMENT(&model->nodes, kept, struct model_node)->parent_node_index;
			if (kept >= 0 && kept < node_count)
				matrices[index].position = matrices[kept].position;
			matrices[index].scale = 0.0f;
		}
	}
	/* in a seat the game has it where it belongs (and the view rides the
	seat): only its chest goes back, out of the near plane, the hands kept
	on the controls */
	if (seated)
	{
		vr_body_seat_lean_back(model, matrices, node_count);
		return;
	}
	/* under the head: moved across by the head's offset from the game's
	camera, turned from the unit's facing to the head's yaw about the head,
	then set back */
	shift.i = vr_render.head_camera.position.x - vr_render.game_camera_position.x;
	shift.j = vr_render.head_camera.position.y - vr_render.game_camera_position.y;
	body_forward = object->object.forward;
	body_forward.k = 0.0f;
	if (magnitude_squared3d(&body_forward) < 1e-6f)
		return;
	normalize3d(&body_forward);
	vr_head_yaw_forward(&head_forward);
	c = body_forward.i * head_forward.i + body_forward.j * head_forward.j;
	s = body_forward.i * head_forward.j - body_forward.j * head_forward.i;
	memset(rotation, 0, sizeof(rotation));
	rotation[0][0] = c;
	rotation[0][1] = -s;
	rotation[1][0] = s;
	rotation[1][1] = c;
	rotation[2][2] = 1.0f;
	pivot = vr_render.head_camera.position;
	/* looking down, the body goes back with it (up to VR_BODY_BACK_LOOKING_DOWN
	more, looking straight down): the eyes meet the chest, not the collar */
	{
		real down = -vr_render.head_camera.forward.k;

		back = vr_body_back[vr_render_body_setting()] + (down > 0.0f ? down : 0.0f) * VR_BODY_BACK_LOOKING_DOWN;
	}
	for (index = 0; index < node_count; index++)
	{
		real_point3d to;

		matrices[index].position.x += shift.i;
		matrices[index].position.y += shift.j;
		to.x = pivot.x - head_forward.i * back * units;
		to.y = pivot.y - head_forward.j * back * units;
		to.z = pivot.z - vr_body_down[vr_render_body_setting()] * units;
		vr_carry(&matrices[index], &pivot, &to, rotation);
	}
}

#endif /* HALO_VR */
