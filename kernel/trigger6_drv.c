// SPDX-License-Identifier: GPL-2.0-only
/*
 * trigger6_drv.c -- MCT Trigger 6 USB Display DRM Driver
 *
 * Two-head USB display adapter with per-head transport selection:
 *   Head 0: raw XRGB8888 with row-level damage tracking and multi-write
 *           partial updates (only dirty rows are sent over USB).
 *   Head 1: NV12 by default (BGRX->NV12 conversion, ~3x bandwidth savings),
 *           or in-kernel JPEG with adaptive quality, or userspace JPEG
 *           injection, or experimental raw -- selected via module parameters.
 *
 * Adaptive per-head pacing: raw heads skip the pacing gate entirely (damage
 * tracking is the natural throttle); JPEG/NV12 heads pace adaptively based
 * on last encode time, capped by secondary_frame_min_interval_ms.
 *
 * Architecture: udl (USB DisplayLink) pattern for proven stability.
 * Protocol: mcttrigger/triggerdm (GPL) + cyrozap reverse engineering.
 *
 * Key safety patterns from udl:
 * - drm_dev_enter/exit guards on ALL USB calls
 * - Worker-based USB submission (never blocks atomic commit)
 * - Synchronous chunked bulk writes with centralized io_lock serialization
 * - Proper disconnect: poll_fini -> unplug -> shutdown
 *
 * Copyright (C) 2026 Authentra / Ralph Friedman
 */

#include <linux/module.h>
#include <linux/math64.h>
#include <linux/fs.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/timer.h>
#include <linux/poll.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_atomic.h>
#include <drm/drm_crtc.h>
#include <drm/drm_plane.h>
#include <drm/drm_encoder.h>
#include <drm/drm_gem_atomic_helper.h>

/* DRM renamed the atomic transaction type in newer kernels. */
#ifdef T6_HAVE_DRM_ATOMIC_COMMIT
#define t6_atomic_state drm_atomic_commit
#else
#define t6_atomic_state drm_atomic_state
#endif

#include "trigger6.h"
#include "trigger6_codec.h"

#define DRIVER_NAME	"trigger6"
#define DRIVER_DESC	"MCT Trigger 6 USB Display"

/*
 * Global mutex serialising USB bulk I/O across all T6 devices.
 * Two stations on the same hub can wedge the xHCI host controller
 * when both submit interleaved bulk URBs simultaneously.
 */
static DEFINE_MUTEX(t6_usb_bus_lock);

/* Partial VRAM writes are not validated on single-output T6 firmware.
 * Keep them opt-in, while still skipping unchanged frames on the CPU.
 */
static bool t6_primary_userspace_nv12;
module_param_named(primary_userspace_nv12, t6_primary_userspace_nv12, bool, 0444);
MODULE_PARM_DESC(primary_userspace_nv12,
                 "Expose primary SIMD conversion bridge with kernel fallback");

static bool t6_primary_nv12;
module_param_named(primary_nv12, t6_primary_nv12, bool, 0444);
MODULE_PARM_DESC(primary_nv12,
                 "Use full-frame NV12 on primary head (4:2:0 colour; default: false)");

static bool t6_primary_partial_updates;
module_param_named(primary_partial_updates, t6_primary_partial_updates, bool, 0644);
MODULE_PARM_DESC(primary_partial_updates,
                 "Experimental primary-head partial VRAM writes (default: false)");

static bool t6_experimental_secondary_raw;
module_param_named(experimental_secondary_raw,
		   t6_experimental_secondary_raw,
		   bool,
		   0644);
MODULE_PARM_DESC(experimental_secondary_raw,
		 "Force logical output 1 to use the raw XRGB8888 transport instead of the default NV12 path; uses double-buffered VRAM slots for tear-free display");

static bool t6_secondary_userspace_jpeg;
module_param_named(secondary_userspace_jpeg,
		   t6_secondary_userspace_jpeg,
		   bool,
		   0644);
MODULE_PARM_DESC(secondary_userspace_jpeg,
		 "Expose logical output 1 through DRM while userspace injects pre-encoded JPEG frames via /dev/trigger6-*-out1-jpeg");

static unsigned int t6_jpeg_quality = T6_JPEG_QUALITY_DEFAULT;

static int t6_jpeg_quality_set(const char *val, const struct kernel_param *kp)
{
	unsigned int v;
	int ret;

	ret = kstrtouint(val, 0, &v);
	if (ret)
		return ret;
	if (v < 1 || v > 100)
		return -EINVAL;
	WRITE_ONCE(t6_jpeg_quality, v);
	return 0;
}

static int t6_jpeg_quality_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(t6_jpeg_quality));
}

static const struct kernel_param_ops t6_jpeg_quality_ops = {
	.set = t6_jpeg_quality_set,
	.get = t6_jpeg_quality_get,
};

module_param_cb(jpeg_quality, &t6_jpeg_quality_ops, NULL, 0644);
MODULE_PARM_DESC(jpeg_quality,
		 "Baseline JPEG quality for logical output 1 when using the native in-kernel encoder (range: 1-100, default: "
		 __stringify(T6_JPEG_QUALITY_DEFAULT) ")");

static unsigned int t6_frame_min_interval_ms =
	T6_FRAME_MIN_INTERVAL_MS_DEFAULT;

static int t6_frame_min_interval_ms_set(const char *val,
					const struct kernel_param *kp)
{
	unsigned int v;
	int ret;

	ret = kstrtouint(val, 0, &v);
	if (ret)
		return ret;
	if (v > 1000)
		return -EINVAL;
	WRITE_ONCE(t6_frame_min_interval_ms, v);
	return 0;
}

static int t6_frame_min_interval_ms_get(char *buf,
					const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(t6_frame_min_interval_ms));
}

static const struct kernel_param_ops t6_frame_min_interval_ms_ops = {
	.set = t6_frame_min_interval_ms_set,
	.get = t6_frame_min_interval_ms_get,
};

module_param_cb(frame_min_interval_ms, &t6_frame_min_interval_ms_ops,
		NULL, 0644);
MODULE_PARM_DESC(frame_min_interval_ms,
		 "Raw-frame pacing gate in milliseconds (range: 0-1000, default: "
		 __stringify(T6_FRAME_MIN_INTERVAL_MS_DEFAULT)
		 "); JPEG/NV12 heads use adaptive pacing capped by secondary_frame_min_interval_ms");

static unsigned int t6_secondary_frame_min_interval_ms =
	T6_SECONDARY_FRAME_MIN_INTERVAL_MS_DEFAULT;

static int t6_secondary_frame_min_interval_ms_set(const char *val,
						  const struct kernel_param *kp)
{
	unsigned int v;
	int ret;

	ret = kstrtouint(val, 0, &v);
	if (ret)
		return ret;
	if (v > 5000)
		return -EINVAL;
	WRITE_ONCE(t6_secondary_frame_min_interval_ms, v);
	return 0;
}

static int t6_secondary_frame_min_interval_ms_get(char *buf,
						  const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%u\n",
			  READ_ONCE(t6_secondary_frame_min_interval_ms));
}

static const struct kernel_param_ops t6_secondary_frame_min_interval_ms_ops = {
	.set = t6_secondary_frame_min_interval_ms_set,
	.get = t6_secondary_frame_min_interval_ms_get,
};

module_param_cb(secondary_frame_min_interval_ms,
		&t6_secondary_frame_min_interval_ms_ops, NULL, 0644);
MODULE_PARM_DESC(secondary_frame_min_interval_ms,
		 "Upper cap for adaptive pacing on JPEG/NV12 secondary heads in milliseconds (range: 0-5000, default: "
		 __stringify(T6_SECONDARY_FRAME_MIN_INTERVAL_MS_DEFAULT)
		 "; 0 removes the cap and lets adaptive pacing run unconstrained)");

static bool t6_manual_only;
module_param_named(manual_only,
		   t6_manual_only,
		   bool,
		   0644);
MODULE_PARM_DESC(manual_only,
		 "Keep DRM connectors disconnected and disable automatic scanout; allow only manual userspace JPEG injection and guarded probe logging. Default: false for full driver operation.");

/*
 * The vendor Windows driver sends no keepalive frames (an 8 s idle gap in
 * its USB capture). Off by default since six monitors kept their picture
 * for 59 to 88 s without a single frame; keepalive_ms=2000 restores the old
 * behaviour for a monitor that blanks a static picture. Resends after a
 * fault, resume or event flag 0x10 do not depend on this setting.
 */
static unsigned int t6_keepalive_ms = T6_KEEPALIVE_DEFAULT_MS;

static int t6_keepalive_ms_set(const char *val, const struct kernel_param *kp)
{
	unsigned int v;
	int ret;

	ret = kstrtouint(val, 0, &v);
	if (ret)
		return ret;
	if (v && (v < 100 || v > 10000))
		return -EINVAL;
	WRITE_ONCE(t6_keepalive_ms, v);
	return 0;
}

static int t6_keepalive_ms_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(t6_keepalive_ms));
}

static const struct kernel_param_ops t6_keepalive_ms_ops = {
	.set = t6_keepalive_ms_set,
	.get = t6_keepalive_ms_get,
};

module_param_cb(keepalive_ms, &t6_keepalive_ms_ops, NULL, 0644);
MODULE_PARM_DESC(keepalive_ms,
		 "Resend the last frame after this many idle milliseconds (100-10000, 0 = off, default: "
		 __stringify(T6_KEEPALIVE_DEFAULT_MS) "; 2000 if a monitor blanks a static picture)");

/* Re-arm the keepalive timer, unless keepalive_ms=0. */
static void t6_arm_keepalive(struct t6_head *head)
{
	unsigned int ms = READ_ONCE(t6_keepalive_ms);

	if (ms)
		mod_timer(&head->keepalive_timer, jiffies + msecs_to_jiffies(ms));
}

static bool t6_serialize_usb_bus = true;
module_param_named(serialize_usb_bus,
		   t6_serialize_usb_bus,
		   bool,
		   0644);
MODULE_PARM_DESC(serialize_usb_bus,
		 "Serialize bulk USB writes across all T6 devices. Disable experimentally to allow parallel multi-adapter transfers on hosts that tolerate interleaved bulk traffic.");

/*
 * Hardware init data captured from Windows USB traces.
 * These register blobs configure the T6 color pipeline, display timing,
 * and 1920x1080 @ 60 Hz mode during chip initialisation.
 *
 * t6_init_color is ten little-endian u32 values, byte for byte what the
 * vendor Windows driver sends with request 0x23 (mctt6.pcapng, frame 120):
 * 3, 0, 0x1f, 0x1f, then six times 0x0f. The copy inherited from upstream
 * lacked the leading 0x03 byte, so every field was shifted by one byte.
 */
static const u8 t6_init_color[] = {
	0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x1f, 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00,
	0x0f, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00,
	0x0f, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00,
	0x0f, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00,
};
/*
 * Request 0x24 payload "HDMI audio off" (byte 0 = 1 would switch it on).
 * Upstream called these two blobs "timing pre/post" and sent "on" during
 * every init without ever sending audio data.
 */
static const u8 t6_audio_off[] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x80, 0x25, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00,
};
/*
 * Built-in display timing blobs (32 bytes each).
 * Format: pixel_clock(u32 LE kHz), refresh(u16), htotal(u16), hdisplay(u16),
 *         hsync_start(u16), hsync_width(u16), vtotal(u16), vdisplay(u16),
 *         vsync_start(u16), vsync_width(u16), PLL FNUM(u16), FDEN(u16),
 *         IDIV(u8), OutputSelect(u8), hsync_pol(u8), vsync_pol(u8),
 *         reduced(u8), flags(u8)
 * Pixel clock = {1250, 5000, 2500}[OutputSelect] kHz * (IDIV + FNUM/FDEN).
 *
 * The device reports the same format in its own mode table (request 0x89).
 * 640x480, 800x600, 1024x768, 720p and 1280x1024 below are byte-identical
 * to its entries. 1600x900 (vtotal 926 vs 920) and 1280x800 (FNUM 400 vs
 * 399) deliberately keep the standard CVT-RB / exact 83.5 MHz values.
 */
static const u8 t6_mode_1080p[] = {	/* 1920x1080@60 CEA */
	/*
	 * Sync fields from the device's own table: hsync 2008/44, vsync
	 * 1084/5 (CEA-861 VIC 16). The upstream blob had hsync_start 1816
	 * and vsync_start 1000, inside the visible area, and vsync_width 29.
	 */
	0x14, 0x44, 0x02, 0x00, 0x3c, 0x00, 0x98, 0x08,
	0x80, 0x07, 0xd8, 0x07, 0x2c, 0x00, 0x65, 0x04,
	0x38, 0x04, 0x3c, 0x04, 0x05, 0x00, 0xbb, 0x02,
	0xe8, 0x03, 0x1d, 0x01, 0x01, 0x01, 0x00, 0x00,
};

static const u8 t6_mode_720p[] = {	/* 1280x720@60 CEA */
	0x0a, 0x22, 0x01, 0x00, 0x3c, 0x00, 0x72, 0x06,
	0x00, 0x05, 0x6e, 0x05, 0x28, 0x00, 0xee, 0x02,
	0xd0, 0x02, 0xd5, 0x02, 0x05, 0x00, 0xbb, 0x02,
	0xe8, 0x03, 0x1d, 0x02, 0x01, 0x01, 0x00, 0x00,
};

static const u8 t6_mode_1024x768[] = {	/* 1024x768@60 VESA DMT */
	0xe8, 0xfd, 0x00, 0x00, 0x3c, 0x00, 0x40, 0x05,
	0x00, 0x04, 0x18, 0x04, 0x88, 0x00, 0x26, 0x03,
	0x00, 0x03, 0x03, 0x03, 0x06, 0x00, 0x00, 0x00,
	0xe8, 0x03, 0x1a, 0x02, 0x00, 0x00, 0x00, 0x00,
};

static const u8 t6_mode_1600x900[] = {	/* 1600x900@60 CVT RB */
	0xd6, 0x7d, 0x01, 0x00, 0x3c, 0x00, 0xe0, 0x06,
	0x40, 0x06, 0x70, 0x06, 0x20, 0x00, 0x9e, 0x03,
	0x84, 0x03, 0x87, 0x03, 0x05, 0x00, 0x64, 0x00,
	0xe8, 0x03, 0x27, 0x02, 0x01, 0x00, 0x01, 0x00,
};

static const u8 t6_mode_1080p30[] = {	/* 1920x1080@30 */
	/* CEA-861 VIC 34: same porches as 1080p60, 74.25 MHz (720p's PLL). */
	0x0a, 0x22, 0x01, 0x00, 0x1e, 0x00, 0x98, 0x08,
	0x80, 0x07, 0xd8, 0x07, 0x2c, 0x00, 0x65, 0x04,
	0x38, 0x04, 0x3c, 0x04, 0x05, 0x00, 0xbb, 0x02,
	0xe8, 0x03, 0x1d, 0x02, 0x01, 0x01, 0x00, 0x00,
};

static const u8 t6_mode_1280x1024[] = {	/* 1280x1024@60 VESA */
	0xe0, 0xa5, 0x01, 0x00, 0x3c, 0x00, 0x98, 0x06,
	0x00, 0x05, 0x30, 0x05, 0x70, 0x00, 0x2a, 0x04,
	0x00, 0x04, 0x01, 0x04, 0x03, 0x00, 0x58, 0x02,
	0xe8, 0x03, 0x15, 0x01, 0x01, 0x01, 0x00, 0x00,
};

static const u8 t6_mode_1280x800[] = {	/* 1280x800@60 CVT */
	0x2c, 0x46, 0x01, 0x00, 0x3c, 0x00, 0x90, 0x06,
	0x00, 0x05, 0x48, 0x05, 0x80, 0x00, 0x3f, 0x03,
	0x20, 0x03, 0x23, 0x03, 0x06, 0x00, 0x90, 0x01,
	0xe8, 0x03, 0x21, 0x02, 0x00, 0x01, 0x00, 0x00,
};

static const u8 t6_mode_800x600[] = {	/* 800x600@60 VESA */
	0x40, 0x9c, 0x00, 0x00, 0x3c, 0x00, 0x20, 0x04,
	0x20, 0x03, 0x48, 0x03, 0x80, 0x00, 0x74, 0x02,
	0x58, 0x02, 0x59, 0x02, 0x04, 0x00, 0x00, 0x00,
	0xe8, 0x03, 0x20, 0x00, 0x01, 0x01, 0x00, 0x00,
};

static const u8 t6_mode_640x480[] = {	/* 640x480@60 VGA */
	0x57, 0x62, 0x00, 0x00, 0x3c, 0x00, 0x20, 0x03,
	0x80, 0x02, 0x90, 0x02, 0x60, 0x00, 0x0d, 0x02,
	0xe0, 0x01, 0xea, 0x01, 0x02, 0x00, 0x8c, 0x00,
	0xe8, 0x03, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* All hardware-validated built-in modes */
const struct t6_builtin_mode t6_builtin_modes[] = {
	{ 1920, 1080, 60, t6_mode_1080p },
	{ 1920, 1080, 30, t6_mode_1080p30 },
	{ 1600,  900, 60, t6_mode_1600x900 },
	{ 1280, 1024, 60, t6_mode_1280x1024 },
	{ 1280,  800, 60, t6_mode_1280x800 },
	{ 1280,  720, 60, t6_mode_720p },
	{ 1024,  768, 60, t6_mode_1024x768 },
	{  800,  600, 60, t6_mode_800x600 },
	{  640,  480, 60, t6_mode_640x480 },
};
const int t6_builtin_mode_count = ARRAY_SIZE(t6_builtin_modes);

/*
 * Only modes with a timing blob can be programmed. Allow 1 Hz of slack
 * because drm_mode_vrefresh() rounds CVT modes (640x480@60 reports 59).
 * No width/height-only fallback: that would program 60 Hz for a 30 Hz mode.
 */
const u8 *t6_find_mode_blob(int width, int height, int refresh)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(t6_builtin_modes); i++) {
		if (t6_builtin_modes[i].width == width &&
		    t6_builtin_modes[i].height == height &&
		    abs(t6_builtin_modes[i].refresh - refresh) <= 1)
			return t6_builtin_modes[i].blob;
	}
	return NULL;
}

static u16 t6_blob_u16(const u8 *blob, unsigned int off)
{
	return blob[off] | (blob[off + 1] << 8);
}

static void t6_blob_put_u16(u8 *blob, unsigned int off, u16 val)
{
	blob[off] = val & 0xff;
	blob[off + 1] = val >> 8;
}

/*
 * Timing for one output: the built-in blobs first (validated, and 1600x900
 * and 1280x800 deliberately keep standard values), then the output's own
 * device mode table, so every mode the device reports can be programmed.
 */
const u8 *t6_head_mode_blob(const struct t6_head *head, int width, int height,
			    int refresh)
{
	const u8 *blob = t6_find_mode_blob(width, height, refresh);
	unsigned int i;

	if (blob || !head->mode_table)
		return blob;
	for (i = 0; i < head->mode_count; i++) {
		const u8 *e = head->mode_table + i * T6_RES_ENTRY_SIZE;

		if (t6_blob_u16(e, 8) == width && t6_blob_u16(e, 16) == height &&
		    abs(t6_blob_u16(e, 4) - refresh) <= 1)
			return e;
	}
	return NULL;
}

/* DRM mode with exactly the timing a blob programs (table convention). */
void t6_blob_to_mode(const u8 *blob, struct drm_display_mode *mode)
{
	u16 hs = t6_blob_u16(blob, 10), vs = t6_blob_u16(blob, 18);

	mode->clock = blob[0] | (blob[1] << 8) | (blob[2] << 16) |
		      ((u32)blob[3] << 24);
	mode->htotal = t6_blob_u16(blob, 6);
	mode->hdisplay = t6_blob_u16(blob, 8);
	mode->hsync_start = hs;
	mode->hsync_end = hs + t6_blob_u16(blob, 12);
	mode->vtotal = t6_blob_u16(blob, 14);
	mode->vdisplay = t6_blob_u16(blob, 16);
	mode->vsync_start = vs;
	mode->vsync_end = vs + t6_blob_u16(blob, 20);
	mode->flags = (blob[28] ? DRM_MODE_FLAG_PHSYNC : DRM_MODE_FLAG_NHSYNC) |
		      (blob[29] ? DRM_MODE_FLAG_PVSYNC : DRM_MODE_FLAG_NVSYNC);
	drm_mode_set_name(mode);
}

static u32 t6_blob_clock(const u8 *blob)
{
	return blob[0] | (blob[1] << 8) | (blob[2] << 16) | ((u32)blob[3] << 24);
}

/* True if @blob programs exactly @mode (as t6_blob_to_mode() builds it). */
bool t6_timing_matches_mode(const u8 *blob, const struct drm_display_mode *mode)
{
	u16 hs = t6_blob_u16(blob, 10), vs = t6_blob_u16(blob, 18);

	return mode->clock == t6_blob_clock(blob) &&
	       mode->hdisplay == t6_blob_u16(blob, 8) &&
	       mode->hsync_start == hs &&
	       mode->hsync_end == hs + t6_blob_u16(blob, 12) &&
	       mode->htotal == t6_blob_u16(blob, 6) &&
	       mode->vdisplay == t6_blob_u16(blob, 16) &&
	       mode->vsync_start == vs &&
	       mode->vsync_end == vs + t6_blob_u16(blob, 20) &&
	       mode->vtotal == t6_blob_u16(blob, 14);
}

/*
 * Highest pixel clock used for timings built from a monitor's EDID: the
 * fastest mode this output's device table lists (197 MHz on the tested
 * adapter), or the fastest built-in mode without a table.
 */
u32 t6_head_max_clock(const struct t6_head *head)
{
	u32 max = 148500;
	unsigned int i;

	for (i = 0; head->mode_table && i < head->mode_count; i++)
		max = max_t(u32, max, t6_blob_clock(head->mode_table +
						      i * T6_RES_ENTRY_SIZE));
	return max;
}

/*
 * Build a timing block (table convention) the way the vendor driver builds
 * its custom modes (T6InDisp.dll 0x1800f5d90, PLL helper 0x1800f7530): a
 * 40 MHz reference, a VCO of 800 to 1600 MHz and a post divider of 4, 8, 16
 * or 32 (output select 3, 1, 2, 0), so the clock is 10000, 5000, 2500 or
 * 1250 kHz * (IDIV + FNUM / 1000) with the smallest divider that reaches
 * 800 MHz. "Reduced" is always 0; byte 31 is 2 for widths 1360 and 1680 and
 * for widths above 1920 that are not a multiple of 32. The vendor computes
 * FNUM in floating point and truncates, so for some clocks its FNUM is one
 * lower (at most 5 kHz); this is the exact value. Applied to the device
 * table's own timings this reproduces 29 of its 30 entries, apart from that
 * FNUM rounding; the 30th sets "reduced" on 1600x900.
 */
static bool t6_build_timing(u8 *b, u32 khz, u16 w, u16 h, u16 htotal,
			    u16 hsync_start, u16 hsync_w, u16 vtotal,
			    u16 vsync_start, u16 vsync_w, bool hpos, bool vpos)
{
	static const u8 outsel[] = { 3, 1, 2, 0 };
	u32 base = 0;
	unsigned int i;

	if (khz < 25000 || khz > 300000 || !htotal || !vtotal)
		return false;
	for (i = 0; i < ARRAY_SIZE(outsel); i++) {
		if ((khz << (i + 2)) >= 800000) {
			base = 40000 >> (i + 2);
			break;
		}
	}
	if (!base)
		return false;

	memset(b, 0, T6_RES_ENTRY_SIZE);
	b[0] = khz & 0xff;
	b[1] = (khz >> 8) & 0xff;
	b[2] = (khz >> 16) & 0xff;
	b[3] = khz >> 24;
	t6_blob_put_u16(b, 4, DIV_ROUND_CLOSEST_ULL((u64)khz * 1000,
						    (u32)htotal * vtotal));
	t6_blob_put_u16(b, 6, htotal);
	t6_blob_put_u16(b, 8, w);
	t6_blob_put_u16(b, 10, hsync_start);
	t6_blob_put_u16(b, 12, hsync_w);
	t6_blob_put_u16(b, 14, vtotal);
	t6_blob_put_u16(b, 16, h);
	t6_blob_put_u16(b, 18, vsync_start);
	t6_blob_put_u16(b, 20, vsync_w);
	t6_blob_put_u16(b, 22, (khz % base) * 1000 / base);
	t6_blob_put_u16(b, 24, 1000);
	b[26] = khz / base;
	b[27] = outsel[i];
	b[28] = hpos;
	b[29] = vpos;
	b[31] = (w == 1360 || w == 1680 || (w > 1920 && (w & 0x1f))) ? 2 : 0;
	return true;
}

/*
 * Timing blocks for the detailed timings of an EDID base block (the only
 * block read from the device). Only what this adapter can show: up to
 * T6_MAX_WIDTH x T6_MAX_HEIGHT, at most 60 Hz, progressive, digital separate
 * sync. The pixel clock is checked against t6_head_max_clock() when a
 * timing is used, because the device table is read after the EDID.
 */
static void t6_build_edid_timings(struct t6_head *head, const u8 *edid)
{
	u8 t[T6_EDID_TIMINGS][T6_RES_ENTRY_SIZE];
	unsigned int i, n = 0;
	bool pref = false;

	for (i = 0; i < 4 && n < T6_EDID_TIMINGS; i++) {
		const u8 *d = edid + 0x36 + 18 * i;
		u32 khz = (d[0] | (d[1] << 8)) * 10;
		u16 ha = d[2] | ((d[4] & 0xf0) << 4);
		u16 hb = d[3] | ((d[4] & 0x0f) << 8);
		u16 va = d[5] | ((d[7] & 0xf0) << 4);
		u16 vb = d[6] | ((d[7] & 0x0f) << 8);
		u16 hfp = d[8] | ((d[11] & 0xc0) << 2);
		u16 hsw = d[9] | ((d[11] & 0x30) << 4);
		u16 vfp = (d[10] >> 4) | ((d[11] & 0x0c) << 2);
		u16 vsw = (d[10] & 0x0f) | ((d[11] & 0x03) << 4);

		if (!khz || (d[17] & 0x80) || (d[17] & 0x18) != 0x18)
			continue;
		if (!ha || !va || ha > T6_MAX_WIDTH || va > T6_MAX_HEIGHT ||
		    !hsw || !vsw || hfp + hsw > hb || vfp + vsw > vb)
			continue;
		if (!t6_build_timing(t[n], khz, ha, va, ha + hb, ha + hfp, hsw,
				     va + vb, va + vfp, vsw, d[17] & 0x02,
				     d[17] & 0x04) ||
		    t6_blob_u16(t[n], 4) > 60)
			continue;
		if (i == 0)
			pref = true;
		n++;
	}

	spin_lock(&head->edid_lock);
	memcpy(head->edid_timings, t, n * T6_RES_ENTRY_SIZE);
	head->edid_timing_count = n;
	head->edid_timing_pref = pref;
	spin_unlock(&head->edid_lock);
}

/*
 * Timing block for exactly @mode: built-in, device table, then the
 * monitor's EDID timings. Size and refresh alone are not enough: the
 * device table's 1920x1200@60 runs at 193.25 MHz, a monitor's reduced
 * blanking 1920x1200@60 at 154 MHz. Modes that are not from this driver's
 * own list fall back to the size and refresh lookup.
 */
bool t6_head_find_timing(struct t6_head *head,
			 const struct drm_display_mode *mode, u8 *out)
{
	const u8 *blob = NULL;
	bool found = false;
	unsigned int i;

	for (i = 0; i < t6_builtin_mode_count && !blob; i++)
		if (t6_timing_matches_mode(t6_builtin_modes[i].blob, mode))
			blob = t6_builtin_modes[i].blob;
	for (i = 0; head->mode_table && i < head->mode_count && !blob; i++) {
		const u8 *e = head->mode_table + i * T6_RES_ENTRY_SIZE;

		if (t6_timing_matches_mode(e, mode))
			blob = e;
	}
	if (blob) {
		memcpy(out, blob, T6_RES_ENTRY_SIZE);
		return true;
	}

	spin_lock(&head->edid_lock);
	for (i = 0; i < head->edid_timing_count && !found; i++) {
		const u8 *e = head->edid_timings[i];

		if (t6_timing_matches_mode(e, mode) &&
		    t6_blob_clock(e) <= t6_head_max_clock(head)) {
			memcpy(out, e, T6_RES_ENTRY_SIZE);
			found = true;
		}
	}
	spin_unlock(&head->edid_lock);
	if (found)
		return true;

	blob = t6_head_mode_blob(head, mode->hdisplay, mode->vdisplay,
				 drm_mode_vrefresh(mode));
	if (!blob)
		return false;
	memcpy(out, blob, T6_RES_ENTRY_SIZE);
	return true;
}

/*
 * SET_RESOLUTION takes the table entry with a 1-based sync start: the
 * vendor driver adds 1 to hsync/vsync start and subtracts 1 from their
 * widths for every entry before sending it (T6InDisp.dll 0x1800f8f34;
 * its 1360x768 request in the USB capture shows the same). The sync end
 * is unchanged. Blobs here use the table's own convention.
 */

static int t6_set_resolution(struct t6_device *t6, struct t6_head *head,
			     const u8 *blob)
{
	u8 b[T6_RES_ENTRY_SIZE];

	int ret;

	memcpy(b, blob, sizeof(b));
	t6_blob_put_u16(b, 10, t6_blob_u16(b, 10) + 1);
	t6_blob_put_u16(b, 12, t6_blob_u16(b, 12) - 1);
	t6_blob_put_u16(b, 18, t6_blob_u16(b, 18) + 1);
	t6_blob_put_u16(b, 20, t6_blob_u16(b, 20) - 1);
	ret = t6_ctrl_out(t6, T6_REQ_SET_RESOLUTION, head->output_idx, 0,
			  b, sizeof(b));
	if (ret >= 0) {
		if (blob != head->cur_timing)
			memcpy(head->cur_timing, blob, T6_RES_ENTRY_SIZE);
		head->cur_timing_valid = true;
	}
	return ret;
}

static const u32 t6_formats[] = { DRM_FORMAT_XRGB8888 };

static const struct drm_mode_config_funcs t6_mode_config_funcs = {
	/* Xorg PRIME updates existing buffers through DIRTYFB. */
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static const struct drm_mode_config_helper_funcs t6_mode_config_helper_funcs = {
	.atomic_commit_tail = drm_atomic_helper_commit_tail_rpm,
};

static const unsigned int t6_reprobe_delays_ms[] = {
	200,
	500,
	1000,
	3000,
	5000,
};

static bool t6_edid_base_block_valid(const u8 *edid);

static void t6_schedule_reprobe(struct t6_device *t6);
static void t6_trip_transport_fault(struct t6_device *t6,
					 struct t6_head *head,
					 int ret,
					 const char *source);

static const char *t6_transport_name(enum t6_head_transport transport)
{
	switch (transport) {
	case T6_HEAD_TRANSPORT_JPEG_CMD:
		return "jpeg-cmd";
	case T6_HEAD_TRANSPORT_USER_JPEG:
		return "user-jpeg";
	case T6_HEAD_TRANSPORT_NV12:
		return "nv12";
	case T6_HEAD_TRANSPORT_RAW:
	default:
		return "raw";
	}
}

static void t6_metrics_update_last_max(u64 *last, u64 *max, u64 value)
{
	u64 prev_max;

	WRITE_ONCE(*last, value);
	prev_max = READ_ONCE(*max);
	if (value > prev_max)
		WRITE_ONCE(*max, value);
}

static void t6_metrics_record_timing(atomic64_t *samples,
				     atomic64_t *total,
				     u64 *last,
				     u64 *max,
				     u64 value)
{
	if (!value)
		return;

	atomic64_inc(samples);
	atomic64_add(value, total);
	t6_metrics_update_last_max(last, max, value);
}

static u64 t6_metrics_avg_ns(const atomic64_t *total, const atomic64_t *samples)
{
	u64 sample_count = (u64)atomic64_read(samples);

	if (!sample_count)
		return 0;

	return div64_u64((u64)atomic64_read(total), sample_count);
}

static void t6_reset_head_metrics(struct t6_head *head)
{
	memset(&head->metrics, 0, sizeof(head->metrics));
	WRITE_ONCE(head->tx_queued_ns, 0);
}

static void t6_reset_device_metrics(struct t6_device *t6)
{
	memset(&t6->metrics, 0, sizeof(t6->metrics));
}

static void t6_head_set_error(struct t6_head *head, int error,
			      const char *source)
{
	WRITE_ONCE(head->metrics.last_error, error);
	if (source)
		strscpy(head->metrics.last_error_source, source,
			sizeof(head->metrics.last_error_source));
	else
		head->metrics.last_error_source[0] = '\0';
}

static void t6_head_record_send_error(struct t6_head *head, int error,
				      const char *source)
{
	atomic64_inc(&head->metrics.send_errors);
	t6_head_set_error(head, error, source);
}

static ssize_t t6_metrics_show(struct device *dev,
			       struct device_attribute *attr,
			       char *buf)
{
	struct usb_interface *intf = to_usb_interface(dev);
	struct t6_device *t6 = usb_get_intfdata(intf);
	ssize_t len = 0;
	unsigned int i;
	(void)attr;

	if (!t6)
		return sysfs_emit(buf, "trigger6=disconnected\n");

	len += sysfs_emit_at(buf, len,
		"trigger6 bus=%03u dev=%03u interface=%s manual_only=%u io_faulted=%u io_last_error=%d\n",
		t6->udev->bus->busnum,
		t6->udev->devnum,
		dev_name(dev),
		t6->manual_only,
		READ_ONCE(t6->io_faulted),
		t6->io_last_error);
	if (len >= PAGE_SIZE - 256)
		return len;
	len += sysfs_emit_at(buf, len,
		"device bulk_calls=%lld bulk_chunks=%lld bulk_bytes=%lld bulk_errors=%lld bulk_short_writes=%lld tx_work_runs=%lld tx_work_empty=%lld tx_adapter_defers=%lld last_bulk_us=%llu max_bulk_us=%llu last_bulk_bytes=%zu int_running=%u int_events=%lld int_hotplug=%lld int_wake=%lld int_unknown=%lld int_errors=%lld\n",
		(long long)atomic64_read(&t6->metrics.bulk_calls),
		(long long)atomic64_read(&t6->metrics.bulk_chunks),
		(long long)atomic64_read(&t6->metrics.bulk_bytes),
		(long long)atomic64_read(&t6->metrics.bulk_errors),
		(long long)atomic64_read(&t6->metrics.bulk_short_writes),
		(long long)atomic64_read(&t6->metrics.tx_work_runs),
		(long long)atomic64_read(&t6->metrics.tx_work_empty),
		(long long)atomic64_read(&t6->metrics.tx_adapter_defers),
		div64_u64(READ_ONCE(t6->metrics.last_bulk_ns), 1000),
		div64_u64(READ_ONCE(t6->metrics.max_bulk_ns), 1000),
		READ_ONCE(t6->metrics.last_bulk_bytes),
		READ_ONCE(t6->int_running),
		(long long)atomic64_read(&t6->int_events),
		(long long)atomic64_read(&t6->int_hotplug),
		(long long)atomic64_read(&t6->int_wake),
		(long long)atomic64_read(&t6->int_unknown),
		(long long)atomic64_read(&t6->int_errors));

	for (i = 0; i < t6->output_count; i++) {
		struct t6_head *head = t6_get_head(t6, i);

		if (len >= PAGE_SIZE - 256)
			return len;
		len += sysfs_emit_at(buf, len,
			"head%u transport=%s connected=%u io_faulted=%u status=%u pending=%u width=%d height=%d refresh=%d frame_seq=%u modes=%u flips_done=%lld last_done_fence=%u\n",
			head->output_idx,
			t6_transport_name(head->transport),
			READ_ONCE(head->connected),
			READ_ONCE(head->io_faulted),
			head->status,
			head->tx_pending,
			head->width,
			head->height,
			head->refresh,
			head->frame_seq,
			head->mode_count,
			(long long)atomic64_read(&head->metrics.flips_done),
			READ_ONCE(head->last_done_fence));
		len += sysfs_emit_at(buf, len,
			"head%u queue_attempts=%lld queued_frames=%lld coalesced_overwrites=%lld skipped_frames=%lld pacing_defers=%lld transport_faults=%lld jpeg_quality=%u\n",
			head->output_idx,
			(long long)atomic64_read(&head->metrics.queue_attempts),
			(long long)atomic64_read(&head->metrics.queued_frames),
			(long long)atomic64_read(&head->metrics.coalesced_overwrites),
			(long long)atomic64_read(&head->metrics.skipped_frames),
			(long long)atomic64_read(&head->metrics.pacing_defers),
			(long long)atomic64_read(&head->metrics.transport_faults),
			READ_ONCE(head->jpeg_adaptive_quality));
		len += sysfs_emit_at(buf, len,
			"head%u sent_frames=%lld sent_bytes=%lld send_errors=%lld keepalive_sent=%lld keepalive_errors=%lld last_payload_bytes=%zu\n",
			head->output_idx,
			(long long)atomic64_read(&head->metrics.sent_frames),
			(long long)atomic64_read(&head->metrics.sent_bytes),
			(long long)atomic64_read(&head->metrics.send_errors),
			(long long)atomic64_read(&head->metrics.keepalive_sent),
			(long long)atomic64_read(&head->metrics.keepalive_errors),
			READ_ONCE(head->metrics.last_payload_bytes));
		if (len >= PAGE_SIZE - 256)
			return len;
		len += sysfs_emit_at(buf, len,
			"head%u stage_avg_us=%llu stage_last_us=%llu stage_max_us=%llu queue_wait_avg_us=%llu queue_wait_last_us=%llu queue_wait_max_us=%llu\n",
			head->output_idx,
			div64_u64(t6_metrics_avg_ns(&head->metrics.stage_ns_total,
						    &head->metrics.stage_samples), 1000),
			div64_u64(READ_ONCE(head->metrics.last_stage_ns), 1000),
			div64_u64(READ_ONCE(head->metrics.max_stage_ns), 1000),
			div64_u64(t6_metrics_avg_ns(&head->metrics.queue_wait_ns_total,
						    &head->metrics.queue_wait_samples), 1000),
			div64_u64(READ_ONCE(head->metrics.last_queue_wait_ns), 1000),
			div64_u64(READ_ONCE(head->metrics.max_queue_wait_ns), 1000));
		len += sysfs_emit_at(buf, len,
			"head%u encode_avg_us=%llu encode_last_us=%llu encode_max_us=%llu usb_avg_us=%llu usb_last_us=%llu usb_max_us=%llu send_avg_us=%llu send_last_us=%llu send_max_us=%llu\n",
			head->output_idx,
			div64_u64(t6_metrics_avg_ns(&head->metrics.encode_ns_total,
						    &head->metrics.encode_samples), 1000),
			div64_u64(READ_ONCE(head->metrics.last_encode_ns), 1000),
			div64_u64(READ_ONCE(head->metrics.max_encode_ns), 1000),
			div64_u64(t6_metrics_avg_ns(&head->metrics.usb_ns_total,
						    &head->metrics.usb_samples), 1000),
			div64_u64(READ_ONCE(head->metrics.last_usb_ns), 1000),
			div64_u64(READ_ONCE(head->metrics.max_usb_ns), 1000),
			div64_u64(t6_metrics_avg_ns(&head->metrics.send_ns_total,
						    &head->metrics.send_samples), 1000),
			div64_u64(READ_ONCE(head->metrics.last_send_ns), 1000),
			div64_u64(READ_ONCE(head->metrics.max_send_ns), 1000));
		len += sysfs_emit_at(buf, len,
			"head%u last_error=%d last_error_source=%s\n",
			head->output_idx,
			READ_ONCE(head->metrics.last_error),
			head->metrics.last_error_source[0] ?
				head->metrics.last_error_source : "none");
	}

	{
		char line[160];

		t6_audio_metrics(t6, line, sizeof(line));
		len += sysfs_emit_at(buf, len, "%s\n", line);
	}
	return len;
}

static ssize_t t6_metrics_reset_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf,
				      size_t count)
{
	struct usb_interface *intf = to_usb_interface(dev);
	struct t6_device *t6 = usb_get_intfdata(intf);
	unsigned int i;
	(void)attr;

	if (!t6)
		return -ENODEV;
	if (!sysfs_streq(buf, "1") && !sysfs_streq(buf, "reset"))
		return -EINVAL;

	t6_reset_device_metrics(t6);
	for (i = 0; i < t6->output_count; i++)
		t6_reset_head_metrics(t6_get_head(t6, i));

	return count;
}

static DEVICE_ATTR_RO(t6_metrics);
static DEVICE_ATTR_WO(t6_metrics_reset);

static ssize_t t6_head_enable_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct usb_interface *intf = to_usb_interface(dev);
	struct t6_device *t6 = usb_get_intfdata(intf);

	if (!t6)
		return -ENODEV;
	return sysfs_emit(buf, "%u %u\n",
			  READ_ONCE(t6_get_head(t6, 0)->connected) ? 1 : 0,
			  READ_ONCE(t6_get_head(t6, 1)->connected) ? 1 : 0);
}

static ssize_t t6_head_enable_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct usb_interface *intf = to_usb_interface(dev);
	struct t6_device *t6 = usb_get_intfdata(intf);
	unsigned int head_idx, enable;
	struct t6_head *head;
	int idx, ret;

	if (!t6)
		return -ENODEV;
	if (sscanf(buf, "%u %u", &head_idx, &enable) != 2 ||
	    head_idx >= t6->output_count || enable > 1)
		return -EINVAL;

	head = t6_get_head(t6, head_idx);
	if (!drm_dev_enter(&t6->drm, &idx))
		return -ENODEV;

	/* io_lock, like every other control transfer (ctrl_error_count). */
	mutex_lock(&t6->io_lock);
	ret = t6_ctrl_out(t6, T6_REQ_MONITOR_CTRL, head->output_idx,
			  enable, NULL, 0);
	mutex_unlock(&t6->io_lock);
	drm_dev_exit(idx);

	if (ret < 0)
		return ret;

	if (!enable) {
		timer_delete_sync(&head->keepalive_timer);
		cancel_delayed_work_sync(&head->boot_keepalive_work);
	}

	return count;
}

static DEVICE_ATTR_RW(t6_head_enable);

static struct attribute *t6_attrs[] = {
	&dev_attr_t6_metrics.attr,
	&dev_attr_t6_metrics_reset.attr,
	&dev_attr_t6_head_enable.attr,
	NULL,
};

static const struct attribute_group t6_group = {
	.attrs = t6_attrs,
};

static int t6_pack_xrgb8888_tight(struct t6_head *head,
				 const void *src,
				 u32 src_stride,
				 u8 *dst,
				 size_t dst_size)
{
	const u8 *src_base = src;
	u32 row_bytes = head->width * 4;
	u32 y;

	if (!src || !dst || src_stride < row_bytes)
		return -EINVAL;

	if ((size_t)row_bytes * head->height > dst_size)
		return -E2BIG;

	if (src_stride == row_bytes) {
		memcpy(dst, src_base, (size_t)row_bytes * head->height);
		return 0;
	}

	for (y = 0; y < head->height; y++) {
		const u8 *src_row = src_base + (size_t)y * src_stride;
		u8 *dst_row = dst + (size_t)y * row_bytes;

		memcpy(dst_row, src_row, row_bytes);
	}

	return 0;
}

static void t6_wake_all_export_waiters(struct t6_device *t6)
{
	unsigned int i;

	for (i = 0; i < T6_OUTPUT_COUNT; i++)
		wake_up_interruptible(&t6_get_head(t6, i)->fb_export_wq);
}

static void t6_cancel_head_activity(struct t6_device *t6)
{
	unsigned int i;

	for (i = 0; i < T6_OUTPUT_COUNT; i++) {
		struct t6_head *head = t6_get_head(t6, i);

		timer_delete_sync(&head->keepalive_timer);
		cancel_delayed_work_sync(&head->boot_keepalive_work);
		cancel_delayed_work_sync(&head->tx_defer_work);
		cancel_work_sync(&head->tx_work);
		WRITE_ONCE(head->keepalive_due, false);
	}
}

static void t6_reset_head_stream_state(struct t6_head *head)
{
	if (head->fb_slot_count > 1)
		head->fb_slot_index = 1;
	else
		head->fb_slot_index = 0;

	head->cmd_cursor_addr = head->cmd_base_addr;
	head->frame_seq = 0;
	head->tx_last_jiffies = 0;
	WRITE_ONCE(head->keepalive_due, false);
}

static void t6_init_head_addressing(struct t6_device *t6, struct t6_head *head)
{
	u32 tmp = t6->ram_mb - 18;

	memset(head->fb_slots, 0, sizeof(head->fb_slots));
	head->cmd_base_addr = 0;
	head->cmd_limit_addr = 0;

	if (head->output_idx == 0) {
		head->fb_addr = (tmp - 12) * 1024 * 1024;
		head->cmd_addr = 0;
		head->fb_slots[0] = head->fb_addr;
		head->fb_slot_count = 1;
		if (head->transport == T6_HEAD_TRANSPORT_NV12) {
			/* Three non-overlapping 4 MiB slots in primary's 12 MiB.
			 * Each holds up to 1920x1200 NV12 plus framing.
			 */
			head->fb_slots[1] = head->fb_addr + 4 * 1024 * 1024;
			head->fb_slots[2] = head->fb_addr + 8 * 1024 * 1024;
			head->fb_slot_count = 3;
		}
	} else if (head->transport == T6_HEAD_TRANSPORT_RAW) {
		/*
		 * Secondary raw: double-buffer with two 8 MB+ VRAM slots
		 * so the display reads from one while the host writes the
		 * other.  Without this, the display scans the same buffer
		 * we are writing to and visible tearing results.
		 *
		 * Layout (ram_mb=58, tmp=40):
		 *   cmd ring:    40 MB – 42 MB  (2 MB, 2 × 1 MB slots)
		 *   fb_slots[0]: 42 MB – 50 MB  (8.3 MB frame fits in 8 MB gap)
		 *   fb_slots[1]: 50 MB – 58 MB  (8.3 MB frame fits in 8 MB gap)
		 */
		head->cmd_addr = tmp * 1024 * 1024;
		head->fb_slots[0] = (tmp + 2) * 1024 * 1024;
		head->fb_slots[1] = (tmp + 10) * 1024 * 1024;
		head->fb_slots[2] = 0;
		head->fb_slot_count = 2;
		head->fb_addr = head->fb_slots[0];
		head->cmd_base_addr = head->cmd_addr;
		head->cmd_limit_addr = head->fb_slots[0];
	} else {
		/* Secondary JPEG/NV12/userspace: triple-buffer with 3 × 4 MB slots */
		head->cmd_addr = tmp * 1024 * 1024;
		head->fb_slots[0] = (t6->ram_mb - 12) * 1024 * 1024;
		head->fb_slots[1] = (t6->ram_mb - 8) * 1024 * 1024;
		head->fb_slots[2] = (t6->ram_mb - 4) * 1024 * 1024;
		head->fb_slot_count = T6_JPEG_FB_SLOT_COUNT;
		head->fb_addr = head->fb_slots[0];
		head->cmd_base_addr = head->cmd_addr;
		/*
		 * The vendor-style output-1 JPEG cmd ring stops at the first fb slot.
		 * Wrapping later than that walks command buckets into framebuffer space,
		 * which matches the observed "a few visible frames, then stop" failure.
		 */
		head->cmd_limit_addr = head->fb_slots[0];
	}

	t6_reset_head_stream_state(head);
}

static void t6_keepalive_timeout(struct timer_list *t);
static void t6_boot_keepalive_fn(struct work_struct *work);
static void t6_frame_defer_work(struct work_struct *work);
static void t6_frame_work(struct work_struct *work);

static void t6_head_defaults(struct t6_device *t6,
			     struct t6_head *head,
			     u8 output_idx)
{
	memset(head, 0, sizeof(*head));
	head->t6 = t6;
	head->output_idx = output_idx;
	head->width = T6_SCANOUT_WIDTH;
	head->height = T6_SCANOUT_HEIGHT;
	head->refresh = T6_SCANOUT_REFRESH_HZ;
	strscpy(head->monitor_name, "T6 Display", sizeof(head->monitor_name));
	timer_setup(&head->keepalive_timer, t6_keepalive_timeout, 0);
	INIT_WORK(&head->tx_work, t6_frame_work);
	INIT_DELAYED_WORK(&head->tx_defer_work, t6_frame_defer_work);
	INIT_DELAYED_WORK(&head->boot_keepalive_work, t6_boot_keepalive_fn);
	spin_lock_init(&head->edid_lock);
	init_waitqueue_head(&head->fb_export_wq);
	atomic_set(&head->fb_export_seq, 0);
	t6_reset_head_metrics(head);
	if (output_idx == 0)
		head->transport = (t6_primary_nv12 || t6_primary_userspace_nv12) ? T6_HEAD_TRANSPORT_NV12 :
			T6_HEAD_TRANSPORT_RAW;
	else if (t6_secondary_userspace_jpeg)
		head->transport = T6_HEAD_TRANSPORT_USER_JPEG;
	else if (t6_experimental_secondary_raw)
		head->transport = T6_HEAD_TRANSPORT_RAW;
	else
		head->transport = T6_HEAD_TRANSPORT_NV12;

	t6_init_head_addressing(t6, head);
}

/*
 * Extract the monitor name from an EDID descriptor block.
 * EDID descriptors at offsets 0x36..0x7E each span 18 bytes.
 * Tag 0xFC marks the "Monitor Name" string (up to 13 ASCII chars).
 */
static void t6_parse_edid_monitor_name(struct t6_head *head)
{
	int i, j, len;

	if (head->edid_len < 128)
		return;
	for (i = 0x36; i + 18 <= 128; i += 18) {
		if (head->edid_data[i] != 0 ||
		    head->edid_data[i + 1] != 0 ||
		    head->edid_data[i + 3] != 0xFC)
			continue;

		len = min(13, (int)sizeof(head->monitor_name) - 1);
		memcpy(head->monitor_name, &head->edid_data[i + 5], len);
		head->monitor_name[len] = '\0';

		/* Trim trailing whitespace / control characters */
		for (j = len - 1; j >= 0; j--) {
			if (head->monitor_name[j] <= ' ')
				head->monitor_name[j] = '\0';
			else
				break;
		}
		return;
	}
}

static int t6_probe_head(struct t6_device *t6, struct t6_head *head)
{
	u8 status = 0;
	u8 edid[EDID_LENGTH];
	int preferred_width = T6_SCANOUT_WIDTH;
	int preferred_height = T6_SCANOUT_HEIGHT;
	bool was_connected = READ_ONCE(head->connected);
	int ret;

	ret = t6_ctrl_in(t6, T6_REQ_GET_STATUS, head->output_idx, 0, &status, 1);
	if (ret < 1)
		return ret < 0 ? ret : -EIO;

	head->status = status;
	WRITE_ONCE(head->connected, status == 1);
	if (!head->connected)
		return 0;

	/* A status poll is not a new hotplug. Avoid re-reading EDID while
	 * scanout is active; retry only missing EDID or a new connection.
	 */
	if (was_connected && head->edid_len >= 128)
		return 0;

	/* Read into a local buffer: get_modes may be reading edid_data. */
	ret = t6_ctrl_in(t6, T6_REQ_GET_EDID, 0, head->output_idx,
			 edid, EDID_LENGTH);
	/*
	 * A different monitor (or the first one) must not inherit the
	 * previous monitor's timing; the same monitor replugged keeps it.
	 * Only this worker writes edid_data, so it can be read unlocked here.
	 */
	if (ret >= EDID_LENGTH && t6_edid_base_block_valid(edid))
		head->edid_new = head->edid_len < EDID_LENGTH ||
				 memcmp(head->edid_data, edid, EDID_LENGTH);
	else
		head->edid_new = head->edid_len >= EDID_LENGTH;
	spin_lock(&head->edid_lock);
	if (ret >= EDID_LENGTH && t6_edid_base_block_valid(edid)) {
		memcpy(head->edid_data, edid, EDID_LENGTH);
		head->edid_len = EDID_LENGTH;
	} else {
		head->edid_len = 0;
		head->edid_timing_count = 0;
		head->edid_timing_pref = false;
	}
	spin_unlock(&head->edid_lock);

	if (head->edid_len >= EDID_LENGTH) {
		preferred_width = head->edid_data[0x38] |
			((head->edid_data[0x3A] & 0xF0) << 4);
		preferred_height = head->edid_data[0x3B] |
			 ((head->edid_data[0x3D] & 0xF0) << 4);
		t6_parse_edid_monitor_name(head);
		t6_build_edid_timings(head, edid);
		dev_info(&t6->udev->dev,
			 "Head %u: monitor prefers %dx%d; %u EDID timing(s) usable on this adapter%s\n",
			 head->output_idx, preferred_width, preferred_height,
			 head->edid_timing_count,
			 head->edid_timing_pref ? ", including the preferred one" : "");
	} else if (ret >= 128) {
		dev_warn(&t6->udev->dev,
			 "Head %u returned an EDID block with invalid checksum; using fixed %dx%d mode assumptions\n",
			 head->output_idx,
			 T6_SCANOUT_WIDTH,
			 T6_SCANOUT_HEIGHT);
	}

	/* Polling EDID must preserve the active hardware scanout mode.
	 * Initial dimensions are set by t6_head_defaults().
	 */

	return 0;
}

static int t6_alloc_head_buffers(struct t6_device *t6)
{
	unsigned int idx;

	/* Pre-allocate shared USB bulk chunk buffer */
	t6->bulk_chunk = kmalloc(T6_USB_XFER_CHUNK_SIZE, GFP_KERNEL);
	if (!t6->bulk_chunk)
		return -ENOMEM;
	t6->bulk_chunk2 = kmalloc(T6_USB_XFER_CHUNK_SIZE, GFP_KERNEL);
	t6->bulk_urb[0] = usb_alloc_urb(0, GFP_KERNEL);
	t6->bulk_urb[1] = usb_alloc_urb(0, GFP_KERNEL);
	/* Missing async resources only disable the overlap, not the driver. */

	for (idx = 0; idx < t6->output_count; idx++) {
		struct t6_head *head = t6_get_head(t6, idx);
		u32 frame_size;

		head->tx_buf_size = T6_MAX_WIDTH * T6_MAX_HEIGHT * 4;
		head->tx_front = kvzalloc(head->tx_buf_size, GFP_KERNEL);
		head->tx_back = kvzalloc(head->tx_buf_size, GFP_KERNEL);
		if (!head->tx_front || !head->tx_back)
			return -ENOMEM;

		/* Pre-allocate video staging for raw transport */
		if (head->transport == T6_HEAD_TRANSPORT_RAW) {
			frame_size = head->tx_buf_size;
			head->video_staging_size =
				sizeof(struct t6_flip_header) + frame_size;
			head->video_staging =
				kvzalloc(head->video_staging_size, GFP_KERNEL);
			if (!head->video_staging)
				return -ENOMEM;
			/* Damage tracking: last-sent reference frame */
			head->last_sent_frame =
				kvzalloc(frame_size, GFP_KERNEL);
			if (!head->last_sent_frame)
				return -ENOMEM;
			head->last_sent_valid = false;
		}

		/* Pre-allocate JPEG/NV12 staging for non-raw heads */
		if (head->transport != T6_HEAD_TRANSPORT_RAW) {
			head->jpeg_staging_size = sizeof(struct t6_flip_header) +
				ALIGN(T6_MAX_WIDTH, 16) * T6_MAX_HEIGHT * 3 / 2 +
				T6_JPEG_PADDING_SIZE;
			head->jpeg_staging =
				kvzalloc(head->jpeg_staging_size, GFP_KERNEL);
			if (!head->jpeg_staging)
				return -ENOMEM;
		}

		/* NV12 heads: allocate damage reference for zero-skip */
		if (head->transport == T6_HEAD_TRANSPORT_NV12) {
			frame_size = head->tx_buf_size;
			head->last_sent_frame =
				kvzalloc(frame_size, GFP_KERNEL);
			if (!head->last_sent_frame)
				return -ENOMEM;
			head->last_sent_valid = false;
		}

		/* Framebuffer export buffer for hybrid JPEG compositor path */
		if (head->transport == T6_HEAD_TRANSPORT_USER_JPEG ||
		    (idx == 0 && t6_primary_userspace_nv12)) {
			head->fb_export_buf =
				kvzalloc(head->tx_buf_size, GFP_KERNEL);
			if (!head->fb_export_buf)
				return -ENOMEM;
		}
	}

	return 0;
}

static void t6_free_head_buffers(struct t6_device *t6)
{
	unsigned int idx;

	for (idx = 0; idx < T6_OUTPUT_COUNT; idx++) {
		struct t6_head *head = t6_get_head(t6, idx);

		kvfree(head->tx_back);
		kvfree(head->tx_front);
		head->tx_back = NULL;
		head->tx_front = NULL;
		head->tx_buf_size = 0;
		head->tx_len = 0;
		head->tx_stride = 0;
		head->tx_pending = false;

		kvfree(head->video_staging);
		head->video_staging = NULL;
		head->video_staging_size = 0;

		kvfree(head->last_sent_frame);
		head->last_sent_frame = NULL;
		head->last_sent_valid = false;

		kvfree(head->jpeg_staging);
		head->jpeg_staging = NULL;
		head->jpeg_staging_size = 0;

		kvfree(head->fb_export_buf);
		head->fb_export_buf = NULL;
		head->fb_export_len = 0;

		timer_delete_sync(&head->keepalive_timer);
		kvfree(head->last_jpeg_data);
		head->last_jpeg_data = NULL;
		head->last_jpeg_len = 0;
		head->last_jpeg_cap = 0;
	}

	kfree(t6->bulk_chunk);
	t6->bulk_chunk = NULL;
	kfree(t6->bulk_chunk2);
	t6->bulk_chunk2 = NULL;
	usb_free_urb(t6->bulk_urb[0]);
	usb_free_urb(t6->bulk_urb[1]);
	t6->bulk_urb[0] = t6->bulk_urb[1] = NULL;
}

enum t6_tx_defer_reason {
	T6_TX_DEFER_NONE = 0,
	T6_TX_DEFER_ADAPTER,
	T6_TX_DEFER_HEAD,
};

static bool t6_head_has_pending_activity_locked(struct t6_head *head)
{
	return head->tx_pending || READ_ONCE(head->keepalive_due);
}

static bool t6_pop_pending_frame_locked(struct t6_head *head,
					size_t *len,
					u64 *queued_ns)
{
	u8 *front;

	if (!head->tx_pending || !head->tx_front || !head->tx_back)
		return false;

	front = head->tx_front;
	head->tx_front = head->tx_back;
	head->tx_back = front;
	*len = head->tx_len;
	*queued_ns = READ_ONCE(head->tx_queued_ns);
	head->tx_pending = false;
	WRITE_ONCE(head->tx_queued_ns, 0);
	return true;
}

static void t6_requeue_popped_head_locked(struct t6_head *head,
					  size_t len,
					  u64 queued_ns)
{
	u8 *front = head->tx_front;

	head->tx_front = head->tx_back;
	head->tx_back = front;
	head->tx_pending = true;
	head->tx_len = len;
	if (queued_ns)
		WRITE_ONCE(head->tx_queued_ns, queued_ns);
}

static unsigned long t6_head_tx_delay_jiffies(struct t6_device *t6,
					      struct t6_head *head,
					      enum t6_tx_defer_reason *reason)
{
	unsigned int gate_ms = 0;
	unsigned long elapsed;
	unsigned long required;

	*reason = T6_TX_DEFER_NONE;

	if (head->transport == T6_HEAD_TRANSPORT_RAW ||
	    (head->output_idx == 0 && head->transport == T6_HEAD_TRANSPORT_NV12)) {
		/* Bound USB submissions even for tiny cursor-only updates. */
		gate_ms = READ_ONCE(t6_frame_min_interval_ms);
	}

	/*
	 * JPEG/NV12 heads: adaptive gate based on last encode/convert
	 * time.  Send as fast as the encoder allows, bounded by the
	 * secondary_frame_min_interval_ms cap for safety.
	 */
	if (head->transport == T6_HEAD_TRANSPORT_JPEG_CMD ||
	    (head->output_idx != 0 && head->transport == T6_HEAD_TRANSPORT_NV12)) {
		u64 last_encode = READ_ONCE(head->metrics.last_encode_ns);
		unsigned int cap = READ_ONCE(t6_secondary_frame_min_interval_ms);

		if (last_encode)
			gate_ms = (unsigned int)(last_encode / 1000000ULL);
		else
			gate_ms = cap ? cap : 50;

		/* Floor of 5ms, cap at secondary param (0 = no cap) */
		gate_ms = max(gate_ms, 5U);
		if (cap)
			gate_ms = min(gate_ms, cap);
	}

	if (gate_ms && head->tx_last_jiffies) {
		required = msecs_to_jiffies(gate_ms);
		elapsed = jiffies - head->tx_last_jiffies;
		if (elapsed < required) {
			*reason = T6_TX_DEFER_HEAD;
			return required - elapsed;
		}
	}

	return 0;
}

static struct t6_head *t6_pick_next_pending_head_locked(struct t6_device *t6,
							bool skip_head_deferred)
{
	unsigned int i;

	for (i = 0; i < t6->output_count; i++) {
		struct t6_head *candidate;
		enum t6_tx_defer_reason reason;

		candidate = t6_get_head(t6, (t6->tx_next_head + i) % t6->output_count);
		if (!t6_head_has_pending_activity_locked(candidate))
			continue;
		if (skip_head_deferred &&
		    t6_head_tx_delay_jiffies(t6, candidate, &reason) &&
		    reason == T6_TX_DEFER_HEAD)
			continue;
		return candidate;
	}

	return NULL;
}

static void t6_queue_head_tx_work(struct t6_head *head)
{
	struct t6_device *t6 = head->t6;

	if (t6->wq)
		queue_work(t6->wq, &head->tx_work);
}

static void t6_queue_next_pending_head(struct t6_device *t6)
{
	struct t6_head *next;

	mutex_lock(&t6->tx_lock);
	next = t6_pick_next_pending_head_locked(t6, false);
	mutex_unlock(&t6->tx_lock);

	if (next)
		t6_queue_head_tx_work(next);
}

static void t6_requeue_head_activity(struct t6_head *head,
				     size_t len,
				     u64 queued_ns,
				     bool keepalive_due)
{
	struct t6_device *t6 = head->t6;

	mutex_lock(&t6->tx_lock);
	if (!keepalive_due) {
		if (!head->tx_pending)
			t6_requeue_popped_head_locked(head, len, queued_ns);
	} else if (!head->tx_pending) {
		WRITE_ONCE(head->keepalive_due, true);
	}
	mutex_unlock(&t6->tx_lock);
}

static void t6_defer_head_activity(struct t6_head *head,
				   size_t len,
				   u64 queued_ns,
				   bool keepalive_due,
				   unsigned long delay,
				   enum t6_tx_defer_reason reason)
{
	struct t6_device *t6 = head->t6;

	t6_requeue_head_activity(head, len, queued_ns, keepalive_due);
	if (reason == T6_TX_DEFER_ADAPTER)
		atomic64_inc(&t6->metrics.tx_adapter_defers);
	else if (reason == T6_TX_DEFER_HEAD)
		atomic64_inc(&head->metrics.pacing_defers);
	if (t6->wq)
		mod_delayed_work(t6->wq, &head->tx_defer_work, delay);
}

static bool t6_should_yield_turn(struct t6_head *head,
				 size_t len,
				 u64 queued_ns,
				 bool keepalive_due)
{
	struct t6_device *t6 = head->t6;
	struct t6_head *next;

	if (head->transport != T6_HEAD_TRANSPORT_JPEG_CMD &&
	    head->transport != T6_HEAD_TRANSPORT_NV12)
		return false;

	mutex_lock(&t6->tx_lock);
	next = t6_pick_next_pending_head_locked(t6, true);
	if (next && next != head && next->transport == T6_HEAD_TRANSPORT_RAW) {
		if (!keepalive_due) {
			if (!head->tx_pending)
				t6_requeue_popped_head_locked(head, len, queued_ns);
		} else if (!head->tx_pending) {
			WRITE_ONCE(head->keepalive_due, true);
		}
	} else {
		next = NULL;
	}
	mutex_unlock(&t6->tx_lock);

	if (next)
		t6_queue_head_tx_work(next);

	return next != NULL;
}

static void t6_stage_pending_head_locked(struct t6_head *head,
					 size_t len,
					 u32 stride,
					 u64 queued_ns)
{
	if (head->tx_pending)
		atomic64_inc(&head->metrics.coalesced_overwrites);
	else
		atomic64_inc(&head->metrics.queued_frames);

	head->tx_len = len;
	head->tx_stride = stride;
	head->tx_pending = true;
	WRITE_ONCE(head->tx_queued_ns, queued_ns);
}

/* ------------------------------------------------------------------
 * USB helpers -- ALL use kmalloc'd DMA-safe buffers
 * ------------------------------------------------------------------
 */

int t6_ctrl_out(struct t6_device *t6, u8 req, u16 val,
		u16 idx, const void *data, u16 size)
{
	void *buf = NULL;
	int ret;

	if (size && data) {
		buf = kmemdup(data, size, GFP_KERNEL);
		if (!buf)
			return -ENOMEM;
	}
	ret = usb_control_msg(t6->udev, usb_sndctrlpipe(t6->udev, 0),
			      req, USB_TYPE_VENDOR | USB_DIR_OUT,
			      val, idx, buf, size,
			      T6_USB_CTRL_TIMEOUT_MS);
	kfree(buf);
	if (ret < 0) {
		if (++t6->ctrl_error_count > T6_CTRL_ERROR_THRESHOLD) {
			dev_err(&t6->udev->dev,
				"Wedged: %u consecutive ctrl errors; scheduling USB reset\n",
				t6->ctrl_error_count);
			usb_queue_reset_device(t6->intf);
		}
	} else {
		t6->ctrl_error_count = 0;
	}
	return ret;
}

int t6_ctrl_in(struct t6_device *t6, u8 req, u16 val,
	       u16 idx, void *data, u16 size)
{
	void *buf;
	int ret;

	buf = kzalloc(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	memset(data, 0, size); /* zero caller's buffer in case of short read */
	ret = usb_control_msg(t6->udev, usb_rcvctrlpipe(t6->udev, 0),
			      req, USB_TYPE_VENDOR | USB_DIR_IN,
			      val, idx, buf, size,
			      T6_USB_CTRL_TIMEOUT_MS);
	if (ret >= 0) {
		memcpy(data, buf, min_t(int, ret, size));
		t6->ctrl_error_count = 0;
	} else {
		if (++t6->ctrl_error_count > T6_CTRL_ERROR_THRESHOLD) {
			dev_err(&t6->udev->dev,
				"Wedged: %u consecutive ctrl errors; scheduling USB reset\n",
				t6->ctrl_error_count);
			usb_queue_reset_device(t6->intf);
		}
	}
	kfree(buf);
	return ret;
}

static bool t6_edid_base_block_valid(const u8 *edid)
{
	u8 checksum = 0;
	unsigned int idx;

	for (idx = 0; idx < 128; idx++)
		checksum += edid[idx];

	return checksum == 0;
}

struct t6_bulk_slot {
	struct completion done;
	bool busy;
};

static void t6_bulk_complete(struct urb *urb)
{
	complete(urb->context);
}

/* Wait for one in-flight chunk; returns 0 or a negative errno. */
static int t6_bulk_reap(struct t6_device *t6, struct t6_bulk_slot *slot,
			struct urb *urb, size_t expect)
{
	int ret = 0;

	if (!slot->busy)
		return 0;
	slot->busy = false;
	if (!wait_for_completion_timeout(&slot->done,
					 msecs_to_jiffies(T6_USB_XFER_TIMEOUT_MS))) {
		usb_kill_urb(urb);
		wait_for_completion(&slot->done);
		atomic64_inc(&t6->metrics.bulk_errors);
		dev_err(&t6->udev->dev,
			"USB bulk write timed out; scheduling device reset\n");
		usb_queue_reset_device(t6->intf);
		return -ETIMEDOUT;
	}
	if (urb->status) {
		atomic64_inc(&t6->metrics.bulk_errors);
		return urb->status;
	}
	if (urb->actual_length != expect) {
		atomic64_inc(&t6->metrics.bulk_short_writes);
		return -EIO;
	}
	return ret;
}

/* Double-buffered: memcpy of chunk N+1 overlaps the USB transfer of chunk N. */
static int t6_bulk_write_async(struct t6_device *t6, const void *src, size_t len)
{
	u8 *bufs[2] = { t6->bulk_chunk, t6->bulk_chunk2 };
	struct t6_bulk_slot slots[2];
	size_t sizes[2] = { 0, 0 };
	const u8 *cursor = src;
	size_t remaining = len;
	unsigned int n = 0, i;
	int ret = 0;

	for (i = 0; i < 2; i++) {
		init_completion(&slots[i].done);
		slots[i].busy = false;
	}
	while (remaining) {
		unsigned int k = n & 1;
		size_t chunk = min_t(size_t, T6_USB_XFER_CHUNK_SIZE, remaining);

		ret = t6_bulk_reap(t6, &slots[k], t6->bulk_urb[k], sizes[k]);
		if (ret)
			break;
		atomic64_inc(&t6->metrics.bulk_chunks);
		memcpy(bufs[k], cursor, chunk);
		reinit_completion(&slots[k].done);
		usb_fill_bulk_urb(t6->bulk_urb[k], t6->udev,
				  usb_sndbulkpipe(t6->udev, T6_EP_BULK_OUT),
				  bufs[k], chunk, t6_bulk_complete,
				  &slots[k].done);
		ret = usb_submit_urb(t6->bulk_urb[k], GFP_KERNEL);
		if (ret) {
			atomic64_inc(&t6->metrics.bulk_errors);
			break;
		}
		slots[k].busy = true;
		sizes[k] = chunk;
		cursor += chunk;
		remaining -= chunk;
		n++;
	}
	for (i = 0; i < 2; i++) {
		int r;

		if (ret && slots[i].busy) {
			usb_kill_urb(t6->bulk_urb[i]);
			wait_for_completion(&slots[i].done);
			slots[i].busy = false;
			continue;
		}
		r = t6_bulk_reap(t6, &slots[i], t6->bulk_urb[i], sizes[i]);
		if (r && !ret)
			ret = r;
	}
	return ret;
}

static int t6_bulk_write_locked(struct t6_device *t6, const void *src, size_t len)
{
	u8 *chunk_buf = t6->bulk_chunk;
	const u8 *cursor = src;
	size_t remaining = len;
	u64 bulk_start_ns;
	bool serialize_usb_bus;
	int ret = 0;

	if (!len)
		return 0;
	if (!chunk_buf)
		return -ENOMEM;

	atomic64_inc(&t6->metrics.bulk_calls);
	atomic64_add(len, &t6->metrics.bulk_bytes);
	WRITE_ONCE(t6->metrics.last_bulk_bytes, len);
	bulk_start_ns = ktime_get_ns();
	serialize_usb_bus = READ_ONCE(t6_serialize_usb_bus);

	if (serialize_usb_bus)
		mutex_lock(&t6_usb_bus_lock);
	if (len > T6_USB_XFER_CHUNK_SIZE && t6->bulk_chunk2 &&
	    t6->bulk_urb[0] && t6->bulk_urb[1]) {
		ret = t6_bulk_write_async(t6, src, len);
		remaining = 0;
	}
	while (remaining) {
		size_t chunk = min_t(size_t, T6_USB_XFER_CHUNK_SIZE, remaining);
		int actual = 0;

		atomic64_inc(&t6->metrics.bulk_chunks);
		memcpy(chunk_buf, cursor, chunk);
		ret = usb_bulk_msg(t6->udev,
				   usb_sndbulkpipe(t6->udev, T6_EP_BULK_OUT),
				   chunk_buf,
				   chunk,
				   &actual,
				   T6_USB_XFER_TIMEOUT_MS);
		if (ret < 0) {
			atomic64_inc(&t6->metrics.bulk_errors);
			if (ret == -ETIMEDOUT) {
				dev_err(&t6->udev->dev,
					"USB bulk write timed out; scheduling device reset\n");
				usb_queue_reset_device(t6->intf);
			}
			break;
		}
		if (actual != chunk) {
			atomic64_inc(&t6->metrics.bulk_short_writes);
			ret = -EIO;
			break;
		}

		cursor += chunk;
		remaining -= chunk;
	}
	if (serialize_usb_bus)
		mutex_unlock(&t6_usb_bus_lock);
	t6_metrics_update_last_max(&t6->metrics.last_bulk_ns,
				   &t6->metrics.max_bulk_ns,
				   ktime_get_ns() - bulk_start_ns);

	return ret;
}

/* Keepalive frame cache; last_jpeg_cap is always the real allocation size. */
static int t6_last_frame_store(struct t6_head *head, const void *data,
			       size_t len)
{
	if (head->last_jpeg_cap < len || !head->last_jpeg_data) {
		u8 *buf = kvmalloc(len, GFP_KERNEL);

		if (!buf)
			return -ENOMEM;
		kvfree(head->last_jpeg_data);
		head->last_jpeg_data = buf;
		head->last_jpeg_cap = len;
	}
	memcpy(head->last_jpeg_data, data, len);
	head->last_jpeg_len = len;
	return 0;
}

int t6_bulk_write(struct t6_device *t6, const void *src, size_t len)
{
	int ret;

	/* The async path shares chunk buffers and URBs, so always serialize per device. */
	mutex_lock(&t6->bulk_lock);
	ret = t6_bulk_write_locked(t6, src, len);
	mutex_unlock(&t6->bulk_lock);
	return ret;
}

/* ------------------------------------------------------------------
 * T6 chip init (probe context only -- no drm_dev_enter needed)
 * ------------------------------------------------------------------
 */

/*
 * Read one output's mode table (request 0x89, wValue = output, wIndex = byte
 * offset) in 512-byte chunks, as the vendor driver does. The vendor driver
 * takes the entry count from its own host-side list and never sends 0x84,
 * so stop at a short read, an error or the first implausible entry.
 */
/*
 * Which outputs the adapter has (request 0xB3, read by the vendor driver at
 * start-up, T6InDisp.dll 0x1800fb480): it enables an output when the low 4
 * bits of its u32 at byte 88 + 4 * output are set. A single-output adapter
 * reports 0 for output 1; the dual-output adapter in the vendor capture
 * reports 0x04301e01. Bits 8-15 give the output's mode table length (30 on
 * both). Without a valid block, keep the old assumption of two outputs.
 */
static void t6_read_device_info(struct t6_device *t6)
{
	u8 *info = kzalloc(T6_DEVICE_INFO_SIZE, GFP_KERNEL);
	unsigned int i, present = 0;
	u32 out[T6_OUTPUT_COUNT] = { 0 };
	int ret;

	t6->output_count = T6_OUTPUT_COUNT;
	if (!info)
		return;
	ret = t6_ctrl_in(t6, T6_REQ_GET_DEVICE_INFO, 0, 0, info,
			 T6_DEVICE_INFO_SIZE);
	if (ret < T6_DEVICE_INFO_SIZE || memcmp(info, "DISP", 4)) {
		dev_info(&t6->udev->dev,
			 "No device information block (%d); assuming %u outputs\n",
			 ret, T6_OUTPUT_COUNT);
		kfree(info);
		return;
	}
	for (i = 0; i < T6_OUTPUT_COUNT; i++) {
		__le32 v;

		memcpy(&v, info + T6_DEVICE_INFO_OUTPUTS + 4 * i, sizeof(v));
		out[i] = le32_to_cpu(v);
		if (!(out[i] & 0xf))
			break;
		t6->table_entries[i] = (out[i] >> 8) & 0xff;
		present++;
	}
	kfree(info);
	if (!present) {
		dev_warn(&t6->udev->dev,
			 "Device information lists no outputs; assuming %u\n",
			 T6_OUTPUT_COUNT);
		return;
	}
	t6->output_count = present;
	dev_info(&t6->udev->dev,
		 "Device information: %u output%s (output 0: type %u, %u modes%s)\n",
		 present, present == 1 ? "" : "s", out[0] & 0xf,
		 t6->table_entries[0],
		 present > 1 ? "" : "; output 1 not present");
}

static void t6_read_mode_table(struct t6_device *t6, struct t6_head *head)
{
	unsigned int entries = t6->table_entries[head->output_idx];
	unsigned int max = T6_RES_MAX_ENTRIES * T6_RES_ENTRY_SIZE;
	unsigned int got = 0, i;
	u8 *buf;

	/* With the count from request 0xB3, read exactly that, as the vendor does. */
	if (entries && entries <= T6_RES_MAX_ENTRIES)
		max = entries * T6_RES_ENTRY_SIZE;
	/* drmm: the connector reads this until the DRM device is released. */
	buf = drmm_kzalloc(&t6->drm, max, GFP_KERNEL);
	if (!buf)
		return;
	while (got < max) {
		unsigned int len = min_t(unsigned int, T6_RES_CHUNK_SIZE,
					 max - got);
		int ret = t6_ctrl_in(t6, T6_REQ_GET_RES_TABLE, head->output_idx,
				     got, buf + got, len);

		if (ret <= 0)
			break;
		got += ret;
		if (ret < len)
			break;
	}

	for (i = 0; i < got / T6_RES_ENTRY_SIZE; i++) {
		const u8 *e = buf + i * T6_RES_ENTRY_SIZE;
		u32 clk = e[0] | (e[1] << 8) | (e[2] << 16) | ((u32)e[3] << 24);
		u16 refresh = t6_blob_u16(e, 4), htotal = t6_blob_u16(e, 6);
		u16 w = t6_blob_u16(e, 8), vtotal = t6_blob_u16(e, 14);
		u16 h = t6_blob_u16(e, 16);

		if (!clk || clk > 600000 || !refresh || refresh > 240 ||
		    w < 320 || h < 200 || htotal <= w || vtotal <= h)
			break;
	}

	if (head->mode_table)
		drmm_kfree(&t6->drm, head->mode_table);
	head->mode_table = i ? buf : NULL;
	head->mode_count = i;
	if (!i)
		drmm_kfree(&t6->drm, buf);
	dev_info(&t6->udev->dev, "Output %u: device mode table has %u entries\n",
		 head->output_idx, i);
}

/* Identity gamma for one output, as the vendor driver uploads it. */
static int t6_load_identity_lut(struct t6_device *t6, struct t6_head *head)
{
	static const u16 channels[] = { 0x100, 0x200, 0x400 };
	u16 out = head->output_idx << 8;
	u8 lut[256];
	unsigned int i;
	int ret;

	for (i = 0; i < sizeof(lut); i++)
		lut[i] = i;
	for (i = 0; i < ARRAY_SIZE(channels); i++) {
		ret = t6_ctrl_out(t6, T6_REQ_LUT_WRITE, out, channels[i],
				  lut, sizeof(lut));
		if (ret < 0)
			return ret;
	}
	return t6_ctrl_out(t6, T6_REQ_LUT_CTRL, out | T6_LUT_OP_COMMIT, 0,
			   NULL, 0);
}

/*
 * Chip init in the order the vendor Windows driver uses (USB capture of a
 * 0711:5601, frames 120-258): colour setup, LUT reset for both outputs, one
 * all-zero 0x31, [mode tables], then per output SET_RESOLUTION and enable,
 * audio off, and an identity gamma upload with commit. The old sequence sent
 * colour after the resets, 0x31 per output, wrapped everything in two 0x24
 * requests (HDMI audio off, then *on* without audio data) and committed the
 * LUT of output 0 without uploading one.
 */
/* True if two timing blocks program the same signal (FNUM rounding aside). */
static bool t6_timing_same(const u8 *a, const u8 *b)
{
	return !memcmp(a, b, 4) && !memcmp(a + 6, b + 6, 16) &&
	       a[28] == b[28] && a[29] == b[29];
}

/*
 * A different monitor was connected: start it on its own preferred timing
 * (its first EDID detailed timing, if this adapter can show it) or 1080p60,
 * not on the previous monitor's timing, which it may not be able to show
 * (a 1080p monitor after a 1920x1200 one stayed black until the desktop
 * set its mode). The desktop can still pick another mode afterwards.
 * Returns true if the timing to program changed. Caller holds io_lock or
 * runs in probe.
 */
static bool t6_adopt_monitor_timing(struct t6_device *t6, struct t6_head *head)
{
	u8 t[T6_RES_ENTRY_SIZE];
	const u8 *blob = t6_mode_1080p;

	head->edid_new = false;
	spin_lock(&head->edid_lock);
	if (head->edid_timing_pref && head->edid_timing_count &&
	    t6_blob_clock(head->edid_timings[0]) <= t6_head_max_clock(head)) {
		memcpy(t, head->edid_timings[0], sizeof(t));
		blob = t;
	}
	spin_unlock(&head->edid_lock);

	/*
	 * Use the built-in or table block for the same signal, as get_modes
	 * and atomic_enable do, so the desktop's mode set finds it unchanged.
	 */
	if (blob == t) {
		unsigned int i;

		for (i = 0; i < t6_builtin_mode_count; i++)
			if (t6_timing_same(t6_builtin_modes[i].blob, t)) {
				blob = t6_builtin_modes[i].blob;
				break;
			}
		for (i = 0; blob == t && head->mode_table &&
			    i < head->mode_count; i++)
			if (t6_timing_same(head->mode_table +
					   i * T6_RES_ENTRY_SIZE, t))
				blob = head->mode_table + i * T6_RES_ENTRY_SIZE;
	}

	if (head->cur_timing_valid && t6_timing_same(blob, head->cur_timing))
		return false;
	mutex_lock(&t6->tx_lock);
	head->width = t6_blob_u16(blob, 8);
	head->height = t6_blob_u16(blob, 16);
	head->refresh = t6_blob_u16(blob, 4);
	head->last_sent_valid = false;
	mutex_unlock(&t6->tx_lock);
	memcpy(head->cur_timing, blob, T6_RES_ENTRY_SIZE);
	head->cur_timing_valid = true;
	dev_info(&t6->udev->dev,
		 "Output %u: new monitor, starting with its preferred %dx%d@%d\n",
		 head->output_idx, head->width, head->height, head->refresh);
	return true;
}

static int t6_chip_init(struct t6_device *t6, bool read_tables)
{
	bool init_head[T6_OUTPUT_COUNT] = { false };
	unsigned int idx;
	int ret;
	bool any_connected = false;

	for (idx = 0; idx < t6->output_count; idx++) {
		if (t6_get_head(t6, idx)->connected) {
			init_head[idx] = true;
			any_connected = true;
		}
	}
	if (!any_connected)
		init_head[0] = true;

	t6->init_mask = 0;
	ret = t6_ctrl_out(t6, T6_REQ_SET_COLOR, 0, 0,
			  t6_init_color, sizeof(t6_init_color));
	if (ret < 0)
		return ret;
	/* Both pipes of the chip, also on single-output adapters (tested). */
	for (idx = 0; idx < T6_OUTPUT_COUNT; idx++) {
		ret = t6_ctrl_out(t6, T6_REQ_LUT_CTRL,
				  (idx << 8) | T6_LUT_OP_RESET, 0, NULL, 0);
		if (ret < 0)
			return ret;
	}
	ret = t6_ctrl_out(t6, T6_REQ_SET_READY, 0, 0, NULL, 0);
	if (ret < 0)
		return ret;

	/* Once, at probe: get_modes reads these tables without locking. */
	if (read_tables)
		for (idx = 0; idx < t6->output_count; idx++)
			t6_read_mode_table(t6, t6_get_head(t6, idx));

	for (idx = 0; idx < t6->output_count; idx++) {
		struct t6_head *head = t6_get_head(t6, idx);

		/* After the tables: the EDID timing's clock limit needs them. */
		if (init_head[idx] && head->edid_new)
			t6_adopt_monitor_timing(t6, head);
	}

	for (idx = 0; idx < t6->output_count; idx++) {
		struct t6_head *head = t6_get_head(t6, idx);
		const u8 *blob;

		if (!init_head[idx])
			continue;

		/*
		 * Re-init (resume, monitor replug) must restore the mode the
		 * compositor chose, not 1080p: head->width/height still describe
		 * the active DRM mode and atomic_enable only reprograms on change.
		 */
		if (head->cur_timing_valid &&
		    t6_blob_u16(head->cur_timing, 8) == head->width &&
		    t6_blob_u16(head->cur_timing, 16) == head->height)
			blob = head->cur_timing;
		else
			blob = t6_head_mode_blob(head, head->width,
						 head->height, head->refresh);
		if (!blob) {
			blob = t6_mode_1080p;
			mutex_lock(&t6->tx_lock);
			head->width = T6_SCANOUT_WIDTH;
			head->height = T6_SCANOUT_HEIGHT;
			head->refresh = T6_SCANOUT_REFRESH_HZ;
			head->last_sent_valid = false;
			mutex_unlock(&t6->tx_lock);
		}
		ret = t6_set_resolution(t6, head, blob);
		if (ret < 0)
			return ret;
		ret = t6_ctrl_out(t6, T6_REQ_MONITOR_CTRL, head->output_idx, 1,
				  NULL, 0);
		if (ret < 0)
			return ret;
		t6_reset_head_stream_state(head);
	}

	ret = t6_ctrl_out(t6, T6_REQ_AUDIO_CTRL, 0, 0,
			  t6_audio_off, sizeof(t6_audio_off));
	if (ret < 0)
		return ret;
	t6_audio_engine_lost(t6);

	for (idx = 0; idx < t6->output_count; idx++) {
		if (!init_head[idx])
			continue;
		ret = t6_load_identity_lut(t6, t6_get_head(t6, idx));
		if (ret < 0)
			return ret;
	}

	for (idx = 0; idx < t6->output_count; idx++)
		if (init_head[idx])
			t6->init_mask |= BIT(idx);
	return 0;
}

/* ------------------------------------------------------------------
 * Frame sending -- synchronous bulk transfers
 * ------------------------------------------------------------------
 */

/*
 * Scan the incoming frame against the last-sent frame, row by row,
 * and return a damage descriptor listing dirty row ranges.  If no
 * last-sent reference exists (first frame, mode change) or the damage
 * exceeds T6_MAX_DIRTY_RANGES, the caller falls back to a full frame.
 */
static void t6_compute_frame_damage(struct t6_head *head,
				    const void *pixels,
				    struct t6_frame_damage *dmg)
{
	u32 stride = head->width * 4;
	const u8 *cur = pixels;
	const u8 *ref = head->last_sent_frame;
	u32 row;
	bool in_dirty = false;

	dmg->num_ranges = 0;
	dmg->full_frame = true;

	if (!ref || !head->last_sent_valid)
		return;

	dmg->full_frame = false;

	for (row = 0; row < head->height; row++) {
		bool dirty = memcmp(cur + (size_t)row * stride,
				    ref + (size_t)row * stride,
				    stride) != 0;
		if (dirty && !in_dirty) {
			if (dmg->num_ranges >= T6_MAX_DIRTY_RANGES) {
				dmg->full_frame = true;
				return;
			}
			dmg->ranges[dmg->num_ranges].y_start = row;
			dmg->ranges[dmg->num_ranges].y_count = 1;
			in_dirty = true;
		} else if (dirty && in_dirty) {
			dmg->ranges[dmg->num_ranges].y_count++;
		} else if (!dirty && in_dirty) {
			dmg->num_ranges++;
			in_dirty = false;
		}
	}

	if (in_dirty)
		dmg->num_ranges++;
}

static int t6_send_frame_raw(struct t6_device *t6, struct t6_head *head,
			     const void *pixels, size_t pixel_len,
			     const struct t6_frame_damage *dmg)
{
	u32 stride = head->width * 4;
	u32 frame_size = stride * head->height;
	const u8 *src = pixels;
	struct t6_bulk_header bch;
	struct t6_flip_header fh;
	u64 send_start_ns;
	size_t total_sent = 0;
	int ret;
	int i;

	if (!READ_ONCE(head->connected))
		return -ENODEV;

	if (pixel_len < frame_size)
		return -EINVAL;

	/*
	 * If damage tracking says nothing changed, skip the send entirely.
	 * The monitor continues showing the previous frame from VRAM.
	 */
	if (dmg && !dmg->full_frame && dmg->num_ranges == 0) {
		atomic64_inc(&head->metrics.skipped_frames);
		return 0;
	}

	send_start_ns = ktime_get_ns();

	/* Build flip header */
	memset(&fh, 0, sizeof(fh));
	fh.command = cpu_to_le32(head->output_idx == 0 ?
				T6_CMD_FLIP_PRIMARY : T6_CMD_FLIP_SECONDARY);
	fh.payload_size = cpu_to_le32(frame_size);
	fh.target_format = cpu_to_le32(T6_FMT_RGB32);
	fh.y_pitch = cpu_to_le16(stride);
	fh.y_fb_offset = cpu_to_le32(head->fb_addr + sizeof(fh));
	fh.source_format = cpu_to_le32(T6_FMT_RGB32);
	if (head->frame_seq < 10)
		fh.flag = T6_FLAG_RESET;

	if (head->output_idx == 0 && READ_ONCE(t6_primary_partial_updates) &&
	    head->frame_seq >= 10 && dmg && !dmg->full_frame) {
		/*
		 * Primary head, partial damage: write only dirty row
		 * ranges to VRAM, then the flip header.  The firmware
		 * reads the flip directly from VRAM for the primary
		 * head, and VRAM retains unchanged rows from prior
		 * frames.  This is the fast path — sub-ms USB for
		 * typical desktop updates.
		 */
		for (i = 0; i < dmg->num_ranges; i++) {
			u32 offset = dmg->ranges[i].y_start * stride;
			u32 size = dmg->ranges[i].y_count * stride;

			memset(&bch, 0, sizeof(bch));
			bch.payload_length = cpu_to_le32(size);
			bch.payload_address = cpu_to_le32(
				head->fb_addr + sizeof(fh) + offset);
			bch.packet_length = cpu_to_le32(size);

			ret = t6_bulk_write(t6, &bch, sizeof(bch));
			if (ret)
				goto out;
			ret = t6_bulk_write(t6, src + offset, size);
			if (ret)
				goto out;
			total_sent += size;
		}

		/* Flip header last — triggers display after all rows written */
		memset(&bch, 0, sizeof(bch));
		bch.payload_length = cpu_to_le32(sizeof(fh));
		bch.payload_address = cpu_to_le32(head->fb_addr);
		bch.packet_length = cpu_to_le32(sizeof(fh));

		ret = t6_bulk_write(t6, &bch, sizeof(bch));
		if (ret)
			goto out;
		ret = t6_bulk_write(t6, &fh, sizeof(fh));
		if (ret)
			goto out;
		total_sent += sizeof(fh);
	} else if (head->output_idx == 0) {
		/*
		 * Primary head, full frame: first 10 frames (RESET),
		 * or damage exceeded T6_MAX_DIRTY_RANGES.
		 */
		u32 total_payload = sizeof(fh) + frame_size;

		if (!head->video_staging ||
		    total_payload > head->video_staging_size)
			return -ENOMEM;

		memcpy(head->video_staging, &fh, sizeof(fh));
		memcpy(head->video_staging + sizeof(fh), src, frame_size);

		memset(&bch, 0, sizeof(bch));
		bch.payload_length = cpu_to_le32(total_payload);
		bch.payload_address = cpu_to_le32(head->fb_addr);
		bch.packet_length = cpu_to_le32(total_payload);

		ret = t6_bulk_write(t6, &bch, sizeof(bch));
		if (ret)
			goto out;
		ret = t6_bulk_write(t6, head->video_staging, total_payload);
		if (ret)
			goto out;
		total_sent = total_payload;
	} else {
		/*
		 * Secondary head (experimental raw): cmd ring + fb slot
		 * rotation for double-buffered display.  Only reached
		 * when experimental_secondary_raw=1.
		 */
		u8 next_slot = (head->fb_slot_count > 1) ?
			(head->fb_slot_index + 1) % head->fb_slot_count : 0;
		u32 fb_dest = head->fb_slots[next_slot] ?
			      head->fb_slots[next_slot] : head->fb_addr;
		u32 cmd_addr = head->cmd_cursor_addr;
		u32 cmd_step = 1024 * 1024u;

		if (head->cmd_limit_addr > head->cmd_base_addr &&
		    cmd_addr + cmd_step > head->cmd_limit_addr)
			cmd_addr = head->cmd_base_addr;

		if (!head->video_staging)
			return -ENOMEM;
		memcpy(head->video_staging + sizeof(fh), src, frame_size);

		memset(&bch, 0, sizeof(bch));
		bch.payload_length = cpu_to_le32(frame_size);
		bch.payload_address = cpu_to_le32(fb_dest);
		bch.packet_length = cpu_to_le32(frame_size);

		ret = t6_bulk_write(t6, &bch, sizeof(bch));
		if (ret)
			goto out;
		ret = t6_bulk_write(t6, head->video_staging + sizeof(fh),
				    frame_size);
		if (ret)
			goto out;

		fh.y_fb_offset = cpu_to_le32(fb_dest);
		memset(&bch, 0, sizeof(bch));
		bch.payload_length = cpu_to_le32(sizeof(fh));
		bch.payload_address = cpu_to_le32(cmd_addr);
		bch.packet_length = cpu_to_le32(sizeof(fh));

		ret = t6_bulk_write(t6, &bch, sizeof(bch));
		if (ret)
			goto out;
		ret = t6_bulk_write(t6, &fh, sizeof(fh));
		if (ret)
			goto out;

		head->cmd_cursor_addr = cmd_addr + cmd_step;
		head->fb_slot_index = next_slot;
		total_sent = frame_size + sizeof(fh);
	}

	/* Update the damage reference frame */
	if (head->last_sent_frame) {
		if (READ_ONCE(t6_primary_partial_updates) && head->frame_seq >= 10 &&
		    dmg && !dmg->full_frame) {
			for (i = 0; i < dmg->num_ranges; i++) {
				u32 offset = dmg->ranges[i].y_start * stride;
				u32 size = dmg->ranges[i].y_count * stride;

				memcpy(head->last_sent_frame + offset,
				       src + offset, size);
			}
		} else {
			memcpy(head->last_sent_frame, src, frame_size);
		}
		head->last_sent_valid = true;
	}

	head->frame_seq++;
	atomic64_inc(&head->metrics.sent_frames);
	atomic64_add(total_sent, &head->metrics.sent_bytes);
	WRITE_ONCE(head->metrics.last_payload_bytes, total_sent);
	t6_metrics_record_timing(&head->metrics.usb_samples,
				 &head->metrics.usb_ns_total,
				 &head->metrics.last_usb_ns,
				 &head->metrics.max_usb_ns,
				 ktime_get_ns() - send_start_ns);
	t6_metrics_record_timing(&head->metrics.send_samples,
				 &head->metrics.send_ns_total,
				 &head->metrics.last_send_ns,
				 &head->metrics.max_send_ns,
				 ktime_get_ns() - send_start_ns);

out:
	if (ret)
		t6_head_record_send_error(head, ret, "raw");
	return ret;
}

static int t6_send_jpeg_blob(struct t6_device *t6, struct t6_head *head,
			     const void *jpg_data,
			     size_t jpg_len)
{
	struct t6_bulk_header bch;
	struct t6_flip_header fh;
	u8 *video_payload;
	const u8 *jpeg_payload;
	u8 flag;
	u8 fb_slot_index;
	u16 y_pitch;
	u32 cmd_addr;
	u32 cmd_step;
	u32 fb_addr;
	u32 y_block_size;
	u32 total_payload;
	bool wrapped = false;
	u64 usb_start_ns;
	int ret = 0;

	if (!jpg_len)
		return -EINVAL;

	total_payload = sizeof(fh) + jpg_len + T6_JPEG_PADDING_SIZE;
	if (total_payload <= 1024 * 1024u)
		cmd_step = 1024 * 1024u;
	else if (total_payload <= 2 * 1024 * 1024u)
		cmd_step = 2 * 1024 * 1024u;
	else if (total_payload <= 3 * 1024 * 1024u)
		cmd_step = 3 * 1024 * 1024u;
	else
		return -E2BIG;

	cmd_addr = head->cmd_cursor_addr;
	if (head->cmd_limit_addr > head->cmd_base_addr &&
	    cmd_addr + cmd_step > head->cmd_limit_addr) {
		cmd_addr = head->cmd_base_addr;
		wrapped = true;
	}
	if (head->fb_slot_count > 1)
		fb_slot_index = (head->fb_slot_index + 1) % head->fb_slot_count;
	else
		fb_slot_index = 0;
	fb_addr = head->fb_slots[fb_slot_index] ?
		head->fb_slots[fb_slot_index] : head->fb_addr;
	flag = head->frame_seq < 10 || wrapped ? T6_FLAG_RESET : 0;

	y_pitch = ALIGN(head->width, 32);
	y_block_size = y_pitch * ALIGN(head->height, 32) + T6_JPEG_PADDING_SIZE;

	if (total_payload > head->jpeg_staging_size)
		return -E2BIG;
	video_payload = head->jpeg_staging;
	usb_start_ns = ktime_get_ns();

	memset(&fh, 0, sizeof(fh));
	fh.command = cpu_to_le32(head->output_idx == 0 ?
				T6_CMD_FLIP_PRIMARY : T6_CMD_FLIP_SECONDARY);
	fh.payload_size = cpu_to_le32(total_payload - sizeof(fh));
	fh.fence_id = cpu_to_le32(head->frame_seq + 1);
	fh.target_format = cpu_to_le32(T6_FMT_NV12);
	fh.y_pitch = cpu_to_le16(y_pitch);
	fh.uv_pitch = cpu_to_le16(y_pitch);
	fh.y_fb_offset = cpu_to_le32(fb_addr);
	fh.u_offset = cpu_to_le32(fb_addr + y_block_size);
	fh.source_format = cpu_to_le32(T6_FMT_JPEG);
	fh.flag = flag;

	/*
	 * Three cases for placing JPEG data at video_payload + sizeof(fh):
	 *
	 * 1. Pre-placed (native kernel encode path): jpg_data already points to
	 *    video_payload + sizeof(fh) because t6_prepare_frame_jpeg_cmd encodes
	 *    directly into that offset.  No copy needed.
	 *
	 * 2. External buffer (userspace injection, keepalive): completely disjoint
	 *    allocation -- use memcpy.
	 *
	 * 3. Legacy base-pointer alias (jpg_data == video_payload): data starts at
	 *    offset 0 and needs shifting -- use memmove.  Kept for safety but not
	 *    hit by any current caller.
	 */
	if (jpg_data == video_payload + sizeof(fh)) {
		/* Case 1: data already in place, nothing to move */
	} else if (jpg_data != video_payload) {
		memcpy(video_payload + sizeof(fh), jpg_data, jpg_len);
	} else {
		memmove(video_payload + sizeof(fh), jpg_data, jpg_len);
	}
	jpeg_payload = video_payload + sizeof(fh);
	memset(video_payload + sizeof(fh) + jpg_len, 0, T6_JPEG_PADDING_SIZE);
	memcpy(video_payload, &fh, sizeof(fh));

	memset(&bch, 0, sizeof(bch));
	bch.payload_length = cpu_to_le32(total_payload);
	bch.payload_address = cpu_to_le32(cmd_addr);
	bch.packet_length = cpu_to_le32(total_payload);

	ret = t6_bulk_write(t6, &bch, sizeof(bch));
	if (ret)
		goto out;
	ret = t6_bulk_write(t6, video_payload, total_payload);
	if (ret)
		goto out;

	head->frame_seq++;
	head->cmd_cursor_addr = cmd_addr + cmd_step;
	head->fb_slot_index = fb_slot_index;
	atomic64_inc(&head->metrics.sent_frames);
	atomic64_add(total_payload, &head->metrics.sent_bytes);
	WRITE_ONCE(head->metrics.last_payload_bytes, total_payload);
	t6_metrics_record_timing(&head->metrics.usb_samples,
				 &head->metrics.usb_ns_total,
				 &head->metrics.last_usb_ns,
				 &head->metrics.max_usb_ns,
				 ktime_get_ns() - usb_start_ns);

	/* Store last JPEG data for keepalive */
	t6_last_frame_store(head, jpeg_payload, jpg_len);

	/* Start or reset keepalive timer */
	t6_arm_keepalive(head);

out:
	return ret;
}

/*
 * The T6 chip blanks JPEG-decoded outputs that stop receiving frames
 * after roughly 2-3 seconds.  Re-send the last JPEG blob every 2 s
 * to keep the display alive when the compositor has no new content.
 */
static void t6_keepalive_timeout(struct timer_list *t)
{
	struct t6_head *head = container_of(t, struct t6_head, keepalive_timer);

	if (!READ_ONCE(head->connected) || READ_ONCE(head->io_faulted) ||
	    !head->last_jpeg_data)
		return;

	WRITE_ONCE(head->keepalive_due, true);
	t6_queue_head_tx_work(head);
}

/*
 * Boot keepalive: sends cached black frame directly via bulk write,
 * bypassing t6_frame_work and its drm_dev_enter guard.  This keeps
 * the HDMI signal alive during the gap between priming and compositor
 * startup, when drm_dev_enter would fail (DRM not yet registered).
 */
static void t6_boot_keepalive_fn(struct work_struct *work)
{
	struct t6_head *head = container_of(to_delayed_work(work),
					    struct t6_head,
					    boot_keepalive_work);
	struct t6_device *t6 = head->t6;
	struct t6_bulk_header bch;

	if (!READ_ONCE(head->connected) || READ_ONCE(head->io_faulted) ||
	    READ_ONCE(t6->io_faulted) || !READ_ONCE(t6_keepalive_ms))
		return;
	if (!head->last_jpeg_data || !head->last_jpeg_len)
		return;

	mutex_lock(&t6->io_lock);
	/* The compositor owns the output once the CRTC is on; stop here. */
	if (head->crtc_active || !head->last_jpeg_data) {
		mutex_unlock(&t6->io_lock);
		return;
	}
	memset(&bch, 0, sizeof(bch));
	bch.payload_length = cpu_to_le32(head->last_jpeg_len);
	bch.payload_address = cpu_to_le32(head->last_frame_addr);
	bch.packet_length = cpu_to_le32(head->last_jpeg_len);
	t6_bulk_write(t6, &bch, sizeof(bch));
	t6_bulk_write(t6, head->last_jpeg_data, head->last_jpeg_len);
	mutex_unlock(&t6->io_lock);

	atomic64_inc(&head->metrics.keepalive_sent);

	if (t6->wq && READ_ONCE(head->connected))
		mod_delayed_work(t6->wq, &head->boot_keepalive_work,
				 msecs_to_jiffies(T6_BOOT_KEEPALIVE_INTERVAL_MS));
}

static int t6_prepare_frame_jpeg_cmd(struct t6_head *head,
				     const void *pixels,
				     size_t pixel_len)
{
	unsigned int quality;
	size_t tight_len = (size_t)head->width * head->height * 4;
	u64 encode_start_ns;
	int ret;

	if (head->transport == T6_HEAD_TRANSPORT_USER_JPEG)
		return 0;
	if (!READ_ONCE(head->connected))
		return -ENODEV;
	if (!pixels || !pixel_len)
		return -EINVAL;
	if (pixel_len < tight_len)
		return -EINVAL;
	if (!head->jpeg_staging || !head->jpeg_staging_size)
		return -ENOMEM;

	/* Use per-head adaptive quality if active, else global param */
	quality = head->jpeg_adaptive_quality ?
		  head->jpeg_adaptive_quality :
		  READ_ONCE(t6_jpeg_quality);

	encode_start_ns = ktime_get_ns();
	/*
	 * Encode directly into jpeg_staging[sizeof(flip_header)..] so that
	 * t6_send_jpeg_blob can detect the pre-placed case and skip the memmove
	 * that would otherwise shift up to several MB of JPEG data by 48 bytes.
	 * Reserve sizeof(flip_header) at the front and T6_JPEG_PADDING_SIZE at
	 * the end so the total_payload always fits within jpeg_staging_size.
	 */
	ret = t6_jpeg_encode_xrgb8888(pixels,
				     head->width,
				     head->height,
				     head->tx_stride ? head->tx_stride : head->width * 4,
				     quality,
				     head->jpeg_staging + sizeof(struct t6_flip_header),
				     head->jpeg_staging_size -
					sizeof(struct t6_flip_header) -
					T6_JPEG_PADDING_SIZE);
	if (ret < 0) {
		t6_head_record_send_error(head, ret, "jpeg-encode");
		return ret;
	}

	{
		u64 encode_ns = ktime_get_ns() - encode_start_ns;
		u64 encode_ms = encode_ns / 1000000ULL;
		unsigned int target_ms = 30;
		unsigned int q = quality;

		t6_metrics_record_timing(&head->metrics.encode_samples,
					 &head->metrics.encode_ns_total,
					 &head->metrics.last_encode_ns,
					 &head->metrics.max_encode_ns,
					 encode_ns);

		/*
		 * Adaptive quality feedback: if encode is too slow,
		 * drop quality aggressively.  If encode has headroom,
		 * raise quality gently.  Asymmetric steps prevent
		 * oscillation.
		 */
		if (encode_ms > target_ms && q > 25)
			q -= 5;
		else if (encode_ms < target_ms * 7 / 10 && q < 93)
			q += 2;
		head->jpeg_adaptive_quality = clamp(q, 20U, 95U);
	}

	return ret;
}

static int t6_send_prepared_jpeg_cmd(struct t6_device *t6,
				     struct t6_head *head,
				     size_t jpeg_len)
{
	u64 send_start_ns = ktime_get_ns();
	int ret;

	ret = t6_send_jpeg_blob(t6, head,
				head->jpeg_staging + sizeof(struct t6_flip_header),
				jpeg_len);
	if (ret < 0) {
		t6_head_record_send_error(head, ret, "jpeg-cmd");
		return ret;
	}
	t6_metrics_record_timing(&head->metrics.send_samples,
				 &head->metrics.send_ns_total,
				 &head->metrics.last_send_ns,
				 &head->metrics.max_send_ns,
				 ktime_get_ns() - send_start_ns);

	return 0;
}

/*
 * NV12 transport for secondary heads: convert BGRX→NV12 in kernel,
 * send to triple-buffered VRAM fb_slots, flip via direct write.
 * Same protocol as triggerdm's raw YV12/NV12 — single contiguous
 * write of flip_header + NV12 data to fbAddr, with slot rotation.
 *
 * Performance: ~2-3ms conversion + ~3-5ms USB = 5-8ms per frame
 * vs JPEG: ~52ms encode + ~1ms USB = 53ms per frame (10x faster)
 */
static int t6_prepare_frame_nv12(struct t6_head *head,
				 const void *pixels,
				 size_t pixel_len)
{
	u16 y_pitch = ALIGN(head->width, 16);
	u16 uv_pitch = ALIGN(head->width, 16);
	u32 y_size = (u32)y_pitch * head->height;
	u32 uv_size = (u32)uv_pitch * (head->height / 2);
	u32 nv12_size = y_size + uv_size;
	u32 total = sizeof(struct t6_flip_header) + nv12_size +
		    T6_JPEG_PADDING_SIZE;
	u8 *staging;
	u64 encode_start_ns;
	int ret;

	if (!head->jpeg_staging || total > head->jpeg_staging_size)
		return -ENOMEM;
	if (!READ_ONCE(head->connected))
		return -ENODEV;

	staging = head->jpeg_staging;
	encode_start_ns = ktime_get_ns();

	ret = t6_convert_xrgb8888_to_nv12(pixels,
					   head->width, head->height,
					   head->tx_stride ? head->tx_stride :
					   head->width * 4,
					   staging + sizeof(struct t6_flip_header),
					   staging + sizeof(struct t6_flip_header) +
					   y_size,
					   y_pitch, uv_pitch);
	if (ret < 0)
		return ret;

	/* Zero padding after NV12 data */
	memset(staging + sizeof(struct t6_flip_header) + nv12_size,
	       0, T6_JPEG_PADDING_SIZE);

	t6_metrics_record_timing(&head->metrics.encode_samples,
				 &head->metrics.encode_ns_total,
				 &head->metrics.last_encode_ns,
				 &head->metrics.max_encode_ns,
				 ktime_get_ns() - encode_start_ns);

	return (int)nv12_size;
}

/* Flip header for an NV12 frame whose header+data start at fb_addr. */
static void t6_build_nv12_flip(const struct t6_head *head,
			       struct t6_flip_header *fh,
			       u32 fb_addr, size_t nv12_len,
			       u32 fence_id, u8 flag)
{
	u16 pitch = ALIGN(head->width, 16);
	u32 y_size = (u32)pitch * head->height;

	memset(fh, 0, sizeof(*fh));
	fh->command = cpu_to_le32(head->output_idx == 0 ?
				 T6_CMD_FLIP_PRIMARY : T6_CMD_FLIP_SECONDARY);
	fh->payload_size = cpu_to_le32(nv12_len + T6_JPEG_PADDING_SIZE);
	fh->fence_id = cpu_to_le32(fence_id);
	fh->target_format = cpu_to_le32(T6_FMT_NV12);
	fh->y_pitch = cpu_to_le16(pitch);
	fh->uv_pitch = cpu_to_le16(pitch);
	fh->y_fb_offset = cpu_to_le32(fb_addr + sizeof(*fh));
	fh->u_offset = cpu_to_le32(fb_addr + sizeof(*fh) + y_size);
	fh->source_format = cpu_to_le32(T6_FMT_NV12);
	fh->flag = flag;
}

static int t6_send_prepared_nv12(struct t6_device *t6,
				 struct t6_head *head,
				 size_t nv12_len)
{
	struct t6_bulk_header bch;
	struct t6_flip_header fh;
	u32 total_payload;
	u8 fb_slot_index;
	u32 fb_addr;
	u64 send_start_ns = ktime_get_ns();
	int ret;

	total_payload = sizeof(fh) + nv12_len + T6_JPEG_PADDING_SIZE;

	/* Triple-buffer rotation — same as JPEG path */
	if (head->fb_slot_count > 1)
		fb_slot_index = (head->fb_slot_index + 1) % head->fb_slot_count;
	else
		fb_slot_index = 0;

	fb_addr = head->fb_slots[fb_slot_index] ?
		  head->fb_slots[fb_slot_index] : head->fb_addr;

	t6_build_nv12_flip(head, &fh, fb_addr, nv12_len, head->frame_seq + 1,
			   head->frame_seq < 10 ? T6_FLAG_RESET : 0);
	memcpy(head->jpeg_staging, &fh, sizeof(fh));

	/* Single contiguous write: flip_header + NV12 data to fbAddr */
	memset(&bch, 0, sizeof(bch));
	bch.payload_length = cpu_to_le32(total_payload);
	bch.payload_address = cpu_to_le32(fb_addr);
	bch.packet_length = cpu_to_le32(total_payload);

	ret = t6_bulk_write(t6, &bch, sizeof(bch));
	if (ret)
		goto out;
	ret = t6_bulk_write(t6, head->jpeg_staging, total_payload);
	if (ret)
		goto out;

	head->frame_seq++;
	head->fb_slot_index = fb_slot_index;
	atomic64_inc(&head->metrics.sent_frames);
	atomic64_add(total_payload, &head->metrics.sent_bytes);
	WRITE_ONCE(head->metrics.last_payload_bytes, total_payload);
	t6_metrics_record_timing(&head->metrics.usb_samples,
				 &head->metrics.usb_ns_total,
				 &head->metrics.last_usb_ns,
				 &head->metrics.max_usb_ns,
				 ktime_get_ns() - send_start_ns);
	t6_metrics_record_timing(&head->metrics.send_samples,
				 &head->metrics.send_ns_total,
				 &head->metrics.last_send_ns,
				 &head->metrics.max_send_ns,
				 ktime_get_ns() - send_start_ns);

	/* Keepalive: stash last frame for re-send */
	if (head->last_jpeg_cap != head->jpeg_staging_size) {
		u8 *buf = kvmalloc(head->jpeg_staging_size, GFP_KERNEL);

		if (buf) {
			kvfree(head->last_jpeg_data);
			head->last_jpeg_data = buf;
			head->last_jpeg_cap = head->jpeg_staging_size;
		}
	}
	if (head->last_jpeg_cap == head->jpeg_staging_size &&
	    total_payload <= head->jpeg_staging_size) {
		/* Swap instead of copying 3 MB: io_lock is held by all users. */
		u8 *tmp = head->jpeg_staging;

		head->jpeg_staging = head->last_jpeg_data;
		head->last_jpeg_data = tmp;
		head->last_jpeg_len = total_payload;
	} else {
		t6_last_frame_store(head, head->jpeg_staging, total_payload);
	}
	/* The cached header points into this slot: resend it to the same slot. */
	head->last_frame_addr = fb_addr;
	t6_arm_keepalive(head);

out:
	if (ret)
		t6_head_record_send_error(head, ret, "nv12");
	return ret;
}

#include "trigger6_bridge.h"

/*
 * Deprecated userspace JPEG feeder for output 1. Each open file holds a DRM
 * device reference, like the NV12 bridge, so the head stays valid after unplug.
 */
static int t6_jpeg_misc_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct t6_head *head = container_of(misc, struct t6_head, jpeg_miscdev);
	struct t6_bridge_file *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	drm_dev_get(&head->t6->drm);
	ctx->head = head;
	/* First read() still waits for a frame newer than open(). */
	ctx->seen = atomic_read(&head->fb_export_seq);
	file->private_data = ctx;
	return 0;
}

static int t6_jpeg_misc_release(struct inode *inode, struct file *file)
{
	struct t6_bridge_file *ctx = file->private_data;

	drm_dev_put(&ctx->head->t6->drm);
	kfree(ctx);
	return 0;
}

static ssize_t t6_jpeg_misc_write(struct file *file, const char __user *buf,
				  size_t len, loff_t *ppos)
{
	struct t6_bridge_file *ctx = file->private_data;
	struct t6_head *head = ctx->head;
	struct t6_device *t6 = head->t6;
	void *jpg_data;
	int idx;
	int ret;
	u64 send_start_ns;

	if (*ppos != 0)
		return -ESPIPE;
	if (!len || len > head->tx_buf_size)
		return -EINVAL;
	if (!READ_ONCE(head->connected))
		return -ENODEV;
	if (READ_ONCE(head->io_faulted))
		return head->io_last_error ? head->io_last_error : -EIO;
	if (READ_ONCE(t6->io_faulted))
		return t6->io_last_error ? t6->io_last_error : -EIO;
	if (head->transport != T6_HEAD_TRANSPORT_USER_JPEG)
		return -EOPNOTSUPP;

	jpg_data = memdup_user(buf, len);
	if (IS_ERR(jpg_data))
		return PTR_ERR(jpg_data);
	send_start_ns = ktime_get_ns();

	mutex_lock(&t6->io_lock);
	if (!drm_dev_enter(&t6->drm, &idx)) {
		ret = -ENODEV;
		goto out_unlock;
	}

	/* Output switched off: do not wake the monitor or re-arm keepalive. */
	if (head->crtc_active)
		ret = t6_send_jpeg_blob(t6, head, jpg_data, len);
	else
		ret = -ESTALE;
	drm_dev_exit(idx);

out_unlock:
	mutex_unlock(&t6->io_lock);
	kvfree(jpg_data);
	if (ret == -ESTALE)
		return ret;
	if (ret) {
		t6_head_record_send_error(head, ret, "userspace-jpeg");
		t6_trip_transport_fault(t6, head, ret, "userspace-jpeg");
		return ret;
	}
	t6_metrics_record_timing(&head->metrics.send_samples,
				 &head->metrics.send_ns_total,
				 &head->metrics.last_send_ns,
				 &head->metrics.max_send_ns,
				 ktime_get_ns() - send_start_ns);
	return len;
}

static ssize_t t6_jpeg_misc_read(struct file *file, char __user *buf,
				 size_t len, loff_t *ppos)
{
	struct t6_bridge_file *ctx = file->private_data;
	struct t6_head *head = ctx->head;
	struct t6_device *t6 = head->t6;
	int seq, ret, idx;
	size_t frame_len;

	if (*ppos != 0)
		return -ESPIPE;
	if (head->transport != T6_HEAD_TRANSPORT_USER_JPEG)
		return -EOPNOTSUPP;

	/* Wait for a frame newer than the last one this file read */
	ret = wait_event_interruptible(head->fb_export_wq,
		atomic_read(&head->fb_export_seq) != ctx->seen ||
		drm_dev_is_unplugged(&t6->drm));
	if (ret)
		return ret;
	if (!drm_dev_enter(&t6->drm, &idx))
		return -ENODEV;

	/*
	 * tx_lock is also held by t6_plane_atomic_update() while it swaps
	 * fb_export_buf, so the buffer cannot be reused during the copy.
	 */
	mutex_lock(&t6->tx_lock);
	seq = atomic_read(&head->fb_export_seq);
	frame_len = head->fb_export_len;
	if (!head->fb_export_buf || !frame_len)
		ret = -EAGAIN;
	else if (len < frame_len)
		ret = -EINVAL;
	else if (copy_to_user(buf, head->fb_export_buf, frame_len))
		ret = -EFAULT;
	else
		ret = frame_len;
	if (ret > 0)
		ctx->seen = seq;
	mutex_unlock(&t6->tx_lock);
	drm_dev_exit(idx);

	return ret;
}

static __poll_t t6_jpeg_misc_poll(struct file *file,
				  struct poll_table_struct *wait)
{
	struct t6_bridge_file *ctx = file->private_data;
	struct t6_head *head = ctx->head;
	struct t6_device *t6 = head->t6;
	__poll_t mask = 0;

	poll_wait(file, &head->fb_export_wq, wait);

	/* Always writable (for JPEG injection) */
	mask |= EPOLLOUT | EPOLLWRNORM;

	/* Readable when a frame newer than this file's last read exists */
	if (atomic_read(&head->fb_export_seq) != ctx->seen)
		mask |= EPOLLIN | EPOLLRDNORM;

	if (drm_dev_is_unplugged(&t6->drm))
		mask |= EPOLLHUP;

	return mask;
}

static const struct file_operations t6_jpeg_misc_fops = {
	.owner = THIS_MODULE,
	.open = t6_jpeg_misc_open,
	.release = t6_jpeg_misc_release,
	.read = t6_jpeg_misc_read,
	.write = t6_jpeg_misc_write,
	.poll = t6_jpeg_misc_poll,
	.llseek = noop_llseek,
};

static int t6_register_jpeg_device(struct t6_head *head)
{
	int ret;

	if (head->connected && head->output_idx == 0 && t6_primary_userspace_nv12)
		return t6_register_nv12_bridge(head);

	if (!head->connected || head->output_idx != 1 ||
	    head->transport != T6_HEAD_TRANSPORT_USER_JPEG)
		return 0;

	snprintf(head->jpeg_dev_name, sizeof(head->jpeg_dev_name),
		 "trigger6-%03u-%03u-out%u-jpeg",
		 head->t6->udev->bus->busnum,
		 head->t6->udev->devnum,
		 head->output_idx);
	head->jpeg_miscdev.minor = MISC_DYNAMIC_MINOR;
	head->jpeg_miscdev.name = head->jpeg_dev_name;
	head->jpeg_miscdev.fops = &t6_jpeg_misc_fops;
	head->jpeg_miscdev.mode = 0660;

	ret = misc_register(&head->jpeg_miscdev);
	if (ret)
		return ret;

	head->jpeg_misc_registered = true;
	dev_info(&head->t6->udev->dev,
		 "Registered userspace JPEG injection device /dev/%s for output %u\n",
		 head->jpeg_dev_name,
		 head->output_idx);
	return 0;
}

static void t6_unregister_jpeg_device(struct t6_head *head)
{
	if (!head->jpeg_misc_registered)
		return;

	misc_deregister(&head->jpeg_miscdev);
	head->jpeg_misc_registered = false;
}

static void t6_unregister_jpeg_devices(struct t6_device *t6)
{
	unsigned int idx;

	for (idx = 0; idx < T6_OUTPUT_COUNT; idx++) {
		struct t6_head *head = t6_get_head(t6, idx);

		t6_unregister_jpeg_device(head);
	}
}

/*
 * Put the latest content back on an output whose VRAM or stream state was
 * reset (fault recovery, resume). The compositor sends nothing new for a
 * static desktop, so without this the output stays black or stale.
 */
static void t6_resend_last_frame(struct t6_head *head)
{
	struct t6_device *t6 = head->t6;
	bool queue;

	/*
	 * A cached NV12 frame starts with its flip header. Stream state was
	 * reset, so flag it RESET like the first frames of a new stream.
	 * io_lock: t6_send_prepared_nv12() swaps last_jpeg_data under it.
	 */
	mutex_lock(&t6->io_lock);
	if (head->transport == T6_HEAD_TRANSPORT_NV12 && head->last_jpeg_data &&
	    head->last_jpeg_len >= sizeof(struct t6_flip_header))
		((struct t6_flip_header *)head->last_jpeg_data)->flag =
			T6_FLAG_RESET;
	mutex_unlock(&t6->io_lock);

	mutex_lock(&t6->tx_lock);
	head->last_sent_valid = false;
	if (head->transport == T6_HEAD_TRANSPORT_RAW) {
		/*
		 * tx_front holds the last frame the worker took. Copy it into
		 * the pending slot rather than swapping pointers: a worker may
		 * be sending from tx_front right now, and only it may swap.
		 */
		if (!head->tx_pending && head->tx_len &&
		    head->tx_front && head->tx_back) {
			memcpy(head->tx_back, head->tx_front, head->tx_len);
			t6_stage_pending_head_locked(head, head->tx_len,
						     head->width * 4,
						     ktime_get_ns());
		}
	} else if (head->last_jpeg_data && !head->tx_pending) {
		/* Cached header+frame for NV12/JPEG, see last_frame_addr. */
		WRITE_ONCE(head->keepalive_due, true);
	}
	queue = t6_head_has_pending_activity_locked(head);
	mutex_unlock(&t6->tx_lock);

	if (queue)
		t6_queue_head_tx_work(head);
}

/* Interrupt flag 0x10: put the current picture back on every active output. */
static void t6_resend_work(struct work_struct *work)
{
	struct t6_device *t6 = container_of(work, struct t6_device,
					    resend_work);
	unsigned int idx;
	int enter_idx;

	if (!drm_dev_enter(&t6->drm, &enter_idx))
		return;
	for (idx = 0; idx < t6->output_count; idx++) {
		struct t6_head *head = t6_get_head(t6, idx);

		if (READ_ONCE(head->crtc_active))
			t6_resend_last_frame(head);
	}
	drm_dev_exit(enter_idx);
}

static void t6_trip_transport_fault(struct t6_device *t6,
					 struct t6_head *head,
					 int ret,
					 const char *source)
{
	bool already_faulted;

	mutex_lock(&t6->tx_lock);
	already_faulted = head->io_faulted;
	if (!already_faulted) {
		WRITE_ONCE(head->io_faulted, true);
		head->fault_jiffies = jiffies;
		head->io_last_error = ret;
		atomic64_inc(&head->metrics.transport_faults);
		t6_head_set_error(head, ret, source);
		head->tx_pending = false;
		head->tx_len = 0;
		WRITE_ONCE(head->tx_queued_ns, 0);
	}
	mutex_unlock(&t6->tx_lock);

	if (already_faulted)
		return;

	dev_err(&t6->udev->dev,
		"Halting T6 frame traffic on output %u after %s transport error: %d\n",
		head->output_idx, source, ret);
	dev_err(&t6->udev->dev,
		"Output %u disabled; auto-recovery scheduled in 60s\n",
		head->output_idx);

	if (!drm_dev_is_unplugged(&t6->drm)) {
		drm_kms_helper_hotplug_event(&t6->drm);
		/* Schedule auto-recovery reprobe after 60 seconds */
		schedule_delayed_work(&t6->reprobe_work,
				      msecs_to_jiffies(T6_FAULT_RECOVERY_DELAY_MS));
	}
}

static void t6_update_connector_status(struct t6_head *head)
{
	head->connector.status = t6_head_runtime_connected(head) ?
		connector_status_connected : connector_status_disconnected;
}

static void t6_log_head(struct t6_head *head)
{
	const char *scanout_state;

	if (!t6_head_scanout_supported(head))
		scanout_state = "blocked";
	else if (head->t6->manual_only)
		scanout_state = "manual-only";
	else if (READ_ONCE(head->io_faulted))
		scanout_state = "halted";
	else if (READ_ONCE(head->t6->io_faulted))
		scanout_state = "halted";
	else
		scanout_state = "enabled";

	dev_info(&head->t6->udev->dev,
		 "Head %u: %s (%dx%d) transport=%s scanout=%s fb=0x%08x cmd=0x%08x cmd-cursor=0x%08x\n",
		 head->output_idx,
		 head->monitor_name,
		 head->width,
		 head->height,
		 t6_transport_name(head->transport),
		 scanout_state,
		 head->fb_addr,
		 head->cmd_addr,
		 head->cmd_cursor_addr);
	if (head->connected && !t6_head_scanout_supported(head))
		dev_warn(&head->t6->udev->dev,
			 "Head %u is detected but DRM scanout is disabled (no supported transport configured)\n",
			 head->output_idx);
	else if (head->connected && head->transport == T6_HEAD_TRANSPORT_USER_JPEG)
		dev_info(&head->t6->udev->dev,
			 "Head %u is compositor-managed; a userspace daemon must read() raw frames and write() JPEG data to /dev/trigger6-*-out1-jpeg\n",
			 head->output_idx);
	else if (head->connected && head->t6->manual_only)
		dev_info(&head->t6->udev->dev,
			 "Head %u is in manual-only mode; DRM connectors stay disconnected until the module is reloaded with manual_only=0\n",
			 head->output_idx);
}

static bool t6_any_head_disconnected(struct t6_device *t6)
{
	unsigned int idx;

	for (idx = 0; idx < t6->output_count; idx++) {
		if (!t6_get_head(t6, idx)->connected)
			return true;
	}

	return false;
}

static void t6_reprobe_work(struct work_struct *work)
{
	struct t6_device *t6 = container_of(to_delayed_work(work),
					   struct t6_device,
					   reprobe_work);
	unsigned long recovery = msecs_to_jiffies(T6_FAULT_RECOVERY_DELAY_MS);
	unsigned long fault_wait = 0;
	unsigned int recovered = 0;
	bool any_new_connection = false;
	bool changed = false;
	unsigned int idx;
	int enter_idx;
	int ret;

	if (!drm_dev_enter(&t6->drm, &enter_idx))
		return;

	mutex_lock(&t6->io_lock);

	/*
	 * Auto-recover from transport faults once they are older than
	 * T6_FAULT_RECOVERY_DELAY_MS. t6_trip_transport_fault() only marks
	 * the head, so per-head faults must be checked here; the old code
	 * looked only at t6->io_faulted, which nothing sets, and a head
	 * stayed dead until replug. The hotplug event below lets the
	 * compositor see the connector again.
	 */
	WRITE_ONCE(t6->io_faulted, false);
	for (idx = 0; idx < t6->output_count; idx++) {
		struct t6_head *head = t6_get_head(t6, idx);
		unsigned long due;

		if (!READ_ONCE(head->io_faulted))
			continue;
		due = head->fault_jiffies + recovery;
		if (time_before(jiffies, due)) {
			if (!fault_wait || due - jiffies < fault_wait)
				fault_wait = due - jiffies;
			continue;
		}
		mutex_lock(&t6->tx_lock);
		WRITE_ONCE(head->io_faulted, false);
		head->last_sent_valid = false;
		mutex_unlock(&t6->tx_lock);
		/* Restart with RESET-flagged frames, as after chip init. */
		t6_reset_head_stream_state(head);
		recovered |= BIT(idx);
		changed = true;
		dev_info(&t6->udev->dev,
			 "Output %u: clearing transport fault (%d); attempting recovery\n",
			 head->output_idx, head->io_last_error);
	}

	for (idx = 0; idx < t6->output_count; idx++) {
		struct t6_head *head = t6_get_head(t6, idx);
		bool was_connected = head->connected;
		int old_width = head->width;
		int old_height = head->height;
		char old_name[sizeof(head->monitor_name)];

		memcpy(old_name, head->monitor_name, sizeof(old_name));
		ret = t6_probe_head(t6, head);
		if (ret < 0) {
			dev_warn(&t6->udev->dev,
				 "Deferred head %u probe failed: %d\n",
				 head->output_idx, ret);
			continue;
		}

		/*
		 * A monitor on an output the last chip init already
		 * programmed needs no second init: without a monitor at probe,
		 * output 0 is initialised anyway and head->width/height still
		 * match what was programmed. An unplugged output loses that
		 * state, so a replugged monitor gets a full init.
		 */
		if (!was_connected && head->connected &&
		    !(t6->init_mask & BIT(idx))) {
			any_new_connection = true;
		} else if (!was_connected && head->connected &&
			   head->edid_new &&
			   t6_adopt_monitor_timing(t6, head)) {
			/*
			 * Output already programmed (no monitor at probe), but
			 * this monitor wants another timing: switch it like a
			 * mode set, SET_RESOLUTION and enable.
			 */
			ret = t6_set_resolution(t6, head, head->cur_timing);
			if (ret >= 0)
				ret = t6_ctrl_out(t6, T6_REQ_MONITOR_CTRL,
						  head->output_idx, 1, NULL, 0);
			if (ret < 0)
				dev_warn(&t6->udev->dev,
					 "Output %u: switching to the new monitor's timing failed: %d\n",
					 head->output_idx, ret);
			changed = true;
		}
		if (was_connected && !head->connected)
			t6->init_mask &= ~BIT(idx);

		if (was_connected != head->connected ||
		    old_width != head->width ||
		    old_height != head->height ||
		    strncmp(old_name, head->monitor_name, sizeof(old_name)))
			changed = true;
	}

	if (any_new_connection) {
		ret = t6_chip_init(t6, false);
		if (ret < 0)
			dev_warn(&t6->udev->dev,
				 "Deferred chip re-init failed after late head detection: %d\n",
				 ret);
		else
			changed = true;
	}

	for (idx = 0; idx < t6->output_count; idx++) {
		struct t6_head *head = t6_get_head(t6, idx);

		t6_update_connector_status(head);
		if (head->connected) {
			if (!head->jpeg_misc_registered) {
				ret = t6_register_jpeg_device(head);
				if (ret < 0)
					dev_warn(&t6->udev->dev,
						 "Failed to register deferred JPEG device for output %u: %d\n",
						 head->output_idx, ret);
			}
			if (changed)
				t6_log_head(head);
		} else {
			t6_unregister_jpeg_device(head);
		}
	}
	mutex_unlock(&t6->io_lock);
	drm_dev_exit(enter_idx);

	if (drm_dev_is_unplugged(&t6->drm))
		return;

	if (changed)
		t6->reprobe_attempt = 0;

	if (changed)
		drm_kms_helper_hotplug_event(&t6->drm);

	for (idx = 0; idx < t6->output_count; idx++) {
		struct t6_head *head = t6_get_head(t6, idx);

		if ((recovered & BIT(idx)) && READ_ONCE(head->crtc_active))
			t6_resend_last_frame(head);
	}

	if (t6_any_head_disconnected(t6))
		t6_schedule_reprobe(t6);
	else if (fault_wait)
		schedule_delayed_work(&t6->reprobe_work, fault_wait);
}

static void t6_schedule_reprobe(struct t6_device *t6)
{
	unsigned int delay_ms;

	if (t6->reprobe_attempt >= ARRAY_SIZE(t6_reprobe_delays_ms) - 1) {
		delay_ms = t6_reprobe_delays_ms[ARRAY_SIZE(t6_reprobe_delays_ms) - 1];
		t6->reprobe_attempt = ARRAY_SIZE(t6_reprobe_delays_ms) - 1;
	} else {
		delay_ms = t6_reprobe_delays_ms[t6->reprobe_attempt++];
	}
	schedule_delayed_work(&t6->reprobe_work,
			      msecs_to_jiffies(delay_ms));
}

static void t6_frame_defer_work(struct work_struct *work)
{
	struct t6_head *head = container_of(to_delayed_work(work),
				   struct t6_head, tx_defer_work);

	t6_queue_head_tx_work(head);
}

static void t6_frame_work(struct work_struct *work)
{
	struct t6_head *head = container_of(work, struct t6_head, tx_work);
	struct t6_device *t6 = head->t6;
	size_t len = 0;
	int ret = 0;
	int idx;
	int encoded_len = 0;
	bool keepalive_due = false;
	bool io_locked = false;
	enum t6_tx_defer_reason reason;
	unsigned long delay;
	u64 queued_ns = 0;
	u64 queue_wait_ns;
	struct t6_frame_damage damage;

	atomic64_inc(&t6->metrics.tx_work_runs);
	if (READ_ONCE(t6->io_faulted) || READ_ONCE(head->io_faulted))
		return;

	mutex_lock(&t6->tx_lock);
	if (!t6_pop_pending_frame_locked(head, &len, &queued_ns)) {
		if (READ_ONCE(head->keepalive_due) && head->last_jpeg_data) {
			WRITE_ONCE(head->keepalive_due, false);
			keepalive_due = true;
		} else {
			mutex_unlock(&t6->tx_lock);
			atomic64_inc(&t6->metrics.tx_work_empty);
			return;
		}
	}
	mutex_unlock(&t6->tx_lock);

	if (!t6_head_runtime_connected(head))
		return;

	if (t6_should_yield_turn(head, len, queued_ns, keepalive_due))
		return;

	delay = t6_head_tx_delay_jiffies(t6, head, &reason);
	if (delay) {
		t6_defer_head_activity(head, len, queued_ns, keepalive_due,
				      delay, reason);
		return;
	}

	if (!keepalive_due && head->transport == T6_HEAD_TRANSPORT_JPEG_CMD) {
		encoded_len = t6_prepare_frame_jpeg_cmd(head, head->tx_front, len);
		if (encoded_len < 0) {
			t6_trip_transport_fault(t6, head, encoded_len, "drm");
			dev_warn_ratelimited(&t6->udev->dev,
					     "Frame prepare failed on output %u: %d\n",
					     head->output_idx, encoded_len);
			return;
		}
	}

	/* Compute damage before io_lock — this is pure CPU work */
	if (!keepalive_due && (head->transport == T6_HEAD_TRANSPORT_RAW ||
			       head->transport == T6_HEAD_TRANSPORT_NV12))
		t6_compute_frame_damage(head, head->tx_front, &damage);

	/* NV12: skip conversion entirely when zero damage */
	if (!keepalive_due && head->transport == T6_HEAD_TRANSPORT_NV12) {
		if (!damage.full_frame && damage.num_ranges == 0) {
			atomic64_inc(&head->metrics.skipped_frames);
			return;
		}
		if (t6_primary_userspace_nv12 && head->output_idx == 0) {
			mutex_lock(&t6->io_lock);
			io_locked = true;
		}
		encoded_len = t6_prepare_frame_nv12(head, head->tx_front, len);
		if (encoded_len < 0) {
			if (io_locked)
				mutex_unlock(&t6->io_lock);
			t6_trip_transport_fault(t6, head, encoded_len, "drm");
			dev_warn_ratelimited(&t6->udev->dev,
					     "NV12 convert failed on output %u: %d\n",
					     head->output_idx, encoded_len);
			return;
		}
	}

	if (!io_locked)
		mutex_lock(&t6->io_lock);
	delay = t6_head_tx_delay_jiffies(t6, head, &reason);
	if (delay) {
		mutex_unlock(&t6->io_lock);
		t6_defer_head_activity(head, len, queued_ns, keepalive_due,
				      delay, reason);
		return;
	}

	if (!drm_dev_enter(&t6->drm, &idx)) {
		mutex_unlock(&t6->io_lock);
		return;
	}
	/* atomic_disable clears crtc_active under io_lock; send nothing after. */
	if (!head->crtc_active) {
		drm_dev_exit(idx);
		mutex_unlock(&t6->io_lock);
		return;
	}
	if (!keepalive_due && queued_ns) {
		queue_wait_ns = ktime_get_ns() - queued_ns;
		t6_metrics_record_timing(&head->metrics.queue_wait_samples,
					 &head->metrics.queue_wait_ns_total,
					 &head->metrics.last_queue_wait_ns,
					 &head->metrics.max_queue_wait_ns,
					 queue_wait_ns);
	}

	if (keepalive_due) {
		if (head->transport == T6_HEAD_TRANSPORT_RAW ||
		    head->transport == T6_HEAD_TRANSPORT_NV12) {
			/*
			 * Re-send the cached header+frame to the slot it was
			 * written to. fb_slot_index is reset by chip init, so
			 * it no longer matches the slot the header points at.
			 */
			struct t6_bulk_header ka_bch;

			memset(&ka_bch, 0, sizeof(ka_bch));
			ka_bch.payload_length = cpu_to_le32(head->last_jpeg_len);
			ka_bch.payload_address = cpu_to_le32(head->last_frame_addr);
			ka_bch.packet_length = cpu_to_le32(head->last_jpeg_len);
			ret = t6_bulk_write(t6, &ka_bch, sizeof(ka_bch));
			if (!ret)
				ret = t6_bulk_write(t6, head->last_jpeg_data,
						    head->last_jpeg_len);
		} else {
			ret = t6_send_jpeg_blob(t6, head,
					       head->last_jpeg_data,
					       head->last_jpeg_len);
		}
		if (!ret) {
			atomic64_inc(&head->metrics.keepalive_sent);
			t6_arm_keepalive(head);
		}
		else {
			atomic64_inc(&head->metrics.keepalive_errors);
			t6_head_record_send_error(head, ret, "keepalive");
		}
	} else if (head->transport == T6_HEAD_TRANSPORT_RAW) {
		ret = t6_send_frame_raw(t6, head, head->tx_front, len, &damage);
	} else if (head->transport == T6_HEAD_TRANSPORT_NV12) {
		ret = t6_send_prepared_nv12(t6, head, encoded_len);
	} else {
		ret = t6_send_prepared_jpeg_cmd(t6, head, encoded_len);
	}
	mutex_unlock(&t6->io_lock);
	drm_dev_exit(idx);

	head->tx_last_jiffies = jiffies;
	t6->tx_next_head = (head->output_idx + 1) % t6->output_count;

	if (ret) {
		if (keepalive_due)
			return;
		t6_trip_transport_fault(t6, head, ret, "drm");
		dev_warn_ratelimited(&t6->udev->dev,
				     "Frame send failed on output %u: %d\n",
				     head->output_idx, ret);
		return;
	}

	/* Update NV12 damage reference after successful send */
	if (!keepalive_due && head->transport == T6_HEAD_TRANSPORT_NV12 &&
	    head->last_sent_frame) {
		u32 stride = head->width * 4;
		u32 frame_size = stride * head->height;

		memcpy(head->last_sent_frame, head->tx_front, frame_size);
		head->last_sent_valid = true;
	}

	/* New frames can arrive while USB or JPEG work is in flight, so pick the next head now. */
	t6_queue_next_pending_head(t6);
}

/* ------------------------------------------------------------------
 * DRM pipe callbacks
 * ------------------------------------------------------------------
 */

static void t6_crtc_atomic_enable(struct drm_crtc *crtc,
				  struct t6_atomic_state *state)
{
	struct t6_head *head = t6_head_from_crtc(crtc);
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	struct t6_device *t6 = head->t6;
	int idx;

	if (!crtc_state)
		return;

	/*
	 * Mark the output as compositor-owned before anything else: the boot
	 * keepalive checks this under io_lock and stops resending the primed
	 * frame. Set even while the head is faulted, so recovery resumes.
	 */
	mutex_lock(&t6->io_lock);
	head->crtc_active = true;
	mutex_unlock(&t6->io_lock);

	if (!t6_head_runtime_connected(head))
		return;

	/* Cancel boot keepalive — compositor is taking over */
	cancel_delayed_work_sync(&head->boot_keepalive_work);
	timer_delete_sync(&head->keepalive_timer);

	if (!drm_dev_enter(&t6->drm, &idx))
		return;
	mutex_lock(&t6->io_lock);

	/*
	 * Switch resolution if compositor requested a different mode. The
	 * vendor Windows driver sends only SET_RESOLUTION for the output,
	 * followed by its enable (below). The inherited sequence wrapped it in
	 * 0x24 (HDMI audio off/on), re-sent every head with 0x31 and
	 * committed output 0's gamma LUT.
	 */
	{
		struct drm_display_mode *mode = &crtc_state->mode;
		int new_w = mode->hdisplay;
		int new_h = mode->vdisplay;
		int new_r = drm_mode_vrefresh(mode);
		u8 blob[T6_RES_ENTRY_SIZE];
		bool found = t6_head_find_timing(head, mode, blob);

		/*
		 * Compare whole timings, not only sizes: 1080p60 -> 1080p30
		 * keeps the size, and two 1920x1200@60 timings can differ.
		 */
		if (new_w && new_h && !found) {
			dev_warn(&t6->udev->dev,
				 "Output %u: no timing for %dx%d@%d, keeping %dx%d\n",
				 head->output_idx, new_w, new_h, new_r,
				 head->width, head->height);
		} else if (new_w && new_h &&
			   (!head->cur_timing_valid ||
			    memcmp(blob, head->cur_timing, sizeof(blob)))) {
			int ret = t6_set_resolution(t6, head, blob);

			if (ret >= 0) {
				mutex_lock(&t6->tx_lock);
				head->width = new_w;
				head->height = new_h;
				head->refresh = new_r;
				head->last_sent_valid = false;
				mutex_unlock(&t6->tx_lock);
				dev_info(&t6->udev->dev,
					 "Output %u: mode changed to %dx%d@%d\n",
					 head->output_idx, new_w, new_h, new_r);
			} else {
				dev_warn(&t6->udev->dev,
					 "Mode set %dx%d failed on output %u: %d\n",
					 new_w, new_h, head->output_idx, ret);
			}
		}
	}

	{
		int ret = t6_ctrl_out(t6, T6_REQ_MONITOR_CTRL,
				      head->output_idx, 1, NULL, 0);
		if (ret < 0)
			dev_warn(&t6->udev->dev,
				 "Monitor enable failed on output %u: %d\n",
				 head->output_idx, ret);
	}
	mutex_unlock(&t6->io_lock);
	drm_dev_exit(idx);
}

static void t6_crtc_atomic_disable(struct drm_crtc *crtc,
				   struct t6_atomic_state *state)
{
	struct t6_head *head = t6_head_from_crtc(crtc);
	struct t6_device *t6 = head->t6;
	int idx;

	/*
	 * Stop ALL frame activity before turning the output off.
	 * Without this, the keepalive timer re-sends frames which
	 * wakes the monitor back up in a power-on/off cycle.
	 *
	 * Clearing crtc_active under io_lock first waits for any send in
	 * flight (including a helper write through the NV12 bridge, which
	 * nothing here can cancel) and makes every later send a no-op, so
	 * nothing re-arms the keepalive timer after it is deleted below.
	 */
	mutex_lock(&t6->io_lock);
	head->crtc_active = false;
	mutex_unlock(&t6->io_lock);

	cancel_delayed_work_sync(&head->boot_keepalive_work);
	timer_delete_sync(&head->keepalive_timer);
	cancel_delayed_work_sync(&head->tx_defer_work);
	cancel_work_sync(&head->tx_work);
	WRITE_ONCE(head->keepalive_due, false);

	/*
	 * Clear cached keepalive data so no stale resend can occur. Under
	 * io_lock: t6_send_prepared_nv12() swaps this buffer with
	 * jpeg_staging, and a free racing that swap left jpeg_staging
	 * pointing at freed memory.
	 */
	mutex_lock(&t6->io_lock);
	kvfree(head->last_jpeg_data);
	head->last_jpeg_data = NULL;
	head->last_jpeg_len = 0;
	head->last_jpeg_cap = 0;
	head->last_frame_addr = 0;
	mutex_unlock(&t6->io_lock);

	if (!READ_ONCE(head->connected))
		return;

	if (!drm_dev_enter(&t6->drm, &idx))
		return;
	mutex_lock(&t6->io_lock);

	{
		int ret = t6_ctrl_out(t6, T6_REQ_MONITOR_CTRL,
				      head->output_idx, 0, NULL, 0);
		if (ret < 0)
			dev_warn(&t6->udev->dev,
				 "Monitor disable failed on output %u: %d\n",
				 head->output_idx, ret);
	}
	mutex_unlock(&t6->io_lock);
	drm_dev_exit(idx);
}

/*
 * USB displays have no hardware vblank interrupt. Tell the atomic helper to
 * fake vblank events so drm_atomic_helper_wait_for_flip_done() doesn't block
 * indefinitely.
 */
static int t6_crtc_atomic_check(struct drm_crtc *crtc,
				struct t6_atomic_state *state)
{
	struct drm_crtc_state *crtc_state =
		drm_atomic_get_new_crtc_state(state, crtc);

	if (!crtc_state)
		return 0;
	crtc_state->no_vblank = true;
	return 0;
}

static int t6_plane_atomic_check(struct drm_plane *plane,
				 struct t6_atomic_state *state)
{
	struct drm_plane_state *new_plane_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *new_crtc_state = NULL;

	if (!new_plane_state)
		return 0;
	if (new_plane_state->crtc)
		new_crtc_state = drm_atomic_get_new_crtc_state(
			state, new_plane_state->crtc);

	return drm_atomic_helper_check_plane_state(new_plane_state,
						   new_crtc_state,
						   DRM_PLANE_NO_SCALING,
						   DRM_PLANE_NO_SCALING,
						   false,
						   new_crtc_state &&
						   new_crtc_state->enable);
}

static void t6_plane_atomic_update(struct drm_plane *plane,
				   struct t6_atomic_state *atomic_state)
{
	struct drm_plane_state *state = drm_atomic_get_new_plane_state(atomic_state, plane);
	struct drm_shadow_plane_state *shadow;
	struct drm_framebuffer *fb;
	struct t6_head *head = t6_head_from_plane(plane);
	struct t6_device *t6;
	void *vaddr;
	size_t src_offset;
	size_t frame_len;
	int idx;
	int ret;

	if (!state)
		return;

	fb = state->fb;
	if (!fb)
		return;

	/*
	 * Scan out from the plane's source rectangle, not the framebuffer
	 * origin. The atomic check keeps src inside the fb and, without
	 * scaling, the same size as the mode.
	 */
	src_offset = (size_t)(state->src.y1 >> 16) * fb->pitches[0] +
		     (size_t)(state->src.x1 >> 16) * fb->format->cpp[0];

	/* Userspace owns frame injection for the hybrid secondary JPEG path.
	 * Capture the compositor's framebuffer and make it available to the
	 * userspace daemon via read() on the JPEG misc device.
	 */
	if (head->transport == T6_HEAD_TRANSPORT_USER_JPEG) {
		struct drm_shadow_plane_state *export_shadow;
		void *export_vaddr;
		u32 export_stride;
		size_t export_len;
		u8 *tmp;
		u64 stage_start_ns;

		export_shadow = to_drm_shadow_plane_state(state);
		export_vaddr = export_shadow->data[0].vaddr;
		if (!export_vaddr)
			return;
		export_vaddr += src_offset;
		export_stride = fb->pitches[0];

		export_len = (size_t)head->width * head->height * 4;
		if (export_len > head->tx_buf_size || !head->fb_export_buf)
			return;

		/*
		 * Write tightly packed XRGB8888 into tx_back for userspace
		 * export. tx_lock serialises this with t6_jpeg_misc_read(),
		 * which copies fb_export_buf to userspace under the same lock.
		 */
		mutex_lock(&head->t6->tx_lock);
		stage_start_ns = ktime_get_ns();
		if (t6_pack_xrgb8888_tight(head,
					  export_vaddr,
					  export_stride,
					  head->tx_back,
					  head->tx_buf_size)) {
			mutex_unlock(&head->t6->tx_lock);
			return;
		}
		t6_metrics_record_timing(&head->metrics.stage_samples,
					 &head->metrics.stage_ns_total,
					 &head->metrics.last_stage_ns,
					 &head->metrics.max_stage_ns,
					 ktime_get_ns() - stage_start_ns);

		/* Swap tx_back and fb_export_buf so the reader gets the fresh frame
		 * while the next pipe_update writes to the other buffer.
		 */
		tmp = head->fb_export_buf;
		head->fb_export_buf = head->tx_back;
		head->fb_export_len = export_len;
		head->tx_back = tmp;
		atomic_inc(&head->fb_export_seq);
		mutex_unlock(&head->t6->tx_lock);
		wake_up_interruptible(&head->fb_export_wq);
		return;
	}

	t6 = head->t6;
	if (t6->manual_only)
		return;
	if (READ_ONCE(t6->io_faulted))
		return;
	if (!t6_head_runtime_connected(head))
		return;

	shadow = to_drm_shadow_plane_state(state);

	vaddr = shadow->data[0].vaddr;
	if (!vaddr)
		return;
	vaddr += src_offset;

	if (head->output_idx == 0 && t6_primary_userspace_nv12 &&
	    t6_bridge_export(head, vaddr, fb->pitches[0]))
		return;

	/*
	 * Kernel 6.17 can hand us a stale old_state during drm_dev_register().
	 * Queue the latest full frame and let the worker perform USB I/O outside
	 * the atomic commit path.
	 *
	 * Queue depth stays bounded to one pending frame per head: tx_front may be
	 * in flight on the worker while tx_back remains the pending slot. A newer
	 * update overwrites the previous pending contents instead of getting dropped.
	 */
	{
		int w = READ_ONCE(head->width);
		int h = READ_ONCE(head->height);

		if (!w || !h)
			return;
		frame_len = (size_t)w * h * 4;
	}
	if (!fb->pitches[0] || frame_len > head->tx_buf_size)
		return;

	if (!drm_dev_enter(&t6->drm, &idx))
		return;

	atomic64_inc(&head->metrics.queue_attempts);
	/*
	 * Use a blocking lock rather than trylock.  The double-buffer design
	 * (tx_front in flight, tx_back as the pending slot) exists precisely so
	 * that a new frame can overwrite the pending slot while the worker sends
	 * from tx_front.  Dropping frames at this point defeats that design and
	 * was the source of the high busy-drop counts observed on both the raw
	 * and JPEG heads.  tx_lock is held for only microseconds by the worker
	 * (pointer swaps), so contention here is negligible.
	 */
	mutex_lock(&t6->tx_lock);
	{
		u64 stage_start_ns = ktime_get_ns();

		ret = t6_pack_xrgb8888_tight(head,
				   vaddr,
				   fb->pitches[0],
				   head->tx_back,
				   head->tx_buf_size);
		if (!ret)
			t6_metrics_record_timing(&head->metrics.stage_samples,
						 &head->metrics.stage_ns_total,
						 &head->metrics.last_stage_ns,
						 &head->metrics.max_stage_ns,
						 ktime_get_ns() - stage_start_ns);
	}
	if (ret) {
		mutex_unlock(&t6->tx_lock);
		goto out_exit;
	}
	t6_stage_pending_head_locked(head,
					 frame_len,
					 head->width * 4,
					 ktime_get_ns());
	mutex_unlock(&t6->tx_lock);

	t6_queue_head_tx_work(head);

out_exit:
	drm_dev_exit(idx);
}

static const struct drm_crtc_helper_funcs t6_crtc_helper_funcs = {
	.atomic_enable = t6_crtc_atomic_enable,
	.atomic_disable = t6_crtc_atomic_disable,
	.atomic_check = t6_crtc_atomic_check,
};

static const struct drm_crtc_funcs t6_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};

static const struct drm_plane_helper_funcs t6_plane_helper_funcs = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check = t6_plane_atomic_check,
	.atomic_update = t6_plane_atomic_update,
};

static const struct drm_plane_funcs t6_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	DRM_GEM_SHADOW_PLANE_FUNCS,
};

static const struct drm_encoder_funcs t6_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

/* ------------------------------------------------------------------
 * DRM driver
 * ------------------------------------------------------------------
 */

DEFINE_DRM_GEM_FOPS(t6_fops);

#ifndef T6_HAVE_DRM_SET_DMA_DEV
/* Kernels before drm_dev_set_dma_dev(): import dma-bufs against the USB host controller. */
static struct drm_gem_object *t6_gem_prime_import(struct drm_device *dev,
						  struct dma_buf *buf)
{
	struct t6_device *t6 = container_of(dev, struct t6_device, drm);

	return drm_gem_prime_import_dev(dev, buf,
					t6->dmadev ? t6->dmadev : dev->dev);
}
#endif

static const struct drm_driver t6_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.major = 1,
	.minor = 0,
	.fops = &t6_fops,
	DRM_GEM_SHMEM_DRIVER_OPS,
#ifndef T6_HAVE_DRM_SET_DMA_DEV
	.gem_prime_import = t6_gem_prime_import,
#endif
};

/* ------------------------------------------------------------------
 * Interrupt endpoint (0x83)
 *
 * The vendor Windows driver keeps a read pending on this endpoint. Its read
 * handler drops events without bit 0x04 in byte 0 (the adapter also sends
 * 0x20 events after start-up and hotplug) and tests the flags in byte 19 in
 * this order: 0x04 = frame displayed (fence id at byte 12), 0x01/0x02 =
 * monitor connection changed (it then re-reads status for each output),
 * 0x10 = resend the current picture on every output. Byte 19 is not the flip command: on a single
 * output adapter every flip reports 0x04.
 * ------------------------------------------------------------------
 */

#define T6_INT_MAX_CONSECUTIVE_ERRORS	10

static void t6_int_event(struct t6_device *t6, const u8 *buf, unsigned int len)
{
	u8 flags;

	atomic64_inc(&t6->int_events);
	if (len <= T6_INT_FLAGS_OFFSET || !(buf[0] & T6_INT_EVT_VALID))
		return;
	flags = buf[T6_INT_FLAGS_OFFSET];

	if (flags & T6_INT_FLAG_FLIP_DONE) {
		/*
		 * The event names no output. Credit output 0 unless only
		 * output 1 is scanning out.
		 */
		struct t6_head *head = t6_get_head(t6, 0);

		if (!READ_ONCE(head->crtc_active) &&
		    READ_ONCE(t6_get_head(t6, 1)->crtc_active))
			head = t6_get_head(t6, 1);
		WRITE_ONCE(head->last_done_fence,
			   le32_to_cpup((const __le32 *)(buf + T6_INT_FENCE_OFFSET)));
		atomic64_inc(&head->metrics.flips_done);
		return;
	}

	if (flags & (T6_INT_FLAG_STATUS_OUT0 | T6_INT_FLAG_STATUS_OUT1)) {
		atomic64_inc(&t6->int_hotplug);
		/*
		 * Re-check status now instead of at the next poll, but never
		 * before probe has finished: reprobe_work registers misc
		 * devices and updates connectors, which probe is still setting
		 * up. Probe schedules its own check when it is done.
		 */
		if (READ_ONCE(t6->probed) && !drm_dev_is_unplugged(&t6->drm))
			mod_delayed_work(system_wq, &t6->reprobe_work, 0);
		return;
	}

	if (flags & T6_INT_FLAG_WAKE) {
		/*
		 * The vendor driver marks every active output "resend the
		 * current frame" (the same flag it sets after a failed send)
		 * and releases threads waiting on the adapter
		 * (T6InDisp.dll 0x1800f5448). This driver never waits on the
		 * adapter, so only the resend applies; it takes mutexes, so
		 * it runs from a work item.
		 */
		atomic64_inc(&t6->int_wake);
		dev_info_ratelimited(&t6->udev->dev,
				     "T6 event 0x10: resending the current picture\n");
		if (READ_ONCE(t6->probed) && !drm_dev_is_unplugged(&t6->drm))
			schedule_work(&t6->resend_work);
		return;
	}

	atomic64_inc(&t6->int_unknown);
	dev_info_ratelimited(&t6->udev->dev, "T6 event (%u bytes): %*ph\n",
			     len, (int)min(len, (unsigned int)T6_INT_PACKET_SIZE), buf);
}

static void t6_int_complete(struct urb *urb)
{
	struct t6_device *t6 = urb->context;
	int ret;

	switch (urb->status) {
	case 0:
		t6->int_consecutive_errors = 0;
		t6_int_event(t6, urb->transfer_buffer, urb->actual_length);
		break;
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
	case -ENODEV:
		return;		/* killed or unplugged */
	default:
		atomic64_inc(&t6->int_errors);
		if (++t6->int_consecutive_errors >= T6_INT_MAX_CONSECUTIVE_ERRORS) {
			dev_err_ratelimited(&t6->udev->dev,
					    "Interrupt endpoint failing (%d); stopped until resume\n",
					    urb->status);
			WRITE_ONCE(t6->int_running, false);
			return;
		}
		break;
	}

	if (!READ_ONCE(t6->int_running))
		return;
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret && ret != -EPERM && ret != -ENODEV)
		dev_err_ratelimited(&t6->udev->dev,
				    "Interrupt URB resubmit failed: %d\n", ret);
}

/* Non-fatal: without the endpoint the driver works as before. */
static void t6_int_init(struct t6_device *t6, struct usb_interface *intf)
{
	struct usb_endpoint_descriptor *ep;
	unsigned int len;

	if (usb_find_int_in_endpoint(intf->cur_altsetting, &ep)) {
		dev_info(&intf->dev, "No interrupt IN endpoint; events not read\n");
		return;
	}
	if (ep->bEndpointAddress != T6_EP_INT_IN)
		dev_info(&intf->dev, "Interrupt IN endpoint is 0x%02x, expected 0x%02x\n",
			 ep->bEndpointAddress, T6_EP_INT_IN);

	len = max_t(unsigned int, usb_endpoint_maxp(ep), T6_INT_PACKET_SIZE);
	t6->int_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!t6->int_urb)
		return;
	t6->int_buf = usb_alloc_coherent(t6->udev, len, GFP_KERNEL, &t6->int_dma);
	if (!t6->int_buf) {
		usb_free_urb(t6->int_urb);
		t6->int_urb = NULL;
		return;
	}
	usb_fill_int_urb(t6->int_urb, t6->udev,
			 usb_rcvintpipe(t6->udev, ep->bEndpointAddress),
			 t6->int_buf, len, t6_int_complete, t6, ep->bInterval);
	t6->int_urb->transfer_dma = t6->int_dma;
	t6->int_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
}

static void t6_int_start(struct t6_device *t6)
{
	int ret;

	if (!t6->int_urb)
		return;
	t6->int_consecutive_errors = 0;
	WRITE_ONCE(t6->int_running, true);
	ret = usb_submit_urb(t6->int_urb, GFP_KERNEL);
	if (ret) {
		WRITE_ONCE(t6->int_running, false);
		dev_warn(&t6->udev->dev, "Interrupt URB submit failed: %d\n", ret);
	}
}

static void t6_int_stop(struct t6_device *t6)
{
	WRITE_ONCE(t6->int_running, false);
	if (t6->int_urb)
		usb_kill_urb(t6->int_urb);
}

static void t6_int_free(struct t6_device *t6)
{
	t6_int_stop(t6);
	if (t6->int_buf)
		usb_free_coherent(t6->udev, t6->int_urb->transfer_buffer_length,
				  t6->int_buf, t6->int_dma);
	usb_free_urb(t6->int_urb);
	t6->int_buf = NULL;
	t6->int_urb = NULL;
}

/* ------------------------------------------------------------------
 * USB probe / disconnect
 * ------------------------------------------------------------------
 */

static int t6_usb_probe(struct usb_interface *intf,
			const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct t6_device *t6;
	struct drm_device *drm;
	__le32 ram_le;
	int ret;
	unsigned int head_idx;

	t6 = devm_drm_dev_alloc(&intf->dev, &t6_drm_driver,
				struct t6_device, drm);
	if (IS_ERR(t6))
		return PTR_ERR(t6);

	drm = &t6->drm;
	t6->udev = usb_get_dev(udev);
	t6->intf = intf;
	t6->manual_only = t6_manual_only;
	t6->io_faulted = false;
	t6->io_last_error = 0;
	t6_reset_device_metrics(t6);

	/* DMA device for GEM */
	t6->dmadev = usb_intf_get_dma_device(intf);
#ifdef T6_HAVE_DRM_SET_DMA_DEV
	if (t6->dmadev)
		drm_dev_set_dma_dev(drm, t6->dmadev);
#endif

	usb_set_intfdata(intf, t6);
	t6_int_init(t6, intf);

	/* Query RAM */
	ret = t6_ctrl_in(t6, T6_REQ_GET_RAM, 0, 0, &ram_le, 4);
	if (ret < 4) {
		dev_err(&intf->dev, "RAM query failed: %d\n", ret);
		ret = -EIO;
		goto err_put;
	}
	t6->ram_mb = le32_to_cpu(ram_le);
	if (t6->ram_mb < 32 || t6->ram_mb > 512) {
		dev_err(&intf->dev, "Unexpected T6 RAM size: %u MB\n",
			t6->ram_mb);
		ret = -ENODEV;
		goto err_put;
	}
	dev_info(&intf->dev, "T6 RAM: %u MB\n", t6->ram_mb);
	t6_read_device_info(t6);

	/*
	 * Every head gets its defaults (timers, work items and wait queues
	 * are cancelled and woken for all of them), but only the outputs the
	 * adapter has are probed, get buffers, DRM objects and device nodes.
	 */
	for (head_idx = 0; head_idx < T6_OUTPUT_COUNT; head_idx++)
		t6_head_defaults(t6, t6_get_head(t6, head_idx), head_idx);
	for (head_idx = 0; head_idx < t6->output_count; head_idx++) {
		struct t6_head *head = t6_get_head(t6, head_idx);

		ret = t6_probe_head(t6, head);
		if (ret < 0)
			dev_warn(&intf->dev,
				 "Head %u probe failed, using defaults: %d\n",
				 head_idx, ret);
	}

	/*
	 * Retry undetected heads in a tight poll loop before DRM
	 * registration so the compositor sees all monitors in one
	 * hotplug event instead of staggered appearances.
	 */
	if (t6_any_head_disconnected(t6)) {
		unsigned long deadline = jiffies +
			msecs_to_jiffies(T6_INITIAL_PROBE_WINDOW_MS);

		while (time_before(jiffies, deadline) &&
		       t6_any_head_disconnected(t6)) {
			msleep(T6_INITIAL_PROBE_RETRY_MS);
			for (head_idx = 0; head_idx < t6->output_count;
			     head_idx++) {
				struct t6_head *head =
					t6_get_head(t6, head_idx);
				if (head->connected)
					continue;
				ret = t6_probe_head(t6, head);
				if (ret >= 0 && head->connected)
					dev_info(&intf->dev,
						 "Head %u detected during probe window\n",
						 head_idx);
			}
		}
	}

	for (head_idx = 0; head_idx < t6->output_count; head_idx++) {
		struct t6_head *head = t6_get_head(t6, head_idx);

		if (!head->connected)
			continue;

		t6_log_head(head);
	}

	mutex_init(&t6->tx_lock);
	mutex_init(&t6->io_lock);
	mutex_init(&t6->bulk_lock);
	t6->tx_next_head = 0;
	/* Before the first goto err_buffers, which cancels this work. */
	INIT_DELAYED_WORK(&t6->reprobe_work, t6_reprobe_work);
	INIT_WORK(&t6->resend_work, t6_resend_work);
	t6->reprobe_attempt = 0;

	t6->wq = alloc_workqueue("t6_io", WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
	if (!t6->wq) {
		ret = -ENOMEM;
		goto err_buffers;
	}

	ret = t6_alloc_head_buffers(t6);
	if (ret < 0) {
		ret = -ENOMEM;
		goto err_buffers;
	}

	/* Initialize T6 chip; this also reads each output's mode table. */
	ret = t6_chip_init(t6, true);
	if (ret < 0) {
		dev_err(&intf->dev, "Chip init failed: %d\n", ret);
		goto err_buffers;
	}

	t6_int_start(t6);

	/*
	 * Prime connected heads with black frames to stabilize the HDMI
	 * signal.  Multi-output chips need a few frames to clear internal
	 * buffers and lock the signal before real content arrives.
	 * Prime secondary heads FIRST — they typically take longer to
	 * lock the HDMI signal, so starting them earlier balances the
	 * visual appearance (both monitors come up closer together).
	 * Matches the triggerdm reference implementation.
	 */
	for (head_idx = t6->output_count; head_idx-- > 0; ) {
		struct t6_head *head = t6_get_head(t6, head_idx);
		struct t6_flip_header fh;
		struct t6_bulk_header bch;
		u32 fb, total;
		u8 *staging;
		int frame;

		if (!head->connected)
			continue;

		/* Black RGB32 frame for raw heads (inherited from upstream). */
		if (head->transport == T6_HEAD_TRANSPORT_RAW) {
			u32 stride = head->width * 4;
			u32 frame_size = stride * head->height;

			staging = head->video_staging;
			total = sizeof(fh) + frame_size;
			if (!staging || total > head->video_staging_size)
				continue;
			fb = head->fb_addr;

			memset(&fh, 0, sizeof(fh));
			fh.command = cpu_to_le32(head->output_idx == 0 ?
						T6_CMD_FLIP_PRIMARY :
						T6_CMD_FLIP_SECONDARY);
			fh.payload_size = cpu_to_le32(frame_size);
			fh.target_format = cpu_to_le32(T6_FMT_RGB32);
			fh.y_pitch = cpu_to_le16(stride);
			fh.y_fb_offset = cpu_to_le32(fb + sizeof(fh));
			fh.source_format = cpu_to_le32(T6_FMT_RGB32);
			fh.flag = T6_FLAG_RESET;
			memset(staging, 0, total);
		} else {
			/*
			 * Only raw heads are primed. The vendor Windows driver
			 * primes nothing (its first bulk transfer is a real
			 * frame), so 0.9.0-r9's NV12 black frames were reverted.
			 */
			continue;
		}
		memcpy(staging, &fh, sizeof(fh));

		for (frame = 0; frame < T6_PRIME_FRAME_COUNT; frame++) {
			int bret;

			memset(&bch, 0, sizeof(bch));
			bch.payload_length = cpu_to_le32(total);
			bch.payload_address = cpu_to_le32(fb);
			bch.packet_length = cpu_to_le32(total);
			bret = t6_bulk_write(t6, &bch, sizeof(bch));
			if (bret)
				break;
			bret = t6_bulk_write(t6, staging, total);
			if (bret)
				break;
		}

		/*
		 * Cache the black frame for keepalive so the HDMI signal
		 * stays alive until the compositor starts rendering.
		 * Without this, the T6 chip blanks the output after ~2s
		 * and the monitor loses signal for 20-30s.
		 */
		if (!t6_last_frame_store(head, staging, total)) {
			head->last_frame_addr = fb;
			/* Use boot keepalive (bypasses DRM state checks) */
			if (t6->wq)
				queue_delayed_work(t6->wq,
					&head->boot_keepalive_work,
					msecs_to_jiffies(
						T6_BOOT_KEEPALIVE_INTERVAL_MS));
		}
	}

	/* DRM mode config */
	ret = drmm_mode_config_init(drm);
	if (ret)
		goto err_buffers;
	drm->mode_config.funcs = &t6_mode_config_funcs;
	drm->mode_config.helper_private = &t6_mode_config_helper_funcs;

	drm->mode_config.min_width = 640;
	drm->mode_config.min_height = 480;
	drm->mode_config.max_width = T6_MAX_WIDTH;
	drm->mode_config.max_height = T6_MAX_HEIGHT;
	drm->mode_config.preferred_depth = 32;

	for (head_idx = 0; head_idx < t6->output_count; head_idx++) {
		struct t6_head *head = t6_get_head(t6, head_idx);

		ret = t6_connector_init(t6, head);
		if (ret)
			goto err_buffers;

		/* Primary plane */
		ret = drm_universal_plane_init(drm, &head->primary_plane,
					       0, &t6_plane_funcs,
					       t6_formats,
					       ARRAY_SIZE(t6_formats),
					       NULL,
					       DRM_PLANE_TYPE_PRIMARY,
					       "t6-plane-%u",
					       head->output_idx);
		if (ret)
			goto err_buffers;
		drm_plane_helper_add(&head->primary_plane,
				     &t6_plane_helper_funcs);

		/* CRTC */
		ret = drm_crtc_init_with_planes(drm, &head->crtc,
						&head->primary_plane,
						NULL, &t6_crtc_funcs,
						"t6-crtc-%u",
						head->output_idx);
		if (ret)
			goto err_buffers;
		drm_crtc_helper_add(&head->crtc, &t6_crtc_helper_funcs);

		/* Set plane's possible_crtcs now that CRTC is initialized */
		head->primary_plane.possible_crtcs = drm_crtc_mask(&head->crtc);

		/* Encoder */
		ret = drm_encoder_init(drm, &head->encoder,
				       &t6_encoder_funcs,
				       DRM_MODE_ENCODER_VIRTUAL,
				       "t6-encoder-%u",
				       head->output_idx);
		if (ret)
			goto err_buffers;
		head->encoder.possible_crtcs = drm_crtc_mask(&head->crtc);

		/* Attach encoder to connector */
		ret = drm_connector_attach_encoder(&head->connector,
						   &head->encoder);
		if (ret)
			goto err_buffers;
	}

	drm_mode_config_reset(drm);

	for (head_idx = 0; head_idx < t6->output_count; head_idx++) {
		struct t6_head *head = t6_get_head(t6, head_idx);

		t6_update_connector_status(head);
	}

	if (t6_experimental_secondary_raw)
		dev_warn(&intf->dev,
			 "experimental_secondary_raw=1 forces logical output 1 to raw XRGB8888 transport instead of the default NV12; uses more USB bandwidth but avoids colour-space conversion\n");
	if (t6_secondary_userspace_jpeg)
		dev_warn(&intf->dev,
			 "secondary_userspace_jpeg=1 forces the deprecated userspace JPEG feeder path for logical output 1\n");
	else if (!t6_experimental_secondary_raw && t6->output_count > 1)
		dev_info(&intf->dev,
			 "Logical output 1 uses native NV12 transport (BGRX->NV12 conversion with zero-damage skip)\n");
	if (t6->manual_only)
		dev_warn(&intf->dev,
			 "manual_only=1 keeps DRM connectors disconnected and blocks automatic scanout; use this for the safest guarded live probes\n");

	for (head_idx = 0; head_idx < t6->output_count; head_idx++) {
		ret = t6_register_jpeg_device(t6_get_head(t6, head_idx));
		if (ret)
			goto err_jpeg;
	}

	/* Register DRM device */
	ret = drm_dev_register(drm, 0);
	if (ret)
		goto err_jpeg;
	ret = devm_device_add_group(&intf->dev, &t6_group);
	if (ret)
		dev_warn(&intf->dev, "Failed to create sysfs group: %d\n", ret);
	t6_audio_init(t6, intf);

	dev_info(&intf->dev, "T6 display ready with %u output%s\n",
		 t6->output_count, t6->output_count == 1 ? "" : "s");
	WRITE_ONCE(t6->probed, true);
	if (t6_any_head_disconnected(t6))
		t6_schedule_reprobe(t6);
	return 0;

err_jpeg:
	t6_unregister_jpeg_devices(t6);
err_buffers:
	/*
	 * Do not clean up DRM objects here. drmm_mode_config_init() registers
	 * drm_mode_config_cleanup(), which destroys every plane, CRTC, encoder
	 * and connector that was initialised when devm releases the device.
	 * The old explicit drm_encoder_cleanup()/drm_crtc_cleanup() calls also
	 * ran on never-initialised (zeroed) objects, e.g. after a chip init
	 * failure, and drm_encoder_cleanup() oopses on those.
	 */
	t6_int_free(t6);
	cancel_delayed_work_sync(&t6->reprobe_work);
	cancel_work_sync(&t6->resend_work);
	t6_cancel_head_activity(t6);
	if (t6->wq) {
		destroy_workqueue(t6->wq);
		t6->wq = NULL;
	}
	t6_free_head_buffers(t6);
err_put:
	t6_int_free(t6);
	if (t6->dmadev)
		put_device(t6->dmadev);
	usb_put_dev(t6->udev);
	return ret;
}

static void t6_usb_disconnect(struct usb_interface *intf)
{
	struct t6_device *t6 = usb_get_intfdata(intf);

	usb_set_intfdata(intf, NULL);
	/* sysfs group removed automatically by devm */

	if (!t6)
		return;

	dev_info(&intf->dev,
		 "T6 disconnecting: bulk_errors=%lld bytes=%lld primary_frames=%lld last_error=%d\n",
		 (long long)atomic64_read(&t6->metrics.bulk_errors),
		 (long long)atomic64_read(&t6->metrics.bulk_bytes),
		 (long long)atomic64_read(&t6->heads[0].metrics.sent_frames),
		 READ_ONCE(t6->io_last_error));

	/*
	 * Disconnect ordering:
	 * 1. Unplug -- waits for in-progress drm_dev_enter, blocks new ones
	 * 2. Cancel async work -- reprobe/hotplug must not run after unplug
	 * 3. Shutdown -- disables outputs (pipe_update guarded by drm_dev_enter)
	 * 4. Tear down userspace-visible state and buffers
	 * No poll_fini needed -- polling is disabled.
	 * The sound card goes first: its work takes io_lock and sends USB.
	 */
	t6_audio_fini(t6);
	drm_dev_unplug(&t6->drm);
	/* Wake any userspace readers blocked on framebuffer export */
	t6_wake_all_export_waiters(t6);
	/* Before reprobe_work: unknown events reschedule it. */
	t6_int_stop(t6);
	cancel_delayed_work_sync(&t6->reprobe_work);
	cancel_work_sync(&t6->resend_work);
	t6_cancel_head_activity(t6);
	if (t6->wq) {
		flush_workqueue(t6->wq);
		destroy_workqueue(t6->wq);
		t6->wq = NULL;
	}
	drm_atomic_helper_shutdown(&t6->drm);
	t6_unregister_jpeg_devices(t6);
	t6_free_head_buffers(t6);
	t6_int_free(t6);

	if (t6->dmadev) {
		put_device(t6->dmadev);
		t6->dmadev = NULL;
	}
	usb_put_dev(t6->udev);
}

/* ------------------------------------------------------------------
 * USB suspend / resume
 * ------------------------------------------------------------------
 */

static int t6_usb_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct t6_device *t6 = usb_get_intfdata(intf);
	int ret;

	if (!t6)
		return 0;

	/*
	 * Turn the CRTCs off through DRM and restore them on resume, like
	 * udl and gm12u320 do. Without this the CRTC stayed "on" across
	 * suspend, atomic_enable never ran again, and the boot keepalive
	 * started in resume kept rewriting a stale frame every 1.5 s.
	 */
	ret = drm_mode_config_helper_suspend(&t6->drm);
	if (ret)
		return ret;

	t6_audio_suspend(t6);
	t6_int_stop(t6);
	cancel_delayed_work_sync(&t6->reprobe_work);
	cancel_work_sync(&t6->resend_work);
	t6_cancel_head_activity(t6);

	return 0;
}

static int t6_usb_resume(struct usb_interface *intf)
{
	struct t6_device *t6 = usb_get_intfdata(intf);
	unsigned int hi;
	int ret;

	if (!t6)
		return 0;

	/* Clear stale error state from before suspend */
	WRITE_ONCE(t6->io_faulted, false);
	t6->ctrl_error_count = 0;
	for (hi = 0; hi < t6->output_count; hi++) {
		struct t6_head *head = t6_get_head(t6, hi);

		WRITE_ONCE(head->io_faulted, false);
		head->last_sent_valid = false;
	}

	/* Re-init chip — T6 loses all state across USB suspend */
	mutex_lock(&t6->io_lock);
	ret = t6_chip_init(t6, false);
	mutex_unlock(&t6->io_lock);
	if (ret < 0) {
		/*
		 * Still hand back the state saved at suspend, then let a USB
		 * reset rebind the driver, which re-runs probe and chip init.
		 */
		dev_err(&intf->dev, "Chip re-init after resume failed: %d; resetting\n",
			ret);
		usb_queue_reset_device(intf);
		drm_mode_config_helper_resume(&t6->drm);
		return 0;
	}
	t6_int_start(t6);
	t6_audio_resume(t6);

	/*
	 * Only heads the compositor never enabled still hold the primed
	 * black frame; keep their HDMI signal alive. CRTCs that were on
	 * are switched back on by drm_mode_config_helper_resume() below,
	 * whose atomic_enable stops this keepalive and whose plane update
	 * sends the real framebuffer.
	 */
	for (hi = 0; hi < t6->output_count; hi++) {
		struct t6_head *head = t6_get_head(t6, hi);

		if (!READ_ONCE(head->connected) || !head->last_jpeg_data)
			continue;
		if (t6->wq)
			queue_delayed_work(t6->wq,
					   &head->boot_keepalive_work,
					   msecs_to_jiffies(
						T6_BOOT_KEEPALIVE_INTERVAL_MS));
	}

	/*
	 * Suspend cancelled reprobe_work and nothing restarted it, so monitor
	 * hotplug detection stopped after the first suspend. Re-check now:
	 * a monitor may also have been swapped while suspended.
	 */
	t6->reprobe_attempt = 0;
	schedule_delayed_work(&t6->reprobe_work,
			      msecs_to_jiffies(t6_reprobe_delays_ms[0]));

	ret = drm_mode_config_helper_resume(&t6->drm);
	if (ret)
		dev_warn(&intf->dev, "Restoring display state failed: %d\n", ret);

	/* Notify compositor to re-evaluate connectors */
	if (!drm_dev_is_unplugged(&t6->drm))
		drm_kms_helper_hotplug_event(&t6->drm);

	return 0;
}

/* ------------------------------------------------------------------
 * USB driver
 * ------------------------------------------------------------------
 */

static const struct usb_device_id t6_ids[] = {
	{ USB_DEVICE(T6_VID, T6_PID) },
	{ }
};
MODULE_DEVICE_TABLE(usb, t6_ids);

static struct usb_driver t6_usb_driver = {
	.name = DRIVER_NAME,
	.probe = t6_usb_probe,
	.disconnect = t6_usb_disconnect,
	.suspend = t6_usb_suspend,
	.resume = t6_usb_resume,
	.reset_resume = t6_usb_resume,
	.id_table = t6_ids,
};
module_usb_driver(t6_usb_driver);

MODULE_AUTHOR("Authentra / Ralph Friedman");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");

MODULE_VERSION("1.1.0-beta2");
