/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * trigger6.h -- MCT Trigger 6 USB Display DRM Driver
 *
 * Shared state and inline helpers for the T6 two-head USB display adapter.
 * Head 0 uses raw XRGB8888 transport with row-level damage tracking and
 * multi-write partial updates.  Head 1 defaults to NV12 transport with
 * zero-damage skip, but can be switched to in-kernel JPEG, userspace JPEG,
 * or experimental raw via module parameters.  Adaptive pacing gates each
 * transport independently.
 *
 * Based on udl (USB DisplayLink) architecture for proven stability.
 * Protocol from mcttrigger/triggerdm + cyrozap reverse engineering.
 *
 * Copyright (C) 2026 Authentra / Ralph Friedman
 */

#ifndef _TRIGGER6_H_
#define _TRIGGER6_H_

#include <linux/usb.h>
#include <linux/mutex.h>
#include <linux/miscdevice.h>
#include <linux/workqueue.h>
#include <linux/timer.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <drm/drm_connector.h>
#include <drm/drm_device.h>
#include <drm/drm_crtc.h>
#include <drm/drm_plane.h>
#include <drm/drm_encoder.h>

#define T6_JPEG_QUALITY_DEFAULT	40
#define T6_MAX_WIDTH		1920
#define T6_MAX_HEIGHT		1200
#define T6_SCANOUT_WIDTH	1920
#define T6_SCANOUT_HEIGHT	1080
#define T6_SCANOUT_REFRESH_HZ	60

/* USB IDs */
#define T6_VID			0x0711
#define T6_PID			0x5601

/* USB endpoints */
#define T6_EP_BULK_OUT		0x02
#define T6_EP_INT_IN		0x83	/* 64-byte device event packets */

/*
 * Interrupt event packet (64 bytes), decoded as the vendor driver's read
 * handler does (T6InDisp.dll 0x1800f7d70): events without bit 0x04 in byte 0
 * are dropped, byte 19 holds flags, a flip-done event carries its u32 fence
 * id at byte 12.
 */
#define T6_INT_PACKET_SIZE	64
#define T6_INT_EVT_VALID	0x04	/* byte 0 */
#define T6_INT_FENCE_OFFSET	12
#define T6_INT_FLAGS_OFFSET	19
#define T6_INT_FLAG_STATUS_OUT0	0x01	/* monitor connection changed */
#define T6_INT_FLAG_STATUS_OUT1	0x02
#define T6_INT_FLAG_FLIP_DONE	0x04
#define T6_INT_FLAG_WAKE	0x10	/* vendor driver wakes every output */

/* USB control-message timeout (ms) — kept short to speed probe */
#define T6_USB_CTRL_TIMEOUT_MS	1000

/* Consecutive control-message failures before scheduling USB reset */
#define T6_CTRL_ERROR_THRESHOLD	5

/* Vendor requests */
#define T6_REQ_MONITOR_CTRL	0x03	/* wValue=output, wIndex=1 on / 0 off */
#define T6_REQ_SET_RESOLUTION	0x12	/* wValue=output, 32-byte timing blob */
#define T6_REQ_SET_COLOR	0x23
/*
 * 0x24 is HDMI audio start/stop, not display timing: payload u32 on/off,
 * u32 0, u32 buffer bytes (0x2580), u16 0, u16 channels (2). The vendor
 * driver sends "off" before the first frame and "on" only with audio data.
 */
#define T6_REQ_AUDIO_CTRL	0x24
#define T6_REQ_SET_READY	0x31	/* sent once, all zero, during init */
#define T6_REQ_GET_EDID		0x80
#define T6_REQ_GET_STATUS	0x87
#define T6_REQ_GET_RAM		0x88
#define T6_REQ_GET_RES_TABLE	0x89	/* wValue=output, wIndex=byte offset */

/* Device mode table: 32-byte timing entries, read in 512-byte chunks */
#define T6_RES_ENTRY_SIZE	32
#define T6_RES_CHUNK_SIZE	512
#define T6_RES_MAX_ENTRIES	64	/* the tested table has 30 */

/*
 * 0x1C/0x1D: gamma LUT. 0x1C wValue=(output << 8) | op: op 0 at init,
 * op 2 commits after three 256-byte 0x1D writes (wValue=output << 8,
 * wIndex=0x100/0x200/0x400 per channel).
 */
#define T6_REQ_LUT_CTRL		0x1C
#define T6_REQ_LUT_WRITE	0x1D
#define T6_LUT_OP_RESET		0x00
#define T6_LUT_OP_COMMIT	0x02

/* Video commands */
#define T6_CMD_FLIP_PRIMARY	3
#define T6_CMD_FLIP_SECONDARY	4

#define T6_OUTPUT_COUNT		2

/* Secondary transport format IDs (JPEG and NV12) */
#define T6_FMT_NV12		6
#define T6_FMT_JPEG		13
#define T6_JPEG_PADDING_SIZE	1024
#define T6_JPEG_FB_SLOT_COUNT	3

/* Pixel format */
#define T6_FMT_RGB32		8   /* BGRX 32-bit */

/* JPEG reset flag */
#define T6_FLAG_RESET		0x80

/* Damage tracking for raw heads */
#define T6_MAX_DIRTY_RANGES	32

struct t6_dirty_range {
	u32 y_start;	/* first dirty row */
	u32 y_count;	/* number of dirty rows */
};

struct t6_frame_damage {
	struct t6_dirty_range ranges[T6_MAX_DIRTY_RANGES];
	int num_ranges;
	bool full_frame;
};

/* USB bulk transfer tuning */
#define T6_USB_XFER_CHUNK_SIZE	0xffc00
#define T6_USB_XFER_TIMEOUT_MS	5000

/* Keepalive: re-send last frame to prevent blanking (module param keepalive_ms) */
#define T6_KEEPALIVE_INTERVAL_MS	2000
#define T6_BOOT_KEEPALIVE_INTERVAL_MS	1500

/* Auto-recovery: clear io_faulted and reprobe after this delay */
#define T6_FAULT_RECOVERY_DELAY_MS	60000

/* Probe window: retry undetected heads before DRM registration */
#define T6_INITIAL_PROBE_WINDOW_MS	500
#define T6_INITIAL_PROBE_RETRY_MS	50

/* HDMI signal priming: black frames to stabilize output after init */
#define T6_PRIME_FRAME_COUNT		8
#define T6_FRAME_MIN_INTERVAL_MS_DEFAULT	0	/* raw USB submission pacing */
#define T6_SECONDARY_FRAME_MIN_INTERVAL_MS_DEFAULT	250	/* adaptive pacing cap for JPEG/NV12 secondary heads */

/*
 * Bulk command header (32 bytes).
 */
struct t6_bulk_header {
	__le32 signature;
	__le32 payload_length;
	__le32 payload_address;
	__le32 packet_length;
	__le32 bytes_written;
	__le32 output_index;
	__le32 reserved[2];
} __packed;

/*
 * Video flip header (48 bytes).
 */
struct t6_flip_header {
	__le32 command;
	__le32 payload_size;
	__le32 fence_id;
	__le32 target_format;
	__le16 y_pitch;
	__le16 uv_pitch;
	__le32 y_fb_offset;
	__le32 u_offset;
	__le32 v_offset;
	__le32 source_format;
	u8 padding[11];
	u8 flag;
} __packed;

enum t6_head_transport {
	T6_HEAD_TRANSPORT_RAW = 0,
	T6_HEAD_TRANSPORT_JPEG_CMD = 1,
	T6_HEAD_TRANSPORT_USER_JPEG = 2,
	T6_HEAD_TRANSPORT_NV12 = 3,
};

struct t6_head_metrics {
	atomic64_t queue_attempts;
	atomic64_t queued_frames;
	atomic64_t coalesced_overwrites;
	atomic64_t busy_drops;
	atomic64_t skipped_frames;
	atomic64_t sent_frames;
	atomic64_t sent_bytes;
	atomic64_t send_errors;
	atomic64_t keepalive_sent;
	atomic64_t keepalive_errors;
	atomic64_t flips_done;		/* flip-done events from EP 0x83 */
	atomic64_t transport_faults;
	atomic64_t pacing_defers;
	atomic64_t stage_samples;
	atomic64_t stage_ns_total;
	atomic64_t queue_wait_samples;
	atomic64_t queue_wait_ns_total;
	atomic64_t encode_samples;
	atomic64_t encode_ns_total;
	atomic64_t usb_samples;
	atomic64_t usb_ns_total;
	atomic64_t send_samples;
	atomic64_t send_ns_total;
	u64 last_stage_ns;
	u64 max_stage_ns;
	u64 last_queue_wait_ns;
	u64 max_queue_wait_ns;
	u64 last_encode_ns;
	u64 max_encode_ns;
	u64 last_usb_ns;
	u64 max_usb_ns;
	u64 last_send_ns;
	u64 max_send_ns;
	size_t last_payload_bytes;
	int last_error;
	char last_error_source[24];
};

struct t6_device_metrics {
	atomic64_t bulk_calls;
	atomic64_t bulk_chunks;
	atomic64_t bulk_bytes;
	atomic64_t bulk_errors;
	atomic64_t bulk_short_writes;
	atomic64_t tx_work_runs;
	atomic64_t tx_work_empty;
	atomic64_t tx_adapter_defers;
	u64 last_bulk_ns;
	u64 max_bulk_ns;
	size_t last_bulk_bytes;
};

struct t6_head {
	struct t6_device *t6;
	u8 output_idx;
	bool connected;
	bool io_faulted;
	/* Set by atomic_enable/disable under io_lock; gates every frame send. */
	bool crtc_active;
	unsigned long fault_jiffies;
	u8 status;
	u32 fb_addr;
	u32 cmd_addr;
	u32 fb_slots[T6_JPEG_FB_SLOT_COUNT];
	u8 fb_slot_count;
	u8 fb_slot_index;
	u32 cmd_base_addr;
	u32 cmd_limit_addr;
	u32 cmd_cursor_addr;
	int width;
	int height;
	int refresh;
	char monitor_name[16];
	/* This output's own mode table (request 0x89), entries as read */
	u8 *mode_table;
	unsigned int mode_count;
	u32 last_done_fence;	/* from flip-done events */
	u32 frame_seq;
	u8 edid_data[256];
	int edid_len;
	spinlock_t edid_lock;	/* edid_data/edid_len vs. get_modes */
	enum t6_head_transport transport;

	struct drm_crtc crtc;
	struct drm_plane primary_plane;
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct work_struct tx_work;
	struct delayed_work tx_defer_work;
	u8 *tx_front;
	u8 *tx_back;
	size_t tx_buf_size;
	size_t tx_len;
	u32 tx_stride;
	bool tx_pending;
	bool keepalive_due;
	u64 tx_queued_ns;
	unsigned long tx_last_jiffies;
	struct miscdevice jpeg_miscdev;
	char jpeg_dev_name[48];
	bool jpeg_misc_registered;
	struct t6_head_metrics metrics;

	/* Pre-allocated staging buffer for raw frame sends (primary head) */
	u8 *video_staging;
	size_t video_staging_size;

	/* Damage tracking: last-sent frame for row-level diffing (raw and NV12 heads) */
	u8 *last_sent_frame;
	bool last_sent_valid;

	/* Adaptive JPEG quality: 0 = use global param, else per-head */
	unsigned int jpeg_adaptive_quality;

	/* Pre-allocated staging buffer for JPEG and NV12 frame sends */
	u8 *jpeg_staging;
	size_t jpeg_staging_size;

	/* Framebuffer export for hybrid JPEG path (compositor → userspace encode) */
	u8 *fb_export_buf;	/* protected by tx_lock */
	wait_queue_head_t fb_export_wq;
	atomic_t fb_export_seq;
	size_t fb_export_len;
	u32 fb_export_width, fb_export_height;
	atomic_t bridge_open;
	unsigned long bridge_heartbeat;
	bool bridge_waiting;	/* helper is blocked in read(), waiting for a frame */

	/* Keepalive for monitor stability */
	struct timer_list keepalive_timer;
	struct delayed_work boot_keepalive_work;
	u8 *last_jpeg_data;
	u32 last_frame_addr;	/* VRAM address the cached frame was written to */
	size_t last_jpeg_len;
	size_t last_jpeg_cap;
	int io_last_error;
};

/*
 * Per-device state.
 */
struct t6_device {
	struct drm_device drm;
	struct usb_device *udev;
	struct usb_interface *intf;
	struct device *dmadev;

	/* Shared chip info */
	u32 ram_mb;
	struct t6_head heads[T6_OUTPUT_COUNT];

	/* Interrupt IN endpoint (0x83): flip-done and other device events */
	struct urb *int_urb;
	u8 *int_buf;
	dma_addr_t int_dma;
	unsigned int int_pipe;
	int int_interval;
	bool int_running;
	unsigned int int_consecutive_errors;
	bool probed;			/* probe finished: reprobe may run */
	atomic64_t int_events;
	atomic64_t int_hotplug;
	atomic64_t int_unknown;
	atomic64_t int_errors;

	/* Heads programmed by the last successful chip init (io_lock). */
	u8 init_mask;

	/* Serialize frame staging and USB submission across both heads. */
	struct delayed_work reprobe_work;
	struct mutex tx_lock;
	struct mutex io_lock;
	struct mutex bulk_lock;

	/* Dedicated unbound workqueue for USB I/O — avoids hogging system_wq */
	struct workqueue_struct *wq;
	struct t6_device_metrics metrics;
	u8 tx_next_head;
	unsigned int reprobe_attempt;
	bool manual_only;
	bool io_faulted;
	int io_last_error;
	u32 ctrl_error_count;

	/* Pre-allocated USB bulk transfer chunk buffer */
	u8 *bulk_chunk;
	u8 *bulk_chunk2;
	struct urb *bulk_urb[2];
};

static inline struct t6_device *to_t6(struct drm_device *drm)
{
	return container_of(drm, struct t6_device, drm);
}

static inline struct t6_head *t6_get_head(struct t6_device *t6,
					       unsigned int idx)
{
	return &t6->heads[idx];
}

static inline struct t6_head *t6_head_from_crtc(struct drm_crtc *crtc)
{
	return container_of(crtc, struct t6_head, crtc);
}

static inline struct t6_head *t6_head_from_plane(struct drm_plane *plane)
{
	return container_of(plane, struct t6_head, primary_plane);
}

static inline struct t6_head *t6_head_from_connector(struct drm_connector *connector)
{
	return container_of(connector, struct t6_head, connector);
}

/*
 * Returns true if the head has a working transport path to the display.
 * All four transports (raw XRGB, in-kernel JPEG, userspace JPEG, and
 * NV12) support DRM scanout.
 */
static inline bool t6_head_scanout_supported(const struct t6_head *head)
{
	return head->transport == T6_HEAD_TRANSPORT_RAW ||
	       head->transport == T6_HEAD_TRANSPORT_JPEG_CMD ||
	       head->transport == T6_HEAD_TRANSPORT_USER_JPEG ||
	       head->transport == T6_HEAD_TRANSPORT_NV12;
}

static inline bool t6_head_drm_scanout_enabled(const struct t6_head *head)
{
	return t6_head_scanout_supported(head) && !head->t6->manual_only;
}

static inline bool t6_head_runtime_connected(const struct t6_head *head)
{
	return READ_ONCE(head->connected) &&
	       !READ_ONCE(head->io_faulted) &&
	       t6_head_drm_scanout_enabled(head) &&
	       !READ_ONCE(head->t6->io_faulted);
}

/* Built-in display modes */
struct t6_builtin_mode {
	u16 width;
	u16 height;
	u16 refresh;
	const u8 *blob;
};

extern const struct t6_builtin_mode t6_builtin_modes[];
extern const int t6_builtin_mode_count;
const u8 *t6_find_mode_blob(int width, int height, int refresh);
const u8 *t6_head_mode_blob(const struct t6_head *head, int width, int height,
			    int refresh);
void t6_blob_to_mode(const u8 *blob, struct drm_display_mode *mode);

/* connector */
int t6_connector_init(struct t6_device *t6, struct t6_head *head);

#endif
