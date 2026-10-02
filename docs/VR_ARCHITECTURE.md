# Halo CE VR on Steam Frame: architecture and status

This fork adds a VR build of the Android port for the Steam Frame. The headset runs Android
apps inside Lepton, a container, and the build uses OpenXR to talk to SteamVR. Desktop and phone
builds are unchanged: everything here is behind `configure.py --vr` (`HALO_VR`).

## Status (2026-10-02)

| Area | State |
| --- | --- |
| Build, deploy, OpenXR session | Done; played in the headset. |
| Menus and loading on a flat screen | Done; played. |
| Stereo gameplay with a HUD layer | Done; played. The HUD's alpha is made from its brightness. |
| Performance | **72 Hz held at 1.5× resolution (2592² an eye)**, played. Fixed: lens-flare occlusion reads that stalled the CPU on the GPU (now asynchronous), Mesa's trace markers, per-draw buffer maps (now persistently mapped). The game thread is busy 53% of a 72 Hz frame. |
| 90 Hz | Set in the Frame's display settings. The game thread needs about 7.3 ms of CPU a frame, against 90 Hz's 11.1 ms. **Not yet seen worn at 90.** |
| Hand-aimed weapons | Default (`vr.aim = "hand"`); verified unattended with synthetic hands. **Not yet played.** |
| Arm IK | Default (`vr.arms = "ik"`); verified unattended. **Not yet played.** |
| 3D cutscenes on a screen, fades | Default (`vr.cinema_3d`); verified unattended. **Not yet seen worn.** |
| Vehicles: first-person seats, stick steering | Built, **not yet run on the device** (the headset went offline mid-test). `vr.vehicle_view` "first_person" (default) or "chase"; `vr.vehicle_steering` "stick" (default), "head" or "hand". |
| Scope, comfort options, foveation | Not started. |

## Device facts (Steam Frame, Lepton 2.8.14, 2026-10-02)

- **Android:** Android 11 (API 30), arm64-v8a, 4 KB pages. Upstream's limit on 16 KB pages
  does not apply here.
- **GL:** OpenGL ES 3.2 is Mesa **Zink** on Turnip (Adreno 750). `GL_EXT_sRGB_write_control`
  is available.
- **OpenXR:** the runtime is SteamVR 2.17.10, found through
  `/vendor/etc/openxr/1/active_runtime.json`; the Khronos runtime broker is absent.
  - Eyes are 1728×1728, and the default refresh is 72 Hz.
  - Swapchain formats: `SRGB8_ALPHA8`, `SRGB8`, depth.
  - `XR_VALVE_frame_controller_interaction` is present.
- **Missing services:** Lepton runs without a clipboard service.
  `port/android/patches/sdl3-no-clipboard-service.patch` handles that in SDL3.
- **Storage:** `/sdcard/Documents` is the headset's `~/Documents`, which survives Lepton
  resets. Game data, `config.toml` and saves go to `Documents/HaloCE`.

## Build and deploy

```bash
export ANDROID_HOME=$HOME/Library/Android/sdk JAVA_HOME=/opt/homebrew/opt/openjdk@17
python3 configure.py --lto=off --pgo=off --vr
ninja android_apk          # port/android/app/build/outputs/apk/vr/debug/app-vr-debug.apk
tools/steam_frame/extract_maps.py "Halo.iso" /tmp/halo   # once: maps out of your disc image
tools/steam_frame/deploy.sh --maps /tmp/halo/maps        # copy, install, start
```

**To play** on the headset alone, launch **HaloCEVR** from the Steam library. It is a Lepton
devkit title running `~/devkit-game/HaloCEVR/HaloCE-VR.apk`, and `deploy.sh` refreshes that
copy. To register it on another headset, copy the APK there, then run:

```bash
ssh steamos@10.86.200.233 'cd ~/devkit-utils && python3 steam-client-create-shortcut --parms "{\"gameid\": \"HaloCEVR\", \"directory\": \"/home/steamos/devkit-game/HaloCEVR\", \"force_appid\": \"\", \"argv\": [\"HaloCE-VR.apk\"], \"env\": {}, \"settings\": {\"steam_play\": \"1\", \"compat_tool\": \"lepton\"}, \"lepton_args\": \"\"}"'
```

**The refresh rate** follows the Steam Frame's display setting: SteamVR offers the app only
the rate it is on.

**To develop**, start **Lepton Development** from the Frame's Steam library first. Logs:
`adb -s 127.0.0.1:5555 logcat -s halo`.

Ninja does not rerun Gradle when only Java sources changed (the SDL patch, for example). Run
`./gradlew -PhaloVr assembleVrDebug` in `port/android` in that case.

## How it fits together

**Host (64-bit, `port/android/host/host_xr.c`).** The host owns:
- the Khronos loader, 1.1.63, fetched from Maven Central with its SHA-256 checked;
- the instance and a GLES session on the game thread's EGL context;
- three swapchains: left eye, right eye, and a quad;
- controller actions for the Frame, Touch and Index profiles;
- recentring.

The guest calls seven functions (`host_imports_vr.list`) with plain structures
(`halo_android_abi.h`, layout asserted on both ABIs). Swapchain images go to the guest as GL
texture names, which it draws into directly.

**Guest platform layer (`port/linux/src/vr_frame.c`, `vr.h`).**
- **Frame protocol.** The runtime's frame is begun by whichever comes first: `vr_aim`
  (`player_control_update`), `vr_stereo_begin` (`main_game_render`) or `vr_present`
  (`D3DDevice_Present`). It is ended in `vr_present`, which replaces the window blit and swap,
  so `xrWaitFrame` paces the loop. Halo already interpolates its 30 Hz simulation to the display
  rate, so no display-time scheduler is needed.
- **Screen.** In VR the game draws its 640×480 screen at the eye resolution
  (`vr.resolution_scale` × 1728, through the existing `screen_scale`).
- **Flat frames** (menus, loading, cutscenes) copy the picture to an opaque quad floating ahead
  (`vr.screen_distance`, `vr.screen_width`).
- **Eye views.** An eye's view is the game camera's position plus the tracked eye offset (head
  reach clamped to 0.35 m; 1 world unit = 10 ft), turned by the VR heading. Its frustum bounds are
  `{tanL/A, tanR/A, tanD, tanU}` with a 90° vertical field of view and A = 4/3. That is exactly
  `render_camera_build_frustum`'s off-centre form.
- **Copies to swapchains** run with `GL_FRAMEBUFFER_SRGB_EXT` disabled. The game's pixels are
  gamma-encoded already.

**Game side (`port/linux/game/vr_render.c`, `port/linux/include/halo_vr.h`).**
- **Windows.** `vr_render_windows` turns `main_game_render`'s single player window into four
  windows: left eye, right eye, HUD, console. `render_frame` then renders them in its own window
  loop. `render.frame_index` advances once per frame, and each eye has its own `window_index`, so
  lens-flare occlusion and fog history stay per eye.
- **Eye resolve.** `vr_render_window_end` copies each eye's back buffer into its swapchain image.
- **HUD pass.** It clears to transparent and draws only `interface_draw_screen`, the screen
  flash and the UI widgets. `vr_present` shows that image head-locked with alpha
  (`vr.hud_distance`, `vr.hud_width`).
- **Once per frame** (left eye only): the sky's animation phase (`render_sky.c`), weather
  simulation, glow particles, and the first-person weapon's pose. The weapon is posed from the
  head between the eyes. The fog's screen layers are left out of the eyes.
- **Head aim.** `vr_player_control_facing` (in `player_control.c`) sets the player's facing to
  the head's direction turned by the heading.
  - The right stick turns the heading, not the game: `vr.snap_turn` (30°) or
    `vr.smooth_turn_speed`.
  - Pressing both sticks recentres.
  - When the game turns the player itself, the heading follows.
  - View magnetism is off while the head aims.

**Hand aim (`vr.aim = "hand"`).**
- **On foot.** The right controller's aim pose sets the facing. Shots, grenades and melee follow
  the aiming vector.
  - The left stick is turned to move relative to the head.
  - In local games shots start at the hand, through `unit_adjust_projectile_ray`, unless a wall
    stands between the hand and the unit's eye.
- **Weapon model.** It is posed from a camera at the grip minus `vr.weapon_offset_*` (in the aim
  frame).
- **Reticle.** The HUD crosshair is hidden. A reticle quad is drawn where
  `collision_test_vector` along the hand's ray meets the world.
- **Seats.** In vehicle and turret seats the head aims.

**Input.** The headset's controllers merge into the first Xbox pad (`xinput_sdl.c`). The Frame
controllers map one-to-one onto it (A/B/X/Y, bumpers as white/black, d-pad, view/menu as
back/start). While the head aims, the right stick is consumed for turning.

## Measuring without wearing the headset

- `vr.force_render` renders stereo with the headset in standby. It uses a synthetic head and
  hands: the head is turned by `vr.diag_yaw`, the right hand by `vr.diag_hand_yaw`.
- `vr.dump_frame` and `vr.dump_cinema_frame` write the eyes and HUD of a gameplay or cutscene
  frame. A dump logs the aim state (seated or on foot, hand or head, yaws).
- `vr.timing` logs where a frame's time goes (`[vr-frame]`); `vr.timing_gpu` waits for the GPU
  to time it separately.
- `debug.telnet_console` opens a script console on 127.0.0.1:2323. Reach it with
  `adb forward tcp:2323 tcp:2323`. Useful commands: `map_name levels\a30\a30` loads a level,
  and `cinematic_skip_start_internal` / `cinematic_skip_stop_internal` skip its intro.
- `simpleperf` from the NDK runs in Lepton (`adb push` it to `/data/local/tmp`) and profiles the
  game thread. Threads' CPU time: `/proc/<pid>/task/*/stat`.
- Large copies over SSH time out while the game runs; use `adb pull`.

## Settings (`config.toml`, `[vr]`)

`enabled`, `stereo`, `resolution_scale` (1.5), `refresh_rate` (90), `world_scale` (0.328084 units/m),
`cinema_3d`, `cinema_separation`, `cinema_convergence`, `cinema_distance`, `cinema_width`, `arms`
("ik" / "hidden" / "animated"),
`screen_distance`, `screen_width`, `hud_distance`, `hud_width`, `aim` ("head"/"hand"),
`weapon_offset_right`/`_up`/`_back`, `snap_turn`, `smooth_turn_speed`.

Diagnostics:
- `probe_seconds`: dim colours in each eye.
- `force_render` + `diag_yaw`: synthetic head with the headset in standby.
- `dump_frame`: writes `vr-eye0.bmp`, `vr-eye1.bmp` and `vr-hud.bmp` to the data folder.

`tools/steam_frame/deploy.sh --config vr.KEY=VALUE` edits these on the headset.

## Known gaps and next steps

- **Two-handed aiming.** With the left hand on the foregrip, the left arm snaps to the gun, but
  the gun's angle still comes from the right controller alone.
- **Scope zoom.** It is ignored in stereo. A picture-in-picture scope would bring it back.
- **Vehicles.** `vr.vehicle_view = "first_person"` makes the director treat every seat as first
  person, which hides your own body. The eyes sit at your character's `head` marker. A seat's
  camera marker is the chase camera's place, so it isn't used.
  - The view turns with the vehicle's yaw only, keeping the horizon level. A passenger seat
    facing aside (the Pelican's) keeps the turn it had when you sat down.
  - The driver's right stick steers as in flat Halo, and your head only looks. Gunner seats aim
    with your head, and the handheld weapon is hidden in driver and gunner seats.
  - `vr.diag_drive_seconds` seats you as a driver for unattended tests; spawn a vehicle first
    with `cheat_all_vehicles`.
  - Untested on the device.
- **The HUD** is a flat panel ahead of you.
- **Comfort options** are missing: no vignette, no seated or standing height.
- **Map loads** block the game loop. SteamVR shows its own loading state while one runs.
- **Battery.** The Frame discharges even on the Mac's USB.
