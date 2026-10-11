# Halo: Combat Evolved VR for Steam Frame

This is my Steam Frame VR fork of [OpenCE](https://github.com/OpenCommunityEdition/OpenCE), previously called Halo CE Universal. It runs Halo on the headset itself, using the Android port through Lepton and OpenXR through SteamVR. No gaming PC needed to play.

**EXPERIMENTAL.** I made this for personal use, and it's still a work in progress. You need your own Xbox Halo game data; this repo doesn't supply it.

The VR target is **Steam Frame, ARM64 Android in Lepton**. Upstream's flat Linux, Windows and Android ports are still here. Other VR headsets aren't tested.

## What this fork adds

### Weapons and hands

- Full stereo rendering, head tracking and tracked controller aiming. You can aim with your hand or your head, and choose either hand for the gun.
- Two-handed weapon aiming and stabilization, adjustable gun offsets, and extra aim smoothing when zoomed.
- Arms that follow your hands using inverse kinematics, with animated or hidden arms as alternatives. Optional legs or a torso, plus a seated body with hands on the vehicle controls.
- Swing-to-melee, physical crouching, weapon holsters, swapping the gun between hands, and a flashlight gesture near your head. The normal button controls are still available.
- Weapon and interaction haptics.
- A picture-in-picture scope held at the gun while using hand aiming, so zooming doesn't magnify your entire view.

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

## Performance

I've tested the VR features on my Frame, and it runs at a stable **120 Hz at the Frame's native 2160×2160 resolution per eye**.

The default `vr.resolution_scale = 1.25` renders at 2160×2160 per eye. The multiplier is relative to OpenXR's 1728×1728 eye target.

The [architecture and development notes](docs/VR_ARCHITECTURE.md) cover how the port works and keep the older development history.

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

## Build the VR APK

Build on x86-64 Linux, macOS, or x86-64 Ubuntu under Windows WSL2. The NDK's Linux host tools are x86-64; the Steam Frame is the ARM64 run target, not an assumed build host. Apple Silicon Macs need Rosetta for the NDK’s x86-64 tools.

On Windows, install Ubuntu with [WSL2](https://learn.microsoft.com/windows/wsl/install), then install a Linux JDK 17 or later and the Linux Android SDK Command-Line Tools inside Ubuntu. For example, install OpenJDK with `sudo apt update && sudo apt install -y openjdk-17-jdk`, and download/extract the Linux SDK Command-Line Tools under `$HOME/Android/Sdk`. Do not point `JAVA_HOME` or `ANDROID_HOME` at Windows installations. Keep the checkout and SDK under the WSL Linux home; for example, use `~/src/HaloCE-VR`. The official [Android SDK command-line tools](https://developer.android.com/tools/sdkmanager) and [NDK downloads](https://developer.android.com/ndk/downloads) pages explain the Linux packages. See also the [Android toolchain requirements](port/android/README.md#requirements).

You need Python 3, Ninja, CMake, JDK 17 or later, Android SDK platform 35 and an NDK, plus a host `clang` with the `arm64_32` target. `configure.py` selects the newest installed NDK unless `--android-ndk` is supplied. The build needs internet access for public dependencies, but no Xbox SDK, Halo maps or game data. Set `ANDROID_HOME` to your SDK and `JAVA_HOME` to your JDK. Common SDK locations are `$HOME/Library/Android/sdk` on macOS and `$HOME/Android/Sdk` on Linux or WSL2. After installing the SDK packages, accept their licenses with `sdkmanager --licenses`.

```bash
mkdir -p "$HOME/src"
cd "$HOME/src"
git clone https://github.com/astromaddie/HaloCE-VR.git
cd HaloCE-VR
export ANDROID_HOME="/path/to/android-sdk"  # Common paths are listed above.
export JAVA_HOME="/path/to/jdk-17"
export PATH="$ANDROID_HOME/cmdline-tools/latest/bin:$PATH"
sdkmanager --sdk_root="$ANDROID_HOME" "platforms;android-35"
sdkmanager --licenses
python3 configure.py --vr --lto=off --pgo=off
ninja android_apk
```

The VR APK is `port/android/app/build/outputs/apk/vr/debug/app-vr-debug.apk`, package `com.halo.decomp.vr`. `configure.py` chooses the newest NDK under `ANDROID_HOME` unless you pass `--android-ndk`; pass `--android-guest-cc /path/to/clang` if the `arm64_32` compiler isn't found automatically. `--vr` selects the Frame/OpenXR build; the flat Android target is a different APK. The repository contains source, not a prebuilt VR APK. The exact checked revision and prior full-build result are recorded in [PR0 readiness](docs/PR0_READINESS.md) and [its metadata](docs/PR0_METADATA.json).

## Game data, install and play

You need the `maps/` folder from your own Xbox Halo: Combat Evolved disc image (`.iso` or `.xiso`). PC Halo and MCC data aren't inputs for this port. Extract the maps on the build computer. In WSL2, a Windows file such as `C:\Users\you\Backups\Halo.iso` is reachable at `/mnt/c/Users/you/Backups/Halo.iso`; copy it into the Linux filesystem first if you prefer:

```bash
python3 tools/steam_frame/extract_maps.py "/mnt/c/Users/you/Backups/Halo.iso" /path/to/extracted-halo
```

For the first Steam entry, copy the APK to a folder and use Valve's public [SteamOS Devkit Client upload flow](https://partner.steamgames.com/doc/steamhardware/steamframe/loadgames): pair the Frame with the Devkit Client, open **Title Upload**, set **Name** to `HaloCEVR`, **Local Folder** to the folder containing the APK, **Start Command** to `HaloCE-VR.apk`, and **Runtime** to **Android**, then upload. This creates the library entry. For example:

```bash
mkdir -p .frame-upload/halo
cp port/android/app/build/outputs/apk/vr/debug/app-vr-debug.apk .frame-upload/halo/HaloCE-VR.apk
```

On Windows, copy `.frame-upload/halo` to your Windows Downloads folder (available inside WSL at `/mnt/c/Users/YOUR_WINDOWS_USERNAME/Downloads/`), then select that copied folder in the Devkit Client.

For later builds, or to copy the maps, enable SSH to the Frame and set its address. The build computer needs OpenSSH (`ssh`, `scp`) and `rsync`; `--no-adb` skips the USB ADB connection while it copies maps and refreshes the APK in an existing `~/devkit-game/HaloCEVR` title directory:

```bash
export FRAME_HOST="steamos@FRAME_HOST_OR_IP"
tools/steam_frame/deploy.sh --maps /path/to/extracted-halo/maps --no-adb
```

If the Devkit Client created the title under a different directory, upload each updated APK with that client. For ADB development installs, install Android SDK platform-tools (`adb`), connect the Frame by USB, start **Lepton Development** from its Steam library, and omit `--no-adb`; this helper expects the USB ADB device name `frame`. For play, launch **HaloCEVR** from the Steam library so SteamVR supplies controller input. The helper does not launch the game through ADB in `--no-adb` mode.

Maps live in `~/Documents/HaloCE/maps` on SteamOS, exposed as `/sdcard/Documents/HaloCE/maps` inside Lepton. `config.toml` and saves use the same HaloCE folder, outside Lepton's disposable app storage.

## Credits and licenses

The game port comes from [OpenCE](https://github.com/OpenCommunityEdition/OpenCE), based on [bnunu/halo-1](https://github.com/bnunu/halo-1) and [punpckhdq/halo](https://github.com/punpckhdq/halo). The VR controls, gestures, smoothing, haptics and scope follow designs from LivingFray's PC mod, [HaloCEVR](https://github.com/LivingFray/HaloCEVR), reimplemented here for the decompiled game. The shared VR helpers come from [sfvr](https://github.com/astromaddie/sfvr).

The original [upstream README](UPSTREAM_README.md) and [CC0 notice](LICENSE.md) are kept, along with third-party notices. The bundled sfvr code has its own [MIT license](port/third_party/sfvr/LICENSE). These source notices don't grant permission to redistribute Halo's commercial game data, textures or audio. Bring your own game data.
