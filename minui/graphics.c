/*
 * Copyright (c) 2007 The Android Open Source Project
 * Copyright (c) 2014 - 2023 Jolla Ltd.
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

#include <time.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <errno.h>

#include <sys/mman.h>
#include <sys/types.h>
#include <sys/ioctl.h>

#include <linux/fb.h>
#include <linux/kd.h>

#include "font_10x18.h"
#include "minui.h"
#include "graphics.h"

typedef struct {
	GRSurface *texture;
	int cwidth;
	int cheight;
} GRFont;

static GRFont *gr_font = NULL;
static minui_backend *gr_backend = NULL;

static int overscan_percent  = OVERSCAN_PERCENT;
static int overscan_offset_x = 0;
static int overscan_offset_y = 0;

static int gr_vt_fd = -1;

static unsigned char gr_current_r = 255;
static unsigned char gr_current_g = 255;
static unsigned char gr_current_b = 255;
static unsigned char gr_current_a = 255;

static GRSurface *gr_draw = NULL;

/* ------------------------------------------------------------------------ */

static bool
outside(int x, int y)
{
	return x < 0 || x >= gr_draw->width || y < 0 || y >= gr_draw->height;
}

/* ------------------------------------------------------------------------ */

int gr_measure(const char *s)
{
    return gr_font->cwidth * strlen(s);
}

/* ------------------------------------------------------------------------ */

void gr_font_size(int *x, int *y)
{
    *x = gr_font->cwidth;
    *y = gr_font->cheight;
}

/* ------------------------------------------------------------------------ */

static void
text_blend(unsigned char *src_p, int src_row_bytes, unsigned char *dst_p,
	   int dst_row_bytes, int width, int height)
{
	int i, j;

	for (j = 0; j < height; j++) {
		unsigned char *sx = src_p, *px = dst_p;

		for (i = 0; i < width; i++) {
			unsigned char a = *sx++;

			if (gr_current_a < 255)
				a = (int)a * gr_current_a / 255;

			if (a == 255) {
				*px++ = gr_current_r;
				*px++ = gr_current_g;
				*px++ = gr_current_b;
				px++;
			} else if (a > 0) {
				int b = 255 - a;
				*px = (*px * b + gr_current_r * a) / 255;
				px += 1;
				*px = (*px * b + gr_current_g * a) / 255;
				px += 1;
				*px = (*px * b + gr_current_b * a) / 255;
				px += 2;
			} else {
				px += 4;
			}
		}
		src_p += src_row_bytes;
		dst_p += dst_row_bytes;
	}
}

/* ------------------------------------------------------------------------ */

void
gr_text(int x, int y, const char *s, int bold)
{
	GRFont *font = gr_font;
	unsigned chr;

	if (!font->texture)
		return;

	if (gr_current_a == 0)
		return;

	int has_bold = font->texture->height != font->cheight;
	bold = bold && has_bold;

	int fw = font->cwidth;
	int fh = font->cheight;
	int cx = 0;
	int cy = 0;
	int tab = fw * 8;
	int sx, sy;
	unsigned char *src_p, *dst_p;

	while ((chr = *s++)) {
		switch (chr) {
		case '\a': // bell
			bold = !bold && has_bold;
			break;
		case '\f': // formfeed
			break;
		case '\b': // backspace
			cx -= fw;
			break;
		case '\t': // horizontal tab
			cx += tab;
			cx -= cx % tab;
			break;
		case '\v': // vertical tab
			cy += fh;
			break;
		case '\r': // carriage ret
			cx = 0;
			break;
		case '\n': // new line
			cx =  0;
			cy += fh;
			break;
		default:
			if (chr < 32 || chr > 127)
				chr = 127;
			chr -= 32;
			sx = overscan_offset_x + x + cx;
			sy = overscan_offset_y + y + cy;
			if (!outside(sx, sy) &&
			    !outside(sx + fw - 1, sy + fh - 1)) {
				src_p = font->texture->data + chr * fw;
				if (bold)
					src_p += fh * font->texture->row_bytes;
				dst_p = gr_draw->data
					+ sy * gr_draw->row_bytes
					+ sx * gr_draw->pixel_bytes;
				text_blend(src_p, font->texture->row_bytes,
					   dst_p, gr_draw->row_bytes,
					   fw, fh);
			}
			cx += fw;
			break;
		}
	}
}

/* ------------------------------------------------------------------------ */

void
gr_texticon(int x, int y, GRSurface *icon)
{
	unsigned char *src_p, *dst_p;

	if (!icon)
		return;

	if (icon->pixel_bytes != 1) {
		printf("gr_texticon: source has wrong format\n");
		return;
	}

	x += overscan_offset_x;
	y += overscan_offset_y;

	if (outside(x, y) ||
	    outside(x + icon->width - 1, y + icon->height - 1))
		return;

	src_p = icon->data;
	dst_p = gr_draw->data + y * gr_draw->row_bytes +
				x * gr_draw->pixel_bytes;

	text_blend(src_p, icon->row_bytes, dst_p, gr_draw->row_bytes,
		   icon->width, icon->height);
}

/* ------------------------------------------------------------------------ */

void
gr_color(unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
	gr_current_r = r;
	gr_current_g = g;
	gr_current_b = b;
	gr_current_a = a;
}

/* ------------------------------------------------------------------------ */

void
gr_clear(void)
{
	if (gr_current_r == gr_current_g && gr_current_r == gr_current_b)
		memset(gr_draw->data, gr_current_r,
		       gr_draw->height * gr_draw->row_bytes);
	else {
		int x, y;
		unsigned char *px = gr_draw->data;

		for (y = 0; y < gr_draw->height; y++) {
			for (x = 0; x < gr_draw->width; x++) {
				*px++ = gr_current_r;
				*px++ = gr_current_g;
				*px++ = gr_current_b;
				px++;
			}

			px += gr_draw->row_bytes -
			      (gr_draw->width * gr_draw->pixel_bytes);
		}
	}
}

/* ------------------------------------------------------------------------ */

void
gr_fill(int x1, int y1, int x2, int y2)
{
	unsigned char *p;

	x1 += overscan_offset_x;
	y1 += overscan_offset_y;

	x2 += overscan_offset_x;
	y2 += overscan_offset_y;

	if (outside(x1, y1) || outside(x2 - 1, y2 - 1))
		return;

	p = gr_draw->data + y1 * gr_draw->row_bytes +
	    x1 * gr_draw->pixel_bytes;

	if (gr_current_a == 255) {
		int x, y;

		for (y = y1; y < y2; y++) {
			unsigned char *px = p;

			for (x = x1; x < x2; x++) {
				*px++ = gr_current_r;
				*px++ = gr_current_g;
				*px++ = gr_current_b;
				px++;
			}

			p += gr_draw->row_bytes;
		}
	} else if (gr_current_a > 0) {
		int x, y;

		for (y = y1; y < y2; y++) {
			unsigned char *px = p;

			for (x = x1; x < x2; x++) {
				*px = (*px * (255 - gr_current_a) +
				       gr_current_r * gr_current_a) / 255;
				px++;
				*px = (*px * (255 - gr_current_a) +
				       gr_current_g * gr_current_a) / 255;
				px++;
				*px = (*px * (255 - gr_current_a) +
				       gr_current_b * gr_current_a) / 255;
				px++;
				px++;
			}

			p += gr_draw->row_bytes;
		}
	}
}

/* ------------------------------------------------------------------------ */

void
gr_blit(GRSurface *source, int sx, int sy, int w, int h, int dx, int dy)
{
	int i;
	unsigned char *src_p, *dst_p;

	if (!source)
		return;

	if (gr_draw->pixel_bytes != source->pixel_bytes) {
		printf("gr_blit: source has wrong format\n");
		return;
	}

	dx += overscan_offset_x;
	dy += overscan_offset_y;

	if (dx < 0) sx -= dx, w += dx, dx = 0;
	if (dy < 0) sy -= dy, h += dy, dy = 0;
	if (dx + w > gr_draw->width) w = gr_draw->width - dx;
	if (dy + h > gr_draw->height) h = gr_draw->height - dy;
	if (w <= 0 || h <= 0)
		return;

	src_p = source->data + sy * source->row_bytes +
			       sx * source->pixel_bytes;
	dst_p = gr_draw->data + dy * gr_draw->row_bytes +
				dx * gr_draw->pixel_bytes;

	for (i = 0; i < h; i++) {
		memcpy(dst_p, src_p, w * source->pixel_bytes);
		src_p += source->row_bytes;
		dst_p += gr_draw->row_bytes;
	}
}

/* ------------------------------------------------------------------------ */

unsigned int
gr_get_width(GRSurface *surface)
{
	if (!surface)
		return 0;

	return surface->width;
}

/* ------------------------------------------------------------------------ */

unsigned int
gr_get_height(GRSurface *surface)
{
	if (!surface)
		return 0;

	return surface->height;
}

/* ------------------------------------------------------------------------ */

static void
gr_init_font(void)
{
	int res;
	static const char font_path[] = "/res/images/font.png";

	/* TODO: Check for error */
	gr_font = calloc(sizeof(*gr_font), 1);

	bool font_loaded = false;

	if (access(font_path, F_OK) == -1 && errno == ENOENT) {
		/* Not having a font file is normal, no need
		 * to complain. */
	}
	else if (!(res = res_create_alpha_surface(font_path, NULL, &gr_font->texture))) {
		/* The font image should be a 96x2 array of character images.
		 * The columns are the printable ASCII characters 0x20 - 0x7f.
		 * The top row is regular text; the bottom row is bold. */
		gr_font->cwidth = gr_font->texture->width / 96;
		gr_font->cheight = gr_font->texture->height / 2;
		font_loaded = true;
	}
	else {
		printf("%s: failed to read font: res=%d\n", font_path, res);
	}

	if (!font_loaded) {
		unsigned char *bits, data, *in = font.rundata;


		/* fall back to the compiled-in font. */
		/* TODO: Check for error */
		gr_font->texture = malloc(sizeof(*gr_font->texture));
		gr_font->texture->width = font.width;
		gr_font->texture->height = font.height;
		gr_font->texture->row_bytes = font.width;
		gr_font->texture->pixel_bytes = 1;

		/* TODO: Check for error */
		bits = malloc(font.width * font.height);
		gr_font->texture->data = (void *)bits;

		while ((data = *in++)) {
			memset(bits, (data & 0x80) ? 255 : 0, data & 0x7f);
			bits += (data & 0x7f);
		}

		gr_font->cwidth = font.cwidth;
		gr_font->cheight = font.cheight;
	}
}

/* ------------------------------------------------------------------------ */

void
gr_flip(void)
{
	gr_draw = gr_backend->flip(gr_backend);
}

/* ------------------------------------------------------------------------ */

static int gr_init_fbdev(bool blank)
{
	gr_backend = open_fbdev();
	gr_draw = gr_backend->init(gr_backend, blank);
	if (gr_draw)
		gr_flip();
	if (gr_draw)
		gr_flip();
	if (!gr_draw)
		gr_backend->exit(gr_backend);
	return gr_draw ? 0 : -1;
}

static int gr_init_drm(bool blank)
{
	gr_backend = open_drm();

	/* At least in Xperia 10: the first display open succeeds
	 * without any trace of problems, but nothing is actually
	 * drawn on screen - make sure we get past that...
	 */
	gr_backend->init(gr_backend, blank);
	gr_backend->exit(gr_backend);

	/* Assume that failures can happen due to there being
	 * another process that is trying to release display
	 * and allow some slack for that to finish.
	 */
	for (int failures = 0;;) {
		gr_draw = gr_backend->init(gr_backend, blank);
		if (gr_draw)
			gr_flip();
		if (gr_draw)
			gr_flip();
		if (gr_draw)
			break;
		gr_backend->exit(gr_backend);
		if (++failures >= 5)
			break;
		struct timespec ts = { 0, 100 * 1000 * 1000 };
		nanosleep(&ts, NULL);
	}
	return gr_draw ? 0 : -1;
}
int
gr_init(bool blank)
{
	gr_init_font();

	if ((gr_vt_fd = open("/dev/tty0", O_RDWR | O_SYNC)) < 0) {
		/* This is non-fatal; post-Cupcake kernels don't have tty0. */
		perror("can't open /dev/tty0");
	} else if (ioctl(gr_vt_fd, KDSETMODE, (void *)KD_GRAPHICS)) {
		/* However, if we do open tty0, we expect the ioctl
		 * to work. */
		perror("failed KDSETMODE to KD_GRAPHICS on tty0");
		gr_exit();
		return -1;
	}

	if (gr_init_fbdev(blank) != 0 && gr_init_drm(blank) != 0)
		return -1;

	overscan_offset_x = gr_draw->width  * overscan_percent / 100;
	overscan_offset_y = gr_draw->height * overscan_percent / 100;

	return 0;
}

/* ------------------------------------------------------------------------ */

void
gr_exit(void)
{
	if (gr_backend) {
		gr_backend->exit(gr_backend);
		gr_backend = NULL;
	}

	if (gr_vt_fd != -1) {
		ioctl(gr_vt_fd, KDSETMODE, (void *)KD_TEXT);
		close(gr_vt_fd);
		gr_vt_fd = -1;
	}
}

/* ------------------------------------------------------------------------ */

int
gr_fb_width(void)
{
	return gr_draw->width - 2 * overscan_offset_x;
}

/* ------------------------------------------------------------------------ */

int
gr_fb_height(void)
{
	return gr_draw->height - 2 * overscan_offset_y;
}

/* ------------------------------------------------------------------------ */

void
gr_fb_blank(bool blank)
{
	gr_backend->blank(gr_backend, blank);
}

/* ------------------------------------------------------------------------ */

/* Save screen content to internal buffer. */
void
gr_save(void)
{
	if (gr_backend->save)
		gr_backend->save(gr_backend);
}

/* ------------------------------------------------------------------------ */

/* Restore screen content from internal buffer. */
void
gr_restore(void)
{
	if (gr_backend->restore)
		gr_backend->restore(gr_backend);
}
