# Halo: Combat Evolved VR for Steam Frame

This is my Steam Frame VR fork of [OpenCE](https://github.com/OpenCommunityEdition/OpenCE), previously called Halo CE Universal. It runs Halo on the headset itself, using the Android port through Lepton and OpenXR through SteamVR. No gaming PC needed to play.

**EXPERIMENTAL.** I made this for personal use, and it's still a work in progress. I've played it on my Frame, but several of the newer features and fixes still need another headset test. You need your own Xbox Halo game data; this repo doesn't supply it.

The VR target is **Steam Frame, ARM64 Android in Lepton**. Upstream's flat Linux, Windows and Android ports are still here. Other VR headsets aren't tested.

## What this fork adds

### Weapons and hands

- Full stereo rendering, head tracking and tracked controller aiming. You can aim with your hand or your head, and choose either hand for the gun.
- Two-handed weapon aiming and stabilization, adjustable gun offsets, and extra aim smoothing when zoomed.
- Arms that follow your hands using inverse kinematics, with animated or hidden arms as alternatives. Optional legs or a torso, plus a seated body with hands on the vehicle controls.
- Swing-to-melee, physical crouching, weapon holsters, swapping the gun between hands, and a flashlight gesture near your head. The normal button controls are still available.
- Weapon and interaction haptics.
- A picture-in-picture scope held at the gun, so zooming doesn't magnify your entire view. This currently needs hand aiming; the head-aim scope is unfinished.

### Movement and vehicles

- A VR controller layout, or the original Xbox-style layout if you prefer it. The Frame controllers have their own OpenXR bindings, with Touch and Index fallbacks in the code.
- Snap, smooth or no stick turning; movement relative to your head or either controller; and a hold-to-recentre button.
- Optional room-scale walking with collision checks. It's off by default and disabled during vehicles, cutscenes, scripted movement, death and network play.
- First-person vehicle seats, a chase-camera option, a level horizon, and smoother transitions into and out of seats.
- Warthog and Ghost driving on the left stick: sideways steers, forward is gas, back is reverse. An optional Warthog steering-wheel mode uses both grips on the wheel and the triggers as pedals.
- Banshee flying in the direction your weapon hand points. The Scorpion keeps its normal driving controls and head-aimed cannon. Stick, head and hand steering options are also available.

### HUD, menus and cutscenes

- A separate VR HUD with far, near and leash modes, adjustable placement and size, plus an aiming reticle.
- Stereo pause menus on a panel in front of you, with a controller laser and pointer dot.
- Two pages of VR settings in the pause menu. Changes save immediately, and older VR configs are migrated.
- Stereo 3D cutscenes on a cinema screen, with a flat-screen option and fades across both eyes.

### Rendering and storage

- A new single-pass stereo renderer draws the world for both eyes together. It's on by default; `vr.multiview = false` restores the two-pass renderer.
- Eyes rendered at their actual target resolution, anisotropic texture filtering, and reduced windshield reflections.
- Fixes for lens-flare stalls, buffer uploads and repeated compositor work, plus frame-time logging for checking performance on the Frame.
- Game data, settings and saves in `Documents/HaloCE`, outside Lepton's disposable app storage.

## What's been tested, and what hasn't

Earlier builds have been installed and played on a real Frame: menus, head tracking, stereo campaign gameplay, controller gunplay, vehicles and the 3D cinema screen. That doesn't mean every feature above has passed a headset test in the current build.

The latest development notes describe [8d73d4f6](https://github.com/astromaddie/HaloCE-VR/commit/8d73d4f670767bfb031303dbb9f0df23c6c97be1), from 5 October 2026. The new single-pass renderer was built and measured without wearing the headset. In Silent Cartographer, at 1.25× resolution and a 90 Hz runtime, it measured **67–89 fps**, versus **55–85 fps** with two passes. That still needs a proper playtest before I can say how it feels. Earlier headset tests held 72 Hz at 1.5× resolution; one indoor level held 90 Hz, while an open-level run fell to about 46 fps on an older build.

For now, start with **72 Hz in SteamVR's per-app display settings**. Stable 90 or 120 Hz across the campaign isn't there yet. The current render scale defaults to 1.25×.

Known limitations:

- The latest single-pass rendering, driving changes, gestures, scope, room-scale movement, HUD modes and settings still need another playtest on the Frame. Some have only build checks or tests without wearing the headset.
- The last headset session found broken strafing, an invisible menu pointer, windshield reflections and invalid OpenXR layer poses. Fixes are in the source, but haven't been confirmed in another worn session.
- The scope's placement and gesture thresholds still need tuning. The scope doesn't work with head aiming.
- Single-pass stereo doesn't use the hidden-area mask yet. MSAA, foveated rendering, a comfort vignette and a dedicated seated mode aren't implemented.
- Level loading can block the frame loop. There hasn't been a full campaign playthrough or a complete pause/resume, save-retention and exit acceptance test on the current build. VR multiplayer hasn't been tested.
- Quit through SteamVR's menu; there isn't a separate quit button in the VR settings.

The [architecture and development notes](docs/VR_ARCHITECTURE.md) have the measurements, headset feedback and implementation details.

## Default controls

These are the default VR controls. Left-handed mode swaps the weapon-hand trigger and bumper roles.

| Input | Action |
| --- | --- |
| Weapon-hand trigger / off-hand trigger | Fire / zoom |
| Weapon-hand bumper / off-hand bumper | Grenade / flashlight |
| Left stick / click | Move / crouch |
| Right stick / click | Turn / melee |
| Right A / B | Jump / action and reload |
| Right X / Y | Switch grenades / switch weapons |
| Menu | Pause, including VR settings |
| View, short press | Back |
| View, held 1 second | Recentre position and heading |
| View, held 3 seconds | Recentre and reset standing height |

## Game data and installation

You need the `maps/` folder from your own Xbox Halo: Combat Evolved disc image (`.iso` or `.xiso`). PC Halo and MCC data aren't the input for this port.

Extract the maps on your computer:

```bash
python3 tools/steam_frame/extract_maps.py /path/to/Halo.iso /path/to/extracted-halo
```

After building the APK below, connect the Frame over USB and start **Lepton Development** from its Steam library. The deploy helper needs `adb`, SSH access and `rsync`. Set your own Frame address rather than using the helper's development default:

```bash
export FRAME_HOST="steamos@YOUR_FRAME_ADDRESS"
tools/steam_frame/deploy.sh --maps /path/to/extracted-halo/maps --no-launch
```

Register the APK once as a **HaloCEVR** Lepton devkit title, following the [build and deploy instructions](docs/VR_ARCHITECTURE.md#build-and-deploy). Launch it from the Steam library so the controllers bind correctly. Later deployments update that title's APK.

Maps live in `~/Documents/HaloCE/maps` on SteamOS, exposed as `/sdcard/Documents/HaloCE/maps` inside Lepton. `config.toml` and saves use the same HaloCE folder. That location is intended to survive a Lepton reset; the current build still needs the full save-retention check.

## Build the VR version

You'll need Python 3, ninja, CMake, an Android SDK and NDK, a JDK 17 or later, and a clang with the `arm64_32` target. See the [Android toolchain requirements](port/android/README.md#requirements). You don't need the Xbox SDK or game data to compile.

```bash
git clone https://github.com/astromaddie/HaloCE-VR.git
cd HaloCE-VR
export ANDROID_HOME="/path/to/android-sdk"
export JAVA_HOME="/path/to/jdk-17"
python3 configure.py --vr --lto=off --pgo=off
ninja android_apk
```

The APK is `port/android/app/build/outputs/apk/vr/debug/app-vr-debug.apk`, with package name `com.halo.decomp.vr`. The first build downloads its public dependencies. If the tools aren't found automatically, use `--android-ndk /path/to/ndk` and `--android-guest-cc /path/to/clang`.

`--vr` matters: without it, you build the flat Android app. The inherited CI also builds the flat ports, so its artifacts aren't VR APKs. Automatic flat-build releases and Discord notifications are opt-in in this fork. This update publishes source, with no new prebuilt VR release.

## Credits and licenses

The game port comes from [OpenCE](https://github.com/OpenCommunityEdition/OpenCE), based on [bnunu/halo-1](https://github.com/bnunu/halo-1) and [punpckhdq/halo](https://github.com/punpckhdq/halo). The VR controls, gestures, smoothing, haptics and scope follow designs from LivingFray's PC mod, [HaloCEVR](https://github.com/LivingFray/HaloCEVR), reimplemented here for the decompiled game. The shared VR helpers come from [sfvr](https://github.com/astromaddie/sfvr).

The original [upstream README](UPSTREAM_README.md) and [CC0 notice](LICENSE.md) are kept, along with third-party notices. The bundled sfvr code has its own [MIT license](port/third_party/sfvr/LICENSE). These source notices don't grant permission to redistribute Halo's commercial game data, textures or audio. Bring your own game data.
