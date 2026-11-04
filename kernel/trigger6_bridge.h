/* SPDX-License-Identifier: GPL-2.0-only */
/* Optional userspace SIMD conversion. The USB protocol remains in the kernel. */
#define T6_BRIDGE_MAGIC 0x314e3654u /* T6N1 */
struct t6_bridge_packet {
	__le32 magic, width, height, stride, sequence, format, length, reserved;
} __packed;
struct t6_bridge_file {
	struct t6_head *head;
	int seen;
};

static int t6_bridge_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct t6_head *head = container_of(misc, struct t6_head, jpeg_miscdev);
	struct t6_bridge_file *ctx;
	if (atomic_cmpxchg(&head->bridge_open, 0, 1))
		return -EBUSY;
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		atomic_set(&head->bridge_open, 0);
		return -ENOMEM;
	}
	drm_dev_get(&head->t6->drm);
	ctx->head = head;
	file->private_data = ctx;
	WRITE_ONCE(head->bridge_heartbeat, jiffies);
	return 0;
}

static int t6_bridge_release(struct inode *inode, struct file *file)
{
	struct t6_bridge_file *ctx = file->private_data;
	struct t6_head *head = ctx->head;
	mutex_lock(&head->t6->tx_lock);
	head->last_sent_valid = false;
	WRITE_ONCE(head->bridge_waiting, false);
	atomic_set(&head->bridge_open, 0);
	mutex_unlock(&head->t6->tx_lock);
	drm_dev_put(&head->t6->drm);
	kfree(ctx);
	return 0;
}

static ssize_t t6_bridge_read(struct file *file, char __user *buf,
			      size_t len, loff_t *ppos)
{
	struct t6_bridge_file *ctx = file->private_data;
	struct t6_head *head = ctx->head;
	struct t6_device *t6 = head->t6;
	struct t6_bridge_packet packet;
	int ret, idx, sequence;
	size_t bytes;
	/* A helper blocked here is idle, not dead: see t6_bridge_export(). */
	WRITE_ONCE(head->bridge_waiting, true);
	ret = wait_event_interruptible(head->fb_export_wq,
		atomic_read(&head->fb_export_seq) != ctx->seen ||
		drm_dev_is_unplugged(&t6->drm));
	WRITE_ONCE(head->bridge_waiting, false);
	if (ret)
		return ret;
	if (!drm_dev_enter(&t6->drm, &idx))
		return -ENODEV;
	mutex_lock(&t6->tx_lock);
	sequence = atomic_read(&head->fb_export_seq);
	bytes = head->fb_export_len;
	if (!head->fb_export_buf || !bytes) {
		ret = -EAGAIN;
		goto out;
	}
	if (len < sizeof(packet) + bytes) {
		ret = -EMSGSIZE;
		goto out;
	}
	memset(&packet, 0, sizeof(packet));
	packet.magic = cpu_to_le32(T6_BRIDGE_MAGIC);
	packet.width = cpu_to_le32(head->fb_export_width);
	packet.height = cpu_to_le32(head->fb_export_height);
	packet.stride = cpu_to_le32(head->fb_export_width * 4);
	packet.sequence = cpu_to_le32(sequence);
	packet.format = cpu_to_le32(T6_FMT_RGB32);
	packet.length = cpu_to_le32(bytes);
	/* Producer uses this mutex too: copy_to_user never sees a reused buffer. */
	if (copy_to_user(buf, &packet, sizeof(packet)) ||
	    copy_to_user(buf + sizeof(packet), head->fb_export_buf, bytes)) {
		ret = -EFAULT;
		goto out;
	}
	ctx->seen = sequence;
	WRITE_ONCE(head->bridge_heartbeat, jiffies);
	ret = sizeof(packet) + bytes;
out:
	mutex_unlock(&t6->tx_lock);
	drm_dev_exit(idx);
	return ret;
}

static ssize_t t6_bridge_write(struct file *file, const char __user *buf,
			       size_t len, loff_t *ppos)
{
	struct t6_bridge_file *ctx = file->private_data;
	struct t6_head *head = ctx->head;
	struct t6_device *t6 = head->t6;
	struct t6_bridge_packet packet;
	u32 width, height, pitch, bytes;
	int ret, idx;
	if (len < sizeof(packet) || len > sizeof(packet) + head->tx_buf_size)
		return -EINVAL;
	if (copy_from_user(&packet, buf, sizeof(packet)))
		return -EFAULT;
	width = le32_to_cpu(packet.width);
	height = le32_to_cpu(packet.height);
	pitch = ALIGN(width, 16);
	if (le32_to_cpu(packet.magic) != T6_BRIDGE_MAGIC ||
	    le32_to_cpu(packet.format) != T6_FMT_NV12 ||
	    !width || !height || width > T6_MAX_WIDTH || height > T6_MAX_HEIGHT ||
	    (width & 1) || (height & 1) || le32_to_cpu(packet.stride) != pitch)
		return -EINVAL;
	bytes = pitch * height * 3 / 2;
	if (le32_to_cpu(packet.length) != bytes || len != sizeof(packet) + bytes)
		return -EINVAL;
	mutex_lock(&t6->io_lock);
	if (!drm_dev_enter(&t6->drm, &idx)) {
		ret = -ENODEV;
		goto out;
	}
	if (width != head->width || height != head->height) {
		ret = -ESTALE;
		goto out_exit;
	}
	if (!t6_head_runtime_connected(head)) {
		ret = -ENODEV;
		goto out_exit;
	}
	/* Output switched off after this frame was exported: drop it. */
	if (!head->crtc_active) {
		ret = -ESTALE;
		goto out_exit;
	}
	if (bytes + sizeof(struct t6_flip_header) + T6_JPEG_PADDING_SIZE >
	    head->jpeg_staging_size) {
		ret = -EINVAL;
		goto out_exit;
	}
	if (copy_from_user(head->jpeg_staging + sizeof(struct t6_flip_header),
			   buf + sizeof(packet), bytes)) {
		ret = -EFAULT;
		goto out_exit;
	}
	memset(head->jpeg_staging + sizeof(struct t6_flip_header) + bytes,
	       0, T6_JPEG_PADDING_SIZE);
	ret = t6_send_prepared_nv12(t6, head, bytes);
	if (!ret) {
		WRITE_ONCE(head->bridge_heartbeat, jiffies);
		mutex_lock(&t6->tx_lock);
		head->last_sent_valid = false;
		mutex_unlock(&t6->tx_lock);
	}
out_exit:
	drm_dev_exit(idx);
out:
	mutex_unlock(&t6->io_lock);
	return ret ? ret : len;
}

static const struct file_operations t6_bridge_fops = {
	.owner = THIS_MODULE,
	.open = t6_bridge_open,
	.release = t6_bridge_release,
	.read = t6_bridge_read,
	.write = t6_bridge_write,
	.llseek = noop_llseek,
};

static int t6_register_nv12_bridge(struct t6_head *head)
{
	int ret;
	snprintf(head->jpeg_dev_name, sizeof(head->jpeg_dev_name),
		 "trigger6-%03u-%03u-out0-nv12", head->t6->udev->bus->busnum,
		 head->t6->udev->devnum);
	head->jpeg_miscdev.minor = MISC_DYNAMIC_MINOR;
	head->jpeg_miscdev.name = head->jpeg_dev_name;
	head->jpeg_miscdev.fops = &t6_bridge_fops;
	head->jpeg_miscdev.mode = 0600;
	ret = misc_register(&head->jpeg_miscdev);
	if (!ret)
		head->jpeg_misc_registered = true;
	return ret;
}

static bool t6_bridge_export(struct t6_head *head, const void *pixels, u32 stride)
{
	struct t6_device *t6 = head->t6;
	bool alive;
	int ret, idx;
	if (!atomic_read(&head->bridge_open))
		return false;
	if (!drm_dev_enter(&t6->drm, &idx))
		return true;
	mutex_lock(&t6->tx_lock);
	ret = t6_pack_xrgb8888_tight(head, pixels, stride,
				   head->fb_export_buf, head->tx_buf_size);
	/*
	 * The helper owns this frame if it is blocked in read() waiting for one,
	 * or was active within 200 ms. Only a helper that is stuck (stopped,
	 * hung in conversion or write) falls back to kernel conversion. Judging
	 * by the last read/write alone sent every first frame after an idle
	 * pause down both paths. Sample before waking it: it clears the flag.
	 */
	alive = READ_ONCE(head->bridge_waiting) ||
		time_before(jiffies, READ_ONCE(head->bridge_heartbeat) +
			    msecs_to_jiffies(200));
	if (!ret) {
		head->fb_export_width = head->width;
		head->fb_export_height = head->height;
		head->fb_export_len = (size_t)head->width * head->height * 4;
		atomic_inc(&head->fb_export_seq);
	}
	mutex_unlock(&t6->tx_lock);
	drm_dev_exit(idx);
	if (!ret)
		wake_up_interruptible(&head->fb_export_wq);
	return !ret && alive;
}
