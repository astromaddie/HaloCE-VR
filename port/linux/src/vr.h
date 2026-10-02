/*
VR.H

The headset (OpenXR) side of the Android VR build (HALO_VR): the host owns
the OpenXR session (port/android/host/host_xr.c); this side drives its
frames from the game loop and draws into its swapchain images. Every
function is a no-op returning 0 when the build or vr.enabled leaves VR
off.
*/

#ifndef __HALO_LINUX_VR_H
#define __HALO_LINUX_VR_H

#ifdef HALO_VR

/* 1 once the OpenXR session exists (after the GL context does) */
int vr_active(void);
/* sets up the session; called once the GL context is current */
void vr_initialize(void);
/* vr.probe_seconds: dim test colours in each eye before the game starts */
void vr_probe(void);
/* the pixels per unit of the game's 640x480 screen in the headset: its
render targets match the eyes' resolution (d3d8_gl.c); 0 before the
session exists */
int vr_screen_scale(float scale[2]);
/* shows the frame the game drew, read from framebuffer `source` of
width x height pixels (row 0 at the top), and ends the runtime's frame;
replaces the flat screen's blit and swap (D3DDevice_Present) */
void vr_present(unsigned int source, int width, int height);
/* the headset's controllers as an Xbox pad: 1 with their state when the
session has input focus */
int vr_controller(unsigned int *buttons, float trigger[2], float thumb[4]);

/* ---------- stereo (port/linux/game/vr_render.c)

Halo's axes: +x forward, +y left, +z up, in world units of 10 feet. */

/* begins the runtime's frame (waiting for it) and says whether the game
may draw it in stereo: the session shows it, the head is tracked and
vr.stereo allows it. The answer holds until the frame is presented. */
int vr_stereo_begin(void);
/* eye 0 (left) or 1: its position, forward and up for a viewer at
`position` facing `forward` (only its heading counts: the head gives the
pitch and roll), and its frustum bounds (left, right, bottom, top) for a
90-degree vertical field of view on a screen of the aspect given
(render_camera_build_frustum); 0 outside a stereo frame */
int vr_eye_view(int eye, const float position[3], const float forward[3], float aspect,
	float out_position[3], float out_forward[3], float out_up[3], float bounds[4]);
/* the head between the eyes, as vr_eye_view gives an eye */
int vr_head_view(const float position[3], const float forward[3],
	float out_position[3], float out_forward[3], float out_up[3]);
/* copies the eye drawn into framebuffer `source` (width x height, row 0
at the top) into the eye's image */
void vr_resolve_eye(int eye, unsigned int source, int width, int height);

/* ---------- aiming with the head (port/linux/game/vr_render.c)

The view's heading is the player's, turned by the right stick (vr.snap_turn
or vr.smooth_turn_speed) rather than by the game: the game's facing follows
the head. */

/* begins the runtime's frame and gives the direction the player aims, for
a game whose facing has the yaw given; 0 when the head does not aim (no
stereo this frame). The heading takes up the game's yaw when the game
turned the player itself (a script, a respawn, another pad's stick). */
int vr_aim(float game_yaw, float out_forward[3]);
/* 1 while the head aims: magnetism and the right stick leave the view alone */
int vr_aiming(void);
/* the heading the eyes are turned by (Halo's x, y) */
int vr_heading_forward(float out_forward[3]);

#else

#define vr_active() 0
#define vr_initialize() ((void)0)
#define vr_probe() ((void)0)
#define vr_screen_scale(scale) 0
#define vr_present(source, width, height) ((void)0)
#define vr_controller(buttons, trigger, thumb) 0
#define vr_stereo_begin() 0
#define vr_aiming() 0

#endif

#endif
