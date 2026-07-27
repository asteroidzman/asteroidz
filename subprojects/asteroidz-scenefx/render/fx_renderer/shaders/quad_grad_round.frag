#define LEN %d

#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

varying vec4 v_color;
varying vec2 v_texcoord;

uniform vec2 size;
uniform vec2 position;

uniform float radius_top_left;
uniform float radius_top_right;
uniform float radius_bottom_left;
uniform float radius_bottom_right;

uniform vec2 clip_position;
uniform vec2 clip_size;
uniform float clip_radius_top_left;
uniform float clip_radius_top_right;
uniform float clip_radius_bottom_left;
uniform float clip_radius_bottom_right;

uniform vec4 colors[LEN];
uniform vec2 grad_size;
uniform float degree;
uniform vec2 grad_box;
uniform vec2 origin;
uniform bool linear;
uniform bool blend;
uniform int count;

vec4 gradient(vec4 colors[LEN], int count, vec2 size, vec2 grad_box, vec2 origin, float degree, bool linear, bool blend);

float corner_alpha(vec2 size, vec2 position, bool is_cutout,
		float radius_tl, float radius_tr, float radius_bl, float radius_br);

// TODO:
void main() {
	float quad_corner_alpha = corner_alpha(
		size - 1.0,
		position + 0.5,
		false,
		radius_top_left,
		radius_top_right,
		radius_bottom_left,
		radius_bottom_right
	);
	// Clipping (e.g. cutting the window interior out of a border rect)
	float clip_corner_alpha = corner_alpha(
		clip_size - 1.0,
		clip_position + 0.5,
		true,
		clip_radius_top_left,
		clip_radius_top_right,
		clip_radius_bottom_left,
		clip_radius_bottom_right
	);
	float rect_alpha = v_color.a * quad_corner_alpha * clip_corner_alpha;

	gl_FragColor = mix(vec4(0.0), gradient(colors, count, size, grad_box, origin, degree, linear, blend), rect_alpha);
}
