/*
	PSFlyCast - HDR output (ps5_hdr.h).

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-3.0-or-later
	Additional terms (GPL-3.0 section 7(b)): keep the attribution "OGBox by Press5elect".

	The fragment shader is OGBox's shaders/glsl/ogbox/hdr.frag (github.com/Press5elect/OGBox
	3abbec1, OGBox by Press5elect), its maths as they are, the layers always read through UNORM
	views (OGBox's metrics.z 0). OGBox took its PQ and BT.2020 steps from VLC for PS5's ui_pq.frag.
*/
#include "ps5_hdr.h"
#include "ps5_diag.h"
#include "types.h"
#include "rend/vulkan/compiler.h"
#include "rend/vulkan/texture.h"
#include "rend/vulkan/utils.h"
#include "rend/vulkan/vulkan_context.h"

#include <array>
#include <map>
#include <memory>

// The driver's (shell/ps5/mesa/0007).
extern "C" int wsi_videoout_set_hdr10(int enable);
extern "C" int wsi_videoout_output_range(void);

namespace ps5::hdr
{
config::Option<bool> Enabled("HDR", false, "ps5");
config::Option<int> White("HDRWhite", 203, "ps5");
config::Option<int> Peak("HDRPeak", 1000, "ps5");

namespace
{
const char Vertex[] = R"(
layout (location = 0) out vec2 v_uv;
void main()
{
	vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	v_uv = p;
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// OGBox's hdr.frag (OGBox by Press5elect), the sRGB-view branch left out.
const char Fragment[] = R"(
layout (set = 0, binding = 0) uniform sampler2D u_picture;
layout (set = 0, binding = 1) uniform sampler2D u_ui;     // premultiplied (ImGui's blending over clear 0)
layout (push_constant) uniform PC { vec4 metrics; vec4 cal; } pc;
layout (location = 0) in vec2 v_uv;
layout (location = 0) out vec4 o;

const float M1 = 0.1593017578125, M2 = 78.84375;
const float C1 = 0.8359375, C2 = 18.8515625, C3 = 18.6875;
const float KNEE = 0.5;

const mat3 BT709_TO_2020 = mat3(
	0.6274, 0.0691, 0.0164,
	0.3293, 0.9195, 0.0880,
	0.0433, 0.0114, 0.8956);

vec3 pq(vec3 nits)
{
	vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(M1));
	return pow((C1 + C2 * y) / (1.0 + C3 * y), vec3(M2));
}

vec3 decode(vec3 c)
{
	return pow(max(c, vec3(0.0)), vec3(2.2));
}

bool pattern(vec2 p, vec2 s, out float nits)
{
	vec2 c = s * 0.5;
	if (pc.cal.x < 1.5) {
		float side = 0.40 * s.y;
		vec2 d = (p - c) / side;
		if (max(abs(d.x), abs(d.y)) >= 0.5)
			return false;
		bool x = max(abs(d.x), abs(d.y)) < 0.3 && min(abs(d.x - d.y), abs(d.x + d.y)) < 0.07;
		nits = x ? pc.cal.y : 10000.0;
		return true;
	}
	vec2 size = vec2(0.64 * s.x, 0.40 * s.y);
	vec2 d = (p - (c - size * 0.5)) / size;
	if (any(lessThan(d, vec2(0.0))) || any(greaterThanEqual(d, vec2(1.0))))
		return false;
	float level = 1.0;
	if (d.y < 0.42) {
		float k = d.x * 11.0;
		level = fract(k) < 0.06 || d.y > 0.38 ? 0.0 : floor(k) / 10.0;
	}
	nits = pow(level, 2.2) * pc.cal.y;
	return true;
}

void main()
{
	ivec2 at = ivec2(gl_FragCoord.xy);
	float test;
	if (pc.cal.x > 0.5 && pattern(gl_FragCoord.xy, vec2(textureSize(u_ui, 0)), test)) {
		o = vec4(pq(vec3(test)), 1.0);
		return;
	}
	vec3 picture = texelFetch(u_picture, at, 0).rgb;
	vec4 ui = texelFetch(u_ui, at, 0);
	float white = max(pc.metrics.x, 1.0);
	float gain = 1.0;
	if (pc.metrics.w > 0.5 && pc.metrics.y > white) {
		float l = dot(decode(picture), vec3(0.2126, 0.7152, 0.0722));
		if (l > KNEE) {
			float t = (min(l, 1.0) - KNEE) / (1.0 - KNEE);
			gain = (l + (pc.metrics.y / white - 1.0) * t * t) / l;
		}
	}
	vec3 sdr = ui.rgb + picture * (1.0 - ui.a);
	vec3 light = decode(sdr) * mix(gain, 1.0, ui.a);
	o = vec4(pq(max(BT709_TO_2020 * light, vec3(0.0)) * white), 1.0);
}
)";

struct Push
{
	float metrics[4];
	float cal[4];
};

struct State
{
	u32 width = 0, height = 0;
	vk::UniqueDescriptorSetLayout setLayout;
	vk::UniqueSampler sampler;
	vk::UniquePipelineLayout layout;
	vk::UniqueShaderModule vertex, fragment;
	vk::UniqueRenderPass hdrPass;	// into the A2R10G10B10 image
	vk::UniqueRenderPass keepPass;	// on the framebuffer, keeping the copy
	vk::UniquePipeline pipeline;
	std::unique_ptr<FramebufferAttachment> picture, menus, out;
	vk::UniqueFramebuffer pictureFb, menusFb, outFb;
	vk::UniqueDescriptorSet set;
	std::map<VkImageView, vk::UniqueFramebuffer> keepFbs;
	vk::RenderPass layerPass;		// the swapchain's, the layers' framebuffers are made with
};
std::unique_ptr<State> state;

bool on;				// the framebuffers are HDR10
bool refused;			// the driver or VideoOut said no: not asked again until the setting changes
bool lastWanted;
int range = -1;			// the output: 1 SDR, 2 HDR, 0 unknown, -1 not asked or no answer
int framesToAsk;
int calStep, calValue;
std::string why = "Off";

VulkanContext *context()
{
	return VulkanContext::Instance();
}

constexpr vk::Format OutFormat = vk::Format::eA2R10G10B10UnormPack32;

void makeState(vk::RenderPass renderPass, vk::Format presentFormat, u32 width, u32 height)
{
	const vk::Device device = context()->GetDevice();
	auto s = std::make_unique<State>();
	s->width = width;
	s->height = height;
	s->layerPass = renderPass;
	std::array<vk::DescriptorSetLayoutBinding, 2> bindings = {
		vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment),
		vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment),
	};
	s->setLayout = device.createDescriptorSetLayoutUnique(vk::DescriptorSetLayoutCreateInfo(vk::DescriptorSetLayoutCreateFlags(), bindings));
	s->sampler = device.createSamplerUnique(vk::SamplerCreateInfo(vk::SamplerCreateFlags(), vk::Filter::eNearest,
			vk::Filter::eNearest, vk::SamplerMipmapMode::eNearest, vk::SamplerAddressMode::eClampToEdge,
			vk::SamplerAddressMode::eClampToEdge, vk::SamplerAddressMode::eClampToEdge, 0.f, false, 1.f, false,
			vk::CompareOp::eNever, 0.f, 0.f, vk::BorderColor::eFloatOpaqueBlack));
	vk::PushConstantRange range(vk::ShaderStageFlagBits::eFragment, 0, sizeof(Push));
	s->layout = device.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo(vk::PipelineLayoutCreateFlags(), s->setLayout.get(), range));
	s->vertex = ShaderCompiler::TryCompile(vk::ShaderStageFlagBits::eVertex, VulkanSource().addSource(Vertex).generate());
	s->fragment = ShaderCompiler::TryCompile(vk::ShaderStageFlagBits::eFragment, VulkanSource().addSource(Fragment).generate());
	if (!s->vertex || !s->fragment)
		throw std::runtime_error("the HDR shader did not compile");

	// The HDR10 image: drawn whole, then copied from.
	{
		vk::AttachmentDescription attachment(vk::AttachmentDescriptionFlags(), OutFormat, vk::SampleCountFlagBits::e1,
				vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eStore, vk::AttachmentLoadOp::eDontCare,
				vk::AttachmentStoreOp::eDontCare, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferSrcOptimal);
		vk::AttachmentReference colour(0, vk::ImageLayout::eColorAttachmentOptimal);
		vk::SubpassDescription subpass(vk::SubpassDescriptionFlags(), vk::PipelineBindPoint::eGraphics, nullptr, colour);
		std::array<vk::SubpassDependency, 2> dependencies = {
			// the copy of the frame before has read it
			vk::SubpassDependency(vk::SubpassExternal, 0, vk::PipelineStageFlagBits::eTransfer,
					vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::AccessFlagBits::eTransferRead,
					vk::AccessFlagBits::eColorAttachmentWrite),
			vk::SubpassDependency(0, vk::SubpassExternal, vk::PipelineStageFlagBits::eColorAttachmentOutput,
					vk::PipelineStageFlagBits::eTransfer, vk::AccessFlagBits::eColorAttachmentWrite,
					vk::AccessFlagBits::eTransferRead),
		};
		s->hdrPass = device.createRenderPassUnique(vk::RenderPassCreateInfo(vk::RenderPassCreateFlags(), attachment, subpass, dependencies));
	}
	// The framebuffer after the copy: kept, and ended as the swapchain's pass ends it.
	{
		vk::AttachmentDescription attachment(vk::AttachmentDescriptionFlags(), presentFormat, vk::SampleCountFlagBits::e1,
				vk::AttachmentLoadOp::eLoad, vk::AttachmentStoreOp::eStore, vk::AttachmentLoadOp::eDontCare,
				vk::AttachmentStoreOp::eDontCare, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::ePresentSrcKHR);
		vk::AttachmentReference colour(0, vk::ImageLayout::eColorAttachmentOptimal);
		vk::SubpassDescription subpass(vk::SubpassDescriptionFlags(), vk::PipelineBindPoint::eGraphics, nullptr, colour);
		vk::SubpassDependency dependency(vk::SubpassExternal, 0, vk::PipelineStageFlagBits::eTransfer,
				vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::AccessFlagBits::eTransferWrite,
				vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite);
		s->keepPass = device.createRenderPassUnique(vk::RenderPassCreateInfo(vk::RenderPassCreateFlags(), attachment, subpass, dependency));
	}

	// The pipeline: a triangle over the screen, no blending.
	vk::PipelineVertexInputStateCreateInfo vertexInput;
	vk::PipelineInputAssemblyStateCreateInfo inputAssembly(vk::PipelineInputAssemblyStateCreateFlags(), vk::PrimitiveTopology::eTriangleList);
	vk::Viewport viewport(0.f, 0.f, (float)width, (float)height, 0.f, 1.f);
	vk::Rect2D scissor({ 0, 0 }, { width, height });
	vk::PipelineViewportStateCreateInfo viewportState(vk::PipelineViewportStateCreateFlags(), viewport, scissor);
	vk::PipelineRasterizationStateCreateInfo rasterization;
	rasterization.lineWidth = 1.f;
	vk::PipelineMultisampleStateCreateInfo multisample;
	vk::PipelineDepthStencilStateCreateInfo depthStencil;
	vk::PipelineColorBlendAttachmentState blendAttachment;
	blendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG
			| vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
	vk::PipelineColorBlendStateCreateInfo blend(vk::PipelineColorBlendStateCreateFlags(), false, vk::LogicOp::eNoOp, blendAttachment);
	std::array<vk::PipelineShaderStageCreateInfo, 2> stages = {
		vk::PipelineShaderStageCreateInfo(vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eVertex, *s->vertex, "main"),
		vk::PipelineShaderStageCreateInfo(vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eFragment, *s->fragment, "main"),
	};
	vk::GraphicsPipelineCreateInfo info(vk::PipelineCreateFlags(), stages, &vertexInput, &inputAssembly, nullptr, &viewportState,
			&rasterization, &multisample, &depthStencil, &blend, nullptr, *s->layout, *s->hdrPass, 0);
	s->pipeline = device.createGraphicsPipelineUnique(context()->GetPipelineCache(), info).value;

	// The two layers, in the swapchain's format and made with its render pass, and the HDR10 image.
	auto layer = [&](std::unique_ptr<FramebufferAttachment>& image, vk::UniqueFramebuffer& fb, vk::Format format,
			vk::ImageUsageFlags usage, vk::RenderPass pass, const char *name) {
		image = std::make_unique<FramebufferAttachment>(context()->GetPhysicalDevice(), device);
		image->Init(width, height, format, usage, name);
		const vk::ImageView view = image->GetImageView();
		fb = device.createFramebufferUnique(vk::FramebufferCreateInfo(vk::FramebufferCreateFlags(), pass, view, width, height, 1));
	};
	const vk::ImageUsageFlags layerUsage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled;
	layer(s->picture, s->pictureFb, presentFormat, layerUsage, renderPass, "HDR picture layer");
	layer(s->menus, s->menusFb, presentFormat, layerUsage, renderPass, "HDR menus layer");
	layer(s->out, s->outFb, OutFormat, vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc,
			*s->hdrPass, "HDR10 image");

	s->set = std::move(device.allocateDescriptorSetsUnique(
			vk::DescriptorSetAllocateInfo(context()->GetDescriptorPool(), s->setLayout.get())).front());
	std::array<vk::DescriptorImageInfo, 2> images = {
		vk::DescriptorImageInfo(*s->sampler, s->picture->GetImageView(), vk::ImageLayout::eShaderReadOnlyOptimal),
		vk::DescriptorImageInfo(*s->sampler, s->menus->GetImageView(), vk::ImageLayout::eShaderReadOnlyOptimal),
	};
	std::array<vk::WriteDescriptorSet, 2> writes = {
		vk::WriteDescriptorSet(*s->set, 0, 0, vk::DescriptorType::eCombinedImageSampler, images[0]),
		vk::WriteDescriptorSet(*s->set, 1, 0, vk::DescriptorType::eCombinedImageSampler, images[1]),
	};
	device.updateDescriptorSets(writes, nullptr);
	state = std::move(s);
	ps5::diag::mark("hdr: ready for %u x %u", width, height);
}

// The two layers from the render pass's end (present layout) to be read.
void layersToRead(vk::CommandBuffer cmd)
{
	std::array<vk::ImageMemoryBarrier, 2> barriers;
	const vk::Image images[2] = { state->picture->GetImage(), state->menus->GetImage() };
	for (int i = 0; i < 2; i++)
		barriers[i] = vk::ImageMemoryBarrier(vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eShaderRead,
				vk::ImageLayout::ePresentSrcKHR, vk::ImageLayout::eShaderReadOnlyOptimal, vk::QueueFamilyIgnored,
				vk::QueueFamilyIgnored, images[i], vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
	cmd.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eFragmentShader, {},
			nullptr, nullptr, barriers);
}
} // namespace

bool frameStart(vk::Format presentFormat, u32 width, u32 height)
{
	const bool wanted = Enabled;
	if (wanted != lastWanted)
	{
		lastWanted = wanted;
		refused = false;
		framesToAsk = 0;
	}
	if (wanted && framesToAsk-- <= 0)
	{
		// What the console sends: asked when HDR is turned on, and every 30 s or so while it is
		// (the TV, or the console's own HDR setting, can change).
		framesToAsk = 1800;
		const int now = wsi_videoout_output_range();
		if (now != range)
			ps5::diag::mark("hdr: the console's output is %s", now == 2 ? "HDR" : now == 1 ? "SDR" : "not known");
		range = now;
	}
	const bool unorm = presentFormat == vk::Format::eB8G8R8A8Unorm || presentFormat == vk::Format::eR8G8B8A8Unorm;
	const bool want = wanted && !refused && range == 2 && unorm;
	why = !wanted ? "Off" : refused ? "Refused by the console" : range != 2 ? "The console sends SDR"
			: !unorm ? "Not with this display's format" : "On";
	if (want != on)
	{
		// The frames drawn so far are shown as they were drawn, then the framebuffers change.
		context()->WaitIdle();
		const int rc = wsi_videoout_set_hdr10(want ? 1 : 0);
		ps5::diag::mark("hdr: framebuffers %s: %s (%#x)", want ? "HDR10" : "SDR", rc == 0 ? "taken" : "refused", (unsigned)rc);
		if (rc == 0)
			on = want;
		else if (want)
		{
			refused = true;
			why = "Refused by the console";
		}
	}
	if (on && (!state || state->width != width || state->height != height || state->layerPass != context()->GetRenderPass()))
	{
		try {
			state.reset();
			makeState(context()->GetRenderPass(), presentFormat, width, height);
		} catch (const std::exception& e) {
			state.reset();
			ps5::diag::mark("hdr: could not be set up (%s): SDR", e.what());
			refused = true;
			context()->WaitIdle();
			if (wsi_videoout_set_hdr10(0) == 0)
				on = false;
		}
	}
	return on && state;
}

void beginPicture(vk::CommandBuffer cmd, vk::RenderPass renderPass, u32 width, u32 height, const vk::ClearValue& border)
{
	// The frame before has read the layers (fragment shader) before they are drawn again.
	cmd.pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader, vk::PipelineStageFlagBits::eColorAttachmentOutput, {},
			nullptr, nullptr, nullptr);
	cmd.beginRenderPass(vk::RenderPassBeginInfo(renderPass, *state->pictureFb, vk::Rect2D({ 0, 0 }, { width, height }), border),
			vk::SubpassContents::eInline);
}

void beginMenus(vk::CommandBuffer cmd, vk::RenderPass renderPass, u32 width, u32 height)
{
	cmd.endRenderPass();
	const vk::ClearValue nothing(vk::ClearColorValue(std::array<float, 4>{ 0.f, 0.f, 0.f, 0.f }));
	cmd.beginRenderPass(vk::RenderPassBeginInfo(renderPass, *state->menusFb, vk::Rect2D({ 0, 0 }, { width, height }), nothing),
			vk::SubpassContents::eInline);
}

void finish(vk::CommandBuffer cmd, vk::Image swapImage, vk::ImageView swapView, vk::Format presentFormat, u32 width, u32 height)
{
	State& s = *state;
	cmd.endRenderPass();
	layersToRead(cmd);
	cmd.beginRenderPass(vk::RenderPassBeginInfo(*s.hdrPass, *s.outFb, vk::Rect2D({ 0, 0 }, { width, height })),
			vk::SubpassContents::eInline);
	cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *s.pipeline);
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *s.layout, 0, *s.set, nullptr);
	Push push{};
	push.metrics[0] = (float)std::clamp((int)White, 80, 400);
	push.metrics[1] = (float)std::clamp((int)Peak, 0, 4000);
	push.metrics[3] = 1.f;
	push.cal[0] = (float)calStep;
	push.cal[1] = (float)calValue;
	cmd.pushConstants(*s.layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(push), &push);
	cmd.draw(3, 1, 0, 0);
	cmd.endRenderPass();

	// Into the framebuffer, bit for bit: its pixels are read as 10:10:10:2 (the driver retyped them).
	const vk::ImageSubresourceRange whole(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
	vk::ImageMemoryBarrier toCopy({}, vk::AccessFlagBits::eTransferWrite, vk::ImageLayout::eUndefined,
			vk::ImageLayout::eTransferDstOptimal, vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, swapImage, whole);
	cmd.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eTransfer, {}, nullptr,
			nullptr, toCopy);
	const vk::ImageSubresourceLayers layers(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
	cmd.copyImage(s.out->GetImage(), vk::ImageLayout::eTransferSrcOptimal, swapImage, vk::ImageLayout::eTransferDstOptimal,
			vk::ImageCopy(layers, { 0, 0, 0 }, layers, { 0, 0, 0 }, { width, height, 1 }));

	auto fb = s.keepFbs.find((VkImageView)swapView);
	if (fb == s.keepFbs.end())
		fb = s.keepFbs.emplace((VkImageView)swapView, context()->GetDevice().createFramebufferUnique(vk::FramebufferCreateInfo(
				vk::FramebufferCreateFlags(), *s.keepPass, swapView, width, height, 1))).first;
	(void)presentFormat;
	cmd.beginRenderPass(vk::RenderPassBeginInfo(*s.keepPass, *fb->second, vk::Rect2D({ 0, 0 }, { width, height })),
			vk::SubpassContents::eInline);
}

void setCalibration(int step, int value)
{
	calStep = step;
	calValue = value;
}

std::string status()
{
	return why;
}

void reset()
{
	state.reset();
}
}
