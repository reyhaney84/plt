/*
 * i.MX IPUv3 overlay driver
 *
 * Copyright (C) 2011 Sascha Hauer, Pengutronix
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <media/v4l2-dev.h>
#include <asm/poll.h>
#include <asm/io.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-contig.h>
#include <video/imx-ipu-v3.h>
#include <video/imx-ipu-image-convert.h>
#include "../../../gpu/ipu-v3/ipu-prv.h"
#include <drm/imx-ipu-v3-vout.h>

#include <media/v4l2-dev.h>

#include "imx-ipu.h"

static int usealpha;

module_param(usealpha, int, 0);

struct vout_buffer {
	struct vb2_buffer		vb;
};

#define vb2q_to_vout(q)	container_of(q, struct vout_data, vidq)

enum {
	VOUT_IDLE,
	VOUT_STARTING,
	VOUT_RUNNING,
	VOUT_STOPPING,
};

struct vout_queue {
	struct ipu_image	image;
	void			*virt;
	dma_addr_t		phys;
	size_t			size;
	struct list_head	list;
	struct vb2_buffer	*vb;
	struct vout_data	*vout;
};

#define NUMBUFS	3
#define IMX_IPU_OVL_NAME "i.MX v4l2 ovl"

struct vout_data {
	struct v4l2_device	v4l2_dev;
	struct video_device	*video_dev;

	int			status;

	struct ipu_soc		*ipu;
	struct ipuv3_channel	*ipu_ch, *ipu_ch_bg;
	struct dmfc_channel	*dmfc;
	struct ipu_dp		*dp;

	struct vb2_queue	vidq;

	struct device	*alloc_ctx;
	spinlock_t		lock;
	struct device		*dev;

	int			irq;

	struct ipu_image	out_image; /* output image */
	struct ipu_image	in_image; /* input image */
	struct v4l2_window	win; /* user selected output window (after scaler) */
	struct v4l2_rect	crop; /* cropping rectangle in the input image */

	struct list_head	idle_list;
	struct list_head	scale_list;
	struct list_head	show_list;

	struct vout_queue	*showing, *next_showing;
	int			width_base;
	int			height_base;

	int			opened;
	int			dma;
};

/*
 * This function is a major hack. We can't leave the base framebuffer
 * with the overlay. To make this sure we read directly from cpmem
 * of the base layer. We rather need some notification mechanism
 * for this.
 */
static void ipu_ovl_get_base_resolution(struct vout_data *vout)
{
	struct ipuv3_channel *ipu_ch = vout->ipu_ch_bg;

	vout->width_base = ipu_ch_param_read_field(ipu_ch, IPU_FIELD_FW) + 1;
	vout->height_base = ipu_ch_param_read_field(ipu_ch, IPU_FIELD_FH) + 1;
}

static int ipu_ovl_sanitize(struct vout_data *vout)
{
	struct ipu_image *in = &vout->in_image;
	struct ipu_image *out = &vout->out_image;
	struct v4l2_rect *crop = &vout->crop;
	struct v4l2_window *win = &vout->win;

	ipu_ovl_get_base_resolution(vout);

	if (vout->width_base == 1 || vout->height_base == 1) {
		dev_err(vout->dev, "get base resolution failed.");
		return -EINVAL;
	}

	/* Do not allow to leave base framebuffer for now */
	if (win->w.left < 0)
		win->w.left = 0;
	if (win->w.top  < 0)
		win->w.top = 0;
	if (win->w.left + win->w.width > vout->width_base)
		win->w.left = vout->width_base - win->w.width;
	if (win->w.top + win->w.height > vout->height_base)
		win->w.top = vout->height_base - win->w.height;
	if (win->w.left < 0)
		win->w.left = 0;
	if (win->w.top  < 0)
		win->w.top = 0;

	dev_dbg(vout->dev, "start: win:  %dx%d@%dx%d crop: %dx%d@%dx%d\n",
			win->w.width, win->w.height, win->w.left, win->w.top,
			crop->width, crop->height, crop->left, crop->top);

	in->rect.left = crop->left;
	in->rect.top = crop->top;
	in->rect.height = crop->height;
	in->rect.width = crop->width;

	out->pix.width = crop->width;
	out->pix.height = crop->height;
	out->rect.width = crop->width;
	out->rect.height = crop->height;
	out->rect.left = crop->left;
	out->rect.top = crop->top;

	dev_dbg(vout->dev, "result in: %dx%d crop: %dx%d@%dx%d\n",
			in->pix.width, in->pix.height, in->rect.width,
			in->rect.height, in->rect.left, in->rect.top);
	dev_dbg(vout->dev, "result out: %dx%d crop: %dx%d@%dx%d\n",
			out->pix.width, out->pix.height, out->rect.width,
			out->rect.height, out->rect.left, out->rect.top);

	return 0;
}

static int vidioc_querycap(struct file *file, void  *priv,
		struct v4l2_capability *cap)
{
	strlcpy(cap->driver, IMX_IPU_OVL_NAME, sizeof(cap->driver));
	strlcpy(cap->bus_info, "platform: imx-ipu-ovl", sizeof(cap->bus_info));
	strlcpy(cap->card, "imx-ipu-ovl: (XX)", sizeof(cap->card));

	cap->device_caps = V4L2_CAP_VIDEO_OUTPUT | V4L2_CAP_STREAMING |
		V4L2_CAP_VIDEO_OUTPUT_OVERLAY;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;

	return 0;
}

static int vidioc_cropcap(struct file *file, void *priv,
		struct v4l2_cropcap *cropcap)
{
	struct video_device *dev = video_devdata(file);
	struct vout_data *vout = video_get_drvdata(dev);
	struct v4l2_pix_format *pix = &vout->in_image.pix;

	if (cropcap->type != V4L2_BUF_TYPE_VIDEO_OUTPUT &&
		cropcap->type != V4L2_BUF_TYPE_VIDEO_OUTPUT_OVERLAY)
		return -EINVAL;

	cropcap->bounds.left = 0;
	cropcap->bounds.top = 0;
	cropcap->bounds.width = pix->width;
	cropcap->bounds.height = pix->height;
	cropcap->defrect.left = 0;
	cropcap->defrect.top = 0;
	cropcap->defrect.width = pix->width;
	cropcap->defrect.height = pix->height;
	cropcap->pixelaspect.numerator = 1;
	cropcap->pixelaspect.denominator = 1;

	return 0;
}

static int ipu_ovl_vidioc_g_fmt_vid_out(struct file *file, void *fh,
					struct v4l2_format *f)
{
	struct video_device *dev = video_devdata(file);
	struct vout_data *vout = video_get_drvdata(dev);

	if (f->type != V4L2_BUF_TYPE_VIDEO_OUTPUT &&
		f->type != V4L2_BUF_TYPE_VIDEO_OUTPUT_OVERLAY)
		return -EINVAL;

	return ipu_g_fmt(f, &vout->out_image.pix);
}

static int ipu_ovl_vidioc_try_fmt_vid_out(struct file *file, void *fh,
					struct v4l2_format *f)
{
	return ipu_try_fmt(file, fh, f);
}

static int ipu_ovl_vidioc_s_fmt_vid_out(struct file *file, void *fh,
					struct v4l2_format *f)
{
	struct video_device *dev = video_devdata(file);
	struct vout_data *vout = video_get_drvdata(dev);
	struct v4l2_pix_format *pix = &vout->in_image.pix;
	int ret;

	ret = ipu_s_fmt(file, fh, f, pix);
	if (ret)
		return ret;

	vout->win.w.left = 0;
	vout->win.w.top = 0;
	vout->win.w.width = pix->width;
	vout->win.w.height = pix->height;

	vout->crop.left = 0;
	vout->crop.top = 0;
	vout->crop.width = pix->width;
	vout->crop.height = pix->height;

	return ipu_ovl_sanitize(vout);
}

static int ipu_ovl_vidioc_g_fmt_vid_out_overlay(struct file *file, void *fh,
					struct v4l2_format *f)
{
	struct video_device *dev = video_devdata(file);
	struct vout_data *vout = video_get_drvdata(dev);

	f->fmt.win = vout->win;

	return 0;
}

static int ipu_ovl_vidioc_try_fmt_vid_overlay(struct file *file, void *fh,
					struct v4l2_format *f)
{
	struct v4l2_window *win = &f->fmt.win;

	win->w.width &= ~0x3;
	win->w.height &= ~0x1;

	return 0;
}

static int ipu_ovl_vidioc_s_fmt_vid_out_overlay(struct file *file, void *fh,
					struct v4l2_format *f)
{
	struct video_device *dev = video_devdata(file);
	struct vout_data *vout = video_get_drvdata(dev);
	struct v4l2_window *win = &f->fmt.win;
	int ret = 0;

	win->w.width &= ~0x3;
	win->w.height &= ~0x1;

	vout->win.w.width = win->w.width;
	vout->win.w.height = win->w.height;
	vout->win.w.left = win->w.left;
	vout->win.w.top = win->w.top;

	ret = ipu_ovl_sanitize(vout);
	if (!ret) {
		win->w.width = vout->win.w.width;
		win->w.height = vout->win.w.height;
		win->w.left = vout->win.w.left;
		win->w.top = vout->win.w.top;
	}

	return ret;
}

static int ipu_ovl_vidioc_g_crop(struct file *file, void *fh,
				 struct v4l2_crop *crop)
{
	struct video_device *dev = video_devdata(file);
	struct vout_data *vout = video_get_drvdata(dev);

	crop->c = vout->crop;

	return 0;
}

static int ipu_ovl_vidioc_s_crop(struct file *file, void *fh,
				 const struct v4l2_crop *crop)
{
	struct video_device *dev = video_devdata(file);
	struct vout_data *vout = video_get_drvdata(dev);

	vout->crop = crop->c;

	return ipu_ovl_sanitize(vout);
}

static int ipu_ovl_vidioc_s_fbuf(struct file *file, void *fh,
				const struct v4l2_framebuffer *a)
{
	/* TODO */
	return 0;
}

static int ipu_ovl_vidioc_g_fbuf(struct file *file, void *fh,
		struct v4l2_framebuffer *a)
{

	/* TODO */
	return 0;
}

static int vout_videobuf_setup(struct vb2_queue *vq,
		unsigned int *count, unsigned int *num_planes,
		unsigned int sizes[], struct device *alloc_ctxs[])
{
	struct vout_data *vout = vb2q_to_vout(vq);
	struct ipu_image *image = &vout->in_image;

	*num_planes = 1;
	sizes[0] = image->pix.sizeimage;
	alloc_ctxs[0] = vout->alloc_ctx;

	if (!*count)
		*count = 32;

	ipu_dp_set_global_alpha(vout->dp, usealpha ? 0 : 1, 0, 1);

	return 0;
}

static int vout_videobuf_prepare(struct vb2_buffer *vb)
{
	struct vout_data *vout = vb2q_to_vout(vb->vb2_queue);
	struct v4l2_pix_format *pix = &vout->in_image.pix;

	vb2_set_plane_payload(vb, 0, pix->bytesperline * pix->height);

	return 0;
}

static irqreturn_t vout_handler(int irq, void *context)
{
	struct vout_data *vout = context;
	unsigned long flags;
	struct vout_queue *q;
	int current_active = ipu_idmac_get_current_buffer(vout->ipu_ch);

	spin_lock_irqsave(&vout->lock, flags);

	if(vout->status == VOUT_IDLE)
		goto out;

	if (vout->showing) {
		/*
		 * this could have already happened on EOF, but we
		 * want to avoid another 60 interrupts per second.
		 */
		vb2_buffer_done(vout->showing->vb, VB2_BUF_STATE_DONE);
		list_add_tail(&vout->showing->list, &vout->idle_list);
		vout->showing = NULL;
	}

	if (vout->status == VOUT_STOPPING) {
		list_splice_tail_init(&vout->show_list, &vout->idle_list);
		spin_unlock_irqrestore(&vout->lock, flags);
		ipu_dp_disable_channel(vout->dp);
		ipu_idmac_wait_busy(vout->ipu_ch, 100);
		ipu_idmac_disable_channel(vout->ipu_ch);
		ipu_dmfc_disable_channel(vout->dmfc);
		vout->status = VOUT_IDLE;
		return IRQ_HANDLED;
	}

	if (list_is_singular(&vout->show_list))
		goto out;

	q = list_first_entry(&vout->show_list, struct vout_queue, list);

	/* remember currently displayed buffer separately */
	list_del(&q->list);
	vout->showing = q;

	q = list_first_entry(&vout->show_list, struct vout_queue, list);

	current_active = ipu_idmac_get_current_buffer(vout->ipu_ch);
	ipu_cpmem_set_buffer(vout->ipu_ch, !current_active, q->image.phys0 +
			q->image.rect.left + q->image.pix.width * q->image.rect.top);

	ipu_idmac_select_buffer(vout->ipu_ch, !current_active);
out:
	spin_unlock_irqrestore(&vout->lock, flags);

	return IRQ_HANDLED;
}

static int vout_enable(struct vout_queue *q)
{
	struct vout_data *vout = q->vout;
	struct ipu_image *image = &q->image;
	int ret;

	ret = ipu_cpmem_set_image(vout->ipu_ch, image);
	if (ret) {
		dev_err(vout->dev, "setup cpmem failed with %d\n", ret);
		return ret;
	}
	ipu_idmac_set_double_buffer(vout->ipu_ch, 1);

	ipu_cpmem_set_buffer(vout->ipu_ch, 0, image->phys0 + image->rect.left + image->pix.width * image->rect.top);
	ipu_cpmem_set_buffer(vout->ipu_ch, 1, image->phys0 + image->rect.left + image->pix.width * image->rect.top);
	ipu_idmac_select_buffer(vout->ipu_ch, 0);
	ipu_idmac_select_buffer(vout->ipu_ch, 1);

	ipu_cpmem_set_high_priority(vout->ipu_ch);

	ipu_dmfc_config_wait4eot(vout->dmfc, image->pix.width);

	ipu_dmfc_enable_channel(vout->dmfc);
	ipu_idmac_enable_channel(vout->ipu_ch);
	ipu_dp_setup_channel(vout->dp,
			ipu_pixelformat_to_colorspace(image->pix.pixelformat),
			IPUV3_COLORSPACE_RGB);
	ipu_dp_set_window_pos(vout->dp, vout->win.w.left, vout->win.w.top);
	ipu_dp_enable_channel(vout->dp);

	return 0;
}

static void vout_scaler_complete(struct ipu_image_convert_run *run, void *context)
{
	struct vout_queue *q = context;
	struct vout_data *vout = q->vout;
	unsigned long flags;

	spin_lock_irqsave(&vout->lock, flags);

	if (vout->status == VOUT_STARTING || vout->status == VOUT_RUNNING)
		list_move_tail(&q->list, &vout->show_list);
	else
		list_move_tail(&q->list, &vout->idle_list);

	if (vout->status == VOUT_STARTING) {
		vout_enable(q);
		vout->status = VOUT_RUNNING;
	}

	spin_unlock_irqrestore(&vout->lock, flags);
}

static void vout_videobuf_queue(struct vb2_buffer *vb)
{
	struct vout_data *vout = vb2q_to_vout(vb->vb2_queue);
	unsigned long flags;
	struct ipu_image *image;
	struct vout_queue *q;
	int scale = 1;

	spin_lock_irqsave(&vout->lock, flags);

	if (list_empty(&vout->idle_list)) {
		vb2_buffer_done(vb, VB2_BUF_STATE_DONE);
		spin_unlock_irqrestore(&vout->lock, flags);
		return;
	}

	q = list_first_entry(&vout->idle_list, struct vout_queue, list);

	if (vout->in_image.rect.width == vout->out_image.rect.width &&
			vout->in_image.rect.height == vout->out_image.rect.height) {
		int i = 0;
		struct list_head *pos;
		scale = 0;
		list_for_each(pos, &vout->show_list)
			i++;
		if (!list_empty(&vout->show_list) && !list_is_singular(&vout->show_list)) {
			vb2_buffer_done(vb, VB2_BUF_STATE_DONE);
			goto out;
		}
		list_move_tail(&q->list, &vout->show_list);
	} else {
		list_move_tail(&q->list, &vout->scale_list);
	}

	q->vb = vb;

	image = &q->image;

	if (scale) {
		image->pix = vout->out_image.pix;
		image->rect = vout->out_image.rect;
		image->phys0 = q->phys;

		image->pix.pixelformat = V4L2_PIX_FMT_UYVY;
		image->pix.bytesperline = image->pix.width * 2;

		vout->in_image.phys0 = vb2_dma_contig_plane_dma_addr(vb, 0);

		/*FIXME: this shoud be investigated or removed */
		ipu_image_convert(vout->ipu,
				IC_TASK_POST_PROCESSOR,
				&vout->in_image,
				image,
				IPU_ROTATE_90_RIGHT,
				vout_scaler_complete,
				q);
	} else {
		image->pix = vout->in_image.pix;
		image->rect = vout->in_image.rect;
		image->phys0 = vb2_dma_contig_plane_dma_addr(vb, 0);
	}
out:
	spin_unlock_irqrestore(&vout->lock, flags);
}

static void vout_videobuf_release(struct vb2_buffer *vb)
{
}

static int vout_videobuf_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct vout_data *vout = vb2q_to_vout(vq);
	struct vout_queue *q;
	unsigned long flags;
	int ret;

	if (vout->status != VOUT_IDLE)
		return -EINVAL;

	vout->irq = ipu_idmac_channel_irq(vout->ipu, vout->ipu_ch, IPU_IRQ_NFACK);
	ret = request_threaded_irq(vout->irq, NULL, vout_handler, IRQF_ONESHOT,
			"imx-ipu-ovl", vout);
	if (ret)
		return ret;

	spin_lock_irqsave(&vout->lock, flags);

	vout->status = VOUT_STARTING;

	if (!list_empty(&vout->show_list)) {
		q = list_first_entry(&vout->show_list, struct vout_queue, list);
		spin_unlock_irqrestore(&vout->lock, flags);
		vout_enable(q);
		vout->status = VOUT_RUNNING;
	} else {
		spin_unlock_irqrestore(&vout->lock, flags);
	}

	return 0;
}

static void vout_videobuf_stop_streaming(struct vb2_queue *vq)
{
	struct vout_data *vout = vb2q_to_vout(vq);
	unsigned long flags;
	struct vout_queue *q, *tmp;

	if (vout->status == VOUT_IDLE)
		return;

	spin_lock_irqsave(&vout->lock, flags);

	vout->status = VOUT_STOPPING;

	while (!list_empty(&vout->scale_list) || !list_empty(&vout->show_list)) {
		spin_unlock_irqrestore(&vout->lock, flags);
		schedule();
		spin_lock_irqsave(&vout->lock, flags);
	}

	vout->status = VOUT_IDLE;

	list_for_each_entry_safe(q, tmp, &vout->idle_list, list)
		if (q->vb && q->vb->state == VB2_BUF_STATE_ACTIVE)
			vb2_buffer_done(q->vb, VB2_BUF_STATE_ERROR);

	spin_unlock_irqrestore(&vout->lock, flags);

	free_irq(vout->irq, vout);

	return;
}

static int vout_videobuf_init(struct vb2_buffer *vb)
{
	return 0;
}

static struct vb2_ops vout_videobuf_ops = {
	.queue_setup		= vout_videobuf_setup,
	.buf_prepare		= vout_videobuf_prepare,
	.buf_queue		= vout_videobuf_queue,
	.buf_cleanup		= vout_videobuf_release,
	.buf_init		= vout_videobuf_init,
	.start_streaming	= vout_videobuf_start_streaming,
	.stop_streaming		= vout_videobuf_stop_streaming,
#if 0
	/* FIXME: do we need these? */
	.wait_prepare		= vout_videobuf_unlock,
	.wait_finish		= vout_videobuf_lock,
#endif
};

static int ipu_ovl_vidioc_reqbufs(struct file *file, void *priv,
			struct v4l2_requestbuffers *reqbuf)
{
	struct video_device *dev = video_devdata(file);
	struct vout_data *vout = video_get_drvdata(dev);
	int ret;

	ret = vb2_reqbufs(&vout->vidq, reqbuf);

	return ret;
}

static int ipu_ovl_vidioc_querybuf(struct file *file, void *fh, struct v4l2_buffer *buf)
{
	struct video_device *dev = video_devdata(file);
	struct vout_data *vout = video_get_drvdata(dev);

	return vb2_querybuf(&vout->vidq, buf);
}

static int ipu_ovl_vidioc_qbuf(struct file *file, void *fh, struct v4l2_buffer *buf)
{
	struct video_device *dev = video_devdata(file);
	struct vout_data *vout = video_get_drvdata(dev);

	return vb2_qbuf(&vout->vidq, buf);
}

static int ipu_ovl_vidioc_expbuf(struct file *file, void *fh, struct v4l2_exportbuffer *eb)
{
	struct video_device *dev = video_devdata(file);
	struct vout_data *vout = video_get_drvdata(dev);

	return vb2_expbuf(&vout->vidq, eb);
}

static int ipu_ovl_vidioc_dqbuf(struct file *file, void *fh, struct v4l2_buffer *buf)
{
	struct video_device *dev = video_devdata(file);
	struct vout_data *vout = video_get_drvdata(dev);

	return vb2_dqbuf(&vout->vidq, buf, file->f_flags & O_NONBLOCK);
}

static int ipu_ovl_vidioc_create_bufs(struct file*file, void *fh,
				      struct v4l2_create_buffers *create)
{
	struct video_device *dev = video_devdata(file);
	struct vout_data *vout = video_get_drvdata(dev);

	return vb2_create_bufs(&vout->vidq, create);
}

static int ipu_ovl_vidioc_streamon(struct file *file, void *fh, enum v4l2_buf_type i)
{
	struct video_device *dev = video_devdata(file);
	struct vout_data *vout = video_get_drvdata(dev);

	return vb2_streamon(&vout->vidq, i);
}

static int ipu_ovl_vidioc_streamoff(struct file *file, void *fh, enum v4l2_buf_type i)
{
	struct video_device *dev = video_devdata(file);
	struct vout_data *vout = video_get_drvdata(dev);

	return vb2_streamoff(&vout->vidq, i);
}

static int ipu_ovl_vidioc_enum_fmt_vid_out(struct file *file, void *fh,
		struct v4l2_fmtdesc *f)
{
	return ipu_enum_fmt(file, fh, f);
}

static int mxc_v4l2out_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct video_device *dev = video_devdata(file);
	struct vout_data *vout = video_get_drvdata(dev);

	return vb2_mmap(&vout->vidq, vma);
}

static int ipu_ovl_vidioc_g_output(struct file *file, void *priv, unsigned *o)
{
	*o = 0;
	return 0;
}

static int ipu_ovl_vidioc_g_ctrl(struct file *file, void *fh, struct v4l2_control *ctrl)
{

	return 0;
}

static int mxc_v4l2out_open(struct file *file)
{
	struct video_device *dev = video_devdata(file);
	struct vout_data *vout = video_get_drvdata(dev);
	struct vb2_queue *q = &vout->vidq;
	int ret;
	int i;

	if (vout->opened)
		return -EBUSY;

	vout->opened++;

	INIT_LIST_HEAD(&vout->idle_list);
	INIT_LIST_HEAD(&vout->scale_list);
	INIT_LIST_HEAD(&vout->show_list);

	ipu_ovl_get_base_resolution(vout);

	for (i = 0; i < NUMBUFS; i++) {
		struct vout_queue *q;

		q = kzalloc(sizeof (*q), GFP_KERNEL);
		if (!q)
			ret = -ENOMEM;
		q->size = vout->width_base * vout->height_base * 2;
		q->virt = dma_alloc_coherent(NULL, q->size, &q->phys,
					       GFP_DMA | GFP_KERNEL);
		q->vout = vout;
		BUG_ON(!q->virt);

		list_add_tail(&q->list, &vout->idle_list);
	}

	q->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	q->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF;
	q->drv_priv = vout;
	q->ops = &vout_videobuf_ops;
	q->mem_ops = &vb2_dma_contig_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->buf_struct_size = sizeof(struct vb2_buffer);

	return vb2_queue_init(q);
}

static int mxc_v4l2out_close(struct file *file)
{
	struct video_device *dev = video_devdata(file);
	struct vout_data *vout = video_get_drvdata(dev);
	struct vout_queue *q, *tmp;

	vb2_queue_release(&vout->vidq);

	list_for_each_entry_safe(q, tmp, &vout->idle_list, list) {
		dma_free_coherent(NULL, q->size, q->virt, q->phys);
		kfree(q);
	}

	vout->opened--;

	return 0;
}

static const struct v4l2_ioctl_ops mxc_ioctl_ops = {
	.vidioc_querycap		= vidioc_querycap,
	.vidioc_cropcap			= vidioc_cropcap,

	.vidioc_enum_fmt_vid_out	= ipu_ovl_vidioc_enum_fmt_vid_out,
	.vidioc_g_fmt_vid_out		= ipu_ovl_vidioc_g_fmt_vid_out,
	.vidioc_s_fmt_vid_out		= ipu_ovl_vidioc_s_fmt_vid_out,
	.vidioc_try_fmt_vid_out		= ipu_ovl_vidioc_try_fmt_vid_out,

	.vidioc_enum_fmt_vid_overlay	= ipu_ovl_vidioc_enum_fmt_vid_out,
	.vidioc_g_fmt_vid_out_overlay	= ipu_ovl_vidioc_g_fmt_vid_out_overlay,
	.vidioc_s_fmt_vid_out_overlay	= ipu_ovl_vidioc_s_fmt_vid_out_overlay,
	.vidioc_try_fmt_vid_overlay	= ipu_ovl_vidioc_try_fmt_vid_overlay,

	.vidioc_s_crop			= ipu_ovl_vidioc_s_crop,
	.vidioc_g_crop			= ipu_ovl_vidioc_g_crop,

	.vidioc_reqbufs			= ipu_ovl_vidioc_reqbufs,
	.vidioc_querybuf		= ipu_ovl_vidioc_querybuf,
	.vidioc_qbuf			= ipu_ovl_vidioc_qbuf,
	.vidioc_expbuf			= ipu_ovl_vidioc_expbuf,
	.vidioc_dqbuf			= ipu_ovl_vidioc_dqbuf,
	.vidioc_create_bufs		= ipu_ovl_vidioc_create_bufs,
	.vidioc_streamon		= ipu_ovl_vidioc_streamon,
	.vidioc_streamoff		= ipu_ovl_vidioc_streamoff,

	.vidioc_g_output		= ipu_ovl_vidioc_g_output,

	.vidioc_s_fbuf			= ipu_ovl_vidioc_s_fbuf,
	.vidioc_g_fbuf			= ipu_ovl_vidioc_g_fbuf,

	.vidioc_g_ctrl			= ipu_ovl_vidioc_g_ctrl,

#ifdef CONFIG_VIDEO_V4L1_COMPAT
	.vidiocgmbuf			= vidiocgmbuf,
#endif
};

static struct v4l2_file_operations mxc_v4l2out_fops = {
	.owner		= THIS_MODULE,
	.open		= mxc_v4l2out_open,
	.release	= mxc_v4l2out_close,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= mxc_v4l2out_mmap,
};

static u64 vout_dmamask = ~(u32)0;

static int mxc_v4l2out_probe(struct platform_device *pdev)
{
	struct ipu_ovl_pdata *pdata = pdev->dev.platform_data;
	struct vout_data *vout;
	int ret;

	if (!pdata)
		return -EINVAL;

	pdev->dev.dma_mask = &vout_dmamask;
	pdev->dev.coherent_dma_mask = 0xffffffff;

	vout = kzalloc(sizeof(struct vout_data), GFP_KERNEL);
	if (!vout)
		return -ENOMEM;

	vout->ipu = pdata->ipu;

	ret = v4l2_device_register(&pdev->dev, &vout->v4l2_dev);
	if (ret)
		goto failed_v4l2_dev_register;

	vout->video_dev = video_device_alloc();
	if (!vout->video_dev) {
		ret = -ENOMEM;
		goto failed_vdev_alloc;
	}

	vout->alloc_ctx = &pdev->dev;
	vout->dma = pdata->dma[0];
	/* get main flow channel number */
	vout->ipu_ch = pdata->ipu_ch;
	vout->ipu_ch_bg = pdata->ipu_ch_bg;
	vout->dp = pdata->dp;
	vout->dmfc = pdata->dmfc;

	vout->video_dev->minor = -1;

	strcpy(vout->video_dev->name, "imx-ipuv3-ovl");
	vout->video_dev->fops = &mxc_v4l2out_fops;
	vout->video_dev->ioctl_ops = &mxc_ioctl_ops;
	vout->video_dev->release = video_device_release;
	vout->video_dev->vfl_dir = VFL_DIR_TX;
	vout->video_dev->v4l2_dev = &vout->v4l2_dev;

	spin_lock_init(&vout->lock);
	vout->dev = &pdev->dev;

	ret = video_register_device(vout->video_dev, VFL_TYPE_GRABBER, -1);
	if (ret) {
		dev_err(&pdev->dev, "register failed with %d\n", ret);
		goto failed_register;
	}

	platform_set_drvdata(pdev, vout);
	video_set_drvdata(vout->video_dev, vout);

	return 0;

failed_register:
	kfree(vout->video_dev);
failed_vdev_alloc:
	v4l2_device_unregister(&vout->v4l2_dev);
failed_v4l2_dev_register:
	kfree(vout);
	return ret;

	return 0;
}

static int mxc_v4l2out_remove(struct platform_device *pdev)
{
	struct vout_data *vout = platform_get_drvdata(pdev);

	video_unregister_device(vout->video_dev);
	v4l2_device_unregister(&vout->v4l2_dev);

	kfree(vout);

	return 0;
}

static struct platform_driver mxc_v4l2out_driver = {
	.driver = {
		   .name = "imx-ipuv3-ovl",
	},
	.probe = mxc_v4l2out_probe,
	.remove = mxc_v4l2out_remove,
};

static int mxc_v4l2out_init(void)
{
	return platform_driver_register(&mxc_v4l2out_driver);
}

static void mxc_v4l2out_exit(void)
{
	platform_driver_unregister(&mxc_v4l2out_driver);
}

module_init(mxc_v4l2out_init);
module_exit(mxc_v4l2out_exit);

MODULE_AUTHOR("Sascha Hauer <s.hauer@pengutronix.de>");
MODULE_DESCRIPTION("V4L2-driver for MXC video output");
MODULE_LICENSE("GPL");
MODULE_SUPPORTED_DEVICE("video");
