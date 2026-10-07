/*
	PSFlyCast - frame generation: pictures in between the game's own.

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	How a picture in between is made (see ps5_framegen.h for when):

	1. The game's picture is reduced to its brightness, 384 lines high, and
	   that five times more to half the size each (six levels).
	2. The way the picture moved is looked for from the smallest level up,
	   and both ways round: from the picture before (kept from the last
	   frame) to this one - for every place of the picture before, where
	   what is there is in this one - and from this one back to the one
	   before. The smallest level tries every way up to six of its texels,
	   which is over a third of the picture, and prefers standing still.
	   Each larger one starts from what the smaller found here, tries what
	   it found beside here and standing still, and looks closer around the
	   one that suits best. Where the picture has nothing to tell one way
	   from another (a plain sky), and where a way leads out of the picture
	   (what leaves it at an edge in a turn), the way stays the one the
	   smaller level found: that of what is around. At the largest level
	   nothing new is looked for: every place takes the way, of those found
	   around it one level down, that suits it best, so that the line
	   between two things that move differently runs where the picture has
	   it. At the two largest levels a way that stands out alone among those
	   around it is replaced by theirs.
	3. How far each way is to be trusted: not where its two ends do not look
	   alike, and not where the way back from its end leads somewhere else -
	   that is what is hidden in the other picture, behind something that
	   moved over it.
	4. The picture in between, at a moment t between the two. For every
	   place in it, the place in the picture before whose way passes here at
	   that moment is looked for (the way there is followed back), and what
	   is there is mixed with what is at the end of its way in this picture;
	   the same is done from this picture back. The two are mixed by how far
	   each is trusted: what comes out from behind something is in one
	   picture only, and one of the two ways round may still find it. Where
	   neither is trusted the nearer of the game's two pictures is shown as
	   it is: that part of the picture then moves as it did without all
	   this, which is plainer to the eye than a guess. When the whole
	   picture is another one than the one before - the game cut to another
	   scene - all of it is the nearer picture.
	5. What has not changed for several frames running (a score over a
	   moving picture) is left as it is, whatever way was found there: how
	   long every place has been unchanged is counted in an image of its own.

	Every step is a draw of one triangle with a fragment shader, into an image
	of its own: nothing here needs more of the driver than the picture
	filters do. When anything fails, the game's pictures are shown as they
	always were: a failure is a line in the log, not a black screen.

	The file is built into the title, and into a test on the build machine
	(FRAMEGEN_TEST) that runs it on a software Vulkan driver: what it needs
	from where it runs is the few functions of `host`.
*/
#include "ps5_framegen.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

#ifndef FRAMEGEN_TEST
#include "ps5_diag.h"
#include "ps5_frontend.h"
#include "cfg/option.h"
#include "rend/vulkan/compiler.h"
#include "rend/vulkan/utils.h"
#include "rend/vulkan/vulkan_context.h"

namespace ps5
{
// 0 off, 1 light (one picture between two of the game's), 2 full (one for every present that would repeat).
config::Option<int> FrameGeneration("FrameGeneration", 0, "ps5");
}
#endif

namespace ps5::framegen
{

// What this file needs from where it runs.
namespace host
{
#ifndef FRAMEGEN_TEST
vk::Device device() { return VulkanContext::Instance()->GetDevice(); }
vk::PhysicalDevice gpu() { return VulkanContext::Instance()->GetPhysicalDevice(); }
// Which of the frames in flight this one is: what is written for one is not
// touched until that one is done.
unsigned frameIndex() { return (unsigned)VulkanContext::Instance()->GetCurrentImageIndex(); }
void waitIdle() { VulkanContext::Instance()->WaitIdle(); }
vk::UniqueShaderModule compile(vk::ShaderStageFlagBits stage, const char *source)
{
	return ShaderCompiler::TryCompile(stage, VulkanSource().addSource(source).generate());
}
int strength() { return ps5::FrameGeneration; }
void mark(const char *line) { ps5::diag::mark("%s", line); }
#else
vk::Device device();
vk::PhysicalDevice gpu();
unsigned frameIndex();
void waitIdle();
vk::UniqueShaderModule compile(vk::ShaderStageFlagBits stage, const char *source);
int strength();
void mark(const char *line);
#endif
}

namespace
{

void mark(const char *format, ...) __attribute__((format(printf, 1, 2)));
void mark(const char *format, ...)
{
	char line[256];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	host::mark(line);
}

// ---- the shaders

// One triangle over the whole target.
const char VertexSource[] = R"(
void main()
{
	vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// What every fragment shader here starts with. size.xy is one pixel of the
// target, as a share of it; size.zw one texel of the level's brightness.
const char Head[] = R"(
layout (set = 0, binding = 0) uniform sampler2D tex0;
layout (set = 0, binding = 1) uniform sampler2D tex1;
layout (set = 0, binding = 2) uniform sampler2D tex2;
layout (set = 0, binding = 3) uniform sampler2D tex3;
layout (set = 0, binding = 4) uniform sampler2D tex4;
layout (set = 0, binding = 5) uniform sampler2D tex5;
layout (set = 0, binding = 6) uniform sampler2D tex6;
layout (set = 0, binding = 7) uniform sampler2D tex7;
layout (push_constant) uniform pushBlock
{
	vec4 size;
	vec4 more;
	vec4 extra;
} pc;
layout (location = 0) out vec4 FragColor;
)";

// The game's picture to its brightness, much smaller: sixteen samples over
// what one pixel of the target covers.
const char BrightnessSource[] = R"(
void main()
{
	vec2 uv = gl_FragCoord.xy * pc.size.xy;
	vec3 sum = vec3(0.0);
	for (int j = 0; j < 4; j++)
		for (int i = 0; i < 4; i++)
			sum += textureLod(tex0, uv + (vec2(i, j) - 1.5) * 0.25 * pc.size.xy, 0.0).rgb;
	float l = dot(sum * (1.0 / 16.0), vec3(0.299, 0.587, 0.114));
	FragColor = vec4(l, l, l, 1.0);
}
)";

// A level to half its size: four samples of four texels each, so that fine
// print does not turn into a pattern of its own.
const char HalfSource[] = R"(
void main()
{
	vec2 uv = gl_FragCoord.xy * pc.size.xy;
	vec2 o = 0.5 * pc.size.xy;
	float l = 0.25 * (textureLod(tex0, uv + vec2(-o.x, -o.y), 0.0).r + textureLod(tex0, uv + vec2(o.x, -o.y), 0.0).r
			+ textureLod(tex0, uv + vec2(-o.x, o.y), 0.0).r + textureLod(tex0, uv + vec2(o.x, o.y), 0.0).r);
	FragColor = vec4(l, l, l, 1.0);
}
)";

// How unlike one picture (tex0) around a place and the other (tex1) around
// the place a way further are: 0 the same. Over extra.x texels each way
// (five by five, or three by three); with extra.z every other one of them,
// as the squares of one colour on a chessboard. What the way takes out of
// the picture is not there to compare: it is left out, and the way costs
// extra.y for all of it that is (so that what leaves the picture at an edge
// is followed out of it, by what of it is still in).
const char Unlike[] = R"(
float around[25];

bool taken(int i, int j)
{
	return pc.extra.z == 0.0 || ((i + j) & 1) == 0;
}

void look(vec2 uv)
{
	int reach = int(pc.extra.x), n = 0;
	for (int j = -reach; j <= reach; j++)
		for (int i = -reach; i <= reach; i++)
			if (taken(i, j))
				around[n++] = textureLod(tex0, uv + vec2(i, j) * pc.size.zw, 0.0).r;
}

float unlike(vec2 uv, vec2 way, float outside)
{
	int reach = int(pc.extra.x), n = 0;
	float sum = 0.0, there = 0.0;
	for (int j = -reach; j <= reach; j++)
		for (int i = -reach; i <= reach; i++)
			if (taken(i, j))
			{
				vec2 at = uv + vec2(i, j) * pc.size.zw + way;
				if (all(greaterThanEqual(at, vec2(0.0))) && all(lessThanEqual(at, vec2(1.0))))
				{
					sum += abs(around[n] - textureLod(tex1, at, 0.0).r);
					there += 1.0;
				}
				n++;
			}
	return (there > 0.0 ? sum / there : 0.0) + outside * (1.0 - there / float(n));
}

// Standing still at the smallest level, where it is preferred: a way has to
// be clearly better.
float still(vec2 uv)
{
	return unlike(uv, vec2(0.0), 0.0) * 0.85 - 0.002;
}
)";

// The smallest level: every way up to six texels. more.x is what a texel
// further costs.
const char SearchSource[] = R"(
void main()
{
	vec2 uv = gl_FragCoord.xy * pc.size.xy;
	look(uv);
	vec2 best = vec2(0.0);
	float least = still(uv);
	for (int j = -6; j <= 6; j++)
		for (int i = -6; i <= 6; i++)
		{
			if (i == 0 && j == 0)
				continue;
			vec2 way = vec2(i, j) * pc.size.zw;
			float cost = unlike(uv, way, pc.extra.y) + pc.more.x * length(vec2(i, j));
			if (cost < least)
			{
				least = cost;
				best = way;
			}
		}
	FragColor = vec4(best, least, 1.0);
}
)";

// A larger level: what the smaller one (tex2) found here and beside here,
// then closer by half a texel, a quarter, and at the last such level an
// eighth (more.y: how many of these). more.zw is one texel of the smaller
// level.
const char RefineSource[] = R"(
const vec2 beside[5] = vec2[5](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(-1.0, 0.0), vec2(0.0, 1.0), vec2(0.0, -1.0));

void main()
{
	vec2 uv = gl_FragCoord.xy * pc.size.xy;
	look(uv);
	// What the smaller level found here comes first: another way has to be
	// better by more.x, and what takes it out of the picture - where there is
	// nothing to compare - does not count against it. Standing still is one
	// more way to try, not a preferred one: where the picture has nothing to
	// tell ways apart, the way is that of what is around.
	vec2 best = textureLod(tex2, uv, 0.0).xy;
	float outside = 0.0;
	float kept = unlike(uv, best, outside);
	float least = kept;
	float zero = unlike(uv, vec2(0.0), 0.0);
	if (zero + pc.more.x < least)
	{
		least = zero + pc.more.x;
		kept = zero;
		best = vec2(0.0);
	}
	for (int k = 1; k < 5; k++)
	{
		vec2 way = textureLod(tex2, uv + beside[k] * pc.more.zw, 0.0).xy;
		float cost = unlike(uv, way, pc.extra.y);
		if (cost + pc.more.x < least)
		{
			least = cost + pc.more.x;
			kept = cost;
			best = way;
			outside = pc.extra.y;
		}
	}
	// Then closer, around the one taken.
	least = kept;
	float reach = 0.5;
	for (int stage = 0; stage < int(pc.more.y); stage++)
	{
		vec2 centre = best;
		for (int j = -1; j <= 1; j++)
			for (int i = -1; i <= 1; i++)
			{
				if (i == 0 && j == 0)
					continue;
				vec2 way = centre + vec2(i, j) * reach * pc.size.zw;
				float cost = unlike(uv, way, outside) + 0.0005 * reach;
				if (cost < least)
				{
					least = cost;
					best = way;
				}
			}
		reach *= 0.5;
	}
	FragColor = vec4(best, least, 1.0);
}
)";

// The largest level: nothing new is looked for. Every place takes the way,
// of those the level below (tex2) found at the thirteen places around it,
// that suits it best. more.zw is one texel of the level below; more.x what
// a way from beside costs.
const char ChooseSource[] = R"(
const vec2 beside[13] = vec2[13](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(-1.0, 0.0), vec2(0.0, 1.0), vec2(0.0, -1.0),
		vec2(1.0, 1.0), vec2(-1.0, 1.0), vec2(1.0, -1.0), vec2(-1.0, -1.0),
		vec2(2.0, 0.0), vec2(-2.0, 0.0), vec2(0.0, 2.0), vec2(0.0, -2.0));

void main()
{
	vec2 uv = gl_FragCoord.xy * pc.size.xy;
	look(uv);
	// The way found here, as it lies between the level below's texels.
	vec2 best = textureLod(tex2, uv, 0.0).xy;
	float kept = unlike(uv, best, 0.0);
	float least = kept;
	float zero = unlike(uv, vec2(0.0), 0.0);
	if (zero + pc.more.x < least)
	{
		least = zero + pc.more.x;
		kept = zero;
		best = vec2(0.0);
	}
	// And the ways of the texels around, each as it is.
	vec2 middle = (floor(uv / pc.more.zw) + 0.5) * pc.more.zw;
	for (int k = 0; k < 13; k++)
	{
		vec2 way = textureLod(tex2, middle + beside[k] * pc.more.zw, 0.0).xy;
		float cost = unlike(uv, way, pc.extra.y);
		if (cost + pc.more.x < least)
		{
			least = cost + pc.more.x;
			kept = cost;
			best = way;
		}
	}
	FragColor = vec4(best, kept, 1.0);
}
)";

// How long every place has been unchanged: the count so far (tex0, in
// eighths) and one more where the brightness before (tex1) and now (tex2)
// are the same, none where they are not. more.x is 0 to start again.
const char UnchangedSource[] = R"(
void main()
{
	vec2 uv = gl_FragCoord.xy * pc.size.xy;
	float same = abs(textureLod(tex1, uv, 0.0).r - textureLod(tex2, uv, 0.0).r) < 0.012 ? 1.0 : 0.0;
	float count = (textureLod(tex0, uv, 0.0).r + 0.125) * same * pc.more.x;
	FragColor = vec4(count, count, count, 1.0);
}
)";

// Whether the whole picture is another one than the one before - the game
// cut to another scene: one number, 0 no and 1 yes, in an image of one
// pixel. It is, when the ways found (tex0) cost much all over the picture, or
// when much of it is not trusted either way (tex1, tex2). A picture that
// moves faster than can be followed counts as one too, and rightly.
const char WholeSource[] = R"(
void main()
{
	float cost = 0.0, doubt = 0.0;
	for (int j = 0; j < 16; j++)
		for (int i = 0; i < 24; i++)
		{
			vec2 at = (vec2(i, j) + 0.5) / vec2(24.0, 16.0);
			cost += textureLod(tex0, at, 0.0).z;
			doubt += textureLod(tex1, at, 0.0).r + textureLod(tex2, at, 0.0).r;
		}
	cost /= 384.0;
	doubt /= 768.0;
	FragColor = vec4(max(smoothstep(0.012, 0.02, cost), smoothstep(0.17, 0.23, doubt)));
}
)";

// The ways found (tex0), with what stands out alone taken away: every place
// takes the way, of the nine at and around it, that is nearest to the other
// eight. A line between two things that move differently stays where it is;
// one place that was found a way of its own, in the middle of others that
// agree, takes theirs.
const char EvenSource[] = R"(
void main()
{
	vec2 uv = gl_FragCoord.xy * pc.size.xy;
	vec3 ways[9];
	for (int j = 0; j < 3; j++)
		for (int i = 0; i < 3; i++)
			ways[j * 3 + i] = textureLod(tex0, uv + vec2(i - 1, j - 1) * pc.size.xy, 0.0).xyz;
	int best = 4;
	float least = 1e9;
	for (int a = 0; a < 9; a++)
	{
		float sum = 0.0;
		for (int b = 0; b < 9; b++)
			sum += length((ways[a].xy - ways[b].xy) / pc.size.xy);
		// Its own, when it is as near to the others as any.
		if (sum < least - (a == 4 ? -0.01 : 0.01))
		{
			least = sum;
			best = a;
		}
	}
	FragColor = vec4(ways[best], 1.0);
}
)";

// How far a way is not to be trusted, for every place of the picture it
// starts from: 0 trusted, 1 not. tex0 is the way, tex1 the way back from the
// other picture; tex2 and tex3 the brightness, small, of the picture it
// starts from and of the other. A way is doubted where its two ends do not
// look alike (in the small brightness: a way that is a pixel off across fine
// print is still the way; more.y and more.z are how far apart they may be),
// and where the way back from its end does not lead here again: that is what
// is hidden in the other picture, behind something that moved over it. It is
// kept small and taken over five places, so that it has no holes and no
// specks: the picture in between changes over from a made part to one of the
// game's own along a line, not in dots.
const char TrustSource[] = R"(
const vec2 beside[5] = vec2[5](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(-1.0, 0.0), vec2(0.0, 1.0), vec2(0.0, -1.0));

// x: how far the way from here is not to be trusted; y: how far its other
// end is not what is here (and is then not mixed in).
vec2 doubt(vec2 at)
{
	vec3 way = textureLod(tex0, at, 0.0).xyz;
	vec2 end = at + way.xy;
	if (any(lessThan(end, vec2(0.0))) || any(greaterThan(end, vec2(1.0))))
		return vec2(0.0, 1.0);
	float unlike = smoothstep(pc.more.y, pc.more.z, abs(textureLod(tex2, at, 0.0).r - textureLod(tex3, end, 0.0).r));
	float apart = length((way.xy + textureLod(tex1, end, 0.0).xy) / pc.size.xy);
	float allowed = 1.5 + 0.2 * length(way.xy / pc.size.xy);
	float hidden = smoothstep(allowed, 3.0 * allowed, apart);
	return vec2(max(unlike, hidden), hidden);
}

void main()
{
	vec2 uv = gl_FragCoord.xy * pc.size.xy;
	float sum = 0.0, most = 0.0;
	for (int k = 1; k < 5; k++)
	{
		float d = doubt(uv + beside[k] * pc.size.xy).x;
		sum += d;
		most = max(most, d);
	}
	vec2 d = doubt(uv);
	d.x = mix((sum + d.x) * 0.2, max(most, d.x), 0.5);
	FragColor = vec4(d, 0.0, 1.0);
}
)";

// The picture in between, at the moment more.x (0 the picture before, tex0;
// 1 this one, tex1). tex2 is the way from the picture before to this one,
// tex3 the way back; tex5 and tex6 how far each is not to be trusted. tex4
// is how long every place has been unchanged; tex7 whether the whole
// picture is another one than the one before (a cut to another scene).
const char BetweenSource[] = R"(
bool inside(vec2 at)
{
	return all(greaterThanEqual(at, vec2(0.0))) && all(lessThanEqual(at, vec2(1.0)));
}

void main()
{
	vec2 uv = gl_FragCoord.xy * pc.size.xy;
	float t = pc.more.x;

	// From the picture before: the place there whose way passes here at the moment t.
	vec2 from = uv - t * textureLod(tex2, uv, 0.0).xy;
	from = uv - t * textureLod(tex2, from, 0.0).xy;
	vec2 way = textureLod(tex2, from, 0.0).xy;
	vec2 to = from + way;
	float doubt1 = max(smoothstep(1.0, 3.0, length((from + t * way - uv) / pc.size.xy)), textureLod(tex5, from, 0.0).r);
	vec3 colour1 = textureLod(tex0, from, 0.0).rgb;
	if (!inside(from))
		doubt1 = 1.0;
	else if (inside(to))
		colour1 = mix(colour1, textureLod(tex1, to, 0.0).rgb, t * (1.0 - textureLod(tex5, from, 0.0).g));

	// From this picture: the place here whose way back passes there at the same moment.
	vec2 here = uv - (1.0 - t) * textureLod(tex3, uv, 0.0).xy;
	here = uv - (1.0 - t) * textureLod(tex3, here, 0.0).xy;
	vec2 back = textureLod(tex3, here, 0.0).xy;
	vec2 there = here + back;
	float doubt2 = max(smoothstep(1.0, 3.0, length((here + (1.0 - t) * back - uv) / pc.size.xy)), textureLod(tex6, here, 0.0).r);
	vec3 colour2 = textureLod(tex1, here, 0.0).rgb;
	if (!inside(here))
		doubt2 = 1.0;
	else if (inside(there))
		colour2 = mix(colour2, textureLod(tex0, there, 0.0).rgb, (1.0 - t) * (1.0 - textureLod(tex6, here, 0.0).g));

	// The two, by how far each is trusted; where neither is, the nearer of the game's own pictures.
	vec3 nearer = t < 0.5 ? textureLod(tex0, uv, 0.0).rgb : textureLod(tex1, uv, 0.0).rgb;
	float trust1 = 1.0 - doubt1, trust2 = 1.0 - doubt2;
	trust1 *= trust1;
	trust2 *= trust2;
	float trust = max(trust1, trust2);
	vec3 colour = trust > 0.001 ? (colour1 * trust1 + colour2 * trust2) / (trust1 + trust2) : nearer;
	// A cut to another scene: nothing is trusted.
	trust *= 1.0 - textureLod(tex7, vec2(0.5), 0.0).r;
	colour = mix(nearer, colour, smoothstep(0.0, 0.2, trust));
	// What has not changed for four frames or more is left as it is.
	colour = mix(colour, nearer, smoothstep(0.375, 0.625, textureLod(tex4, uv, 0.0).r));
	FragColor = vec4(colour, 1.0);
}
)";

// The game's picture, kept for the next frame.
const char KeepSource[] = R"(
void main()
{
	FragColor = vec4(textureLod(tex0, gl_FragCoord.xy * pc.size.xy, 0.0).rgb, 1.0);
}
)";

struct Constants
{
	float size[4];
	float more[4];
	float extra[4];
};

// What a way costs for all of it that leaves the picture. (Less, and what is
// hard to find at an edge is found a way out of the picture instead.)
constexpr float OutsideCost = 0.05f;
// What a way other than the one the smaller level found here costs.
constexpr float OtherCost = 0.0005f;

// ---- what is on the device

// An image a pass draws into and later ones read.
struct Target
{
	vk::UniqueDeviceMemory memory;
	vk::UniqueImage image;
	vk::UniqueImageView view;
	vk::UniqueFramebuffer framebuffer;
	vk::Extent2D extent;
	vk::Format format = vk::Format::eUndefined;
};

enum { Levels = 6 };		// levels of brightness: [0] the largest, 384 lines
enum { TrustLevel = 2 };	// the level whose size the trust in a way is kept at
enum { Finest = 1 };		// the level the way is looked for down to; [0] chooses among its ways
enum { Images = 8 };		// images a shader can read
enum { Shaders = 11 };

struct State
{
	// What does not depend on the picture's size.
	vk::UniqueSampler sampler;
	vk::UniqueDescriptorSetLayout setLayout;
	vk::UniquePipelineLayout layout;
	vk::UniqueDescriptorPool pool;
	vk::UniqueRenderPass colourPass;		// into an image of colours or brightness
	vk::UniqueRenderPass wayPass;			// into an image of ways
	vk::UniqueShaderModule vertexShader;
	vk::UniqueShaderModule shaders[Shaders];
	vk::UniquePipeline brightness, half, search, refine, choose, even, trusting, between, keep, unchanged, whole;

	// What does.
	vk::Extent2D extent;
	Target kept;							// the picture before
	Target output;							// the picture in between
	Target luma[2][Levels];					// brightness: of this frame and of the one before, in turn
	Target way[2][Levels];					// the way found: [0] from the picture before to this one, [1] back
	Target found[2][2];						// the way as the two largest levels found it, before it is evened
	Target trust[2];						// how far each is not to be trusted
	Target still[2];						// how long every place has been unchanged, in turn
	Target unlikeness;						// whether the whole picture is another one than the one before: one pixel
	// Sets that read these images only: one for each turn.
	vk::UniqueDescriptorSet halfSets[2][Levels - 1];
	vk::UniqueDescriptorSet searchSets[2][2];			// [which way][turn]
	vk::UniqueDescriptorSet refineSets[2][2][Levels];
	vk::UniqueDescriptorSet chooseSets[2][2];
	vk::UniqueDescriptorSet evenSets[2][2];
	vk::UniqueDescriptorSet trustSets[2][2];
	vk::UniqueDescriptorSet stillSets[2];
	vk::UniqueDescriptorSet wholeSet;
	// These read the game's own picture, which is another image every frame:
	// one of each for every frame in flight.
	std::vector<vk::UniqueDescriptorSet> brightnessSets, betweenSets, keepSets;

	int now = 0;							// which of luma[] is this frame's
	bool hasBefore = false;					// there is a picture before, and its brightness
	bool inBetween = false;					// this frame's way was found: its presents show pictures in between
	bool toKeep = false;					// this frame's picture is not kept yet
	bool made = false;						// the output holds this frame's picture in between (light: made once)
};

std::unique_ptr<State> state;
bool failed;			// until the next reset(): the game's pictures are shown as they are
unsigned madeCount;

vk::UniquePipeline makePipeline(vk::ShaderModule vertex, vk::ShaderModule fragment, vk::PipelineLayout layout,
		vk::RenderPass renderPass)
{
	vk::PipelineVertexInputStateCreateInfo vertexInput;
	vk::PipelineInputAssemblyStateCreateInfo inputAssembly(vk::PipelineInputAssemblyStateCreateFlags(),
			vk::PrimitiveTopology::eTriangleList);
	vk::PipelineViewportStateCreateInfo viewport(vk::PipelineViewportStateCreateFlags(), 1, nullptr, 1, nullptr);
	vk::PipelineRasterizationStateCreateInfo rasterization;
	rasterization.lineWidth = 1.f;
	vk::PipelineMultisampleStateCreateInfo multisample;
	vk::PipelineDepthStencilStateCreateInfo depthStencil;
	vk::PipelineColorBlendAttachmentState blendAttachment;
	blendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG
			| vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
	vk::PipelineColorBlendStateCreateInfo blend(vk::PipelineColorBlendStateCreateFlags(), false, vk::LogicOp::eNoOp,
			blendAttachment);
	std::array<vk::DynamicState, 2> dynamicStates = { vk::DynamicState::eViewport, vk::DynamicState::eScissor };
	vk::PipelineDynamicStateCreateInfo dynamic(vk::PipelineDynamicStateCreateFlags(), dynamicStates);
	std::array<vk::PipelineShaderStageCreateInfo, 2> stages = {
			vk::PipelineShaderStageCreateInfo(vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eVertex, vertex, "main"),
			vk::PipelineShaderStageCreateInfo(vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eFragment, fragment, "main"),
	};
	vk::GraphicsPipelineCreateInfo info(vk::PipelineCreateFlags(), stages, &vertexInput, &inputAssembly, nullptr, &viewport,
			&rasterization, &multisample, &depthStencil, &blend, &dynamic, layout, renderPass, 0);
	return host::device().createGraphicsPipelineUnique(vk::PipelineCache(), info).value;
}

// A render pass of one attachment, which is all written and read as a texture afterwards.
vk::UniqueRenderPass makePass(vk::Format format)
{
	vk::AttachmentDescription attachment(vk::AttachmentDescriptionFlags(), format,
			vk::SampleCountFlagBits::e1, vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eStore,
			vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
			vk::ImageLayout::eUndefined, vk::ImageLayout::eShaderReadOnlyOptimal);
	vk::AttachmentReference colorReference(0, vk::ImageLayout::eColorAttachmentOptimal);
	vk::SubpassDescription subpass(vk::SubpassDescriptionFlags(), vk::PipelineBindPoint::eGraphics, nullptr, colorReference);
	std::array<vk::SubpassDependency, 2> dependencies = {
			// A pass before read the image; this one writes it again.
			vk::SubpassDependency(vk::SubpassExternal, 0,
					vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eColorAttachmentOutput,
					vk::PipelineStageFlagBits::eColorAttachmentOutput,
					vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eColorAttachmentWrite,
					vk::AccessFlagBits::eColorAttachmentWrite),
			vk::SubpassDependency(0, vk::SubpassExternal, vk::PipelineStageFlagBits::eColorAttachmentOutput,
					vk::PipelineStageFlagBits::eFragmentShader, vk::AccessFlagBits::eColorAttachmentWrite,
					vk::AccessFlagBits::eShaderRead),
	};
	return host::device().createRenderPassUnique(vk::RenderPassCreateInfo(vk::RenderPassCreateFlags(), attachment, subpass,
			dependencies));
}

constexpr vk::Format ColourFormat = vk::Format::eR8G8B8A8Unorm;
constexpr vk::Format WayFormat = vk::Format::eR16G16B16A16Sfloat;

// What does not depend on the picture's size. Throws when the driver or the
// shader compiler refuses something.
void makeState()
{
	if (state)
		return;
	const vk::Device device = host::device();
	auto s = std::make_unique<State>();

	std::array<vk::DescriptorSetLayoutBinding, Images> bindings;
	for (uint32_t i = 0; i < Images; i++)
		bindings[i] = vk::DescriptorSetLayoutBinding(i, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment);
	s->setLayout = device.createDescriptorSetLayoutUnique(
			vk::DescriptorSetLayoutCreateInfo(vk::DescriptorSetLayoutCreateFlags(), bindings));
	vk::PushConstantRange range(vk::ShaderStageFlagBits::eFragment, 0, sizeof(Constants));
	s->layout = device.createPipelineLayoutUnique(
			vk::PipelineLayoutCreateInfo(vk::PipelineLayoutCreateFlags(), s->setLayout.get(), range));
	// Some forty sets that stay, and three for every frame in flight.
	const uint32_t sets = 128;
	vk::DescriptorPoolSize poolSize(vk::DescriptorType::eCombinedImageSampler, sets * Images);
	s->pool = device.createDescriptorPoolUnique(
			vk::DescriptorPoolCreateInfo(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, sets, poolSize));
	s->sampler = device.createSamplerUnique(vk::SamplerCreateInfo(vk::SamplerCreateFlags(), vk::Filter::eLinear,
			vk::Filter::eLinear, vk::SamplerMipmapMode::eNearest, vk::SamplerAddressMode::eClampToEdge,
			vk::SamplerAddressMode::eClampToEdge, vk::SamplerAddressMode::eClampToEdge, 0.f, false, 1.f, false,
			vk::CompareOp::eNever, 0.f, 0.f, vk::BorderColor::eFloatOpaqueBlack));
	s->colourPass = makePass(ColourFormat);
	s->wayPass = makePass(WayFormat);

	s->vertexShader = host::compile(vk::ShaderStageFlagBits::eVertex, VertexSource);
	const std::string head(Head), unlike(Unlike);
	const std::string sources[Shaders] = {
			head + BrightnessSource, head + HalfSource, head + unlike + SearchSource, head + unlike + RefineSource,
			head + unlike + ChooseSource, head + BetweenSource, head + KeepSource, head + UnchangedSource, head + WholeSource,
			head + TrustSource, head + EvenSource,
	};
	if (!s->vertexShader)
		throw std::runtime_error("a shader did not compile");
	for (int i = 0; i < Shaders; i++)
	{
		s->shaders[i] = host::compile(vk::ShaderStageFlagBits::eFragment, sources[i].c_str());
		if (!s->shaders[i])
			throw std::runtime_error("a shader did not compile");
	}
	s->brightness = makePipeline(*s->vertexShader, *s->shaders[0], *s->layout, *s->colourPass);
	s->half = makePipeline(*s->vertexShader, *s->shaders[1], *s->layout, *s->colourPass);
	s->search = makePipeline(*s->vertexShader, *s->shaders[2], *s->layout, *s->wayPass);
	s->refine = makePipeline(*s->vertexShader, *s->shaders[3], *s->layout, *s->wayPass);
	s->choose = makePipeline(*s->vertexShader, *s->shaders[4], *s->layout, *s->wayPass);
	s->between = makePipeline(*s->vertexShader, *s->shaders[5], *s->layout, *s->colourPass);
	s->keep = makePipeline(*s->vertexShader, *s->shaders[6], *s->layout, *s->colourPass);
	s->unchanged = makePipeline(*s->vertexShader, *s->shaders[7], *s->layout, *s->colourPass);
	s->whole = makePipeline(*s->vertexShader, *s->shaders[8], *s->layout, *s->wayPass);
	s->trusting = makePipeline(*s->vertexShader, *s->shaders[9], *s->layout, *s->colourPass);
	s->even = makePipeline(*s->vertexShader, *s->shaders[10], *s->layout, *s->wayPass);
	state = std::move(s);
	mark("frame generation: ready");
}

Target makeTarget(uint32_t width, uint32_t height, vk::Format format, vk::RenderPass renderPass)
{
	const vk::Device device = host::device();
	Target target;
	target.extent = vk::Extent2D(width, height);
	target.format = format;
	target.image = device.createImageUnique(vk::ImageCreateInfo(vk::ImageCreateFlags(), vk::ImageType::e2D, format,
			vk::Extent3D(width, height, 1), 1, 1, vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal,
			vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled
#ifdef FRAMEGEN_TEST
					| vk::ImageUsageFlagBits::eTransferSrc
#endif
			, vk::SharingMode::eExclusive, 0, nullptr, vk::ImageLayout::eUndefined));
	const vk::MemoryRequirements needs = device.getImageMemoryRequirements(*target.image);
	const vk::PhysicalDeviceMemoryProperties memory = host::gpu().getMemoryProperties();
	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; i < memory.memoryTypeCount && type == UINT32_MAX; i++)
		if ((needs.memoryTypeBits & (1u << i)) != 0
				&& (memory.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal))
			type = i;
	if (type == UINT32_MAX)
		throw std::runtime_error("no memory for an image");
	target.memory = device.allocateMemoryUnique(vk::MemoryAllocateInfo(needs.size, type));
	device.bindImageMemory(*target.image, *target.memory, 0);
	target.view = device.createImageViewUnique(vk::ImageViewCreateInfo(vk::ImageViewCreateFlags(), *target.image,
			vk::ImageViewType::e2D, format, vk::ComponentMapping(),
			vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
	target.framebuffer = device.createFramebufferUnique(vk::FramebufferCreateInfo(vk::FramebufferCreateFlags(), renderPass,
			*target.view, width, height, 1));
	return target;
}

vk::UniqueDescriptorSet allocateSet()
{
	return std::move(host::device().allocateDescriptorSetsUnique(
			vk::DescriptorSetAllocateInfo(*state->pool, state->setLayout.get())).front());
}

// A set's images; one that a shader does not read is given the first.
void writeSet(vk::DescriptorSet set, std::initializer_list<vk::ImageView> views)
{
	std::array<vk::DescriptorImageInfo, Images> images;
	std::array<vk::WriteDescriptorSet, Images> writes;
	for (uint32_t i = 0; i < Images; i++)
	{
		const vk::ImageView view = i < views.size() ? views.begin()[i] : *views.begin();
		images[i] = vk::DescriptorImageInfo(*state->sampler, view, vk::ImageLayout::eShaderReadOnlyOptimal);
		writes[i] = vk::WriteDescriptorSet(set, i, 0, 1, vk::DescriptorType::eCombinedImageSampler, &images[i]);
	}
	host::device().updateDescriptorSets(writes, nullptr);
}

// A set that stays: made the first time, and written for the images there are now.
void staySet(vk::UniqueDescriptorSet& set, std::initializer_list<vk::ImageView> views)
{
	if (!set)
		set = allocateSet();
	writeSet(*set, views);
}

// This frame in flight's set of a kind.
vk::DescriptorSet frameSet(std::vector<vk::UniqueDescriptorSet>& sets)
{
	const unsigned index = host::frameIndex();
	if (index >= sets.size())
		sets.resize(index + 1);
	if (!sets[index])
		sets[index] = allocateSet();
	return *sets[index];
}

// The images of a picture's size, and the sets that read them. Throws as makeState does.
void makeSized(const vk::Extent2D& extent)
{
	State& s = *state;
	if (s.extent == extent)
		return;
	if (s.extent.width != 0)
		// Frames in flight read the old ones.
		host::waitIdle();
	s.extent = vk::Extent2D();
	s.hasBefore = s.inBetween = s.toKeep = s.made = false;
	s.brightnessSets.clear();
	s.betweenSets.clear();
	s.keepSets.clear();

	s.kept = makeTarget(extent.width, extent.height, ColourFormat, *s.colourPass);
	s.output = makeTarget(extent.width, extent.height, ColourFormat, *s.colourPass);
	// The largest level: 384 lines, and as wide as the picture's shape makes
	// it, both in thirty-twos so that every level halves evenly.
	uint32_t height = 384;
	uint32_t width = std::clamp<uint32_t>((uint32_t)std::lround(384.0 * extent.width / extent.height / 32.0) * 32, 256, 1024);
	for (int level = 0; level < Levels; level++)
	{
		for (int turn = 0; turn < 2; turn++)
			s.luma[turn][level] = makeTarget(width, height, ColourFormat, *s.colourPass);
		for (int which = 0; which < 2; which++)
			s.way[which][level] = makeTarget(width, height, WayFormat, *s.wayPass);
		width /= 2;
		height /= 2;
	}
	for (int which = 0; which < 2; which++)
	{
		for (int level = 0; level <= Finest; level++)
		{
			s.found[which][level] = makeTarget(s.way[0][level].extent.width, s.way[0][level].extent.height, WayFormat, *s.wayPass);
			staySet(s.evenSets[which][level], { *s.found[which][level].view });
		}
	}
	for (int which = 0; which < 2; which++)
		s.trust[which] = makeTarget(s.way[0][TrustLevel].extent.width, s.way[0][TrustLevel].extent.height, ColourFormat, *s.colourPass);
	s.unlikeness = makeTarget(1, 1, WayFormat, *s.wayPass);
	staySet(s.wholeSet, { *s.way[0][Finest].view, *s.trust[0].view, *s.trust[1].view });
	for (int turn = 0; turn < 2; turn++)
		s.still[turn] = makeTarget(s.luma[0][0].extent.width, s.luma[0][0].extent.height, ColourFormat, *s.colourPass);
	for (int turn = 0; turn < 2; turn++)
	{
		staySet(s.stillSets[turn], { *s.still[turn ^ 1].view, *s.luma[turn ^ 1][0].view, *s.luma[turn][0].view });
		for (int level = 0; level < Levels - 1; level++)
			staySet(s.halfSets[turn][level], { *s.luma[turn][level].view });
		for (int which = 0; which < 2; which++)
		{
			// The way from the picture before to this one starts from the one before's brightness; the way back, from this one's.
			const Target *start = s.luma[which == 0 ? turn ^ 1 : turn], *end = s.luma[which == 0 ? turn : turn ^ 1];
			staySet(s.searchSets[which][turn], { *start[Levels - 1].view, *end[Levels - 1].view });
			for (int level = Finest; level < Levels - 1; level++)
				staySet(s.refineSets[which][turn][level], { *start[level].view, *end[level].view, *s.way[which][level + 1].view });
			staySet(s.chooseSets[which][turn], { *start[0].view, *end[0].view, *s.way[which][Finest].view });
			staySet(s.trustSets[which][turn], { *s.way[which][0].view, *s.way[which ^ 1][0].view, *start[Finest].view,
					*end[Finest].view });
		}
	}
	s.extent = extent;
	mark("frame generation: pictures of %u x %u, the way found at %u x %u", extent.width, extent.height,
			s.way[0][0].extent.width, s.way[0][0].extent.height);
}

// One draw over all of a target.
void draw(vk::CommandBuffer commandBuffer, vk::RenderPass renderPass, const Target& target, vk::Pipeline pipeline,
		vk::DescriptorSet set, const Constants& constants)
{
	commandBuffer.beginRenderPass(vk::RenderPassBeginInfo(renderPass, *target.framebuffer, vk::Rect2D({ 0, 0 }, target.extent)),
			vk::SubpassContents::eInline);
	commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
	commandBuffer.setViewport(0, vk::Viewport(0.f, 0.f, (float)target.extent.width, (float)target.extent.height, 0.f, 1.f));
	commandBuffer.setScissor(0, vk::Rect2D({ 0, 0 }, target.extent));
	commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *state->layout, 0, set, nullptr);
	commandBuffer.pushConstants(*state->layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(constants), &constants);
	commandBuffer.draw(3, 1, 0, 0);
	commandBuffer.endRenderPass();
}

Constants sizes(const Target& target)
{
	const float x = 1.f / target.extent.width, y = 1.f / target.extent.height;
	return Constants{ { x, y, x, y }, { 0, 0, 0, 0 }, { 2, OutsideCost, 0, 0 } };
}

// The way between the two pictures, one way round, from the smallest level up.
void findWay(vk::CommandBuffer commandBuffer, int which)
{
	State& s = *state;
	const int now = s.now;
	Constants constants = sizes(s.way[which][Levels - 1]);
	constants.more[0] = 0.0015f;
	draw(commandBuffer, *s.wayPass, s.way[which][Levels - 1], *s.search, *s.searchSets[which][now], constants);
	for (int level = Levels - 2; level >= Finest; level--)
	{
		constants = sizes(s.way[which][level]);
		constants.more[0] = OtherCost;
		// The last of these levels looks closest.
		constants.more[1] = level == Finest ? 3.f : 2.f;
		// The largest of these is most of the work: it compares every other texel of the five by five.
		if (level == Finest)
			constants.extra[2] = 1.f;
		constants.more[2] = 1.f / s.way[which][level + 1].extent.width;
		constants.more[3] = 1.f / s.way[which][level + 1].extent.height;
		if (level == Finest)
		{
			// What stands out alone is taken away before the largest level chooses.
			draw(commandBuffer, *s.wayPass, s.found[which][level], *s.refine, *s.refineSets[which][now][level], constants);
			draw(commandBuffer, *s.wayPass, s.way[which][level], *s.even, *s.evenSets[which][level], sizes(s.way[which][level]));
		}
		else
			draw(commandBuffer, *s.wayPass, s.way[which][level], *s.refine, *s.refineSets[which][now][level], constants);
	}
	// The largest level chooses among those ways, over three texels by three.
	constants = sizes(s.way[which][0]);
	constants.more[0] = OtherCost;
	constants.more[2] = 1.f / s.way[which][Finest].extent.width;
	constants.more[3] = 1.f / s.way[which][Finest].extent.height;
	constants.extra[0] = 1.f;
	draw(commandBuffer, *s.wayPass, s.found[which][0], *s.choose, *s.chooseSets[which][now], constants);
	// And again after.
	draw(commandBuffer, *s.wayPass, s.way[which][0], *s.even, *s.evenSets[which][0], sizes(s.way[which][0]));
}

void fail(const char *what)
{
	failed = true;
	state.reset();
	mark("frame generation: failed (%s): the game's own pictures are shown", what);
}

} // namespace

bool wanted()
{
	return host::strength() > 0 && !failed;
}

unsigned made()
{
	return madeCount;
}

vk::ImageView show(vk::CommandBuffer commandBuffer, bool fresh, vk::ImageView view, const vk::Extent2D& extent, int slot,
		int slots)
{
	if (!wanted() || !view || extent.width < 64 || extent.height < 64)
	{
		// Off: when it is on again, there is no picture before.
		if (state)
			state->hasBefore = state->inBetween = state->toKeep = state->made = false;
		return view;
	}
	try {
		makeState();
		makeSized(extent);
		State& s = *state;
		if (fresh)
		{
			// A picture that was not kept (its presents were cut short) is not the one before this.
			if (s.toKeep)
				s.hasBefore = false;
			s.toKeep = true;
			s.made = false;
			s.now ^= 1;
			const int now = s.now;
			// This picture's brightness, level by level.
			const vk::DescriptorSet brightnessSet = frameSet(s.brightnessSets);
			writeSet(brightnessSet, { view });
			draw(commandBuffer, *s.colourPass, s.luma[now][0], *s.brightness, brightnessSet, sizes(s.luma[now][0]));
			for (int level = 1; level < Levels; level++)
				draw(commandBuffer, *s.colourPass, s.luma[now][level], *s.half, *s.halfSets[now][level - 1],
						sizes(s.luma[now][level]));
			// How long every place has been unchanged; from nothing when there is no picture before
			// (the set then read is one of this frame's own brightness).
			{
				Constants constants = sizes(s.still[now]);
				constants.more[0] = s.hasBefore ? 1.f : 0.f;
				draw(commandBuffer, *s.colourPass, s.still[now], *s.unchanged,
						s.hasBefore ? *s.stillSets[now] : *s.halfSets[now][0], constants);
			}
			// The way from the picture before to this one, and back.
			s.inBetween = s.hasBefore && slots > 1;
			if (s.inBetween)
			{
				findWay(commandBuffer, 0);
				findWay(commandBuffer, 1);
				for (int which = 0; which < 2; which++)
				{
					Constants constants = sizes(s.trust[which]);
					constants.more[1] = 0.04f;
					constants.more[2] = 0.14f;
					draw(commandBuffer, *s.colourPass, s.trust[which], *s.trusting, *s.trustSets[which][s.now], constants);
				}
				draw(commandBuffer, *s.wayPass, s.unlikeness, *s.whole, *s.wholeSet, sizes(s.unlikeness));
			}
		}
		// Which of this frame's pictures this present shows: with full
		// strength one for each present, the game's own last; with light
		// strength two, each for half of the presents.
		const int pictures = host::strength() >= 2 ? slots : std::min(slots, 2);
		const int picture = slot * pictures / std::max(slots, 1);
		if (s.inBetween && picture < pictures - 1)
		{
			// A picture in between: the further on, the nearer this frame's own.
			if (!s.made || pictures > 2)
			{
				const vk::DescriptorSet set = frameSet(s.betweenSets);
				writeSet(set, { *s.kept.view, view, *s.way[0][0].view, *s.way[1][0].view, *s.still[s.now].view,
						*s.trust[0].view, *s.trust[1].view, *s.unlikeness.view });
				Constants constants = sizes(s.output);
				constants.more[0] = (float)(picture + 1) / pictures;
				draw(commandBuffer, *s.colourPass, s.output, *s.between, set, constants);
				s.made = true;
			}
			madeCount++;
			return *s.output.view;
		}
		if (s.toKeep)
		{
			// The game's own picture, the first time it is shown: it is the one before the next.
			const vk::DescriptorSet set = frameSet(s.keepSets);
			writeSet(set, { view });
			draw(commandBuffer, *s.colourPass, s.kept, *s.keep, set, sizes(s.kept));
			s.toKeep = false;
			s.hasBefore = true;
			s.inBetween = false;
		}
	} catch (const std::exception& e) {
		fail(e.what());
	}
	return view;
}

void reset()
{
	state.reset();
	failed = false;
}

#ifdef FRAMEGEN_TEST
TestImage testOutput()
{
	return TestImage{ *state->output.image, state->output.extent, state->output.format };
}

TestImage testTrust()
{
	const Target& trust = state->trust[0];
	return TestImage{ *trust.image, trust.extent, trust.format };
}

TestImage testFlow()
{
	const Target& way = state->way[0][0];
	return TestImage{ *way.image, way.extent, way.format };
}
#endif

} // namespace ps5::framegen
