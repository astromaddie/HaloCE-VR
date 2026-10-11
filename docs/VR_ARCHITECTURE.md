# Halo CE VR on Steam Frame: architecture and status

This fork adds a VR build of the Android port for the Steam Frame. The headset runs Android
apps inside Lepton, a container, and the build uses OpenXR to talk to SteamVR. Desktop and phone
builds are unchanged: everything here is behind `configure.py --vr` (`HALO_VR`).

For the current feature list, start with the [fork's README](../README.md).

**Current status (2026-10-11):** the author has confirmed the VR features in the headset and
stable 120 Hz at the Frame's native 2160×2160 resolution per eye. Single-pass stereo is on
by default. `vr.resolution_scale = 1.25` gives that native resolution from OpenXR's 1728×1728
eye target; the settings version is 4.

The notes below keep the development history, including earlier defaults, benchmarks and
"not yet worn" notes. Those describe the state at the time; the current confirmation above
supersedes the old performance advice and pending headset tests.

## Early status notes (2026-10-02, with later annotations)

| Area | State |
| --- | --- |
| Build, deploy, OpenXR session | Done; played in the headset. |
| Menus and loading on a flat screen | Done; played. |
| Stereo gameplay with a HUD layer | Done; played. The HUD's alpha is made from its brightness. |
| Performance | **72 Hz held at 1.5× resolution (2592² an eye)**, played. Fixed: lens-flare occlusion reads that stalled the CPU on the GPU (now asynchronous), Mesa's trace markers, per-draw buffer maps (now persistently mapped). The game thread is busy 53% of a 72 Hz frame. |
| 90 Hz | `vr.refresh_rate` defaults to 90, asked for at start (the Frame's display setting decides what is offered). The game thread needs about 7.3 ms of CPU a frame, against 90 Hz's 11.1 ms. **Not yet seen worn at 90.** If 90 is not held, the host falls back to 72 by itself (see Display rate). |
| Hand-aimed weapons | Default (`vr.aim = "hand"`); verified unattended with synthetic hands. **Not yet played.** |
| Arm IK | Default (`vr.arms = "ik"`); verified unattended. **Not yet played.** |
| 3D cutscenes on a screen, fades | Default (`vr.cinema_3d`); verified unattended. **Not yet seen worn.** |
| Vehicles: first-person seats | Worn 2026-10-04 (the Warthog bounced; code-ea's eased seat view since). `vr.vehicle_view` "first_person" (default) or "chase" |
| Driving: `vr.vehicle_steering` "drive" (default: the left stick steers in proportion, gas and reverse on it too), "wheel" (both grips on the Warthog's wheel, triggers as pedals), the Banshee flying where the hand points; "stick", "head", "hand" kept | Built after the 2026-10-04 wear (two-stick driving found bad; the user chose options A, C and D of the driving mockup). **Not yet worn** |
| VR-native controls, gestures, left-handed play | Built (`vr.controls = "vr"`, default; `"pad"` keeps the Xbox mapping). **Not yet run on the device.** |
| Aim smoothing when zoomed, haptics | Built, **not yet run on the device.** |
| Picture-in-picture scope | Built (`vr.scope`), **not yet run on the device.** |
| Far HUD drawn from the head | Built (15 m away, 10 m wide), **not yet seen worn.** |
| Near and leash HUD modes (`vr.hud_mode`) | Built, **not yet run on the device.** |
| Menu laser pointer | Worn 2026-10-04: the dot never showed (it was under the panel's layer). Now a beam and a dot over the panel; **the fix not yet worn.** |
| Menus in stereo on a world-locked panel (2 m, 1.9 m wide) | Built after the 2026-10-04 wear (the menu jittered on the head-locked HUD), **not yet worn.** |
| Room-scale walking | Built (`vr.roomscale`, off by default), **not yet run on the device.** |
| VR settings in the pause menu | Built, **not yet run on the device.** |
| System layer (View: short press back, 1 s recentre, 3 s recentre and standing height; the both-sticks recentre removed) | Built, **not yet run on the device.** |
| `vr.settings_version`, range clamping of numeric `vr.*` settings, the old HUD defaults migrated | Built; the config code was exercised on a computer (a fresh file, a version-0 file, clamps, the environment), **not yet on the device.** |
| Touch and Index fallbacks: grenade and flashlight (grip with a stick click) | Built, **not yet run on the device.** |
| Recentre from the runtime (`XrEventDataReferenceSpaceChangePending`) | Built, **not yet run on the device.** |
| 72 Hz automatic fallback | In the host since the refresh request was added; **not yet seen to fire worn.** |
| Comfort options, foveation | Not started. |
| sfvr (the cross-port library, vendored): View timing, turning, the haptic queue, the `[vr-perf]` line, settings ranges | Built; sfvr's own tests pass in its repo and the config code was exercised on a computer, **not yet run on the device.** |
| `vr.turn_mode` / `vr.snap_angle` (settings version 2, `vr.snap_turn` migrated); `vr.hud_mode` on the VR settings' second page; Index pause on a trackpad press | Built, **not yet run on the device.** |
| Cross-port standard (v0.1) items not done here: vignette, seated mode and `height_offset`, `camera_shake`, the standard's settings tabs, a `dominant_hand` setting, `hud_scale` and `hud_visible`, depth layer submission | Not started. |

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

Build-host choices, prerequisites and the full quick start are in the [README](../README.md#build-the-vr-apk). This recorded route uses Bash on x86-64 Linux, macOS (Rosetta on Apple Silicon), or x86-64 Ubuntu under Windows WSL2. The Frame itself is ARM64 and is the run target.

```bash
export ANDROID_HOME="/path/to/android-sdk"
export JAVA_HOME="/path/to/jdk-17"
python3 configure.py --lto=off --pgo=off --vr
ninja android_apk          # port/android/app/build/outputs/apk/vr/debug/app-vr-debug.apk
python3 tools/steam_frame/extract_maps.py /path/to/Halo.iso /path/to/extracted-halo
```

For a first Steam library entry, use Valve's [SteamOS Devkit Client Title Upload](https://partner.steamgames.com/doc/steamhardware/steamframe/loadgames). Name the entry `HaloCEVR`, upload a folder containing the APK, set its Start Command to the APK filename, and choose the Android runtime. For example, stage the built APK as `HaloCE-VR.apk` before selecting that folder in the client.

```bash
mkdir -p .frame-upload/halo
cp port/android/app/build/outputs/apk/vr/debug/app-vr-debug.apk .frame-upload/halo/HaloCE-VR.apk
```

On Windows, copy `.frame-upload/halo` to your Windows Downloads folder (available inside WSL at `/mnt/c/Users/YOUR_WINDOWS_USERNAME/Downloads/`), then select that copied folder in the Devkit Client.

To refresh an existing entry and copy maps, set your Frame's SSH address and run `deploy.sh`. Its `--no-adb` option uses SSH/`rsync` only; it also updates the title APK when the expected title directory exists. If the Devkit Client uses a different folder, upload the new APK through the client again.

```bash
export FRAME_HOST="steamos@FRAME_HOST_OR_IP"
tools/steam_frame/deploy.sh --maps /path/to/extracted-halo/maps --no-adb
```

For ADB development installs, start **Lepton Development**, connect over USB, and omit `--no-adb`; the helper expects the attached device to appear as `frame`. To play, launch **HaloCEVR** from the Frame's Steam library so SteamVR supplies controller input. ADB launches are for development only and do not provide the Steam title's controller setup.

**The refresh rate** follows the Steam Frame's display setting: SteamVR offers the app only
the rate it is on.

**Display rate and its fallback.** `vr.refresh_rate` (default 90) is asked for at start with
`xrRequestDisplayRefreshRateFB`, taking the highest rate the runtime offers at or below it
(`host_xr_set_refresh_rate`). The host logs `[vr-perf]` every 10 s. While worn (focused), if a
rate above 72 is requested and two of those windows in a row come in under 90% of it, the host
asks for 72 instead and logs `[openxr] N Hz not held ... back to 72 Hz`. The change is for that
run: the setting is not rewritten, and the next start asks for 90 again. 72 is the rate that
was held at 1.5× resolution on the device.

**To develop**, start **Lepton Development** from the Frame's Steam library first. Logs:
`adb -s 127.0.0.1:5555 logcat -s halo`.

Ninja does not rerun Gradle when only Java sources changed (the SDL patch, for example). Run
`./gradlew -PhaloVr assembleVrDebug` in `port/android` in that case.

## How it fits together

**Host (64-bit, `port/android/host/host_xr.c`).** The host owns:
- the Khronos loader, 1.1.63, fetched from Maven Central with its SHA-256 checked;
- the instance and a GLES session on the game thread's EGL context;
- four swapchains. An atlas holds the left eye, the right eye and a quad (the flat screen or
  HUD) side by side, acquired once a frame and released as it ends: SteamVR's GL binding makes
  its own context current for every acquire, wait and release, which costs a flush in Zink each
  time. The others are the hand's reticle (drawn once), a fade to black, and the scope;
- controller actions for the Frame, Touch and Index profiles;
- recentring (the origin's yaw and horizontal position, and apart from them its height, the
  standing height; see System layer);
- the display refresh request and its fallback;
- the session's end (`host_xr_request_exit`).

The guest calls nine functions (`host_imports_vr.list`; `host_xr_init`, `_begin_frame`,
`_acquire`, `_release`, `_end_frame`, `_recenter(flags)`, `_set_refresh_rate`, `_haptic`,
`_request_exit`) with plain structures (`halo_android_abi.h`, layout asserted on both ABIs).
`host_xr_recenter` takes `HALO_XR_RECENTRE_HEIGHT` as its flag, and `host_xr_request_exit`
adds no structure. Swapchain images go to the guest as GL texture names, which it draws into
directly.

**Guest platform layer (`port/linux/src/vr_frame.c`, `vr.h`).**
- **Frame protocol.** The runtime's frame is begun by whichever comes first: `vr_aim`
  (`player_control_update`), `vr_stereo_begin` (`main_game_render`) or `vr_present`
  (`D3DDevice_Present`). It is ended in `vr_present`, which replaces the window blit and swap,
  so `xrWaitFrame` paces the loop. Halo already interpolates its 30 Hz simulation to the display
  rate, so no display-time scheduler is needed.
- **Screen.** In VR the game draws its 640×480 screen at the eye resolution
  (`vr.resolution_scale` × 1728, through the existing `screen_scale`). The eyes' swapchain
  images are that size too (`host_xr_init`'s `eye_scale_percent`, within the runtime's maximum),
  so the compositor samples the whole picture. They were the recommended 1728 until 2026-10-04,
  and each eye was shrunk 1.5:1 by a plain blit first, which softened it.
- **Textures** are filtered anisotropically in the headset (`vr.texture_anisotropy`, 8 by
  default): Halo asks for it almost nowhere, and in VR the eyes look along floors and walls.
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
  windows: left eye, right eye, HUD, console. While a hand-aimed weapon is zoomed it makes five:
  left eye, right eye, scope, HUD, console. `render_frame` then renders them in its own window
  loop. `render.frame_index` advances once per frame, and each eye has its own `window_index`, so
  lens-flare occlusion and fog history stay per eye.
- **Eye resolve.** `vr_render_window_end` copies each eye's back buffer into its swapchain image.
- **HUD pass.** It clears to transparent and draws only `interface_draw_screen`, the screen
  flash and the UI widgets. `vr_present` shows that image as a quad layer with alpha, placed by
  `vr.hud_mode`:

  | `vr.hud_mode` | Panel | Placement |
  | --- | --- | --- |
  | `far` (default) | 15 m away, 10 m wide (37°), as HaloCEVR's | Head-locked in VIEW space, so it turns with the head including its roll. The eyes needn't refocus between it and the world |
  | `near` | 1.5 m away, 1.1 m wide (40°) | Head-locked in VIEW space, turned back by the head's roll: it follows the head's yaw and pitch, never its roll. (Until 2026-10-04 it was posed in LOCAL space each frame from the predicted head, and juddered whenever the compositor showed a frame late or twice) |
  | `leash` | 1.5 m away, 1.1 m wide (40°) | In LOCAL space, level at the head's height, and following the head's position. It keeps its yaw until the head's yaw is more than 25° from it, then eases to the head's with a 0.1 s time constant (about 0.3 s). A recentre or a return from a flat screen puts it ahead of the head |

  `vr.hud_distance` and `vr.hud_width` override the mode's distance and width when above 0
  (0, the default, is the mode's own). This is the cross-port standard's HUD setting. The
  default stays `far` until the user has worn the three side by side.
  - It is drawn from the head's place, turned as the panel is (`vr_hud_view`), with frustum
    bounds matching the panel's field (`vr_hud_bounds`, from the same distance and width in
    every mode). Nav points and friends' names are projected from where the eyes are, so
    they sit over what they mark (not where the hand points). With `vr.aim = "head"`, the
    leash's crosshair is at the panel's centre, not where the head aims.
  - The motion sensor turns with the head (`vr_render_motion_sensor_yaw`), as the HUD does.
- **Once per frame** (left eye only): the sky's animation phase (`render_sky.c`), weather
  simulation, glow particles, and the first-person weapon's pose. The weapon is posed from the
  head between the eyes. The fog's screen layers are left out of the eyes.
- **Head aim.** `vr_player_control_facing` (in `player_control.c`) sets the player's facing to
  the head's direction turned by the heading.
  - The right stick turns the heading, not the game (`sfvr_turn_update`): `vr.turn_mode`
    "snap" (a flick of `vr.snap_angle`, 30° by default; 15, 30, 45, 60 or 90), "smooth"
    (`vr.smooth_turn_speed`, 120°/s by default, with a 0.2 dead zone) or "off".
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

**Input.** The headset's controllers merge into the first Xbox pad (`xinput_sdl.c`). Each frame
`layout_controls` (`vr_frame.c`) builds that pad from the controllers by `vr.controls`:
- `"pad"`: the Frame controllers map one-to-one onto the Xbox pad (A/B/X/Y, bumpers as
  white/black, d-pad, view/menu as back/start).
- `"vr"` (default), a VR-native layout from each hand's own buttons (`hand_buttons` in the frame):

  | Input | Action |
  | --- | --- |
  | Weapon hand's trigger | Fire |
  | Off hand's trigger | Zoom (the scope) |
  | Right A | Jump |
  | Right B | Action / reload |
  | Right X | Switch grenades |
  | Right Y | Switch weapons |
  | Right stick click | Melee |
  | Weapon hand's bumper | Throw a grenade |
  | Off hand's bumper | Flashlight |
  | Left stick | Move; click to crouch |
  | Right stick | Turn |
  | Menu | Pause |
  | View | Back (a short press); held 1 s, recentre; held 3 s, recentre and standing height (see System layer) |

While the head or hand aims, the right stick is consumed for turning. Stick clicks and chords
never do system work.

**System layer** (`system_layer` in `vr_frame.c`; the cross-port standard's section 1, the
same in both `vr.controls` layouts). The left controller's View button (on Touch and Index, its
upper face button) and Menu:

| Input | Does |
| --- | --- |
| Menu | The game's Start (pause). The VR settings page is in its menu |
| View, short press | The game's Back, as the button lets go |
| View, held 1 s | Recentre: the head's yaw becomes forward and its horizontal position the origin; the standing height is kept. A 0.6/80 ms buzz in both hands |
| View, held 3 s | Recentre with the standing height taken again (from which crouching is measured), and a second buzz. The 1 s recentre has already happened |

- The host applies a recentre at the next frame boundary (`update_recentre`, and the frame's
  `HALO_XR_FRAME_RECENTRED` flag), when the heading, the room-scale standing point and the
  leash HUD start again. The flat and cutscene screens are placed ahead of the head again.
- The head's height at the recentre is also kept at the start and whenever the headset is put
  on (focus gained).
- The runtime's own recentre (`XrEventDataReferenceSpaceChangePending` for LOCAL) is a
  recentre with height, applied once the runtime has moved the space.
- The both-sticks recentre of earlier versions is gone: it also pressed crouch and melee.

**sfvr** (`port/third_party/sfvr`) is the cross-port C library for the parts every port does
the same: the View button's timing (`sfvr_view`), turning (`sfvr_turn`), the haptic vocabulary and
per-frame queue (`sfvr_haptics`), the `[vr-perf]` formatter (`sfvr_perf`) and the settings key
registry (`sfvr_settings`). It is a copy of `~/code/sfvr` (its commit is in `VERSION`) and is never
edited here: change it there, re-copy it, and update `VERSION`. The guest build compiles its
sources (`tools/android_build.py`). Room-scale stays Halo's own (`vr_room_step`), which is what
`sfvr_room` was derived from.

**Touch and Index** (fallback profiles, used only where the Frame's is not): they have no
bumpers, so a grip held with a stick click stands for the hand's bumper (grenade for the weapon
hand's, flashlight for the other's); the stick click alone still melees and crouches. The left
upper face button is View, and the left lower one switches grenades. The host knows each hand's
controller from the interaction profile changed event. Index's Menu (pause) is a firm
press of either trackpad (`trackpad/force`, a float on a boolean action, which reads as
pressed past the runtime's threshold), as its system button belongs to the runtime. That
binding's threshold and feel have not been tried.

**Gestures** (`update_gestures`, either layout; injected in `handle_one_player_input`):

| Gesture | Does | Setting |
| --- | --- | --- |
| A hand swung up or down fast | Melee | `melee_speed` (2.5 m/s) |
| Off hand at the side of the head | Flashlight | `flashlight_distance` (0.2 m) |
| Head lowered below the standing height | Crouch (held) | `crouch_height` (0.15 m) |
| Weapon hand's grip at a shoulder | Switch weapons | `holsters` |
| Off hand's grip held (with the gun) | Two-handed aim: the gun points from the weapon hand to the off hand | `two_handed` ("grip", "auto", "off") |
| Palms together, gripping | Swap the weapon hand | |

`vr.left_handed` starts with the gun in the left hand; the first-person model is drawn
mirrored (`halo_vr_mirror_winding` flips the triangles' winding).

**Weapon feel.** When zoomed, the aim is eased toward the hand's (`steady_aim`, after
HaloCEVR's half-life formula), steadying the scope. Shots buzz the weapon hand by weapon (and the
off hand when two-handed), scaled by `vr.haptics`. Buzzes are queued as they are asked for (`vr_haptic`) and played once a frame
by `vr_present` (`sfvr_haptic_flush`), the strongest of those overlapping a hand's; the View recentre's is
the standard's system buzz, 0.6 for 80 ms in both hands.

**VR settings in the pause menu** (`port/linux/game/vr_menu.c`). Halo's menus are widget
tags in each map, so when a level's tags load the VR build adds its own, cloned from the solo
pause menu's: same font, colours, buttons and boxes.
- **The pause menu** gets a fifth item, VR SETTINGS. Its button hints (B back, A select) move
  from under the list to under the mission objectives, making room.
- **VR SETTINGS** is the pause menu's backdrop and boxes with two columns of five items a page.
  Page 1: controls (VR / Xbox), aim (hand / head), gun hand, turning (snap 30 / snap 45 /
  smooth / off), room-scale, scope, vehicles (inside / chase), steering (stick / head /
  hand), cutscenes (3D / flat) and MORE..., which opens page 2: HUD (far / near / leash) and
  body. B goes back a page. `vr_menu_page` builds each page from the items marked with it.
- **No quit item:** SteamVR's own menu quits applications (the cross-port standard; QUIT TO
  STEAM was removed after the 2026-10-04 wear).
- **Changing a setting:** A or right steps it on, left steps it back, and B returns to the pause
  menu. The laser pointer clicks them too.
- **Saving:** each change is written into `config.toml` (`config_write_*`) and taken up at once
  (`vr_reload_settings`).
- **How the tags are added:** `cache_file_add_tag` puts tags made in memory after the map's own,
  in a copy of its tag table. A button's text comes from code (game data input function 41), and
  its changes run event handler functions 102 and 103, one past the end of each table.

**Menus in stereo** (the pause menu, the VR settings). While a menu is up (`vr_ui_pointer` is
told so each frame), the HUD's image leaves the HUD's placement for a panel held in the room:
2 m ahead where the head faced as the menu came up, at the head's height, 1.9 m wide (about 50°),
re-placed by a recentre. This is the cross-port standard's menu panel.

**Menus' laser pointer** (`vr_ui_pointer`, behind `halo_ui_pointer_update`). The weapon hand
points at the screen the menus are on, as the desktop builds' mouse does: the flat screen, or in
stereo the menu's panel. Its trigger clicks (with a tick of buzz), the right B goes back, and the
pad's buttons still work. It shows as a beam from the hand (the reticle image's middle row,
stretched along the ray and turned to face the head) and a dot where it lands, on layers of their
own (`HALO_XR_LAYER_BEAM`, `HALO_XR_LAYER_POINTER`), submitted last of all, over every panel,
quad, screen and fade: layers have no depth, and the cross-port standard has the pointer always
on top (on the reticle's layer the dot was hidden under the panel). The hand's aiming reticle faces
the head upright in the world, never rolling with it.

**Room-scale** (`vr.roomscale`, off by default; after HaloCEVR's). Each tick on foot,
`biped_update_moving` moves the player's collision pill by how far the head walked from where
the player stands (`vr_room_step`), with `collision_move_pill`, so a step into a wall slides
along it as walking does. Then that place to stand moves to the head.
- The whole step is taken even where a wall stopped the player. The view stays with the player
  rather than in the wall, and the room and the game drift apart; recentre to line them up.
- The eyes lean from that place, blended between its last two ticks as the game's camera is
  (`head_offset`). Walking then has no added latency and no judder at the 30 Hz ticks.
- Vehicles, cutscenes, scripts holding the player, death and network games hold it. Walking
  resumes from wherever the head is then, as does a jump of more than 0.5 m in a tick (tracking
  lost, a recentre).
- `vr.diag_walk_speed` walks the synthetic head ahead and logs how far the player went each
  second.

**Scope** (`vr.scope`, with the hand aiming). While zoomed:
- A pass between the eyes and the HUD (window 2, a repeat pass) renders the view along the gun.
  The camera is the hand's aim, rolled with the gun and kept out of walls. Its field is the middle
  of the game's zoomed one: half its height for a round sight, the sniper rifle's wide rectangle
  for the sniper.
- It draws into a square at the corner of the back buffer, as large as the scope's image (768²),
  so its cost scales with the scope, not the eyes. It has no HUD, fog screen or mirror, and the
  first-person gun is left out.
- `vr_resolve_scope` copies it through a mask (disc or rounded rectangle, a darker rim, a thin
  cross) into the scope swapchain. The image is shown on a layer held at the gun, at HaloCEVR's
  per-weapon offsets (pistol, sniper rifle, rocket launcher), `vr.scope_size` across.
- The eyes are never zoomed: the first-person gun stays in the hand, and the HUD layer leaves out
  the zoom mask.

## Measuring without wearing the headset

- `vr.force_render` renders stereo with the headset in standby. It uses a synthetic head and
  hands: the head is turned by `vr.diag_yaw`, the right hand by `vr.diag_hand_yaw`.
- `vr.dump_frame` and `vr.dump_cinema_frame` write the eyes and HUD of a gameplay or cutscene
  frame, and the scope when one is shown (`vr-scope.bmp`). A dump logs the aim state (seated or
  on foot, hand or head, yaws).
- `vr.diag_zoom_seconds` zooms in that long into play, switching first to a weapon that zooms.
  `vr.diag_two_handed` holds the synthetic left hand on the gun.
- The game logs the standard `[vr-perf]` line every 10 s (`sfvr_perf`, in `vr_present`), for
  example `[vr-perf] fps=71.9 missed=3 cpu=7.31ms game=1.20 render=5.02 copy=0.80 end=0.29
  eye=2.21/2.19ms gpu=n/a`. Its stages are `game` (the game's own work), `render` (the draw
  calls of the passes: eyes, scope, HUD), `copy` (the copies into the images) and `end`
  (ending the frame); the waits for the runtime are not CPU time. `eye` is each eye's pass,
  `missed` counts frames whose display time came more than a half period late, and `gpu`
  is the wait for the GPU, which only `vr.timing_gpu` (waiting each frame, slowing it) makes.
  `vr.timing` makes the window 2 s. The host's own statistics line (which drives the 72 Hz
  fallback) is an `[openxr]` line.
- `debug.telnet_console` opens a script console on 127.0.0.1:2323. Reach it with
  `adb forward tcp:2323 tcp:2323`. Useful commands: `map_name levels\a30\a30` loads a level,
  and `cinematic_skip_start_internal` / `cinematic_skip_stop_internal` skip its intro.
- `simpleperf` from the NDK runs in Lepton (`adb push` it to `/data/local/tmp`) and profiles the
  game thread. Threads' CPU time: `/proc/<pid>/task/*/stat`.
- Large copies over SSH time out while the game runs; use `adb pull`.

## Settings (`config.toml`, `[vr]`)

`enabled`, `stereo`, `resolution_scale` (1.25), `refresh_rate` (90), `world_scale` (0.328084 units/m),
`cinema_3d`, `cinema_separation`, `cinema_convergence`, `cinema_distance`, `cinema_width`, `arms`
("ik" / "hidden" / "animated"), `visibility_mask` (on: the runtime's hidden area masked first in each eye, about 15% of its
pixels unshaded), `body` ("off" / "legs" / "torso": on foot the player's own hips and legs, or also the
chest, headless and armless, under the head; in a vehicle's seat the whole body but its head,
as the game seats it, its chest set back out of the near plane and its hands on the controls),
`screen_distance`, `screen_width`, `hud_mode` ("far"/"near"/"leash"), `hud_distance` and `hud_width`
(0: the mode's own), `aim` ("head"/"hand"),
`weapon_offset_right`/`_up`/`_back`, `turn_mode` ("snap"/"smooth"/"off"), `snap_angle` (30),
`smooth_turn_speed`, `vehicle_view`,
`vehicle_steering`, `controls` ("vr"/"pad"), `move_relative` ("head"/"left"/"right"),
`two_handed`, `left_handed`, `melee_speed`, `flashlight_distance`, `crouch_height`, `holsters`,
`haptics`, `scope`, `scope_size` (0.06 m), `roomscale` (false), `settings_version` (2).

**Settings file hygiene.**
- `vr.settings_version` (now 2) is written to `config.toml`, and an older VR file is migrated
  on load, in place (the player's other lines and comments stay), and read again:
  - 0 to 1: `hud_distance` and `hud_width` at their old written defaults, 15 and 10, become 0
    (the HUD mode's own), so they do not override `near` or `leash`. A value that is not the
    old default is kept.
  - 1 to 2: `snap_turn` (degrees of a snap turn, 0 for smooth) becomes `turn_mode` ("smooth" if
    it was 0, else "snap") and `snap_angle` (the nearest of 15, 30, 45, 60, 90); the old line
    is commented out. `vr.snap_turn` stays as a read-only legacy key (never written or added
    to a file), and `HALO_VR_SNAP_TURN` still works for a run when `HALO_VR_TURN_MODE` and
    `HALO_VR_SNAP_ANGLE` are unset.
- Every numeric `vr.*` value is clamped on load (the file and the environment) to the ranges
  below, with a log line: `settings: vr.haptics is 7, outside 0 to 1; using 1`. The file keeps
  what the player wrote. Defaults are unchanged. A range with "or 0" also allows 0.

  | Setting | Range |
  | --- | --- |
  | `resolution_scale` | 0.75 to 1.5 |
  | `refresh_rate` | 72 to 144, or 0 |
  | `snap_angle` | 15 to 90 |
  | `smooth_turn_speed` | 30 to 360 |
  | `haptics` | 0 to 1 |
  | `hud_distance`, `hud_width` | 0.8 to 20, 0.3 to 30, or 0 (the mode's own) |
  | `screen_distance`, `screen_width` | 0.8 to 5, 0.5 to 6 |
  | `cinema_separation`, `_convergence`, `_distance`, `_width` | 0 to 0.1, 0.5 to 20, 1 to 10, 0.5 to 12 |
  | `world_scale` | 0.1 to 1 |
  | `melee_speed`, `flashlight_distance`, `crouch_height`, `scope_size` | 0 to 10, 0 to 0.5, 0 to 0.6, 0 to 0.3 |
  | `weapon_offset_*` | -0.5 to 0.5 |
  | diagnostics (`diag_*`, `dump_*`, `probe_seconds`) | wide, for sanity only (`probe_seconds` 0 to 60, `diag_walk_speed` 0 to 5) |

  Where sfvr's key registry (`port/third_party/sfvr`, the cross-port standard) has the key, its
  range is the one used (`config_clamp_ranges` looks it up); the rest are limits chosen here.
- `HALO_VR_<KEY>` in the environment overrides a key for one run (the standard's override
  form; a few keys are named otherwise, `HALO_VR_WEAPON_RIGHT` for example).

Diagnostics:
- `probe_seconds`: dim colours in each eye.
- `force_render` + `diag_yaw`: synthetic head with the headset in standby.
- `dump_frame`: writes `vr-eye0.bmp`, `vr-eye1.bmp` and `vr-hud.bmp` to the data folder.

`tools/steam_frame/deploy.sh --config vr.KEY=VALUE` edits these on the headset.

## Known gaps and next steps

- **Scope.** Its offsets and size are HaloCEVR's starting points and need tuning in the headset.
  Its reticle is our own cross, not the game's (the HUD isn't drawn into it). With the head
  aiming (`vr.aim = "head"`) a zoom shows nothing.
- **Gesture thresholds** (melee speed, duck depth, holster spots) are HaloCEVR's defaults and
  need tuning worn.
- **Vehicles.** `vr.vehicle_view = "first_person"` makes the director treat every seat as first
  person, which hides your own body. The eyes sit at your character's `head` marker. A seat's
  camera marker is the chase camera's place, so it isn't used.
  - The view turns with the vehicle's yaw only, keeping the horizon level. A passenger seat
    facing aside (the Pelican's) keeps the turn it had when you sat down.
  - **Driving** (`vr.vehicle_steering`, chosen from the 2026-10-04 driving mockup):
    - "drive" (default, option A): the left stick drives the Warthog and the Ghost. Sideways
      steers in proportion (the driver's facing is the vehicle's heading turned by the stick times
      the wheels' limit, and Halo turns the wheels toward the facing); forward and back are gas
      and reverse; the Ghost no longer strafes. Reversing, the angle is mirrored so the wheels
      follow the stick as a car's do. The head looks freely, and the right stick does nothing.
    - "wheel" (option C): as drive, and in a Warthog with both grips held the hands turn the
      wheel (the tilt of the line between them across the head's heading; a quarter turn is
      full lock) while the triggers are the pedals (right gas, left brake and reverse).
    - With either, a flier (the Banshee) flies where the hand points (option D), and the
      Scorpion keeps the game's driving with its cannon aimed by the head.
    - "stick" (the right stick steers, as in flat Halo), "head" and "hand" remain, for every
      vehicle. Settings version 4 moves the old default "stick" to "drive".
  - Gunner seats aim with your head, and the handheld weapon is hidden in driver and gunner
    seats.
  - `vr.diag_drive_seconds` seats you as a driver for unattended tests; spawn a vehicle first
    with `cheat_all_vehicles`.
  - Untested on the device.
- **The HUD** is a flat panel. Its image is the quad swapchain (1280×960, about 34 px a degree
  at the default size, a little less across the near and leash panels' 40°), shared with the
  flat screen. The near and leash panels sit in LOCAL space at the predicted pose, so unlike
  `far` (VIEW space) they can swim by the head's prediction error. Which mode is best is
  for a wear test to say.
- **Comfort options** are missing: no vignette, no seated mode or height offset. The standing
  height is taken at focus and by View held 3 s.
- **Map loads** block the game loop. SteamVR shows its own loading state while one runs.
- **Battery.** The Frame discharges even on the Mac's USB.

## Credits

The controls, gestures, aim smoothing, haptics and scope follow the designs of LivingFray's PC
mod [HaloCEVR](https://github.com/LivingFray/HaloCEVR), re-implemented here for the decompiled
game.

## Wear feedback, 2026-10-04 (Silent Cartographer)

Liked: smooth, good gunplay, the 3D cinema for the level-load video. The play log
(`lepton-steamlaunch-3667167125.log` on the Frame) shows **stereo gameplay at about 46 fps on the
90 Hz display**: frames missed in 88 of 104 `[vr-perf]` samples, about 13 ms of CPU a frame
(game 5.4, render 4.1, end 3.7) against 11.1 ms. SteamVR ran Halo at half rate and reprojected.
That alone gives judder and edge pop-in while turning. The way out is CPU time (not started) or
the Frame's display at 72 Hz, which Halo held before.

| # | Report | Status |
| --- | --- | --- |
| 1 | The Warthog bounces and flickers the whole drive | code-ea (seated view, interpolated head and heading, eased in and out). Half-rate frames add to it |
| 2 | The HUD jitters as the head turns | The near HUD is head-locked in VIEW space with the roll cancelled. **Not yet worn** |
| 3 | General jitter, pop-in at the edges | Explained by the half rate above; no fix yet beyond 72 Hz |
| 4 | The menu jitters up and down when opened | Menus are on a world-locked panel. **Not yet worn** |
| 5 | Getting into the Warthog is unsmooth | code-ea (the eased seat view) |
| 6 | Steering with one stick and gas on the other | The user chose A, C and D: built as `vr.vehicle_steering` "drive" (the default), "wheel", and the Banshee flying by hand with either. **Not yet worn** |
| 7 | Holding X went to third person | Not explained by the log, and the director keeps first person in seats with `vr.vehicle_view` "first_person". Physical X sends Black (switch grenades) and Halo's prompts say "X" for Action: a face-button option is in the same mockup. Turn on `vr.diag_buttons` for the next wear to catch it |
| 8 | No laser in menus | A beam and a dot over the panel. **Not yet worn** |
| 9 | QUIT TO STEAM in the VR settings | Removed |
| 10 | The image isn't sharp | Eye images at the render size, anisotropic filtering, and `vr.resolution_scale` 1.25 by default (2160 square, about the panels; settings version 3 moves an untouched 1.5 to it), which should also give open levels the frame time for 90 Hz. **Not yet worn** |
| Wish | An immersive main menu around the ring | Not started. The menu scene has no player window, so it needs its own stereo path; the world-locked menu panel above is the piece it was missing |

## Wear feedback, 2026-10-05 (round 2, on 69b76356)

"Definitely better than before." The play log
(`lepton-steamlaunch-3667167125.log`) shows `xrEndFrame failed: XR_ERROR_POSE_INVALID` on
564 frames.

| # | Report | Status |
| --- | --- | --- |
| 1 | Can't strafe on foot (VR controls and room-scale off didn't help) | A regression from the driving work: the drive throttle zeroed the stick's sideways whenever `vr.vehicle_steering` was "drive", on foot too. Now only for a driver. **Not yet worn** |
| 2 | The Warthog's windshield too reflective to see through | Glass reflects `vr.glass_reflection` (0.3) of the game's own in the headset (`rasterizer_xbox_transparent_geometry.c`, all glass shaders). **Not yet worn** |
| 3 | No laser on the main or pause menu | The beam's pose wasn't a unit quaternion (`look_rotation` from an unnormalised direction), so SteamVR refused every frame that drew it. `look_rotation` normalises, and the host normalises every layer pose. The beam ends 2 cm short of the panel and the dot lies 1 cm off it (a quad through another hides in it). **Not yet worn** |
| 4 | Green debug text over cutscenes as a map loads | The level scripts' `print` (the beta's debugging, `hs_print`): in the headset it goes to debug.txt only, unless `game.console_log` is "all". **Not yet worn** |
| 5 | Silent Cartographer's opening cutscenes drop frames | Measured (below): 13.6–16.1 ms of CPU a frame, over 90 Hz's budget. Needs the performance pass |
| 6 | Wish: anti-aliasing | Not started (MSAA on the eye targets would cost frame time that 90 Hz doesn't have yet) |

**120 Hz standard** (cross-port, 2026-10-05: 120 Hz at about 2160² an eye, falling back to 90
then 72). Measured unattended on 5a883495 at 1.25× (2160²), b30 (The Silent Cartographer), the
headset idle (paced at 45 fps, so only the CPU time per frame counts; its GPU may clock down
unworn):

| Scene | CPU a frame | of which |
| --- | --- | --- |
| Opening cutscene (3D screen) | 13.6–16.1 ms | game 5.0–5.9, render 4.7–6.3, end 3.8–4.0 |
| Gameplay at the start | 16.0–17.0 ms | game 4.9–5.7, render 6.8–7.5 (eyes 3.3–3.6 each), end 3.8–4.0 |

**Performance pass, step 1 (2026-10-05, 82b6d12e+): where the "end" goes.** b30 gameplay,
unattended, the runtime at 90 Hz (11.1 ms), 1.25× unless said:

| Run | fps | CPU a frame | releases | xrEndFrame | GPU wait |
| --- | --- | --- | --- | --- | --- |
| As built | 56 | 13.8–14.1 ms (game 5.1–5.5, render 4.7–4.8, end 3.9) | 2.65 ms | 0.70 ms | — |
| `vr.timing_gpu` (glFinish before the end) | 46–54 | 10.4–12.7 ms without the wait | 0.23 ms | 0.66 ms | 6–9 ms |
| `vr.flush_eyes` (glFlush after each eye) | 56–60 | 13.6–14.1 ms | 2.66 ms | 0.64 ms | — |
| 1.0× (1728²) | 61 | 13.5 ms | 2.69 ms | 0.65 ms | — |

- The "end" is SteamVR's GL binding waiting in `xrReleaseSwapchainImage` for the GPU to finish
  the frame: with the GPU waited on first, the releases take 0.23 ms. CPU and GPU never overlap
  across the frame's end.
- Flushing each eye doesn't help (the GPU isn't waiting for a flush; it's busy), and fewer pixels
  help only a little (56 → 61 fps): the GPU's time isn't mostly pixels. Mesa's driver threads
  (`gdrv0` nearly as busy as the game thread, `gl0`) suggest the draw calls themselves, through
  Zink, are the cost, on the CPU and the GPU alike.
- So the levers are fewer draw calls per frame (single-pass stereo would halve the eyes'; the
  per-pass state the game resends), then the game tick. simpleperf can't record in Lepton at
  the moment (`perf_event_paranoid` refused), so no per-function profile yet.

**Performance pass, step 2: single-pass stereo (2026-10-05, branch mv-core).** Both eyes in one
world pass (`vr.multiview`, on by default; false draws them one at a time):
- The window is drawn from between the eyes with a frustum covering both, so culling and
  visibility happen once for the pair (`vr_render.c`, `multiview_windows`).
- `rasterizer_set_frustum_z` gives the vertex programs each eye's camera: view 0's in
  c[-96..-89] as before, view 1's in `c_view1`. The translated programs (`nv2a_vsh.c`, by Codex
  Sol 6.1) choose them by `gl_ViewID_OVR`.
- `d3d8_gl.c` (by a Claude subagent) draws the back buffer into a two-layer target through
  `GL_OVR_multiview2`, with two-view program variants, and hands each layer to its eye. Reads of
  the back buffer during the pass see the left eye's layer. Two-view occlusion counts are
  halved. The hidden-area mask is not drawn in the single pass yet.

Measured unattended on b30, the runtime at 90 Hz, 1.25×:

| | Draws a frame | Target changes | fps | CPU a frame | end |
| --- | --- | --- | --- | --- | --- |
| Two passes | 1,180–1,230 | 21–23 | 55–85 | 11.5–14.8 ms | 3.1–3.5 ms |
| One pass | 540–720 | 16 | 67–89 | 10.2–12.7 ms | 1.9–3.0 ms |

The eye dumps from the single pass show each eye from its own camera (far scenery 97 px apart,
as the synthetic head's asymmetric fields give; near things more). Not yet worn.

**Next steps proposed (not started):**

1. *(Done as step 2 above.)* **Single-pass stereo** (`GL_OVR_multiview2`, offered by Zink here): both eyes in one pass, so
   the game issues each draw once, not twice. That halves the eyes' draw calls on the CPU and
   in Zink. The work: render the eyes into a two-layer image; give the translated NV2A vertex
   programs (`nv2a_vsh.c`) each eye's camera matrices, chosen by `gl_ViewID_OVR`; cull with
   one frustum covering both eyes; keep the per-frame state (fog, flares, sky) once. The largest
   change to the renderer so far: days, with a fallback to two passes.
2. Then the game's own 5 ms (the tick, interpolation, UI): needs a profile, so simpleperf
   working in Lepton first.
3. Anti-aliasing afterwards: `GL_EXT_multisampled_render_to_texture` is offered, the cheap
   kind of MSAA on a tiled GPU (resolved on the chip). Worth trying once frames have room.

Until then, HaloCEVR's refresh rate set to 72 Hz in SteamVR's per-app settings gives a full rate.

The budgets are 8.3 ms at 120 Hz, 11.1 ms at 90 and 13.9 ms at 72. Halo **does not hold 120,
or 90, on this open level** (a30 held 90 in an earlier wear), and needs a performance pass: the
3.9 ms "end" (SteamVR's GL binding, lesson 38), the per-eye passes and the game tick. Until then
the Frame's display is best at 72 Hz for open levels. A worn measurement should confirm this.

