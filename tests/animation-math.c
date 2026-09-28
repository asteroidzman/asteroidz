#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define ASTEROIDZ_MIN(a, b) ((a) < (b) ? (a) : (b))
struct wlr_box { int32_t x, y, width, height; };
#define SHATTER_MATH_ONLY
#include "animation/shatter.h"

static double cross(double ax, double ay, double bx, double by) {
	return ax * by - ay * bx;
}

int main(void) {
	const struct wlr_box sizes[] = {
		{0, 0, 800, 600}, {-1920, -200, 1920, 1080},
		{700, 40, 321, 951}, {0, 0, 1, 1}, {0, 0, 3840, 2160},
	};
	for (int n = 2; n <= 12; n++) {
		int count = shatter_frag_count(n);
		struct ShatterFrag shards[336];
		assert(count > 0 && count <= 336);
		for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
			const struct wlr_box *win = &sizes[s];
			double area = 0;
			for (int i = 0; i < count; i++) {
				struct ShatterFrag *f = &shards[i];
				shatter_frag_init(f, i, n, win);
				double a = cross(f->local[2] - f->local[0], f->local[3] - f->local[1],
					f->local[4] - f->local[0], f->local[5] - f->local[1]) * 0.5;
				assert(a > 0.0);
				area += a;
				float cor[8];
				shatter_corners_at(f, 0.0, ASTEROIDZ_MIN(win->width, win->height), cor);
				for (int k = 0; k < 3; k++) {
					assert(f->uv[k * 2] >= 0 && f->uv[k * 2] <= 1);
					assert(f->uv[k * 2 + 1] >= 0 && f->uv[k * 2 + 1] <= 1);
					assert(fabs(cor[k * 2] - (win->x + f->uv[k * 2] * win->width)) < 0.001);
					assert(fabs(cor[k * 2 + 1] - (win->y + f->uv[k * 2 + 1] * win->height)) < 0.001);
				}
				/* Sample out of order as mixed-refresh outputs would. Geometry
				 * must depend on time, never on the number/order of ticks. */
				float first[8], again[8];
				shatter_corners_at(f, 0.37, win->height, first);
				for (int t = 100; t >= 0; t--) {
					shatter_corners_at(f, t / 100.0, win->height, cor);
					for (int k = 0; k < 8; k++) assert(isfinite(cor[k]));
					assert(cor[4] == cor[6] && cor[5] == cor[7]);
					assert(isfinite(shatter_light_at(f, t / 100.0)));
				}
				shatter_corners_at(f, 0.37, win->height, again);
				for (int k = 0; k < 8; k++) assert(first[k] == again[k]);
			}
			assert(fabs(area - (double)win->width * win->height) < area * 1e-12);
		}
		/* Area alone cannot rule out a hole paired with an overlap. Each
		 * interior probe must lie in exactly one triangle of the source mesh. */
		for (int j = 0; j < 257; j++) {
			double x = shatter_jitter((uint32_t)j + 9001u, 0.001, 0.999);
			double y = shatter_jitter((uint32_t)j + 811u, 0.001, 0.999);
			int hits = 0;
			for (int i = 0; i < count; i++) {
				float *uv = shards[i].uv;
				bool inside = true;
				for (int k = 0; k < 3; k++) {
					int next = (k + 1) % 3;
					inside &= cross(uv[next * 2] - uv[k * 2], uv[next * 2 + 1] - uv[k * 2 + 1],
						x - uv[k * 2], y - uv[k * 2 + 1]) >= 0;
				}
				hits += inside;
			}
			assert(hits == 1);
		}
	}
	puts("Glass mesh: coverage, UV continuity, bounds and time-independent motion passed");
	return 0;
}
