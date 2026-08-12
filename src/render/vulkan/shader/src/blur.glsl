/*
 * Dual-Kawase blur: the push-constant overlay, and the two kernels.
 *
 * WHY DUAL-KAWASE AND NOT A SEPARABLE GAUSSIAN. Both were considered and the
 * choice is about where the work goes, not about which is "better maths".
 *
 * A separable Gaussian of radius R costs 2 passes at FULL resolution and
 * ~2*(2R+1) samples per output pixel. At the radius a compositor backdrop wants
 * -- tens of pixels -- that is hundreds of full-resolution samples, and the
 * cost grows linearly in R.
 *
 * Dual-Kawase gets its radius from RESOLUTION instead of from taps. Each
 * downsample halves the image and each upsample doubles it, with a fixed 5- and
 * 8-tap kernel; the effective support doubles per level, so radius grows
 * EXPONENTIALLY in pass count while cost falls geometrically -- level n costs a
 * quarter of level n-1. Two down and two up passes is 4 draws whose combined
 * fragment count is about 1.7x one full-resolution pass, for a support a
 * full-resolution Gaussian would need ~30 taps to match.
 *
 * It is also what the reference does (blur1.frag / blur2.frag), which matters
 * for a different reason than parity: asteroidz's blur config -- `passes`,
 * `radius` -- is expressed in dual-Kawase terms, and a Gaussian would have to
 * reinterpret both, changing every existing user's desktop.
 *
 * The tradeoff, stated: dual-Kawase is an approximation of a Gaussian, not one.
 * Its impulse response is slightly boxier and it can show faint ringing on a
 * single bright pixel. That is measured in test_blur_impulse() rather than
 * asserted away.
 *
 * THE PUSH-CONSTANT OVERLAY. A blur draw uses no colour, no corner radii and no
 * inner box, so those slots carry the kernel and the post-effects instead. See
 * push.glsl for the full table; this names the readings at the point of use.
 */

/* Sampling step, in SOURCE texture units. The reference calls this
 * `halfpixel * radius`; it is folded on the CPU so the shader does one
 * multiply-free add per tap. */
#define AZ_BLUR_STEP    pc.color.xy

/*
 * The LAST VALID texel centre in the source, in UV.
 *
 * The transient pool rounds an allocation up to a 64 px granularity, so a
 * 32x32 blur level lives in a 64x64 image and three quarters of that image was
 * never written. Sampling the full [0,1] range therefore averages in unwritten
 * memory -- which on a 128x128 fixture cost a factor of four of energy and made
 * levels 2 and 3 produce nothing at all.
 *
 * CLAMP_TO_EDGE does not help: it clamps to the ALLOCATION edge, which is the
 * padding. The valid bound has to be carried explicitly and applied per tap.
 * This is exactly the "logical valid extent separately from the allocation
 * extent" the M4F brief asks for, and the reason it is asked for.
 */
#define AZ_BLUR_UV_MAX  pc.color.zw

/* One tap, clamped into the valid region. */
vec4 az_blur_tap(sampler2D tex, vec2 uv) {
	return texture(tex, clamp(uv, vec2(0.0), AZ_BLUR_UV_MAX));
}

/* Post-effects, folded into the LAST upsample rather than run as a separate
 * full-screen pass -- which is what the reference's blur2.frag does and the
 * reason it exists at all. */
#define AZ_BLUR_BRIGHTNESS  pc.inner_box.x
#define AZ_BLUR_CONTRAST    pc.inner_box.y
#define AZ_BLUR_SATURATION  pc.inner_box.z
#define AZ_BLUR_NOISE       pc.inner_box.w
#define AZ_BLUR_EFFECTS     pc.inner_corners.x

/*
 * Oklab chroma scaling for saturation.
 *
 * Not the legacy RGB saturation matrix: that shifts hue and distorts luminance
 * as chroma grows past 1, which on a wallpaper with saturated regions reads as
 * a colour cast rather than as more colour. Requires linear input, hence the
 * round trip below.
 */
vec3 az_linear_to_oklab(vec3 c) {
	float l = dot(vec3(0.4122214708, 0.5363325363, 0.0514459929), c);
	float m = dot(vec3(0.2119034982, 0.6806995451, 0.1073969566), c);
	float s = dot(vec3(0.0883024619, 0.2817188376, 0.6299787005), c);
	vec3 lms = pow(max(vec3(l, m, s), vec3(0.0)), vec3(1.0 / 3.0));
	return vec3(
		dot(vec3(0.2104542553, 0.7936177850, -0.0040720468), lms),
		dot(vec3(1.9779984951, -2.4285922050, 0.4505937099), lms),
		dot(vec3(0.0259040371, 0.7827717662, -0.8086757660), lms));
}

vec3 az_oklab_to_linear(vec3 lab) {
	vec3 lms = vec3(
		lab.x + 0.3963377774 * lab.y + 0.2158037573 * lab.z,
		lab.x - 0.1055613458 * lab.y - 0.0638541728 * lab.z,
		lab.x - 0.0894841775 * lab.y - 1.2914855480 * lab.z);
	lms = lms * lms * lms;
	return vec3(
		dot(vec3(4.0767416621, -3.3077115913, 0.2309699292), lms),
		dot(vec3(-1.2684380046, 2.6097574011, -0.3413193965), lms),
		dot(vec3(-0.0041960863, -0.7034186147, 1.7076147010), lms));
}

float az_blur_noise(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * 1689.1984);
	p3 += dot(p3, p3.yzx + 33.33);
	return (fract((p3.x + p3.y) * p3.z) - 0.5) * AZ_BLUR_NOISE;
}

/*
 * Brightness, contrast, saturation and noise.
 *
 * EVERY FORM HERE KEEPS BLACK AT BLACK, and that is not cosmetic: contrast is a
 * power law about the 0.5 pivot rather than a lerp toward grey, brightness is a
 * pure gain rather than an offset, and the noise is a RATIO rather than an
 * additive term. An absolute offset survives into an HDR/PQ output as a lifted
 * floor, which reads as a grey glow where the blur should be black -- M5 makes
 * that path live, so the shapes are chosen now rather than retrofitted.
 */
vec4 az_blur_effects(vec4 color, vec2 uv) {
	vec3 rgb = max(color.rgb, vec3(0.0));

	if (AZ_BLUR_SATURATION != 1.0) {
		/* The blur buffer holds gamma-encoded values today, so linearise
		 * around the Oklab step. M5 composites scene-linear and this round
		 * trip becomes the identity -- the reason it is written as an explicit
		 * pair rather than folded into the matrices. */
		vec3 linear = pow(rgb, vec3(2.2));
		vec3 lab = az_linear_to_oklab(linear);
		lab.yz *= AZ_BLUR_SATURATION;
		rgb = pow(max(az_oklab_to_linear(lab), vec3(0.0)), vec3(1.0 / 2.2));
	}
	if (AZ_BLUR_CONTRAST != 1.0) {
		rgb = 0.5 * pow(rgb * 2.0, vec3(AZ_BLUR_CONTRAST));
	}
	rgb *= AZ_BLUR_BRIGHTNESS * (1.0 + az_blur_noise(uv));
	return vec4(rgb, color.a);
}
