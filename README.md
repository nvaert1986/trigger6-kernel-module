# MCT Trigger 6 (T6) USB display driver for Linux, release `1.2.0-beta1`

An out-of-tree Linux DRM/KMS kernel driver and a small user-space helper for the
MCT "T6 USB Station" chipset, as found in the **StarTech USB32HDES** USB 3.0 to HDMI adapter.
This release reaches about **60 new frames per second at 1920x1080** on the tested
setup, with a stable USB connection.

> **New in 1.2.0 (beta): partial updates and USB 2 support.** Only what changed on screen is sent, as rectangles
> into the hidden frame buffer followed by a flip, and an unchanged picture is not sent at all; on a USB 2 port
> every update goes as small rectangle writes. A simulated vblank is included but **off by default** (it made the
> desktop jerky). Partial updates are tested on the adapter over USB 3 and USB 2 (USB 2: works, a bit laggy). See the
> [1.2.0-beta1 entry](#120-beta1-2026-11-06); `partial_updates=0` gives the 1.1.0 behaviour.

> **New in 1.1.0: HDMI audio, hardware cursor, gamma table, full EDID.** The adapter also appears as a sound card
> ("Trigger6 USB to HDMI Adapter"), heard through a monitor's speakers and back by itself after an HDMI replug. The
> mouse pointer is drawn by the adapter (moving it no longer sends whole frames), and the monitor's full EDID and
> audio capabilities (ELD) are passed on. Tested on Gentoo, Debian 13 and Arch Linux, including DKMS with automatic
> module signing and suspend/resume. Packages for Debian/Ubuntu, Fedora, Arch Linux and Gentoo: see
> [Packages](#packages). Details in the [1.1.0 entry](#110-2026-11-06).

> **Status: experimental.** Tested on one adapter model (see
> [Tested configuration](#tested-configuration)). Version 1.1.0 works on Wayland and runs on Gentoo (kernel 6.18),
> Debian 13 (kernel 6.12) and Arch Linux (kernel 7.2). **On Xorg, unplugging the USB adapter crashes the X server** (a bug in Xorg, reported to
> Debian, see [Limitations](#limitations)); unplugging only the HDMI cable is fine. Use at your own risk.

---

## Contents

1. [Tested configuration](#tested-configuration)
2. [Results](#results)
3. [What is in this repository](#what-is-in-this-repository)
4. [How it works](#how-it-works)
5. [Requirements](#requirements)
6. [Packages](#packages)
7. [Build and install](#build-and-install)
8. [Running the fast helper](#running-the-fast-helper)
9. [Activating the monitor](#activating-the-monitor)
10. [Module parameters](#module-parameters)
11. [Troubleshooting](#troubleshooting)
12. [Limitations](#limitations)
13. [Changelog](#changelog)
14. [Origin, license and credits](#origin-license-and-credits)

---

## Tested configuration

| Item | Value |
|---|---|
| Adapter | StarTech **USB32HDES**, reported as `MCT Corp. T6 USB Station` |
| **USB ID** | **`0711:5601`** (`idVendor=0711`, `idProduct=5601`) |
| On-board memory reported by the device | 58 MB |
| USB link | Direct to a USB 3.0 port (5000 Mbit/s). The first tests through a USB 2.0 hub were not usable for high frame rates |
| Monitors | Dell P2725HE, 1920x1080 @ 60 Hz (all versions); with 1.0.3 also iiyama PL2595W (1920x1200), Dell P2317H, P2726HE, P2319H and P2422H, HP E233 (see the [1.0.3 entry](#103-2026-11-05)) |
| Host OS / kernel (`0.9.0-r8` measurements) | Arch Linux, `7.2.9-arch1-1`, Xorg |
| 1.0.0 hardware test | Works on Wayland. On Xorg it works until the USB adapter is unplugged: then the X server crashes (see [Limitations](#limitations)) |
| 1.0.0 builds | Arch Linux kernel 7.2, Debian 13 kernel 6.12, Linux 6.18.59 (`W=1`, no warnings) |
| 1.0.0 start-up test | Gentoo, Linux 6.18.59 with enforced module signatures: plug-in, chip init, mode table and unplug checked against a USB capture (no monitor attached yet) |
| 1.0.0 and 1.0.1 monitor test | Gentoo, Linux 6.18.59, KDE Plasma (Wayland), Dell P2725HE: picture, helper, mode switches, HDMI hotplug, flip-done events and `keepalive_ms=0`, checked against `t6_metrics` and a USB capture |
| 1.0.3 monitor test | Gentoo, Linux 6.18.59, KDE Plasma (Wayland), seven monitors from three brands: each detected on plug-in, set to its native mode, clean picture, no errors |
| 1.1.0-beta1 audio test | Gentoo, Linux 6.18.59, KDE Plasma (Wayland), PipeWire: sound card registered, playback through `aplay` and PipeWire, USB transfers checked with usbmon. **Not heard**: no monitor with speakers or audio output available |
| 1.1.0-beta3 audio test (by ear) | Gentoo, Linux 6.18.59, KDE Plasma (Wayland), PipeWire, iiyama PL2595W (2 x 2 W speakers, OSD Audio Source = HDMI): audio heard, 4532 packets with 0 underruns (`audio_fills=0`), no USB errors; ELD "PL2595W" with LPCM 2 ch 32/44.1/48 kHz 16/20/24 bit, jack plugged; after an HDMI replug during playback the audio came back by itself (engine restarted, still 0 underruns) |
| 1.2.0-beta1 USB 2 test | Debian 13 machine (kernel 6.12.115), LXDE on Xorg, Dell P2725HE on a USB 2 port, packages `1.2.0~beta1-1`: USB 2 detected automatically ("USB 2 link" in the kernel log), every update after the first frames sent as rectangle writes (551 of 561), no errors; usable for desktop work, noticeably laggy (about 13.6 ms of USB time per update, up to 100 ms for large changes) |
| 1.1.0 Debian 13 test | Runs and works on a Debian 13 machine (kernel 6.12), including the udev rule and the systemd user service |
| 1.1.0 Arch Linux test | Installed with DKMS and automatic module signing, builds without errors, works, including suspend/resume with the adapter connected |
| 1.1.0 packages | Built and install-tested in containers: Debian 13 (`.deb`, DKMS build for 6.12.115, signed), Fedora 45 (`.rpm`, DKMS build for 7.2.15, signed), Arch Linux (`PKGBUILD`, DKMS hook build for 7.2.14); `lintian`, `rpmlint` and `namcap` without relevant findings |
| 1.1.0-beta4 replug test | Gentoo, Linux 6.18.59, KDE Plasma (Wayland), Dell P2725HE: slow and fast HDMI replugs with the pointer on the monitor and on the laptop screen: one cursor and a clean picture each time, no errors |
| 1.1.0-beta4 builds | Linux 6.18.59 and Debian 13 `linux-headers-6.12.113+deb13-amd64` (cross-build, `W=1`, no warnings) |
| 1.1.0-beta3 test | Gentoo, Linux 6.18.59, KDE Plasma (Wayland), Dell P2725HE: EDID extension block read, ELD and jack controls, hardware cursor used by KWin, gamma table checked with a warm test table; feature level 8 |
| 1.1.0-beta3 builds | Linux 6.18.59 and Debian 13 `linux-headers-6.12.113+deb13-amd64` (cross-build, `W=1`, no warnings) |
| 1.1.0-beta2 builds | Linux 6.18.59 and Debian 13 `linux-headers-6.12.107+deb13-amd64` (cross-build, `W=1`, no warnings); on 6.12 the audio code uses `hrtimer_init()`, on 6.18 `hrtimer_setup()` |
| 1.0.5 monitor swap test | Gentoo, Linux 6.18.59, KDE Plasma (Wayland): HP E233, iiyama PL2595W and Dell P2319H swapped on the running system; each started on its own preferred timing, no mode change by the desktop afterwards |
| 1.0.6 builds | Linux 6.18.59 (`W=1`, no warnings); compiled code identical to 1.0.5, no new kernel interfaces |
| 1.0.5 builds | Linux 6.18.59 (`W=1`, no warnings); uses no kernel interfaces beyond those 1.0.0 needs, which builds on Debian 13 (6.12) and Arch Linux (7.2) |
| 1.0.4 builds | Linux 6.18.59 (`W=1`, no warnings); uses no kernel interfaces beyond those 1.0.0 needs, which builds on Debian 13 (6.12) and Arch Linux (7.2) |
| 1.0.4 keepalive test | Same setup, `keepalive_ms=0` on six monitors (Dell P2725HE, P2319H, P2726HE, P2317H, iiyama PL2595W, HP E233): picture kept for 59 to 88 s without a single frame |
| 1.0.4 single-output test | Gentoo, Linux 6.18.59, KDE Plasma (Wayland), Dell P2725HE: one output detected from the device information block, about 32 MB less memory, no idle USB polling, HDMI replug detected through interrupt events |
| 1.0.3 builds | Linux 6.18.59 (`W=1`, no warnings); uses no kernel interfaces beyond those 1.0.0 needs, which builds on Debian 13 (6.12) and Arch Linux (7.2) |
| 1.0.2 helper autostart | Gentoo with systemd, KDE Plasma (Wayland): udev rule and user service start the helper on plug-in and end it cleanly on unplug |
| 1.0.1 builds | Linux 6.18.59 (`W=1`, no warnings); uses no kernel interfaces beyond those 1.0.0 needs, which builds on Debian 13 (6.12) and Arch Linux (7.2) |

Check your own adapter with:

```bash
lsusb | grep -i 0711:5601        # or: cat /sys/bus/usb/devices/*/idVendor
```

Other T6-based adapters may work but are **not** validated. The code only binds to `0711:5601`.

## Results

Measured as *frames sent by the driver* while a test window changed colour at about 57 updates per
second on the adapter's output, with a simulated mouse moving over the screen:

| Version | Duration | Frames/s | Notes |
|---|---:|---:|---|
| RGB32, original-style transfer | 30 s | 20.79 | baseline |
| Kernel NV12 | 360 s | 28.78 | |
| NV12 + user-space SIMD conversion (serial) | 30 s | 35.96 | |
| + overlapped conversion/sending | 30 s | 49.58 | |
| + asynchronous USB (this driver, `0.9.0-r8`) | 360 s | **59.99** | 21 596 frames, 0 USB errors, no re-enumeration |

Caveats, stated plainly:

* The figure counts **frames submitted to the device**. It was not measured with a camera or an HDMI
  analyser, and it includes frames that differ only by mouse-cursor movement.
* With the helper stopped the driver falls back to in-kernel conversion at roughly **29 frames/s**.
* The refresh rate of the HDMI mode is 60 Hz, so 60 frames/s is the practical ceiling.

## What is in this repository

| Path | What it is |
|---|---|
| `kernel/` | The DRM/KMS kernel module `trigger6.ko` (sources, `Makefile`, `Kbuild`); `trigger6_audio.c` is the HDMI audio sound card |
| `userspace/trigger6-fast.c` | The user-space SIMD helper `trigger6-fast` (NV12 conversion with libyuv), see [section C](#c-install-the-helper) |
| `dkms.conf` | DKMS configuration, see [section B](#b-permanent-install-with-dkms-debian-13--ubuntu--arch) |
| `packaging/debian13/trigger6-fast.service` | systemd **user** service that runs the helper, see [section D](#d-device-permissions-and-autostart-recommended) |
| `packaging/debian13/71-trigger6.rules` | udev rule: gives the logged-in user access to the adapter and starts the helper service when its device appears |
| `packaging/debian13/trigger6.conf` | modprobe options file (`primary_userspace_nv12=1`) for `/etc/modprobe.d/` |
| `packaging/debian13/Makefile.helper` | Makefile for building the helper |
| `packaging/debian13/trigger6-fast.c`, `dkms.conf` | Identical copies of the helper source and `dkms.conf`, so the folder is a complete packaging input |
| `packaging/debian13/DEPENDENCIES.md` | Package dependencies and test notes for Debian 13 |
| `packaging/deb/build-deb.sh` | Builds the Debian packages `trigger6-dkms` and `trigger6-fast`, see [Packages](#packages) |
| `packaging/rpm/trigger6.spec` | Fedora packages `trigger6-dkms` and `trigger6-fast` |
| `packaging/arch/PKGBUILD` | Arch Linux packages `trigger6-dkms` and `trigger6-fast` (built for the release page) |
| `LICENSE` | GPL-2.0-only for the kernel module and the helper; MIT notice for upstream-derived user-space tools |

## How it works

The T6 is a USB "display server on a chip". It does not scan out the host's frame buffer. The host
pushes pixel data over USB bulk transfers into the adapter's own video memory, then sends a small
*flip* command telling the chip which memory block to display. Everything below is about making that
push as fast and as steady as possible.

### 1. Data path

```text
 Xorg / compositor
        |  (DRM dirty-framebuffer request, XRGB8888, 8 294 400 bytes per 1080p frame)
        v
 trigger6.ko  ---- exports the frame ---->  /dev/trigger6-<bus>-<dev>-out0-nv12
        ^                                          |
        |                                          v
        |                              trigger6-fast (user space)
        |                              libyuv ARGBToNV12, SIMD, ~4.3 ms
        |                                          |
        +---- NV12 frame (3 111 472 bytes) --------+
        |
        v
   USB bulk transfers (1 047 552-byte chunks, two URBs in flight)
        v
   T6 video memory  -- flip command -->  HDMI output
```

### 2. Use NV12 instead of RGB32 (the biggest single win)

A 1920x1080 frame in 32-bit RGB is **8.3 MB**. In **NV12** (4:2:0 YUV: a full-resolution luma plane plus a
half-resolution interleaved chroma plane) the same frame is **3.1 MB**, about 62 % less data on the USB link.
The chip converts NV12 to RGB itself. The cost is colour detail: luma stays at full resolution, chroma is
shared by every 2x2 pixel block, so very small coloured text can look slightly softer than with RGB32.
The kernel averages all four pixels of each block (instead of taking only one), which keeps edges clean.

### 3. Fast colour conversion in user space

Converting 2 million pixels from RGB to NV12 took about 17 to 23 ms with the original scalar C code in the
kernel, which alone limited the driver to about 29 frames/s. The helper `trigger6-fast` uses **libyuv**
(SIMD) and needs about **4.3 ms** per frame. Kernel code cannot comfortably use libyuv, so the design is:

* the kernel *exports* the latest XRGB frame to the helper through a character device,
* the helper converts it and *writes* the NV12 frame back,
* the kernel still owns **all USB traffic, framing and hardware addresses**; the helper never talks to USB.

The bridge validates every packet (magic, size, stride, format, length), allows only one helper at a time
(`EBUSY` otherwise), and takes a DRM device reference while open. A helper that is blocked in `read()` waiting
for the next frame counts as alive, however long the desktop has been idle. If the helper stops or stalls
(for example it is paused, or hangs in a conversion or write for more than 200 ms), the kernel
**falls back to in-kernel conversion automatically**, so the screen keeps updating (slower) instead of freezing.
When the helper returns, it takes over again. While the output is switched off, helper writes are refused
with `ESTALE`, which the helper counts and ignores.

### 4. "JPEG" and compression: what this driver actually does

The adapter also understands JPEG-compressed frames, and the Windows driver contains a JPEG path
(JPEG source format 13 decoded into NV12 on the chip side). This code base still contains that path (used by the
experimental *secondary* output, `jpeg_quality=`), **but the 60 fps primary output does not use JPEG**.
JPEG was not necessary to reach 60 fps here: NV12 already cuts the data by about 62 %, and the CPU cost of JPEG
encoding at 1080p60 would be higher than the SIMD NV12 conversion. The "compression" in this design is
therefore the **chroma subsampling of NV12**, not a lossy entropy coder.
The kernel only exports a frame when the desktop reports an update. Like the vendor Windows driver, it sends nothing
while the picture is static: six monitors kept their picture for 59 to 88 s without a single frame (1.0.4 entry in
the [Changelog](#changelog)). A periodic keepalive that resends the last frame is still available with
`keepalive_ms=2000` for a monitor that blanks a static picture.

### 5. Buffering ("dual/triple buffering") at four levels

1. **Device memory:** three NV12 frame slots (4 MiB each) in the adapter's video memory. Each new frame is written
   into the next slot with an increasing fence id and the flip command points the display at it, so the chip is
   never scanning out a half-written frame. This resembles the Windows driver's three-slot scheme. The adapter's
   flip-done events (interrupt endpoint 0x83) are read and counted (`flips_done`, `last_done_fence` in `t6_metrics`),
   but not yet used to pace frames, so perfect tear-free behaviour is not claimed.
2. **Helper:** three NV12 buffers, at most one finished frame waiting while another is being sent.
   A newer frame replaces a stale waiting one, which bounds latency instead of building a queue.
3. **Overlapped pipeline:** conversion of frame *N+1* (about 4 ms) runs while frame *N* is still on its way over USB
   (about 13 ms) in a separate writer thread. Throughput is set by the slower stage, not the sum.
4. **USB layer:** two bulk URBs, so the CPU copies the next 1 MiB chunk into a DMA buffer while the previous chunk is
   still on the bus, instead of waiting for each chunk to finish (`usb_bulk_msg`) before preparing the next.

### 6. Fewer copies per frame

Each frame used to be copied four times in the kernel (user buffer, staging buffer, USB chunk, keepalive cache).
Now the user buffer is copied straight into staging, USB chunks are copied while the bus is busy, and the
keepalive cache is exchanged by **swapping pointers** instead of copying 3 MB. In the helper, the 8 MB
frame-compare/copy for duplicate detection was removed (the kernel exports on desktop updates only, so a few duplicate frames may be sent).
Together this removed the stutter that appeared when the mouse moved.

### 7. Why the connection is stable

Early versions suffered from complete USB disconnect/re-enumerations. The root cause was **not proven**.
These measures were taken, and the long tests then ran without a reset:

* Use a USB 3.0 port directly. On USB 2 (since 1.2.0, tested) the adapter works, but at about a tenth of the
  bandwidth: desktop use works thanks to partial updates but feels a bit laggy, and full-screen motion (video,
  scrolling a whole page) is slow.
* Full frames are sent. The experimental partial VRAM writes (`primary_partial_updates`) are **off** by default.
* USB transfers follow the size the vendor's Windows code uses (`0xffc00` = 1 047 552 bytes).
* Bulk writes are serialised per device with a mutex, have a 5 s timeout, kill the URB and queue a device reset on a timeout.
* A valid EDID is cached rather than re-read on every hotplug poll, and polling no longer resets the active mode.
* The helper is separate from the kernel, so a crash of the helper cannot crash the USB path (fallback takes over).
* Verified: pausing the helper, killing it with `SIGKILL`, restarting it, and de-authorising the USB device
  mid-transfer all end cleanly (no oops, module unloads, display recovers on re-authorisation).

## Requirements

| Component | Debian 13 ("trixie", kernel 6.12 LTS) | Arch Linux |
|---|---|---|
| Kernel headers matching the running kernel | `linux-headers-amd64` | `linux-headers` / `linux-lts-headers` |
| C compiler (same major version as the kernel was built with) | `gcc` (>= 14), `make` | `gcc`, `make` |
| DKMS (for automatic rebuilds on kernel updates) | `dkms` (>= 3.0), `kmod` | `dkms` |
| libyuv runtime (helper) | `libyuv0` | `libyuv` |
| Build helper | `gcc`, `make` | `gcc`, `make` |
| Optional | `pahole` (BTF), `libyuv-dev` | |

Kernel options required (default on Debian and Arch): `CONFIG_DRM`, `CONFIG_DRM_KMS_HELPER`,
`CONFIG_DRM_GEM_SHMEM_HELPER`, `CONFIG_USB`. HDMI audio also needs `CONFIG_SND_PCM` (default on desktop kernels);
without it the module builds and works without the sound card.
Secure Boot systems must sign the module or enrol the DKMS key.

## Packages

Each release has two packages: **`trigger6-dkms`** (the kernel module, built by DKMS for every installed kernel
with headers, plus `/etc/modprobe.d/trigger6.conf`) and **`trigger6-fast`** (the helper, the udev rule and the
systemd user service). Install both, then plug in the adapter; the helper starts by itself.

* **Debian 13 / Ubuntu:** download `trigger6-dkms_1.1.0-1_all.deb` and `trigger6-fast_1.1.0-1_amd64.deb` from the
  [release page](https://github.com/nvaert1986/trigger6-kernel-module/releases/tag/v1.1.0), then
  `sudo apt install linux-headers-amd64 ./trigger6-dkms_1.1.0-1_all.deb ./trigger6-fast_1.1.0-1_amd64.deb`.
* **Fedora:** download the two `.rpm` files from the release page, then
  `sudo dnf install ./trigger6-dkms-1.1.0-1.fc45.noarch.rpm ./trigger6-fast-1.1.0-1.fc45.x86_64.rpm`.
* **Arch Linux:** download `trigger6-dkms-1.1.0-1-any.pkg.tar.zst` and `trigger6-fast-1.1.0-1-x86_64.pkg.tar.zst`
  from the release page, then `sudo pacman -S linux-headers` (or the headers of your kernel) and
  `sudo pacman -U ./trigger6-dkms-1.1.0-1-any.pkg.tar.zst ./trigger6-fast-1.1.0-1-x86_64.pkg.tar.zst`. Or build
  `packaging/arch/PKGBUILD` yourself with `makepkg -si`. There is no AUR package.
* **Gentoo:** the overlay [trigger6-overlay](https://github.com/nvaert1986/trigger6-overlay), package
  `x11-drivers/trigger6`; see its README.

With Secure Boot, DKMS signs the module with its own key (`/var/lib/dkms/mok.pub`); enrol it once with
`mokutil --import /var/lib/dkms/mok.pub`. Uninstalling `trigger6-dkms` removes the module from every kernel.

## Build and install

Get the source:

```bash
git clone https://github.com/nvaert1986/trigger6-kernel-module.git
cd trigger6-kernel-module
```

### A. Quick test (no installation)

```bash
cd kernel
make                                    # builds trigger6.ko for the running kernel (as your user)
sudo make load                          # loads the dependencies, then insmod with primary_userspace_nv12=1
sudo make unload                        # rmmod trigger6
```

`make load` takes other module parameters with `PARAMS`, for example
`sudo make load PARAMS="primary_userspace_nv12=1 keepalive_ms=2000"`. It exists because `insmod` loads only this one
file: if `drm_shmem_helper` is not loaded yet, a plain `insmod` fails with `Unknown symbol in module` (`dmesg` names
`drm_gem_shmem_dumb_create`). By hand, the same is
`sudo modprobe -a drm_kms_helper drm_shmem_helper && sudo insmod ./trigger6.ko primary_userspace_nv12=1`.
An installed module (section B) loads its dependencies by itself with `modprobe trigger6`.

On a kernel that only accepts signed modules (Secure Boot, `CONFIG_MODULE_SIG_FORCE`, lockdown), sign the module
before `insmod`, with the key your kernel was built with, and again after every rebuild:

```bash
sudo /lib/modules/$(uname -r)/build/scripts/sign-file sha256 <signing key .pem> <certificate .x509> trigger6.ko
modinfo -F signer trigger6.ko           # prints the key's name when signed
```

For a different kernel use `make KVER=<version>`. To unload: stop the helper, switch the output off (Xorg:
`xrandr --output <T6 output> --off`; Wayland: in the display settings), then `sudo make unload` (or `sudo rmmod trigger6`). Some desktops hold the device open. If `rmmod` says
"in use", first unbind it: `echo 2-2:1.0 | sudo tee /sys/bus/usb/drivers/trigger6/unbind` (use the interface
name shown under `/sys/bus/usb/drivers/trigger6/`) and retry after a second or two.

### B. Permanent install with DKMS (Debian 13 / Ubuntu / Arch)

```bash
sudo apt install dkms build-essential linux-headers-amd64 libyuv0   # Debian 13
sudo pacman -S --needed dkms base-devel linux-headers libyuv        # Arch (linux-lts-headers for the LTS kernel)

VER=1.2.0-beta1
sudo mkdir -p /usr/src/trigger6-$VER
sudo cp -r kernel dkms.conf LICENSE README.md /usr/src/trigger6-$VER/
sudo dkms add     -m trigger6 -v $VER
sudo dkms build   -m trigger6 -v $VER
sudo dkms install -m trigger6 -v $VER

sudo install -m 0644 packaging/debian13/trigger6.conf /etc/modprobe.d/trigger6.conf
sudo modprobe trigger6
```

`dkms.conf` is in the repository root (an identical copy is in `packaging/debian13/`) and is copied along as shown.
DKMS then rebuilds the module for each newly installed kernel. Remember that matching headers must be installed.
`trigger6.conf` sets `primary_userspace_nv12=1`, so the module uses the fast helper whenever it runs.
Check the installed version with `modinfo -F version trigger6` (prints `1.2.0-beta1`).

### C. Install the helper

```bash
cc -O3 -Wall -Wextra -pthread -o trigger6-fast userspace/trigger6-fast.c -ldl
sudo install -m 0755 trigger6-fast /usr/local/bin/trigger6-fast
```

`make -C packaging/debian13 -f Makefile.helper` builds the same binary (in `packaging/debian13/`) for packaging.
The helper loads `libyuv.so.0` (or `libyuv.so`) with `dlopen`, so no development package is needed at build time.

### D. Device permissions and autostart (recommended)

The bridge device `/dev/trigger6-<bus>-<dev>-out0-nv12` is created root-only. Install the udev rule and user service from
`packaging/debian13/`:

```bash
sudo install -m 0644 packaging/debian13/71-trigger6.rules /etc/udev/rules.d/
sudo install -m 0644 packaging/debian13/trigger6-fast.service /etc/systemd/user/
sudo sed -i 's#/usr/bin/trigger6-fast#/usr/local/bin/trigger6-fast#' /etc/systemd/user/trigger6-fast.service
sudo udevadm control --reload-rules
systemctl --user daemon-reload                     # as the desktop user, not as root
```

The service file starts `/usr/bin/trigger6-fast` (the path a distribution package would use); the `sed` line points
it at `/usr/local/bin`, where section C installs the helper. The udev rule gives the logged-in seat user access to the
adapter and asks systemd to start the helper service when the bridge node appears, so there is nothing to enable.
Unplug and replug the adapter once (or reboot) so the rule applies. Check it with
`systemctl --user status trigger6-fast`. When the adapter is unplugged, the helper ends with exit code 3, which the
service treats as a normal end (`SuccessExitStatus=3`): it shows as inactive and is not restarted until the adapter
returns. Crashes are still restarted after 2 s. Tested on Gentoo with systemd and KDE Plasma (Wayland), not yet on
Debian. Manual alternative for one session:

```bash
sudo setfacl -m u:$USER:rw /dev/trigger6-*-out0-nv12
trigger6-fast &
```

## Running the fast helper

```bash
trigger6-fast                     # auto-detects exactly one /dev/trigger6-*-out0-nv12
trigger6-fast /dev/trigger6-002-003-out0-nv12   # or give the node explicitly
```

It prints a status line every 30 s (`sent`, `converted`, `avg_convert_ms`, `avg_read_ms`, `avg_write_ms`). Stop it with
`SIGTERM`/`Ctrl-C`. The screen then continues on the slower in-kernel fallback. Exit codes: 0 after a stop signal,
3 when no adapter is present or it was unplugged, 1 for any other error.

## Activating the monitor

The adapter appears as an additional DRM/KMS output (for example `HDMI-1-3`). On Xorg:

```bash
xrandr                                                   # find the new output
xrandr --output HDMI-1-3 --mode 1920x1080 --right-of HDMI-2 --rate 60
```

Pick the output names from your own `xrandr` listing. On Wayland, use the desktop's display settings
(on KDE Plasma also `kscreen-doctor -o` to list outputs and `kscreen-doctor output.<name>.enable` to switch one on).
Check the driver's own counters while running:

```bash
cat /sys/bus/usb/devices/*:1.0/t6_metrics        # frames, bytes, bulk errors, timings
```

## Module parameters

| Parameter | Default | Meaning |
|---|---|---|
| `primary_userspace_nv12` | `0` (set `1`) | Primary output uses NV12 with the helper bridge; kernel conversion fallback stays active |
| `primary_nv12` | `0` | Pure in-kernel NV12 without helper (about 29 frames/s) |
| `partial_updates` | `1` | Send only what changed in the NV12 picture (rectangles into the hidden frame buffer, then a flip) and skip unchanged pictures; `0` sends every frame whole, as in 1.1.0. Can be changed at run time |
| `vblank_timer` | `0` | Experimental simulated vblank at the mode's refresh rate (load time only). Off by default: it made KDE Plasma run at about half the refresh rate on this adapter. `0` reports every frame as shown at once, as in 1.1.0 |
| `primary_partial_updates` | `0` | Old experimental partial VRAM writes for the raw RGB transport only. Off for stability |
| `frame_min_interval_ms` | `0` | Extra wait between frames (USB time still limits speed) |
| `jpeg_quality` | `40` | Only for the JPEG path of the experimental secondary head |
| `experimental_secondary_raw` | `0` | Experimental second output (only on adapters that report two outputs) |
| `serialize_usb_bus` | `1` | Serialise USB writes across devices |
| `manual_only` | `0` | Do not push frames automatically |
| `audio` | `1` | Offer HDMI audio as an ALSA sound card when the adapter supports it; `0` leaves it out (load time only) |
| `audio_lead_ms` | `90` | How far ahead of playback HDMI audio is sent (20-500 ms); also the latency the sound card reports |
| `keepalive_ms` | `0` | Resend the last frame after this many idle ms (100-10000); `0` (default since 1.0.4) sends nothing while the picture is static. Set `2000` (the old default) if a monitor blanks a static picture |

Set one at load time with `modprobe trigger6 name=value`.

## Troubleshooting

* **No output / black screen after plug-in:** re-activate the output with `xrandr` (Xorg) or the display settings (Wayland). After a re-enumeration the device number changes, so the helper must pick up the new node (it does when restarted).
* **Stutter when moving the mouse:** make sure `trigger6-fast` is actually running (`pgrep -a trigger6`). Without it you only get the in-kernel fallback.
* **`insmod: File exists` / `rmmod: in use`:** unbind the interface first (see section A).
* **`insmod: ... Unknown symbol in module`:** a dependency is not loaded. Use `sudo make load`, or run `modprobe -a drm_kms_helper drm_shmem_helper`
  first (see section A).
* **`insmod: ... Key was rejected by service`:** the kernel only accepts signed modules; sign it (see section A).
* **Build fails with an unknown DRM symbol:** please report the exact kernel version. The code detects `drm_atomic_commit` and `drm_dev_set_dma_dev` from the headers, but other API differences may exist.
* **Frequent disconnects:** connect the adapter directly to a USB 3.0 port, not a hub.
* **Xorg crashes (SIGSEGV) when the USB adapter is unplugged:** this is the X server failing on the removal of a
  video device, not a kernel crash; see [Limitations](#limitations). Use a Wayland session if the adapter is
  unplugged while the desktop runs. Unplugging only the HDMI cable is safe on both.
* **No sound from the monitor:** HDMI audio only plays while the adapter's output is on with a mode set, and the
  monitor needs speakers or an audio output. Pick the sink "Trigger6 USB to HDMI Adapter" (PipeWire may still call
  it "T6 USB Station Stereo") or ALSA card `Trigger6`; it is not made the default output. `dmesg` must show
  `HDMI audio: 48 kHz, 16 bit, stereo (sound card N, ...)`, and the `audio` line of `t6_metrics` counts the packets
  sent (`audio_packets`) and gaps filled with silence (`audio_fills`). A warning about less than 112 px horizontal
  blanking means the current mode leaves HDMI too little room for audio: choose another mode.
* **The monitor goes black or flickers while the picture does not change:** the adapter sends nothing while the
  desktop is static (since 1.0.4), and none of the tested monitors minded. If yours does, turn the keepalive back on:
  `echo 2000 | sudo tee /sys/module/trigger6/parameters/keepalive_ms` takes effect at once; for the next load add
  `keepalive_ms=2000` to the `options trigger6` line in `/etc/modprobe.d/trigger6.conf`. Please report the monitor.
* **Output disappears after a USB error** (`Halting T6 frame traffic on output 0` in `dmesg`): the driver re-enables
  the output automatically after about 60 s and puts the last picture back. A bulk timeout instead resets the USB
  device, which reloads the driver, so the helper must be restarted (the systemd user service does this).

## Limitations

* Validated: 1920x1080 @ 60 Hz on six monitors and 1920x1200 @ 60 Hz (the adapter's maximum) on one, plus the mode
  switches listed in the checklist. Other modes are offered but were not all tried.
* Offered modes: the built-in ones (1920x1080 @ 60/30, 1600x900, 1280x1024, 1280x800, 1280x720, 1024x768, 800x600,
  640x480) plus every other mode in the adapter's own mode table up to 1920x1200 and 60 Hz (on the captured adapter:
  1920x1080@50, 1360x768, 1366x768, 1440x900, 1680x1050, 1600x1200, 1920x1200 and more). The adapter is specified for
  at most 60 Hz, so the 75 Hz entries in its table are not offered. With a readable EDID, modes larger than any mode
  the monitor lists, or outside its refresh and pixel clock limits, are left out (item 28): on a 1080p monitor with a
  48-100 Hz range that removes 1920x1200, 1600x1200 and 1920x1080@30. The monitor's own detailed timings are offered
  as well, up to 1920x1200, 60 Hz and the fastest clock in the adapter's table (item 31).
* HDMI audio (1.1.0 beta): playback only (48 kHz, 16-bit, stereo), as the adapter reports. Heard on one monitor
  with built-in speakers (iiyama PL2595W, see [Tested configuration](#tested-configuration)); not tried on a TV or
  an AV receiver, or with other sample rates (the adapter offers only 48 kHz; PipeWire resamples). The monitor's
  audio capabilities (ELD) and a jack control are offered on the sound card (1.1.0-beta3); PipeWire currently uses a
  generic stereo profile for this card and does not read them.
* Gamma table (1.1.0-beta3): offered on feature level 8 adapters; KDE's Night Light does not use it on this output
  (it applies its colour change in shaders), so the table is used only by programs that set `GAMMA_LUT` or the
  legacy gamma ioctl.
* NV12 uses 4:2:0 colour, so tiny coloured text is slightly less sharp than RGB.
* The reported FPS is the number of frames sent. It is not a camera or analyser measurement of visible frames.
* Only the StarTech USB32HDES / `0711:5601` was tested. Other MCT T6 products (including the 4-output USB-C models in the upstream project) are not covered by these measurements.
* **Xorg: unplugging the USB adapter crashes the X server** with a segmentation fault when it handles the removed
  video device. Unplugging and replugging the HDMI cable works, and Wayland handles unplugging the adapter without
  problems. The driver removes the device the standard DRM way (`drm_dev_unplug()`), and the crash is in the X server,
  so it has been reported to Debian as an Xorg bug. Until that is fixed, use Wayland, or log out of the Xorg session
  before unplugging the adapter.
* Runs on Gentoo (6.18), Debian 13 (6.12) and Arch Linux (7.2), including the udev rule and the systemd user service;
  suspend/resume with the adapter connected tested on Arch Linux. Not tested with KASAN/lockdep kernels.
* Not tested: recovery from a USB transport fault (step 7, no clean way to cause one).

## Changelog

### 1.2.0-beta1 (2026-11-06)

**Beta: partial updates and USB 2 support; the simulated vblank is off by default.** Upgrading an existing DKMS
install: run `dkms remove trigger6/<installed version> --all`, then install `1.2.0-beta1` as in
[section B](#b-permanent-install-with-dkms-debian-13--ubuntu--arch). For Debian 13 / Ubuntu the release page has
`trigger6-dkms_1.2.0.beta1-1_all.deb` and `trigger6-fast_1.2.0.beta1-1_amd64.deb` (install as in
[Packages](#packages)). GitHub turns the `~` of the package version `1.2.0~beta1-1` into a dot in the file name;
the version inside stays `1.2.0~beta1-1`, so the final 1.2.0 sorts higher. The helper and its service are
unchanged.
If the picture misbehaves, set `partial_updates=0` (also at run time, in `/sys/module/trigger6/parameters/`) to get
the 1.1.0 behaviour back, and report it.

**Testing status, stated plainly:** builds without warnings (`W=1`) against Linux 6.18.59 and, as a cross-build,
against Debian 13's `linux-headers-6.12.113+deb13-amd64`; `checkpatch` without findings. Tested on the adapter
(0711:5601, USB 3) with a Dell P2725HE under KDE Plasma (Wayland), Gentoo, Linux 6.18.59: the 5601 accepts the
rectangle writes (mct-linux had verified them only on a 0711:5600; the vendor's Windows driver uses JPEG strips
on the 5601). Typing, scrolling, window moves and a static desktop showed a correct picture with no errors; in one
run 834 unchanged pictures were not sent, 360 went as rectangles (1,750 writes) and 597 whole, 2.0 GB over USB
instead of about 5.6 GB. Switching `partial_updates` off and on at run time kept the picture correct. **USB 2**
tested on a Debian 13 machine with the `.deb` packages: detected automatically, works, a bit laggy (item 49). The
simulated vblank was tested and is off by default (item 48).

47. **Partial updates** (`t6_send_prepared_nv12`, `t6_diff_bands`, `t6_write_rect`, `t6_send_nv12_rects`), ported
    from mct-linux and fitted to this driver's frame buffer layout (three 4 MiB slots per output, a 48-byte flip
    header and then the NV12 picture with its pitch rounded up to 16). The host keeps a copy of what each slot holds.
    A frame identical to the one on screen is not sent at all. Otherwise the row pairs that differ from the slot
    about to be written are merged into up to 16 bands (columns rounded out to 16 pixels) and written with
    rectangle writes (session type 7: 2n Y rows to the Y address, then n UV rows to the UV address, each write at
    most one 0x19000-byte fragment), followed by a flip without image data; the picture changes in one flip, never
    while it is written. When more than half of the picture changed, or the slot's content is unknown, the full
    flip of 1.1.0 is sent instead. Chip init, fault recovery, event `0x10`, a reconnect, a send error and a new
    mode size all mark the slot copies unknown. Both the helper and the in-kernel conversion use it; the helper
    still converts whole frames, so the saving is USB bandwidth, not CPU time. New `t6_metrics` line:
    `partial_updates`, `partial_frames`, `same_frames`, `rect_writes`, `rect_bytes`, `usb2`, `vblank`. Module
    parameter `partial_updates` (default on, changeable at run time).
48. **Simulated vblank** (`t6_vblank_fn`, `t6_crtc_enable_vblank`, `t6_crtc_send_event`). The adapter has no vblank
    interrupt and its flip-done event is not tied to the monitor's refresh, so until now every commit was reported
    as shown at once (`no_vblank`). A timer per output now runs at the mode's refresh rate and delivers the commit
    events at the next tick, as the kernel's virtual display drivers do, so compositors pace their frames to the
    refresh rate. The kernel's own vblank timer helpers are not used because 6.12 lacks them. Module parameter
    `vblank_timer`, **off by default**: on the adapter with KDE Plasma (Wayland) the desktop on the monitor ran at
    about half the refresh rate with it (cursor moves reaching the adapter at most ~35/s instead of ~53/s, window
    moves jerky too). A vblank sets a hard deadline, and a commit here copies the whole picture for the helper
    (on top of KWin's copy between the GPUs), so it often lands just after a tick and slips a whole frame;
    mct-linux's commits copy only the changed rows. Worth revisiting once the helper converts only the damaged
    areas.
49. **USB 2 support** (probe, `t6_connector_mode_valid_common`, `t6_send_nv12_rects`). On a high-speed link (480
    Mbit/s, about a tenth of USB 3) the driver now does what the vendor driver and mct-linux do there: no mode wider
    than 2048 pixels, and rectangle writes of at most 0x6400 bytes. Every update after the first frames of a stream
    goes as rectangles, also a whole picture, so desktop use only sends what changed. The kernel log names the
    link at plug-in. *Tested:* Debian 13 (kernel 6.12.115, LXDE on Xorg), Dell P2725HE on a USB 2 port, installed
    from the `1.2.0~beta1-1` packages: "USB 2 link: modes up to 2048 pixels wide, 25600-byte writes" at plug-in,
    551 of 561 frames as rectangles (the rest are the first frames of the stream), 2,806 unchanged pictures not
    sent, no send or USB errors. Usable for desktop work but noticeably laggy: about 283 KB and 13.6 ms of USB
    time per update on average, up to 100 ms for a large change. That is about 21 MB/s, roughly half of what USB 2
    can carry, because each rectangle goes as two transfers (header, then data) sent one after the other.

### 1.1.0 (2026-11-06)

**Final release of 1.1.0.** The code is 1.1.0-beta4; only the version and `dkms.conf` changed (the `CLEAN` line,
deprecated in DKMS 3, is gone). Upgrading an existing DKMS install: run `dkms remove trigger6/<installed version> --all`,
then install `1.1.0` as in [section B](#b-permanent-install-with-dkms-debian-13--ubuntu--arch), or use the
[packages](#packages).

What 1.1.0 brings over 1.0.x (items 37 to 46 below): HDMI audio as a sound card with volume control (item 37), the
monitor's full EDID (40), ELD and jack controls (42), a gamma table (43), a hardware cursor (44) and two HDMI replug
fixes (45, 46); 1.0.6 (item 39) is included.

**Testing status, stated plainly:** tested on the adapter on Gentoo (Linux 6.18.59, KDE Plasma Wayland; display,
audio by ear, cursor, gamma, replugs), on a Debian 13 machine (runs, udev rule and user service) and on Arch Linux
(DKMS with automatic signing, suspend/resume). New in this release: packages for Debian/Ubuntu (`.deb`), Fedora
(`.rpm`), Arch Linux (`.pkg.tar.zst`) and Gentoo (overlay), each built and install-tested in a container of
that distribution, including the DKMS build against its kernel headers.

### 1.1.0-beta4 (2026-11-05)

Two fixes for HDMI replugs; no new features. Upgrading an existing DKMS install: run
`dkms remove trigger6/1.1.0-beta3 --all` (or the installed version), then install `1.1.0-beta4` as in
[section B](#b-permanent-install-with-dkms-debian-13--ubuntu--arch). The helper and its service are unchanged.

**Testing status, stated plainly:** builds without warnings (`W=1`) against Linux 6.18.59 and, as a cross-build,
against Debian 13's `linux-headers-6.12.113+deb13-amd64`. Tested on the adapter with a Dell P2725HE under KDE Plasma
(Wayland) on Gentoo, Linux 6.18.59, per item below. Not yet run on a Debian machine; DKMS, suspend/resume and an
Arch Linux build are planned.

45. **Picture resent after an HDMI replug** (`t6_reprobe_work`). When a monitor is connected again while the
    desktop keeps the output on (a replug fast enough that the desktop never saw the monitor go), the chip init or
    timing switch reset the stream, and without a keepalive the monitor showed whatever the chip's frame slots held
    until the desktop redrew: a half-garbled picture after one HDMI replug on 1.0.5. The last frame is now sent
    again, RESET-flagged, as after event `0x10` and a fault recovery. *Tested:* five slow and two fast HDMI replugs,
    picture clean each time. In all of them KDE switched the output off and on itself and drew a fresh picture, so
    the resend was not needed (it counts as `keepalive_sent`); it covers a desktop that keeps the output on.
46. **Second cursor after an HDMI replug** (`t6_cursor_resync`, `t6_crtc_atomic_enable`). With the pointer on the
    monitor while its HDMI cable was unplugged, the old cursor came back on the monitor next to the real one on the
    other screen, until the pointer moved onto the monitor. The commit tail used here
    (`drm_atomic_helper_commit_tail_rpm`) skips plane updates and `atomic_flush` on an output that is off, so the
    desktop's "cursor off" with the output switch-off never reached the driver. When an output is switched on, the
    cursor's state is now taken from the DRM plane state, a cursor that is off gets an explicit hide (after a
    re-init the chip's cursor state is unknown), and the gamma table is checked against the CRTC state and uploaded
    if it changed meanwhile. *Tested:* Dell P2725HE, replugs with the pointer on the monitor and on the laptop
    screen: one cursor each time; the debug log showed the stale "on" replaced by the plane's "off" and a hide sent.

### 1.1.0-beta3 (2026-11-05)

Still a **beta** (not yet run on a Debian machine). **HDMI audio was heard with this release** after it was
published: see the audio test in [Tested configuration](#tested-configuration). New display features: a hardware
cursor, a gamma table and the monitor's full EDID. Upgrading an existing DKMS install: run
`dkms remove trigger6/1.1.0-beta2 --all` (or the installed version), then install `1.1.0-beta3` as in
[section B](#b-permanent-install-with-dkms-debian-13--ubuntu--arch). Includes `1.0.6` (item 39). The helper and its
service are unchanged.

**Testing status, stated plainly:** builds without warnings (`W=1`) against Linux 6.18.59 and, as a cross-build,
against Debian 13's `linux-headers-6.12.113+deb13-amd64`. Tested on the adapter with a Dell P2725HE under KDE Plasma
(Wayland) on Gentoo, Linux 6.18.59, per item below. The cursor, gamma and EDID requests were compared with the
vendor's Windows driver (`T6InDisp.dll`, `TrgIdd.dll`, `t6sta.sys`). Afterwards also tested with an iiyama PL2595W:
audio heard, ELD with its audio formats, cursor looks correct. Not tested: a dual-output adapter (second cursor and
gamma table), suspend/resume.


40. **EDID extension blocks are read** (`t6_read_edid_ext`, `t6_probe_head`, `t6_connector_get_modes`). Only the
    128-byte base block was read, and its extension count was cleared so DRM would not expect missing blocks. The
    driver now reads every extension block the monitor announces (up to three) with request `0x80`, wValue = byte
    offset (block x 128), as the vendor driver does, and passes the whole EDID to DRM. A block that fails to read,
    is empty or has a bad checksum is left out, and the base block's count and checksum are corrected to match. With
    the CTA-861 block DRM knows whether the monitor is HDMI or DVI and which audio formats it takes (the basis for
    ELD and jack reporting later). The offered modes are unchanged: they still come from the adapter's mode table and
    the base block. The kernel log reports the number of extension blocks per monitor.
    *Tested:* Dell P2725HE on Gentoo, Linux 6.18.59, KDE Plasma (Wayland): one CTA-861 block read, the connector's
    EDID is 256 bytes and passes `edid-decode -c` (warnings only from the monitor's own EDID), no errors.
41. **Volume at start-up checked** (`t6_audio_read_volume`). The adapter's volume read back as 31 of 31 after chip
    init, in the same layout as the `0x23` request. The "2 of 31" seen earlier was the desktop's own volume: PipeWire
    puts a new sink at 40% (-23.9 dB), which is 2 of 31 on this linear scale. The claim that the adapter powers up at
    volume 2 is removed from the code comment and item 37.
42. **ELD and jack controls for HDMI audio** (`t6_audio_eld`). The sound card now has an "ELD" control (the
    monitor's name and audio formats, built by DRM from the EDID, item 40) and an "HDMI/DP,pcm=0 Jack" control, as
    mct-linux and HDMI sound drivers offer them. The jack is plugged while a monitor with an EDID is connected. The
    vendor's Windows driver takes its jack state from interrupt event `0x20` instead, but the tested adapter sent that
    jack bit with no monitor connected at all, so it is not used. Whether PipeWire uses these controls depends on its
    profile for this card (it currently picks a generic stereo profile). *Tested:* Dell P2725HE: ELD "DELL P2725HE",
    jack plugged, both visible with `amixer -c Trigger6 contents`.
43. **Gamma table** (`t6_crtc_atomic_flush`, `t6_load_lut`). The CRTC offers a 256-entry `GAMMA_LUT` (and the
    legacy gamma ioctl through it), uploaded with `0x1D` and enabled with `0x1C`, and kept for every chip init. As
    in the vendor's Windows driver (`TrgIdd.dll`, `T6InDisp.dll`), the tables go to wIndex `0x100` = blue, `0x200` =
    green, `0x400` = red, and only on feature level 8 and up (request `0xB0` index 4, now read and logged at probe);
    below that no gamma request is sent and no gamma property offered (mct-linux saw them stall on a level 6
    adapter). *Tested:* feature level 8 on the tested adapter; a warm test table (red full, green 80%, blue 50%)
    turned the Dell warm, which confirms the channel order. KDE's Night Light currently does not use the table on
    this output (it applies its colour matrix in shaders), so it looks the same as before.
44. **Hardware cursor** (`t6_cursor_*`). A 64x64 ARGB cursor plane. As in the vendor's Windows driver, the image
    goes into one of five slots per output (`0x10`/`0x11`, 512-byte pieces), is selected with `0x05`/`0x07` and moved
    with `0x04`/`0x06` (top left of the image, negative values as v - 1). A new image goes into a slot that is not on
    screen, and shapes seen before are selected again without an upload (mct-linux). Moving the mouse sends one
    small control request instead of a frame. The cursor is sent again after a chip init, a mode set and event
    `0x10`; after two failed cursor requests in a row it stops until the next re-init, so it cannot drive the
    control error counter into a USB reset. The DRM minimum framebuffer size (640x480, inherited) is now 0: it
    rejected the compositor's 64x64 cursor buffers. *Tested:* KDE Plasma (Wayland) uses the plane: 73 cursor moves,
    2 uploads, 2 shapes reused from a slot, 27 frames sent while the mouse moved, no errors.

### 1.1.0-beta2 (2026-11-05)

Still a **beta: HDMI audio is tested virtually, physically untested** (see 1.1.0-beta1 below). Upgrading an existing
DKMS install: run `dkms remove trigger6/1.1.0-beta1 --all` (or the installed 1.0.x version), then install
`1.1.0-beta2` as in [section B](#b-permanent-install-with-dkms-debian-13--ubuntu--arch).

38. **Debian 13 (kernel 6.12) build confirmed** (`kernel/Kbuild`). Kernel 6.12 has no `hrtimer_setup()`; the audio
    code then uses `hrtimer_init()`, chosen at build time from the kernel headers. The check now matches only
    `hrtimer_setup()` itself, not the related `hrtimer_setup_on_stack()` and `hrtimer_setup_sleeper_on_stack()`, so a
    backport of only one of them cannot pick the wrong path. Cross-built against Debian 13's
    `linux-headers-6.12.107+deb13-amd64` (local GCC 15 instead of Debian's GCC 14): no warnings with `W=1`, the
    module uses `hrtimer_init`, vermagic `6.12.107+deb13-amd64`. Not yet run on a Debian machine. 1.1.0-beta1 also
    built correctly on 6.12, as those headers do not mention `hrtimer_setup` at all.

### 1.1.0-beta1 (2026-11-05)

**Adds HDMI audio support. Beta: tested virtually, physically untested.** The minor version goes up because this is a
new feature. Upgrading an existing DKMS install: run `dkms remove trigger6/1.0.5 --all`, then install `1.1.0-beta1`
as in [section B](#b-permanent-install-with-dkms-debian-13--ubuntu--arch). The display part and the helper are
unchanged since 1.0.5; to leave the sound card out, load the module with `audio=0`.

**Testing status, stated plainly:** builds without warnings (`W=1`) against Linux 6.18.59. Checked on the adapter
(StarTech USB32HDES) under KDE Plasma (Wayland) with PipeWire on Gentoo, Linux 6.18.59, **up to the USB transfers,
not by ear**: none of the available monitors has speakers or an audio output.

* The adapter reports one playback format, 48 kHz 16-bit stereo, a 128 KiB buffer and a volume node (0-31); the
  driver registers the sound card and PipeWire offers it as a sink (not as the default output).
* Recorded with usbmon while a 3 s 1 kHz tone played: `0x24` with activity 1 at the start, 32-byte session headers
  with signature 3 and payload 1920 bytes (10 ms), one per 10 ms, `0x24` with activity 0 at the end.
* With `aplay` directly on the card: 1 start, 300 packets for 3 s, no underrun. Through PipeWire, after the fix
  below: one start, no underrun in 195 samples of the stream state, no silence fills, the engine switched off when
  PipeWire suspended the sink. The picture kept running (no USB, send or event errors).

37. **HDMI audio** (`trigger6_audio.c`). The adapter has no USB audio interface; the vendor's Windows bus driver
    `t6sta.sys` presents an emulated USB Audio Class device to Windows and translates its stream into the T6
    protocol. The protocol was decoded and verified by ear on a 0711:5600 adapter by the
    [mct-linux](https://github.com/XXCoreRangerX/mct-linux) project (GPL-2.0-only), whose implementation this is a
    port of: playback runs over the same bulk pipe as video, a 32-byte session header with signature 3 followed by
    the PCM, paced by the host clock with 10 ms packets; `0x24` switches the engine on and off (activity, format
    index, 9600, cyclic buffer size); `0xA5` lists the formats, `0xA4` gives the engine state, `0xA2`/`0xA3` the
    volume node and `0x23` sets it. Cross-checked statically against `t6sta.sys`: it uses exactly these requests with
    the same lengths (`0x23`/`0xA3` 40 bytes, `0x24`, `0xA1`, `0xA2`, `0xA4` 16 bytes, `0xA5` n * 32 bytes), its
    answers for older firmware have the same layout (formats 44.1 and 48 kHz, 16-bit stereo), and the vendor capture's
    "audio off" request has the same `0x24` layout. The signature-3 header itself could not be located statically
    (the value is not an immediate); it is the part verified by ear in mct-linux.
    The driver offers an ALSA sound card "Trigger6 USB to HDMI Adapter" (card ID `Trigger6`, one PCM playback
    device, 48 kHz S16_LE stereo) with "PCM Playback Volume" and "PCM Playback Switch", sets the adapter's volume to
    31 of 31 at start-up as Windows and macOS do (read back as 31 of 31 on the tested adapter), restarts the engine
    after a chip re-init (which sends audio off), and reports `audio`, `audio_engine`, `audio_starts`, `audio_packets`, `audio_prefills` and
    `audio_fills` in `t6_metrics`. New module parameters `audio` and `audio_lead_ms`.
    **Change from mct-linux:** it took the whole 90 ms lead from the ALSA buffer at the start. PipeWire keeps about
    100 ms queued, so it was left with a few ms of margin and ran into an underrun every 0.4 s (20 engine restarts
    during a 3 s tone; `aplay` with its fuller buffer did not). The driver now sends the lead as silence first and
    takes the application's audio at the rate it plays; the lead is reported as latency.


### 1.0.6 (2026-11-05)

Maintenance release in the 1.0 series (branch `1.0.x`): a correction of names and documentation, **no change in
behaviour**. The compiled code is identical to 1.0.5; only the version string differs. The same fix is on `main` and
will be in the next 1.1.0 release.

**Testing status, stated plainly:** builds without warnings (`W=1`) against Linux 6.18.59; the machine code and
read-only data of `trigger6_drv.o` were compared with a 1.0.5 build and are identical. Not re-tested on the adapter,
as nothing it receives has changed.

39. **Request `0x23` is the audio volume, not a colour setting** (`t6_init_volume`, `T6_REQ_AUD_NODE_SET`, chip
    init). Inherited from upstream as `T6_REQ_SET_COLOR` / `t6_init_color`, and described as colour setup in items 17
    and 20. A vendor USB capture with Windows playing audio shows the vendor's audio driver sending exactly this
    payload to set the HDMI audio volume (audio node 0, channels left and right, 31 of 31), and mct-linux documents
    `0x23` as the audio node-value request. The driver still sends it at chip init, as the vendor driver does; only
    the names, comments and documentation were wrong. The colour-related requests in chip init are the gamma tables
    (`0x1C`/`0x1D`, item 20). In 1.1.0 the audio code uses the same request to set the volume (item 37).

### 1.0.5 (2026-11-05)

Upgrading an existing DKMS install: run `dkms remove trigger6/1.0.4 --all`, then install `1.0.5` as in
[section B](#b-permanent-install-with-dkms-debian-13--ubuntu--arch). The helper and its service are unchanged since
1.0.2. Includes everything from `1.0.4` below.

**Testing status, stated plainly:** builds without warnings (`W=1`) against Linux 6.18.59. Tested on the adapter
under KDE Plasma (Wayland) on Gentoo, Linux 6.18.59, by swapping monitors on the running system:

| Swap | First timing sent | Mode change by the desktop afterwards |
|---|---|---|
| HP E233 to iiyama PL2595W | 1920x1200 @ 60 Hz, the iiyama's own | none |
| iiyama PL2595W to Dell P2319H | 1920x1080 @ 60 Hz, the Dell's own (1.0.4: the iiyama's 1920x1200 first) | none (1.0.4: one) |
| Dell P2319H to iiyama PL2595W | 1920x1200 @ 60 Hz, the iiyama's own | none |

Each monitor showed its picture at once, without the black moment, and switching felt quicker; the brief desktop
freeze when connecting a monitor no longer occurred. No USB, send or unknown-event errors. After the release the
same-monitor case was checked as well: an HDMI replug of the Dell P2725HE showed no "new monitor" line and kept
1920x1080@60.

36. **A new monitor started on the previous monitor's timing** (`t6_adopt_monitor_timing`, `t6_probe_head`,
    chip init, `t6_reprobe_work`). Since 1.0.3 the driver restores the last programmed timing on reconnect, which
    is right when the same monitor is plugged back in, but a different monitor first got the previous monitor's
    timing: a 1080p Dell after the 1920x1200 iiyama received 1920x1200 and stayed black until the desktop set its
    mode a moment later. *Fix:* the driver compares the EDID it reads on a new connection with the previous one.
    For a different monitor (or the first one) it starts with that monitor's preferred timing, its first EDID
    detailed timing if the adapter can show it, otherwise 1080p60, using the identical built-in or table block
    when there is one so the desktop's mode set does not reprogram it. The same monitor plugged back in keeps the
    last timing. When the adapter was plugged in without a monitor, the first monitor only gets a new timing if it
    needs one (SET_RESOLUTION and enable, as a mode set). The desktop can still choose another mode afterwards.

### 1.0.4 (2026-11-05)

Upgrading an existing DKMS install: run `dkms remove trigger6/1.0.3 --all`, then install `1.0.4` as in
[section B](#b-permanent-install-with-dkms-debian-13--ubuntu--arch). The helper and its service are unchanged since
1.0.2. Includes everything from `1.0.3` below. Note the new default `keepalive_ms=0` (item 35).

**Testing status, stated plainly:** builds without warnings (`W=1`) against Linux 6.18.59. Tested on the adapter
(single HDMI output) with a Dell P2725HE under KDE Plasma (Wayland) on Gentoo, Linux 6.18.59: the driver reports
"Device information: 1 output (output 0: type 8, 30 modes; output 1 not present)" and "T6 display ready with 1
output"; only one DRM connector exists; the large-allocation total (`VmallocUsed`) was about 32 MB lower than with
1.0.3; no control request at all went to the adapter in 15 s with the monitor connected (1.0.3 polled both outputs
every 5 s); an HDMI unplug/replug was detected through the adapter's interrupt events; clean picture, helper
running, no errors. Event flag `0x10` (item 33) has not occurred in any test or in the vendor capture. `make load`
and `make unload` (item 34) were used for these tests. Six monitors kept their picture without keepalive (item 35).

32. **The absent second output was set up anyway** (`t6_read_device_info`, probe, reprobe, chip init). The driver
    always modelled two outputs: about 31 MB of buffers, a second DRM connector, CRTC and plane, a 0.5 s wait for
    a monitor on the missing output at every plug-in, and a status poll of both outputs every 5 s for as long as
    the adapter stayed plugged in, because output 1 never reported a monitor. Its "mode table" request returned
    unrelated data. The vendor driver reads a 112-byte device information block (request `0xB3`, signature
    `DISP`, `T6InDisp.dll` 0x1800fb480) and enables an output only when the low 4 bits of its u32 at byte
    88 + 4 * output are set; bits 8-15 give that output's mode table length. This adapter reports `0x00701e08` for
    output 0 (type 8, 30 entries) and 0 for output 1; the dual-output adapter in the vendor capture reports
    `0x04301e01` for output 1, which is why the vendor capture has two real mode tables. *Fix:* the driver reads
    the block at probe and only probes, allocates, registers and polls the outputs it lists, and reads exactly the
    reported number of mode table entries. Without a valid block it keeps the old two-output behaviour. The gamma
    table reset at chip init still covers both pipes of the chip, as tested.
33. **Event flag `0x10` was not handled** (`t6_int_event`, `t6_resend_work`). The vendor driver's handler
    (`T6InDisp.dll` 0x1800f5448) marks every active output "resend the current frame", the same flag it sets after
    a failed frame transfer, and releases threads that wait on the adapter. This driver never waits on the adapter,
    so only the resend applies: on `0x10` it resends the last frame of every active output, as after fault
    recovery, from a work item, logs "T6 event 0x10: resending the current picture" and counts it as `int_wake=`
    in `t6_metrics`.
34. **Quick test needed the dependencies loaded by hand** (`kernel/Makefile`). *Fix:* `make load` loads the
    modules `modinfo` lists as dependencies and then runs `insmod` with `PARAMS` (default
    `primary_userspace_nv12=1`); `make unload` runs `rmmod trigger6`.
35. **Keepalive on by default** (`keepalive_ms`). Since 1.0.0 the driver resent the last frame every 2 s on a static
    desktop, a full 3 MB frame over USB, although the vendor driver sends none (8 s without frames in its capture).
    Tested with `keepalive_ms=0` on six monitors, each left static after plug-in while the frame counters were
    sampled once a second:

    | Monitor | Mode | Longest stretch without any frame | Picture |
    |---|---|---|---|
    | Dell P2725HE | 1920x1080 | 88 s | kept |
    | Dell P2319H | 1920x1080 | 74 s | kept |
    | Dell P2726HE | 1920x1080 | 70 s | kept |
    | iiyama PL2595W | 1920x1200 | 82 s | kept |
    | Dell P2317H | 1920x1080 | 59 s | kept |
    | HP E233 | 1920x1080 | 75 s | kept |

    The Dell P2725HE had earlier kept its picture for 155 s as well. *Fix:* `keepalive_ms` now defaults to 0. The
    parameter stays: `keepalive_ms=2000` restores the old behaviour (see [Troubleshooting](#troubleshooting)).
    Resends after a transport fault, resume or event flag `0x10` do not depend on it.

### 1.0.3 (2026-11-05)

Upgrading an existing DKMS install: run `dkms remove trigger6/1.0.2 --all`, then install `1.0.3` as in
[section B](#b-permanent-install-with-dkms-debian-13--ubuntu--arch). The helper and its service are unchanged since
1.0.2. Includes everything from `1.0.2` below.

**Testing status, stated plainly:** builds without warnings (`W=1`) against Linux 6.18.59. Tested on the adapter on
Gentoo, Linux 6.18.59, KDE Plasma (Wayland), with seven monitors swapped on the running system (no reload in
between). Every monitor was detected on plug-in, got its native mode and showed a clean picture, with no USB, send
or unknown-event errors:

| Monitor | Native | Result with 1.0.3 |
|---|---|---|
| iiyama PL2595W | 1920x1200 | Native 1920x1200 @ 60 Hz with the monitor's own reduced-blanking timing (154 MHz); the monitor reports 1920x1200, 74 kHz, 60 Hz. With 1.0.2 it only got 1920x1080 |
| Dell P2725HE | 1920x1080 | Unchanged from 1.0.2: 1080p60 from the built-in timing, same mode list |
| Dell P2317H | 1920x1080 | 1080p60; 1920x1080@50 not offered (EDID range 56-76 Hz) |
| Dell P2726HE | 1920x1080 | 1080p60; 1920x1080@50 offered (EDID range 48-120 Hz) |
| Dell P2319H | 1920x1080 | 1080p60; 1920x1080@50 not offered (EDID range 56-76 Hz) |
| HP E233 | 1920x1080 | 1080p60; 1920x1080@50 offered (EDID range 50-60 Hz) |
| Dell P2422H | 1920x1080 | 1080p60; 1920x1080@50 not offered (EDID range 56-76 Hz) |

None of the 1080p monitors got 1920x1200 or 1600x1200 offered. On the six 1080p monitors the EDID's preferred timing
is identical to the built-in 1080p60 timing, so it is not offered twice.

31. **The monitor's own timings could not be used** (`t6_build_timing`, `t6_build_edid_timings`,
    `t6_head_find_timing`). A mode could only be set when a built-in timing block or the adapter's own mode table had
    one for that size and refresh rate. The iiyama PL2595W wants 1920x1200 @ 60 Hz with reduced blanking at 154 MHz
    and accepts at most 170 MHz; the adapter's table only has 1920x1200 @ 60 Hz at 193.25 MHz, which the EDID filter
    (item 28) correctly drops, so the monitor ran at 1920x1080. The vendor driver builds custom timing blocks for such
    modes (`T6InDisp.dll` 0x1800f5d90, clock helper 0x1800f7530): a 40 MHz reference, a VCO of 800-1600 MHz and a
    post divider of 4, 8, 16 or 32, so the pixel clock is 10000, 5000, 2500 or 1250 kHz * (IDIV + FNUM/1000), with
    the smallest divider that reaches 800 MHz; the other fields come straight from the timing, "reduced" is 0, and
    byte 31 is 2 for widths 1360 and 1680 and for widths above 1920 that are not a multiple of 32. *Fix:* the driver
    builds such a block for every detailed timing in the monitor's EDID that the adapter can show (progressive, up
    to 1920x1200, at most 60 Hz, no faster than the fastest mode in the adapter's own table), offers them after the
    built-in and table modes unless one of those has exactly the same timing, and marks the monitor's first detailed
    timing as preferred. Modes are now matched by their exact timing (clock, sizes, sync, totals) rather than size
    and refresh, so the iiyama's 1920x1200 gets its 154 MHz timing and not the table's 193.25 MHz one, and the
    driver remembers the last timing it programmed, so a re-init after replug restores exactly that timing. Checked
    offline: the generator reproduces all 30 entries of the adapter's table except for two expected differences: 11
    entries where the vendor's floating-point truncation makes FNUM one lower (at most 5 kHz; the driver uses the exact
    value) and the "reduced" byte the table sets on 1600x900. The misleading log line "the current reverse-engineered
    modeset path is fixed to 1920x1080@60" is replaced by one that reports the monitor's preferred size and how many
    of its timings are usable.

Swapping monitors: on reconnect the driver first restores the last programmed timing; the desktop then sets the new
monitor's mode a moment later. A monitor that cannot show the previous timing can stay black for that moment.

### 1.0.2 (2026-11-05)

Upgrading: install the new helper and `trigger6-fast.service` as in [section C](#c-install-the-helper) and
[section D](#d-device-permissions-and-autostart-recommended), then run `systemctl --user daemon-reload`. The kernel
module is unchanged apart from its version number; a DKMS install can stay on 1.0.1 or be replaced as in
[section B](#b-permanent-install-with-dkms-debian-13--ubuntu--arch). Includes everything from `1.0.1` below.

**Testing status, stated plainly:** tested on Gentoo with systemd, KDE Plasma (Wayland) and a Dell P2725HE. The udev
rule and user service start the helper on plug-in and give the logged-in user access to the adapter without manual
`setfacl`. After this fix, unplugging ends the helper once with exit code 3 and the service stays inactive (0
restarts; before, 8 restarts in about 16 s and counting). The same round confirmed checklist steps 2, 3, 8 and 9
(see the checklist below). Not yet tested on Debian.

30. **Helper service restarted endlessly after unplug** (`trigger6-fast.c`, `trigger6-fast.service`). When the adapter
    was unplugged, the helper exited with code 1, the service (`Restart=on-failure`) started it again every 2 s, and
    every new start failed at once with "Specify exactly one T6 NV12 device" because the device node was gone;
    `ConditionPathExistsGlob` did not stop these automatic restarts. *Fix:* the helper exits with code 3 when no
    adapter is present, when its device node is gone, or when reading or writing fails because the device was
    removed; other errors still exit with 1. The service treats code 3 as a normal end (`SuccessExitStatus=3`), so it
    is not restarted and does not show as failed. The udev rule starts it again when the adapter returns.

### 1.0.1 (2026-11-05)

Upgrading an existing DKMS install: run `dkms remove trigger6/1.0.0 --all`, then install `1.0.1` as in
[section B](#b-permanent-install-with-dkms-debian-13--ubuntu--arch). Includes everything from `1.0.0` below.

**Testing status, stated plainly:** builds without warnings (`W=1`) against Linux 6.18.59. The changes use no kernel
interfaces beyond those 1.0.0 already needs, and 1.0.0 builds on Debian 13 (6.12) and Arch Linux (7.2). Tested on the adapter with a Dell P2725HE under KDE Plasma (Wayland) on Gentoo,
Linux 6.18.59. With 1.0.0 the monitor test confirmed checklist steps 1, 6 (with 1920x1080@50 instead of @30), 11, 12
and 13: picture at 1920x1080@60, the helper taking over, 1280x720, 1920x1080@50, 1680x1050 and back,
HDMI unplug/replug with the monitor back within about 0.1 s, every frame sent answered by a flip-done event
(1417 of 1417), and 155 s without any frame and with `keepalive_ms=0` while the picture stayed. That test found
items 27 and 28. The 1.0.1 build was then checked on the same setup: flip-done events counted on output 0, plug-in
and HDMI unplug/replug counted as connection events, no `T6 event` log lines, one chip init per plug-in, and the
mode list without 1920x1200, 1600x1200 and 1080p30.

27. **Interrupt events decoded wrongly** (`t6_int_event`). Byte 19 was read as the flip command (3 = output 0,
    4 = output 1), so every flip-done event was counted for output 1 and `head0 flips_done` stayed 0; the
    "monitor connection changed" event was logged as unknown. The vendor driver's read handler (`T6InDisp.dll`,
    0x1800f7d70) drops events without bit `0x04` in byte 0 and reads byte 19 as flags: `0x04` = frame displayed
    (fence id at byte 12), `0x01`/`0x02` = monitor connection changed (it then re-reads the status of each output),
    `0x10` = wake every output. *Fix:* the same decoding. Flip-done events count for output 0 (output 1 only when it
    is the only active output); a connection change starts the status check at once instead of at the next 5 s poll
    and is counted in the new `int_hotplug=` field of `t6_metrics`; the `0x20` event the adapter sends after start-up
    and after every hotplug is dropped silently, as the vendor driver does; genuinely unknown events are logged with
    all 64 bytes instead of 32.
28. **Modes offered that the monitor cannot show** (`trigger6_connector.c`). The driver offered every mode the adapter
    can program, so a 1080p monitor was offered 1920x1200 and 1600x1200. *Fix:* with a readable EDID, a mode is only
    offered when the monitor lists a mode at least as large (established, standard or detailed timings of the EDID
    base block) and its refresh rate and pixel clock lie within the monitor's range limits. Smaller modes stay,
    because monitors scale them (1680x1050 worked on a 1080p monitor that does not list it). Without an EDID, or if
    nothing would be left, the full list is offered as before. On monitors with a range starting above 30 Hz this
    also removes 1920x1080@30.
29. **Chip init ran twice at plug-in** (`t6_reprobe_work`). Without a monitor detected yet, probe initialises output 0
    anyway; when the status poll then found the monitor, the whole init ran again 0.12 s later. *Fix:* the driver
    records which outputs the last init programmed and skips the second init when the monitor appears on one of
    them. An HDMI unplug clears that record, so a replugged monitor still gets a full init, as before.

#### Not changed in 1.0.1

* `keepalive_ms` stays at 2000. The adapter kept its picture without it on the tested monitor, but other monitors
  have not been tried.
* Output 1 is still modelled on this single-output adapter (about 31 MB and a status poll every 5 s).

### 1.0.0 (2026-11-04)

First public release, previously developed as `0.9.0-r10`. Upgrading an existing DKMS install: find the installed
version with `dkms status trigger6`, run `dkms remove trigger6/<old version> --all`, then install `1.0.0` as in
[section B](#b-permanent-install-with-dkms-debian-13--ubuntu--arch). Includes everything from `0.9.0-r9` below.

**Testing status, stated plainly:** builds on Arch Linux (kernel 7.2) and Debian 13 (kernel 6.12), and without
warnings (`W=1`) against Linux 6.18.59. Checked offline against the vendor driver's USB capture: the adjusted
`SET_RESOLUTION` for the adapter's 1360x768 entry matches the vendor request byte for byte, and all 30 table entries
of the captured adapter pass the table reader and the mode lookup. **Hardware test on the adapter:** fully working on
Wayland, including the changed init and mode-change sequence (items 20 and 21). On Xorg everything works except
unplugging the USB adapter, which crashes the X server with a segmentation fault (an Xorg bug, reported to Debian;
see [Limitations](#limitations)). Unplugging the HDMI cable works on both. Colours confirmed correct.
A start-up test on Linux 6.18.59 (adapter without monitor, recorded with usbmon) showed the new init sequence
(items 20 and 23) on the wire in the same order as the vendor capture, with every request accepted, 30 entries read
from the mode table (item 22), the interrupt endpoint running (item 18) and a clean unplug. That test also showed a
so far unknown interrupt event, type `0x20`, once after each start-up; it is logged as `T6 event` and is harmless.

#### Protocol changes

Each item below is backed by the USB capture of the vendor driver and/or static analysis of `T6InDisp.dll`.

18. **Interrupt endpoint 0x83 is now read** (`t6_int_*`). The vendor driver keeps a read pending and gets one 64-byte
    event per flip (type 4, the frame's fence at byte 12, the flip command at byte 19); this driver never read it.
    Flip-done events update `flips_done`/`last_done_fence` per output in `t6_metrics`. Other event types have not been
    seen yet: they are logged (`T6 event (...)`) and trigger an immediate status re-check, as they may be hotplug
    events. After 10 consecutive transfer errors the endpoint is left alone until resume. New module parameter
    `keepalive_ms` (default 2000, `0` = off): the vendor driver sends no keepalive at all (8 s without frames in the
    capture), so test whether the picture still blanks with `keepalive_ms=0` now that the endpoint is serviced.
19. **No NV12 startup frames** (reverts item 8). Only raw outputs are primed, as before 0.9.0-r9. The vendor driver sends
    no startup frames at all; its first bulk transfer is a real frame.
20. **Chip init follows the vendor order** (`t6_chip_init`). Audio volume first (`0x23`; described as colour setup
    until 1.0.6, item 39), LUT reset for both outputs (`0x1C` 0x0000/0x0100), one all-zero `0x31`, the per-output
    mode tables, `SET_RESOLUTION` and enable per output, HDMI audio off, then an identity gamma upload with commit per
    output (`0x1D` x3, `0x1C` (output << 8) | 2).
    Before: the volume after the resets, `0x31` per output, and two `0x24` requests that turned out to be **HDMI audio
    off/on** (payload: on/off, buffer 0x2580 bytes, 2 channels), so every init switched audio on without audio data;
    and `0x1C` 0x0002 committed output 0's gamma table without uploading one. `0x24` is now only sent as "off".
21. **Mode change = `SET_RESOLUTION` + enable** (`t6_crtc_atomic_enable`), as the vendor driver's mode-set routine
    does. The inherited sequence wrapped it in the two `0x24` audio requests, re-sent every output with `0x31` and
    committed output 0's gamma table.
22. **The adapter's own mode table is used** (`t6_read_mode_table`, `t6_head_mode_blob`, connector). Each output's
    table is read with `wValue` = output in 512-byte chunks, as the vendor driver does (the tested adapter reports 30
    entries); the unverified `0x84` count request is no longer sent. DRM modes are built from the exact table timing
    (no more CVT approximations), modes without a built-in blob are programmed from the table, and the monitor's
    preferred size from its EDID is marked preferred when the adapter supports it (fallback 1920x1080).
23. **Sync start/width encoding** (`t6_set_resolution`). `SET_RESOLUTION` expects a 1-based sync start: the vendor
    driver adds 1 to hsync and vsync start and subtracts 1 from both widths for every table entry before sending it
    (`T6InDisp.dll` at `0x1800f8f34`). Applying this to the adapter's 1360x768 entry reproduces the vendor driver's
    request in the capture byte for byte. All blobs keep the table convention and are adjusted when sent.

#### Found in the bug-check round of the new code

24. **Event-triggered status checks could run during probe** (`t6_int_event`). An unknown interrupt event arriving
    while probe was still running scheduled `reprobe_work` immediately, which registers misc devices and updates
    connectors that probe was still setting up (double `misc_register` of the same device). *Fix:* event-triggered
    checks only start once probe has finished.
25. **Unknown events were not rate-limited.** Each one scheduled an immediate status check; an unknown event type that
    arrives per frame would have meant a control transfer under `io_lock` at frame rate. *Fix:* at most one
    event-triggered check per second.
26. **`t6_head_enable` sysfs store** sent `MONITOR_CTRL` without `io_lock`, unlike every other control transfer.
    *Fix:* it takes `io_lock`.

#### Not changed

* Modes above 60 Hz: the adapter is specified for at most 60 Hz, so the 75 Hz entries in its own mode table are not
  offered.
* The second logical output is still allocated (about 31 MB) and polled every 5 s on single-output adapters. The
  USB ID does not say whether the board has one or two outputs, so this needs a probe-time capability check first.

#### Hardware test checklist (0.9.0-r9 to 1.1.0-beta2 changes)

Result so far: fully working on Wayland; on Xorg, unplugging the USB adapter crashes the X server (see
[Limitations](#limitations)). Confirmed on the adapter: steps 1, 2, 3 (the monitor went to standby and stayed
there, no frames sent while the output was off), 6 (1920x1080@50 instead of @30, which 1.0.1 does not offer on
monitors that do not accept 30 Hz), 8 (`systemctl --user stop` returned in 37 ms), 9 (the monitor reports 1920x1080
at 60 Hz, 24-bit), 10, 11 (with 1.0.1: `head0 flips_done` follows `sent_frames` plus `keepalive_sent`), 12 (picture
kept for 155 s without frames) and 13, plus HDMI unplug/replug of the monitor. Not tested: steps 4 and 5 (no
suspend on the test machine), 7 (no clean way to cause a transport fault) and 14 (nothing connected to the
monitor's audio output). Since 1.0.1 no `T6 event`
lines are expected around start-up or hotplug; any that appear are genuinely unknown events. The steps that use `xrandr` are for Xorg; on Wayland, use the desktop's display
settings (on KDE Plasma: `kscreen-doctor`) instead.

1. Plug in with `primary_userspace_nv12=1`: desktop after login. `dmesg` shows `Output 0: device mode table has N
   entries` (item 22) and no `T6 event` lines on a static desktop.
2. With the helper running, idle the desktop, then type: the `head0 encode_*` timings in `t6_metrics` must not move,
   because only kernel-side conversion records them (item 7).
3. `xrandr --output <T6> --off` while the picture is changing, wait 10 s: the monitor stays in standby (item 3).
4. Suspend and resume with the output on: no black or garbage flash every 1.5 s, hotplug still works (items 4, 9).
5. Switch to 1280x720, then suspend/resume and unplug/replug the monitor: the mode stays correct (item 5).
6. Switch between 1080p60 and 1080p30, or 1080p50 if the monitor does not accept 30 Hz (items 5, 28).
7. Whenever `dmesg` shows `Halting T6 frame traffic`, the output must come back within about 60 s (item 1). There is
   no clean way to force such an error; de-authorising the device is a disconnect, not a transport fault.
8. `systemctl --user stop trigger6-fast` on a static desktop returns at once (item 14).
9. 1920x1080@60 shows a stable picture; the monitor's OSD reports 1920x1080 60 Hz. If available, also try a monitor
   that showed "No Signal" before (item 16).
10. Colours look right: compare with the same image on another output.
11. `t6_metrics`: `int_running=1`, and `head0 flips_done` grows with `sent_frames` (item 18). Report any `T6 event`
    lines in `dmesg`, especially around monitor unplug/replug: they are unknown event types.
12. Leave a static desktop for a few minutes with the default `keepalive_ms=0`: does the picture blank (items 18, 35)?
13. `xrandr` lists the adapter's modes (for example 1920x1080@50, 1680x1050); switch to a few and back (items 21-23).
14. With a monitor that has speakers or an audio output: play a test tone to the "Trigger6 USB to HDMI Adapter" sink;
    it must sound clean, without dropouts, also while windows move (items 20, 37).

### 0.9.0-r9 (2026-11-04): review fixes on top of `0.9.0-r8`

Fixes from a code review of `0.9.0-r8` (not published separately). The frame rates in [Results](#results) were
measured on `0.9.0-r8` and have not been re-measured since.

**Testing status, stated plainly:** at release, built without warnings (`W=1`) against Linux 6.18.59 only (the new
DRM calls, `drm_edid_alloc()` and friends, exist since 6.3). The helper's new stop logic was tested (SIGTERM/SIGINT
while blocked in `read()`: exits in about 20 ms). Nothing was run on the adapter as `0.9.0-r9`; the changes below were
hardware-tested as part of `1.0.0` (see its testing status above).

#### High severity

1. **Transport faults never recovered** (`trigger6_drv.c`: `t6_trip_transport_fault`, `t6_reprobe_work`).
   A bulk transfer error marked the output faulted and logged "auto-recovery scheduled in 60s", but the recovery
   code only cleared a device-wide flag that nothing ever set. One USB error (stall, protocol error, short write)
   left the monitor disconnected until replug or module reload.
   *Fix:* the fault time is recorded per output; the reprobe worker clears faults older than 60 s, restarts the
   stream with RESET-flagged frames, sends a hotplug event and puts the last picture back on screen.
2. **Kernel oops when probe failed** (`t6_usb_probe` error path).
   The error path called `drm_encoder_cleanup()`, `drm_crtc_cleanup()` and `drm_plane_cleanup()` on objects that
   were never initialised, for example after a chip init failure on a flaky link. `drm_encoder_cleanup()` crashes on
   a zeroed encoder. *Fix:* the manual cleanup is removed. `drmm_mode_config_init()` already cleans up every
   initialised DRM object when the device is released. `reprobe_work` is now initialised before the first error exit.

#### Medium severity

3. **Use-after-free and monitor wake-up when the output is switched off** (`t6_crtc_atomic_disable`, bridge write).
   Turning the output off freed the cached keepalive frame without `io_lock`, while a helper write could be swapping
   that same buffer into `jpeg_staging`. A helper write that finished after switch-off also re-armed the 2 s keepalive,
   which wakes the monitor again.
   *Fix:* new per-output `crtc_active` flag, set and cleared under `io_lock`. Switch-off clears it first, so every
   later send (kernel worker, helper write, JPEG feeder, boot keepalive) is dropped. The cache is freed under
   `io_lock`. Helper writes to a switched-off output return `ESTALE`.
4. **Stale frame every 1.5 s after suspend/resume** (`t6_usb_suspend`, `t6_usb_resume`).
   Resume restarted the boot keepalive, which only CRTC enable/disable stopped, and after resume neither happened.
   Raw outputs got the black priming frame written over the desktop every 1.5 s. NV12 outputs got a header pointing
   at another VRAM slot (garbage after resume). Resume also never restarted monitor hotplug polling.
   *Fix:* suspend/resume now use `drm_mode_config_helper_suspend()/resume()` (as `udl` and `gm12u320` do), so the
   compositor's state is switched off and restored through the normal enable path, which sends a fresh frame.
   The boot keepalive only runs for outputs the compositor never enabled. Hotplug polling restarts after resume.
   If chip re-init fails on resume, a USB reset rebinds the driver.
5. **Re-init forced 1080p** (`t6_chip_init`).
   Chip init always programmed 1920x1080, but the driver kept the user's mode, so after resume or a monitor replug a
   non-1080p mode showed garbage. *Fix:* chip init programs each output's current mode. Related: switching between
   1080p60 and 1080p30 was ignored (same size), mode changes are now detected by timing, and the refresh rate is stored.
6. **Modes offered that could not be programmed** (`trigger6_connector.c`).
   When the adapter reported its own mode table, every entry was offered and accepted, but only modes with a
   built-in timing table can be programmed. Others (for example 1920x1200) left the chip at 1080p with a cropped
   desktop. *Fix:* such entries are skipped and `mode_valid` accepts only modes with a timing table (1 Hz slack,
   because DRM reports CVT 640x480@60 as 59 Hz). The fallback that ignored the refresh rate is removed.
7. **Helper treated as dead after any 200 ms pause** (`trigger6_bridge.h`).
   Liveness was the time of the last read/write, so after any idle pause the first update (a keystroke, a cursor
   blink) was converted by the kernel (about 20 ms under `io_lock`) and by the helper, and sent twice.
   *Fix:* a helper blocked in `read()` waiting for a frame counts as alive. The 200 ms rule now only catches a helper
   that is stopped or hangs while converting or writing.

#### Low severity

8. **NV12 outputs were never primed** (probe). The startup black frame was always 8.3 MB RGB32, which does not fit the
   3.4 MB NV12 buffer, so the recommended `primary_userspace_nv12=1` setup silently got no priming frames and no boot
   keepalive. *Fix:* NV12 outputs are primed with an NV12 black frame (Y=16, Cb=Cr=128) in VRAM slot 0.
   **Reverted again, see item 19:** the vendor driver primes nothing.
9. **Keepalive wrote to the wrong slot after re-init.** The keepalive resent the cached NV12 frame to the current slot
   index, which chip init resets, while the cached header points at the slot it was sent to. *Fix:* the slot address
   is stored with the cached frame (`last_frame_addr`) and the resend after recovery carries the RESET flag.
10. **EDID** (`t6_probe_head`, `t6_connector_get_modes`). Only the 128-byte base block is read, but it was passed to
    DRM with the monitor's extension count, so DRM saw an all-zero extension block, or read past the 256-byte buffer
    with two or more extensions. Reprobe also rewrote the buffer while `get_modes` could read it.
    *Fix:* EDID is read into a local buffer and published under a spinlock; `get_modes` uses a snapshot with the
    extension count cleared and the checksum fixed, through the `drm_edid` API.
11. **Deprecated JPEG feeder device** (`secondary_userspace_jpeg=1` only). No DRM reference was held while open
    (use-after-free after unplug), reads could return half-updated frames, and `poll` always reported data ready.
    *Fix:* open/release with a DRM reference, copies under `tx_lock` (also taken by the compositor-side swap), and
    per-file "new frame" tracking for `read` and `poll`. Writes to a switched-off output return `ESTALE`.
12. **Plane source offset ignored.** Frames were always copied from the framebuffer origin. *Fix:* the plane's
    `src` offset is applied.
13. **Mode change programmed the absent second output.** On the single-output USB32HDES every mode change also sent
    SET_RESOLUTION/SET_READY to output 1. *Fix:* only connected outputs are reprogrammed, as chip init already did.
    Superseded by item 21: a mode change now reprograms only the output being changed.
14. **Helper stop race** (`userspace/trigger6-fast.c`, also `packaging/debian13/`). A SIGTERM arriving just before
    `read()` left the helper blocked until the next frame, so `systemctl stop` could hang up to 90 s on a static
    desktop. *Fix:* a signal thread `sigwait()`s for SIGTERM/SIGINT (and SIGUSR1 from the writer thread on error)
    and keeps interrupting the reader with SIGUSR2 until it has left its loop.
15. Docs: `DEPENDENCIES.md` header said `0.9.0-r7`; the README claimed `dkms.conf` only ships in `packaging/debian13/`.

#### Timing and volume data (also inherited from upstream `mct-t6-linux`)

16. **Corrupt 1080p timing** (`t6_mode_1080p`, `t6_mode_1080p30`). Both blobs had hsync_start 1816 and vsync_start
    1000, which lie inside the visible 1920x1080 area, and a vsync width of 29 lines. Chip init sends the 1080p60
    blob at every start. *Fix:* the sync fields now hold the CEA-861 values (hsync 2008/44, vsync 1084/5). The new
    1080p60 blob is byte-identical to the 1920x1080@60 entry in the adapter's own mode table (request `0x89`).
    Tolerant monitors such as the tested Dell accepted the old values; stricter ones may have shown "No Signal".
17. **Volume request shifted by one byte** (`t6_init_volume`, then called `t6_init_color`, request `0x23`; described
    as colour setup until 1.0.6, see item 39). The vendor driver sends ten
    little-endian u32 values (3, 0, 0x1f, 0x1f, then six times 0x0f). The inherited copy lacked the leading `0x03`
    byte, so every field was misaligned. *Fix:* the blob is now byte-identical to the vendor driver's request.

Evidence: `trigger/upstream-trigger6/captures/mctt6.pcapng`, a USB capture of a 0711:5601 under the vendor
Windows driver (2023 driver stack), plus static analysis of the 2025 `T6InDisp.dll`. The 1600x900 and 1280x800 blobs
differ slightly from the adapter's table but were kept: ours match the standard CVT-RB timing (59.98 Hz instead of
60.37 Hz) and the exact 83.5 MHz pixel clock. The other five built-in blobs are byte-identical to the adapter's table.

## Origin, license and credits

### Whose package this is based on

This module is a **modified fork of `mct-t6-linux` by Ralph Friedman / Authentra**
(<https://github.com/ramifriedman/mct-t6-linux>), upstream base commit `e2aca2c`.
All credit for the driver architecture, the DRM/KMS implementation, the T6 protocol handling, the codec code and the
original companion tools goes to the original author. The copyright notice
`Copyright (C) 2026 Authentra / Ralph Friedman` is kept unchanged in the kernel sources
(`trigger6_drv.c`, `trigger6_connector.c`, `trigger6_codec.c`, `trigger6_codec.h`, `trigger6.h`), as is `MODULE_AUTHOR`.

### Upstream's own credits (carried over)

* The T6 protocol was decoded from [mcttrigger/triggerdm](https://github.com/mcttrigger/triggerdm), the GPL ChromeOS driver by **Magic Control Technology Corp.** (MCT).
* Reverse-engineering documentation: [cyrozap/mct-usb-display-adapter-re](https://github.com/cyrozap/mct-usb-display-adapter-re).
* The driver architecture follows the Linux UDL (USB DisplayLink) pattern.
* The upstream README lists as contributors: Authentra, Claude Opus 4.6 (Anthropic), Codex 5.4 (OpenAI) and Gemini (Google).

* HDMI audio (1.1.0-beta1) is a port of the implementation in [mct-linux](https://github.com/XXCoreRangerX/mct-linux)
  (GPL-2.0-only), which decoded and verified the T6 audio protocol.

### What this fork changes

* Kernel 7.2 and Debian 13 / 6.12 DRM API compatibility (`drm_atomic_commit`, `drm_dev_set_dma_dev`) and Xorg dirty-framebuffer support.
* Stability changes (no partial VRAM writes by default, EDID caching, stable active mode, per-device bulk mutex, keepalive-buffer fixes).
* Primary-output NV12 transport, 1 047 552-byte USB chunks, three device-side frame slots.
* The user-space SIMD bridge (`trigger6_bridge.h`, `trigger6-fast`), the asynchronous two-URB USB path and the removal of per-frame copies.
* `0.9.0-r9`: review fixes for fault recovery, probe error handling, output switch-off, suspend/resume, mode handling and the helper, plus corrected 1080p timing and volume request data.
* `1.0.0`: init and mode-set sequence, interrupt endpoint, the adapter's own mode table and sync encoding (see [Changelog](#changelog)).
* `1.2.0-beta1`: partial updates and USB 2 support (both tested), simulated vblank (off by default).
* `1.1.0`: final release of the 1.1.0 betas; packages for Debian/Ubuntu, Fedora, Arch Linux and Gentoo.
* `1.1.0-beta4`: HDMI replug fixes (second cursor, picture resend).
* `1.1.0-beta3`: hardware cursor, gamma table, EDID extension blocks, ELD and jack controls.
* `1.1.0-beta2`: Debian 13 (6.12) build of the audio code confirmed.
* `1.1.0-beta1`: HDMI audio as an ALSA sound card (beta: tested virtually, physically untested), ported from mct-linux and adapted for PipeWire.
* `1.0.6`: request `0x23` correctly named as the audio volume (no change in behaviour).
* `1.0.5`: a different monitor starts on its own preferred timing instead of the previous monitor's.
* `1.0.4`: only the outputs the adapter reports (request `0xB3`), so single-output adapters no longer carry a second output; event flag `0x10` resends the picture; keepalive off by default; `make load` / `make unload`.
* `1.0.3`: the monitor's own EDID timings, built as the vendor driver builds its custom modes, so 1920x1200 monitors get their native mode.
* `1.0.2`: the helper ends cleanly when the adapter is unplugged instead of being restarted every 2 s.
* `1.0.1`: interrupt events decoded as the vendor driver does (flip done, monitor hotplug), modes filtered by the monitor's EDID, and no second chip init at plug-in.
* This work was done by the fork maintainer together with AI coding assistants (OpenAI Codex, then Anthropic Claude Code). Protocol facts also come from a static analysis of the vendor's Windows driver. No vendor code is included.

These changes are **not** endorsed by or affiliated with the original author. Please report problems with this fork in this
repository, not to upstream.

### License

* The kernel driver (`kernel/`) is **GPL-2.0-only**, as in upstream (see `LICENSE`).
* Upstream's `LICENSE` states that its *userspace* tools are **MIT**, Copyright (c) 2026 Authentra / Ralph Friedman. Keep that notice for any upstream-derived user-space files.
* `userspace/trigger6-fast.c` (and its copy in `packaging/debian13/`) is new code in this fork and is **GPL-2.0-only**, as its `SPDX-License-Identifier` header states.
* The helper loads libyuv (BSD-style license) at run time. It is not bundled.
* The vendor's Windows installer and firmware are not part of this repository and must not be redistributed here.
* This is an independent community project and is not affiliated with MCT or StarTech. Product names are trademarks of their owners.
