#ifndef ASTEROIDZ_COMMON_CORNER_LOCATION_H
#define ASTEROIDZ_COMMON_CORNER_LOCATION_H

#include <scenefx/types/fx/clipped_region.h>

/* scenefx 0.5 removed enum corner_location in favor of struct
 * fx_corner_radii. Asteroidz still tracks which corners of a client should be
 * rounded as a bitmask, so keep the old enum locally and convert at the
 * scenefx API boundary. */
enum corner_location {
	CORNER_LOCATION_NONE = 0,
	CORNER_LOCATION_TOP_LEFT = 1 << 0,
	CORNER_LOCATION_TOP_RIGHT = 1 << 1,
	CORNER_LOCATION_BOTTOM_RIGHT = 1 << 2,
	CORNER_LOCATION_BOTTOM_LEFT = 1 << 3,
	CORNER_LOCATION_TOP = CORNER_LOCATION_TOP_LEFT | CORNER_LOCATION_TOP_RIGHT,
	CORNER_LOCATION_BOTTOM =
		CORNER_LOCATION_BOTTOM_LEFT | CORNER_LOCATION_BOTTOM_RIGHT,
	CORNER_LOCATION_LEFT =
		CORNER_LOCATION_TOP_LEFT | CORNER_LOCATION_BOTTOM_LEFT,
	CORNER_LOCATION_RIGHT =
		CORNER_LOCATION_TOP_RIGHT | CORNER_LOCATION_BOTTOM_RIGHT,
	CORNER_LOCATION_ALL = CORNER_LOCATION_TOP_LEFT | CORNER_LOCATION_TOP_RIGHT |
		CORNER_LOCATION_BOTTOM_LEFT | CORNER_LOCATION_BOTTOM_RIGHT,
};

static inline struct fx_corner_radii
corner_radii_from_location(int radius, enum corner_location location) {
	uint16_t r = corner_radius_clamp(radius);
	return (struct fx_corner_radii){
		.top_left = (location & CORNER_LOCATION_TOP_LEFT) ? r : 0,
		.top_right = (location & CORNER_LOCATION_TOP_RIGHT) ? r : 0,
		.bottom_right = (location & CORNER_LOCATION_BOTTOM_RIGHT) ? r : 0,
		.bottom_left = (location & CORNER_LOCATION_BOTTOM_LEFT) ? r : 0,
	};
}

#endif
