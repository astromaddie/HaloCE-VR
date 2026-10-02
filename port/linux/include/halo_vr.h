/*
HALO_VR.H

The game's side of stereo rendering in the VR build (HALO_VR;
port/linux/game/vr_render.c, over port/linux/src/vr_frame.c).

A stereo frame renders the one player window three times in render_frame's
window loop: the left and right eyes (each resolved into its swapchain
image as it finishes), then the HUD alone on a transparent ground, which
the headset shows on a layer ahead of the head. The game advances its
per-frame state once: systems that move on as they draw do so for the left
eye only.
*/

#ifndef __HALO_VR_H
#define __HALO_VR_H

#ifdef HALO_VR

struct render_window;
struct render_camera;
union real_rectangle2d;

enum
{
	_vr_render_pass_none = -1,
	_vr_render_pass_left_eye,
	_vr_render_pass_right_eye,
	_vr_render_pass_hud,
};

/* which pass the window being rendered is */
extern int vr_render_pass;

#define VR_RENDER_EYE() (vr_render_pass == _vr_render_pass_left_eye || vr_render_pass == _vr_render_pass_right_eye)
#define VR_RENDER_HUD() (vr_render_pass == _vr_render_pass_hud)
/* the passes after the first, which must not advance per-frame state */
#define VR_RENDER_REPEAT() (vr_render_pass > _vr_render_pass_left_eye)

/* main_game_render: makes a single player window (followed by the console
window) into the eyes, the HUD and the console window when this frame is
drawn in stereo; returns the window count to render */
short vr_render_windows(struct render_window *windows, short window_count);
/* render_frame, around each window */
void vr_render_window_begin(short window_index);
void vr_render_window_end(short window_index);
/* render_player_frame: the eye's frustum bounds replace the window's */
void vr_render_frustum_bounds(union real_rectangle2d *bounds);
/* the camera the first-person weapon is posed from: the head's in stereo */
void vr_render_weapon_camera(struct render_camera *camera);
/* player_control_update: the head aims the first local player (vr.h) */
void vr_player_control_facing(short local_player_index);
/* 1 while the head aims: no magnetism dragging the view */
int vr_render_aiming(void);
/* clears the target being drawn to transparent black (the HUD pass) */
void halo_vr_clear_transparent(void);
/* copies the back buffer into an eye's image (port/linux/src/d3d8_gl.c) */
void halo_vr_resolve_eye(int eye);

#else

#define VR_RENDER_EYE() 0
#define VR_RENDER_HUD() 0
#define VR_RENDER_REPEAT() 0

#endif

#endif
