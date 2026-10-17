/*
	PSFlyCast - what fills the screen beside (or above and below) a game's
	picture that does not fit it. The five looks of PSSwanStation's "Beside
	the picture":

	  0  black
	  1  the picture's own light: the picture blurred wide and soft over the
	     whole screen, dimmer towards the screen's edges
	  2  a quiet gradient in the interface's accent colour
	  3  a picture of your own: borders/<game ID>.png (or .jpg), else
	     borders/default.png, in the title's folder
	  4  the picture carried on past its edges: each edge mirrored outwards,
	     blurred so that shapes stay and detail goes, darker towards the edges

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	The blur is made the way PSSwanStation makes it, by size alone with the
	linear sampler: the picture is drawn down through six small images
	(256 x 192 to 8 x 6), each from the one before, and back up. That is a
	wide, smooth blur for a handful of tiny draws and no shader of its own. It
	is made when the game shows a new picture, in render passes of its own
	before the swapchain's, like FSR's upscale (ps5_fsr.cpp); the bars are
	then drawn in the swapchain's pass, before the game's picture.
*/
#include "ps5_border.h"
#include "ps5_diag.h"
#include "ps5_frontend.h"
#include "cfg/option.h"
#include "types.h"
#include "rend/vulkan/compiler.h"
#include "rend/vulkan/texture.h"
#include "rend/vulkan/utils.h"
#include "rend/vulkan/vulkan_context.h"

#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

namespace ps5::border
{
namespace
{

constexpr int Levels = 6;
constexpr int BaseWidth = 256, BaseHeight = 192;

// One quad: where it goes (clip space), what part of the picture it shows, and how it is lit.
const char Vertex[] = R"(
layout (push_constant) uniform pushBlock
{
	vec4 rect;		// x0, y0, x1, y1 in clip space
	vec4 uv;		// u0, v0, u1, v1
	vec4 top;		// the colour (a gradient's top)
	vec4 bottom;	// a gradient's bottom
	vec4 pic;		// the game's rectangle on the screen: x0, x1, y0, y1 in pixels
	vec4 misc;		// screen width, height, how dark the screen's far edges are, 1 for no texture
} pc;
layout (location = 0) out vec2 vUv;
void main()
{
	vec2 p = vec2(gl_VertexIndex & 1, (gl_VertexIndex >> 1) & 1);
	vUv = mix(pc.uv.xy, pc.uv.zw, p);
	gl_Position = vec4(mix(pc.rect.xy, pc.rect.zw, p), 0.0, 1.0);
}
)";

const char Fragment[] = R"(
layout (set = 0, binding = 0) uniform sampler2D tex;
layout (push_constant) uniform pushBlock
{
	vec4 rect;
	vec4 uv;
	vec4 top;
	vec4 bottom;
	vec4 pic;
	vec4 misc;
} pc;
layout (location = 0) in vec2 vUv;
layout (location = 0) out vec4 FragColor;
void main()
{
	vec4 t = pc.misc.w > 0.5 ? vec4(1.0) : texture(tex, vUv);
	vec3 c = t.rgb * mix(pc.top.rgb, pc.bottom.rgb, clamp(gl_FragCoord.y / max(pc.misc.y, 1.0), 0.0, 1.0));
	float x = gl_FragCoord.x, y = gl_FragCoord.y;
	float dx = x < pc.pic.x ? (pc.pic.x - x) / max(pc.pic.x, 1.0)
			: x > pc.pic.y ? (x - pc.pic.y) / max(pc.misc.x - pc.pic.y, 1.0) : 0.0;
	float dy = y < pc.pic.z ? (pc.pic.z - y) / max(pc.pic.z, 1.0)
			: y > pc.pic.w ? (y - pc.pic.w) / max(pc.misc.y - pc.pic.w, 1.0) : 0.0;
	c *= 1.0 - pc.misc.z * clamp(max(dx, dy), 0.0, 1.0);
	FragColor = vec4(c, 1.0);
}
)";

struct Push
{
	float rect[4];
	float uv[4];
	float top[4];
	float bottom[4];
	float pic[4];
	float misc[4];
};

struct State
{
	vk::UniqueDescriptorSetLayout setLayout;
	vk::UniqueSampler sampler;
	vk::UniquePipelineLayout layout;
	vk::UniqueShaderModule vertex, fragment;
	vk::UniqueRenderPass blurPass;
	vk::UniquePipeline blurPipeline;
	vk::UniquePipeline screenPipeline;
	vk::RenderPass screenPass;
	std::array<std::unique_ptr<FramebufferAttachment>, Levels> levels;
	std::array<vk::UniqueFramebuffer, Levels> framebuffers;
	std::map<VkImageView, vk::UniqueDescriptorSet> sets;
	bool blurred = false;			// level 0 holds a blurred picture
	int blurredDepth = 0;
	vk::ImageView shown;			// this present's picture
	// The picture of your own.
	std::unique_ptr<Texture> own;
	std::string ownLoaded;
	float ownAspect = 1.f;
};
std::unique_ptr<State> state;
bool failed;
unsigned accent = 0x3a86ff;

VulkanContext *context()
{
	return VulkanContext::Instance();
}

vk::UniquePipeline makePipeline(vk::RenderPass pass)
{
	State& s = *state;
	vk::PipelineVertexInputStateCreateInfo vertexInput;
	vk::PipelineInputAssemblyStateCreateInfo inputAssembly(vk::PipelineInputAssemblyStateCreateFlags(),
			vk::PrimitiveTopology::eTriangleStrip);
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
			vk::PipelineShaderStageCreateInfo(vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eVertex, *s.vertex, "main"),
			vk::PipelineShaderStageCreateInfo(vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eFragment, *s.fragment, "main"),
	};
	vk::GraphicsPipelineCreateInfo info(vk::PipelineCreateFlags(), stages, &vertexInput, &inputAssembly, nullptr, &viewport,
			&rasterization, &multisample, &depthStencil, &blend, &dynamic, *s.layout, pass, 0);
	return context()->GetDevice().createGraphicsPipelineUnique(context()->GetPipelineCache(), info).value;
}

void makeState()
{
	if (state)
		return;
	const vk::Device device = context()->GetDevice();
	auto s = std::make_unique<State>();
	vk::DescriptorSetLayoutBinding binding(0, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment);
	s->setLayout = device.createDescriptorSetLayoutUnique(
			vk::DescriptorSetLayoutCreateInfo(vk::DescriptorSetLayoutCreateFlags(), binding));
	s->sampler = device.createSamplerUnique(vk::SamplerCreateInfo(vk::SamplerCreateFlags(), vk::Filter::eLinear,
			vk::Filter::eLinear, vk::SamplerMipmapMode::eNearest, vk::SamplerAddressMode::eClampToEdge,
			vk::SamplerAddressMode::eClampToEdge, vk::SamplerAddressMode::eClampToEdge, 0.f, false, 1.f, false,
			vk::CompareOp::eNever, 0.f, 0.f, vk::BorderColor::eFloatOpaqueBlack));
	vk::PushConstantRange range(vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, sizeof(Push));
	s->layout = device.createPipelineLayoutUnique(
			vk::PipelineLayoutCreateInfo(vk::PipelineLayoutCreateFlags(), s->setLayout.get(), range));
	s->vertex = ShaderCompiler::TryCompile(vk::ShaderStageFlagBits::eVertex, VulkanSource().addSource(Vertex).generate());
	s->fragment = ShaderCompiler::TryCompile(vk::ShaderStageFlagBits::eFragment, VulkanSource().addSource(Fragment).generate());
	if (!s->vertex || !s->fragment)
		throw std::runtime_error("a shader did not compile");

	// The blur's render pass: one colour attachment, read as a texture afterwards.
	vk::AttachmentDescription attachment(vk::AttachmentDescriptionFlags(), vk::Format::eR8G8B8A8Unorm,
			vk::SampleCountFlagBits::e1, vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eStore,
			vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
			vk::ImageLayout::eUndefined, vk::ImageLayout::eShaderReadOnlyOptimal);
	vk::AttachmentReference colorReference(0, vk::ImageLayout::eColorAttachmentOptimal);
	vk::SubpassDescription subpass(vk::SubpassDescriptionFlags(), vk::PipelineBindPoint::eGraphics, nullptr, colorReference);
	std::array<vk::SubpassDependency, 2> dependencies = {
			vk::SubpassDependency(vk::SubpassExternal, 0,
					vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eColorAttachmentOutput,
					vk::PipelineStageFlagBits::eColorAttachmentOutput,
					vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eColorAttachmentWrite,
					vk::AccessFlagBits::eColorAttachmentWrite),
			vk::SubpassDependency(0, vk::SubpassExternal, vk::PipelineStageFlagBits::eColorAttachmentOutput,
					vk::PipelineStageFlagBits::eFragmentShader, vk::AccessFlagBits::eColorAttachmentWrite,
					vk::AccessFlagBits::eShaderRead),
	};
	s->blurPass = device.createRenderPassUnique(vk::RenderPassCreateInfo(vk::RenderPassCreateFlags(), attachment, subpass,
			dependencies));
	state = std::move(s);
	state->blurPipeline = makePipeline(*state->blurPass);

	int w = BaseWidth, h = BaseHeight;
	for (int i = 0; i < Levels; i++, w /= 2, h /= 2)
	{
		auto target = std::make_unique<FramebufferAttachment>(context()->GetPhysicalDevice(), device);
		target->Init(w, h, vk::Format::eR8G8B8A8Unorm,
				vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled, "Border light " + std::to_string(i));
		const vk::ImageView view = target->GetImageView();
		state->framebuffers[i] = device.createFramebufferUnique(vk::FramebufferCreateInfo(vk::FramebufferCreateFlags(),
				*state->blurPass, view, (u32)w, (u32)h, 1));
		state->levels[i] = std::move(target);
	}
	ps5::diag::mark("border: ready");
}

void fail(const char *what)
{
	failed = true;
	ps5::diag::mark("border: failed (%s): black beside the picture", what);
	WARN_LOG(RENDERER, "Border: %s", what);
}

// The descriptor set that reads `view`.
vk::DescriptorSet setFor(vk::ImageView view)
{
	State& s = *state;
	auto it = s.sets.find((VkImageView)view);
	if (it == s.sets.end())
	{
		if (s.sets.size() > 24)
		{
			// Views of frames long gone: frames in flight may still read them.
			context()->WaitIdle();
			s.sets.clear();
		}
		auto set = std::move(context()->GetDevice().allocateDescriptorSetsUnique(
				vk::DescriptorSetAllocateInfo(context()->GetDescriptorPool(), s.setLayout.get())).front());
		vk::DescriptorImageInfo imageInfo(*s.sampler, view, vk::ImageLayout::eShaderReadOnlyOptimal);
		vk::WriteDescriptorSet write(*set, 0, 0, vk::DescriptorType::eCombinedImageSampler, imageInfo);
		context()->GetDevice().updateDescriptorSets(write, nullptr);
		it = s.sets.emplace((VkImageView)view, std::move(set)).first;
	}
	return *it->second;
}

void colour(float out[4], unsigned rgb, float scale = 1.f)
{
	out[0] = ((rgb >> 16) & 255) / 255.f * scale;
	out[1] = ((rgb >> 8) & 255) / 255.f * scale;
	out[2] = (rgb & 255) / 255.f * scale;
	out[3] = 1.f;
}

Push plain()
{
	Push p{};
	const float rect[4] = { -1.f, -1.f, 1.f, 1.f }, uv[4] = { 0.f, 0.f, 1.f, 1.f };
	memcpy(p.rect, rect, sizeof(rect));
	memcpy(p.uv, uv, sizeof(uv));
	for (int i = 0; i < 4; i++)
		p.top[i] = p.bottom[i] = 1.f;
	return p;
}

// One draw into a level of the blur: `from` read, whole.
void blit(vk::CommandBuffer cmd, int level, vk::ImageView from)
{
	State& s = *state;
	const vk::Extent2D extent = s.levels[level]->getExtent();
	cmd.beginRenderPass(vk::RenderPassBeginInfo(*s.blurPass, *s.framebuffers[level], vk::Rect2D({ 0, 0 }, extent)),
			vk::SubpassContents::eInline);
	cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *s.blurPipeline);
	cmd.setViewport(0, vk::Viewport(0.f, 0.f, (float)extent.width, (float)extent.height, 0.f, 1.f));
	cmd.setScissor(0, vk::Rect2D({ 0, 0 }, extent));
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *s.layout, 0, setFor(from), nullptr);
	const Push p = plain();
	cmd.pushConstants(*s.layout, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, sizeof(p), &p);
	cmd.draw(4, 1, 0, 0);
	cmd.endRenderPass();
}

// The picture blurred: down through `depth` levels and back up.
void blur(vk::CommandBuffer cmd, vk::ImageView shown, int depth)
{
	State& s = *state;
	blit(cmd, 0, shown);
	for (int i = 1; i < depth; i++)
		blit(cmd, i, s.levels[i - 1]->GetImageView());
	for (int i = depth - 2; i >= 0; i--)
		blit(cmd, i, s.levels[i + 1]->GetImageView());
	s.blurred = true;
	s.blurredDepth = depth;
}

// The picture of your own for this game: its file, or "".
std::string ownFile()
{
	for (const std::string& name : { ::settings.content.gameId, std::string("default") })
	{
		if (name.empty())
			continue;
		for (const char *ext : { ".png", ".jpg", ".jpeg" })
		{
			const std::string path = ps5::rootDir + "borders/" + name + ext;
			if (FILE *f = fopen(path.c_str(), "rb"))
			{
				fclose(f);
				return path;
			}
		}
	}
	return "";
}

void loadOwn(vk::CommandBuffer cmd)
{
	State& s = *state;
	const std::string want = ownFile();
	if (want == s.ownLoaded)
		return;
	s.ownLoaded = want;
	if (s.own)
	{
		s.own->deferDeleteResource(context());
		s.own.reset();
	}
	if (want.empty())
		return;
	int w = 0, h = 0, n = 0;
	u8 *pixels = stbi_load(want.c_str(), &w, &h, &n, STBI_rgb_alpha);
	if (pixels == nullptr)
	{
		ps5::diag::mark("border: %s cannot be read", want.c_str());
		return;
	}
	auto texture = std::make_unique<Texture>();
	texture->tex_type = TextureType::_8888;
	texture->SetCommandBuffer(cmd);
	texture->UploadToGPU(w, h, pixels, false);
	texture->SetCommandBuffer(nullptr);
	stbi_image_free(pixels);
	s.own = std::move(texture);
	s.ownAspect = h > 0 ? (float)w / h : 1.f;
	ps5::diag::mark("border: %s (%d x %d)", want.c_str(), w, h);
}

} // namespace

void setAccent(unsigned rgb)
{
	accent = rgb & 0xffffff;
}

void prepare(vk::CommandBuffer cmd, bool fresh, vk::ImageView shown, const vk::Extent2D&)
{
	const int kind = ps5::SidePicture;
	if (failed || kind == 0 || !shown || config::Rotate90 || !context())
		return;
	try {
		makeState();
		State& s = *state;
		s.shown = shown;
		if (kind == 1 || kind == 4)
		{
			const int depth = kind == 1 ? Levels : 3;
			if (fresh || !s.blurred || s.blurredDepth != depth)
				blur(cmd, shown, depth);
		}
		else if (kind == 3)
			loadOwn(cmd);
	} catch (const std::exception& e) {
		fail(e.what());
	}
}

void draw(vk::CommandBuffer cmd, vk::RenderPass pass, int width, int height, const vk::Rect2D& picture)
{
	const int kind = ps5::SidePicture;
	if (failed || kind == 0 || !state || !state->shown || config::Rotate90 || width <= 0 || height <= 0)
		return;
	const float W = (float)width, H = (float)height;
	const float p0x = (float)picture.offset.x, p0y = (float)picture.offset.y;
	const float p1x = p0x + (float)picture.extent.width, p1y = p0y + (float)picture.extent.height;
	// A picture that fills the screen has nothing beside it.
	if (p0x < 2.f && p0y < 2.f)
		return;
	try {
		State& s = *state;
		if (s.screenPass != pass || !s.screenPipeline)
		{
			context()->WaitIdle();
			s.screenPipeline = makePipeline(pass);
			s.screenPass = pass;
		}
		cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *s.screenPipeline);
		cmd.setViewport(0, vk::Viewport(0.f, 0.f, W, H, 0.f, 1.f));
		const auto scissor = [&](float x0, float y0, float x1, float y1) {
			cmd.setScissor(0, vk::Rect2D({ (int)x0, (int)y0 },
					{ (u32)std::max(0, (int)x1 - (int)x0), (u32)std::max(0, (int)y1 - (int)y0) }));
		};
		const auto push = [&](const Push& p) {
			cmd.pushConstants(*s.layout, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, sizeof(p), &p);
			cmd.draw(4, 1, 0, 0);
		};
		const auto toClip = [&](Push& p, float x0, float y0, float x1, float y1) {
			p.rect[0] = x0 / W * 2.f - 1.f;
			p.rect[1] = y0 / H * 2.f - 1.f;
			p.rect[2] = x1 / W * 2.f - 1.f;
			p.rect[3] = y1 / H * 2.f - 1.f;
		};
		Push p = plain();
		p.pic[0] = p0x;
		p.pic[1] = p1x;
		p.pic[2] = p0y;
		p.pic[3] = p1y;
		p.misc[0] = W;
		p.misc[1] = H;

		// A picture spread to cover the whole screen with its shape kept (and a little more, where asked).
		const auto cover = [&](float aspect, float more, float& x0, float& y0, float& x1, float& y1) {
			float sw = W, sh = W / aspect;
			if (sh < H)
			{
				sh = H;
				sw = H * aspect;
			}
			sw *= more;
			sh *= more;
			x0 = (W - sw) * 0.5f;
			y0 = (H - sh) * 0.5f;
			x1 = x0 + sw;
			y1 = y0 + sh;
		};

		if (kind == 1 && s.blurred)
		{
			cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *s.layout, 0, setFor(s.levels[0]->GetImageView()), nullptr);
			float x0, y0, x1, y1;
			cover((p1x - p0x) / std::max(p1y - p0y, 1.f), 1.12f, x0, y0, x1, y1);
			toClip(p, x0, y0, x1, y1);
			colour(p.top, 0xebebeb);
			colour(p.bottom, 0xebebeb);
			p.misc[2] = 0.43f;
			scissor(0, 0, W, H);
			push(p);
		}
		else if (kind == 2)
		{
			cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *s.layout, 0, setFor(s.shown), nullptr);
			// The accent colour in a quiet dark: 20% of it at the top, 4% at the bottom.
			const auto mix = [&](unsigned dark, float k, float out[4]) {
				const float a[3] = { ((dark >> 16) & 255) / 255.f, ((dark >> 8) & 255) / 255.f, (dark & 255) / 255.f };
				const float b[3] = { ((accent >> 16) & 255) / 255.f, ((accent >> 8) & 255) / 255.f, (accent & 255) / 255.f };
				for (int i = 0; i < 3; i++)
					out[i] = a[i] + (b[i] - a[i]) * k;
				out[3] = 1.f;
			};
			mix(0x06080e, 0.20f, p.top);
			mix(0x020306, 0.04f, p.bottom);
			p.misc[3] = 1.f;
			scissor(0, 0, W, H);
			push(p);
		}
		else if (kind == 3 && s.own)
		{
			cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *s.layout, 0, setFor(s.own->GetImageView()), nullptr);
			float x0, y0, x1, y1;
			cover(s.ownAspect, 1.f, x0, y0, x1, y1);
			toClip(p, x0, y0, x1, y1);
			scissor(0, 0, W, H);
			push(p);
		}
		else if (kind == 4 && s.blurred)
		{
			cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *s.layout, 0, setFor(s.levels[0]->GetImageView()), nullptr);
			colour(p.top, 0xe1e1e1);
			colour(p.bottom, 0xe1e1e1);
			p.misc[2] = 0.67f;
			const float pw = std::max(p1x - p0x, 1.f), ph = std::max(p1y - p0y, 1.f);
			const float left = std::min(p0x / pw, 0.5f), right = std::min((W - p1x) / pw, 0.5f);
			const float above = std::min(p0y / ph, 0.5f), below = std::min((H - p1y) / ph, 0.5f);
			// One part of the screen: its rectangle, and the part of the light it shows.
			const auto part = [&](float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
				if (x1 - x0 < 1.f || y1 - y0 < 1.f)
					return;
				toClip(p, x0, y0, x1, y1);
				p.uv[0] = u0;
				p.uv[1] = v0;
				p.uv[2] = u1;
				p.uv[3] = v1;
				scissor(x0, y0, x1, y1);
				push(p);
			};
			// Beside: the picture's rows, mirrored; above and below: its columns; the corners both ways.
			part(0, p0y, p0x, p1y, left, 0, 0, 1);
			part(p1x, p0y, W, p1y, 1, 0, 1 - right, 1);
			part(p0x, 0, p1x, p0y, 0, above, 1, 0);
			part(p0x, p1y, p1x, H, 0, 1, 1, 1 - below);
			part(0, 0, p0x, p0y, left, above, 0, 0);
			part(p1x, 0, W, p0y, 1, above, 1 - right, 0);
			part(0, p1y, p0x, H, left, 1, 0, 1 - below);
			part(p1x, p1y, W, H, 1, 1, 1 - right, 1 - below);
		}
		// The game's picture is drawn over this with its own viewport and scissor.
	} catch (const std::exception& e) {
		fail(e.what());
	}
}

void reset()
{
	if (state && context())
		context()->WaitIdle();
	state.reset();
	failed = false;
}

}
