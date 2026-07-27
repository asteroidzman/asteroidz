#version 450

// scenefx soft-edge blur shader (fx_vk fork). Ports GLES shaders/tex_soft_edge.frag:
// renders an already-blurred offscreen texture (always plain 2D RGBA -- this is
// only used for wlr_scene_blur's own edge when blur->edge_softness > 0) with the
// SAME wide analytic gaussian falloff box_shadow.frag uses, instead of
// texture_round.frag's hard corner_alpha() SDF + ~1px AA band. A hard-edged
// rounded rect makes a uniform-strength blur look like an obviously rectangular
// "blurred patch" of whatever's behind it; fading the blur's own visibility
// over the same span the shadow tint fades over (same box, same blur_sigma)
// lets the two blend into one continuous soft halo with no seam anywhere.
//
// Two fx_vk-specific changes vs GLES tex_soft_edge.frag, matching
// texture_round.frag/box_shadow.frag's conventions:
//  - the coverage sample point comes from the box_pos varying (unit-quad coord,
//    layout top-left origin) instead of gl_FragCoord, which is in flipped
//    framebuffer space under the FLIPPED_180 projection (see common.vert);
//  - the source is already premultiplied (the blur effect image's own format)
//    and the output stays premultiplied, matching the fx_vk premultiplied-
//    blend pipeline -- a straight scalar multiply is enough, no un/re-premultiply
//    round trip like texture_round.frag's color-managed path needs.

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(location = 0) in vec2 uv;
// Box-relative unit coordinate from the vertex shader (see common.vert).
layout(location = 1) in vec2 box_pos;
layout(location = 0) out vec4 out_color;

// alpha shares fx_vk_frag_texture_pcr_data's offset (144) so callers can reuse
// the exact same push as the other texture shaders; matrix/luminance_multiplier
// are unused here (blur effect images need no color transform) and simply
// aren't declared. The corner block matches fx_vk_frag_corner_pcr_data at
// offset 160 (same slot texture_round.frag's corner data uses, ending at 224);
// blur_sigma is pushed right after at 224 (see FX_VK_TEX_SOFT_EDGE_SIGMA_OFFSET
// in pass.c, and the frag_corner_end budget bump in renderer.c's
// init_tex_layouts).
layout(push_constant) uniform UBO {
	layout(offset = 144) float alpha;
	layout(offset = 160) vec2 size;
	vec2 position;
	vec4 radius;       // tl, tr, bl, br -- the soft-edge box's own corner radii
	vec2 clip_size;
	vec2 clip_position;
	vec4 clip_radius;  // tl, tr, bl, br
	layout(offset = 224) float blur_sigma;
} data;

float gaussian(float x, float sigma) {
	const float pi = 3.141592653589793;
	return exp(-(x * x) / (2.0 * sigma * sigma)) / (sqrt(2.0 * pi) * sigma);
}

// approximates the error function, needed for the gaussian integral
vec2 erf(vec2 x) {
	vec2 s = sign(x), a = abs(x);
	x = 1.0 + (0.278393 + (0.230389 + 0.078108 * (a * a)) * a) * a;
	x *= x;
	return s - s / (x * x);
}

// identical to box_shadow.frag's roundedBoxShadowX/roundedBoxShadow: the
// coverage mask of a blurred rounded box, sampled at the current fragment
float roundedBoxShadowX(float x, float y, float sigma, float corner_l,
		float corner_r, vec2 halfSize) {
	float delta_l = min(halfSize.y - corner_l - abs(y), 0.0);
	float delta_r = min(halfSize.y - corner_r - abs(y), 0.0);
	float curved_l = halfSize.x - corner_l + sqrt(max(0.0, corner_l * corner_l - delta_l * delta_l));
	float curved_r = halfSize.x - corner_r + sqrt(max(0.0, corner_r * corner_r - delta_r * delta_r));
	vec2 integral = 0.5 + 0.5 * erf((x + vec2(-curved_l, curved_r)) * (sqrt(0.5) / sigma));
	return integral.y - integral.x;
}

float roundedBoxShadow(vec2 lower, vec2 upper, vec2 point, float sigma,
		float r_tl, float r_tr, float r_bl, float r_br) {
	vec2 center = (lower + upper) * 0.5;
	vec2 halfSize = (upper - lower) * 0.5;
	point -= center;

	float low = point.y - halfSize.y;
	float high = point.y + halfSize.y;
	float start = clamp(-3.0 * sigma, low, high);
	float end = clamp(3.0 * sigma, low, high);

	float step = (end - start) / 4.0;
	float y = start + step * 0.5;
	float value = 0.0;
	for (int i = 0; i < 4; i++) {
		float sy = point.y - y;
		// negative y is the top of the box (same orientation as texture_round's corner_alpha)
		float corner_l = sy < 0.0 ? r_tl : r_bl;
		float corner_r = sy < 0.0 ? r_tr : r_br;
		value += roundedBoxShadowX(point.x, sy, sigma, corner_l, corner_r, halfSize) * gaussian(y, sigma) * step;
		y += step;
	}

	return value;
}

float get_dist(vec2 q, float radius) {
	return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - radius;
}

// Interior-clip cutout, identical to texture_round.frag's corner_alpha.
float corner_alpha(vec2 relative_pos, vec2 size, bool is_cutout,
		float radius_tl, float radius_tr, float radius_bl, float radius_br) {
	vec2 top_left = abs(relative_pos - size) - size + radius_tl;
	vec2 top_right = abs(relative_pos - vec2(0, size.y)) - size + radius_tr;
	vec2 bottom_left = abs(relative_pos - vec2(size.x, 0)) - size + radius_bl;
	vec2 bottom_right = abs(relative_pos) - size + radius_br;

	float dist = max(
		max(get_dist(top_left, radius_tl), get_dist(top_right, radius_tr)),
		max(get_dist(bottom_left, radius_bl), get_dist(bottom_right, radius_br))
	);

	float aa = max(fwidth(dist), 1e-4);
	float result = smoothstep(0.0, aa, dist);
	float arc = is_cutout ? result : 1.0 - result;

	if (radius_tl <= 0.0 && radius_tr <= 0.0
			&& radius_bl <= 0.0 && radius_br <= 0.0) {
		return 1.0;
	}

	if (relative_pos.x < 0.0 || relative_pos.y < 0.0
			|| relative_pos.x > size.x || relative_pos.y > size.y) {
		return is_cutout ? 1.0 : 0.0;
	}

	bool is_top_left = radius_tl > 0.0
		&& relative_pos.x <= radius_tl && relative_pos.y <= radius_tl;
	bool is_top_right = radius_tr > 0.0
		&& relative_pos.x >= size.x - radius_tr && relative_pos.y <= radius_tr;
	bool is_bottom_left = radius_bl > 0.0
		&& relative_pos.x <= radius_bl && relative_pos.y >= size.y - radius_bl;
	bool is_bottom_right = radius_br > 0.0
		&& relative_pos.x >= size.x - radius_br && relative_pos.y >= size.y - radius_br;
	if (!is_top_left && !is_top_right && !is_bottom_left && !is_bottom_right) {
		return is_cutout ? 0.0 : 1.0;
	}

	return arc;
}

void main() {
	vec4 color = textureLod(tex, uv, 0);

	// Sample point in the soft-edge box's own layout space (flip-independent,
	// matches box_shadow.frag's box_pos usage).
	vec2 frag_layout = data.position + box_pos * data.size;

	// Same box + blur_sigma the shadow tint itself fades over: at
	// blur_sigma == 0 this degenerates to a hard rect (callers should not use
	// this shader path in that case -- see fx_vk_render_pass_add_blur).
	float coverage = roundedBoxShadow(
		data.position + data.blur_sigma,
		data.position + data.size - data.blur_sigma,
		frag_layout, data.blur_sigma * 0.5,
		data.radius.x, data.radius.y, data.radius.z, data.radius.w);

	float clip_corner_alpha = corner_alpha(
		frag_layout - (data.clip_position + 0.5),
		data.clip_size - 1.0,
		true,
		data.clip_radius.x, data.clip_radius.y,
		data.clip_radius.z, data.clip_radius.w);

	out_color = color * data.alpha * coverage * clip_corner_alpha;
}
