/*
	PSFlyCast - FSR 1 upscaling of the game's picture.

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	The emulator draws a game at its internal resolution and stretches the
	result to the screen with a bilinear filter. With "Upscaling" on FSR 1 the
	stretch is AMD's FidelityFX Super Resolution 1.0 instead (fsr/ffx_a.h and
	fsr/ffx_fsr1.h, MIT, compiled here as GLSL): EASU, an edge-adaptive
	upscale, into an image the size the picture has on the screen, then RCAS,
	a sharpening pass, from that image into the swapchain. A picture rendered
	at 1440 or 1920 lines then looks close to one rendered at 2160, for much
	less work, which is what the per-pixel transparency renderer needs at 4K.

	Two draws of one triangle that covers the target. The first is in a render
	pass of its own, before the swapchain's (upscale); the second replaces the
	emulator's quad in the swapchain's (present). When anything here fails,
	the emulator's own stretch is used: a failure is a line in the log, not a
	black screen.
*/
#include "ps5_fsr.h"
#include "ps5_fsr_constants.h"
#include "ps5_diag.h"
#include "ps5_frontend.h"
#include "cfg/option.h"
#include "rend/vulkan/compiler.h"
#include "rend/vulkan/texture.h"
#include "rend/vulkan/utils.h"
#include "rend/vulkan/vulkan_context.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

namespace ps5
{
config::Option<int> Upscaling("Upscaling", UpscalingOff, "ps5");
}

namespace ps5::fsr
{
namespace
{

// AMD's two headers, as text (ps5.cmake wraps them in a string literal).
const char FfxA[] =
#include "ffx_a.inc"
;
const char FfxFsr1[] =
#include "ffx_fsr1.inc"
;

// One triangle over the whole viewport.
const char VertexSource[] = R"(
void main()
{
	vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char EasuTop[] = R"(
layout (set = 0, binding = 0) uniform sampler2D tex;
layout (push_constant) uniform pushBlock
{
	uvec4 con0;
	uvec4 con1;
	uvec4 con2;
	uvec4 con3;
} pc;
layout (location = 0) out vec4 FragColor;

#define A_GPU 1
#define A_GLSL 1
)";

const char EasuMain[] = R"(
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return textureGather(tex, p, 0); }
AF4 FsrEasuGF(AF2 p) { return textureGather(tex, p, 1); }
AF4 FsrEasuBF(AF2 p) { return textureGather(tex, p, 2); }
)";

const char EasuBottom[] = R"(
void main()
{
	AF3 c;
	FsrEasuF(c, AU2(gl_FragCoord.xy), pc.con0, pc.con1, pc.con2, pc.con3);
	FragColor = vec4(c, 1.0);
}
)";

const char RcasTop[] = R"(
layout (set = 0, binding = 0) uniform sampler2D tex;
layout (push_constant) uniform pushBlock
{
	uvec4 con;
	ivec4 origin;		// of the picture, in the framebuffer
} pc;
layout (location = 0) out vec4 FragColor;

#define A_GPU 1
#define A_GLSL 1
)";

const char RcasMain[] = R"(
#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) { return texelFetch(tex, clamp(p, ASU2(0), textureSize(tex, 0) - ASU2(1)), 0); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
)";

const char RcasBottom[] = R"(
void main()
{
	AF3 c;
	FsrRcasF(c.r, c.g, c.b, AU2(ASU2(gl_FragCoord.xy) - pc.origin.xy), pc.con);
	FragColor = vec4(c, 1.0);
}
)";

// The sharpening, in stops: 0 is the sharpest.
float sharpnessStops()
{
	switch (ps5::Upscaling)
	{
	case UpscalingFsrSharp:
		return 0.f;
	case UpscalingFsrSoft:
		return 1.f;
	default:
		return 0.25f;
	}
}

struct State
{
	vk::UniqueRenderPass easuPass;
	vk::UniqueDescriptorSetLayout setLayout;
	vk::UniquePipelineLayout easuLayout;
	vk::UniquePipelineLayout rcasLayout;
	vk::UniqueShaderModule vertexShader;
	vk::UniqueShaderModule easuShader;
	vk::UniqueShaderModule rcasShader;
	vk::UniquePipeline easuPipeline;
	vk::UniquePipeline rcasPipeline;
	vk::RenderPass rcasRenderPass;		// the one rcasPipeline was made for
	vk::UniqueSampler sampler;
	std::unique_ptr<FramebufferAttachment> target;
	vk::UniqueFramebuffer framebuffer;
	std::vector<vk::UniqueDescriptorSet> easuSets;
	std::vector<vk::UniqueDescriptorSet> rcasSets;
	// What the target holds, upscaled.
	bool ready = false;
	vk::ImageView source;
	vk::Extent2D sourceExtent;
};

std::unique_ptr<State> state;
bool failed;		// until the next reset(): the emulator's own stretch is used

VulkanContext *context()
{
	return VulkanContext::Instance();
}

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
	return context()->GetDevice().createGraphicsPipelineUnique(context()->GetPipelineCache(), info).value;
}

// What does not depend on the picture's size. Throws when the driver or the
// shader compiler refuses something.
void makeState()
{
	if (state)
		return;
	const vk::Device device = context()->GetDevice();
	auto s = std::make_unique<State>();

	// The upscale's render pass: one colour attachment, read as a texture afterwards.
	vk::AttachmentDescription attachment(vk::AttachmentDescriptionFlags(), vk::Format::eR8G8B8A8Unorm,
			vk::SampleCountFlagBits::e1, vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eStore,
			vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
			vk::ImageLayout::eUndefined, vk::ImageLayout::eShaderReadOnlyOptimal);
	vk::AttachmentReference colorReference(0, vk::ImageLayout::eColorAttachmentOptimal);
	vk::SubpassDescription subpass(vk::SubpassDescriptionFlags(), vk::PipelineBindPoint::eGraphics, nullptr, colorReference);
	std::array<vk::SubpassDependency, 2> dependencies = {
			// The frame before read the image; this one writes it again.
			vk::SubpassDependency(vk::SubpassExternal, 0,
					vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eColorAttachmentOutput,
					vk::PipelineStageFlagBits::eColorAttachmentOutput,
					vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eColorAttachmentWrite,
					vk::AccessFlagBits::eColorAttachmentWrite),
			vk::SubpassDependency(0, vk::SubpassExternal, vk::PipelineStageFlagBits::eColorAttachmentOutput,
					vk::PipelineStageFlagBits::eFragmentShader, vk::AccessFlagBits::eColorAttachmentWrite,
					vk::AccessFlagBits::eShaderRead),
	};
	s->easuPass = device.createRenderPassUnique(vk::RenderPassCreateInfo(vk::RenderPassCreateFlags(), attachment, subpass,
			dependencies));

	vk::DescriptorSetLayoutBinding binding(0, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment);
	s->setLayout = device.createDescriptorSetLayoutUnique(
			vk::DescriptorSetLayoutCreateInfo(vk::DescriptorSetLayoutCreateFlags(), binding));
	vk::PushConstantRange easuRange(vk::ShaderStageFlagBits::eFragment, 0, sizeof(EasuConstants));
	s->easuLayout = device.createPipelineLayoutUnique(
			vk::PipelineLayoutCreateInfo(vk::PipelineLayoutCreateFlags(), s->setLayout.get(), easuRange));
	vk::PushConstantRange rcasRange(vk::ShaderStageFlagBits::eFragment, 0, sizeof(RcasConstants));
	s->rcasLayout = device.createPipelineLayoutUnique(
			vk::PipelineLayoutCreateInfo(vk::PipelineLayoutCreateFlags(), s->setLayout.get(), rcasRange));

	s->sampler = device.createSamplerUnique(vk::SamplerCreateInfo(vk::SamplerCreateFlags(), vk::Filter::eLinear,
			vk::Filter::eLinear, vk::SamplerMipmapMode::eNearest, vk::SamplerAddressMode::eClampToEdge,
			vk::SamplerAddressMode::eClampToEdge, vk::SamplerAddressMode::eClampToEdge, 0.f, false, 1.f, false,
			vk::CompareOp::eNever, 0.f, 0.f, vk::BorderColor::eFloatOpaqueBlack));

	s->vertexShader = ShaderCompiler::TryCompile(vk::ShaderStageFlagBits::eVertex,
			VulkanSource().addSource(VertexSource).generate());
	s->easuShader = ShaderCompiler::TryCompile(vk::ShaderStageFlagBits::eFragment,
			VulkanSource().addSource(EasuTop).addSource(FfxA).addSource(EasuMain).addSource(FfxFsr1).addSource(EasuBottom).generate());
	s->rcasShader = ShaderCompiler::TryCompile(vk::ShaderStageFlagBits::eFragment,
			VulkanSource().addSource(RcasTop).addSource(FfxA).addSource(RcasMain).addSource(FfxFsr1).addSource(RcasBottom).generate());
	if (!s->vertexShader || !s->easuShader || !s->rcasShader)
		throw std::runtime_error("a shader did not compile");
	s->easuPipeline = makePipeline(*s->vertexShader, *s->easuShader, *s->easuLayout, *s->easuPass);
	state = std::move(s);
	ps5::diag::mark("fsr: ready");
}

// The image the picture is upscaled into, of the size it has on the screen.
void makeTarget(const vk::Extent2D& extent)
{
	State& s = *state;
	if (s.target && s.target->getExtent() == extent)
		return;
	s.ready = false;
	if (s.target)
	{
		// Frames in flight read the old one.
		context()->WaitIdle();
		s.framebuffer.reset();
		s.target.reset();
	}
	auto target = std::make_unique<FramebufferAttachment>(context()->GetPhysicalDevice(), context()->GetDevice());
	target->Init(extent.width, extent.height, vk::Format::eR8G8B8A8Unorm,
			vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled, "FSR target");
	const vk::ImageView view = target->GetImageView();
	s.framebuffer = context()->GetDevice().createFramebufferUnique(vk::FramebufferCreateInfo(vk::FramebufferCreateFlags(),
			*s.easuPass, view, extent.width, extent.height, 1));
	s.target = std::move(target);
	ps5::diag::mark("fsr: a target of %u x %u", extent.width, extent.height);
}

// This swapchain image's descriptor set of a kind, with the image to read in it.
vk::DescriptorSet descriptorSet(std::vector<vk::UniqueDescriptorSet>& sets, vk::ImageView view)
{
	const unsigned index = (unsigned)context()->GetCurrentImageIndex();
	if (index >= sets.size())
		sets.resize(index + 1);
	if (!sets[index])
		sets[index] = std::move(context()->GetDevice().allocateDescriptorSetsUnique(
				vk::DescriptorSetAllocateInfo(context()->GetDescriptorPool(), state->setLayout.get())).front());
	vk::DescriptorImageInfo imageInfo(*state->sampler, view, vk::ImageLayout::eShaderReadOnlyOptimal);
	vk::WriteDescriptorSet write(*sets[index], 0, 0, vk::DescriptorType::eCombinedImageSampler, imageInfo);
	context()->GetDevice().updateDescriptorSets(write, nullptr);
	return *sets[index];
}

void fail(const char *where, const char *what)
{
	failed = true;
	if (state)
		state->ready = false;
	ps5::diag::mark("fsr: %s failed (%s): the picture is stretched the usual way", where, what);
	WARN_LOG(RENDERER, "FSR: %s failed: %s", where, what);
}

} // namespace

void upscale(vk::CommandBuffer commandBuffer, bool fresh, vk::ImageView view, const vk::Extent2D& extent,
		const vk::Rect2D& region)
{
	const bool wanted = ps5::Upscaling != UpscalingOff && !failed && !config::Rotate90 && view
			&& extent.width > 0 && extent.height > 0
			&& extent.width < region.extent.width && extent.height < region.extent.height;
	if (!wanted)
	{
		if (state)
			state->ready = false;
		return;
	}
	vk::DescriptorSet set;
	try {
		makeState();
		makeTarget(region.extent);
		State& s = *state;
		if (!fresh && s.ready && s.source == view && s.sourceExtent == extent)
			return;		// the same picture, presented again
		set = descriptorSet(s.easuSets, view);
	} catch (const std::exception& e) {
		fail("the upscale", e.what());
		return;
	}
	State& s = *state;
	const vk::Extent2D out = region.extent;
	commandBuffer.beginRenderPass(vk::RenderPassBeginInfo(*s.easuPass, *s.framebuffer, vk::Rect2D({ 0, 0 }, out)),
			vk::SubpassContents::eInline);
	commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, *s.easuPipeline);
	commandBuffer.setViewport(0, vk::Viewport(0.f, 0.f, (float)out.width, (float)out.height, 0.f, 1.f));
	commandBuffer.setScissor(0, vk::Rect2D({ 0, 0 }, out));
	commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *s.easuLayout, 0, set, nullptr);
	const EasuConstants constants = easuConstants((float)extent.width, (float)extent.height, (float)out.width, (float)out.height);
	commandBuffer.pushConstants(*s.easuLayout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(constants), &constants);
	commandBuffer.draw(3, 1, 0, 0);
	commandBuffer.endRenderPass();
	if (!s.ready || s.sourceExtent != extent)
		ps5::diag::mark("fsr: %u x %u to %u x %u", extent.width, extent.height, out.width, out.height);
	s.ready = true;
	s.source = view;
	s.sourceExtent = extent;
}

bool present(vk::CommandBuffer commandBuffer, vk::ImageView view, vk::RenderPass renderPass, const vk::Rect2D& region,
		int shiftX, int shiftY)
{
	if (!state || !state->ready || failed || ps5::Upscaling == UpscalingOff || state->source != view
			|| !state->target || state->target->getExtent() != region.extent)
		return false;
	State& s = *state;
	vk::DescriptorSet set;
	try {
		if (!s.rcasPipeline || s.rcasRenderPass != renderPass)
		{
			s.rcasPipeline = makePipeline(*s.vertexShader, *s.rcasShader, *s.rcasLayout, renderPass);
			s.rcasRenderPass = renderPass;
		}
		set = descriptorSet(s.rcasSets, s.target->GetImageView());
	} catch (const std::exception& e) {
		fail("the sharpening", e.what());
		return false;
	}
	// The picture, moved by the game's own shift, inside its part of the screen.
	const int left = region.offset.x + shiftX, top = region.offset.y + shiftY;
	const int width = (int)region.extent.width, height = (int)region.extent.height;
	const int x0 = std::max(left, region.offset.x), y0 = std::max(top, region.offset.y);
	const int x1 = std::min(left, region.offset.x) + width, y1 = std::min(top, region.offset.y) + height;
	if (x1 <= x0 || y1 <= y0)
		return true;		// shifted out of sight
	commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, *s.rcasPipeline);
	commandBuffer.setViewport(0, vk::Viewport((float)left, (float)top, (float)width, (float)height, 0.f, 1.f));
	commandBuffer.setScissor(0, vk::Rect2D({ x0, y0 }, { (u32)(x1 - x0), (u32)(y1 - y0) }));
	commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *s.rcasLayout, 0, set, nullptr);
	const RcasConstants constants = rcasConstants(sharpnessStops(), left, top);
	commandBuffer.pushConstants(*s.rcasLayout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(constants), &constants);
	commandBuffer.draw(3, 1, 0, 0);
	return true;
}

void reset()
{
	state.reset();
	failed = false;
}

} // namespace ps5::fsr
