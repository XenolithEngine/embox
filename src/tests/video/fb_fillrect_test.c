/**
 * @file
 * @brief fb_fillrect() on the board's framebuffer paints the colour asked for
 *
 * pixel_to_pat() masked the colour with ~(-1 << bpp). At 32 bpp that shift
 * is undefined, and AArch64 takes a register shift amount modulo 32, so the
 * mask came out 0 and every fill on a 32 bpp framebuffer was black -- found
 * on a Raspberry Pi 4 as a status panel whose colour bars were all invisible.
 *
 * A corner of framebuffer 0 is saved, filled, checked and put back. A board
 * without a framebuffer passes with a line that says so.
 */

#include <stdint.h>
#include <string.h>

#include <drivers/video/fb.h>
#include <embox/test.h>
#include <kernel/printk.h>

EMBOX_TEST_SUITE("fb_fillrect");

#define W 16
#define H 4

static uint32_t saved[H][W];

static uint32_t *fb_px(struct fb_info *fb, unsigned x, unsigned y) {
	size_t pitch = fb->var.xres * (fb->var.bits_per_pixel / 8);

	return (uint32_t *)((char *)fb->screen_base + y * pitch) + x;
}

TEST_CASE("a fill at 32 bpp is the colour asked for, not black") {
	struct fb_info *fb = fb_lookup(0);
	struct fb_fillrect r = {
		.dx = 1, .dy = 1, .width = W - 2, .height = H - 2,
		.color = 0x00c0ffee, .rop = ROP_COPY,
	};
	unsigned x, y;
	int wrong = 0;

	if (fb == NULL || fb->var.bits_per_pixel != 32 || fb->var.xres < W
	    || fb->var.yres < H) {
		printk("fb_fillrect: no 32 bpp framebuffer 0 here; nothing to check\n");
		return;
	}

	for (y = 0; y < H; y++) {
		memcpy(saved[y], fb_px(fb, 0, y), sizeof(saved[y]));
	}

	fb_fillrect(fb, &r);

	for (y = 0; y < H; y++) {
		for (x = 0; x < W; x++) {
			int inside = x >= 1 && x < W - 1 && y >= 1 && y < H - 1;
			uint32_t want = inside ? 0x00c0ffee : saved[y][x];

			if (*fb_px(fb, x, y) != want) {
				if (!wrong) {
					printk("fb_fillrect: (%u,%u) is %#x, want %#x\n", x, y,
					    *fb_px(fb, x, y), want);
				}
				wrong++;
			}
		}
	}

	for (y = 0; y < H; y++) {
		memcpy(fb_px(fb, 0, y), saved[y], sizeof(saved[y]));
	}

	test_assert_zero(wrong);
}
