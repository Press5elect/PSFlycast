/*
	PSFlyCast - FSR 1 upscaling of the game's picture, and its picture filters.

	Copyright 2026 Press5elect and the PSFlyCast contributors
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

	The same setting has two picture filters, "Scanlines" and "CRT"
	(ps5_crt.glsl.h): one draw each, in the swapchain's render pass, straight
	from the game's picture, so that nothing is upscaled before it (present
	alone does it). They fail the same way, and separately from FSR.
*/
#include "ps5_fsr.h"
#include "ps5_fsr_constants.h"
#include "ps5_diag.h"
#include "ps5_frontend.h"
#include "NIS_Config.h"
#include "cfg/option.h"
#include "hw/pvr/pvr_regs.h"
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
#include <chrono>
#include <vector>

namespace ps5
{
config::Option<int> Upscaling("Upscaling", UpscalingOff, "ps5");
config::Option<int> Sharpening("Sharpening", 1, "ps5");
config::Option<int> FilmGrain("FilmGrain", 0, "ps5");
config::Option<int> Brightness("Brightness", 100, "ps5");
config::Option<int> Contrast("Contrast", 100, "ps5");
config::Option<int> Saturation("Saturation", 100, "ps5");
config::Option<int> Gamma("Gamma", 100, "ps5");
config::Option<int> VmuScreenSize("VmuScreenSize", 100, "ps5");
config::Option<int> SidePicture("SidePicture", 0, "ps5");
config::Option<int> OledCare("OledCare", 0, "ps5");
}

namespace ps5
{
// A slow loop: a square of positions, one step every 20 seconds (about 10 px on a 4K screen at most).
void oledShift(int screenHeight, int& x, int& y)
{
	x = y = 0;
	if (OledCare <= 0)
		return;
	static const int route[][2] = { { 0, 0 }, { 1, 0 }, { 2, 0 }, { 2, 1 }, { 2, 2 }, { 1, 2 }, { 0, 2 }, { 0, 1 },
			{ -1, 1 }, { -2, 0 }, { -2, -1 }, { -2, -2 }, { -1, -2 }, { 0, -2 }, { 1, -2 }, { 2, -2 }, { 2, -1 } };
	const long long step = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 20;
	const auto& at = route[step % (sizeof(route) / sizeof(route[0]))];
	const int unit = std::max(1, screenHeight / 540);		// 4 px at 1080p... 4 at 2160
	x = at[0] * unit;
	y = at[1] * unit;
}
}

namespace ps5::fsr
{
// The filters' two values go on from where ps5_frontend.h's enum ends.
static_assert((int)UpscalingScanlines == (int)UpscalingFsrSoft + 1 && (int)UpscalingCrt == (int)UpscalingScanlines + 1
		&& (int)UpscalingSharpBilinear == (int)UpscalingCrt + 1 && (int)UpscalingNis == (int)UpscalingSharpBilinear + 1
		&& (int)UpscalingCas == (int)UpscalingNis + 1, "the picture filters continue ps5::Upscaling's values");

namespace
{

// AMD's two headers, as text (ps5.cmake wraps them in a string literal).
const char FfxA[] =
#include "ffx_a.inc"
;
const char FfxFsr1[] =
#include "ffx_fsr1.inc"
;
const char FfxCas[] =
#include "ffx_cas.inc"
;
// NVIDIA Image Scaling's compute shader (shell/ps5/nis, MIT).
const char NisScaler[] =
#include "NIS_Scaler.inc"
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
	vec4 colour;		// the picture's look (lookTail)
	vec4 grain;
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
	FragColor = vec4(lookTail(c, pc.colour, pc.grain), 1.0);
}
)";

// The picture's look, which every shader that puts the picture on the screen
// ends with: colours (contrast about the middle, brightness, saturation, gamma;
// 1 1 1 1 changes nothing) and a film grain: squares of a few screen pixels, a
// new pattern every frame, strongest in the middle tones (where film's is) and
// fainter in the blacks and whites. Two draws are added, a triangular spread,
// which looks like grain and not like static.
const char LookTail[] = R"(
float lookHash(vec3 p)
{
	p = fract(p * vec3(0.1031, 0.1030, 0.0973));
	p += dot(p, p.yxz + 33.33);
	return fract((p.x + p.y) * p.z);
}

vec3 lookTail(vec3 c, vec4 colour, vec4 grain)
{
	c = (c - 0.5) * colour.y + 0.5;
	c *= colour.x;
	c = mix(vec3(dot(c, vec3(0.299, 0.587, 0.114))), c, colour.z);
	c = pow(clamp(c, 0.0, 1.0), vec3(1.0 / colour.w));
	if (grain.x > 0.0)
	{
		vec2 cell = floor(gl_FragCoord.xy / max(grain.y, 1.0));
		float n = lookHash(vec3(cell, grain.z)) + lookHash(vec3(cell + 71.0, grain.z + 0.37)) - 1.0;
		float luma = dot(c, vec3(0.299, 0.587, 0.114));
		float midtone = 0.35 + 0.65 * (1.0 - abs(2.0 * luma - 1.0));
		c = clamp(c + vec3(n) * grain.x * midtone, 0.0, 1.0);
	}
	return clamp(c, 0.0, 1.0);
}
)";

// Sharp bilinear, CAS and the plain stretch, from the game's picture: LOOK_KIND
// 0 the stretch (the screen's bilinear read), 1 sharp bilinear (each texel grown
// by the whole number of times it fits, only the steps between them blended), 2
// CAS, AMD's contrast adaptive sharpening, which scales the picture as it
// sharpens it (LOOK_CAS_SCALE 1) or sharpens one already the size it has on
// the screen (0).
const char LookTop[] = R"(
layout (set = 0, binding = 0) uniform sampler2D tex;
layout (push_constant) uniform pushBlock
{
	vec2 origin;	// of the picture in the framebuffer, in pixels
	vec2 size;		// of the picture there, in pixels
	uvec4 const0;
	uvec4 const1;
	vec4 colour;
	vec4 grain;
} pc;
layout (location = 0) out vec4 FragColor;

#define A_GPU 1
#define A_GLSL 1
)";

const char LookCasMain[] = R"(
AF3 CasLoad(ASU2 p) { return texelFetch(tex, clamp(p, ASU2(0), textureSize(tex, 0) - ASU2(1)), 0).rgb; }
void CasInput(inout AF1 r, inout AF1 g, inout AF1 b) {}
)";

const char LookBottom[] = R"(
void main()
{
	vec2 uv = (gl_FragCoord.xy - pc.origin) / pc.size;
	vec3 c;
#if LOOK_KIND == 2
	AF3 s;
	CasFilter(s.r, s.g, s.b, AU2(ASU2(gl_FragCoord.xy) - ASU2(pc.origin)), pc.const0, pc.const1, LOOK_CAS_SCALE == 0);
	c = s;
#elif LOOK_KIND == 1
	vec2 texSize = vec2(textureSize(tex, 0));
	vec2 texel = uv * texSize;
	vec2 scale = max(floor(pc.size / texSize), vec2(1.0));
	vec2 centre = fract(texel) - 0.5;
	vec2 region = 0.5 - 0.5 / scale;
	texel = floor(texel) + (centre - clamp(centre, -region, region)) * scale + 0.5;
	c = textureLod(tex, texel / texSize, 0.0).rgb;
#else
	c = textureLod(tex, uv, 0.0).rgb;
#endif
	FragColor = vec4(lookTail(c, pc.colour, pc.grain), 1.0);
}
)";

// The picture filters' shader: built with CRT_TUBE 0 for Scanlines, 1 for CRT.
const char CrtSource[] =
#include "ps5_crt.glsl.h"
;

// The setting asks for one of the picture filters, not for FSR.
bool isFilter(int mode)
{
	return mode == UpscalingScanlines || mode == UpscalingCrt || mode == UpscalingSharpBilinear;
}

// The upscaling is FSR's EASU, into an image of its own, before the swapchain's pass.
bool isFsr(int mode)
{
	return mode == UpscalingFsr || mode == UpscalingFsrSharp || mode == UpscalingFsrSoft;
}

// Film grain or other colours than the game's: the picture is drawn through
// our own shader even with no filter chosen.
bool lookWanted()
{
	return ps5::FilmGrain > 0 || ps5::Brightness != 100 || ps5::Contrast != 100 || ps5::Saturation != 100
			|| ps5::Gamma != 100;
}

LookConstants currentLook(float outHeight)
{
	static unsigned frame;
	return lookConstants(ps5::FilmGrain, ps5::Brightness, ps5::Contrast, ps5::Saturation, ps5::Gamma, outHeight, frame++);
}

// CAS's sharpness, from the setting.
float casSharpness()
{
	static const float sharpness[3] = { 0.2f, 0.5f, 0.8f };
	return sharpness[std::clamp((int)ps5::Sharpening, 0, 2)];
}

// How many lines the game's video mode makes a picture of: 240 when it is not
// interlaced at 15 kHz (as getPvrFramebufferSize has it), 480 when it is
// interlaced or VGA.
float gameLines()
{
	return FB_R_CTRL.vclk_div == 0 && SPG_CONTROL.interlace == 0 ? 240.f : 480.f;
}

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
	vk::UniquePipelineLayout crtLayout;
	vk::UniqueShaderModule vertexShader;
	vk::UniqueShaderModule easuShader;
	vk::UniqueShaderModule rcasShader;
	vk::UniqueShaderModule crtShader[2];		// Scanlines, CRT
	vk::UniquePipeline easuPipeline;
	vk::UniquePipeline rcasPipeline;
	vk::RenderPass rcasRenderPass;		// the one rcasPipeline was made for
	vk::UniquePipeline crtPipeline[2];
	vk::RenderPass crtRenderPass[2];	// the one each was made for
	// Sharp bilinear, CAS and the plain stretch: [kind][CAS scaling].
	vk::UniquePipelineLayout lookLayout;
	vk::UniqueShaderModule lookShader[3][2];
	vk::UniquePipeline lookPipeline[3][2];
	vk::RenderPass lookRenderPass[3][2];
	std::vector<vk::UniqueDescriptorSet> lookSets;
	int lookLogged = -1;
	// NVIDIA Image Scaling: a compute pass, before the swapchain's, into an image of its own.
	vk::UniqueDescriptorPool nisPool;
	vk::UniqueDescriptorSetLayout nisSetLayout;
	vk::UniquePipelineLayout nisLayout;
	vk::UniqueShaderModule nisShader;
	vk::UniquePipeline nisPipeline;
	std::unique_ptr<FramebufferAttachment> nisTarget;
	std::vector<vk::UniqueDescriptorSet> nisSets;
	bool nisReady = false;		// the target holds this picture, scaled
	vk::ImageView nisSource;
	vk::Extent2D nisExtent;
	vk::UniqueSampler sampler;
	std::unique_ptr<FramebufferAttachment> target;
	vk::UniqueFramebuffer framebuffer;
	std::vector<vk::UniqueDescriptorSet> easuSets;
	std::vector<vk::UniqueDescriptorSet> rcasSets;
	std::vector<vk::UniqueDescriptorSet> crtSets;
	// The filter last reported in the log: its kind, lines and height.
	int crtLogged[3] = { -1, 0, 0 };
	// What the target holds, upscaled.
	bool ready = false;
	vk::ImageView source;
	vk::Extent2D sourceExtent;
};

std::unique_ptr<State> state;
bool failed;		// until the next reset(): the emulator's own stretch is used
bool nisFailed;		// the same, for NIS
bool crtFailed;		// the same, for the picture filters

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

// What FSR and the picture filters share. Throws when the driver or the
// shader compiler refuses something.
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
	s->vertexShader = ShaderCompiler::TryCompile(vk::ShaderStageFlagBits::eVertex,
			VulkanSource().addSource(VertexSource).generate());
	if (!s->vertexShader)
		throw std::runtime_error("a shader did not compile");
	state = std::move(s);
}

// FSR's own part of the state, which does not depend on the picture's size.
// Throws as makeState does.
void makeFsr()
{
	makeState();
	State& s = *state;
	if (s.easuPipeline)
		return;
	const vk::Device device = context()->GetDevice();

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
	s.easuPass = device.createRenderPassUnique(vk::RenderPassCreateInfo(vk::RenderPassCreateFlags(), attachment, subpass,
			dependencies));

	vk::PushConstantRange easuRange(vk::ShaderStageFlagBits::eFragment, 0, sizeof(EasuConstants));
	s.easuLayout = device.createPipelineLayoutUnique(
			vk::PipelineLayoutCreateInfo(vk::PipelineLayoutCreateFlags(), s.setLayout.get(), easuRange));
	vk::PushConstantRange rcasRange(vk::ShaderStageFlagBits::eFragment, 0, sizeof(RcasConstants));
	s.rcasLayout = device.createPipelineLayoutUnique(
			vk::PipelineLayoutCreateInfo(vk::PipelineLayoutCreateFlags(), s.setLayout.get(), rcasRange));

	s.easuShader = ShaderCompiler::TryCompile(vk::ShaderStageFlagBits::eFragment,
			VulkanSource().addSource(EasuTop).addSource(FfxA).addSource(EasuMain).addSource(FfxFsr1).addSource(EasuBottom).generate());
	s.rcasShader = ShaderCompiler::TryCompile(vk::ShaderStageFlagBits::eFragment,
			VulkanSource().addSource(RcasTop).addSource(FfxA).addSource(RcasMain).addSource(FfxFsr1).addSource(LookTail).addSource(RcasBottom).generate());
	if (!s.easuShader || !s.rcasShader)
		throw std::runtime_error("a shader did not compile");
	s.easuPipeline = makePipeline(*s.vertexShader, *s.easuShader, *s.easuLayout, *s.easuPass);
	ps5::diag::mark("fsr: ready");
}

// A picture filter's pipeline for this render pass (tube: 0 Scanlines, 1 CRT),
// made the first time it is asked for. Throws as makeState does.
vk::Pipeline filterPipeline(int tube, vk::RenderPass renderPass)
{
	makeState();
	State& s = *state;
	if (!s.crtLayout)
	{
		vk::PushConstantRange range(vk::ShaderStageFlagBits::eFragment, 0, sizeof(CrtConstants));
		s.crtLayout = context()->GetDevice().createPipelineLayoutUnique(
				vk::PipelineLayoutCreateInfo(vk::PipelineLayoutCreateFlags(), s.setLayout.get(), range));
	}
	if (!s.crtShader[tube])
	{
		s.crtShader[tube] = ShaderCompiler::TryCompile(vk::ShaderStageFlagBits::eFragment,
				VulkanSource().addConstant("CRT_TUBE", tube).addSource(LookTail).addSource(CrtSource).generate());
		if (!s.crtShader[tube])
			throw std::runtime_error("a shader did not compile");
	}
	if (!s.crtPipeline[tube] || s.crtRenderPass[tube] != renderPass)
	{
		s.crtPipeline[tube] = makePipeline(*s.vertexShader, *s.crtShader[tube], *s.crtLayout, renderPass);
		s.crtRenderPass[tube] = renderPass;
	}
	return *s.crtPipeline[tube];
}

// Sharp bilinear (kind 1), CAS (2) or the plain stretch (0) for this render
// pass; scale: CAS scales the picture itself. Throws as makeState does.
vk::Pipeline lookPipeline(int kind, int scale, vk::RenderPass renderPass)
{
	makeState();
	State& s = *state;
	if (!s.lookLayout)
	{
		vk::PushConstantRange range(vk::ShaderStageFlagBits::eFragment, 0, sizeof(LookShaderConstants));
		s.lookLayout = context()->GetDevice().createPipelineLayoutUnique(
				vk::PipelineLayoutCreateInfo(vk::PipelineLayoutCreateFlags(), s.setLayout.get(), range));
	}
	if (!s.lookShader[kind][scale])
	{
		VulkanSource source;
		source.addConstant("LOOK_KIND", kind).addConstant("LOOK_CAS_SCALE", scale).addSource(LookTop);
		if (kind == 2)
			source.addSource(FfxA).addSource(LookCasMain).addSource(FfxCas);
		s.lookShader[kind][scale] = ShaderCompiler::TryCompile(vk::ShaderStageFlagBits::eFragment,
				source.addSource(LookTail).addSource(LookBottom).generate());
		if (!s.lookShader[kind][scale])
			throw std::runtime_error("a shader did not compile");
	}
	if (!s.lookPipeline[kind][scale] || s.lookRenderPass[kind][scale] != renderPass)
	{
		s.lookPipeline[kind][scale] = makePipeline(*s.vertexShader, *s.lookShader[kind][scale], *s.lookLayout, renderPass);
		s.lookRenderPass[kind][scale] = renderPass;
	}
	return *s.lookPipeline[kind][scale];
}

// NVIDIA Image Scaling's shader: NIS_Scaler.h as it is, with its texture
// reads given a combined image sampler and its two filter tables (a texel of
// four numbers each, two to a phase) given as constant arrays made from
// NIS_Config.h's, so that nothing but the picture, its answer and 112 bytes of
// constants is bound.
std::string nisTables()
{
	std::string text;
	for (int table = 0; table < 2; table++)
	{
		text += table == 0 ? "const vec4 nisScalerCoef[128] = vec4[128](\n" : "const vec4 nisUsmCoef[128] = vec4[128](\n";
		for (size_t phase = 0; phase < kPhaseCount; phase++)
			for (int half = 0; half < 2; half++)
			{
				const float *row = table == 0 ? coef_scale[phase] : coef_usm[phase];
				char item[160];
				snprintf(item, sizeof(item), "vec4(%.9g, %.9g, %.9g, %.9g)%s\n", row[half * 4], row[half * 4 + 1],
						row[half * 4 + 2], row[half * 4 + 3], phase == kPhaseCount - 1 && half == 1 ? "" : ",");
				text += item;
			}
		text += ");\n";
	}
	return text;
}

std::string nisSource()
{
	std::string scaler = NisScaler;
	auto replace = [&scaler](const std::string& from, const std::string& to) {
		size_t at = scaler.find(from);
		if (at == std::string::npos)
			throw std::runtime_error("NIS_Scaler.h is not the version this was written for");
		while (at != std::string::npos)
		{
			scaler.replace(at, from.size(), to);
			at = scaler.find(from, at + to.size());
		}
	};
	replace("NVTEX_LOAD(coef_scaler, NVI2(vIdx, phase))", "nisScalerCoef[phase * 2 + vIdx]");
	replace("NVTEX_LOAD(coef_usm, NVI2(vIdx, phase))", "nisUsmCoef[phase * 2 + vIdx]");
	replace("textureLod(sampler2D(x, sampler), pos, 0)", "textureLod(x, pos, 0)");
	replace("textureGather(sampler2D(x, sampler), pos, 0)", "textureGather(x, pos, 0)");
	replace("textureGather(sampler2D(x, sampler), pos, 1)", "textureGather(x, pos, 1)");
	replace("textureGather(sampler2D(x, sampler), pos, 2)", "textureGather(x, pos, 2)");
	static const char Declarations[] = R"(
layout (push_constant) uniform pushBlock
{
	float kDetectRatio;
	float kDetectThres;
	float kMinContrastRatio;
	float kRatioNorm;

	float kContrastBoost;
	float kEps;
	float kSharpStartY;
	float kSharpScaleY;

	float kSharpStrengthMin;
	float kSharpStrengthScale;
	float kSharpLimitMin;
	float kSharpLimitScale;

	float kScaleX;
	float kScaleY;
	float kDstNormX;
	float kDstNormY;
	float kSrcNormX;
	float kSrcNormY;

	uint kInputViewportOriginX;
	uint kInputViewportOriginY;
	uint kInputViewportWidth;
	uint kInputViewportHeight;

	uint kOutputViewportOriginX;
	uint kOutputViewportOriginY;
	uint kOutputViewportWidth;
	uint kOutputViewportHeight;

	float reserved0;
	float reserved1;
};
layout (set = 0, binding = 0) uniform sampler2D in_texture;
layout (set = 0, binding = 1, rgba8) uniform writeonly image2D out_texture;
)";
	static const char Main[] = R"(
layout (local_size_x = NIS_THREAD_GROUP_SIZE) in;
void main()
{
	NVScaler(gl_WorkGroupID.xy, gl_LocalInvocationID.x);
}
)";
	return VulkanSource().addConstant("NIS_GLSL", 1).addConstant("NIS_SCALER", 1).addConstant("NIS_USE_HALF_PRECISION", 0)
			.addSource(Declarations).addSource(nisTables()).addSource(scaler).addSource(Main).generate();
}

// The compute pipeline and what it binds. Throws as makeState does.
void makeNis()
{
	makeState();
	State& s = *state;
	if (s.nisPipeline)
		return;
	const vk::Device device = context()->GetDevice();
	std::array<vk::DescriptorSetLayoutBinding, 2> bindings = {
			vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eCompute),
			vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute) };
	s.nisSetLayout = device.createDescriptorSetLayoutUnique(
			vk::DescriptorSetLayoutCreateInfo(vk::DescriptorSetLayoutCreateFlags(), bindings));
	// A pool of its own: the context's has too few storage images to share.
	std::array<vk::DescriptorPoolSize, 2> sizes = { vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler, 8),
			vk::DescriptorPoolSize(vk::DescriptorType::eStorageImage, 8) };
	s.nisPool = device.createDescriptorPoolUnique(vk::DescriptorPoolCreateInfo(
			vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, 8, sizes));
	vk::PushConstantRange range(vk::ShaderStageFlagBits::eCompute, 0, offsetof(NISConfig, reserved1) + sizeof(float));
	s.nisLayout = device.createPipelineLayoutUnique(
			vk::PipelineLayoutCreateInfo(vk::PipelineLayoutCreateFlags(), s.nisSetLayout.get(), range));
	s.nisShader = ShaderCompiler::TryCompile(vk::ShaderStageFlagBits::eCompute, nisSource());
	if (!s.nisShader)
		throw std::runtime_error("the NIS shader did not compile");
	s.nisPipeline = device.createComputePipelineUnique(context()->GetPipelineCache(),
			vk::ComputePipelineCreateInfo(vk::PipelineCreateFlags(),
					vk::PipelineShaderStageCreateInfo(vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eCompute,
							*s.nisShader, "main"), *s.nisLayout)).value;
	ps5::diag::mark("nis: ready");
}

// NVIDIA Image Scaling from view (in: the picture, all of its image) into the
// NIS target (out), by a compute pass recorded outside a render pass.
// Throws as makeState does.
void runNis(vk::CommandBuffer commandBuffer, vk::ImageView view, vk::ImageView original, const vk::Extent2D& in,
		const vk::Extent2D& out)
{
	makeNis();
	State& s = *state;
	const vk::Device device = context()->GetDevice();
	if (!s.nisTarget || s.nisTarget->getExtent() != out)
	{
		s.nisReady = false;
		if (s.nisTarget)
			context()->WaitIdle();
		s.nisSets.clear();
		s.nisTarget.reset();
		auto target = std::make_unique<FramebufferAttachment>(context()->GetPhysicalDevice(), device);
		target->Init(out.width, out.height, vk::Format::eR8G8B8A8Unorm,
				vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled, "NIS target");
		s.nisTarget = std::move(target);
		ps5::diag::mark("nis: a target of %u x %u", out.width, out.height);
	}
	NISConfig config{};
	static const float strength[3] = { 0.15f, 0.5f, 0.85f };
	if (!NVScalerUpdateConfig(config, strength[std::clamp((int)ps5::Sharpening, 0, 2)], 0, 0, in.width, in.height, in.width,
			in.height, 0, 0, out.width, out.height, out.width, out.height))
		throw std::runtime_error("NIS does not scale that far");
	const unsigned index = (unsigned)context()->GetCurrentImageIndex();
	if (index >= s.nisSets.size())
		s.nisSets.resize(index + 1);
	if (!s.nisSets[index])
		s.nisSets[index] = std::move(device.allocateDescriptorSetsUnique(
				vk::DescriptorSetAllocateInfo(*s.nisPool, s.nisSetLayout.get())).front());
	vk::DescriptorImageInfo input(*s.sampler, view, vk::ImageLayout::eShaderReadOnlyOptimal);
	vk::DescriptorImageInfo output({}, s.nisTarget->GetImageView(), vk::ImageLayout::eGeneral);
	std::array<vk::WriteDescriptorSet, 2> writes = {
			vk::WriteDescriptorSet(*s.nisSets[index], 0, 0, vk::DescriptorType::eCombinedImageSampler, input),
			vk::WriteDescriptorSet(*s.nisSets[index], 1, 0, vk::DescriptorType::eStorageImage, output) };
	device.updateDescriptorSets(writes, nullptr);
	// The picture it reads was drawn by a render pass; the image it writes was read a frame ago.
	vk::MemoryBarrier drawn(vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eShaderWrite,
			vk::AccessFlagBits::eShaderRead);
	vk::ImageMemoryBarrier toWrite(vk::AccessFlagBits::eShaderRead, vk::AccessFlagBits::eShaderWrite,
			vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
			s.nisTarget->GetImage(), vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
	commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eFragmentShader
			| vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eComputeShader, {}, drawn, nullptr, toWrite);
	commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, *s.nisPipeline);
	commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *s.nisLayout, 0, *s.nisSets[index], nullptr);
	commandBuffer.pushConstants(*s.nisLayout, vk::ShaderStageFlagBits::eCompute, 0,
			offsetof(NISConfig, reserved1) + sizeof(float), &config);
	// NIS_Scaler.h's blocks: 32 x 24 pixels each.
	commandBuffer.dispatch((out.width + 31) / 32, (out.height + 23) / 24, 1);
	vk::ImageMemoryBarrier toRead(vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eShaderRead, vk::ImageLayout::eGeneral,
			vk::ImageLayout::eShaderReadOnlyOptimal, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
			s.nisTarget->GetImage(), vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
	commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eFragmentShader, {},
			nullptr, nullptr, toRead);
	if (!s.nisReady || s.nisExtent != in)
		ps5::diag::mark("nis: %u x %u to %u x %u", in.width, in.height, out.width, out.height);
	s.nisReady = true;
	s.nisSource = original;
	s.nisExtent = in;
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

// The picture drawn by our look shader (kinds as lookPipeline has them), from
// view, or from the FSR target when CAS is to sharpen what that made. False
// when it cannot be, and the caller draws it the usual way.
bool drawLook(vk::CommandBuffer commandBuffer, int kind, int scale, vk::ImageView view, const vk::Extent2D& extent,
		vk::RenderPass renderPass, const vk::Rect2D& region, int shiftX, int shiftY)
{
	if (crtFailed || config::Rotate90 || !view || region.extent.width == 0 || region.extent.height == 0)
		return false;
	const int left = region.offset.x + shiftX, top = region.offset.y + shiftY;
	const int width = (int)region.extent.width, height = (int)region.extent.height;
	const int x0 = std::max(left, region.offset.x), y0 = std::max(top, region.offset.y);
	const int x1 = std::min(left, region.offset.x) + width, y1 = std::min(top, region.offset.y) + height;
	if (x1 <= x0 || y1 <= y0)
		return true;		// shifted out of sight
	vk::Pipeline pipeline;
	vk::DescriptorSet set;
	try {
		pipeline = lookPipeline(kind, scale, renderPass);
		set = descriptorSet(state->lookSets, view);
	} catch (const std::exception& e) {
		crtFailed = true;
		ps5::diag::mark("look: failed (%s): the picture is stretched the usual way", e.what());
		WARN_LOG(RENDERER, "Picture look failed: %s", e.what());
		return false;
	}
	State& s = *state;
	LookShaderConstants constants{};
	constants.origin[0] = (float)left;
	constants.origin[1] = (float)top;
	constants.size[0] = (float)width;
	constants.size[1] = (float)height;
	constants.look = currentLook((float)height);
	if (kind == 2)
	{
		// The picture CAS reads is the game's (scaled by CAS itself) or the FSR target (already its size).
		const float inW = scale != 0 ? (float)extent.width : (float)width;
		const float inH = scale != 0 ? (float)extent.height : (float)height;
		casConstants(constants.const0, constants.const1, casSharpness(), inW, inH, (float)width, (float)height);
	}
	commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
	commandBuffer.setViewport(0, vk::Viewport((float)left, (float)top, (float)width, (float)height, 0.f, 1.f));
	commandBuffer.setScissor(0, vk::Rect2D({ x0, y0 }, { (u32)(x1 - x0), (u32)(y1 - y0) }));
	commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *s.lookLayout, 0, set, nullptr);
	commandBuffer.pushConstants(*s.lookLayout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(constants), &constants);
	commandBuffer.draw(3, 1, 0, 0);
	const int said = kind * 4 + scale * 2 + (ps5::FilmGrain > 0 ? 1 : 0);
	if (s.lookLogged != said)
	{
		s.lookLogged = said;
		ps5::diag::mark("look: %s%s%s, %d x %d", kind == 2 ? (scale ? "CAS" : "CAS on the FSR picture")
				: kind == 1 ? "sharp bilinear" : "plain stretch", ps5::FilmGrain > 0 ? ", film grain" : "",
				lookWanted() ? ", colours" : "", width, height);
	}
	return true;
}

// The picture through one of the two filters: present, for them.
bool presentFiltered(vk::CommandBuffer commandBuffer, vk::ImageView view, const vk::Extent2D& extent, vk::RenderPass renderPass,
		const vk::Rect2D& region, int shiftX, int shiftY)
{
	// A picture turned on its side is left to the emulator's own stretch, as with FSR.
	if (crtFailed || config::Rotate90 || !view || region.extent.width == 0 || region.extent.height == 0)
		return false;
	if (ps5::Upscaling == UpscalingSharpBilinear)
		return drawLook(commandBuffer, 1, 0, view, extent, renderPass, region, shiftX, shiftY);
	const int tube = ps5::Upscaling == UpscalingCrt ? 1 : 0;
	// The picture, moved by the game's own shift, inside its part of the screen.
	const int left = region.offset.x + shiftX, top = region.offset.y + shiftY;
	const int width = (int)region.extent.width, height = (int)region.extent.height;
	const int x0 = std::max(left, region.offset.x), y0 = std::max(top, region.offset.y);
	const int x1 = std::min(left, region.offset.x) + width, y1 = std::min(top, region.offset.y) + height;
	if (x1 <= x0 || y1 <= y0)
		return true;		// shifted out of sight
	vk::Pipeline pipeline;
	vk::DescriptorSet set;
	try {
		pipeline = filterPipeline(tube, renderPass);
		set = descriptorSet(state->crtSets, view);
	} catch (const std::exception& e) {
		crtFailed = true;
		ps5::diag::mark("filter: failed (%s): the picture is stretched the usual way", e.what());
		WARN_LOG(RENDERER, "Picture filter failed: %s", e.what());
		return false;
	}
	State& s = *state;
	CrtConstants constants = crtConstants(tube != 0, (float)left, (float)top, (float)width, (float)height, gameLines());
	constants.look = currentLook((float)height);
	commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
	commandBuffer.setViewport(0, vk::Viewport((float)left, (float)top, (float)width, (float)height, 0.f, 1.f));
	commandBuffer.setScissor(0, vk::Rect2D({ x0, y0 }, { (u32)(x1 - x0), (u32)(y1 - y0) }));
	commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *s.crtLayout, 0, set, nullptr);
	commandBuffer.pushConstants(*s.crtLayout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(constants), &constants);
	commandBuffer.draw(3, 1, 0, 0);
	if (s.crtLogged[0] != tube || s.crtLogged[1] != (int)constants.beam[0] || s.crtLogged[2] != height)
	{
		s.crtLogged[0] = tube;
		s.crtLogged[1] = (int)constants.beam[0];
		s.crtLogged[2] = height;
		ps5::diag::mark("filter: %s, %d lines over %d x %d", tube ? "CRT" : "scanlines", (int)constants.beam[0], width, height);
	}
	return true;
}

} // namespace

void upscale(vk::CommandBuffer commandBuffer, bool fresh, vk::ImageView view, const vk::Extent2D& extent,
		const vk::Rect2D& region)
{
	// (The picture filters read the picture itself, in present.)
	const int mode = ps5::Upscaling;
	const bool bigger = !config::Rotate90 && view && extent.width > 0 && extent.height > 0
			&& extent.width < region.extent.width && extent.height < region.extent.height;
	// NIS scales by at most twice each way; beyond that FSR's EASU stretches first and NIS sharpens that.
	const bool nis = mode == UpscalingNis && !nisFailed && bigger;
	const bool nisDirect = nis && region.extent.width <= 2 * extent.width && region.extent.height <= 2 * extent.height;
	// FSR's own stretch, or for CAS (which scales only up to 4 times the area itself) the same under it.
	const bool easu = isFsr(mode) || (mode == UpscalingCas
			&& !casCanScale((float)extent.width, (float)extent.height, (float)region.extent.width, (float)region.extent.height))
			|| (nis && !nisDirect);
	const bool wanted = easu && !failed && bigger;
	if (!wanted && !nisDirect)
	{
		if (state)
		{
			state->ready = false;
			state->nisReady = false;
		}
		return;
	}
	if (nis && state && !fresh && state->nisReady && state->nisSource == view && state->nisExtent == extent && state->nisTarget
			&& state->nisTarget->getExtent() == region.extent)
		return;		// the same picture, presented again
	vk::DescriptorSet set;
	if (wanted)
	{
		try {
			makeFsr();
			makeTarget(region.extent);
			State& s = *state;
			if (!nis && !fresh && s.ready && s.source == view && s.sourceExtent == extent)
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
	if (nis)
	{
		try {
			// Straight from the game's picture, or from the EASU target that stretched it.
			if (nisDirect)
				runNis(commandBuffer, view, view, extent, region.extent);
			else
				runNis(commandBuffer, state->target->GetImageView(), view, region.extent, region.extent);
		} catch (const std::exception& e) {
			nisFailed = true;
			if (state)
				state->nisReady = false;
			ps5::diag::mark("nis: failed (%s): the picture is drawn the usual way", e.what());
			WARN_LOG(RENDERER, "NIS failed: %s", e.what());
		}
	}
}

bool present(vk::CommandBuffer commandBuffer, vk::ImageView view, const vk::Extent2D& extent, vk::RenderPass renderPass,
		const vk::Rect2D& region, int shiftX, int shiftY)
{
	if (isFilter(ps5::Upscaling))
		return presentFiltered(commandBuffer, view, extent, renderPass, region, shiftX, shiftY);
	if (ps5::Upscaling == UpscalingCas || ps5::Upscaling == UpscalingNis || (ps5::Upscaling == UpscalingOff && lookWanted()))
	{
		// CAS scales the picture itself where it can; otherwise it sharpens the FSR target, if there is one.
		if (ps5::Upscaling == UpscalingCas)
		{
			if (casCanScale((float)extent.width, (float)extent.height, (float)region.extent.width, (float)region.extent.height))
				return drawLook(commandBuffer, 2, 1, view, extent, renderPass, region, shiftX, shiftY);
			if (state && state->ready && !failed && state->source == view && state->target
					&& state->target->getExtent() == region.extent)
				return drawLook(commandBuffer, 2, 0, state->target->GetImageView(), region.extent, renderPass, region, shiftX, shiftY);
		}
		if (ps5::Upscaling == UpscalingNis && state && state->nisReady && !nisFailed && state->nisSource == view
				&& state->nisTarget && state->nisTarget->getExtent() == region.extent)
			return drawLook(commandBuffer, 0, 0, state->nisTarget->GetImageView(), region.extent, renderPass, region, shiftX, shiftY);
		return drawLook(commandBuffer, 0, 0, view, extent, renderPass, region, shiftX, shiftY);
	}
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
	RcasConstants constants = rcasConstants(sharpnessStops(), left, top);
	constants.look = currentLook((float)height);
	commandBuffer.pushConstants(*s.rcasLayout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(constants), &constants);
	commandBuffer.draw(3, 1, 0, 0);
	return true;
}

void reset()
{
	state.reset();
	failed = false;
	nisFailed = false;
	crtFailed = false;
}

} // namespace ps5::fsr
