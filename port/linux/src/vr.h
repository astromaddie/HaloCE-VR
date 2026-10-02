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

#else

#define vr_active() 0
#define vr_initialize() ((void)0)
#define vr_probe() ((void)0)

#endif

#endif
