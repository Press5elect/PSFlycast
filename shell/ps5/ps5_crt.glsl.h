/*
	PSFlyCast - the game's picture as a tube shows it: "Scanlines" and "CRT".

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	One fragment shader, drawn by ps5_fsr.cpp in place of the emulator's
	stretch: the text of a string literal, compiled at run time as GLSL with
	CRT_TUBE defined 0 (scanlines alone: one texture read a pixel at 480
	lines, three above) or 1 (and the mask, the glow and the vignette: eight
	reads more).

	Written for this port. It uses ideas anyone who has read about CRT shaders
	knows (a beam that is brightest along the middle of its line, a mask of
	red, green and blue stripes, blending in linear light, a smoothstep on the
	fraction of a bilinear read) and no code from crt-royale, crt-lottes,
	crt-guest or any other shader.

	What is particular to it:
	- The lines are the game's (240 or 480 of them over the picture's height),
	  whatever the picture was rendered at and however large it is drawn.
	- Their profile is a cosine and, where a line is wide enough on the
	  screen, its second harmonic: nothing in it is finer than the screen's
	  pixels can show, so the lines do not beat against them. Lines too fine
	  for the screen (under 3 pixels each) are drawn two game lines at a time.
	- The pattern (lines times mask) averages 1 and is applied in linear
	  light, less of it the brighter the colour:
	      out = c + g * depth(m) * (pattern - 1),
	      depth(m) = (1 - m) / (1 + m * (peak - 2)),
	  c being the picture, g the picture averaged over a line's height (c
	  itself at 480 lines), m the larger of the two and peak the pattern's
	  largest value. That keeps the light a flat colour gives exactly, and
	  the brightest pixel of a line at or under white for every c: nothing
	  clips, and a bright line is a wide one, as a beam's is.
	- The lines modulate g, not c: detail finer than the lines stays in the
	  picture and cannot beat against them.

	The same text is compiled for the build machine, as C++ over glm, by the
	test that checks these claims: keep it to what both languages read (no
	swizzles, no arrays, no out parameters).
*/
R"CRT(
#ifndef CRT_HOST
layout (set = 0, binding = 0) uniform sampler2D tex;
layout (push_constant) uniform pushBlock
{
	vec2 origin;	// of the picture in the framebuffer, in pixels
	vec2 size;		// of the picture there, in pixels
	vec4 beam;		// x: the game's lines; y, z: the first and second harmonics of a line's
					// profile; w: how much of the lines is left in white
	vec4 tube;		// x: the mask's pitch, in pixels; y: its depth; z: the glow; w: the vignette
} pc;
layout (location = 0) out vec4 FragColor;
#endif

const float Gamma = 2.2;
const float Turn = 6.28318530718;

vec3 toLight(vec3 c)
{
	return pow(c, vec3(Gamma));
}

vec3 toSignal(vec3 c)
{
	return pow(c, vec3(1.0 / Gamma));
}

vec3 light(vec2 uv)
{
	return toLight(vec3(texture(tex, uv)));
}

void main()
{
	vec2 texSize = vec2(textureSize(tex, 0));
	vec2 frag = vec2(gl_FragCoord.x, gl_FragCoord.y);
	vec2 uv = (frag - pc.origin) / pc.size;

	// The picture: a bilinear read, its fraction steepened where the picture
	// is much enlarged, so that a 480-line one is not a blur.
	vec2 t = uv * texSize - 0.5;
	vec2 f = fract(t);
	vec2 crisp = clamp((pc.size / texSize - 1.5) * 0.3, 0.0, 0.75);
	f = mix(f, f * f * (3.0 - 2.0 * f), crisp);
	vec3 c = light((floor(t) + 0.5 + f) / texSize);

	// The lines. period: screen pixels a line.
	float lines = min(pc.beam.x, texSize.y);
	float period = pc.size.y / lines;
	// Under 3 pixels a line: 2, 4 or 8 of the game's lines to one of these.
	// (The margin is for a period of exactly 3 and a log2 that is not exact.)
	float coarser = exp2(clamp(ceil(log2(3.0 / period) - 0.02), 0.0, 3.0));
	lines = lines / coarser;
	period = period * coarser;
	// A harmonic fades out before it reaches the screen's limit (half a cycle a pixel).
	float m1 = pc.beam.y * (1.0 - smoothstep(0.30, 0.46, 1.0 / period));
	float m2 = pc.beam.z * (1.0 - smoothstep(0.30, 0.46, 2.0 / period));
	float angle = Turn * (uv.y * lines - 0.5);		// 0 along the middle of a line
	float beam = 1.0 + m1 * cos(angle) + m2 * cos(2.0 * angle);
	float peak = 1.0 + m1 + m2;
#if CRT_TUBE
	// The mask: stripes of the screen's own pixels, green in the middle of
	// each period (magenta and green when the pitch is 2).
	float stripe = mod(floor(frag.x), pc.tube.x) / pc.tube.x;
	vec3 mask = 1.0 + pc.tube.y * cos(Turn * (stripe - vec3(1.0, 0.0, -1.0) / 3.0));
	vec3 pattern = beam * mask;
	peak = peak * (1.0 + pc.tube.y);
#else
	vec3 pattern = vec3(beam);
#endif

	// What the lines are made of: the picture, averaged over one line's
	// height where it was rendered finer than the lines. A line carries one
	// row of the game's picture; detail finer than that, were it multiplied
	// by the lines, would beat against them (moire on a floor's tiles).
	vec3 g = c;
	float rows = clamp(texSize.y / lines - 1.0, 0.0, 1.0);
	if (rows > 0.01)
	{
		vec2 third = vec2(0.0, rows / (3.0 * lines));
		g = (c + light(uv + third) + light(uv - third)) / 3.0;
	}
	// Less of the pattern the brighter the colour: see the top of the file.
	vec3 most = max(c, g);
	vec3 depth = (1.0 - most) / max(1.0 + most * (peak - 2.0), 0.0001);
	// And a trace of the lines in what would be plain white: the gaps only, a little darker.
	float gap = pc.beam.w * (m1 / max(pc.beam.y, 0.0001)) * (0.5 - 0.5 * cos(angle));
	vec3 lit = max(c + g * (depth * (pattern - 1.0) - gap * g * g), 0.0);

#if CRT_TUBE
	// The glow of the glass: the picture around, mixed in, so that it lights
	// the gaps between the lines and takes as much from the lines themselves.
	// Eight reads on a spiral (each at its own distance and angle, so that
	// the copies of a sharp edge do not line up into a second edge) out to
	// 1.3 lines of a 480-line picture.
	vec2 reach = vec2(1.3 * pc.size.y / 480.0) / pc.size;
	vec3 glow =
			  light(uv + reach * vec2(0.250, 0.000))
			+ light(uv + reach * vec2(-0.319, 0.292))
			+ light(uv + reach * vec2(0.049, -0.557))
			+ light(uv + reach * vec2(0.402, 0.525))
			+ light(uv + reach * vec2(-0.739, -0.131))
			+ light(uv + reach * vec2(0.700, -0.445))
			+ light(uv + reach * vec2(-0.234, 0.870))
			+ light(uv + reach * vec2(-0.446, -0.859));
	lit = mix(lit, glow / 8.0, pc.tube.z);

	// The corners, a little darker.
	vec2 d = uv * 2.0 - 1.0;
	float r = 0.5 * dot(d, d);
	lit = lit * (1.0 - pc.tube.w * r * r);
#endif

	FragColor = vec4(toSignal(lit), 1.0);
}
)CRT"
