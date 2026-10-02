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

#else

#define vr_active() 0
#define vr_initialize() ((void)0)
#define vr_probe() ((void)0)
#define vr_screen_scale(scale) 0
#define vr_present(source, width, height) ((void)0)
#define vr_controller(buttons, trigger, thumb) 0

#endif

#endif
