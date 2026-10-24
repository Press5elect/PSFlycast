/*
    Created on: Nov 7, 2019

	Copyright 2019 flyinghead

	This file is part of Flycast.

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
#pragma once
#include "vulkan.h"
#include "buffer.h"
#include "shaders.h"

#include <memory>

struct QuadVertex
{
	float x, y, z;
	float u, v;
};

// What the PowerVR2 does to the picture as it writes the tile buffer out to the framebuffer:
// the Y scaler's flicker filter (rend.FlickerFilter) and then packing to a 16-bit format with or
// without dither (rend.Original16BitColour). The renderer fills it for each frame it draws, and the
// accurate quad pipeline (QuadPipeline accurate = true) applies it where the frame is shown.
// It is the push constant block of that pipeline's fragment shader (QuadAccurateFragmentShaderSource).
struct FrameAccuracy
{
	// x: one native line in texels of the frame (0 = no filter), y/z/w: weights of the line above,
	// this line and the line below (they add up to 1)
	float flicker[4] {};
	// rgb: the 8-bit step of each channel (8 = 5 bits, 4 = 6 bits, 16 = 4 bits), a: 1 = 16-bit packing on
	float quantStep[4] {};
	// x: one native pixel in texels (the dither pattern's grid), y: 1 = dither (FB_W_CTRL.fb_dither),
	// z: low bits added on readout to the 5/4-bit channels, w: to the green channel (FB_R_CTRL.fb_concat)
	float quantAux[4] {};

	bool active() const { return flicker[0] > 0.f || quantStep[3] > 0.f; }
};

vk::PipelineVertexInputStateCreateInfo GetQuadInputStateCreateInfo(bool uv);

class QuadBuffer
{
public:
	QuadBuffer()
	{
		buffer = std::make_unique<BufferData>(sizeof(QuadVertex) * 4, vk::BufferUsageFlagBits::eVertexBuffer);
	}

	void Bind(vk::CommandBuffer commandBuffer)
	{
		commandBuffer.bindVertexBuffers(0, buffer->buffer.get(), {0});
	}
	void Draw(vk::CommandBuffer commandBuffer)
	{
		commandBuffer.draw(4, 1, 0, 0);
	}

	void Update(QuadVertex vertices[4] = nullptr)
	{
		if (vertices == nullptr)
		{
			static QuadVertex defaultVtx[4]
			{
				{ -1.f, -1.f, 0.f, 0.f, 0.f },
				{  1.f, -1.f, 0.f, 1.f, 0.f },
				{ -1.f,  1.f, 0.f, 0.f, 1.f },
				{  1.f,  1.f, 0.f, 1.f, 1.f },
			};
			vertices = defaultVtx;
		};

		const size_t quadVerticesHash = hashQuadVertexData(vertices);
		if (quadVerticesHash == lastUploadHash)
		{
			// data already uploaded
			return;
		}

		// new data to upload, update hash
		lastUploadHash = quadVerticesHash;
		buffer->upload(sizeof(QuadVertex) * 4, vertices);

	}
private:
	std::unique_ptr<BufferData> buffer;

	size_t hashQuadVertexData(QuadVertex vertices[4]) const;
	size_t lastUploadHash = 0;
};

class QuadPipeline
{
public:
	// accurate: the fragment shader applies a FrameAccuracy (flicker filter, 16-bit packing) given as
	// the push constants of QuadDrawer::Draw (its color argument) instead of a colour. Opaque output.
	QuadPipeline(bool ignoreTexAlpha, bool rotate = false, bool accurate = false)
		: rotate(rotate), ignoreTexAlpha(ignoreTexAlpha), accurate(accurate) {}
	void Init(ShaderManager *shaderManager, vk::RenderPass renderPass, int subpass);
	void Init(const QuadPipeline& other) { Init(other.shaderManager, other.renderPass, other.subpass); }
	void Term() {
		pipeline.reset();
		linearSampler.reset();
		nearestSampler.reset();
		pipelineLayout.reset();
		descSetLayout.reset();
	}
	void BindPipeline(vk::CommandBuffer commandBuffer) { commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, GetPipeline()); }
	vk::DescriptorSetLayout GetDescSetLayout() const { return *descSetLayout; }
	vk::PipelineLayout GetPipelineLayout() const { return *pipelineLayout; }
	vk::Sampler GetLinearSampler() const { return *linearSampler; }
	vk::Sampler GetNearestSampler() const { return *nearestSampler; }
	// Bytes of push constants the fragment shader reads: a colour, or a FrameAccuracy
	u32 GetPushConstantSize() const { return accurate ? sizeof(FrameAccuracy) : 4 * sizeof(float); }

private:
	vk::Pipeline GetPipeline()
	{
		if (!pipeline)
			CreatePipeline();
		return *pipeline;
	}
	void CreatePipeline();

	vk::RenderPass renderPass;
	int subpass = 0;
	vk::UniquePipeline pipeline;
	vk::UniqueSampler linearSampler;
	vk::UniqueSampler nearestSampler;
	vk::UniquePipelineLayout pipelineLayout;
	vk::UniqueDescriptorSetLayout descSetLayout;
	ShaderManager *shaderManager = nullptr;
	bool rotate;
	bool ignoreTexAlpha;
	bool accurate;
};

class QuadDrawer
{
public:
	QuadDrawer() = default;
	QuadDrawer(QuadDrawer &&) = default;
	QuadDrawer(const QuadDrawer &) = delete;
	QuadDrawer& operator=(QuadDrawer &&) = default;
	QuadDrawer& operator=(const QuadDrawer &) = delete;

	void Init(QuadPipeline *pipeline);
	void Draw(vk::CommandBuffer commandBuffer, vk::ImageView imageView, QuadVertex vertices[4] = nullptr, bool nearestFilter = false, const float *color = nullptr);
private:
	QuadPipeline *pipeline = nullptr;
	std::unique_ptr<QuadBuffer> buffer;
	std::vector<vk::UniqueDescriptorSet> descriptorSets;
};
