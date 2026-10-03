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
| 90 Hz | `vr.refresh_rate` defaults to 90, asked for at start (the Frame's display setting decides what is offered). The game thread needs about 7.3 ms of CPU a frame, against 90 Hz's 11.1 ms. **Not yet seen worn at 90.** If 90 is not held, the host falls back to 72 by itself (see Display rate). |
| Hand-aimed weapons | Default (`vr.aim = "hand"`); verified unattended with synthetic hands. **Not yet played.** |
| Arm IK | Default (`vr.arms = "ik"`); verified unattended. **Not yet played.** |
| 3D cutscenes on a screen, fades | Default (`vr.cinema_3d`); verified unattended. **Not yet seen worn.** |
| Vehicles: first-person seats, stick steering | Built, **not yet run on the device** (the headset went offline mid-test). `vr.vehicle_view` "first_person" (default) or "chase"; `vr.vehicle_steering` "stick" (default), "head" or "hand". |
| VR-native controls, gestures, left-handed play | Built (`vr.controls = "vr"`, default; `"pad"` keeps the Xbox mapping). **Not yet run on the device.** |
| Aim smoothing when zoomed, haptics | Built, **not yet run on the device.** |
| Picture-in-picture scope | Built (`vr.scope`), **not yet run on the device.** |
| Far HUD drawn from the head | Built (15 m away, 10 m wide), **not yet seen worn.** |
| Near and leash HUD modes (`vr.hud_mode`) | Built, **not yet run on the device.** |
| Menu laser pointer | Built, **not yet run on the device.** |
| Room-scale walking | Built (`vr.roomscale`, off by default), **not yet run on the device.** |
| VR settings in the pause menu | Built, **not yet run on the device.** |
| System layer (View: short press back, 1 s recentre, 3 s recentre and standing height; the both-sticks recentre removed) | Built, **not yet run on the device.** |
| Quit to Steam (confirm, then `xrRequestExitSession`) | Built, **not yet run on the device.** |
| `vr.settings_version`, range clamping of numeric `vr.*` settings, the old HUD defaults migrated | Built; the config code was exercised on a computer (a fresh file, a version-0 file, clamps, the environment), **not yet on the device.** |
| Touch and Index fallbacks: grenade and flashlight (grip with a stick click) | Built, **not yet run on the device.** |
| Recentre from the runtime (`XrEventDataReferenceSpaceChangePending`) | Built, **not yet run on the device.** |
| 72 Hz automatic fallback | In the host since the refresh request was added; **not yet seen to fire worn.** |
| Comfort options, foveation | Not started. |
| sfvr (the cross-port library, vendored): View timing, turning, the haptic queue, the `[vr-perf]` line, settings ranges | Built; sfvr's own tests pass in its repo and the config code was exercised on a computer, **not yet run on the device.** |
| `vr.turn_mode` / `vr.snap_angle` (settings version 2, `vr.snap_turn` migrated); `vr.hud_mode` and QUIT on the VR settings' second page; Index pause on a trackpad press | Built, **not yet run on the device.** |
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
  | `near` | 1.5 m away, 1.1 m wide (40°) | In LOCAL space at the head's position, turned by the head's yaw and pitch, never its roll (the pitch is held short of 80°) |
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
  QUIT TO STEAM. B goes back a page. `vr_menu_page` builds each page from the items marked
  with it.
- **QUIT TO STEAM** (the last item, on page 2) is not a setting. A asks ("SURE? A TO QUIT"); A again within
  five seconds calls `vr_request_exit`, which has the host call `xrRequestExitSession`. The
  frame loop already follows the session's STOPPING (the host ends it) and EXITING states to the
  exit in `frame_begin`, and `vr_request_exit` exits after three seconds regardless if they never
  come. Left, another item, or the time puts it back. It does not save: the game keeps what its
  own checkpoints have saved.
- **Changing a setting:** A or right steps it on, left steps it back, and B returns to the pause
  menu. The laser pointer clicks them too.
- **Saving:** each change is written into `config.toml` (`config_write_*`) and taken up at once
  (`vr_reload_settings`).
- **How the tags are added:** `cache_file_add_tag` puts tags made in memory after the map's own,
  in a copy of its tag table. A button's text comes from code (game data input function 41), and
  its changes run event handler functions 102 and 103, one past the end of each table.

**Menus' laser pointer** (`vr_ui_pointer`, behind `halo_ui_pointer_update`). The weapon hand
points at the screen the menus are on, as the desktop builds' mouse does: the flat screen, or
in stereo (the pause menu) the HUD's panel. Its trigger clicks (with a tick of buzz), the right
B goes back, and a dot on the reticle's layer shows where it points. The pad's buttons still work.

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

`enabled`, `stereo`, `resolution_scale` (1.5), `refresh_rate` (90), `world_scale` (0.328084 units/m),
`cinema_3d`, `cinema_separation`, `cinema_convergence`, `cinema_distance`, `cinema_width`, `arms`
("ik" / "hidden" / "animated"), `body` ("off" / "legs": the player's own legs and torso, headless
and armless, under the head; not yet worn),
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
  - The driver's right stick steers as in flat Halo, and your head only looks. Gunner seats aim
    with your head, and the handheld weapon is hidden in driver and gunner seats.
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
