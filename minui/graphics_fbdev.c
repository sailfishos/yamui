/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>

#include <linux/fb.h>
#include <linux/kd.h>

#include <sys/mman.h>
#include <sys/cdefs.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#include "minui.h"
#include "graphics.h"
#include "../yamui-tools.h"

static gr_surface fbdev_init(minui_backend *, bool);
static gr_surface fbdev_flip(minui_backend *);
static void fbdev_blank(minui_backend *, bool);
static void fbdev_exit(minui_backend *);
static void fbdev_save(minui_backend *);
static void fbdev_restore(minui_backend *);

static GRSurface gr_framebuffer[2];
static bool double_buffered;
static GRSurface *gr_draw = NULL;
static int displayed_buffer;

static struct fb_var_screeninfo vi;
static int fb_fd = -1;

static minui_backend my_backend = {
	.init    = fbdev_init,
	.flip    = fbdev_flip,
	.blank   = fbdev_blank,
	.exit    = fbdev_exit,
	.save    = fbdev_save,
	.restore = fbdev_restore,
};

/* ------------------------------------------------------------------------ */

minui_backend *
open_fbdev(void)
{
	return &my_backend;
}

/* ------------------------------------------------------------------------ */

static void
fbdev_blank(minui_backend *backend UNUSED, bool blank)
{
	int ret;

	ret = ioctl(fb_fd, FBIOBLANK,
		    blank ? FB_BLANK_POWERDOWN : FB_BLANK_UNBLANK);
	if (ret < 0)
		perror("ioctl(): blank");
}

/* ------------------------------------------------------------------------ */

static void
set_displayed_framebuffer(unsigned n)
{
	if (n > 1 || !double_buffered)
		return;

	vi.yres_virtual = gr_framebuffer[0].height * 2;
	vi.yoffset = n * gr_framebuffer[0].height;
	vi.bits_per_pixel = gr_framebuffer[0].pixel_bytes * 8;
	if (ioctl(fb_fd, FBIOPUT_VSCREENINFO, &vi) < 0)
		perror("active fb swap failed");

	displayed_buffer = n;
}

/* ------------------------------------------------------------------------ */

static gr_surface
fbdev_init(minui_backend *backend, bool blank)
{
	int fd;
	void *bits;

	struct fb_fix_screeninfo fi;
	struct fb_var_screeninfo vi2;

	fd = open("/dev/graphics/fb0", O_RDWR);
	if (fd < 0) {
		fd = open("/dev/fb0", O_RDWR);
		if (fd < 0) {
			perror("cannot open fb0");
			return NULL;
		}
	}

	if (ioctl(fd, FBIOGET_FSCREENINFO, &fi) < 0) {
		perror("failed to get fb0 info");
		close(fd);
		return NULL;
	}

	if (ioctl(fd, FBIOGET_VSCREENINFO, &vi) < 0) {
		perror("failed to get fb0 info");
		close(fd);
		return NULL;
	}

	/* We print this out for informational purposes only, but
	 * throughout we assume that the framebuffer device uses an RGBX
	 * pixel format.  This is the case for every development device I
	 * have access to.  For some of those devices (eg, hammerhead aka
	 * Nexus 5), FBIOGET_VSCREENINFO *reports* that it wants a
	 * different format (XBGR) but actually produces the correct
	 * results on the display when you write RGBX.
	 *
	 * If you have a device that actually *needs* another pixel format
	 * (ie, BGRX, or 565), patches welcome... */

	printf("fb0 reports (possibly inaccurate):\n"
	       "  vi.bits_per_pixel = %d\n"
	       "  vi.colorspace = %d\n"
	       "  vi.grayscale = %d\n"
	       "  vi.nonstd = %d\n"
	       "  fi.type = %d\n"
	       "  fi.capabilities = %d\n"
	       "  vi.red.offset   = %3d   .length = %3d\n"
	       "  vi.green.offset = %3d   .length = %3d\n"
	       "  vi.blue.offset  = %3d   .length = %3d\n"
	       "  vi.alpha.offset = %3d   .length = %3d\n",
	       vi.bits_per_pixel, vi.colorspace, vi.grayscale, vi.nonstd,
	       fi.type, fi.capabilities, vi.red.offset, vi.red.length,
	       vi.green.offset, vi.green.length, vi.blue.offset,
	       vi.blue.length, vi.transp.offset, vi.transp.length);

	/* sometimes the framebuffer device needs to be told what
	 * we really expect it to be which is RGBA */
	ioctl(fd, FBIOGET_VSCREENINFO, &vi2);
	vi2.red.offset    = 0;
	vi2.red.length    = 8;
	vi2.green.offset  = 8;
	vi2.green.length  = 8;
	vi2.blue.offset   = 16;
	vi2.blue.length   = 8;
	vi2.transp.offset = 24;
	vi2.transp.length = 8;

	/* this might fail on some devices, without actually causing issues */
	if (ioctl(fd, FBIOPUT_VSCREENINFO, &vi2) < 0) {
		perror("failed to put fb0 info, restoring old one.");
		ioctl(fd, FBIOPUT_VSCREENINFO, &vi);
	}

	bits = mmap(0, fi.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		    0);
	if (bits == MAP_FAILED) {
		perror("failed to mmap framebuffer");
		close(fd);
		return NULL;
	}

	gr_framebuffer[0].width = vi.xres;
	gr_framebuffer[0].height = vi.yres;
	gr_framebuffer[0].row_bytes = fi.line_length;
	gr_framebuffer[0].pixel_bytes = vi.bits_per_pixel / 8;
	gr_framebuffer[0].data = bits;
	if (blank)
		memset(gr_framebuffer[0].data, 0,
		       gr_framebuffer[0].height *
		       gr_framebuffer[0].row_bytes);

	/* check if we can use double buffering */
	if (vi.yres * fi.line_length * 2 <= fi.smem_len) {
		double_buffered = true;

		memcpy(gr_framebuffer + 1, gr_framebuffer, sizeof(GRSurface));
		gr_framebuffer[1].data = gr_framebuffer[0].data +
					 gr_framebuffer[0].height *
					 gr_framebuffer[0].row_bytes;

		gr_draw = gr_framebuffer + 1;
	} else {
		double_buffered = false;

		/* Without double-buffering, we allocate RAM for a buffer to
		 * draw in, and then "flipping" the buffer consists of a
		 *memcpy from the buffer we allocated to the framebuffer. */

		/* TODO: Check for error */
		gr_draw = (GRSurface *)malloc(sizeof(GRSurface));
		memcpy(gr_draw, gr_framebuffer, sizeof(GRSurface));
		gr_draw->data = (unsigned char *)
				malloc(gr_draw->height * gr_draw->row_bytes);
		if (!gr_draw->data) {
			perror("failed to allocate in-memory surface");
			return NULL;
		}
	}

	if (blank || !double_buffered)
		memset(gr_draw->data, 0,
		       gr_draw->height * gr_draw->row_bytes);

	fb_fd = fd;
	set_displayed_framebuffer(0);

	printf("framebuffer: %d (%d x %d)\n", fb_fd, gr_draw->width,
	       gr_draw->height);

	if (blank) {
		fbdev_blank(backend, true);
		fbdev_blank(backend, false);
	}

	return gr_draw;
}

/* ------------------------------------------------------------------------ */

static gr_surface
fbdev_flip(minui_backend *backend UNUSED)
{
	/* the framebuffer does not always switch to the selected mode,
	 * so let's keep these work-arounds in mind */
#if defined(RECOVERY_BGRA)
	/* In case of BGRA, do some byte swapping */
	unsigned int idx;
	unsigned char tmp;
	unsigned char *ucfb_vaddr = (unsigned char *)gr_draw->data;

	for (idx = 0 ; idx < gr_draw->height * gr_draw->row_bytes; idx += 4) {
		tmp = ucfb_vaddr[idx];
		ucfb_vaddr[idx ] = ucfb_vaddr[idx + 2];
		ucfb_vaddr[idx + 2] = tmp;
	}
#endif /* defined(RECOVERY_BGRA) */

#if defined(RECOVERY_ARGB)
	/* In case of ARGB, do some byte swapping */
	unsigned int idx;
	unsigned char tmp;
	unsigned char *ucfb_vaddr = (unsigned char*)gr_draw->data;

	for (idx = 0 ; idx < (gr_draw->height * gr_draw->row_bytes); idx += 4) {
		tmp = ucfb_vaddr[idx];
		ucfb_vaddr[idx ] = ucfb_vaddr[idx + 1];
		ucfb_vaddr[idx + 1] = ucfb_vaddr[idx + 2];
		ucfb_vaddr[idx + 2] = ucfb_vaddr[idx + 3];
		ucfb_vaddr[idx + 3] = tmp;
	}
#endif /* defined(RECOVERY_ARGB) */

#if defined(RECOVERY_ALPHA)
	/* we sometimes really need to set an alpha channel */
	unsigned int idx;
	unsigned char *ucfb_vaddr = (unsigned char *)gr_draw->data;

	for (idx = 0 ; idx < gr_draw->height * gr_draw->row_bytes; idx += 4)
		ucfb_vaddr[idx + 3] = 0xff;

#endif /* defined(RECOVERY_ALPHA) */

	if (double_buffered) {
		/* Change gr_draw to point to the buffer currently displayed,
		 * then flip the driver so we're displaying the other buffer
		 * instead. */
		gr_draw = gr_framebuffer + displayed_buffer;
		set_displayed_framebuffer(1 - displayed_buffer);
	} else {
		/* Copy from the in-memory surface to the framebuffer. */
		memcpy(gr_framebuffer[0].data, gr_draw->data,
		       gr_draw->height * gr_draw->row_bytes);
	}

	return gr_draw;
}

/* ------------------------------------------------------------------------ */

static void
fbdev_exit(minui_backend *backend UNUSED)
{
	close(fb_fd);
	fb_fd = -1;

	if (!double_buffered && gr_draw) {
		free(gr_draw->data);
		free(gr_draw);
	}

	gr_draw = NULL;
}

/* ------------------------------------------------------------------------ */

static void *save_buf[2] = { NULL, NULL };

static void
fbdev_save(minui_backend *backend UNUSED)
{
	/* To prevent memory leak in a case when fbdev_save() was called
	 * several times without calling of fbdev_restore(). */
	if (save_buf[0])
		return;

	if (!(save_buf[0] = malloc(gr_draw->height * gr_draw->row_bytes))) {
		perror("Failed to allocate memory.");
		return;
	}

	memcpy(save_buf[0], gr_framebuffer[0].data,
	       gr_draw->height * gr_draw->row_bytes);

	if (double_buffered) {
		if (!(save_buf[1] =
			      malloc(gr_draw->height * gr_draw->row_bytes))) {
			perror("Failed to allocate memory.");
			/* Don't freeing save_buf[0] here since we have saved
			 * successfully it. */
			return;
		}

		memcpy(save_buf[1], gr_framebuffer[1].data,
		       gr_draw->height * gr_draw->row_bytes);
	}
}

/* ------------------------------------------------------------------------ */

static void
fbdev_restore(minui_backend *backend)
{
	fbdev_blank(backend, false);

	if (save_buf[0]) {
		memcpy(gr_framebuffer[0].data, save_buf[0],
		       gr_draw->height * gr_draw->row_bytes);
		free(save_buf[0]);
		save_buf[0] = NULL;
	}

	if (save_buf[1]) {
		memcpy(gr_framebuffer[1].data, save_buf[1],
		       gr_draw->height * gr_draw->row_bytes);
		free(save_buf[1]);
		save_buf[1] = NULL;
	}

	fbdev_flip(backend);
	if (double_buffered)
		fbdev_flip(backend);
}
