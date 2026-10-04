/*
HOST_XR.C

OpenXR for the guest (HALO_VR builds; port/linux/src/vr_*.c drives it).
The host owns the loader, instance, session, swapchains and controller
actions, all called on the game thread with its OpenGL ES context current
(the binding is that context's EGL display, config and context). The
guest sees plain structures (halo_android_abi.h) and GL texture names,
which it can draw into directly: its GL calls reach the same driver.

Poses go to the guest relative to a recentred LOCAL frame: the head's yaw
and position when the session gains focus, the guest asks (host_xr_recenter)
or the runtime recentres its own LOCAL space become the origin, facing -z.
The origin's height (the standing height) is taken at focus and when asked
for with HALO_XR_RECENTRE_HEIGHT, and kept by a plain recentre.
*/

#ifdef HALO_VR

#include "host.h"

#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <jni.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <SDL3/SDL.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define FRAME_CONTROLLER_EXTENSION "XR_VALVE_frame_controller_interaction"
#define FRAME_PROFILE "/interaction_profiles/valve/frame_controller_valve"
#define REFRESH_RATE_EXTENSION "XR_FB_display_refresh_rate"
#define VISIBILITY_MASK_EXTENSION "XR_KHR_visibility_mask"

struct swapchain
{
	XrSwapchain handle;
	uint32_t width, height, count;
	uint32_t images[HALO_XR_MAXIMUM_IMAGES];
	int acquired;
	/* a part of the atlas (the eyes and the quad): at (x, y) in its
	images, acquired and released with it */
	int in_atlas;
	uint32_t x, y;
};

enum
{
	_action_move,
	_action_look,
	_action_trigger_left,
	_action_trigger_right,
	_action_squeeze_left,
	_action_squeeze_right,
	_action_dpad_up,
	_action_dpad_down,
	_action_dpad_left,
	_action_dpad_right,
	_action_grip_pose,
	_action_aim_pose,
	_action_haptic,
	/* each hand's buttons as they are (hand_buttons) */
	_action_hand_south,
	_action_hand_east,
	_action_hand_west,
	_action_hand_north,
	_action_hand_bumper,
	_action_hand_stick,
	_action_hand_menu,
	_action_hand_view,
	NUMBER_OF_ACTIONS
};

static struct
{
	int initialized, failed;
	XrInstance instance;
	XrSystemId system;
	XrSession session;
	XrSessionState state;
	int running, focused, exiting, frame_controller, refresh_rate_extension;
	/* XR_KHR_visibility_mask: each eye's hidden area (what the lenses never
	show) as triangles, in the view's tangent space (x, y at z = -1) */
	int visibility_mask_extension;
	float *mask_vertices[2];
	uint32_t *mask_indices[2];
	uint32_t mask_vertex_count[2], mask_index_count[2];
	XrSpace local, view;
	XrSpace grip[2], aim[2];
	XrActionSet action_set;
	XrAction actions[NUMBER_OF_ACTIONS];
	XrPath hands[2];
	int actions_ready;
	/* each hand's controller has a bumper (the Frame's); Touch and Index do
	not, and a grip with a stick click stands for it (sync_input) */
	int has_bumper[2];
	struct swapchain swapchains[HALO_XR_SWAPCHAIN_COUNT];
	/* The eyes and the quad (the HUD, the flat screen) side by side in one
	swapchain: SteamVR's GL binding makes its own context current for each
	acquire, wait and release (and the game's again after), a flush and
	fences in Zink each time, so one image a frame instead of three saves
	six of those. Its image acquired this frame (atlas_index), if it is. */
	struct swapchain atlas;
	int atlas_index;
	int64_t color_format;
	XrFrameState frame_state;
	int frame_begun;
	XrView views[2];
	int views_valid;
	/* recentring: LOCAL yaw (about +y) and position of the origin. The
	origin's height is the standing height: it is taken again only when
	recentre_height says so (or has never been taken); a recentre not before
	recentre_not_before (the runtime's own, which moves the space at a time
	given) waits for that time */
	int recentre_pending, recentred_this_frame, recentre_height, recentre_height_taken;
	XrTime recentre_not_before;
	float recentre_yaw, recentre_position[3];
	JavaVM *vm;
	jobject activity;
	/* statistics */
	long long frames, frames_rendered;
	/* the refresh rate asked for, and windows in a row it was not held */
	float refresh_rate;
	int refresh_misses;
	struct timespec stats_start;
} xr;

static const char *result_name(XrResult result)
{
	static char text[XR_MAX_RESULT_STRING_SIZE];

	text[0] = 0;
	if (xr.instance == XR_NULL_HANDLE || XR_FAILED(xrResultToString(xr.instance, result, text)))
		snprintf(text, sizeof(text), "%d", (int)result);
	return text;
}

static int check(XrResult result, const char *what)
{
	if (XR_SUCCEEDED(result))
		return 1;
	host_logf(HOST_LOG_ERROR, "[openxr] %s failed: %s", what, result_name(result));
	return 0;
}

static const char *state_name(XrSessionState state)
{
	switch (state)
	{
	case XR_SESSION_STATE_IDLE: return "IDLE";
	case XR_SESSION_STATE_READY: return "READY";
	case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
	case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
	case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
	case XR_SESSION_STATE_STOPPING: return "STOPPING";
	case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
	case XR_SESSION_STATE_EXITING: return "EXITING";
	default: return "UNKNOWN";
	}
}

/* ---------- math (LOCAL space: +y up, -z forward) */

static void quaternion_multiply(const float a[4], const float b[4], float out[4])
{
	float r[4];

	r[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
	r[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
	r[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
	r[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
	memcpy(out, r, sizeof(r));
}

static void rotate_y(float yaw, const float in[3], float out[3])
{
	float c = cosf(yaw), s = sinf(yaw);
	float x = in[0], z = in[2];

	out[0] = c * x + s * z;
	out[1] = in[1];
	out[2] = -s * x + c * z;
}

/* a LOCAL pose, relative to the recentred origin */
static void recentred_pose(const XrPosef *pose, struct halo_xr_pose *out)
{
	float inverse[4] = { 0.0f, sinf(-xr.recentre_yaw * 0.5f), 0.0f, cosf(-xr.recentre_yaw * 0.5f) };
	float orientation[4] = { pose->orientation.x, pose->orientation.y, pose->orientation.z, pose->orientation.w };
	float position[3] = {
		pose->position.x - xr.recentre_position[0],
		pose->position.y - xr.recentre_position[1],
		pose->position.z - xr.recentre_position[2],
	};

	rotate_y(-xr.recentre_yaw, position, out->position);
	quaternion_multiply(inverse, orientation, out->orientation);
}

/* the inverse: a recentred pose back in LOCAL space */
static void local_pose(const struct halo_xr_pose *pose, XrPosef *out)
{
	float yaw[4] = { 0.0f, sinf(xr.recentre_yaw * 0.5f), 0.0f, cosf(xr.recentre_yaw * 0.5f) };
	float orientation[4];
	float position[3];

	rotate_y(xr.recentre_yaw, pose->position, position);
	quaternion_multiply(yaw, pose->orientation, orientation);
	out->position.x = position[0] + xr.recentre_position[0];
	out->position.y = position[1] + xr.recentre_position[1];
	out->position.z = position[2] + xr.recentre_position[2];
	out->orientation.x = orientation[0];
	out->orientation.y = orientation[1];
	out->orientation.z = orientation[2];
	out->orientation.w = orientation[3];
}

static void update_recentre(XrTime time)
{
	XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
	const XrQuaternionf *q;
	float forward_x, forward_z;

	xr.recentred_this_frame = 0;
	if (!xr.recentre_pending)
		return;
	if (xr.recentre_not_before && time < xr.recentre_not_before)
		return; /* the runtime's own recentre has not happened yet */
	if (XR_FAILED(xrLocateSpace(xr.view, xr.local, time, &location)) ||
		!(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) ||
		!(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
	{
		return; /* not tracked yet (the headset in standby): next frame */
	}
	q = &location.pose.orientation;
	/* the view's -z axis */
	forward_x = -2.0f * (q->x * q->z + q->w * q->y);
	forward_z = -(1.0f - 2.0f * (q->x * q->x + q->y * q->y));
	if (forward_x * forward_x + forward_z * forward_z < 1e-4f)
		return; /* looking straight up or down: no heading */
	/* a turn by yaw about +y takes (0, 0, -1) to (-sin yaw, 0, -cos yaw) */
	xr.recentre_yaw = atan2f(-forward_x, -forward_z);
	xr.recentre_position[0] = location.pose.position.x;
	xr.recentre_position[2] = location.pose.position.z;
	if (xr.recentre_height || !xr.recentre_height_taken)
	{
		xr.recentre_position[1] = location.pose.position.y;
		xr.recentre_height_taken = 1;
	}
	xr.recentre_pending = 0;
	xr.recentred_this_frame = 1;
	host_logf(HOST_LOG_INFO, "[openxr] recentred%s: yaw %.1f, head (%.2f, %.2f, %.2f)",
		xr.recentre_height ? " with standing height" : "", xr.recentre_yaw * 57.29578f,
		xr.recentre_position[0], xr.recentre_position[1], xr.recentre_position[2]);
	xr.recentre_height = 0;
	xr.recentre_not_before = 0;
}

/* ---------- the visibility mask */

/* each eye's hidden triangle mesh, once the session is there; none (zero
triangles) if the runtime has no mask */
static void fetch_visibility_masks(void)
{
	PFN_xrGetVisibilityMaskKHR get = NULL;
	int eye;

	if (!xr.visibility_mask_extension ||
		XR_FAILED(xrGetInstanceProcAddr(xr.instance, "xrGetVisibilityMaskKHR", (PFN_xrVoidFunction *)&get)) || !get)
	{
		return;
	}
	for (eye = 0; eye < 2; eye++)
	{
		XrVisibilityMaskKHR mask = { XR_TYPE_VISIBILITY_MASK_KHR };

		if (XR_FAILED(get(xr.session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, (uint32_t)eye,
			XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, &mask)) ||
			mask.vertexCountOutput == 0 || mask.indexCountOutput == 0)
		{
			continue;
		}
		xr.mask_vertices[eye] = malloc(mask.vertexCountOutput * sizeof(XrVector2f));
		xr.mask_indices[eye] = malloc(mask.indexCountOutput * sizeof(uint32_t));
		if (!xr.mask_vertices[eye] || !xr.mask_indices[eye])
			continue;
		mask.vertexCapacityInput = mask.vertexCountOutput;
		mask.vertices = (XrVector2f *)xr.mask_vertices[eye];
		mask.indexCapacityInput = mask.indexCountOutput;
		mask.indices = xr.mask_indices[eye];
		if (XR_FAILED(get(xr.session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, (uint32_t)eye,
			XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, &mask)))
		{
			continue;
		}
		xr.mask_vertex_count[eye] = mask.vertexCountOutput;
		xr.mask_index_count[eye] = mask.indexCountOutput;
		host_logf(HOST_LOG_INFO, "[openxr] visibility mask, %s eye: %u triangles", eye ? "right" : "left",
			mask.indexCountOutput / 3);
	}
}

uint32_t host_xr_visibility_mask(uint32_t eye, float *vertices, uint32_t vertex_capacity, uint32_t *indices,
	uint32_t index_capacity)
{
	if (eye > 1 || xr.mask_vertex_count[eye] > vertex_capacity || xr.mask_index_count[eye] > index_capacity)
		return 0;
	memcpy(vertices, xr.mask_vertices[eye], xr.mask_vertex_count[eye] * 2 * sizeof(float));
	memcpy(indices, xr.mask_indices[eye], xr.mask_index_count[eye] * sizeof(uint32_t));
	return xr.mask_index_count[eye];
}

/* ---------- controller actions */

static XrPath path(const char *text)
{
	XrPath result = XR_NULL_PATH;

	xrStringToPath(xr.instance, text, &result);
	return result;
}

static int create_action(int index, XrActionType type, const char *name, const char *label, int per_hand)
{
	XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };

	info.actionType = type;
	strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
	strncpy(info.localizedActionName, label, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
	if (per_hand)
	{
		info.countSubactionPaths = 2;
		info.subactionPaths = xr.hands;
	}
	return check(xrCreateAction(xr.action_set, &info, &xr.actions[index]), name);
}

struct binding
{
	int action;
	const char *path;
};

static void suggest(const char *profile, const struct binding *bindings, int count)
{
	XrActionSuggestedBinding suggested[NUMBER_OF_ACTIONS * 2];
	XrInteractionProfileSuggestedBinding info = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	XrResult result;
	int index;

	for (index = 0; index < count; index++)
	{
		suggested[index].action = xr.actions[bindings[index].action];
		suggested[index].binding = path(bindings[index].path);
	}
	info.interactionProfile = path(profile);
	info.countSuggestedBindings = (uint32_t)count;
	info.suggestedBindings = suggested;
	result = xrSuggestInteractionProfileBindings(xr.instance, &info);
	host_logf(HOST_LOG_INFO, "[openxr] bindings %s: %s", profile,
		XR_SUCCEEDED(result) ? "suggested" : result_name(result));
}

#define POSES(hand) \
	{ _action_grip_pose, "/user/hand/" hand "/input/grip/pose" }, \
	{ _action_aim_pose, "/user/hand/" hand "/input/aim/pose" }, \
	{ _action_haptic, "/user/hand/" hand "/output/haptic" }

static int create_actions(void)
{
	/* Steam Frame controllers: a split gamepad. Right: A/B/X/Y, menu,
	bumper, trigger, grip; left: d-pad, view, bumper, trigger, grip
	(Steamworks "Steam Frame Input"). The Xbox pad maps across as it is. */
	static const struct binding frame[] =
	{
		{ _action_move, "/user/hand/left/input/thumbstick" },
		{ _action_look, "/user/hand/right/input/thumbstick" },
		{ _action_trigger_left, "/user/hand/left/input/trigger/value" },
		{ _action_trigger_right, "/user/hand/right/input/trigger/value" },
		{ _action_squeeze_left, "/user/hand/left/input/squeeze/value" },
		{ _action_squeeze_right, "/user/hand/right/input/squeeze/value" },
		{ _action_dpad_up, "/user/hand/left/input/dpad_up/click" },
		{ _action_dpad_down, "/user/hand/left/input/dpad_down/click" },
		{ _action_dpad_left, "/user/hand/left/input/dpad_left/click" },
		{ _action_dpad_right, "/user/hand/left/input/dpad_right/click" },
		POSES("left"), POSES("right"),
		{ _action_hand_south, "/user/hand/right/input/a/click" },
		{ _action_hand_east, "/user/hand/right/input/b/click" },
		{ _action_hand_west, "/user/hand/right/input/x/click" },
		{ _action_hand_north, "/user/hand/right/input/y/click" },
		{ _action_hand_bumper, "/user/hand/left/input/bumper/click" },
		{ _action_hand_bumper, "/user/hand/right/input/bumper/click" },
		{ _action_hand_stick, "/user/hand/left/input/thumbstick/click" },
		{ _action_hand_stick, "/user/hand/right/input/thumbstick/click" },
		{ _action_hand_menu, "/user/hand/right/input/menu/click" },
		{ _action_hand_view, "/user/hand/left/input/view/click" },
	};
	/* Touch: X/Y on the left, A/B on the right. It and Index have no
	bumpers and no View button: the left's upper face button is View, and
	the bumpers are a grip and a stick click together (sync_input) */
	static const struct binding touch[] =
	{
		{ _action_move, "/user/hand/left/input/thumbstick" },
		{ _action_look, "/user/hand/right/input/thumbstick" },
		{ _action_trigger_left, "/user/hand/left/input/trigger/value" },
		{ _action_trigger_right, "/user/hand/right/input/trigger/value" },
		{ _action_squeeze_left, "/user/hand/left/input/squeeze/value" },
		{ _action_squeeze_right, "/user/hand/right/input/squeeze/value" },
		POSES("left"), POSES("right"),
		{ _action_hand_south, "/user/hand/right/input/a/click" },
		{ _action_hand_south, "/user/hand/left/input/x/click" },
		{ _action_hand_east, "/user/hand/right/input/b/click" },
		{ _action_hand_east, "/user/hand/left/input/y/click" },
		{ _action_hand_stick, "/user/hand/left/input/thumbstick/click" },
		{ _action_hand_stick, "/user/hand/right/input/thumbstick/click" },
		{ _action_hand_menu, "/user/hand/left/input/menu/click" },
	};
	static const struct binding index_controller[] =
	{
		{ _action_move, "/user/hand/left/input/thumbstick" },
		{ _action_look, "/user/hand/right/input/thumbstick" },
		{ _action_trigger_left, "/user/hand/left/input/trigger/value" },
		{ _action_trigger_right, "/user/hand/right/input/trigger/value" },
		{ _action_squeeze_left, "/user/hand/left/input/squeeze/value" },
		{ _action_squeeze_right, "/user/hand/right/input/squeeze/value" },
		/* (Menu is the trackpads' press: the system button is the runtime's, and
		Index has no other free input. A float bound to a boolean action reads as
		pressed past the runtime's threshold, a firm press) */
		POSES("left"), POSES("right"),
		{ _action_hand_south, "/user/hand/right/input/a/click" },
		{ _action_hand_south, "/user/hand/left/input/a/click" },
		{ _action_hand_east, "/user/hand/right/input/b/click" },
		{ _action_hand_east, "/user/hand/left/input/b/click" },
		{ _action_hand_stick, "/user/hand/left/input/thumbstick/click" },
		{ _action_hand_stick, "/user/hand/right/input/thumbstick/click" },
		{ _action_hand_menu, "/user/hand/left/input/trackpad/force" },
		{ _action_hand_menu, "/user/hand/right/input/trackpad/force" },
	};
	XrActionSetCreateInfo set_info = { XR_TYPE_ACTION_SET_CREATE_INFO };
	XrSessionActionSetsAttachInfo attach = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	int hand, ok = 1;

	xr.hands[0] = path("/user/hand/left");
	xr.hands[1] = path("/user/hand/right");
	strncpy(set_info.actionSetName, "gameplay", XR_MAX_ACTION_SET_NAME_SIZE - 1);
	strncpy(set_info.localizedActionSetName, "Gameplay", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
	if (!check(xrCreateActionSet(xr.instance, &set_info, &xr.action_set), "xrCreateActionSet"))
		return 0;
	ok &= create_action(_action_move, XR_ACTION_TYPE_VECTOR2F_INPUT, "move", "Move", 0);
	ok &= create_action(_action_look, XR_ACTION_TYPE_VECTOR2F_INPUT, "turn", "Turn", 0);
	ok &= create_action(_action_trigger_left, XR_ACTION_TYPE_FLOAT_INPUT, "grenade", "Left trigger (zoom; Xbox layout: throw grenade)", 0);
	ok &= create_action(_action_trigger_right, XR_ACTION_TYPE_FLOAT_INPUT, "fire", "Right trigger (fire)", 0);
	ok &= create_action(_action_squeeze_left, XR_ACTION_TYPE_FLOAT_INPUT, "grip_left", "Left grip", 0);
	ok &= create_action(_action_squeeze_right, XR_ACTION_TYPE_FLOAT_INPUT, "grip_right", "Right grip", 0);
	ok &= create_action(_action_dpad_up, XR_ACTION_TYPE_BOOLEAN_INPUT, "dpad_up", "D-pad up", 0);
	ok &= create_action(_action_dpad_down, XR_ACTION_TYPE_BOOLEAN_INPUT, "dpad_down", "D-pad down", 0);
	ok &= create_action(_action_dpad_left, XR_ACTION_TYPE_BOOLEAN_INPUT, "dpad_left", "D-pad left", 0);
	ok &= create_action(_action_dpad_right, XR_ACTION_TYPE_BOOLEAN_INPUT, "dpad_right", "D-pad right", 0);
	ok &= create_action(_action_grip_pose, XR_ACTION_TYPE_POSE_INPUT, "grip_pose", "Hand", 1);
	ok &= create_action(_action_aim_pose, XR_ACTION_TYPE_POSE_INPUT, "aim_pose", "Aim", 1);
	ok &= create_action(_action_haptic, XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", "Vibration", 1);
	ok &= create_action(_action_hand_south, XR_ACTION_TYPE_BOOLEAN_INPUT, "hand_south", "Lower face button (right: jump; left on Touch and Index: switch grenades)", 1);
	ok &= create_action(_action_hand_east, XR_ACTION_TYPE_BOOLEAN_INPUT, "hand_east", "Upper face button (right: action, reload; left on Touch and Index: View)", 1);
	ok &= create_action(_action_hand_west, XR_ACTION_TYPE_BOOLEAN_INPUT, "hand_west", "Left face button (switch grenades)", 1);
	ok &= create_action(_action_hand_north, XR_ACTION_TYPE_BOOLEAN_INPUT, "hand_north", "Top face button (switch weapons)", 1);
	ok &= create_action(_action_hand_bumper, XR_ACTION_TYPE_BOOLEAN_INPUT, "hand_bumper", "Bumper (weapon hand: throw grenade; other hand: flashlight; Touch and Index: grip and stick click together)", 1);
	ok &= create_action(_action_hand_stick, XR_ACTION_TYPE_BOOLEAN_INPUT, "hand_stick", "Stick click (right: melee; left: crouch)", 1);
	ok &= create_action(_action_hand_menu, XR_ACTION_TYPE_BOOLEAN_INPUT, "hand_menu", "Menu (pause; Index: a firm press of a trackpad)", 1);
	ok &= create_action(_action_hand_view, XR_ACTION_TYPE_BOOLEAN_INPUT, "hand_view", "View (back; held 1 s recentres, 3 s also the standing height)", 1);
	if (!ok)
		return 0;
	if (xr.frame_controller)
		suggest(FRAME_PROFILE, frame, sizeof(frame) / sizeof(frame[0]));
	suggest("/interaction_profiles/oculus/touch_controller", touch, sizeof(touch) / sizeof(touch[0]));
	suggest("/interaction_profiles/valve/index_controller", index_controller,
		sizeof(index_controller) / sizeof(index_controller[0]));
	attach.countActionSets = 1;
	attach.actionSets = &xr.action_set;
	if (!check(xrAttachSessionActionSets(xr.session, &attach), "xrAttachSessionActionSets"))
		return 0;
	for (hand = 0; hand < 2; hand++)
	{
		XrActionSpaceCreateInfo space = { XR_TYPE_ACTION_SPACE_CREATE_INFO };

		space.subactionPath = xr.hands[hand];
		space.poseInActionSpace.orientation.w = 1.0f;
		space.action = xr.actions[_action_grip_pose];
		check(xrCreateActionSpace(xr.session, &space, &xr.grip[hand]), "xrCreateActionSpace(grip)");
		space.action = xr.actions[_action_aim_pose];
		check(xrCreateActionSpace(xr.session, &space, &xr.aim[hand]), "xrCreateActionSpace(aim)");
	}
	return 1;
}

static int action_boolean(int action)
{
	XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
	XrActionStateBoolean state = { XR_TYPE_ACTION_STATE_BOOLEAN };

	info.action = xr.actions[action];
	return XR_SUCCEEDED(xrGetActionStateBoolean(xr.session, &info, &state)) && state.isActive && state.currentState;
}

/* a per-hand action's state for one hand (0 left, 1 right) */
static int action_boolean_hand(int action, int hand)
{
	XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
	XrActionStateBoolean state = { XR_TYPE_ACTION_STATE_BOOLEAN };

	info.action = xr.actions[action];
	info.subactionPath = xr.hands[hand];
	return XR_SUCCEEDED(xrGetActionStateBoolean(xr.session, &info, &state)) && state.isActive && state.currentState;
}

static float action_float(int action)
{
	XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
	XrActionStateFloat state = { XR_TYPE_ACTION_STATE_FLOAT };

	info.action = xr.actions[action];
	if (XR_SUCCEEDED(xrGetActionStateFloat(xr.session, &info, &state)) && state.isActive)
		return state.currentState;
	return 0.0f;
}

static void action_vector(int action, float *x, float *y)
{
	XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
	XrActionStateVector2f state = { XR_TYPE_ACTION_STATE_VECTOR2F };

	info.action = xr.actions[action];
	*x = *y = 0.0f;
	if (XR_SUCCEEDED(xrGetActionStateVector2f(xr.session, &info, &state)) && state.isActive)
	{
		*x = state.currentState.x;
		*y = state.currentState.y;
	}
}

static void sync_input(struct halo_xr_frame *frame)
{
	static const struct { int action; uint32_t bit; } buttons[] =
	{
		{ _action_dpad_up, HALO_XR_BUTTON_DPAD_UP },
		{ _action_dpad_down, HALO_XR_BUTTON_DPAD_DOWN },
		{ _action_dpad_left, HALO_XR_BUTTON_DPAD_LEFT },
		{ _action_dpad_right, HALO_XR_BUTTON_DPAD_RIGHT },
	};
	XrActiveActionSet active = { xr.action_set, XR_NULL_PATH };
	XrActionsSyncInfo sync = { XR_TYPE_ACTIONS_SYNC_INFO };
	unsigned int index;
	int hand;

	if (!xr.actions_ready || !xr.focused)
		return;
	sync.countActiveActionSets = 1;
	sync.activeActionSets = &active;
	if (XR_FAILED(xrSyncActions(xr.session, &sync)))
		return;
	for (index = 0; index < sizeof(buttons) / sizeof(buttons[0]); index++)
	{
		if (action_boolean(buttons[index].action))
			frame->buttons |= buttons[index].bit;
	}
	frame->trigger[0] = action_float(_action_trigger_left);
	frame->trigger[1] = action_float(_action_trigger_right);
	frame->squeeze[0] = action_float(_action_squeeze_left);
	frame->squeeze[1] = action_float(_action_squeeze_right);
	action_vector(_action_move, &frame->thumb[0], &frame->thumb[1]);
	action_vector(_action_look, &frame->thumb[2], &frame->thumb[3]);
	{
		static const struct { int action; uint32_t bit; } hand_buttons[] =
		{
			{ _action_hand_south, HALO_XR_HAND_SOUTH },
			{ _action_hand_east, HALO_XR_HAND_EAST },
			{ _action_hand_west, HALO_XR_HAND_WEST },
			{ _action_hand_north, HALO_XR_HAND_NORTH },
			{ _action_hand_bumper, HALO_XR_HAND_BUMPER },
			{ _action_hand_stick, HALO_XR_HAND_STICK },
			{ _action_hand_menu, HALO_XR_HAND_MENU },
			{ _action_hand_view, HALO_XR_HAND_VIEW },
		};
		int side;

		/* what no profile binds on a hand is not asked for (each query is a
		call into SteamVR): the left's west and north face buttons and the
		right's View anywhere, and on the Frame the left's face buttons and
		Menu */
		uint32_t unbound[2];

		unbound[0] = HALO_XR_HAND_WEST | HALO_XR_HAND_NORTH |
			(xr.has_bumper[0] ? HALO_XR_HAND_SOUTH | HALO_XR_HAND_EAST | HALO_XR_HAND_MENU : 0);
		unbound[1] = HALO_XR_HAND_VIEW;
		for (side = 0; side < 2; side++)
		{
			for (index = 0; index < sizeof(hand_buttons) / sizeof(hand_buttons[0]); index++)
			{
				if (!(unbound[side] & hand_buttons[index].bit) && action_boolean_hand(hand_buttons[index].action, side))
					frame->hand_buttons[side] |= hand_buttons[index].bit;
			}
		}
		/* Touch and Index have no bumpers, so grenade and flashlight would be
		out of reach: a grip held with a stick click stands for one (the stick
		click alone still melees and crouches) */
		for (side = 0; side < 2; side++)
		{
			if (!xr.has_bumper[side] && (frame->hand_buttons[side] & HALO_XR_HAND_STICK) &&
				frame->squeeze[side] > 0.75f)
			{
				frame->hand_buttons[side] &= ~(uint32_t)HALO_XR_HAND_STICK;
				frame->hand_buttons[side] |= HALO_XR_HAND_BUMPER;
			}
		}
		/* the Frame's d-pad is on its left controller */
		if (frame->buttons & HALO_XR_BUTTON_DPAD_UP) frame->hand_buttons[0] |= HALO_XR_HAND_DPAD_UP;
		if (frame->buttons & HALO_XR_BUTTON_DPAD_DOWN) frame->hand_buttons[0] |= HALO_XR_HAND_DPAD_DOWN;
		if (frame->buttons & HALO_XR_BUTTON_DPAD_LEFT) frame->hand_buttons[0] |= HALO_XR_HAND_DPAD_LEFT;
		if (frame->buttons & HALO_XR_BUTTON_DPAD_RIGHT) frame->hand_buttons[0] |= HALO_XR_HAND_DPAD_RIGHT;
	}
	/* The Xbox pad (vr.controls "pad") from each hand's buttons, rather than
	actions of its own: every action bound costs xrSyncActions a lookup a
	frame. A and B are the right's lower and upper face buttons; X and Y the
	Frame's right west and north, on Touch and Index the left's lower and
	upper; white and black the bumpers (or their stand-ins); start Menu,
	back the Frame's View; the stick clicks each hand's. */
	{
		uint32_t left = frame->hand_buttons[0], right = frame->hand_buttons[1];

		if (right & HALO_XR_HAND_SOUTH) frame->buttons |= HALO_XR_BUTTON_A;
		if (right & HALO_XR_HAND_EAST) frame->buttons |= HALO_XR_BUTTON_B;
		if (xr.has_bumper[1] ? right & HALO_XR_HAND_WEST : left & HALO_XR_HAND_SOUTH) frame->buttons |= HALO_XR_BUTTON_X;
		if (xr.has_bumper[1] ? right & HALO_XR_HAND_NORTH : left & HALO_XR_HAND_EAST) frame->buttons |= HALO_XR_BUTTON_Y;
		if (left & HALO_XR_HAND_BUMPER) frame->buttons |= HALO_XR_BUTTON_WHITE;
		if (right & HALO_XR_HAND_BUMPER) frame->buttons |= HALO_XR_BUTTON_BLACK;
		if ((left | right) & HALO_XR_HAND_MENU) frame->buttons |= HALO_XR_BUTTON_START;
		if (left & HALO_XR_HAND_VIEW) frame->buttons |= HALO_XR_BUTTON_BACK;
		if (left & HALO_XR_HAND_STICK) frame->buttons |= HALO_XR_BUTTON_LEFT_THUMB;
		if (right & HALO_XR_HAND_STICK) frame->buttons |= HALO_XR_BUTTON_RIGHT_THUMB;
	}
	for (hand = 0; hand < 2; hand++)
	{
		XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
		const XrSpaceLocationFlags valid = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;

		if (XR_SUCCEEDED(xrLocateSpace(xr.grip[hand], xr.local, xr.frame_state.predictedDisplayTime, &location)) &&
			(location.locationFlags & valid) == valid)
		{
			recentred_pose(&location.pose, &frame->grip[hand]);
			frame->hand_valid[hand] |= 1;
		}
		location.locationFlags = 0;
		if (XR_SUCCEEDED(xrLocateSpace(xr.aim[hand], xr.local, xr.frame_state.predictedDisplayTime, &location)) &&
			(location.locationFlags & valid) == valid)
		{
			recentred_pose(&location.pose, &frame->aim[hand]);
			frame->hand_valid[hand] |= 2;
		}
	}
}

/* ---------- session */

static int create_swapchain(int which, uint32_t width, uint32_t height)
{
	struct swapchain *swapchain = &xr.swapchains[which];
	XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	XrSwapchainImageOpenGLESKHR images[HALO_XR_MAXIMUM_IMAGES];
	uint32_t count = 0, index;

	info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
		XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	info.format = xr.color_format;
	info.sampleCount = 1;
	info.width = width;
	info.height = height;
	info.faceCount = 1;
	info.arraySize = 1;
	info.mipCount = 1;
	if (!check(xrCreateSwapchain(xr.session, &info, &swapchain->handle), "xrCreateSwapchain"))
		return 0;
	xrEnumerateSwapchainImages(swapchain->handle, 0, &count, NULL);
	if (count == 0 || count > HALO_XR_MAXIMUM_IMAGES)
	{
		host_logf(HOST_LOG_ERROR, "[openxr] swapchain has %u images (at most %d handled)", count, HALO_XR_MAXIMUM_IMAGES);
		return 0;
	}
	for (index = 0; index < count; index++)
	{
		images[index].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
		images[index].next = NULL;
	}
	if (!check(xrEnumerateSwapchainImages(swapchain->handle, count, &count,
		(XrSwapchainImageBaseHeader *)images), "xrEnumerateSwapchainImages"))
	{
		return 0;
	}
	for (index = 0; index < count; index++)
		swapchain->images[index] = images[index].image;
	swapchain->count = count;
	swapchain->width = width;
	swapchain->height = height;
	return 1;
}

static int initialize_loader(void)
{
	PFN_xrInitializeLoaderKHR initialize = NULL;
	XrLoaderInitInfoAndroidKHR info = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
	JNIEnv *environment = (JNIEnv *)SDL_GetAndroidJNIEnv();
	jobject activity = environment ? (jobject)SDL_GetAndroidActivity() : NULL;

	if (!environment || !activity || (*environment)->GetJavaVM(environment, &xr.vm) != 0)
	{
		host_logf(HOST_LOG_ERROR, "[openxr] no Java VM or activity: %s", SDL_GetError());
		return 0;
	}
	xr.activity = (*environment)->NewGlobalRef(environment, activity);
	(*environment)->DeleteLocalRef(environment, activity);
	if (!check(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&initialize),
		"xrGetInstanceProcAddr(xrInitializeLoaderKHR)"))
	{
		return 0;
	}
	info.applicationVM = xr.vm;
	info.applicationContext = xr.activity;
	return check(initialize((const XrLoaderInitInfoBaseHeaderKHR *)&info), "xrInitializeLoaderKHR");
}

static int create_instance(struct halo_xr_info *out)
{
	XrInstanceCreateInfoAndroidKHR android = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
	XrInstanceCreateInfo info = { XR_TYPE_INSTANCE_CREATE_INFO };
	XrInstanceProperties properties = { XR_TYPE_INSTANCE_PROPERTIES };
	XrSystemGetInfo system_info = { XR_TYPE_SYSTEM_GET_INFO };
	XrSystemProperties system = { XR_TYPE_SYSTEM_PROPERTIES };
	XrExtensionProperties available[128];
	const char *extensions[5];
	uint32_t count = 0, index;

	for (index = 0; index < sizeof(available) / sizeof(available[0]); index++)
	{
		available[index].type = XR_TYPE_EXTENSION_PROPERTIES;
		available[index].next = NULL;
	}
	xrEnumerateInstanceExtensionProperties(NULL, sizeof(available) / sizeof(available[0]), &count, available);
	for (index = 0; index < count; index++)
	{
		host_logf(HOST_LOG_INFO, "[openxr] extension %s", available[index].extensionName);
		if (!strcmp(available[index].extensionName, FRAME_CONTROLLER_EXTENSION))
			xr.frame_controller = 1;
		if (!strcmp(available[index].extensionName, REFRESH_RATE_EXTENSION))
			xr.refresh_rate_extension = 1;
		if (!strcmp(available[index].extensionName, VISIBILITY_MASK_EXTENSION))
			xr.visibility_mask_extension = 1;
	}
	count = 0;
	extensions[count++] = XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME;
	extensions[count++] = XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME;
	if (xr.frame_controller)
		extensions[count++] = FRAME_CONTROLLER_EXTENSION;
	if (xr.refresh_rate_extension)
		extensions[count++] = REFRESH_RATE_EXTENSION;
	if (xr.visibility_mask_extension)
		extensions[count++] = VISIBILITY_MASK_EXTENSION;
	android.applicationVM = xr.vm;
	android.applicationActivity = xr.activity;
	info.next = &android;
	strncpy(info.applicationInfo.applicationName, "Halo CE VR", XR_MAX_APPLICATION_NAME_SIZE - 1);
	info.applicationInfo.applicationVersion = 1;
	strncpy(info.applicationInfo.engineName, "halo-ce-universal", XR_MAX_ENGINE_NAME_SIZE - 1);
	info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	info.enabledExtensionCount = count;
	info.enabledExtensionNames = extensions;
	if (!check(xrCreateInstance(&info, &xr.instance), "xrCreateInstance"))
		return 0;
	xrGetInstanceProperties(xr.instance, &properties);
	system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	if (!check(xrGetSystem(xr.instance, &system_info, &xr.system), "xrGetSystem"))
		return 0;
	xrGetSystemProperties(xr.instance, xr.system, &system);
	snprintf(out->runtime, sizeof(out->runtime), "%s %u.%u.%u", properties.runtimeName,
		XR_VERSION_MAJOR(properties.runtimeVersion), XR_VERSION_MINOR(properties.runtimeVersion),
		XR_VERSION_PATCH(properties.runtimeVersion));
	snprintf(out->system, sizeof(out->system), "%s", system.systemName);
	return 1;
}

static int create_session(void)
{
	PFN_xrGetOpenGLESGraphicsRequirementsKHR requirements_function = NULL;
	XrGraphicsRequirementsOpenGLESKHR requirements = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
	XrGraphicsBindingOpenGLESAndroidKHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
	XrSessionCreateInfo info = { XR_TYPE_SESSION_CREATE_INFO };
	XrReferenceSpaceCreateInfo space = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	EGLDisplay display = eglGetCurrentDisplay();
	EGLContext context = eglGetCurrentContext();
	EGLConfig config = NULL;
	EGLint config_id = 0, configs = 0;

	if (!check(xrGetInstanceProcAddr(xr.instance, "xrGetOpenGLESGraphicsRequirementsKHR",
		(PFN_xrVoidFunction *)&requirements_function), "xrGetInstanceProcAddr(xrGetOpenGLESGraphicsRequirementsKHR)") ||
		!check(requirements_function(xr.instance, xr.system, &requirements), "xrGetOpenGLESGraphicsRequirementsKHR"))
	{
		return 0;
	}
	host_logf(HOST_LOG_INFO, "[openxr] GLES required %u.%u .. %u.%u, context %s",
		XR_VERSION_MAJOR(requirements.minApiVersionSupported), XR_VERSION_MINOR(requirements.minApiVersionSupported),
		XR_VERSION_MAJOR(requirements.maxApiVersionSupported), XR_VERSION_MINOR(requirements.maxApiVersionSupported),
		(const char *)glGetString(GL_VERSION));
	if (display == EGL_NO_DISPLAY || context == EGL_NO_CONTEXT)
	{
		host_logf(HOST_LOG_ERROR, "[openxr] no current EGL context");
		return 0;
	}
	eglQueryContext(display, context, EGL_CONFIG_ID, &config_id);
	{
		const EGLint attributes[] = { EGL_CONFIG_ID, config_id, EGL_NONE };

		eglChooseConfig(display, attributes, &config, 1, &configs);
	}
	binding.display = display;
	binding.config = configs > 0 ? config : NULL;
	binding.context = context;
	info.next = &binding;
	info.systemId = xr.system;
	if (!check(xrCreateSession(xr.instance, &info, &xr.session), "xrCreateSession"))
		return 0;
	space.poseInReferenceSpace.orientation.w = 1.0f;
	space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	if (!check(xrCreateReferenceSpace(xr.session, &space, &xr.local), "xrCreateReferenceSpace(LOCAL)"))
		return 0;
	space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	return check(xrCreateReferenceSpace(xr.session, &space, &xr.view), "xrCreateReferenceSpace(VIEW)");
}

static int choose_format(void)
{
	/* the game's image is gamma-encoded already: an sRGB swapchain shows
	it as it is when written without conversion (vr_frame.c) */
	static const int64_t preferred[] = { GL_SRGB8_ALPHA8, GL_RGBA8 };
	int64_t formats[64];
	uint32_t count = 0, index, want;

	if (!check(xrEnumerateSwapchainFormats(xr.session, 64, &count, formats), "xrEnumerateSwapchainFormats"))
		return 0;
	for (index = 0; index < count; index++)
		host_logf(HOST_LOG_INFO, "[openxr] swapchain format 0x%llx", (long long)formats[index]);
	for (want = 0; want < sizeof(preferred) / sizeof(preferred[0]) && !xr.color_format; want++)
	{
		for (index = 0; index < count; index++)
		{
			if (formats[index] == preferred[want])
				xr.color_format = formats[index];
		}
	}
	if (!xr.color_format)
		host_logf(HOST_LOG_ERROR, "[openxr] neither SRGB8_ALPHA8 nor RGBA8 is offered");
	return xr.color_format != 0;
}

int host_xr_init(struct halo_xr_info *out, uint32_t quad_width, uint32_t quad_height, uint32_t eye_scale_percent)
{
	XrViewConfigurationView views[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
	uint32_t view_count = 0;
	int which;

	memset(out, 0, sizeof(*out));
	if (xr.failed)
		return -1;
	if (xr.initialized)
		goto describe;
	xr.failed = 1;
	if (!initialize_loader())
		return -1;
	/* SteamVR can be not yet running, or asleep with the headset (it fails
	the instance with XR_ERROR_RUNTIME_FAILURE): wait for it a minute rather
	than give up. A VR build has no flat screen in Lepton to fall back on:
	frames posted to its window are refused, and the display's composer
	then aborts and takes the container down. */
	{
		int attempt;

		for (attempt = 0; !create_instance(out); attempt++)
		{
			if (xr.instance != XR_NULL_HANDLE)
			{
				xrDestroyInstance(xr.instance);
				xr.instance = XR_NULL_HANDLE;
			}
			if (attempt >= 60)
				return -1;
			if (attempt % 10 == 0)
				host_logf(HOST_LOG_WARN, "[openxr] no runtime yet (the headset asleep?): trying again for a minute");
			sleep(1);
		}
	}
	if (!create_session() || !choose_format())
		return -1;
	if (!check(xrEnumerateViewConfigurationViews(xr.instance, xr.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
		2, &view_count, views), "xrEnumerateViewConfigurationViews") || view_count != 2)
	{
		return -1;
	}
	{
		/* the atlas: left eye, right eye, quad, from the left */
		uint32_t widths[3], heights[3], x = 0, height = 0;
		int part;

		/* each eye's image as large as the game draws it (the recommended
		size scaled, within the maximum): the compositor samples that whole,
		where an image at the recommended size would first shrink it by
		a plain blit and soften it */
		for (part = 0; part < 2; part++)
		{
			uint64_t scale = eye_scale_percent ? eye_scale_percent : 100;
			uint32_t width = (uint32_t)(views[part].recommendedImageRectWidth * scale / 100) & ~1u;
			uint32_t height = (uint32_t)(views[part].recommendedImageRectHeight * scale / 100) & ~1u;

			if (views[part].maxImageRectWidth && width > views[part].maxImageRectWidth)
				width = views[part].maxImageRectWidth & ~1u;
			if (views[part].maxImageRectHeight && height > views[part].maxImageRectHeight)
				height = views[part].maxImageRectHeight & ~1u;
			widths[part] = width;
			heights[part] = height;
		}
		widths[2] = quad_width;
		heights[2] = quad_height;
		for (part = 0; part < 3; part++)
			height = heights[part] > height ? heights[part] : height;
		if (!create_swapchain(HALO_XR_SWAPCHAIN_LEFT, widths[0] + widths[1] + widths[2], height))
			return -1;
		xr.atlas = xr.swapchains[HALO_XR_SWAPCHAIN_LEFT];
		for (part = 0; part < 3; part++)
		{
			struct swapchain *swapchain = &xr.swapchains[part];

			*swapchain = xr.atlas;
			swapchain->in_atlas = 1;
			swapchain->x = x;
			swapchain->y = 0;
			swapchain->width = widths[part];
			swapchain->height = heights[part];
			x += widths[part];
		}
	}
	if (!create_swapchain(HALO_XR_SWAPCHAIN_RETICLE, 64, 64) ||
		!create_swapchain(HALO_XR_SWAPCHAIN_FADE, 16, 16) ||
		!create_swapchain(HALO_XR_SWAPCHAIN_SCOPE, 768, 768))
	{
		return -1;
	}
	fetch_visibility_masks();
	xr.actions_ready = create_actions();
	/* (until the runtime says which controllers are held) */
	xr.has_bumper[0] = xr.has_bumper[1] = xr.frame_controller;
	if (!xr.actions_ready)
		host_logf(HOST_LOG_WARN, "[openxr] controller actions unavailable");
	xr.views[0].type = xr.views[1].type = XR_TYPE_VIEW;
	xr.recentre_pending = 1;
	xr.recentre_height = 1;
	clock_gettime(CLOCK_MONOTONIC, &xr.stats_start);
	xr.failed = 0;
	xr.initialized = 1;
	/* the runtime's GL binding brings its own contexts' trace markers */
	host_gl_quiet_trace_markers();
	host_logf(HOST_LOG_INFO, "[openxr] %s, %s: eyes %ux%u (recommended %ux%u, max %ux%u), quad %ux%u, format 0x%llx, "
		"frame controller %s",
		out->runtime, out->system, xr.swapchains[HALO_XR_SWAPCHAIN_LEFT].width,
		xr.swapchains[HALO_XR_SWAPCHAIN_LEFT].height, views[0].recommendedImageRectWidth,
		views[0].recommendedImageRectHeight, views[0].maxImageRectWidth, views[0].maxImageRectHeight, quad_width, quad_height,
		(long long)xr.color_format, xr.frame_controller ? "yes" : "no");
describe:
	out->color_format = xr.color_format;
	for (which = 0; which < HALO_XR_SWAPCHAIN_COUNT; which++)
	{
		out->width[which] = xr.swapchains[which].width;
		out->height[which] = xr.swapchains[which].height;
		out->image_count[which] = xr.swapchains[which].count;
		memcpy(out->images[which], xr.swapchains[which].images, sizeof(out->images[which]));
		out->x[which] = xr.swapchains[which].x;
		out->y[which] = xr.swapchains[which].y;
	}
	return 0;
}

static void session_state_changed(XrSessionState state)
{
	host_logf(HOST_LOG_INFO, "[openxr] session %s", state_name(state));
	if (state == XR_SESSION_STATE_FOCUSED && !xr.focused)
	{
		/* where the player faces on gaining focus is forward, at the height
		they stand */
		xr.recentre_pending = 1;
		xr.recentre_height = 1;
	}
	xr.state = state;
	xr.focused = state == XR_SESSION_STATE_FOCUSED;
	switch (state)
	{
	case XR_SESSION_STATE_READY:
	{
		XrSessionBeginInfo begin = { XR_TYPE_SESSION_BEGIN_INFO };

		begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		if (check(xrBeginSession(xr.session, &begin), "xrBeginSession"))
			xr.running = 1;
		break;
	}
	case XR_SESSION_STATE_STOPPING:
		check(xrEndSession(xr.session), "xrEndSession");
		xr.running = 0;
		break;
	case XR_SESSION_STATE_EXITING:
	case XR_SESSION_STATE_LOSS_PENDING:
		xr.running = 0;
		xr.exiting = 1;
		break;
	default:
		break;
	}
}

/* the runtime moves a reference space (the system's own recentre: the
player held the Steam button, or reset the room): the poses we hold relative
to the old LOCAL origin are stale, so recentre as the View hold does, with
the height (the origin's height may have moved too), once the space has
moved */
static void reference_space_changing(const XrEventDataReferenceSpaceChangePending *change)
{
	if (change->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_LOCAL)
		return;
	host_logf(HOST_LOG_INFO, "[openxr] the runtime recentres LOCAL (in %.0f ms): recentring with it",
		change->changeTime > xr.frame_state.predictedDisplayTime ?
		(change->changeTime - xr.frame_state.predictedDisplayTime) * 1e-6 : 0.0);
	xr.recentre_pending = 1;
	xr.recentre_height = 1;
	xr.recentre_not_before = change->changeTime;
}

/* which controller each hand holds: a Frame's has bumpers, the others'
(Touch, Index) are stood for by a grip and a stick click */
static void interaction_profile_changed(void)
{
	int hand;

	for (hand = 0; hand < 2; hand++)
	{
		XrInteractionProfileState state = { XR_TYPE_INTERACTION_PROFILE_STATE };
		char text[XR_MAX_PATH_LENGTH];
		uint32_t length = 0;

		if (xr.actions_ready && XR_SUCCEEDED(xrGetCurrentInteractionProfile(xr.session, xr.hands[hand], &state)) &&
			state.interactionProfile != XR_NULL_PATH &&
			XR_SUCCEEDED(xrPathToString(xr.instance, state.interactionProfile, sizeof(text), &length, text)))
		{
			xr.has_bumper[hand] = !strcmp(text, FRAME_PROFILE);
			host_logf(HOST_LOG_INFO, "[openxr] %s hand: %s%s", hand ? "right" : "left", text,
				xr.has_bumper[hand] ? "" : " (grip and stick click stand for a bumper)");
		}
	}
}

static void poll_events(void)
{
	XrEventDataBuffer event = { XR_TYPE_EVENT_DATA_BUFFER };

	while (xrPollEvent(xr.instance, &event) == XR_SUCCESS)
	{
		if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
			session_state_changed(((const XrEventDataSessionStateChanged *)&event)->state);
		else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
			xr.exiting = 1;
		else if (event.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED)
			interaction_profile_changed();
		else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING)
			reference_space_changing((const XrEventDataReferenceSpaceChangePending *)&event);
		event.type = XR_TYPE_EVENT_DATA_BUFFER;
		event.next = NULL;
	}
}

float host_xr_set_refresh_rate(float hertz);

static void log_statistics(void)
{
	struct timespec now;
	double seconds;

	clock_gettime(CLOCK_MONOTONIC, &now);
	seconds = (double)(now.tv_sec - xr.stats_start.tv_sec) + (now.tv_nsec - xr.stats_start.tv_nsec) * 1e-9;
	if (seconds < 10.0)
		return;
	/* (the [vr-perf] line is the game's, vr_present; this is the runtime's side of it) */
	host_logf(HOST_LOG_INFO, "[openxr] %.1f frames/s (%lld rendered) over %.1f s, period %.3f ms, state %s",
		xr.frames / seconds, xr.frames_rendered, seconds, xr.frame_state.predictedDisplayPeriod * 1e-6,
		state_name(xr.state));
	/* A rate above 72 that the game does not hold while worn (focused) is
	worse than 72 held: after two windows of ten seconds short of 90% of
	it, fall back */
	if (xr.refresh_rate > 72.5f && xr.focused && xr.frames_rendered > 0)
	{
		if (xr.frames / seconds < xr.refresh_rate * 0.9f)
		{
			if (++xr.refresh_misses >= 2)
			{
				host_logf(HOST_LOG_WARN, "[openxr] %.0f Hz not held (%.1f frames/s): back to 72 Hz",
					xr.refresh_rate, xr.frames / seconds);
				host_xr_set_refresh_rate(72.0f);
			}
		}
		else
		{
			xr.refresh_misses = 0;
		}
	}
	xr.frames = xr.frames_rendered = 0;
	xr.stats_start = now;
}

/* Waits for and begins the next frame when the session runs; otherwise
polls and sleeps briefly so the caller's loop keeps turning. */
int host_xr_begin_frame(struct halo_xr_frame *frame)
{
	XrFrameWaitInfo wait = { XR_TYPE_FRAME_WAIT_INFO };
	XrFrameBeginInfo begin = { XR_TYPE_FRAME_BEGIN_INFO };
	XrViewLocateInfo locate = { XR_TYPE_VIEW_LOCATE_INFO };
	XrViewState view_state = { XR_TYPE_VIEW_STATE };
	XrSpaceLocation head = { XR_TYPE_SPACE_LOCATION };
	uint32_t count = 0;
	int eye;

	memset(frame, 0, sizeof(*frame));
	if (!xr.initialized)
		return 0;
	poll_events();
	frame->session_state = (uint32_t)xr.state;
	if (xr.exiting)
		frame->flags |= HALO_XR_FRAME_EXIT;
	if (!xr.running || xr.frame_begun)
	{
		if (!xr.running)
		{
			struct timespec pause = { 0, 10 * 1000 * 1000 };

			nanosleep(&pause, NULL);
		}
		return 0;
	}
	xr.frame_state.type = XR_TYPE_FRAME_STATE;
	xr.frame_state.next = NULL;
	if (!check(xrWaitFrame(xr.session, &wait, &xr.frame_state), "xrWaitFrame") ||
		!check(xrBeginFrame(xr.session, &begin), "xrBeginFrame"))
	{
		return 0;
	}
	xr.frame_begun = 1;
	xr.frames++;
	frame->flags |= HALO_XR_FRAME_BEGUN;
	frame->predicted_display_time = xr.frame_state.predictedDisplayTime;
	frame->predicted_display_period = xr.frame_state.predictedDisplayPeriod;
	if (xr.frame_state.shouldRender)
		frame->flags |= HALO_XR_FRAME_SHOULD_RENDER;
	if (xr.focused)
		frame->flags |= HALO_XR_FRAME_FOCUSED;
	update_recentre(xr.frame_state.predictedDisplayTime);
	if (xr.recentred_this_frame)
		frame->flags |= HALO_XR_FRAME_RECENTRED;

	locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locate.displayTime = xr.frame_state.predictedDisplayTime;
	locate.space = xr.local;
	xr.views[0].type = xr.views[1].type = XR_TYPE_VIEW;
	xr.views[0].next = xr.views[1].next = NULL;
	xr.views_valid = XR_SUCCEEDED(xrLocateViews(xr.session, &locate, &view_state, 2, &count, xr.views)) &&
		count == 2 &&
		(view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) &&
		(view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT);
	if (xr.views_valid)
	{
		frame->flags |= HALO_XR_FRAME_VIEWS_VALID;
		for (eye = 0; eye < 2; eye++)
		{
			recentred_pose(&xr.views[eye].pose, &frame->eye[eye]);
			frame->fov[eye][0] = xr.views[eye].fov.angleLeft;
			frame->fov[eye][1] = xr.views[eye].fov.angleRight;
			frame->fov[eye][2] = xr.views[eye].fov.angleUp;
			frame->fov[eye][3] = xr.views[eye].fov.angleDown;
		}
	}
	if (XR_SUCCEEDED(xrLocateSpace(xr.view, xr.local, xr.frame_state.predictedDisplayTime, &head)) &&
		(head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
	{
		recentred_pose(&head.pose, &frame->head);
	}
	else
	{
		frame->head.orientation[3] = 1.0f;
	}
	sync_input(frame);
	log_statistics();
	return 1;
}

/* the index of the acquired image in info.images[which], or -1 */
int host_xr_acquire(uint32_t which)
{
	struct swapchain *swapchain;
	XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	uint32_t index = 0;

	if (which >= HALO_XR_SWAPCHAIN_COUNT || !xr.frame_begun)
		return -1;
	swapchain = &xr.swapchains[which];
	if (swapchain->in_atlas)
	{
		/* the atlas's image of this frame, whichever part comes first */
		if (xr.atlas.acquired)
			return xr.atlas_index;
		swapchain = &xr.atlas;
	}
	else if (swapchain->acquired)
		return -1;
	if (!check(xrAcquireSwapchainImage(swapchain->handle, &acquire, &index), "xrAcquireSwapchainImage"))
		return -1;
	wait.timeout = XR_INFINITE_DURATION;
	if (!check(xrWaitSwapchainImage(swapchain->handle, &wait), "xrWaitSwapchainImage"))
		return -1;
	swapchain->acquired = 1;
	if (swapchain == &xr.atlas)
		xr.atlas_index = (int)index;
	return (int)index;
}

void host_xr_release(uint32_t which)
{
	XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };

	/* (the atlas's parts go with it, as the frame ends) */
	if (which >= HALO_XR_SWAPCHAIN_COUNT || xr.swapchains[which].in_atlas || !xr.swapchains[which].acquired)
		return;
	check(xrReleaseSwapchainImage(xr.swapchains[which].handle, &release), "xrReleaseSwapchainImage");
	xr.swapchains[which].acquired = 0;
}

static void release_atlas(void)
{
	XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };

	if (!xr.atlas.acquired)
		return;
	check(xrReleaseSwapchainImage(xr.atlas.handle, &release), "xrReleaseSwapchainImage");
	xr.atlas.acquired = 0;
}

/* Ends the frame begun by host_xr_begin_frame with the layers the guest
drew (their swapchains released); a projection layer uses this frame's
views, so it needs them valid. */
void host_xr_end_frame(const struct halo_xr_layers *layers)
{
	XrCompositionLayerProjectionView views[2];
	XrCompositionLayerProjection projection = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	XrCompositionLayerQuad quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	XrCompositionLayerQuad reticle = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	XrCompositionLayerQuad beam = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	XrCompositionLayerQuad pointer = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	XrCompositionLayerQuad screen[2] = { { XR_TYPE_COMPOSITION_LAYER_QUAD }, { XR_TYPE_COMPOSITION_LAYER_QUAD } };
	XrCompositionLayerQuad fade = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	XrCompositionLayerQuad scope = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	const XrCompositionLayerBaseHeader *list[9];
	XrFrameEndInfo end = { XR_TYPE_FRAME_END_INFO };
	uint32_t count = 0;
	int which;

	if (!xr.frame_begun)
		return;
	for (which = 0; which < HALO_XR_SWAPCHAIN_COUNT; which++)
		host_xr_release((uint32_t)which);
	release_atlas();
	if (layers && (layers->flags & HALO_XR_LAYER_PROJECTION) && xr.views_valid && xr.frame_state.shouldRender)
	{
		int eye;

		for (eye = 0; eye < 2; eye++)
		{
			memset(&views[eye], 0, sizeof(views[eye]));
			views[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
			views[eye].pose = xr.views[eye].pose;
			views[eye].fov = xr.views[eye].fov;
			views[eye].subImage.swapchain = xr.swapchains[eye].handle;
			views[eye].subImage.imageRect.offset.x = (int32_t)xr.swapchains[eye].x;
			views[eye].subImage.imageRect.offset.y = (int32_t)xr.swapchains[eye].y;
			views[eye].subImage.imageRect.extent.width = (int32_t)xr.swapchains[eye].width;
			views[eye].subImage.imageRect.extent.height = (int32_t)xr.swapchains[eye].height;
		}
		projection.space = xr.local;
		projection.viewCount = 2;
		projection.views = views;
		list[count++] = (const XrCompositionLayerBaseHeader *)&projection;
	}
	/* a 3D screen: each eye sees its own image on the same quad */
	if (layers && (layers->flags & HALO_XR_LAYER_STEREO_SCREEN) && xr.frame_state.shouldRender)
	{
		int eye;

		for (eye = 0; eye < 2; eye++)
		{
			const struct swapchain *swapchain = &xr.swapchains[eye];

			screen[eye].eyeVisibility = eye ? XR_EYE_VISIBILITY_RIGHT : XR_EYE_VISIBILITY_LEFT;
			screen[eye].subImage.swapchain = swapchain->handle;
			screen[eye].subImage.imageRect.offset.x = (int32_t)swapchain->x;
			screen[eye].subImage.imageRect.offset.y = (int32_t)swapchain->y;
			screen[eye].subImage.imageRect.extent.width = (int32_t)swapchain->width;
			screen[eye].subImage.imageRect.extent.height = (int32_t)swapchain->height;
			screen[eye].size.width = layers->quad_size[0];
			screen[eye].size.height = layers->quad_size[1];
			screen[eye].space = xr.local;
			local_pose(&layers->quad_pose, &screen[eye].pose);
			list[count++] = (const XrCompositionLayerBaseHeader *)&screen[eye];
		}
	}
	/* the reticle sits in the world, under the HUD */
	if (layers && (layers->flags & HALO_XR_LAYER_RETICLE) && xr.frame_state.shouldRender)
	{
		const struct swapchain *swapchain = &xr.swapchains[HALO_XR_SWAPCHAIN_RETICLE];

		reticle.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		reticle.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		reticle.subImage.swapchain = swapchain->handle;
		reticle.subImage.imageRect.extent.width = (int32_t)swapchain->width;
		reticle.subImage.imageRect.extent.height = (int32_t)swapchain->height;
		reticle.size.width = layers->reticle_size[0];
		reticle.size.height = layers->reticle_size[1];
		reticle.space = xr.local;
		local_pose(&layers->reticle_pose, &reticle.pose);
		list[count++] = (const XrCompositionLayerBaseHeader *)&reticle;
	}
	if (layers && (layers->flags & HALO_XR_LAYER_QUAD) && xr.frame_state.shouldRender)
	{
		const struct swapchain *swapchain = &xr.swapchains[HALO_XR_SWAPCHAIN_QUAD];

		if (layers->flags & HALO_XR_LAYER_QUAD_ALPHA)
			quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		quad.subImage.swapchain = swapchain->handle;
		quad.subImage.imageRect.offset.x = (int32_t)swapchain->x;
		quad.subImage.imageRect.offset.y = (int32_t)swapchain->y;
		quad.subImage.imageRect.extent.width = (int32_t)swapchain->width;
		quad.subImage.imageRect.extent.height = (int32_t)swapchain->height;
		quad.size.width = layers->quad_size[0];
		quad.size.height = layers->quad_size[1];
		if (layers->flags & HALO_XR_LAYER_QUAD_HEAD_LOCKED)
		{
			quad.space = xr.view;
			memcpy(&quad.pose.position, layers->quad_pose.position, sizeof(quad.pose.position));
			memcpy(&quad.pose.orientation, layers->quad_pose.orientation, sizeof(quad.pose.orientation));
		}
		else
		{
			quad.space = xr.local;
			local_pose(&layers->quad_pose, &quad.pose);
		}
		list[count++] = (const XrCompositionLayerBaseHeader *)&quad;
	}
	/* the menus' pointer over their panel (layers have no depth: drawn
	after it, or the panel hides them): the beam, the reticle's middle row
	stretched from the hand to the panel, then the dot where it lands */
	if (layers && (layers->flags & HALO_XR_LAYER_BEAM) && xr.frame_state.shouldRender)
	{
		const struct swapchain *swapchain = &xr.swapchains[HALO_XR_SWAPCHAIN_RETICLE];

		beam.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		beam.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		beam.subImage.swapchain = swapchain->handle;
		beam.subImage.imageRect.offset.y = (int32_t)(swapchain->height / 2);
		beam.subImage.imageRect.extent.width = (int32_t)swapchain->width;
		beam.subImage.imageRect.extent.height = 1;
		beam.size.width = layers->beam_size[0];
		beam.size.height = layers->beam_size[1];
		beam.space = xr.local;
		local_pose(&layers->beam_pose, &beam.pose);
		list[count++] = (const XrCompositionLayerBaseHeader *)&beam;
	}
	if (layers && (layers->flags & HALO_XR_LAYER_POINTER) && xr.frame_state.shouldRender)
	{
		const struct swapchain *swapchain = &xr.swapchains[HALO_XR_SWAPCHAIN_RETICLE];

		pointer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		pointer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		pointer.subImage.swapchain = swapchain->handle;
		pointer.subImage.imageRect.extent.width = (int32_t)swapchain->width;
		pointer.subImage.imageRect.extent.height = (int32_t)swapchain->height;
		pointer.size.width = layers->pointer_size[0];
		pointer.size.height = layers->pointer_size[1];
		pointer.space = xr.local;
		local_pose(&layers->pointer_pose, &pointer.pose);
		list[count++] = (const XrCompositionLayerBaseHeader *)&pointer;
	}
	/* a weapon's scope, held in the hand: nearer than the HUD */
	if (layers && (layers->flags & HALO_XR_LAYER_SCOPE) && xr.frame_state.shouldRender)
	{
		const struct swapchain *swapchain = &xr.swapchains[HALO_XR_SWAPCHAIN_SCOPE];

		scope.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		scope.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		scope.subImage.swapchain = swapchain->handle;
		scope.subImage.imageRect.extent.width = (int32_t)swapchain->width;
		scope.subImage.imageRect.extent.height = (int32_t)swapchain->height;
		scope.size.width = layers->scope_size[0];
		scope.size.height = layers->scope_size[1];
		scope.space = xr.local;
		local_pose(&layers->scope_pose, &scope.pose);
		list[count++] = (const XrCompositionLayerBaseHeader *)&scope;
	}
	/* a fade to black over everything: a quad just ahead of the eyes,
	wider than they see */
	if (layers && (layers->flags & HALO_XR_LAYER_FADE) && xr.frame_state.shouldRender)
	{
		const struct swapchain *swapchain = &xr.swapchains[HALO_XR_SWAPCHAIN_FADE];

		fade.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		fade.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		fade.subImage.swapchain = swapchain->handle;
		fade.subImage.imageRect.extent.width = (int32_t)swapchain->width;
		fade.subImage.imageRect.extent.height = (int32_t)swapchain->height;
		fade.size.width = fade.size.height = 2.0f;
		fade.space = xr.view;
		fade.pose.orientation.w = 1.0f;
		fade.pose.position.z = -0.25f;
		list[count++] = (const XrCompositionLayerBaseHeader *)&fade;
	}
	if (count)
		xr.frames_rendered++;
	end.displayTime = xr.frame_state.predictedDisplayTime;
	end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	end.layerCount = count;
	end.layers = list;
	check(xrEndFrame(xr.session, &end), "xrEndFrame");
	xr.frame_begun = 0;
}

/* asks the runtime for a display refresh rate (XR_FB_display_refresh_rate):
the nearest it offers at or below `hertz`; returns the rate asked for, or 0
when the runtime cannot change it */
float host_xr_set_refresh_rate(float hertz)
{
	PFN_xrEnumerateDisplayRefreshRatesFB enumerate = NULL;
	PFN_xrRequestDisplayRefreshRateFB request = NULL;
	float rates[16], chosen = 0.0f;
	uint32_t count = 0, index;

	if (!xr.initialized || !xr.refresh_rate_extension ||
		XR_FAILED(xrGetInstanceProcAddr(xr.instance, "xrEnumerateDisplayRefreshRatesFB", (PFN_xrVoidFunction *)&enumerate)) ||
		XR_FAILED(xrGetInstanceProcAddr(xr.instance, "xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction *)&request)) ||
		!check(enumerate(xr.session, 16, &count, rates), "xrEnumerateDisplayRefreshRatesFB"))
	{
		return 0.0f;
	}
	for (index = 0; index < count; index++)
	{
		host_logf(HOST_LOG_INFO, "[openxr] display refresh rate %.1f Hz offered", rates[index]);
		if (rates[index] <= hertz + 0.5f && rates[index] > chosen)
			chosen = rates[index];
	}
	if (chosen <= 0.0f || !check(request(xr.session, chosen), "xrRequestDisplayRefreshRateFB"))
		return 0.0f;
	host_logf(HOST_LOG_INFO, "[openxr] display refresh rate %.1f Hz asked for", chosen);
	xr.refresh_rate = chosen;
	xr.refresh_misses = 0;
	return chosen;
}

void host_xr_recenter(uint32_t flags)
{
	xr.recentre_pending = 1;
	if (flags & HALO_XR_RECENTRE_HEIGHT)
		xr.recentre_height = 1;
}

void host_xr_haptic(uint32_t hand, float amplitude, float seconds)
{
	XrHapticActionInfo info = { XR_TYPE_HAPTIC_ACTION_INFO };
	XrHapticVibration vibration = { XR_TYPE_HAPTIC_VIBRATION };

	if (!xr.actions_ready || !xr.focused || hand > 1)
		return;
	info.action = xr.actions[_action_haptic];
	info.subactionPath = xr.hands[hand];
	vibration.amplitude = amplitude;
	vibration.duration = (XrDuration)(seconds * 1e9f);
	vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
	xrApplyHapticFeedback(xr.session, &info, (const XrHapticBaseHeader *)&vibration);
}

/* the player asked to quit: the runtime takes the session through STOPPING
(session_state_changed ends it) to EXITING, and host_xr_begin_frame then
reports HALO_XR_FRAME_EXIT. With no running session there is nothing to wait
for. */
void host_xr_request_exit(void)
{
	if (!xr.initialized || !xr.running)
	{
		xr.exiting = 1;
		return;
	}
	if (!check(xrRequestExitSession(xr.session), "xrRequestExitSession"))
		xr.exiting = 1;
}

#endif /* HALO_VR */
