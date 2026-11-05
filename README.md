# MCT Trigger 6 (T6) USB display driver for Linux, release `1.0.1`

An out-of-tree Linux DRM/KMS kernel driver and a small user-space helper for the
MCT "T6 USB Station" chipset, as found in the **StarTech USB32HDES** USB 3.0 to HDMI adapter.
This release reaches about **60 new frames per second at 1920x1080** on the tested
setup, with a stable USB connection.

> **Status: experimental.** Tested on one adapter model (see
> [Tested configuration](#tested-configuration)). Version 1.0.1 works on Wayland and builds on Arch Linux (kernel 7.2) and
> Debian 13 (kernel 6.12). **On Xorg, unplugging the USB adapter crashes the X server** (a bug in Xorg, reported to
> Debian, see [Limitations](#limitations)); unplugging only the HDMI cable is fine. Use at your own risk.

---

## Contents

1. [Tested configuration](#tested-configuration)
2. [Results](#results)
3. [What is in this repository](#what-is-in-this-repository)
4. [How it works](#how-it-works)
5. [Requirements](#requirements)
6. [Build and install](#build-and-install)
7. [Running the fast helper](#running-the-fast-helper)
8. [Activating the monitor](#activating-the-monitor)
9. [Module parameters](#module-parameters)
10. [Troubleshooting](#troubleshooting)
11. [Limitations](#limitations)
12. [Changelog](#changelog)
13. [Origin, license and credits](#origin-license-and-credits)

---

## Tested configuration

| Item | Value |
|---|---|
| Adapter | StarTech **USB32HDES**, reported as `MCT Corp. T6 USB Station` |
| **USB ID** | **`0711:5601`** (`idVendor=0711`, `idProduct=5601`) |
| On-board memory reported by the device | 58 MB |
| USB link | Direct to a USB 3.0 port (5000 Mbit/s). The first tests through a USB 2.0 hub were not usable for high frame rates |
| Monitor | Dell P2725HE, 1920x1080 @ 60 Hz |
| Host OS / kernel (`0.9.0-r8` measurements) | Arch Linux, `7.2.9-arch1-1`, Xorg |
| 1.0.0 hardware test | Works on Wayland. On Xorg it works until the USB adapter is unplugged: then the X server crashes (see [Limitations](#limitations)) |
| 1.0.0 builds | Arch Linux kernel 7.2, Debian 13 kernel 6.12, Linux 6.18.59 (`W=1`, no warnings) |
| 1.0.0 start-up test | Gentoo, Linux 6.18.59 with enforced module signatures: plug-in, chip init, mode table and unplug checked against a USB capture (no monitor attached yet) |
| 1.0.0 and 1.0.1 monitor test | Gentoo, Linux 6.18.59, KDE Plasma (Wayland), Dell P2725HE: picture, helper, mode switches, HDMI hotplug, flip-done events and `keepalive_ms=0`, checked against `t6_metrics` and a USB capture |
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
| `kernel/` | The DRM/KMS kernel module `trigger6.ko` (sources, `Makefile`, `Kbuild`) |
| `userspace/trigger6-fast.c` | The user-space SIMD helper `trigger6-fast` (NV12 conversion with libyuv), see [section C](#c-install-the-helper) |
| `dkms.conf` | DKMS configuration, see [section B](#b-permanent-install-with-dkms-debian-13--ubuntu--arch) |
| `packaging/debian13/trigger6-fast.service` | systemd **user** service that runs the helper, see [section D](#d-device-permissions-and-autostart-recommended) |
| `packaging/debian13/71-trigger6.rules` | udev rule: gives the logged-in user access to the adapter and starts the helper service when its device appears |
| `packaging/debian13/trigger6.conf` | modprobe options file (`primary_userspace_nv12=1`) for `/etc/modprobe.d/` |
| `packaging/debian13/Makefile.helper` | Makefile for building the helper |
| `packaging/debian13/trigger6-fast.c`, `dkms.conf` | Identical copies of the helper source and `dkms.conf`, so the folder is a complete packaging input |
| `packaging/debian13/DEPENDENCIES.md` | Package dependencies and test notes for Debian 13 |
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
The kernel only exports a frame when the desktop reports an update, and a periodic keepalive (every 2 s, `keepalive_ms`) resends the last frame so the chip does not blank a static picture. The vendor Windows driver sends no keepalive but reads the device's interrupt endpoint, which this driver now also does; whether the keepalive is still needed is one of the open hardware tests.

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

* Use a USB 3.0 port directly. Avoid a USB 2.0 hub.
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
`CONFIG_DRM_GEM_SHMEM_HELPER`, `CONFIG_USB`.
Secure Boot systems must sign the module or enrol the DKMS key.

## Build and install

Get the source:

```bash
git clone https://github.com/nvaert1986/trigger6-kernel-module.git
cd trigger6-kernel-module
```

### A. Quick test (no installation)

```bash
cd kernel
make                                    # builds trigger6.ko for the running kernel
sudo modprobe -a drm_kms_helper drm_shmem_helper   # insmod does not load dependencies
sudo insmod ./trigger6.ko primary_userspace_nv12=1
```

`insmod` loads only this one file. If `drm_shmem_helper` is not loaded yet, it fails with `Unknown symbol in module`
(`dmesg` names `drm_gem_shmem_dumb_create`), hence the `modprobe` line. An installed module (section B) loads its
dependencies by itself with `modprobe trigger6`.

On a kernel that only accepts signed modules (Secure Boot, `CONFIG_MODULE_SIG_FORCE`, lockdown), sign the module
before `insmod`, with the key your kernel was built with, and again after every rebuild:

```bash
sudo /lib/modules/$(uname -r)/build/scripts/sign-file sha256 <signing key .pem> <certificate .x509> trigger6.ko
modinfo -F signer trigger6.ko           # prints the key's name when signed
```

For a different kernel use `make KVER=<version>`. To unload: stop the helper, switch the output off (Xorg:
`xrandr --output <T6 output> --off`; Wayland: in the display settings), then `sudo rmmod trigger6`. Some desktops hold the device open. If `rmmod` says
"in use", first unbind it: `echo 2-2:1.0 | sudo tee /sys/bus/usb/drivers/trigger6/unbind` (use the interface
name shown under `/sys/bus/usb/drivers/trigger6/`) and retry after a second or two.

### B. Permanent install with DKMS (Debian 13 / Ubuntu / Arch)

```bash
sudo apt install dkms build-essential linux-headers-amd64 libyuv0   # Debian 13
sudo pacman -S --needed dkms base-devel linux-headers libyuv        # Arch (linux-lts-headers for the LTS kernel)

VER=1.0.1
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
Check the installed version with `modinfo -F version trigger6` (prints `1.0.1`).

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
`systemctl --user status trigger6-fast`.
**This udev/systemd integration has not been tested on Debian.** Manual alternative for one session:

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
`SIGTERM`/`Ctrl-C`. The screen then continues on the slower in-kernel fallback.

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
| `primary_partial_updates` | `0` | Experimental partial VRAM writes. Off for stability |
| `frame_min_interval_ms` | `0` | Extra wait between frames (USB time still limits speed) |
| `jpeg_quality` | `40` | Only for the JPEG path of the experimental secondary head |
| `experimental_secondary_raw` | `0` | Experimental second output |
| `serialize_usb_bus` | `1` | Serialise USB writes across devices |
| `manual_only` | `0` | Do not push frames automatically |
| `keepalive_ms` | `2000` | Resend the last frame after this many idle ms (100-10000); `0` switches the keepalive off |

Set one at load time with `modprobe trigger6 name=value`.

## Troubleshooting

* **No output / black screen after plug-in:** re-activate the output with `xrandr` (Xorg) or the display settings (Wayland). After a re-enumeration the device number changes, so the helper must pick up the new node (it does when restarted).
* **Stutter when moving the mouse:** make sure `trigger6-fast` is actually running (`pgrep -a trigger6`). Without it you only get the in-kernel fallback.
* **`insmod: File exists` / `rmmod: in use`:** unbind the interface first (see section A).
* **`insmod: ... Unknown symbol in module`:** a dependency is not loaded. Run `modprobe -a drm_kms_helper drm_shmem_helper`
  first (see section A).
* **`insmod: ... Key was rejected by service`:** the kernel only accepts signed modules; sign it (see section A).
* **Build fails with an unknown DRM symbol:** please report the exact kernel version. The code detects `drm_atomic_commit` and `drm_dev_set_dma_dev` from the headers, but other API differences may exist.
* **Frequent disconnects:** connect the adapter directly to a USB 3.0 port, not a hub.
* **Xorg crashes (SIGSEGV) when the USB adapter is unplugged:** this is the X server failing on the removal of a
  video device, not a kernel crash; see [Limitations](#limitations). Use a Wayland session if the adapter is
  unplugged while the desktop runs. Unplugging only the HDMI cable is safe on both.
* **Output disappears after a USB error** (`Halting T6 frame traffic on output 0` in `dmesg`): the driver re-enables
  the output automatically after about 60 s and puts the last picture back. A bulk timeout instead resets the USB
  device, which reloads the driver, so the helper must be restarted (the systemd user service does this).

## Limitations

* 1920x1080 @ 60 Hz is the only validated mode. The hardware is specified up to 1920x1200 @ 60 Hz, which is *not* validated here.
* Offered modes: the built-in ones (1920x1080 @ 60/30, 1600x900, 1280x1024, 1280x800, 1280x720, 1024x768, 800x600,
  640x480) plus every other mode in the adapter's own mode table up to 1920x1200 and 60 Hz (on the captured adapter:
  1920x1080@50, 1360x768, 1366x768, 1440x900, 1680x1050, 1600x1200, 1920x1200 and more). The adapter is specified for
  at most 60 Hz, so the 75 Hz entries in its table are not offered. With a readable EDID, modes larger than any mode
  the monitor lists, or outside its refresh and pixel clock limits, are left out (item 28): on a 1080p monitor with a
  48-100 Hz range that removes 1920x1200, 1600x1200 and 1920x1080@30.
* HDMI audio is not implemented.
* NV12 uses 4:2:0 colour, so tiny coloured text is slightly less sharp than RGB.
* The reported FPS is the number of frames sent. It is not a camera or analyser measurement of visible frames.
* Only the StarTech USB32HDES / `0711:5601` was tested. Other MCT T6 products (including the 4-output USB-C models in the upstream project) are not covered by these measurements.
* **Xorg: unplugging the USB adapter crashes the X server** with a segmentation fault when it handles the removed
  video device. Unplugging and replugging the HDMI cable works, and Wayland handles unplugging the adapter without
  problems. The driver removes the device the standard DRM way (`drm_dev_unplug()`), and the crash is in the X server,
  so it has been reported to Debian as an Xorg bug. Until that is fixed, use Wayland, or log out of the Xorg session
  before unplugging the adapter.
* Builds on Debian 13 (kernel 6.12) and Arch Linux (kernel 7.2). Not tested with KASAN/lockdep kernels, and the
  udev/systemd integration is untested.

## Changelog

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
see [Limitations](#limitations)). Unplugging the HDMI cable works on both. Colours confirmed correct (item 17).
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
20. **Chip init follows the vendor order** (`t6_chip_init`). Colour setup first, LUT reset for both outputs
    (`0x1C` 0x0000/0x0100), one all-zero `0x31`, the per-output mode tables, `SET_RESOLUTION` and enable per output,
    HDMI audio off, then an identity gamma upload with commit per output (`0x1D` x3, `0x1C` (output << 8) | 2).
    Before: colour after the resets, `0x31` per output, and two `0x24` requests that turned out to be **HDMI audio
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

#### Hardware test checklist (0.9.0-r9 to 1.0.1 changes)

Result so far: fully working on Wayland; on Xorg, unplugging the USB adapter crashes the X server (see
[Limitations](#limitations)). Confirmed on the adapter: steps 1, 6 (1920x1080@50 instead of @30, which 1.0.1 does
not offer on monitors that do not accept 30 Hz), 10, 11 (with 1.0.1: `head0 flips_done` follows `sent_frames` plus
`keepalive_sent`), 12 (picture kept for 155 s without frames) and 13, plus HDMI unplug/replug of the monitor. Since 1.0.1 no `T6 event`
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
10. Colours look right: compare with the same image on another output (item 17).
11. `t6_metrics`: `int_running=1`, and `head0 flips_done` grows with `sent_frames` (item 18). Report any `T6 event`
    lines in `dmesg`, especially around monitor unplug/replug: they are unknown event types.
12. Reload with `keepalive_ms=0` and leave a static desktop for a few minutes: does the picture blank (item 18)?
13. `xrandr` lists the adapter's modes (for example 1920x1080@50, 1680x1050); switch to a few and back (items 21-23).
14. No audio device is expected; check that the monitor does not report an audio stream or show audio artefacts (item 20).

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

#### Timing and colour data (also inherited from upstream `mct-t6-linux`)

16. **Corrupt 1080p timing** (`t6_mode_1080p`, `t6_mode_1080p30`). Both blobs had hsync_start 1816 and vsync_start
    1000, which lie inside the visible 1920x1080 area, and a vsync width of 29 lines. Chip init sends the 1080p60
    blob at every start. *Fix:* the sync fields now hold the CEA-861 values (hsync 2008/44, vsync 1084/5). The new
    1080p60 blob is byte-identical to the 1920x1080@60 entry in the adapter's own mode table (request `0x89`).
    Tolerant monitors such as the tested Dell accepted the old values; stricter ones may have shown "No Signal".
17. **Colour setup shifted by one byte** (`t6_init_color`, request `0x23`). The vendor driver sends ten
    little-endian u32 values (3, 0, 0x1f, 0x1f, then six times 0x0f). The inherited copy lacked the leading `0x03`
    byte, so every field was misaligned. *Fix:* the blob is now byte-identical to the vendor driver's request.
    *Watch the colours and picture on the first test: this changes chip configuration that has been wrong all along.*

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

### What this fork changes

* Kernel 7.2 and Debian 13 / 6.12 DRM API compatibility (`drm_atomic_commit`, `drm_dev_set_dma_dev`) and Xorg dirty-framebuffer support.
* Stability changes (no partial VRAM writes by default, EDID caching, stable active mode, per-device bulk mutex, keepalive-buffer fixes).
* Primary-output NV12 transport, 1 047 552-byte USB chunks, three device-side frame slots.
* The user-space SIMD bridge (`trigger6_bridge.h`, `trigger6-fast`), the asynchronous two-URB USB path and the removal of per-frame copies.
* `0.9.0-r9`: review fixes for fault recovery, probe error handling, output switch-off, suspend/resume, mode handling and the helper, plus corrected 1080p timing and colour data.
* `1.0.0`: init and mode-set sequence, interrupt endpoint, the adapter's own mode table and sync encoding (see [Changelog](#changelog)).
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
