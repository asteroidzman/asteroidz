uniform mat3 proj;
uniform vec4 color;
uniform mat3 tex_proj;
// Second texture matrix, used only by the masked blur-composite shaders
// (tex.frag's MASK path) to sample the transparency mask through its own
// wl_output-transform-aware matrix, exactly like the primary texture. Left at
// the default (zero) matrix for every other program, whose fragment shader
// never reads v_texcoord2, so the varying and this uniform are eliminated.
uniform mat3 tex_proj2;
attribute vec2 pos;
varying vec4 v_color;
varying vec2 v_texcoord;
varying vec2 v_texcoord2;

void main() {
	vec3 pos3 = vec3(pos, 1.0);
	gl_Position = vec4(pos3 * proj, 1.0);
	v_color = color;
	v_texcoord = (pos3 * tex_proj).xy;
	v_texcoord2 = (pos3 * tex_proj2).xy;
}
