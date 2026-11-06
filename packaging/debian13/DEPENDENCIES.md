# trigger6 1.2.0-beta3 - target: Debian 13 (trixie), kernel 6.12.x LTS

## Package `trigger6-dkms` (kernel module)
| Dependency | Type | Notes |
|---|---|---|
| dkms (>= 3.0) | Depends | Debian 13: 3.2.2 |
| make, gcc (>= 14) | Depends | Must match the compiler the kernel was built with (Debian 13: gcc-14) |
| linux-headers-amd64 | Depends/Recommends | Headers for 6.12.x (for example 6.12.107+deb13) |
| kmod | Depends | modprobe/depmod |
| ALSA in the kernel (`CONFIG_SND_PCM`) | Kernel | Default in Debian; needed for HDMI audio since 1.1.0-beta1, the display works without it |
| pahole | Recommends | Only for BTF generation by kbuild |
| udev (systemd) | Recommends | `71-trigger6.rules` |

Required kernel options (default on Debian): CONFIG_DRM, CONFIG_DRM_KMS_HELPER, CONFIG_DRM_GEM_SHMEM_HELPER, CONFIG_USB.

## Package `trigger6-fast` (user-space helper, optional but needed for ~60 fps)
| Dependency | Type | Notes |
|---|---|---|
| libc6 (>= 2.34) | Depends | pthread, dlopen in libc |
| libyuv0 | Depends | Loads `libyuv.so.0` with dlopen; Debian 13: 0.0.1904.20250204 |
| gcc, make | Build-Depends | Build with `Makefile.helper` |
| systemd | Recommends | User service `trigger6-fast.service` |
Without the helper the driver falls back to in-kernel conversion (~29 fps).

## Test facts
- Module compiled (no errors or warnings) against Debian `linux-headers-6.12.107+deb13-amd64` on Arch (cross-build). Only the BTF step failed, because of host tools. Linked: yes.
- Run and measured: only Arch 7.2.9, Xorg. Debian 13 itself was **not** run.
- udev rule and systemd user service: not tested on Debian.

## 0.9.0-r8 - additional checks
- Compiled against Debian `linux-headers-6.12.107+deb13-amd64` (compatibility path `T6_HAVE_DRM_SET_DMA_DEV`), against 6.18.55-lts and 7.2.9: no errors or warnings (only the BTF step failed, on the host machine).
- Run: Arch 7.2.9. Simulated unplug (USB de-authorisation) while writing: clean disconnect, module released, recovery after reconnecting.
- Not done: running on Debian 13, KASAN/lockdep kernel.

## 0.9.0-r9 - additional checks
- Compiled against 6.18.59 (Gentoo) with `W=1`: no errors or warnings. Not yet recompiled against Debian `linux-headers-6.12.107+deb13-amd64` and 7.2.9.
- No new dependencies. The `drm_edid` API now used for the EDID exists since kernel 6.3.
- Helper tested with libyuv: stops within about 20 ms on SIGTERM/SIGINT, also while it waits in `read()`.
- Not done: running on the adapter. See the test checklist in the changelog of `README.md`.

## 1.0.0 - additional checks
- Compared with the vendor's Windows driver (USB capture and `T6InDisp.dll`): init and mode-set sequence, interrupt endpoint 0x83, the adapter's own mode table and sync encoding. No new dependencies. New module parameter `keepalive_ms` (0 = off).
- Compiled against Debian 13 (kernel 6.12), Arch Linux (kernel 7.2) and 6.18.59 (Gentoo, `W=1`, no errors or warnings).
- Tested on the adapter: fully working under Wayland. Under Xorg the X server crashes (SIGSEGV) when the USB adapter is unplugged; unplugging only the HDMI cable works. Reported to Debian as an Xorg bug.

## 1.0.1 - additional checks
- No new dependencies.
- Compiled against 6.18.59 (Gentoo, `W=1`, no errors or warnings). The changes use no kernel interfaces beyond those of 1.0.0, which compiles on Debian 13 (kernel 6.12) and Arch Linux (kernel 7.2).
- Tested on the adapter with a Dell P2725HE under KDE Plasma (Wayland), Gentoo with kernel 6.18.59: picture, helper, mode switches, HDMI hotplug, flip-done events and one full start-up per plug-in. See the changelog in `README.md`.

## 1.0.2 - additional checks
- No new dependencies. Kernel module unchanged apart from its version number.
- udev rule and systemd user service tested on Gentoo with systemd and KDE Plasma (Wayland): the helper starts when the adapter is plugged in and ends cleanly (exit code 3, service inactive, no restarts) when it is unplugged. Not yet tested on Debian.

## 1.0.3 - additional checks
- No new dependencies. The changes use no kernel interfaces beyond those of 1.0.0.
- Compiled against 6.18.59 (Gentoo, `W=1`, no errors or warnings).
- Tested on the adapter with seven monitors under KDE Plasma (Wayland), Gentoo with kernel 6.18.59: iiyama PL2595W at its native 1920x1200, and Dell P2725HE, P2317H, P2726HE, P2319H, P2422H and HP E233 at 1920x1080. See the changelog in `README.md`.

## 1.0.4 - additional checks
- No new dependencies. The changes use no kernel interfaces beyond those of 1.0.0.
- Compiled against 6.18.59 (Gentoo, `W=1`, no errors or warnings).
- Tested on the adapter under KDE Plasma (Wayland), Gentoo with kernel 6.18.59: one output detected from the device information block (request `0xB3`), about 32 MB less memory, no idle USB polling, HDMI hotplug through interrupt events, `make load` / `make unload`, and six monitors kept their picture without keepalive (new default `keepalive_ms=0`).

## 1.0.5 - additional checks
- No new dependencies. The changes use no kernel interfaces beyond those of 1.0.0.
- Compiled against 6.18.59 (Gentoo, `W=1`, no errors or warnings).
- Tested on the adapter under KDE Plasma (Wayland), Gentoo with kernel 6.18.59: HP E233, iiyama PL2595W and Dell P2319H swapped on the running system; each monitor started on its own preferred timing, without a mode change by the desktop afterwards.

## 1.0.6 - additional checks
- No new dependencies and no change in behaviour: request `0x23` renamed from colour setup to audio volume.
- Compiled against 6.18.59 (Gentoo, `W=1`, no errors or warnings); the compiled code is identical to 1.0.5.

## 1.1.0-beta1 - additional checks
- HDMI audio as an ALSA sound card ("Trigger6 USB to HDMI Adapter"). No new packages; the kernel needs ALSA PCM (`CONFIG_SND_PCM`, standard in Debian). PipeWire (Debian 13 default) offers it as an extra sink.
- Compiled against 6.18.59 (Gentoo, `W=1`, no errors or warnings). Not yet compiled against Debian 13 (kernel 6.12), where the audio code uses `hrtimer_init()` instead of `hrtimer_setup()`.
- Tested virtually up to the USB transfers (sound card, `aplay`, PipeWire, usbmon capture); physically untested, as no monitor with speakers or an audio output was available.

## 1.1.0-beta2 - additional checks
- Cross-built against Debian 13's `linux-headers-6.12.107+deb13-amd64` with `W=1`: no errors or warnings. On 6.12 the audio code uses `hrtimer_init()` (6.12 has no `hrtimer_setup()`), vermagic `6.12.107+deb13-amd64`. Built with GCC 15 instead of Debian's GCC 14; not yet run on a Debian machine.

## 1.1.0-beta3 - additional checks
- No new packages. New kernel interfaces used: DRM ELD helpers (`drm/drm_eld.h`), colour management (`GAMMA_LUT`) and a cursor plane; all present in 6.12.
- Cross-built against Debian 13's `linux-headers-6.12.113+deb13-amd64` with `W=1`: no errors or warnings, vermagic `6.12.113+deb13-amd64`. Not yet run on a Debian machine.
- Compiled against 6.18.59 (Gentoo, `W=1`, no errors or warnings) and tested on the adapter (Dell P2725HE, KDE Plasma Wayland): hardware cursor, gamma table, EDID extension block, ELD and jack controls.

## 1.1.0-beta4 - additional checks
- No new packages and no new kernel interfaces (fixes for HDMI replugs only).
- Cross-built against Debian 13's `linux-headers-6.12.113+deb13-amd64` with `W=1`: no errors or warnings. Not yet run on a Debian machine.

## 1.1.0 - additional checks
- Runs and works on a Debian 13 machine (kernel 6.12), including the udev rule and the systemd user service.
- Packages `trigger6-dkms` (all) and `trigger6-fast` (amd64) built with `packaging/deb/build-deb.sh` in a Debian 13 container and installed there: DKMS built and signed the module for 6.12.115+deb13.1-amd64; `lintian` reports no errors.
- `dkms.conf` no longer has the `CLEAN` line (deprecated in DKMS 3).

## 1.2.0-beta1 - additional checks
- No new packages. New kernel interfaces used: DRM vblank (`drm_vblank_init`, `drm_crtc_handle_vblank`, `drm_crtc_arm_vblank_event`) and an hrtimer per output; all present in 6.12.
- Cross-built against Debian 13's `linux-headers-6.12.113+deb13-amd64` with `W=1`: no errors or warnings.
- Run on a Debian 13 machine (kernel 6.12.115, LXDE on Xorg) from the packages `trigger6-dkms` / `trigger6-fast` `1.2.0~beta1-1`: DKMS built the module, the helper runs; the adapter on a USB 2 port was detected as such and works (a bit laggy), no errors.

## 1.2.0-beta2 - additional checks
- No new packages. New kernel interfaces used: DRM damage helpers (`drm_plane_enable_fb_damage_clips`, `drm_atomic_helper_damage_merged`), `drm_crtc_vblank_crtc`, `drm_calc_timestamping_constants`, `drm_crtc_vblank_count_and_time` and the plane's async check/update hooks; all present in 6.12 (`kernel/Kbuild` handles the 6.13 change of `atomic_async_check`).
- Cross-built against Debian 13's `linux-headers-6.12.113+deb13-amd64` with `W=1`: no errors or warnings. Run on a Debian 13 machine (kernel 6.12.115) from the `1.2.0~beta3-1` packages: DKMS build and module signing worked; tested over USB 2, also with `vblank_timer=1`.

## 1.2.0-beta3 - additional checks
- No new packages and no new kernel interfaces (URBs and completions as already used for full frames).
- Cross-built against Debian 13's `linux-headers-6.12.113+deb13-amd64` with `W=1`: no errors or warnings. Run on a Debian 13 machine (kernel 6.12.115) from the `1.2.0~beta3-1` packages: DKMS build and module signing worked; tested over USB 2, also with `vblank_timer=1`.
