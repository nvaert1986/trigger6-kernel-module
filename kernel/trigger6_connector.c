// SPDX-License-Identifier: GPL-2.0-only
/*
 * trigger6_connector.c -- DRM connector for MCT T6 USB Display
 *
 * EDID is cached at probe time. ALL callbacks serve from memory.
 * ZERO USB calls from any DRM callback -- prevents deadlocks.
 *
 * Copyright (C) 2026 Authentra / Ralph Friedman
 */

#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_edid.h>
#include <drm/drm_modes.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>

#include "trigger6.h"

/*
 * Always return connected. Verified at probe time.
 * No USB calls -- DRM polls this from contexts where USB deadlocks.
 */
static enum drm_connector_status
t6_connector_detect(struct drm_connector *connector, bool force)
{
	struct t6_head *head = t6_head_from_connector(connector);
	bool connected = t6_head_runtime_connected(head);

	/* No get_modes for a disconnected output: clear its audio here. */
	if (!connected && head->output_idx == 0)
		t6_audio_eld(head->t6, connector, false);
	return connected ?
		connector_status_connected : connector_status_disconnected;
}

/*
 * Return a single mode matching the T6 chip's init timing.
 * No USB calls. No EDID parsing. Just a CVT mode.
 */
static enum drm_mode_status
t6_connector_mode_valid_common(struct t6_head *head,
			       const struct drm_display_mode *mode);

/*
 * What the monitor says it can show, from the EDID base block (the only
 * block read from the device): the sizes it lists and its range limits.
 */
struct t6_edid_limits {
	u16 w[16], h[16];
	unsigned int count;
	unsigned int min_vfreq, max_vfreq;	/* Hz, 0 = not given */
	unsigned int max_clock_khz;		/* 0 = not given */
};

static void t6_edid_limits_add(struct t6_edid_limits *lim, int w, int h)
{
	if (w > 0 && h > 0 && lim->count < ARRAY_SIZE(lim->w)) {
		lim->w[lim->count] = w;
		lim->h[lim->count] = h;
		lim->count++;
	}
}

static void t6_parse_edid_limits(const u8 *edid, struct t6_edid_limits *lim)
{
	bool edid_1_3 = edid[0x12] > 1 || edid[0x13] >= 3;
	bool edid_1_4 = edid[0x12] > 1 || edid[0x13] >= 4;
	unsigned int i;

	memset(lim, 0, sizeof(*lim));

	/* Established timings: only the largest sizes matter here. */
	if (edid[0x23] & 0x03 || edid[0x24] & 0xc0)
		t6_edid_limits_add(lim, 800, 600);
	if (edid[0x24] & 0x0e)
		t6_edid_limits_add(lim, 1024, 768);
	if (edid[0x24] & 0x01)
		t6_edid_limits_add(lim, 1280, 1024);

	/* Standard timings */
	for (i = 0; i < 8; i++) {
		u8 b0 = edid[0x26 + 2 * i], b1 = edid[0x27 + 2 * i];
		int w = (b0 + 31) * 8;

		if (b0 <= 1)
			continue;
		switch (b1 >> 6) {
		case 0:
			t6_edid_limits_add(lim, w, edid_1_3 ? w * 10 / 16 : w);
			break;
		case 1:
			t6_edid_limits_add(lim, w, w * 3 / 4);
			break;
		case 2:
			t6_edid_limits_add(lim, w, w * 4 / 5);
			break;
		default:
			t6_edid_limits_add(lim, w, w * 9 / 16);
			break;
		}
	}

	/* Detailed timings and the range limits descriptor */
	for (i = 0; i < 4; i++) {
		const u8 *d = edid + 0x36 + 18 * i;

		if (d[0] || d[1]) {
			t6_edid_limits_add(lim, d[2] | ((d[4] & 0xf0) << 4),
					   d[5] | ((d[7] & 0xf0) << 4));
		} else if (d[3] == 0xfd) {
			lim->min_vfreq = d[5];
			lim->max_vfreq = d[6];
			/* EDID 1.4 rate offsets: bit 1 = max +255, bit 0 = min +255 */
			if (edid_1_4 && (d[4] & 0x02)) {
				lim->max_vfreq += 255;
				if (d[4] & 0x01)
					lim->min_vfreq += 255;
			}
			lim->max_clock_khz = d[9] * 10000;
		}
	}
}

/* True if the monitor lists a mode at least this large and the rates fit. */
static bool t6_edid_limits_allow(const struct t6_edid_limits *lim,
				 const struct drm_display_mode *mode)
{
	unsigned int vrefresh = drm_mode_vrefresh(mode);
	unsigned int i;

	if (!lim->count)
		return true;
	if (lim->min_vfreq && lim->max_vfreq &&
	    (vrefresh + 1 < lim->min_vfreq || vrefresh > lim->max_vfreq + 1))
		return false;
	if (lim->max_clock_khz && (unsigned int)mode->clock > lim->max_clock_khz)
		return false;
	for (i = 0; i < lim->count; i++)
		if (mode->hdisplay <= lim->w[i] && mode->vdisplay <= lim->h[i])
			return true;
	return false;
}

static u32 t6_blob_clock_khz(const u8 *blob)
{
	return blob[0] | (blob[1] << 8) | (blob[2] << 16) | ((u32)blob[3] << 24);
}

/* True if a built-in or device table timing programs exactly @mode. */
static bool t6_static_timing_exists(const struct t6_head *head,
				    const struct drm_display_mode *mode)
{
	unsigned int i;

	for (i = 0; i < t6_builtin_mode_count; i++)
		if (t6_timing_matches_mode(t6_builtin_modes[i].blob, mode))
			return true;
	for (i = 0; head->mode_table && i < head->mode_count; i++)
		if (t6_timing_matches_mode(head->mode_table +
					   i * T6_RES_ENTRY_SIZE, mode))
			return true;
	return false;
}

static int t6_connector_get_modes(struct drm_connector *connector)
{
	struct t6_head *head = t6_head_from_connector(connector);
	struct drm_display_mode *mode;
	const struct drm_edid *drm_edid = NULL;
	struct t6_edid_limits lim = { 0 };
	u8 edid[T6_EDID_MAX_BLOCKS * EDID_LENGTH];
	u8 et[T6_EDID_TIMINGS][T6_RES_ENTRY_SIZE];
	unsigned int et_count, j;
	const u8 *pref_timing = NULL;
	bool et_pref;
	int edid_len;
	int pref_w, pref_h;
	bool filter;
	int count = 0;
	int i;

	if (!t6_head_runtime_connected(head)) {
		if (head->output_idx == 0)
			t6_audio_eld(head->t6, connector, false);
		return 0;
	}

	/* Snapshot: the reprobe worker may be refreshing edid_data. */
	spin_lock(&head->edid_lock);
	edid_len = head->edid_len;
	if (edid_len >= EDID_LENGTH)
		memcpy(edid, head->edid_data, edid_len);
	et_count = head->edid_timing_count;
	et_pref = head->edid_timing_pref;
	memcpy(et, head->edid_timings, et_count * T6_RES_ENTRY_SIZE);
	spin_unlock(&head->edid_lock);

	if (edid_len >= EDID_LENGTH) {
		/*
		 * The base block and the extension blocks that were read; its
		 * extension count matches them (t6_read_edid_ext). With the
		 * CEA-861 block DRM knows HDMI vs DVI and the audio formats.
		 */
		drm_edid = drm_edid_alloc(edid, edid_len);
		if (drm_edid && !drm_edid_valid(drm_edid)) {
			drm_edid_free(drm_edid);
			drm_edid = NULL;
		}
	}
	drm_edid_connector_update(connector, drm_edid);
	/* HDMI audio is on output 0; DRM built the ELD from the EDID above. */
	if (head->output_idx == 0)
		t6_audio_eld(head->t6, connector, !!drm_edid);
	if (drm_edid) {
		/* Extract physical size for HiDPI scaling (EDID bytes 0x15-0x16 = cm) */
		if (edid[0x15] && edid[0x16]) {
			connector->display_info.width_mm = edid[0x15] * 10;
			connector->display_info.height_mm = edid[0x16] * 10;
		}
	}

	/*
	 * Offer the built-in modes first, then every other mode in this
	 * output's own device table (request 0x89). Each DRM mode carries
	 * exactly the timing its blob programs, and every offered mode can
	 * be programmed: atomic_enable looks it up the same way.
	 *
	 * With an EDID, skip modes larger than any mode the monitor lists or
	 * outside its refresh and pixel clock limits (a 1080p monitor was
	 * offered 1920x1200 and 1600x1200). Smaller modes stay: monitors
	 * scale them (1680x1050 worked on a 1080p monitor that does not list
	 * it). If that leaves nothing, offer the unfiltered list.
	 *
	 * Last come the monitor's own detailed timings, built as the vendor
	 * driver builds its custom modes (t6_build_timing), unless a built-in
	 * or table mode has exactly the same timing. The monitor's first
	 * detailed timing is the preferred mode when the adapter can show it.
	 */
	if (drm_edid)
		t6_parse_edid_limits(edid, &lim);
	filter = lim.count > 0;
	if (drm_edid && et_pref &&
	    t6_blob_clock_khz(et[0]) <= t6_head_max_clock(head))
		pref_timing = et[0];
	pref_w = T6_SCANOUT_WIDTH;
	pref_h = T6_SCANOUT_HEIGHT;
	if (drm_edid) {
		/* First detailed timing = the monitor's preferred mode */
		int w = edid[0x38] | ((edid[0x3A] & 0xF0) << 4);
		int h = edid[0x3B] | ((edid[0x3D] & 0xF0) << 4);

		if (w && h && t6_head_mode_blob(head, w, h, 60)) {
			pref_w = w;
			pref_h = h;
		}
	}

again:
	for (i = 0; i < t6_builtin_mode_count + (int)head->mode_count; i++) {
		const u8 *blob;

		if (i < t6_builtin_mode_count) {
			blob = t6_builtin_modes[i].blob;
		} else {
			blob = head->mode_table +
			       (i - t6_builtin_mode_count) * T6_RES_ENTRY_SIZE;
			/* Covered by a built-in blob: already offered */
			if (t6_find_mode_blob(blob[8] | (blob[9] << 8),
					      blob[16] | (blob[17] << 8),
					      blob[4] | (blob[5] << 8)))
				continue;
		}

		mode = drm_mode_create(connector->dev);
		if (!mode)
			continue;
		t6_blob_to_mode(blob, mode);
		if (t6_connector_mode_valid_common(head, mode) != MODE_OK ||
		    (filter && !t6_edid_limits_allow(&lim, mode))) {
			drm_mode_destroy(connector->dev, mode);
			continue;
		}
		mode->type |= DRM_MODE_TYPE_DRIVER;
		if (pref_timing ? t6_timing_matches_mode(pref_timing, mode) :
		    (mode->hdisplay == pref_w && mode->vdisplay == pref_h &&
		     drm_mode_vrefresh(mode) == 60))
			mode->type |= DRM_MODE_TYPE_PREFERRED;
		drm_mode_probed_add(connector, mode);
		count++;
	}

	for (j = 0; drm_edid && j < et_count; j++) {
		if (t6_blob_clock_khz(et[j]) > t6_head_max_clock(head))
			continue;
		mode = drm_mode_create(connector->dev);
		if (!mode)
			continue;
		t6_blob_to_mode(et[j], mode);
		if (t6_static_timing_exists(head, mode) ||
		    t6_connector_mode_valid_common(head, mode) != MODE_OK ||
		    (filter && !t6_edid_limits_allow(&lim, mode))) {
			drm_mode_destroy(connector->dev, mode);
			continue;
		}
		mode->type |= DRM_MODE_TYPE_DRIVER;
		if (et[j] == pref_timing)
			mode->type |= DRM_MODE_TYPE_PREFERRED;
		drm_mode_probed_add(connector, mode);
		count++;
	}
	if (!count && filter) {
		filter = false;
		goto again;
	}

	drm_edid_free(drm_edid);
	return count;
}

/*
 * Accept only modes that atomic_enable can program on this output: a
 * built-in timing blob, an entry in the output's device mode table or a
 * timing built from the monitor's EDID (t6_head_find_timing). Reject
 * anything else (including user-added modes) to prevent sending timing
 * data the T6 chip can't handle.
 */
static enum drm_mode_status
t6_connector_mode_valid_common(struct t6_head *head,
			       const struct drm_display_mode *mode)
{
	u8 timing[T6_RES_ENTRY_SIZE];

	/* The adapter is specified for at most 60 Hz; its table also lists 75 Hz. */
	if (mode->hdisplay > T6_MAX_WIDTH || mode->vdisplay > T6_MAX_HEIGHT ||
	    drm_mode_vrefresh(mode) > 60)
		return MODE_BAD;
	/* USB 2: no mode wider than 2048 pixels, as the vendor driver */
	if (head->t6->usb2 && mode->hdisplay > T6_HS_MAX_WIDTH)
		return MODE_BAD_HVALUE;

	if (!t6_head_find_timing(head, mode, timing))
		return MODE_NOMODE;

	return MODE_OK;
}

static enum drm_mode_status
t6_connector_mode_valid_mutable(struct drm_connector *connector,
				struct drm_display_mode *mode)
{
	return t6_connector_mode_valid_common(t6_head_from_connector(connector),
					      mode);
}

static enum drm_mode_status
t6_connector_mode_valid_const(struct drm_connector *connector,
			      const struct drm_display_mode *mode)
{
	return t6_connector_mode_valid_common(t6_head_from_connector(connector),
					      mode);
}

/*
 * Kernel API compatibility: the mode_valid callback signature changed from
 * mutable to const struct drm_display_mode * between kernel versions.
 * Use __builtin_types_compatible_p to select the matching variant at compile
 * time so the driver builds cleanly on both old and new kernels.
 */
#define T6_CONNECTOR_MODE_VALID \
	__builtin_choose_expr(\
		__builtin_types_compatible_p(\
			typeof(((struct drm_connector_helper_funcs *)0)->mode_valid), \
			enum drm_mode_status (*)(struct drm_connector *, \
						 const struct drm_display_mode *)), \
		t6_connector_mode_valid_const, \
		t6_connector_mode_valid_mutable)

static const struct drm_connector_helper_funcs t6_conn_helper = {
	.get_modes = t6_connector_get_modes,
	.mode_valid = T6_CONNECTOR_MODE_VALID,
};

static const struct drm_connector_funcs t6_conn_funcs = {
	.detect = t6_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

int t6_connector_init(struct t6_device *t6, struct t6_head *head)
{
	int ret;

	ret = drm_connector_init(&t6->drm, &head->connector, &t6_conn_funcs,
				 DRM_MODE_CONNECTOR_HDMIA);
	if (ret)
		return ret;

	drm_connector_helper_add(&head->connector, &t6_conn_helper);

	/* Standard HDMI subpixel layout for text rendering optimization */
	head->connector.display_info.subpixel_order = SubPixelHorizontalRGB;

	/*
	 * NO polling. Polling caused system freezes -- drm_kms_helper_poll
	 * workqueue takes mode_config.mutex which deadlocks during probe.
	 * Modes are force-populated in probe via t6_force_modes() instead.
	 */
	head->connector.polled = 0;
	head->connector.interlace_allowed = 0;
	head->connector.doublescan_allowed = 0;

	return 0;
}
