/*
	PSFlyCast - anti-aliasing of the game's picture before it is scaled to the screen (ps5_aa.h).

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-3.0-or-later
	Additional terms (GPL-3.0 section 7(b)): keep the attribution "OGBox by Press5elect".

	OGBox's (github.com/Press5elect/OGBox 3abbec1, OGBox by Press5elect): ssaa.frag's down-sampling
	(Soft, the even average of every texel a screen pixel covers; Sharp, Lanczos-2 as wide as two
	screen pixels) and its SMAA 1x passes (the three entry points over SMAA.hlsl, iryoku/smaa, MIT,
	third_party/smaa), compiled here as the title's other shaders are. As OGBox does it: a picture
	larger than its place on the screen is brought down to that size first, SMAA runs over that, and
	the result is shown 1:1; a picture no larger is SMAA'd at its own size, and FSR or the quad
	scales it as before. Every pass renders into an image of its own at the work size, cleared, in
	render passes whose dependencies order them after the frame before's reads.
*/
#include "ps5_aa.h"
#include "ps5_diag.h"
#include "types.h"
#include "cfg/option.h"
#include "rend/vulkan/compiler.h"
#include "rend/vulkan/texture.h"
#include "rend/vulkan/utils.h"
#include "rend/vulkan/vulkan_context.h"

#include "third_party/smaa/AreaTex.h"
#include "third_party/smaa/SearchTex.h"

#include <array>
#include <map>
#include <memory>
#include <vector>

namespace ps5::aa
{
config::Option<int> Downsampling("Downsampling", 0, "ps5");
config::Option<bool> Smaa("SMAA", false, "ps5");

namespace
{
const char SmaaSource[] =
#include "third_party/smaa/SMAA.hlsl.inc"
;

const char Vertex[] = R"(
layout (location = 0) out vec2 v_uv;
void main()
{
	v_uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	gl_Position = vec4(v_uv * 2.0 - 1.0, 0.0, 1.0);
}
)";

// OGBox's ssaa.frag (OGBox by Press5elect).
const char Ssaa[] = R"(
layout (set = 0, binding = 0) uniform sampler2D u_tex;
layout (push_constant) uniform PC { vec4 metrics; } pc;   // .x 1: sharp; the viewport's w, h in .zw
layout (location = 0) in vec2 v_uv;
layout (location = 0) out vec4 o;

const float PI = 3.14159265;

float lanczos2(float x)
{
	x = abs(x);
	if (x < 1e-4)
		return 1.0;
	if (x >= 2.0)
		return 0.0;
	float px = PI * x;
	return 2.0 * sin(px) * sin(px * 0.5) / (px * px);
}

void main()
{
	vec2 tex = vec2(textureSize(u_tex, 0));
	vec2 ratio = max(tex / max(pc.metrics.zw, vec2(1.0)), vec2(1.0));
	if (pc.metrics.x > 0.5) {
		vec2 c = v_uv * tex - 0.5;
		vec2 reach = min(2.0 * ratio, vec2(6.0));
		ivec2 lo = ivec2(floor(c - reach)) + 1, hi = ivec2(floor(c + reach));
		ivec2 size = ivec2(tex) - 1;
		vec3 sum = vec3(0.0);
		float wsum = 0.0;
		for (int y = lo.y; y <= hi.y; y++) {
			float wy = lanczos2((float(y) - c.y) / ratio.y);
			if (wy == 0.0)
				continue;
			for (int x = lo.x; x <= hi.x; x++) {
				float w = wy * lanczos2((float(x) - c.x) / ratio.x);
				sum += w * texelFetch(u_tex, clamp(ivec2(x, y), ivec2(0), size), 0).rgb;
				wsum += w;
			}
		}
		o = vec4(clamp(sum / max(wsum, 1e-4), 0.0, 1.0), 1.0);
		return;
	}
	ivec2 n = min(ivec2(ceil(ratio * 0.5)), ivec2(4));
	vec2 step = ratio / vec2(n) / tex;
	vec2 start = v_uv - 0.5 * ratio / tex + 0.5 * step;
	vec3 sum = vec3(0.0);
	for (int y = 0; y < n.y; y++)
		for (int x = 0; x < n.x; x++)
			sum += textureLod(u_tex, start + step * vec2(x, y), 0.0).rgb;
	o = vec4(sum / float(n.x * n.y), 1.0);
}
)";

// OGBox's three SMAA passes' entry points (OGBox by Press5elect), over SMAA.hlsl.
const char SmaaTop[] = R"(
layout (push_constant) uniform PC { vec4 metrics; } pc;   // 1/w, 1/h, w, h of the picture
#define SMAA_RT_METRICS pc.metrics
#define SMAA_GLSL_4
#define SMAA_PRESET_HIGH
)";
const char EdgeVs[] = R"(
layout (location = 0) out vec2 v_uv;
layout (location = 1) out vec4 v_off[3];
void main()
{
	v_uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	gl_Position = vec4(v_uv * 2.0 - 1.0, 0.0, 1.0);
	SMAAEdgeDetectionVS(v_uv, v_off);
}
)";
const char EdgeFs[] = R"(
layout (set = 0, binding = 0) uniform sampler2D u_color;
layout (location = 0) in vec2 v_uv;
layout (location = 1) in vec4 v_off[3];
layout (location = 0) out vec4 o;
void main()
{
	o = vec4(SMAALumaEdgeDetectionPS(v_uv, v_off, u_color), 0.0, 0.0);
}
)";
const char WeightVs[] = R"(
layout (location = 0) out vec2 v_uv;
layout (location = 1) out vec2 v_pix;
layout (location = 2) out vec4 v_off[3];
void main()
{
	v_uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	gl_Position = vec4(v_uv * 2.0 - 1.0, 0.0, 1.0);
	SMAABlendingWeightCalculationVS(v_uv, v_pix, v_off);
}
)";
const char WeightFs[] = R"(
layout (set = 0, binding = 0) uniform sampler2D u_edges;
layout (set = 0, binding = 1) uniform sampler2D u_area;
layout (set = 0, binding = 2) uniform sampler2D u_search;
layout (location = 0) in vec2 v_uv;
layout (location = 1) in vec2 v_pix;
layout (location = 2) in vec4 v_off[3];
layout (location = 0) out vec4 o;
void main()
{
	o = SMAABlendingWeightCalculationPS(v_uv, v_pix, v_off, u_edges, u_area, u_search, vec4(0.0));
}
)";
const char BlendVs[] = R"(
layout (location = 0) out vec2 v_uv;
layout (location = 1) out vec4 v_off;
void main()
{
	v_uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	gl_Position = vec4(v_uv * 2.0 - 1.0, 0.0, 1.0);
	SMAANeighborhoodBlendingVS(v_uv, v_off);
}
)";
const char BlendFs[] = R"(
layout (set = 0, binding = 0) uniform sampler2D u_color;
layout (set = 0, binding = 1) uniform sampler2D u_blend;
layout (location = 0) in vec2 v_uv;
layout (location = 1) in vec4 v_off;
layout (location = 0) out vec4 o;
void main()
{
	o = SMAANeighborhoodBlendingPS(v_uv, v_off, u_color, u_blend);
}
)";

VulkanContext *context()
{
	return VulkanContext::Instance();
}

constexpr vk::Format Rgba = vk::Format::eR8G8B8A8Unorm, Rg = vk::Format::eR8G8Unorm;

struct Target
{
	std::unique_ptr<FramebufferAttachment> image;
	vk::UniqueFramebuffer framebuffer;
};

// The descriptor sets that read one source picture.
struct SourceSets
{
	vk::UniqueDescriptorSet ssaa, edge, blend;
	vk::ImageView blendFrom;	// what edge and blend read (the source, or the down-sampled image)
};

struct State
{
	vk::UniqueDescriptorSetLayout setLayout;
	vk::UniqueSampler linear, point;
	vk::UniquePipelineLayout layout;
	vk::UniqueRenderPass rgbaPass, rgPass;
	vk::UniquePipeline ssaa, edge, weight, blend;
	std::unique_ptr<Texture> area, search;
	vk::UniqueDescriptorSet weightSet;
	// The work images, at the work size.
	vk::Extent2D downSize{}, smaaSize{};
	Target down, edges, weights, out;
	std::map<VkImageView, SourceSets> sets;
	// What the last apply made, for a picture presented again.
	vk::ImageView lastSource, lastResult;
	vk::Extent2D lastExtent{}, lastResultExtent{};
	int lastMode = -1;
};
std::unique_ptr<State> state;
bool failed;

vk::UniqueShaderModule compile(vk::ShaderStageFlagBits stage, std::initializer_list<const char *> parts)
{
	VulkanSource source;
	for (const char *part : parts)
		source.addSource(part);
	vk::UniqueShaderModule module = ShaderCompiler::TryCompile(stage, source.generate());
	if (!module)
		throw std::runtime_error("a shader did not compile");
	return module;
}

vk::UniqueRenderPass makePass(vk::Format format)
{
	vk::AttachmentDescription attachment(vk::AttachmentDescriptionFlags(), format, vk::SampleCountFlagBits::e1,
			vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore, vk::AttachmentLoadOp::eDontCare,
			vk::AttachmentStoreOp::eDontCare, vk::ImageLayout::eUndefined, vk::ImageLayout::eShaderReadOnlyOptimal);
	vk::AttachmentReference colour(0, vk::ImageLayout::eColorAttachmentOptimal);
	vk::SubpassDescription subpass(vk::SubpassDescriptionFlags(), vk::PipelineBindPoint::eGraphics, nullptr, colour);
	std::array<vk::SubpassDependency, 2> dependencies = {
		vk::SubpassDependency(vk::SubpassExternal, 0,
				vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eColorAttachmentOutput,
				vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eColorAttachmentOutput,
				vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eColorAttachmentWrite,
				vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eColorAttachmentWrite),
		vk::SubpassDependency(0, vk::SubpassExternal, vk::PipelineStageFlagBits::eColorAttachmentOutput,
				vk::PipelineStageFlagBits::eFragmentShader, vk::AccessFlagBits::eColorAttachmentWrite,
				vk::AccessFlagBits::eShaderRead),
	};
	return context()->GetDevice().createRenderPassUnique(vk::RenderPassCreateInfo(vk::RenderPassCreateFlags(), attachment,
			subpass, dependencies));
}

vk::UniquePipeline makePipeline(vk::ShaderModule vs, vk::ShaderModule fs, vk::RenderPass pass)
{
	State& s = *state;
	vk::PipelineVertexInputStateCreateInfo vertexInput;
	vk::PipelineInputAssemblyStateCreateInfo inputAssembly(vk::PipelineInputAssemblyStateCreateFlags(), vk::PrimitiveTopology::eTriangleList);
	vk::PipelineViewportStateCreateInfo viewport(vk::PipelineViewportStateCreateFlags(), 1, nullptr, 1, nullptr);
	vk::PipelineRasterizationStateCreateInfo rasterization;
	rasterization.lineWidth = 1.f;
	vk::PipelineMultisampleStateCreateInfo multisample;
	vk::PipelineDepthStencilStateCreateInfo depthStencil;
	vk::PipelineColorBlendAttachmentState blendAttachment;
	blendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG
			| vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
	vk::PipelineColorBlendStateCreateInfo blend(vk::PipelineColorBlendStateCreateFlags(), false, vk::LogicOp::eNoOp, blendAttachment);
	std::array<vk::DynamicState, 2> dynamicStates = { vk::DynamicState::eViewport, vk::DynamicState::eScissor };
	vk::PipelineDynamicStateCreateInfo dynamic(vk::PipelineDynamicStateCreateFlags(), dynamicStates);
	std::array<vk::PipelineShaderStageCreateInfo, 2> stages = {
		vk::PipelineShaderStageCreateInfo(vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eVertex, vs, "main"),
		vk::PipelineShaderStageCreateInfo(vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eFragment, fs, "main"),
	};
	vk::GraphicsPipelineCreateInfo info(vk::PipelineCreateFlags(), stages, &vertexInput, &inputAssembly, nullptr, &viewport,
			&rasterization, &multisample, &depthStencil, &blend, &dynamic, *s.layout, pass, 0);
	return context()->GetDevice().createGraphicsPipelineUnique(context()->GetPipelineCache(), info).value;
}

// SMAA's tables, widened to RGBA8 (the texture cache's format; SMAA reads .rg and .r).
std::unique_ptr<Texture> table(vk::CommandBuffer cmd, const unsigned char *bytes, int w, int h, int channels)
{
	std::vector<u8> rgba((size_t)w * h * 4, 0);
	for (int i = 0; i < w * h; i++)
		for (int c = 0; c < channels; c++)
			rgba[(size_t)i * 4 + c] = bytes[(size_t)i * channels + c];
	auto texture = std::make_unique<Texture>();
	texture->tex_type = TextureType::_8888;
	texture->SetCommandBuffer(cmd);
	texture->UploadToGPU(w, h, rgba.data(), false);
	texture->SetCommandBuffer(nullptr);
	return texture;
}

vk::UniqueDescriptorSet allocate()
{
	return std::move(context()->GetDevice().allocateDescriptorSetsUnique(
			vk::DescriptorSetAllocateInfo(context()->GetDescriptorPool(), state->setLayout.get())).front());
}

void write(vk::DescriptorSet set, uint32_t binding, vk::Sampler sampler, vk::ImageView view)
{
	vk::DescriptorImageInfo image(sampler, view, vk::ImageLayout::eShaderReadOnlyOptimal);
	vk::WriteDescriptorSet w(set, binding, 0, vk::DescriptorType::eCombinedImageSampler, image);
	context()->GetDevice().updateDescriptorSets(w, nullptr);
}

void makeState(vk::CommandBuffer cmd)
{
	const vk::Device device = context()->GetDevice();
	state = std::make_unique<State>();
	State& s = *state;
	std::array<vk::DescriptorSetLayoutBinding, 3> bindings;
	for (uint32_t i = 0; i < 3; i++)
		bindings[i] = vk::DescriptorSetLayoutBinding(i, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment);
	s.setLayout = device.createDescriptorSetLayoutUnique(vk::DescriptorSetLayoutCreateInfo(vk::DescriptorSetLayoutCreateFlags(), bindings));
	auto sampler = [&](vk::Filter filter) {
		return device.createSamplerUnique(vk::SamplerCreateInfo(vk::SamplerCreateFlags(), filter, filter,
				vk::SamplerMipmapMode::eNearest, vk::SamplerAddressMode::eClampToEdge, vk::SamplerAddressMode::eClampToEdge,
				vk::SamplerAddressMode::eClampToEdge, 0.f, false, 1.f, false, vk::CompareOp::eNever, 0.f, 0.f,
				vk::BorderColor::eFloatOpaqueBlack));
	};
	s.linear = sampler(vk::Filter::eLinear);
	s.point = sampler(vk::Filter::eNearest);
	vk::PushConstantRange range(vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, 16);
	s.layout = device.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo(vk::PipelineLayoutCreateFlags(), s.setLayout.get(), range));
	s.rgbaPass = makePass(Rgba);
	s.rgPass = makePass(Rg);
	const vk::UniqueShaderModule vertex = compile(vk::ShaderStageFlagBits::eVertex, { Vertex });
	const vk::UniqueShaderModule ssaa = compile(vk::ShaderStageFlagBits::eFragment, { Ssaa });
	s.ssaa = makePipeline(*vertex, *ssaa, *s.rgbaPass);
	const char *const vsOnly = "#define SMAA_INCLUDE_PS 0\n";
	const char *const psOnly = "#define SMAA_INCLUDE_VS 0\n";
	const vk::UniqueShaderModule edgeVs = compile(vk::ShaderStageFlagBits::eVertex, { SmaaTop, vsOnly, SmaaSource, EdgeVs });
	const vk::UniqueShaderModule edgeFs = compile(vk::ShaderStageFlagBits::eFragment, { SmaaTop, psOnly, SmaaSource, EdgeFs });
	const vk::UniqueShaderModule weightVs = compile(vk::ShaderStageFlagBits::eVertex, { SmaaTop, vsOnly, SmaaSource, WeightVs });
	const vk::UniqueShaderModule weightFs = compile(vk::ShaderStageFlagBits::eFragment, { SmaaTop, psOnly, SmaaSource, WeightFs });
	const vk::UniqueShaderModule blendVs = compile(vk::ShaderStageFlagBits::eVertex, { SmaaTop, vsOnly, SmaaSource, BlendVs });
	const vk::UniqueShaderModule blendFs = compile(vk::ShaderStageFlagBits::eFragment, { SmaaTop, psOnly, SmaaSource, BlendFs });
	s.edge = makePipeline(*edgeVs, *edgeFs, *s.rgPass);
	s.weight = makePipeline(*weightVs, *weightFs, *s.rgbaPass);
	s.blend = makePipeline(*blendVs, *blendFs, *s.rgbaPass);
	s.area = table(cmd, areaTexBytes, AREATEX_WIDTH, AREATEX_HEIGHT, 2);
	s.search = table(cmd, searchTexBytes, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 1);
	ps5::diag::mark("aa: ready (SSAA's down-sampling and SMAA 1x)");
}

void makeTarget(Target& t, vk::Format format, vk::RenderPass pass, vk::Extent2D size, const char *name)
{
	t.framebuffer.reset();
	t.image = std::make_unique<FramebufferAttachment>(context()->GetPhysicalDevice(), context()->GetDevice());
	t.image->Init(size.width, size.height, format, vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled, name);
	const vk::ImageView view = t.image->GetImageView();
	t.framebuffer = context()->GetDevice().createFramebufferUnique(vk::FramebufferCreateInfo(vk::FramebufferCreateFlags(), pass,
			view, size.width, size.height, 1));
}

// The work images at their sizes; made again (after the frames in flight) when a size changes.
void makeTargets(vk::Extent2D downSize, vk::Extent2D smaaSize)
{
	State& s = *state;
	if (downSize != s.downSize || smaaSize != s.smaaSize)
		context()->WaitIdle();
	if (downSize != s.downSize)
	{
		s.down = Target();
		if (downSize.width != 0)
			makeTarget(s.down, Rgba, *s.rgbaPass, downSize, "SSAA down-sampled");
		s.downSize = downSize;
		s.sets.clear();
		ps5::diag::mark("aa: down-sampling to %u x %u", downSize.width, downSize.height);
	}
	if (smaaSize != s.smaaSize)
	{
		s.edges = Target();
		s.weights = Target();
		s.out = Target();
		s.weightSet.reset();
		if (smaaSize.width != 0)
		{
			makeTarget(s.edges, Rg, *s.rgPass, smaaSize, "SMAA edges");
			makeTarget(s.weights, Rgba, *s.rgbaPass, smaaSize, "SMAA weights");
			makeTarget(s.out, Rgba, *s.rgbaPass, smaaSize, "SMAA result");
			s.weightSet = allocate();
			write(*s.weightSet, 0, *s.linear, s.edges.image->GetImageView());
			write(*s.weightSet, 1, *s.linear, s.area->GetReadOnlyImageView());
			write(*s.weightSet, 2, *s.point, s.search->GetReadOnlyImageView());
		}
		s.smaaSize = smaaSize;
		s.sets.clear();
		ps5::diag::mark("aa: SMAA at %u x %u", smaaSize.width, smaaSize.height);
	}
}

SourceSets& setsFor(vk::ImageView view)
{
	State& s = *state;
	auto it = s.sets.find((VkImageView)view);
	if (it != s.sets.end())
		return it->second;
	if (s.sets.size() > 24)
	{
		// Views of frames long gone: frames in flight may still read their sets.
		context()->WaitIdle();
		s.sets.clear();
	}
	SourceSets sets;
	const vk::ImageView from = s.downSize.width != 0 ? s.down.image->GetImageView() : view;
	if (s.downSize.width != 0)
	{
		sets.ssaa = allocate();
		write(*sets.ssaa, 0, *s.linear, view);
	}
	if (s.smaaSize.width != 0)
	{
		sets.edge = allocate();
		write(*sets.edge, 0, *s.linear, from);
		sets.blend = allocate();
		write(*sets.blend, 0, *s.linear, from);
		write(*sets.blend, 1, *s.linear, s.weights.image->GetImageView());
	}
	sets.blendFrom = from;
	return s.sets.emplace((VkImageView)view, std::move(sets)).first->second;
}

void pass(vk::CommandBuffer cmd, vk::RenderPass renderPass, const Target& target, vk::Extent2D size, vk::Pipeline pipeline,
		vk::DescriptorSet set, const float metrics[4])
{
	State& s = *state;
	const vk::ClearValue nothing(vk::ClearColorValue(std::array<float, 4>{ 0.f, 0.f, 0.f, 0.f }));
	cmd.beginRenderPass(vk::RenderPassBeginInfo(renderPass, *target.framebuffer, vk::Rect2D({ 0, 0 }, size), nothing),
			vk::SubpassContents::eInline);
	cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
	cmd.setViewport(0, vk::Viewport(0.f, 0.f, (float)size.width, (float)size.height, 0.f, 1.f));
	cmd.setScissor(0, vk::Rect2D({ 0, 0 }, size));
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *s.layout, 0, set, nullptr);
	cmd.pushConstants(*s.layout, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, 16, metrics);
	cmd.draw(3, 1, 0, 0);
	cmd.endRenderPass();
}
} // namespace

vk::ImageView apply(vk::CommandBuffer cmd, bool fresh, vk::ImageView view, vk::Extent2D& extent, const vk::Rect2D& region)
{
	const int downMode = std::clamp((int)Downsampling, 0, 2);
	const bool smaa = Smaa;
	if (failed || config::Rotate90 || !view || extent.width == 0 || extent.height == 0 || region.extent.width == 0
			|| region.extent.height == 0 || (downMode == 0 && !smaa))
		return view;
	const bool down = downMode != 0 && (extent.width > region.extent.width || extent.height > region.extent.height);
	if (!down && !smaa)
		return view;
	const vk::Extent2D downSize = down ? region.extent : vk::Extent2D();
	const vk::Extent2D work = down ? region.extent : extent;
	const vk::Extent2D smaaSize = smaa ? work : vk::Extent2D();
	const int mode = downMode * 2 + (smaa ? 1 : 0);
	try {
		if (!state)
			makeState(cmd);
		makeTargets(downSize, smaaSize);
		State& s = *state;
		if (!fresh && s.lastSource == view && s.lastExtent == extent && s.lastMode == mode && s.lastResult)
		{
			// The same picture, presented again.
			extent = s.lastResultExtent;
			return s.lastResult;
		}
		SourceSets& sets = setsFor(view);
		if (down)
		{
			const float metrics[4] = { downMode == 2 ? 1.f : 0.f, 0.f, (float)work.width, (float)work.height };
			pass(cmd, *s.rgbaPass, s.down, work, *s.ssaa, *sets.ssaa, metrics);
		}
		vk::ImageView result = down ? s.down.image->GetImageView() : view;
		if (smaa)
		{
			const float metrics[4] = { 1.f / work.width, 1.f / work.height, (float)work.width, (float)work.height };
			pass(cmd, *s.rgPass, s.edges, work, *s.edge, *sets.edge, metrics);
			pass(cmd, *s.rgbaPass, s.weights, work, *s.weight, *s.weightSet, metrics);
			pass(cmd, *s.rgbaPass, s.out, work, *s.blend, *sets.blend, metrics);
			result = s.out.image->GetImageView();
		}
		if (s.lastMode != mode)
			ps5::diag::mark("aa: %s%s on %u x %u, shown at %u x %u", down ? (downMode == 2 ? "Sharp down-sampling" : "Soft down-sampling") : "",
					smaa ? (down ? " and SMAA" : "SMAA") : "", extent.width, extent.height, work.width, work.height);
		s.lastSource = view;
		s.lastExtent = extent;
		s.lastMode = mode;
		s.lastResult = result;
		s.lastResultExtent = work;
		extent = work;
		return result;
	} catch (const std::exception& e) {
		failed = true;
		state.reset();
		ps5::diag::mark("aa: failed (%s): the picture as it is", e.what());
		return view;
	}
}

void reset()
{
	state.reset();
	failed = false;
}
}
