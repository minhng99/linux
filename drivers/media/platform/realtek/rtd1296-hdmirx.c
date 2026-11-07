// SPDX-License-Identifier: GPL-2.0-only
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/v4l2-dv-timings.h>
#include <media/v4l2-dv-timings.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-contig.h>

#include "rtd1296-hdmirx.h"

static void rx_select_mode(struct rtd1296_rx *rx, const struct rtd1296_rx_mode *mode)
{
	rx->mode = mode;
	rx->width = mode->timings.bt.width / mode->scale_divisor;
	rx->height = mode->timings.bt.height / mode->scale_divisor;
	rx->stride = rx->width * 4;
	rx->sizeimage = rx->stride * rx->height;
}

static void rx_source_event(struct rtd1296_rx *rx)
{
	struct v4l2_event event = {
		.type = V4L2_EVENT_SOURCE_CHANGE,
		.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION,
	};

	v4l2_event_queue(&rx->video, &event);
}

static struct rtd1296_rx_buffer *rx_next_buffer(struct rtd1296_rx *rx)
{
	struct rtd1296_rx_buffer *buffer;

	if (list_empty(&rx->pending))
		return NULL;
	buffer = list_first_entry(&rx->pending, struct rtd1296_rx_buffer, list);
	list_del(&buffer->list);
	return buffer;
}

static void rx_program_slot(struct rtd1296_rx *rx, unsigned int slot)
{
	dma_addr_t address = rx->scratch_dma;

	if (rx->slot[slot])
		address = vb2_dma_contig_plane_dma_addr(&rx->slot[slot]->vb.vb2_buf, 0);
	writel(lower_32_bits(address), rx->dma + RX_DMA_ADDRESS(slot));
	writel(lower_32_bits(address), rx->dma + RX_DMA_UV_ADDRESS(slot));
}

static void rx_return_buffers(struct rtd1296_rx *rx, enum vb2_buffer_state state)
{
	struct rtd1296_rx_buffer *buffer;
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&rx->queue_lock, flags);
	for (i = 0; i < ARRAY_SIZE(rx->slot); i++) {
		if (!rx->slot[i])
			continue;
		vb2_buffer_done(&rx->slot[i]->vb.vb2_buf, state);
		rx->slot[i] = NULL;
	}
	while ((buffer = rx_next_buffer(rx)))
		vb2_buffer_done(&buffer->vb.vb2_buf, state);
	spin_unlock_irqrestore(&rx->queue_lock, flags);
}

static void rx_dma_stop(struct rtd1296_rx *rx)
{
	unsigned long flags;

	spin_lock_irqsave(&rx->queue_lock, flags);
	if (rx->powered) {
		rx_update(rx->wrapper, RX_WRAP_CONTROL, RX_WRAP_DMA, 0);
		writel(0, rx->wrapper + RX_WRAP_IRQ_ENABLE);
		writel(0, rx->dma + RX_DMA_CONTROL);
		writel(0, rx->dma + RX_DMA_FORMAT_CONTROL);
		readl(rx->dma + RX_DMA_CONTROL);
	}
	rx->dma_running = false;
	spin_unlock_irqrestore(&rx->queue_lock, flags);
	synchronize_irq(rx->irq);
	if (rx->powered) {
		reset_control_assert(rx->resets[2].rstc);
		usleep_range(10, 20);
		reset_control_deassert(rx->resets[2].rstc);
	}
}

static void rx_signal_lost(struct rtd1296_rx *rx)
{
	if (rx->streaming) {
		rx_dma_stop(rx);
		vb2_queue_error(&rx->queue);
		rx_return_buffers(rx, VB2_BUF_STATE_ERROR);
	}
	if (rx->locked) {
		rx->locked = false;
		rx_source_event(rx);
	}
	rx->signal_mode = NULL;
	rx->phy_mode = NULL;
	rx->stable_count = 0;
}

static irqreturn_t rx_irq(int irq, void *data)
{
	struct rtd1296_rx *rx = data;
	struct rtd1296_rx_buffer *buffer;
	unsigned long flags;
	u32 status, wrapper_status;
	unsigned int slot;

	spin_lock_irqsave(&rx->queue_lock, flags);
	if (!rx->dma_running) {
		spin_unlock_irqrestore(&rx->queue_lock, flags);
		return IRQ_NONE;
	}
	status = readl(rx->dma + RX_DMA_STATUS) & (RX_DMA_IRQS | RX_DMA_FORMAT_ERRORS);
	wrapper_status = readl(rx->wrapper + RX_WRAP_IRQ_STATUS) &
			 RX_WRAP_DIMENSION_ERROR;
	if (!status && !wrapper_status) {
		spin_unlock_irqrestore(&rx->queue_lock, flags);
		return IRQ_NONE;
	}
	writel(status, rx->dma + RX_DMA_STATUS);
	if (wrapper_status || (status & (RX_DMA_ERRORS | RX_DMA_FORMAT_ERRORS)) ||
	    (status & (RX_DMA_DONE(0) | RX_DMA_DONE(1))) ==
	    (RX_DMA_DONE(0) | RX_DMA_DONE(1))) {
		rx_update(rx->wrapper, RX_WRAP_CONTROL, RX_WRAP_DMA, 0);
		writel(0, rx->wrapper + RX_WRAP_IRQ_ENABLE);
		writel(0, rx->dma + RX_DMA_CONTROL);
		writel(0, rx->dma + RX_DMA_FORMAT_CONTROL);
		readl(rx->dma + RX_DMA_CONTROL);
		rx->dma_running = false;
		vb2_queue_error(&rx->queue);
		spin_unlock_irqrestore(&rx->queue_lock, flags);
		return IRQ_HANDLED;
	}
	for (slot = 0; slot < ARRAY_SIZE(rx->slot); slot++) {
		if (!(status & RX_DMA_DONE(slot)))
			continue;
		buffer = rx->slot[slot];
		rx->slot[slot] = rx_next_buffer(rx);
		rx_program_slot(rx, slot);
		readl(rx->dma + RX_DMA_ADDRESS(slot));
		if (buffer) {
			buffer->vb.vb2_buf.timestamp = ktime_get_ns();
			buffer->vb.sequence = rx->sequence;
			buffer->vb.field = V4L2_FIELD_NONE;
			vb2_buffer_done(&buffer->vb.vb2_buf, VB2_BUF_STATE_DONE);
		}
		rx->sequence++;
		WRITE_ONCE(rx->last_frame, jiffies);
	}
	spin_unlock_irqrestore(&rx->queue_lock, flags);
	return IRQ_HANDLED;
}

static void rx_power_down(struct rtd1296_rx *rx)
{
	if (!rx->powered)
		return;
	gpiod_set_value_cansleep(rx->hpd, 0);
	rx_signal_lost(rx);
	rx->powered = false;
	pm_runtime_mark_last_busy(rx->dev);
	pm_runtime_put_sync_suspend(rx->dev);
}

static int rx_power_up(struct rtd1296_rx *rx)
{
	int ret;

	ret = pm_runtime_resume_and_get(rx->dev);
	if (ret < 0)
		return ret;
	rx->powered = true;
	ret = rtd1296_rx_edid_write(rx);
	if (ret) {
		rx_power_down(rx);
		return ret;
	}
	msleep(100);
	gpiod_set_value_cansleep(rx->hpd, 1);
	return 0;
}

static void rx_detect_work(struct work_struct *work)
{
	struct rtd1296_rx *rx = container_of(to_delayed_work(work),
					   struct rtd1296_rx, detect_work);
	const struct rtd1296_rx_mode *mode;
	int present, ret;

	mutex_lock(&rx->lock);
	if (rx->removing || rx->suspended)
		goto unlock;
	present = !!(readl(rx->cbus + RX_CBUS_STATUS) & RX_CBUS_5V_PRESENT);
	if (rx->cable != present) {
		rx->cable = present;
		__v4l2_ctrl_s_ctrl(rx->power_control, present);
	}
	if (!present || !rx->edid_blocks) {
		rx_power_down(rx);
		goto reschedule;
	}
	if (READ_ONCE(rx->dma_running) &&
	    time_after(jiffies, READ_ONCE(rx->last_frame) + msecs_to_jiffies(1000))) {
		rx_power_down(rx);
		goto reschedule;
	}
	if (!rx->powered) {
		ret = rx_power_up(rx);
		if (ret) {
			dev_err_ratelimited(rx->dev, "receiver startup failed: %d\n", ret);
			goto reschedule;
		}
	}
	ret = rtd1296_rx_detect(rx, &mode, true);
	if (ret && ret != -EAGAIN) {
		rx_signal_lost(rx);
	} else if (!ret && !rx->locked) {
		rx->signal_mode = mode;
		rx->locked = true;
		rx_source_event(rx);
	}
reschedule:
	queue_delayed_work(system_dfl_wq, &rx->detect_work, msecs_to_jiffies(250));
unlock:
	mutex_unlock(&rx->lock);
}

static int rx_queue_setup(struct vb2_queue *queue, unsigned int *buffers,
			  unsigned int *planes, unsigned int sizes[],
			  struct device *alloc_devs[])
{
	struct rtd1296_rx *rx = vb2_get_drv_priv(queue);

	if (*planes)
		return *planes == 1 && sizes[0] >= rx->sizeimage ? 0 : -EINVAL;
	*planes = 1;
	sizes[0] = rx->sizeimage;
	return 0;
}

static int rx_buffer_prepare(struct vb2_buffer *vb)
{
	struct rtd1296_rx *rx = vb2_get_drv_priv(vb->vb2_queue);

	if (vb2_plane_size(vb, 0) < rx->sizeimage)
		return -EINVAL;
	vb2_set_plane_payload(vb, 0, rx->sizeimage);
	return 0;
}

static void rx_buffer_queue(struct vb2_buffer *vb)
{
	struct rtd1296_rx *rx = vb2_get_drv_priv(vb->vb2_queue);
	struct rtd1296_rx_buffer *buffer = container_of(to_vb2_v4l2_buffer(vb),
						struct rtd1296_rx_buffer, vb);
	unsigned long flags;

	spin_lock_irqsave(&rx->queue_lock, flags);
	list_add_tail(&buffer->list, &rx->pending);
	spin_unlock_irqrestore(&rx->queue_lock, flags);
}

static int rx_start_streaming(struct vb2_queue *queue, unsigned int count)
{
	struct rtd1296_rx *rx = vb2_get_drv_priv(queue);
	const struct v4l2_bt_timings *bt = &rx->mode->timings.bt;
	const struct rtd1296_rx_mode *mode;
	unsigned long flags;
	unsigned int i;
	int ret = -ENOLINK;

	if (!rx->locked || !rx->powered || rx->suspended || rx->removing)
		goto fail;
	ret = rtd1296_rx_detect(rx, &mode, false);
	if (ret)
		goto fail;
	if (mode != rx->mode) {
		ret = -EPIPE;
		goto fail;
	}
	rx->scratch = dma_alloc_coherent(rx->dev, rx->sizeimage,
					 &rx->scratch_dma, GFP_KERNEL);
	if (!rx->scratch) {
		ret = -ENOMEM;
		goto fail;
	}
	writel(0, rx->dma + RX_DMA_CONTROL);
	writel(0, rx->dma + RX_DMA_TYPE);
	writel(rx->width << 16 | rx->stride,
	       rx->dma + RX_DMA_SIZE);
	writel(bt->width, rx->dma + RX_DMA_SOURCE_SIZE);
	writel(bt->height, rx->dma + RX_DMA_SOURCE_HEIGHT);
	writel(0, rx->dma + RX_DMA_H_INITIAL);
	writel(0, rx->dma + RX_DMA_V_INITIAL);
	/* Keep output counters active across input timing changes. */
	writel(FIELD_PREP(RX_DMA_SCALER_OUTPUT, rx->width) |
	       RX_DMA_SCALER_UNITY * rx->mode->scale_divisor,
	       rx->dma + RX_DMA_H_STEP);
	writel(FIELD_PREP(RX_DMA_SCALER_OUTPUT, rx->height) |
	       RX_DMA_SCALER_UNITY * rx->mode->scale_divisor,
	       rx->dma + RX_DMA_V_STEP);
	/* Integer sampling selects the zero-phase centre tap. */
	for (i = 0; i < 8; i++) {
		u32 coefficient = i == 7 ? RX_DMA_H_CENTER : 0;

		writel(coefficient, rx->dma + RX_DMA_H_LUMA(i));
		writel(coefficient, rx->dma + RX_DMA_H_CHROMA(i));
	}
	writel(bt->height << 16 | bt->width,
	       rx->wrapper + RX_WRAP_SIZE);
	writel(RX_DMA_IRQS | RX_DMA_FORMAT_ERRORS, rx->dma + RX_DMA_STATUS);
	writel(RX_DMA_FORMAT_CHECK | RX_DMA_FORMAT_IRQ,
	       rx->dma + RX_DMA_FORMAT_CONTROL);
	spin_lock_irqsave(&rx->queue_lock, flags);
	for (i = 0; i < ARRAY_SIZE(rx->slot); i++) {
		rx->slot[i] = rx_next_buffer(rx);
		rx_program_slot(rx, i);
	}
	rx->sequence = 0;
	WRITE_ONCE(rx->last_frame, jiffies);
	rx->streaming = true;
	rx->dma_running = true;
	writel(RX_WRAP_DIMENSION_ERROR, rx->wrapper + RX_WRAP_IRQ_ENABLE);
	writel(RX_DMA_RGB | RX_DMA_HSCALE | RX_DMA_VSCALE | RX_DMA_VNEAREST |
	       RX_DMA_IRQS | RX_DMA_ENABLE,
	       rx->dma + RX_DMA_CONTROL);
	rx_update(rx->wrapper, RX_WRAP_CONTROL, RX_WRAP_DMA, RX_WRAP_DMA);
	spin_unlock_irqrestore(&rx->queue_lock, flags);
	return 0;
fail:
	rx_return_buffers(rx, VB2_BUF_STATE_QUEUED);
	return ret;
}

static void rx_stop_streaming(struct vb2_queue *queue)
{
	struct rtd1296_rx *rx = vb2_get_drv_priv(queue);

	rx_dma_stop(rx);
	rx->streaming = false;
	rx_return_buffers(rx, VB2_BUF_STATE_ERROR);
	if (rx->scratch) {
		dma_free_coherent(rx->dev, rx->sizeimage,
				  rx->scratch, rx->scratch_dma);
		rx->scratch = NULL;
	}
}

static const struct vb2_ops rx_queue_ops = {
	.queue_setup = rx_queue_setup,
	.buf_prepare = rx_buffer_prepare,
	.buf_queue = rx_buffer_queue,
	.start_streaming = rx_start_streaming,
	.stop_streaming = rx_stop_streaming,
};

static void rx_format(struct rtd1296_rx *rx, struct v4l2_pix_format *pix)
{
	*pix = (struct v4l2_pix_format) {
		.width = rx->width,
		.height = rx->height,
		.pixelformat = V4L2_PIX_FMT_XBGR32,
		.field = V4L2_FIELD_NONE,
		.bytesperline = rx->stride,
		.sizeimage = rx->sizeimage,
		.colorspace = V4L2_COLORSPACE_SRGB,
		.xfer_func = V4L2_XFER_FUNC_SRGB,
		.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT,
		.quantization = V4L2_QUANTIZATION_FULL_RANGE,
	};
}

static int rx_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	struct rtd1296_rx *rx = video_drvdata(file);

	strscpy(cap->driver, "rtd1296-hdmirx", sizeof(cap->driver));
	strscpy(cap->card, "RTD1296 HDMI receiver", sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s", dev_name(rx->dev));
	return 0;
}

static int rx_enum_fmt(struct file *file, void *priv, struct v4l2_fmtdesc *fmt)
{
	if (fmt->index)
		return -EINVAL;
	fmt->pixelformat = V4L2_PIX_FMT_XBGR32;
	return 0;
}

static int rx_get_fmt(struct file *file, void *priv, struct v4l2_format *format)
{
	rx_format(video_drvdata(file), &format->fmt.pix);
	return 0;
}

static int rx_set_fmt(struct file *file, void *priv, struct v4l2_format *format)
{
	struct rtd1296_rx *rx = video_drvdata(file);

	if (vb2_is_busy(&rx->queue))
		return -EBUSY;
	return rx_get_fmt(file, priv, format);
}

static int rx_enum_input(struct file *file, void *priv, struct v4l2_input *input)
{
	struct rtd1296_rx *rx = video_drvdata(file);

	if (input->index)
		return -EINVAL;
	strscpy(input->name, "HDMI", sizeof(input->name));
	input->type = V4L2_INPUT_TYPE_CAMERA;
	input->capabilities = V4L2_IN_CAP_DV_TIMINGS;
	input->status = rx->locked ? 0 : V4L2_IN_ST_NO_SIGNAL;
	return 0;
}

static int rx_get_input(struct file *file, void *priv, unsigned int *input)
{
	*input = 0;
	return 0;
}

static int rx_set_input(struct file *file, void *priv, unsigned int input)
{
	return input ? -EINVAL : 0;
}

static int rx_get_timings(struct file *file, void *priv,
			  struct v4l2_dv_timings *timings)
{
	struct rtd1296_rx *rx = video_drvdata(file);

	*timings = rx->mode->timings;
	return 0;
}

static int rx_query_timings(struct file *file, void *priv,
			    struct v4l2_dv_timings *timings)
{
	struct rtd1296_rx *rx = video_drvdata(file);

	if (!rx->locked)
		return -ENOLINK;
	*timings = rx->signal_mode->timings;
	return 0;
}

static int rx_set_timings(struct file *file, void *priv,
			  struct v4l2_dv_timings *timings)
{
	struct rtd1296_rx *rx = video_drvdata(file);
	unsigned int i;

	if (vb2_is_busy(&rx->queue))
		return -EBUSY;
	for (i = 0; i < ARRAY_SIZE(rtd1296_rx_modes); i++) {
		if (!v4l2_match_dv_timings(timings, &rtd1296_rx_modes[i].timings, 0, false))
			continue;
		rx_select_mode(rx, &rtd1296_rx_modes[i]);
		*timings = rx->mode->timings;
		return 0;
	}
	return -EINVAL;
}

static int rx_enum_timings(struct file *file, void *priv,
			   struct v4l2_enum_dv_timings *timings)
{
	if (timings->pad || timings->index >= ARRAY_SIZE(rtd1296_rx_modes))
		return -EINVAL;
	timings->timings = rtd1296_rx_modes[timings->index].timings;
	return 0;
}

static int rx_timings_cap(struct file *file, void *priv,
			  struct v4l2_dv_timings_cap *cap)
{
	if (cap->pad)
		return -EINVAL;
	cap->type = V4L2_DV_BT_656_1120;
	cap->bt = (struct v4l2_bt_timings_cap) {
		.min_width = 1280,
		.max_width = 3840,
		.min_height = 720,
		.max_height = 2160,
		.min_pixelclock = 74250000,
		.max_pixelclock = 297000000,
		.standards = V4L2_DV_BT_STD_CEA861,
		.capabilities = V4L2_DV_BT_CAP_PROGRESSIVE,
	};
	return 0;
}

static int rx_get_edid(struct file *file, void *priv, struct v4l2_edid *edid)
{
	struct rtd1296_rx *rx = video_drvdata(file);

	if (edid->pad)
		return -EINVAL;
	if (!edid->start_block && !edid->blocks) {
		edid->blocks = rx->edid_blocks;
		return 0;
	}
	if (!rx->edid_blocks)
		return -ENODATA;
	if (edid->start_block >= rx->edid_blocks || !edid->blocks)
		return -EINVAL;
	edid->blocks = min(edid->blocks, rx->edid_blocks - edid->start_block);
	memcpy(edid->edid, rx->edid + edid->start_block * 128, edid->blocks * 128);
	return 0;
}

static bool rx_valid_edid(const u8 *edid, unsigned int blocks)
{
	static const u8 header[] = { 0, 255, 255, 255, 255, 255, 255, 0 };
	unsigned int i, b;
	u8 sum;

	if (memcmp(edid, header, sizeof(header)) || edid[126] != blocks - 1)
		return false;
	for (b = 0; b < blocks; b++) {
		sum = 0;
		for (i = 0; i < 128; i++)
			sum += edid[b * 128 + i];
		if (sum)
			return false;
	}
	return true;
}

static int rx_set_edid(struct file *file, void *priv, struct v4l2_edid *edid)
{
	struct rtd1296_rx *rx = video_drvdata(file);
	int ret;

	if (edid->pad || edid->start_block)
		return -EINVAL;
	if (edid->blocks > RTD1296_RX_EDID_BLOCKS) {
		edid->blocks = RTD1296_RX_EDID_BLOCKS;
		return -E2BIG;
	}
	if (edid->blocks && !rx_valid_edid(edid->edid, edid->blocks))
		return -EINVAL;
	if (vb2_is_streaming(&rx->queue))
		return -EBUSY;
	rx_power_down(rx);
	rx->edid_blocks = 0;
	ret = pm_runtime_resume_and_get(rx->dev);
	if (ret < 0)
		return ret;
	if (edid->blocks)
		memcpy(rx->edid, edid->edid, edid->blocks * 128);
	rx->edid_blocks = edid->blocks;
	ret = rtd1296_rx_edid_write(rx);
	if (ret)
		rx->edid_blocks = 0;
	pm_runtime_put_sync_suspend(rx->dev);
	mod_delayed_work(system_dfl_wq, &rx->detect_work, 0);
	return ret;
}

static int rx_subscribe(struct v4l2_fh *fh,
			const struct v4l2_event_subscription *subscription)
{
	if (subscription->type == V4L2_EVENT_SOURCE_CHANGE)
		return v4l2_src_change_event_subscribe(fh, subscription);
	return v4l2_ctrl_subscribe_event(fh, subscription);
}

static const struct v4l2_ioctl_ops rx_ioctl_ops = {
	.vidioc_querycap = rx_querycap,
	.vidioc_enum_fmt_vid_cap = rx_enum_fmt,
	.vidioc_g_fmt_vid_cap = rx_get_fmt,
	.vidioc_try_fmt_vid_cap = rx_get_fmt,
	.vidioc_s_fmt_vid_cap = rx_set_fmt,
	.vidioc_enum_input = rx_enum_input,
	.vidioc_g_input = rx_get_input,
	.vidioc_s_input = rx_set_input,
	.vidioc_g_dv_timings = rx_get_timings,
	.vidioc_s_dv_timings = rx_set_timings,
	.vidioc_query_dv_timings = rx_query_timings,
	.vidioc_enum_dv_timings = rx_enum_timings,
	.vidioc_dv_timings_cap = rx_timings_cap,
	.vidioc_g_edid = rx_get_edid,
	.vidioc_s_edid = rx_set_edid,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
	.vidioc_subscribe_event = rx_subscribe,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static const struct v4l2_file_operations rx_file_ops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
};

static int rx_detector_enable(struct rtd1296_rx *rx)
{
	struct reset_control_bulk_data *resets = rx->resets + RTD1296_RX_VIDEO_RESETS;
	struct clk_bulk_data *clocks = rx->clocks + RTD1296_RX_VIDEO_CLOCKS;
	int ret;

	ret = reset_control_bulk_deassert(RTD1296_RX_DETECT_RESETS, resets);
	if (ret)
		return ret;
	ret = clk_bulk_prepare_enable(RTD1296_RX_DETECT_CLOCKS, clocks);
	if (ret) {
		reset_control_bulk_assert(RTD1296_RX_DETECT_RESETS, resets);
		return ret;
	}
	rx_update(rx->cbus, RX_CBUS_POWER, RX_CBUS_PLL_POWER, RX_CBUS_PLL_POWER);
	usleep_range(200, 300);
	rx->detector_on = true;
	return 0;
}

static void rx_detector_disable(struct rtd1296_rx *rx)
{
	if (!rx->detector_on)
		return;
	rx_update(rx->cbus, RX_CBUS_POWER, RX_CBUS_PLL_POWER, 0);
	clk_bulk_disable_unprepare(RTD1296_RX_DETECT_CLOCKS,
				   rx->clocks + RTD1296_RX_VIDEO_CLOCKS);
	reset_control_bulk_assert(RTD1296_RX_DETECT_RESETS,
				  rx->resets + RTD1296_RX_VIDEO_RESETS);
	rx->detector_on = false;
}

static int rx_runtime_resume(struct device *dev)
{
	struct rtd1296_rx *rx = dev_get_drvdata(dev);
	int ret;

	ret = clk_bulk_prepare_enable(RTD1296_RX_VIDEO_CLOCKS, rx->clocks);
	if (ret)
		return ret;
	ret = reset_control_deassert(rx->domain_reset);
	if (ret)
		goto disable_clocks;
	ret = reset_control_bulk_deassert(RTD1296_RX_VIDEO_RESETS, rx->resets);
	if (ret)
		goto assert_domain;
	rtd1296_rx_hw_init(rx);
	return 0;
assert_domain:
	reset_control_assert(rx->domain_reset);
disable_clocks:
	clk_bulk_disable_unprepare(RTD1296_RX_VIDEO_CLOCKS, rx->clocks);
	return ret;
}

static int rx_runtime_suspend(struct device *dev)
{
	struct rtd1296_rx *rx = dev_get_drvdata(dev);

	rtd1296_rx_hw_stop(rx);
	reset_control_bulk_assert(RTD1296_RX_VIDEO_RESETS, rx->resets);
	reset_control_assert(rx->domain_reset);
	clk_bulk_disable_unprepare(RTD1296_RX_VIDEO_CLOCKS, rx->clocks);
	return 0;
}

static int rx_suspend(struct device *dev)
{
	struct rtd1296_rx *rx = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&rx->lock);
	rx->suspended = true;
	mutex_unlock(&rx->lock);
	cancel_delayed_work_sync(&rx->detect_work);
	mutex_lock(&rx->lock);
	rx_power_down(rx);
	mutex_unlock(&rx->lock);
	ret = pm_runtime_force_suspend(dev);
	if (ret) {
		mutex_lock(&rx->lock);
		rx->suspended = false;
		mutex_unlock(&rx->lock);
		queue_delayed_work(system_dfl_wq, &rx->detect_work, 0);
		return ret;
	}
	rx_detector_disable(rx);
	return 0;
}

static int rx_resume(struct device *dev)
{
	struct rtd1296_rx *rx = dev_get_drvdata(dev);
	int ret;

	ret = rx_detector_enable(rx);
	if (ret)
		return ret;
	ret = pm_runtime_force_resume(dev);
	if (ret) {
		rx_detector_disable(rx);
		return ret;
	}
	mutex_lock(&rx->lock);
	rx->suspended = false;
	mutex_unlock(&rx->lock);
	queue_delayed_work(system_dfl_wq, &rx->detect_work, 0);
	return 0;
}

static const struct dev_pm_ops rx_pm_ops = {
	RUNTIME_PM_OPS(rx_runtime_suspend, rx_runtime_resume, NULL)
	SYSTEM_SLEEP_PM_OPS(rx_suspend, rx_resume)
};

static void rx_release(struct v4l2_device *v4l2)
{
	struct rtd1296_rx *rx = container_of(v4l2, struct rtd1296_rx, v4l2);

	v4l2_ctrl_handler_free(&rx->controls);
	mutex_destroy(&rx->lock);
	kfree(rx);
}

static int rx_probe(struct platform_device *pdev)
{
	static const char * const clock_names[] = {
		"rx", "capture", "vo", "cbus-osc", "cbus", "cbus-tx", "cbus-rx",
	};
	static const char * const reset_names[] = {
		"rx", "wrapper", "capture", "cbus", "cbus-tx", "cbus-rx",
	};
	struct device *dev = &pdev->dev;
	struct rtd1296_rx *rx;
	struct vb2_queue *queue;
	unsigned int i;
	int ret;

	rx = kzalloc_obj(*rx);
	if (!rx)
		return -ENOMEM;
	rx->dev = dev;
	rx_select_mode(rx, &rtd1296_rx_modes[0]);
	platform_set_drvdata(pdev, rx);
	mutex_init(&rx->lock);
	spin_lock_init(&rx->queue_lock);
	INIT_LIST_HEAD(&rx->pending);
	INIT_DELAYED_WORK(&rx->detect_work, rx_detect_work);
	rx->mac = devm_platform_ioremap_resource_byname(pdev, "rx");
	if (IS_ERR(rx->mac)) {
		ret = PTR_ERR(rx->mac);
		goto free_rx;
	}
	rx->wrapper = devm_platform_ioremap_resource_byname(pdev, "wrapper");
	if (IS_ERR(rx->wrapper)) {
		ret = PTR_ERR(rx->wrapper);
		goto free_rx;
	}
	rx->ddc = devm_platform_ioremap_resource_byname(pdev, "ddc");
	if (IS_ERR(rx->ddc)) {
		ret = PTR_ERR(rx->ddc);
		goto free_rx;
	}
	rx->dma = devm_platform_ioremap_resource_byname(pdev, "capture");
	if (IS_ERR(rx->dma)) {
		ret = PTR_ERR(rx->dma);
		goto free_rx;
	}
	rx->cbus = devm_platform_ioremap_resource_byname(pdev, "cbus");
	if (IS_ERR(rx->cbus)) {
		ret = PTR_ERR(rx->cbus);
		goto free_rx;
	}
	rx->clocks = devm_kcalloc(dev, RTD1296_RX_CLOCKS,
				  sizeof(*rx->clocks), GFP_KERNEL);
	rx->resets = devm_kcalloc(dev, RTD1296_RX_RESETS,
				  sizeof(*rx->resets), GFP_KERNEL);
	if (!rx->clocks || !rx->resets) {
		ret = -ENOMEM;
		goto free_rx;
	}
	for (i = 0; i < RTD1296_RX_CLOCKS; i++)
		rx->clocks[i].id = clock_names[i];
	ret = devm_clk_bulk_get(dev, RTD1296_RX_CLOCKS, rx->clocks);
	if (ret)
		goto free_rx;
	for (i = 0; i < RTD1296_RX_RESETS; i++)
		rx->resets[i].id = reset_names[i];
	ret = devm_reset_control_bulk_get_exclusive(dev, RTD1296_RX_RESETS,
						    rx->resets);
	if (ret)
		goto free_rx;
	rx->domain_reset = devm_reset_control_get_shared(dev, "domain");
	if (IS_ERR(rx->domain_reset)) {
		ret = PTR_ERR(rx->domain_reset);
		goto free_rx;
	}
	ret = reset_control_bulk_assert(RTD1296_RX_RESETS, rx->resets);
	if (ret)
		goto free_rx;
	rx->hpd = devm_gpiod_get(dev, "hpd", GPIOD_OUT_LOW);
	if (IS_ERR(rx->hpd)) {
		ret = PTR_ERR(rx->hpd);
		goto free_rx;
	}
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(31));
	if (ret)
		goto free_rx;
	rx->irq = platform_get_irq(pdev, 0);
	if (rx->irq < 0) {
		ret = rx->irq;
		goto free_rx;
	}
	ret = devm_request_irq(dev, rx->irq, rx_irq, IRQF_SHARED, dev_name(dev), rx);
	if (ret)
		goto free_rx;
	ret = v4l2_device_register(dev, &rx->v4l2);
	if (ret)
		goto free_irq;
	rx->v4l2.release = rx_release;
	v4l2_ctrl_handler_init(&rx->controls, 1);
	rx->controls.lock = &rx->lock;
	rx->power_control = v4l2_ctrl_new_std(&rx->controls, NULL,
					      V4L2_CID_DV_RX_POWER_PRESENT, 0, 1, 0, 0);
	if (rx->controls.error) {
		ret = rx->controls.error;
		goto unregister_v4l2;
	}
	rx->v4l2.ctrl_handler = &rx->controls;
	queue = &rx->queue;
	queue->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	queue->io_modes = VB2_MMAP | VB2_DMABUF;
	queue->drv_priv = rx;
	queue->buf_struct_size = sizeof(struct rtd1296_rx_buffer);
	queue->ops = &rx_queue_ops;
	queue->mem_ops = &vb2_dma_contig_memops;
	queue->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	queue->min_queued_buffers = 3;
	queue->lock = &rx->lock;
	queue->dev = dev;
	ret = vb2_queue_init(queue);
	if (ret)
		goto unregister_v4l2;
	strscpy(rx->video.name, "rtd1296-hdmirx", sizeof(rx->video.name));
	rx->video.v4l2_dev = &rx->v4l2;
	rx->video.fops = &rx_file_ops;
	rx->video.ioctl_ops = &rx_ioctl_ops;
	rx->video.release = video_device_release_empty;
	rx->video.lock = &rx->lock;
	rx->video.queue = queue;
	rx->video.device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	rx->video.vfl_dir = VFL_DIR_RX;
	video_set_drvdata(&rx->video, rx);
	pm_runtime_set_suspended(dev);
	pm_runtime_enable(dev);
	ret = rx_detector_enable(rx);
	if (ret)
		goto disable_pm;
	ret = video_register_device(&rx->video, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto disable_detector;
	queue_delayed_work(system_dfl_wq, &rx->detect_work, 0);
	return 0;
disable_detector:
	rx_detector_disable(rx);
disable_pm:
	pm_runtime_disable(dev);
	vb2_queue_release(queue);
unregister_v4l2:
	devm_free_irq(dev, rx->irq, rx);
	v4l2_device_unregister(&rx->v4l2);
	v4l2_device_put(&rx->v4l2);
	return dev_err_probe(dev, ret, "receiver registration failed\n");
free_irq:
	devm_free_irq(dev, rx->irq, rx);
free_rx:
	mutex_destroy(&rx->lock);
	kfree(rx);
	return dev_err_probe(dev, ret, "receiver setup failed\n");
}

static void rx_remove(struct platform_device *pdev)
{
	struct rtd1296_rx *rx = platform_get_drvdata(pdev);

	mutex_lock(&rx->lock);
	rx->removing = true;
	mutex_unlock(&rx->lock);
	cancel_delayed_work_sync(&rx->detect_work);
	vb2_video_unregister_device(&rx->video);
	mutex_lock(&rx->lock);
	rx_power_down(rx);
	mutex_unlock(&rx->lock);
	pm_runtime_disable(rx->dev);
	if (!pm_runtime_status_suspended(rx->dev))
		rx_runtime_suspend(rx->dev);
	rx_detector_disable(rx);
	devm_free_irq(rx->dev, rx->irq, rx);
	v4l2_device_unregister(&rx->v4l2);
	v4l2_device_put(&rx->v4l2);
}

static const struct of_device_id rx_of_match[] = {
	{ .compatible = "realtek,rtd1296-hdmirx" },
	{ }
};
MODULE_DEVICE_TABLE(of, rx_of_match);

static struct platform_driver rx_driver = {
	.probe = rx_probe,
	.remove = rx_remove,
	.driver = {
		.name = "rtd1296-hdmirx",
		.of_match_table = rx_of_match,
		.pm = pm_ptr(&rx_pm_ops),
	},
};
module_platform_driver(rx_driver);

MODULE_DESCRIPTION("Realtek RTD1296 HDMI receiver");
MODULE_LICENSE("GPL");
