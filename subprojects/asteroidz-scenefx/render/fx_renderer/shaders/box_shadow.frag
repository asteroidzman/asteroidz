// Writeup: https://madebyevan.com/shaders/fast-rounded-rectangle-shadows/

#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

varying vec4 v_color;
varying vec2 v_texcoord;

uniform vec2 position;
uniform vec2 size;
uniform float blur_sigma;
uniform float corner_radius_top_left;
uniform float corner_radius_top_right;
uniform float corner_radius_bottom_left;
uniform float corner_radius_bottom_right;
uniform vec2 clip_position;
uniform vec2 clip_size;
uniform float clip_radius_top_left;
uniform float clip_radius_top_right;
uniform float clip_radius_bottom_left;
uniform float clip_radius_bottom_right;

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

// return the blurred mask along the x dimension, with independent corner
// radii for the left and right edge of the current scanline
float roundedBoxShadowX(float x, float y, float sigma, float corner_l,
        float corner_r, vec2 halfSize) {
    float delta_l = min(halfSize.y - corner_l - abs(y), 0.0);
    float delta_r = min(halfSize.y - corner_r - abs(y), 0.0);
    float curved_l = halfSize.x - corner_l + sqrt(max(0.0, corner_l * corner_l - delta_l * delta_l));
    float curved_r = halfSize.x - corner_r + sqrt(max(0.0, corner_r * corner_r - delta_r * delta_r));
    vec2 integral = 0.5 + 0.5 * erf((x + vec2(-curved_l, curved_r)) * (sqrt(0.5) / sigma));
    return integral.y - integral.x;
}

// return the mask for the shadow of a box from lower to upper
float roundedBoxShadow(vec2 lower, vec2 upper, vec2 point, float sigma,
        float r_tl, float r_tr, float r_bl, float r_br) {
    // Center everything to make the math easier
    vec2 center = (lower + upper) * 0.5;
    vec2 halfSize = (upper - lower) * 0.5;
    point -= center;

    // The signal is only non-zero in a limited range, so don't waste samples
    float low = point.y - halfSize.y;
    float high = point.y + halfSize.y;
    float start = clamp(-3.0 * sigma, low, high);
    float end = clamp(3.0 * sigma, low, high);

    // Accumulate samples (we can get away with surprisingly few samples)
    float step = (end - start) / 4.0;
    float y = start + step * 0.5;
    float value = 0.0;
    for (int i = 0; i < 4; i++) {
        float sy = point.y - y;
        // negative y is the top of the box (same orientation as corner_alpha)
        float corner_l = sy < 0.0 ? r_tl : r_bl;
        float corner_r = sy < 0.0 ? r_tr : r_br;
        value += roundedBoxShadowX(point.x, sy, sigma, corner_l, corner_r, halfSize) * gaussian(y, sigma) * step;
        y += step;
    }

    return value;
}

float corner_alpha(vec2 size, vec2 position, bool is_cutout,
        float radius_tl, float radius_tr, float radius_bl, float radius_br);

void main() {
    float shadow_alpha = v_color.a * roundedBoxShadow(
            position + blur_sigma,
            position + size - blur_sigma,
            gl_FragCoord.xy, blur_sigma * 0.5,
            corner_radius_top_left,
            corner_radius_top_right,
            corner_radius_bottom_left,
            corner_radius_bottom_right);

    // Clipping
    float clip_corner_alpha = corner_alpha(
        clip_size - 1.5,
        clip_position + 0.75,
        true,
        clip_radius_top_left,
        clip_radius_top_right,
        clip_radius_bottom_left,
        clip_radius_bottom_right
    );

    gl_FragColor = vec4(v_color.rgb, shadow_alpha) * clip_corner_alpha;
}
