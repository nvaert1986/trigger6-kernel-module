// SPDX-License-Identifier: GPL-2.0-only
/*
 * trigger6_audio.c -- HDMI audio playback for MCT T6 USB displays
 *
 * Ported from mct-linux (kernel/mct_t6.c, GPL-2.0-only), where this protocol
 * was verified by ear on a 0711:5600 adapter (docs/reference/protocol-t6.md,
 * "Audio"). Cross-checked against the vendor's Windows bus driver t6sta.sys:
 * it uses the same requests with the same lengths (0x23 and 0xa3: 40 bytes,
 * 0x24, 0xa1, 0xa2 and 0xa4: 16 bytes, 0xa5: n * 32 bytes), and its audio
 * off request in the vendor USB capture has the same 0x24 layout.
 *
 * The adapter has no USB audio interface. Playback goes over the same bulk
 * pipe as video: a 32-byte session header with signature 3 in its own
 * transfer, then the PCM (48 kHz, 16-bit little-endian stereo). The engine is
 * switched on and off with request 0x24. Every known driver paces by the
 * host clock: a 9600-byte prefill, then 10 ms packets, here about
 * audio_lead_ms ahead of playback. Unlike mct-linux, the lead is prefilled
 * with silence and the application's audio is taken at the rate it plays:
 * taking the whole lead from the ALSA buffer at once left PipeWire, which
 * keeps about 100 ms queued, a few ms of margin, and it ran into an underrun
 * every 0.4 s (aplay, with a fuller buffer, did not). The adapter plays audio only while the
 * output is on with a mode set, and cleanly only with at least 112 px of
 * horizontal blanking, where HDMI carries the audio packets.
 */

#include <linux/hrtimer.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <sound/control.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/tlv.h>

#include <drm/drm_drv.h>

#include "trigger6.h"

#define T6_REQ_AUD_SET		0x24	/* out: set engine state, 16 bytes */
#define T6_REQ_AUD_NODE_CAPS	0xa2	/* in: node type and range, 16 bytes */
#define T6_REQ_AUD_NODE_VALUE	0xa3	/* in: node values, 40 bytes */
#define T6_REQ_AUD_STATE	0xa4	/* in: engine state, 16 bytes */
#define T6_REQ_AUD_FORMATS	0xa5	/* in: format list, 32 bytes each */
#define T6_SIG_AUDIO		3	/* bulk session signature */
#define T6_AUD_RATE		48000
#define T6_AUD_BPS		(T6_AUD_RATE * 4)	/* 16-bit stereo */
#define T6_AUD_PKT		(T6_AUD_BPS / 100)	/* 10 ms, as MCT and Windows */
#define T6_AUD_PKT_F		(T6_AUD_PKT / 4)
#define T6_AUD_RETURN_SIZE	9600	/* "interrupt interval" in 0x24 */
#define T6_AUD_TICK_MS		5
#define T6_AUD_MIN_HBLANK	112

static bool t6_audio_enable = true;
module_param_named(audio, t6_audio_enable, bool, 0444);
MODULE_PARM_DESC(audio, "Offer HDMI audio as an ALSA sound card when the adapter supports it (default: true)");

static unsigned int t6_audio_lead_ms = 90;
module_param_named(audio_lead_ms, t6_audio_lead_ms, uint, 0644);
MODULE_PARM_DESC(audio_lead_ms, "How far ahead of playback HDMI audio is sent, in ms (20-500, default: 90)");

struct t6_audio {
	struct t6_device *t6;
	struct snd_card *card;
	struct snd_pcm_substream *sub;	/* while running, under lock */
	spinlock_t lock;		/* sub, running, restart, positions */
	bool running;			/* ALSA started the stream */
	bool restart;			/* started again, or engine lost */
	/* frames, under lock (the pointer callback reads them) */
	unsigned int pos;		/* in the ALSA buffer, sent next */
	u64 data;			/* sent from ALSA since prepare */
	u64 base;			/* data when the timing started */
	u64 ptr;			/* last position reported */
	u64 notified;			/* position at the last period_elapsed */
	ktime_t start;			/* timing start: device plays frame 0 */
	u64 dev;			/* sent to the device since start */
	u64 silence;			/* of which silence */
	unsigned int lead;		/* audio_lead_ms in frames, at start */
	struct hrtimer timer;		/* paces the work while running */
	struct work_struct work;	/* sends packets, engine on/off */
	/* work only, under io_lock */
	bool engine;			/* playback engine enabled on the device */
	u16 format;			/* format index the device reports */
	u32 cyclic;			/* the device's buffer size */
	u8 *pkt;			/* one packet of PCM */
	u64 packets, fills, prefills, starts;	/* statistics */
	u32 warned_hb;			/* blanking last warned about */
	/* volume node 0, under io_lock */
	bool has_vol;
	bool unmuted;			/* the switch: off sends the minimum */
	int vol_min, vol_max, vol[2];
};

/*
 * Enable or disable the playback engine: activity, format index, the
 * "interrupt interval" (9600 bytes) and the cyclic buffer size. For
 * playback the bulk stream drives the engine and the activity byte is
 * ignored, but the vendor drivers still clear it when they stop.
 * io_lock held.
 */
static int t6_audio_engine(struct t6_audio *aud, bool on)
{
	u8 b[16] = { 0 };
	int ret;

	b[0] = on;
	put_unaligned_le16(aud->format, b + 2);
	put_unaligned_le32(T6_AUD_RETURN_SIZE, b + 8);
	put_unaligned_le32(aud->cyclic, b + 12);
	ret = t6_ctrl_out(aud->t6, T6_REQ_AUD_SET, 0, 0, b, sizeof(b));
	return ret < 0 ? ret : 0;
}

/*
 * One 10 ms packet: the session header (signature 3, payload and packet
 * length = packet size) and the PCM, each its own transfer. The first n
 * frames come from the ALSA buffer at pos, the rest is silence (all of it
 * for n = 0). gap: the silence fills an underrun, so the stream plays that
 * much later. io_lock held, so packets go between video frames.
 */
static int t6_audio_packet(struct t6_audio *aud, struct snd_pcm_runtime *rt,
			   unsigned int n, bool gap)
{
	unsigned int size = rt->buffer_size, pos = aud->pos;
	unsigned int first = min(n, size - pos);
	struct t6_bulk_header hdr = { 0 };
	unsigned long flags;
	int ret;

	memset(aud->pkt, 0, T6_AUD_PKT);
	if (n) {
		memcpy(aud->pkt, rt->dma_area + frames_to_bytes(rt, pos),
		       frames_to_bytes(rt, first));
		memcpy(aud->pkt + frames_to_bytes(rt, first), rt->dma_area,
		       frames_to_bytes(rt, n - first));
	}
	hdr.signature = cpu_to_le32(T6_SIG_AUDIO);
	hdr.payload_length = cpu_to_le32(T6_AUD_PKT);
	hdr.packet_length = cpu_to_le32(T6_AUD_PKT);
	ret = t6_bulk_write(aud->t6, &hdr, sizeof(hdr));
	if (!ret)
		ret = t6_bulk_write(aud->t6, aud->pkt, T6_AUD_PKT);
	if (ret)
		return ret;

	spin_lock_irqsave(&aud->lock, flags);
	aud->pos = (pos + n) % size;
	aud->data += n;
	aud->dev += T6_AUD_PKT_F;
	if (gap)
		aud->silence += T6_AUD_PKT_F - n;
	spin_unlock_irqrestore(&aud->lock, flags);
	if (n)
		aud->packets++;
	else if (gap)
		aud->fills++;
	else
		aud->prefills++;
	return 0;
}

/*
 * The ALSA position (frames since prepare): the application's audio the
 * device plays now (the lead of silence plays first), so it moves smoothly
 * with the clock; never past what was sent, never backwards. The lead is
 * reported as delay. lock held.
 */
static u64 t6_audio_hwptr(struct t6_audio *aud)
{
	s64 played = div_s64(ktime_us_delta(ktime_get(), aud->start) * T6_AUD_RATE,
			     USEC_PER_SEC);
	s64 ptr = (s64)aud->base + played - (s64)aud->silence;

	ptr = clamp_t(s64, ptr, aud->ptr, aud->data);
	aud->ptr = max_t(s64, ptr, aud->ptr);
	return aud->ptr;
}

/* Say once per mode why nothing or noise may be heard. io_lock held. */
static void t6_audio_check_mode(struct t6_audio *aud)
{
	struct t6_head *head = t6_get_head(aud->t6, 0);
	u32 hb = 0;

	if (READ_ONCE(head->crtc_active) && head->cur_timing_valid)
		hb = (head->cur_timing[6] | (head->cur_timing[7] << 8)) -
		     (head->cur_timing[8] | (head->cur_timing[9] << 8));
	if (hb == aud->warned_hb)
		return;
	aud->warned_hb = hb;
	if (!hb)
		dev_info(&aud->t6->udev->dev,
			 "HDMI audio: the output is off, nothing is heard until it is on\n");
	else if (hb < T6_AUD_MIN_HBLANK)
		dev_warn(&aud->t6->udev->dev,
			 "HDMI audio is distorted with less than %u px horizontal blanking (this mode: %u px)\n",
			 T6_AUD_MIN_HBLANK, hb);
}

/*
 * Keeps the device audio_lead_ms ahead of playback, paced by the host clock,
 * and starts and stops the engine with the stream. Only what the
 * application has written is sent; if it falls behind and the device is
 * about to run dry, silence (the device would otherwise replay old contents
 * of its ring buffer).
 */
static void t6_audio_work(struct work_struct *work)
{
	struct t6_audio *aud = container_of(work, struct t6_audio, work);
	struct t6_device *t6 = aud->t6;
	struct snd_pcm_substream *sub;
	bool run, restart, elapsed = false;
	unsigned long flags;
	int idx;

	spin_lock_irqsave(&aud->lock, flags);
	run = aud->running;
	restart = aud->restart;
	aud->restart = false;
	sub = aud->sub;
	spin_unlock_irqrestore(&aud->lock, flags);

	if (!drm_dev_enter(&t6->drm, &idx))
		return;
	mutex_lock(&t6->io_lock);
	if (run && (!aud->engine || restart)) {
		if (!aud->engine) {
			aud->engine = !t6_audio_engine(aud, true);
			aud->starts++;
			t6_audio_check_mode(aud);
		}
		spin_lock_irqsave(&aud->lock, flags);
		aud->start = ktime_get();
		aud->dev = 0;
		aud->silence = 0;
		aud->lead = T6_AUD_RATE / 1000 *
			    clamp(READ_ONCE(t6_audio_lead_ms), 20U, 500U);
		aud->base = aud->data;
		aud->ptr = aud->data;
		aud->notified = aud->data;
		spin_unlock_irqrestore(&aud->lock, flags);
		/* The lead plays first as silence: no jump into the ALSA buffer. */
		if (aud->engine && sub)
			while (aud->dev < aud->lead &&
			       !t6_audio_packet(aud, sub->runtime, 0, false))
				;
	} else if (!run && aud->engine) {
		t6_audio_engine(aud, false);
		aud->engine = false;
	}
	if (run && aud->engine && sub) {
		struct snd_pcm_runtime *rt = sub->runtime;
		bool draining = READ_ONCE(rt->state) == SNDRV_PCM_STATE_DRAINING;
		s64 played = div_s64(ktime_us_delta(ktime_get(), aud->start) *
				     T6_AUD_RATE, USEC_PER_SEC);
		u64 ptr;

		while ((s64)aud->dev < played + aud->lead) {
			snd_pcm_uframes_t queued = (READ_ONCE(rt->control->appl_ptr) -
						    aud->data) % rt->boundary;
			unsigned int n = min_t(snd_pcm_uframes_t, queued,
					       T6_AUD_PKT_F);

			if (n < T6_AUD_PKT_F && !draining) {
				/* Above half the lead: wait for the application. */
				if ((s64)aud->dev >= played + aud->lead / 2)
					break;
				n = 0;	/* about to run dry: silence */
			}
			if (t6_audio_packet(aud, rt, n, !draining))
				break;
		}

		spin_lock_irqsave(&aud->lock, flags);
		ptr = t6_audio_hwptr(aud);
		if (div_u64(ptr, rt->period_size) !=
		    div_u64(aud->notified, rt->period_size) ||
		    (draining && ptr != aud->notified)) {
			aud->notified = ptr;
			elapsed = true;
		}
		spin_unlock_irqrestore(&aud->lock, flags);
	}
	mutex_unlock(&t6->io_lock);
	drm_dev_exit(idx);

	/* May stop the stream (xrun) and so call trigger: not under io_lock. */
	if (elapsed)
		snd_pcm_period_elapsed(sub);
}

static enum hrtimer_restart t6_audio_tick(struct hrtimer *timer)
{
	struct t6_audio *aud = container_of(timer, struct t6_audio, timer);

	if (!READ_ONCE(aud->running))
		return HRTIMER_NORESTART;
	queue_work(system_highpri_wq, &aud->work);
	hrtimer_forward_now(timer, ms_to_ktime(T6_AUD_TICK_MS));
	return HRTIMER_RESTART;
}

static const struct snd_pcm_hardware t6_audio_hw = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_RESUME | SNDRV_PCM_INFO_BATCH,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_48000,
	.rate_min = T6_AUD_RATE,
	.rate_max = T6_AUD_RATE,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = 256 * 1024,
	.period_bytes_min = T6_AUD_PKT,
	.period_bytes_max = 128 * 1024,
	.periods_min = 2,
	.periods_max = 128,
};

static int t6_audio_open(struct snd_pcm_substream *sub)
{
	sub->runtime->hw = t6_audio_hw;
	return 0;
}

static int t6_audio_close(struct snd_pcm_substream *sub)
{
	return 0;
}

static int t6_audio_prepare(struct snd_pcm_substream *sub)
{
	struct t6_audio *aud = snd_pcm_substream_chip(sub);
	unsigned long flags;

	/* Stopped and synced: the work is idle. */
	spin_lock_irqsave(&aud->lock, flags);
	aud->pos = 0;
	aud->data = 0;
	aud->base = 0;
	aud->ptr = 0;
	aud->notified = 0;
	spin_unlock_irqrestore(&aud->lock, flags);
	return 0;
}

/* Before the buffer is freed or reset: the work must not be copying from it. */
static int t6_audio_sync_stop(struct snd_pcm_substream *sub)
{
	struct t6_audio *aud = snd_pcm_substream_chip(sub);

	hrtimer_cancel(&aud->timer);
	flush_work(&aud->work);
	return 0;
}

static int t6_audio_trigger(struct snd_pcm_substream *sub, int cmd)
{
	struct t6_audio *aud = snd_pcm_substream_chip(sub);
	unsigned long flags;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		spin_lock_irqsave(&aud->lock, flags);
		aud->sub = sub;
		aud->running = true;
		aud->restart = true;
		spin_unlock_irqrestore(&aud->lock, flags);
		hrtimer_start(&aud->timer, 0, HRTIMER_MODE_REL);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		spin_lock_irqsave(&aud->lock, flags);
		aud->running = false;
		aud->sub = NULL;
		spin_unlock_irqrestore(&aud->lock, flags);
		hrtimer_try_to_cancel(&aud->timer);
		/* The work disables the engine. */
		queue_work(system_highpri_wq, &aud->work);
		return 0;
	}
	return -EINVAL;
}

/* The delay is what the device holds beyond what it plays now: the lead. */
static snd_pcm_uframes_t t6_audio_pointer(struct snd_pcm_substream *sub)
{
	struct t6_audio *aud = snd_pcm_substream_chip(sub);
	struct snd_pcm_runtime *rt = sub->runtime;
	unsigned long flags;
	u64 ptr;

	spin_lock_irqsave(&aud->lock, flags);
	ptr = t6_audio_hwptr(aud);
	spin_unlock_irqrestore(&aud->lock, flags);
	rt->delay = READ_ONCE(aud->lead);
	return do_div(ptr, rt->buffer_size);
}

static const struct snd_pcm_ops t6_audio_ops = {
	.open = t6_audio_open,
	.close = t6_audio_close,
	.prepare = t6_audio_prepare,
	.sync_stop = t6_audio_sync_stop,
	.trigger = t6_audio_trigger,
	.pointer = t6_audio_pointer,
};

/*
 * The volume node (node 0 of engine 0): 0x23 with a channel mask, 7 zero
 * bytes and 8 s32 values, left and right first. The Windows dB table makes
 * the scale linear in amplitude (value / max); 0 is silence. There is no
 * mute node: muting sends the minimum, as Windows does. io_lock held.
 */
static int t6_audio_send_volume(struct t6_audio *aud)
{
	u8 b[40] = { 0 };
	int i, ret;

	b[0] = 0x03;
	for (i = 0; i < 2; i++)
		put_unaligned_le32(aud->unmuted ? aud->vol[i] : aud->vol_min,
				   b + 8 + 4 * i);
	ret = t6_ctrl_out(aud->t6, T6_REQ_AUD_NODE_SET, 0, 0, b, sizeof(b));
	return ret < 0 ? ret : 0;
}

/* Store and send under io_lock; 1 if changed. */
static int t6_audio_set_volume(struct t6_audio *aud, const int *vol, int unmuted)
{
	struct t6_device *t6 = aud->t6;
	int idx, ret = 0;

	mutex_lock(&t6->io_lock);
	if (vol[0] == aud->vol[0] && vol[1] == aud->vol[1] &&
	    unmuted == aud->unmuted) {
		mutex_unlock(&t6->io_lock);
		return 0;
	}
	aud->vol[0] = vol[0];
	aud->vol[1] = vol[1];
	aud->unmuted = unmuted;
	if (drm_dev_enter(&t6->drm, &idx)) {
		ret = t6_audio_send_volume(aud);
		drm_dev_exit(idx);
	}
	mutex_unlock(&t6->io_lock);
	return ret ?: 1;
}

static int t6_vol_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *info)
{
	struct t6_audio *aud = snd_kcontrol_chip(kc);

	info->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	info->count = 2;
	info->value.integer.min = aud->vol_min;
	info->value.integer.max = aud->vol_max;
	return 0;
}

static int t6_vol_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *v)
{
	struct t6_audio *aud = snd_kcontrol_chip(kc);

	mutex_lock(&aud->t6->io_lock);
	v->value.integer.value[0] = aud->vol[0];
	v->value.integer.value[1] = aud->vol[1];
	mutex_unlock(&aud->t6->io_lock);
	return 0;
}

static int t6_vol_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *v)
{
	struct t6_audio *aud = snd_kcontrol_chip(kc);
	int vol[2], i;

	for (i = 0; i < 2; i++) {
		if (v->value.integer.value[i] < aud->vol_min ||
		    v->value.integer.value[i] > aud->vol_max)
			return -EINVAL;
		vol[i] = v->value.integer.value[i];
	}
	return t6_audio_set_volume(aud, vol, READ_ONCE(aud->unmuted));
}

static int t6_switch_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *v)
{
	struct t6_audio *aud = snd_kcontrol_chip(kc);

	v->value.integer.value[0] = READ_ONCE(aud->unmuted);
	return 0;
}

static int t6_switch_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *v)
{
	struct t6_audio *aud = snd_kcontrol_chip(kc);
	int vol[2] = { aud->vol[0], aud->vol[1] };

	return t6_audio_set_volume(aud, vol, !!v->value.integer.value[0]);
}

/* gain = value / max: what ALSA's linear dB scale from mute to 0 dB means */
static const DECLARE_TLV_DB_LINEAR(t6_vol_tlv, TLV_DB_GAIN_MUTE, 0);

static const struct snd_kcontrol_new t6_vol_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "PCM Playback Volume",
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_TLV_READ,
	.info = t6_vol_info,
	.get = t6_vol_get,
	.put = t6_vol_put,
	.tlv.p = t6_vol_tlv,
};

static const struct snd_kcontrol_new t6_switch_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "PCM Playback Switch",
	.info = snd_ctl_boolean_mono_info,
	.get = t6_switch_get,
	.put = t6_switch_put,
};

/*
 * The volume node's range, then full volume, as Windows and macOS set when
 * the adapter appears: it powers up low (2 of 31 on the tested adapter),
 * which is barely audible. The desktop's volume control works on top of it.
 * Only a plain volume node (type 0) from 0 is used, so the dB scale is right.
 */
static void t6_audio_read_volume(struct t6_audio *aud)
{
	struct t6_device *t6 = aud->t6;
	u8 b[40];
	int lo, hi;

	mutex_lock(&t6->io_lock);
	if (t6_ctrl_in(t6, T6_REQ_AUD_NODE_CAPS, 0, 0, b, 16) != 16 || b[0] != 0)
		goto out;
	lo = get_unaligned_le32(b + 8);
	hi = get_unaligned_le32(b + 12);
	if (lo != 0 || hi <= lo || hi > 255 ||
	    t6_ctrl_in(t6, T6_REQ_AUD_NODE_VALUE, 0, 0, b, 40) != 40)
		goto out;
	aud->vol_min = lo;
	aud->vol_max = hi;
	aud->vol[0] = hi;
	aud->vol[1] = hi;
	aud->unmuted = true;
	aud->has_vol = true;
	if (t6_audio_send_volume(aud))
		dev_warn(&t6->udev->dev, "HDMI audio: setting full volume failed\n");
out:
	mutex_unlock(&t6->io_lock);
}

/*
 * A sound card for the adapter's HDMI audio when it reports the format this
 * driver sends (48 kHz, 16-bit, stereo). Without it the display still works.
 */
void t6_audio_init(struct t6_device *t6, struct usb_interface *intf)
{
	struct t6_audio *aud;
	struct snd_card *card;
	struct snd_pcm *pcm;
	u32 rate = 0, cyclic = 0;
	u16 format = 0;
	u8 b[32], bits = 0, ch = 0;
	int ret;

	if (!t6_audio_enable)
		return;
	mutex_lock(&t6->io_lock);
	if (t6_ctrl_in(t6, T6_REQ_AUD_FORMATS, 0, 0, b, 32) == 32) {
		rate = get_unaligned_le32(b);
		bits = b[6];
		ch = b[7];
	}
	if (t6_ctrl_in(t6, T6_REQ_AUD_STATE, 0, 0, b, 16) == 16) {
		format = get_unaligned_le16(b + 2);
		cyclic = get_unaligned_le32(b + 12);
	}
	mutex_unlock(&t6->io_lock);
	if (rate != T6_AUD_RATE || bits != 16 || ch != 2) {
		dev_info(&intf->dev, "No HDMI audio (adapter reports %u Hz, %u bit, %u channels)\n",
			 rate, bits, ch);
		return;
	}

	ret = snd_card_new(&intf->dev, -1, "Trigger6", THIS_MODULE,
			   sizeof(*aud), &card);
	if (ret)
		goto err;
	aud = card->private_data;
	aud->t6 = t6;
	aud->card = card;
	aud->format = format;
	aud->cyclic = cyclic ?: 0x20000;
	aud->warned_hb = U32_MAX;
	aud->pkt = devm_kzalloc(&intf->dev, T6_AUD_PKT, GFP_KERNEL);
	if (!aud->pkt) {
		ret = -ENOMEM;
		goto err_card;
	}
	spin_lock_init(&aud->lock);
	INIT_WORK(&aud->work, t6_audio_work);
#ifdef T6_HAVE_HRTIMER_SETUP
	hrtimer_setup(&aud->timer, t6_audio_tick, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
#else
	hrtimer_init(&aud->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	aud->timer.function = t6_audio_tick;
#endif

	strscpy(card->driver, "trigger6", sizeof(card->driver));
	strscpy(card->shortname, "Trigger6 USB to HDMI Adapter",
		sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname),
		 "Trigger6 USB to HDMI Adapter at %s", dev_name(&intf->dev));
	ret = snd_pcm_new(card, "HDMI", 0, 1, 0, &pcm);
	if (ret)
		goto err_card;
	pcm->private_data = aud;
	strscpy(pcm->name, "HDMI", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &t6_audio_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 0, 0);
	t6_audio_read_volume(aud);
	if (aud->has_vol) {
		ret = snd_ctl_add(card, snd_ctl_new1(&t6_vol_ctl, aud));
		if (!ret)
			ret = snd_ctl_add(card, snd_ctl_new1(&t6_switch_ctl, aud));
		if (ret)
			goto err_card;
	}
	ret = snd_card_register(card);
	if (ret)
		goto err_card;
	t6->audio = aud;
	dev_info(&intf->dev,
		 "HDMI audio: 48 kHz, 16 bit, stereo (sound card %d, format %u, buffer %u bytes%s)\n",
		 card->number, aud->format, aud->cyclic,
		 aud->has_vol ? ", volume control" : "");
	return;

err_card:
	snd_card_free(card);
err:
	dev_warn(&intf->dev, "No HDMI audio: %d\n", ret);
}

/* Unplug: no more audio work; the card goes once nobody has it open. */
void t6_audio_fini(struct t6_device *t6)
{
	struct t6_audio *aud = t6->audio;

	if (!aud)
		return;
	t6->audio = NULL;
	snd_card_disconnect(aud->card);
	hrtimer_cancel(&aud->timer);
	cancel_work_sync(&aud->work);
	snd_card_free_when_closed(aud->card);
}

/* Suspend: streams are suspended through the card; stop sending. */
void t6_audio_suspend(struct t6_device *t6)
{
	struct t6_audio *aud = t6->audio;

	if (!aud)
		return;
	snd_power_change_state(aud->card, SNDRV_CTL_POWER_D3hot);
	hrtimer_cancel(&aud->timer);
	cancel_work_sync(&aud->work);
	aud->engine = false;	/* enabled again by the next start */
}

/* The adapter may have lost power: send the volume again. */
void t6_audio_resume(struct t6_device *t6)
{
	struct t6_audio *aud = t6->audio;
	int idx;

	if (!aud)
		return;
	if (aud->has_vol && drm_dev_enter(&t6->drm, &idx)) {
		mutex_lock(&t6->io_lock);
		t6_audio_send_volume(aud);
		mutex_unlock(&t6->io_lock);
		drm_dev_exit(idx);
	}
	snd_power_change_state(aud->card, SNDRV_CTL_POWER_D0);
}

/*
 * Chip init sends "audio off" (request 0x24, activity 0), as the vendor
 * driver does. A running stream then has to switch the engine on again and
 * restart its timing. io_lock held.
 */
void t6_audio_engine_lost(struct t6_device *t6)
{
	struct t6_audio *aud = t6->audio;
	unsigned long flags;

	if (!aud || !aud->engine)
		return;
	aud->engine = false;
	spin_lock_irqsave(&aud->lock, flags);
	aud->restart = true;
	spin_unlock_irqrestore(&aud->lock, flags);
}

/* For t6_metrics. */
void t6_audio_metrics(struct t6_device *t6, char *buf, size_t size)
{
	struct t6_audio *aud = t6->audio;

	if (!aud) {
		strscpy(buf, "audio=none", size);
		return;
	}
	snprintf(buf, size,
		 "audio=card%d audio_engine=%u audio_starts=%llu audio_packets=%llu audio_prefills=%llu audio_fills=%llu",
		 aud->card->number, READ_ONCE(aud->engine), aud->starts,
		 aud->packets, aud->prefills, aud->fills);
}
