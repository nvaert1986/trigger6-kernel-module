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

	return t6_head_runtime_connected(head) ?
		connector_status_connected : connector_status_disconnected;
}

/*
 * Return a single mode matching the T6 chip's init timing.
 * No USB calls. No EDID parsing. Just a CVT mode.
 */
static enum drm_mode_status
t6_connector_mode_valid_common(const struct t6_head *head,
			       const struct drm_display_mode *mode);

static int t6_connector_get_modes(struct drm_connector *connector)
{
	struct t6_head *head = t6_head_from_connector(connector);
	struct drm_display_mode *mode;
	const struct drm_edid *drm_edid = NULL;
	u8 edid[EDID_LENGTH];
	int edid_len;
	int pref_w, pref_h;
	int count = 0;
	int i;

	if (!t6_head_runtime_connected(head))
		return 0;

	/* Snapshot: the reprobe worker may be refreshing edid_data. */
	spin_lock(&head->edid_lock);
	edid_len = head->edid_len;
	if (edid_len >= EDID_LENGTH)
		memcpy(edid, head->edid_data, EDID_LENGTH);
	spin_unlock(&head->edid_lock);

	if (edid_len >= EDID_LENGTH) {
		/*
		 * Only the base block is read from the device. Clear its
		 * extension count (and fix the checksum) so neither DRM nor
		 * userspace expects blocks that were never read: with one
		 * extension DRM saw an all-zero block, with more it read past
		 * the end of edid_data.
		 */
		edid[0x7f] += edid[0x7e];
		edid[0x7e] = 0;
		drm_edid = drm_edid_alloc(edid, EDID_LENGTH);
		if (drm_edid && !drm_edid_valid(drm_edid)) {
			drm_edid_free(drm_edid);
			drm_edid = NULL;
		}
	}
	drm_edid_connector_update(connector, drm_edid);
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
	 */
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
		if (t6_connector_mode_valid_common(head, mode) != MODE_OK) {
			drm_mode_destroy(connector->dev, mode);
			continue;
		}
		mode->type |= DRM_MODE_TYPE_DRIVER;
		if (mode->hdisplay == pref_w && mode->vdisplay == pref_h &&
		    drm_mode_vrefresh(mode) == 60)
			mode->type |= DRM_MODE_TYPE_PREFERRED;
		drm_mode_probed_add(connector, mode);
		count++;
	}

	drm_edid_free(drm_edid);
	return count;
}

/*
 * Accept only modes that atomic_enable can program on this output: a
 * built-in timing blob or an entry in the output's device mode table.
 * Reject anything else (including user-added modes) to prevent sending
 * timing data the T6 chip can't handle.
 */
static enum drm_mode_status
t6_connector_mode_valid_common(const struct t6_head *head,
			       const struct drm_display_mode *mode)
{
	/* The adapter is specified for at most 60 Hz; its table also lists 75 Hz. */
	if (mode->hdisplay > T6_MAX_WIDTH || mode->vdisplay > T6_MAX_HEIGHT ||
	    drm_mode_vrefresh(mode) > 60)
		return MODE_BAD;

	if (!t6_head_mode_blob(head, mode->hdisplay, mode->vdisplay,
			       drm_mode_vrefresh(mode)))
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
