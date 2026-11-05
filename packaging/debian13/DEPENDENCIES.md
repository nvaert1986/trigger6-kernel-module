# trigger6 1.0.4 - target: Debian 13 (trixie), kernel 6.12.x LTS

## Package `trigger6-dkms` (kernel module)
| Dependency | Type | Notes |
|---|---|---|
| dkms (>= 3.0) | Depends | Debian 13: 3.2.2 |
| make, gcc (>= 14) | Depends | Must match the compiler the kernel was built with (Debian 13: gcc-14) |
| linux-headers-amd64 | Depends/Recommends | Headers for 6.12.x (for example 6.12.107+deb13) |
| kmod | Depends | modprobe/depmod |
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
