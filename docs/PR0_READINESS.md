# PR0 — Halo CE VR

**EXPERIMENTAL.** This repository is already public. No binary release was uploaded during this review.

A clean build at `e642cad49bbca96e6a2b9c127a2da83a30a3c111` passed `configure.py --vr --lto=off --pgo=off` and `ninja -j4 android_apk`. Dependencies came from public URLs. The build used isolated shell, Java and Gradle homes and generated a temporary debug key; it needed no maps, private Git credentials or signing secrets. The ARM64 APK passes signature and 16 KiB ZIP alignment checks. Its hash is in [PR0_METADATA.json](PR0_METADATA.json).

The history audit found game-derived HUD and menu-header texture sheets under `port/assets/hud/` and `port/assets/titles/`. Their redistribution permission needs to be resolved or the files replaced with an extraction step using the player's own game data. The source's CC0 notice and the VR code's license do not grant rights to those game textures. No map files or ROMs were found; scanner key-marker hits were library source macros.

The [README](../README.md) records the current controls and user-confirmed 120 Hz operation at native 2160×2160 eye resolution. This build review did not change the installed game or run a new device session.
