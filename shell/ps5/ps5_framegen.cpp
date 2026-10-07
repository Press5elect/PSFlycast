/*
	PSFlyCast - frame generation: pictures in between the game's own.

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	How a picture in between is made (see ps5_framegen.h for when):

	1. The game's picture is reduced to its brightness, 384 lines high, and
	   that four times more to half the size each (five levels).
	2. The way the picture moved is looked for from the smallest level up,
	   from the picture before (kept from the last frame) to this one: for
	   every place of the picture before, where what is there is in this
	   one. The smallest level tries every way up to six of its texels; each
	   larger one starts from what the smaller found, here and beside here,
	   and looks closer, the largest to an eighth of its texel. Standing still is preferred, and where the picture
	   has nothing to tell one way from another (a plain sky) the way is the
	   one found around it.
	3. The picture in between, at a moment t between the two. For every
	   place in it, the place in the picture before whose way passes here at
	   that moment is looked for (the way there is followed back, three
	   times over); what is there, and what is at the end of its way in this
	   picture, are mixed. Where that goes wrong - the two do not look
	   alike, or no place's way passes here, as behind something that moved
	   off - the nearer of the game's two pictures is shown there as it is:
	   that part of the picture then moves as it did without all this.
	   When the whole picture is unlike the one before - the game cut to
	   another scene - all of it is the nearer picture.
	4. What has not changed for several frames running (a score over a
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
config::Option<bool> FrameGeneration("FrameGeneration", false, "ps5");
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
bool on() { return ps5::FrameGeneration; }
void mark(const char *line) { ps5::diag::mark("%s", line); }
#else
vk::Device device();
vk::PhysicalDevice gpu();
unsigned frameIndex();
void waitIdle();
vk::UniqueShaderModule compile(vk::ShaderStageFlagBits stage, const char *source);
bool on();
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

// How unlike the picture before (tex0) around a place and this one (tex1)
// around the place a way further are: 0 the same. Over extra.x texels each
// way (five by five, or three by three). What the way takes out of the
// picture is not there to compare: it counts as a little unlike.
const char Unlike[] = R"(
float around[25];

void look(vec2 uv)
{
	int reach = int(pc.extra.x), n = 0;
	for (int j = -reach; j <= reach; j++)
		for (int i = -reach; i <= reach; i++)
			around[n++] = textureLod(tex0, uv + vec2(i, j) * pc.size.zw, 0.0).r;
}

float unlike(vec2 uv, vec2 way)
{
	int reach = int(pc.extra.x), n = 0;
	float sum = 0.0;
	for (int j = -reach; j <= reach; j++)
		for (int i = -reach; i <= reach; i++)
		{
			vec2 at = uv + vec2(i, j) * pc.size.zw + way;
			bool there = all(greaterThanEqual(at, vec2(0.0))) && all(lessThanEqual(at, vec2(1.0)));
			sum += there ? abs(around[n] - textureLod(tex1, at, 0.0).r) : 0.08;
			n++;
		}
	return sum / float(n);
}

// Standing still, which is preferred: a way has to be clearly better.
float still(vec2 uv)
{
	return unlike(uv, vec2(0.0)) * 0.85 - 0.002;
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
			float cost = unlike(uv, way) + pc.more.x * length(vec2(i, j));
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
// then closer by half a texel, a quarter, and at the last level an eighth
// (more.y: how many of these). more.zw is one texel of the smaller level.
const char RefineSource[] = R"(
const vec2 beside[5] = vec2[5](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(-1.0, 0.0), vec2(0.0, 1.0), vec2(0.0, -1.0));

void main()
{
	vec2 uv = gl_FragCoord.xy * pc.size.xy;
	look(uv);
	vec2 best = vec2(0.0);
	float least = still(uv);
	for (int k = 0; k < 5; k++)
	{
		vec2 way = textureLod(tex2, uv + beside[k] * pc.more.zw, 0.0).xy;
		float cost = unlike(uv, way) + (k == 0 ? 0.0 : pc.more.x);
		if (cost < least)
		{
			least = cost;
			best = way;
		}
	}
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
				float cost = unlike(uv, way) + pc.more.x * reach;
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

// How unlike the whole picture is to the one before, at the end of the ways
// found (tex0): one number, in an image of one pixel.
const char WholeSource[] = R"(
void main()
{
	float sum = 0.0;
	for (int j = 0; j < 16; j++)
		for (int i = 0; i < 24; i++)
			sum += textureLod(tex0, (vec2(i, j) + 0.5) / vec2(24.0, 16.0), 0.0).z;
	FragColor = vec4(sum / 384.0);
}
)";

// The picture in between, at the moment more.x (0 the picture before, tex0;
// 1 this one, tex1), by the way found (tex2). tex6 is how unlike the whole
// picture is to the one before (a cut to another scene). tex3 is how long every place
// has been unchanged, tex4 and tex5 the two pictures' brightness at the
// level the way was found at. more.y and more.z: how far apart the two ends of a way may
// be in brightness before it is not trusted. (The brightness, which is
// smaller than the picture: a way that is a pixel off across fine print is
// still the way.)
const char BetweenSource[] = R"(
bool inside(vec2 at)
{
	return all(greaterThanEqual(at, vec2(0.0))) && all(lessThanEqual(at, vec2(1.0)));
}

void main()
{
	vec2 uv = gl_FragCoord.xy * pc.size.xy;
	float t = pc.more.x;
	// The place in the picture before whose way passes here at the moment t.
	vec2 from = uv - t * textureLod(tex2, uv, 0.0).xy;
	from = uv - t * textureLod(tex2, from, 0.0).xy;
	from = uv - t * textureLod(tex2, from, 0.0).xy;
	vec2 way = textureLod(tex2, from, 0.0).xy;
	vec2 to = from + way;
	vec3 start = textureLod(tex0, from, 0.0).rgb;
	vec3 end = textureLod(tex1, to, 0.0).rgb;
	// An end outside the picture is not there to be mixed in.
	float share = inside(to) ? (inside(from) ? t : 1.0) : 0.0;
	vec3 moved = mix(start, end, share);
	// Not trusted: the two ends do not look alike, no way passes here, or both ends are outside.
	float apart = abs(textureLod(tex4, from, 0.0).r - textureLod(tex5, to, 0.0).r);
	float off = length((from + t * way - uv) / pc.size.xy);
	float doubt = max(smoothstep(pc.more.y, pc.more.z, apart), smoothstep(1.0, 3.0, off));
	if (!inside(to) && !inside(from))
		doubt = 1.0;
	doubt = max(doubt, smoothstep(0.015, 0.03, textureLod(tex6, vec2(0.5), 0.0).r));
	vec3 now = textureLod(tex1, uv, 0.0).rgb;
	vec3 nearer = t < 0.5 ? textureLod(tex0, uv, 0.0).rgb : now;
	vec3 colour = mix(moved, nearer, doubt);
	// What has not changed for four frames or more is left as it is.
	colour = mix(colour, nearer, smoothstep(0.375, 0.625, textureLod(tex3, uv, 0.0).r));
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

enum { Levels = 5, FlowFinest = 1 };		// levels of brightness; the way is found down to this one
enum { Images = 7 };						// images a shader can read

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
	vk::UniqueShaderModule shaders[8];
	vk::UniquePipeline brightness, half, search, refine, between, keep, unchanged, whole;

	// What does.
	vk::Extent2D extent;
	Target kept;							// the picture before
	Target output;							// the picture in between
	Target luma[2][Levels];					// brightness: of this frame and of the one before, in turn
	Target way[Levels];						// the way found, [FlowFinest] to [Levels - 1]
	Target still[2];						// how long every place has been unchanged, in turn
	Target unlikeness;						// how unlike the whole picture is to the one before: one pixel
	vk::UniqueDescriptorSet halfSets[2][Levels - 1];
	vk::UniqueDescriptorSet searchSets[2];
	vk::UniqueDescriptorSet refineSets[2][Levels];
	vk::UniqueDescriptorSet stillSets[2];
	vk::UniqueDescriptorSet wholeSet;
	// These read the game's own picture, which is another image every frame:
	// one of each for every frame in flight.
	std::vector<vk::UniqueDescriptorSet> brightnessSets, betweenSets, keepSets;

	int now = 0;							// which of luma[] is this frame's
	bool hasBefore = false;					// there is a picture before, and its brightness
	bool inBetween = false;					// this frame's way was found: its presents show pictures in between
	bool toKeep = false;					// this frame's picture is not kept yet
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
	// A score of sets that stay, and three for every frame in flight.
	const uint32_t sets = 96;
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
	const std::string sources[8] = {
			head + BrightnessSource, head + HalfSource, head + unlike + SearchSource, head + unlike + RefineSource,
			head + BetweenSource, head + KeepSource, head + UnchangedSource, head + WholeSource,
	};
	if (!s->vertexShader)
		throw std::runtime_error("a shader did not compile");
	for (int i = 0; i < 8; i++)
	{
		s->shaders[i] = host::compile(vk::ShaderStageFlagBits::eFragment, sources[i].c_str());
		if (!s->shaders[i])
			throw std::runtime_error("a shader did not compile");
	}
	s->brightness = makePipeline(*s->vertexShader, *s->shaders[0], *s->layout, *s->colourPass);
	s->half = makePipeline(*s->vertexShader, *s->shaders[1], *s->layout, *s->colourPass);
	s->search = makePipeline(*s->vertexShader, *s->shaders[2], *s->layout, *s->wayPass);
	s->refine = makePipeline(*s->vertexShader, *s->shaders[3], *s->layout, *s->wayPass);
	s->between = makePipeline(*s->vertexShader, *s->shaders[4], *s->layout, *s->colourPass);
	s->keep = makePipeline(*s->vertexShader, *s->shaders[5], *s->layout, *s->colourPass);
	s->unchanged = makePipeline(*s->vertexShader, *s->shaders[6], *s->layout, *s->colourPass);
	s->whole = makePipeline(*s->vertexShader, *s->shaders[7], *s->layout, *s->wayPass);
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
	s.hasBefore = s.inBetween = s.toKeep = false;
	s.brightnessSets.clear();
	s.betweenSets.clear();
	s.keepSets.clear();

	s.kept = makeTarget(extent.width, extent.height, ColourFormat, *s.colourPass);
	s.output = makeTarget(extent.width, extent.height, ColourFormat, *s.colourPass);
	// The largest level: 384 lines, and as wide as the picture's shape makes
	// it, both in sixteens so that every level halves evenly.
	uint32_t height = 384;
	uint32_t width = std::clamp<uint32_t>((uint32_t)std::lround(384.0 * extent.width / extent.height / 16.0) * 16, 256, 1024);
	for (int level = 0; level < Levels; level++)
	{
		for (int turn = 0; turn < 2; turn++)
			s.luma[turn][level] = makeTarget(width, height, ColourFormat, *s.colourPass);
		if (level >= FlowFinest)
			s.way[level] = makeTarget(width, height, WayFormat, *s.wayPass);
		width /= 2;
		height /= 2;
	}
	s.unlikeness = makeTarget(1, 1, WayFormat, *s.wayPass);
	if (!s.wholeSet)
		s.wholeSet = allocateSet();
	writeSet(*s.wholeSet, { *s.way[FlowFinest].view });
	for (int turn = 0; turn < 2; turn++)
		s.still[turn] = makeTarget(s.luma[0][0].extent.width, s.luma[0][0].extent.height, ColourFormat, *s.colourPass);
	for (int turn = 0; turn < 2; turn++)
	{
		if (!s.stillSets[turn])
			s.stillSets[turn] = allocateSet();
		writeSet(*s.stillSets[turn], { *s.still[turn ^ 1].view, *s.luma[turn ^ 1][0].view, *s.luma[turn][0].view });
		for (int level = 0; level < Levels - 1; level++)
		{
			if (!s.halfSets[turn][level])
				s.halfSets[turn][level] = allocateSet();
			writeSet(*s.halfSets[turn][level], { *s.luma[turn][level].view });
		}
		if (!s.searchSets[turn])
			s.searchSets[turn] = allocateSet();
		writeSet(*s.searchSets[turn], { *s.luma[turn ^ 1][Levels - 1].view, *s.luma[turn][Levels - 1].view });
		for (int level = FlowFinest; level < Levels - 1; level++)
		{
			if (!s.refineSets[turn][level])
				s.refineSets[turn][level] = allocateSet();
			writeSet(*s.refineSets[turn][level], { *s.luma[turn ^ 1][level].view, *s.luma[turn][level].view, *s.way[level + 1].view });
		}
	}
	s.extent = extent;
	mark("frame generation: pictures of %u x %u, the way found at %u x %u", extent.width, extent.height,
			s.way[FlowFinest].extent.width, s.way[FlowFinest].extent.height);
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
	return Constants{ { x, y, x, y }, { 0, 0, 0, 0 }, { 2, 0, 0, 0 } };
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
	return host::on() && !failed;
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
		{
			state->hasBefore = false;
			state->inBetween = false;
			state->toKeep = false;
		}
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
			// The way from the picture before to this one, from the smallest level up.
			s.inBetween = s.hasBefore && slots > 1;
			if (s.inBetween)
			{
				Constants constants = sizes(s.way[Levels - 1]);
				constants.more[0] = 0.0015f;
				draw(commandBuffer, *s.wayPass, s.way[Levels - 1], *s.search, *s.searchSets[now], constants);
				for (int level = Levels - 2; level >= FlowFinest; level--)
				{
					constants = sizes(s.way[level]);
					constants.more[0] = 0.0005f;
					// The last level looks closest.
					constants.more[1] = level == FlowFinest ? 3.f : 2.f;
					constants.more[2] = 1.f / s.way[level + 1].extent.width;
					constants.more[3] = 1.f / s.way[level + 1].extent.height;
					draw(commandBuffer, *s.wayPass, s.way[level], *s.refine, *s.refineSets[now][level], constants);
				}
				draw(commandBuffer, *s.wayPass, s.unlikeness, *s.whole, *s.wholeSet, sizes(s.unlikeness));
			}
		}
		if (s.inBetween && slot < slots - 1)
		{
			// A picture in between: the further on, the nearer this frame's own.
			const vk::DescriptorSet set = frameSet(s.betweenSets);
			writeSet(set, { *s.kept.view, view, *s.way[FlowFinest].view, *s.still[s.now].view,
					*s.luma[s.now ^ 1][FlowFinest].view, *s.luma[s.now][FlowFinest].view, *s.unlikeness.view });
			Constants constants = sizes(s.output);
			constants.more[0] = (float)(slot + 1) / slots;
			constants.more[1] = 0.04f;
			constants.more[2] = 0.14f;
			draw(commandBuffer, *s.colourPass, s.output, *s.between, set, constants);
			madeCount++;
			return *s.output.view;
		}
		if (s.toKeep)
		{
			// The game's own picture, last of its presents: it is the one before the next.
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

TestImage testFlow()
{
	const Target& way = state->way[FlowFinest];
	return TestImage{ *way.image, way.extent, way.format };
}
#endif

} // namespace ps5::framegen
