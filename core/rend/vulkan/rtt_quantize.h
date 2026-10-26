/*
	Copyright 2026 Press5elect

	This file is part of PSFlyCast, a port of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
// Render to texture in the game's 16-bit format (rend.Original16BitColour).
//
// The PowerVR2 writes a render-to-texture pass out of its tile buffer like a screen frame: packed to
// the format FB_W_CTRL.fb_packmode names (0555 KRGB, 565, 4444 or 1555), with the 4x4 dither added
// first when FB_W_CTRL.fb_dither is set, else truncated. The game then samples those 16-bit texels.
// Flycast keeps render targets at 8 bits per channel, so effects drawn through a texture come out
// smoother than on the console (no banding, no dither pattern).
//
// RttQuantizer packs the finished render target in place, with a small compute pass between the end
// of the render pass and the texture's first use: each texel is quantised to the format's bits and
// read back to 8 bits as the texture unit expands them (bit replication), with the dither on the
// native grid (one native pixel is the render scale in texels). The same pass serves the normal and
// the per-pixel renderers, since both leave the texture in the shader-read layout.
//
// Not used with rend.RenderToTextureBuffer: that path writes the result to VRAM in 16 bits itself
// (WriteTextureToVRam) and the game's texture is decoded from there.
#pragma once
#include "vulkan_context.h"
#include "compiler.h"
#include "utils.h"
#include "hw/pvr/ta_ctx.h"
#include "cfg/option.h"

#include <vector>

class RttQuantizer
{
public:
	// Whether a render-to-texture pass of this context is to be packed here
	static bool wanted(const rend_context& ctx) {
		return config::Original16Bit && !config::RenderToTextureBuffer && ctx.isRTT && ctx.fb_W_CTRL.fb_packmode <= 3;
	}

	// Usage the render target needs for the pass (storage image), on top of what the drawer asks for
	static vk::ImageUsageFlags extraUsage(const rend_context& ctx) {
		return wanted(ctx) ? vk::ImageUsageFlagBits::eStorage : vk::ImageUsageFlags();
	}

	// Records the pass on cmdBuffer, after the render pass that drew into image (single mip level,
	// R8G8B8A8, in eShaderReadOnlyOptimal) and before anything samples it. The image is left in
	// eShaderReadOnlyOptimal. pvrWidth/pvrHeight: the texture's size in native pixels, for the dither grid.
	void apply(vk::CommandBuffer cmdBuffer, FlightManager *flightManager, vk::Image image, vk::ImageView imageView,
			u32 width, u32 height, int pvrWidth, int pvrHeight, const rend_context& ctx)
	{
		if (!wanted(ctx) || width == 0 || height == 0)
			return;
		if (!pipeline)
			init();
		vk::DescriptorSet descSet = allocDescSet();
		if (!descSet)
			return;

		PushConstants pc{};
		const FB_W_CTRL_type fbw = ctx.fb_W_CTRL;
		switch (fbw.fb_packmode)
		{
		case 0: // 0555 KRGB: the texture is read as 1555 (getRTTexture), its top bit is FB_W_CTRL.fb_kval bit 7
			pc.colorStep[0] = pc.colorStep[1] = pc.colorStep[2] = 8.f;
			pc.nativeAux[3] = 2.f;
			pc.alphaAux[1] = (fbw.fb_kval & 0x80) ? 1.f : 0.f;
			break;
		case 1: // 565 RGB: no alpha bits, the texture unit reads alpha as 1
			pc.colorStep[0] = pc.colorStep[2] = 8.f;
			pc.colorStep[1] = 4.f;
			pc.nativeAux[3] = 2.f;
			pc.alphaAux[1] = 1.f;
			break;
		case 2: // 4444 ARGB: alpha truncated to 4 bits too
			pc.colorStep[0] = pc.colorStep[1] = pc.colorStep[2] = 16.f;
			pc.colorStep[3] = 16.f;
			break;
		case 3: // 1555 ARGB: the alpha bit is set where alpha reaches FB_W_CTRL.fb_alpha_threshold
			pc.colorStep[0] = pc.colorStep[1] = pc.colorStep[2] = 8.f;
			pc.nativeAux[3] = 1.f;
			pc.alphaAux[0] = (float)fbw.fb_alpha_threshold;
			break;
		}
		if (pc.colorStep[3] == 0.f)
			pc.colorStep[3] = 1.f;
		pc.nativeAux[0] = pvrWidth > 0 ? std::max(1.f, (float)width / pvrWidth) : 1.f;
		pc.nativeAux[1] = pvrHeight > 0 ? std::max(1.f, (float)height / pvrHeight) : 1.f;
		// With framebuffer emulation on, the polygon shaders already add the dither to every fragment
		// (Drawer::Draw / the per-pixel final pass, "dithering"); here it is then only truncated.
		pc.nativeAux[2] = fbw.fb_dither && !config::EmulateFramebuffer ? 1.f : 0.f;
		pc.size[0] = (int)width;
		pc.size[1] = (int)height;

		vk::DescriptorImageInfo imageInfo(vk::Sampler(), imageView, vk::ImageLayout::eGeneral);
		vk::WriteDescriptorSet write(descSet, 0, 0, vk::DescriptorType::eStorageImage, imageInfo);
		VulkanContext::Instance()->GetDevice().updateDescriptorSets(write, nullptr);

		const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
		// The render pass (or the dynamic-rendering end) made the colour writes available to the
		// fragment shader stage; chain on that and on the attachment output itself.
		vk::ImageMemoryBarrier toGeneral(vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eShaderRead,
				vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite,
				vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eGeneral,
				vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, image, range);
		cmdBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eFragmentShader,
				vk::PipelineStageFlagBits::eComputeShader, {}, nullptr, nullptr, toGeneral);

		cmdBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
		cmdBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipelineLayout, 0, descSet, nullptr);
		cmdBuffer.pushConstants(*pipelineLayout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(pc), &pc);
		cmdBuffer.dispatch((width + 7) / 8, (height + 7) / 8, 1);

		vk::ImageMemoryBarrier toRead(vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eShaderRead,
				vk::ImageLayout::eGeneral, vk::ImageLayout::eShaderReadOnlyOptimal,
				vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, image, range);
		cmdBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
				vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eComputeShader,
				{}, nullptr, nullptr, toRead);

		// The set goes back to the free list once the GPU is done with this command buffer
		flightManager->addToFlight(new DescSetHolder(descSet, this));
	}

	// Call after the command pool that holds the in-flight sets has been terminated
	void term()
	{
		freeSets.clear();
		pipeline.reset();
		pipelineLayout.reset();
		descSetLayout.reset();
		descPool.reset();
		poolsFull = false;
	}

private:
	struct PushConstants
	{
		float colorStep[4];	// rgb: 8-bit step of each channel (8 = 5 bits, 4 = 6 bits, 16 = 4 bits), a: of alpha (1 = kept)
		float nativeAux[4];	// xy: one native pixel in texels, z: 1 = dither, w: alpha mode (0 kept/stepped, 1 threshold, 2 constant)
		float alphaAux[4];	// x: the 1555 alpha threshold (0-255), y: the constant alpha
		int size[4];		// xy: the texture's size in texels
	};

	class DescSetHolder : public Deletable
	{
	public:
		DescSetHolder(vk::DescriptorSet set, RttQuantizer *owner) : set(set), owner(owner) {}
		~DescSetHolder() override {
			owner->freeSets.push_back(set);
		}
	private:
		vk::DescriptorSet set;
		RttQuantizer *owner;
	};

	// Sets come from a pool of this class's own (the shared one has few storage image slots). They
	// are never freed one by one: a used set returns to freeSets, and term() drops the whole pool.
	vk::DescriptorSet allocDescSet()
	{
		if (!freeSets.empty())
		{
			vk::DescriptorSet set = freeSets.back();
			freeSets.pop_back();
			return set;
		}
		if (poolsFull)
			return vk::DescriptorSet();
		try {
			return VulkanContext::Instance()->GetDevice().allocateDescriptorSets(
					vk::DescriptorSetAllocateInfo(*descPool, *descSetLayout)).front();
		} catch (const vk::SystemError&) {
			// Every set is in flight (far more render-to-texture passes than any game does in a frame);
			// those passes stay at 8 bits until sets come back
			WARN_LOG(RENDERER, "RTT 16-bit packing: out of descriptor sets");
			poolsFull = true;
			return vk::DescriptorSet();
		}
	}

	void init()
	{
		vk::Device device = VulkanContext::Instance()->GetDevice();
		const vk::DescriptorPoolSize poolSize(vk::DescriptorType::eStorageImage, MaxSets);
		descPool = device.createDescriptorPoolUnique(vk::DescriptorPoolCreateInfo(vk::DescriptorPoolCreateFlags(), MaxSets, poolSize));
		const vk::DescriptorSetLayoutBinding binding(0, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute);
		descSetLayout = device.createDescriptorSetLayoutUnique(vk::DescriptorSetLayoutCreateInfo(vk::DescriptorSetLayoutCreateFlags(), binding));
		const vk::PushConstantRange pushRange(vk::ShaderStageFlagBits::eCompute, 0, sizeof(PushConstants));
		pipelineLayout = device.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo(vk::PipelineLayoutCreateFlags(), *descSetLayout, pushRange));
		vk::UniqueShaderModule module = ShaderCompiler::Compile(vk::ShaderStageFlagBits::eCompute,
				VulkanSource().addSource(QuantizeShaderSource).generate());
		const vk::PipelineShaderStageCreateInfo stage(vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eCompute, *module, "main");
		pipeline = device.createComputePipelineUnique(VulkanContext::Instance()->GetPipelineCache(),
				vk::ComputePipelineCreateInfo(vk::PipelineCreateFlags(), stage, *pipelineLayout)).value;
		poolsFull = false;
	}

	static constexpr u32 MaxSets = 64;

	// One invocation per texel. The dither table is the framebuffer emulation's (shaders.cpp), offset
	// 0 to just under one step, indexed by native pixel. The 8-bit value plus the offset is truncated
	// to the channel's bits, then expanded back with the top bits repeated below (5 bits: v<<3 | v>>2).
	static constexpr const char *QuantizeShaderSource = R"(
layout (local_size_x = 8, local_size_y = 8) in;

layout (set = 0, binding = 0, rgba8) uniform image2D rtt;

layout (push_constant) uniform pushBlock
{
	vec4 colorStep;
	vec4 nativeAux;
	vec4 alphaAux;
	ivec4 size;
} pc;

void main()
{
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (pos.x >= pc.size.x || pos.y >= pc.size.y)
		return;
	vec4 color = clamp(imageLoad(rtt, pos), 0.0, 1.0);
	float ditherTable[16] = float[](
		5., 13.,  7., 15.,
		9.,  1., 11.,  3.,
		6., 14.,  4., 12.,
		10., 2.,  8.,  0.
	);
	vec2 nativePos = floor(vec2(pos) / pc.nativeAux.xy);
	float r = ditherTable[int(mod(nativePos.y, 4.)) * 4 + int(mod(nativePos.x, 4.))] * pc.nativeAux.z;
	vec3 qstep = pc.colorStep.rgb;
	vec3 c8 = floor(color.rgb * 255.0 + 0.5) + r * qstep / 16.0;
	vec3 levels = 256.0 / qstep;
	vec3 q = min(floor(c8 / qstep), levels - 1.0);
	color.rgb = (q * qstep + floor(q * qstep / levels)) / 255.0;
	float a8 = floor(color.a * 255.0 + 0.5);
	if (pc.nativeAux.w == 1.0)
		color.a = a8 >= pc.alphaAux.x ? 1.0 : 0.0;
	else if (pc.nativeAux.w == 2.0)
		color.a = pc.alphaAux.y;
	else if (pc.colorStep.a > 1.0)
	{
		float aStep = pc.colorStep.a;
		float aLevels = 256.0 / aStep;
		float qa = floor(a8 / aStep);
		color.a = (qa * aStep + floor(qa * aStep / aLevels)) / 255.0;
	}
	imageStore(rtt, pos, color);
}
)";

	vk::UniqueDescriptorPool descPool;
	vk::UniqueDescriptorSetLayout descSetLayout;
	vk::UniquePipelineLayout pipelineLayout;
	vk::UniquePipeline pipeline;
	std::vector<vk::DescriptorSet> freeSets;
	bool poolsFull = false;
};
