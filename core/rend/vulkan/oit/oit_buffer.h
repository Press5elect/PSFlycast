/*
    Created on: Nov 12, 2019

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
#include "../buffer.h"
#include "../texture.h"

#include <cinttypes>
#include <memory>

#ifdef USE_PS5
#include "ps5_diag.h"
extern "C" int64_t sceKernelGetDirectMemorySize(void);
extern "C" int32_t sceKernelAvailableDirectMemorySize(int64_t search_start, int64_t search_end, size_t alignment,
		int64_t *start, size_t *size);
#endif

class OITBuffers
{
public:
	void Init(int width, int height)
	{
		const VulkanContext *context = VulkanContext::Instance();

		if (width <= maxWidth && height <= maxHeight)
			return;
		maxWidth = std::max(maxWidth, width);
		maxHeight = std::max(maxHeight, height);

		if (!pixelCounter)
		{
			pixelCounter = std::make_unique<BufferData>(4,
					vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst, vk::MemoryPropertyFlagBits::eDeviceLocal);
			pixelCounterReset = std::make_unique<BufferData>(4, vk::BufferUsageFlagBits::eTransferSrc);
			const int zero = 0;
			pixelCounterReset->upload(sizeof(zero), &zero);
		}
		// We need to wait until this buffer is not used before deleting it
		context->WaitIdle();
		abufferPointer.reset();
		abufferPointer = std::make_unique<BufferData>(maxWidth * maxHeight * sizeof(int),
				vk::BufferUsageFlagBits::eStorageBuffer, vk::MemoryPropertyFlagBits::eDeviceLocal);
		firstFrameAfterInit = true;
	}

	void updateDescriptorSet(vk::DescriptorSet descSet, std::vector<vk::WriteDescriptorSet>& writeDescSets)
	{
		static vk::DescriptorBufferInfo pixelBufferInfo({}, 0, vk::WholeSize);
		pixelBufferInfo.buffer = *pixelBuffer->buffer;
		/**
		 * The intention behind setting the range to 1 byte in BDA mode is to make it easier to spot any missing or invalid USE_BDA defines in shader sources.
		 * If USE_BDA is missing or invalid, the shader will access PixelBuffer as an SSBO, and if it's this tiny, it should result in obvious graphical glitches.
		 */
		pixelBufferInfo.range = pixelBufferAddress ? 1 : VK_WHOLE_SIZE;
		writeDescSets.emplace_back(descSet, 7, 0, vk::DescriptorType::eStorageBuffer, nullptr, pixelBufferInfo);
		static vk::DescriptorBufferInfo pixelCounterBufferInfo({}, 0, 4);
		pixelCounterBufferInfo.buffer = *pixelCounter->buffer;
		writeDescSets.emplace_back(descSet, 8, 0, vk::DescriptorType::eStorageBuffer, nullptr, pixelCounterBufferInfo);
		static vk::DescriptorBufferInfo abufferPointerInfo({}, 0, vk::WholeSize);
		abufferPointerInfo.buffer = *abufferPointer->buffer;
		writeDescSets.emplace_back(descSet, 9, 0, vk::DescriptorType::eStorageBuffer, nullptr, abufferPointerInfo);
	}

	void OnNewFrame(vk::CommandBuffer commandBuffer)
	{
		firstFrameAfterInit = false;
		const int64_t wanted = wantedPixelBufferSize();
		if (pixelBufferSize != wanted)
		{
			pixelBufferSize = wanted;
			VulkanContext::Instance()->WaitIdle();
			makePixelBuffer();
		}
	}

	void ResetPixelCounter(vk::CommandBuffer commandBuffer)
	{
		// Previous render passes and frames in flight must be done with the counter
		vk::BufferMemoryBarrier barrier(vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eTransferWrite,
				vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, *pixelCounter->buffer, 0, vk::WholeSize);
		commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader, vk::PipelineStageFlagBits::eTransfer,
				{}, nullptr, barrier, nullptr);
    	vk::BufferCopy copy(0, 0, sizeof(int));
    	commandBuffer.copyBuffer(*pixelCounterReset->buffer, *pixelCounter->buffer, copy);
		// The reset must be visible to the fragment shaders of the next render pass
		barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
		barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
		commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader,
				{}, nullptr, barrier, nullptr);
	}

	void Term()
	{
		pixelBufferAddress = 0;
		pixelBuffer.reset();
		pixelCounter.reset();
		pixelCounterReset.reset();
		abufferPointer.reset();
	}

	bool isFirstFrameAfterInit() const { return firstFrameAfterInit; }

	vk::DeviceAddress getPixelBufferAddress() const { return pixelBufferAddress; }

	vk::DeviceSize getPixelBufferSize() const
	{
		return pixelBuffer ? pixelBuffer->bufferSize : 0;
	}

private:
	std::unique_ptr<BufferData> pixelBuffer;
	std::unique_ptr<BufferData> pixelCounter;
	std::unique_ptr<BufferData> pixelCounterReset;
	std::unique_ptr<BufferData> abufferPointer;
	bool firstFrameAfterInit = false;
	int maxWidth = 0;
	int maxHeight = 0;
	int64_t pixelBufferSize = 0;
	vk::DeviceAddress pixelBufferAddress = 0;

	// The pixel buffer holds every translucent fragment of a frame, 16 bytes
	// each, and the fragments past its end are not drawn: whatever is drawn
	// last (a HUD, a menu) goes first. The setting's 512 MB is 32 million
	// fragments, which is less than one layer of a picture rendered at eight
	// times the console's resolution and up. On the PS5 the buffer therefore
	// grows with the picture: six layers of it on average, in steps of 256 MB,
	// up to 3 GB (under the 4 GB a storage buffer can span).
	int64_t wantedPixelBufferSize() const
	{
		int64_t size = config::PixelBufferSize;
#ifdef USE_PS5
		constexpr int64_t Step = 256_MB;
		const int64_t layers = (int64_t)maxWidth * maxHeight * 16 * 6;
		size = std::max(size, std::min<int64_t>((layers + Step - 1) / Step * Step, 3_GB));
#endif
		return size;
	}

#ifdef USE_PS5
	// What the console has left for it: no more than half of the memory still
	// free, so textures and the game's own buffers keep theirs.
	static vk::DeviceSize ps5PixelBufferBudget(vk::DeviceSize wanted, vk::DeviceSize least)
	{
		const int64_t pool = sceKernelGetDirectMemorySize();
		int64_t start = -1;
		size_t available = 0;
		if (pool <= 0 || sceKernelAvailableDirectMemorySize(0, pool, 0x4000, &start, &available) != 0)
			return wanted;
		ps5::diag::mark("per-pixel: %d MB of memory free of %d MB", (int)(available >> 20), (int)(pool >> 20));
		const vk::DeviceSize budget = std::max<vk::DeviceSize>(least, (available / 2) & ~(vk::DeviceSize)(256_MB - 1));
		return std::min(wanted, budget);
	}
#endif

	void makePixelBuffer() {
		const VulkanContext *context = VulkanContext::Instance();
		const u32 maxStorageBufferRange = context->GetMaxStorageBufferRange();
		vk::DeviceSize allocSize = std::min<vk::DeviceSize>(pixelBufferSize, context->GetMaxMemoryAllocationSize());
#ifdef USE_PS5
		// The old buffer first: both do not have to fit at once.
		pixelBuffer.reset();
		pixelBufferAddress = 0;
		const vk::DeviceSize least = std::min<vk::DeviceSize>(allocSize, 512_MB);
		allocSize = ps5PixelBufferBudget(allocSize, least);
		for (;;)
		{
			try {
				pixelBuffer = std::make_unique<BufferData>(allocSize, vk::BufferUsageFlagBits::eStorageBuffer,
						vk::MemoryPropertyFlagBits::eDeviceLocal);
				break;
			} catch (const std::exception& e) {
				ps5::diag::mark("per-pixel: a buffer of %d MB could not be had: %s", (int)(allocSize >> 20), e.what());
				if (allocSize <= least)
					throw;
				allocSize = std::max(least, allocSize / 2);
			}
		}
		ps5::diag::mark("per-pixel: buffer of %d MB for %d x %d (%.1f layers on average)", (int)(allocSize >> 20),
				maxWidth, maxHeight, allocSize / 16.0 / std::max(1.0, (double)maxWidth * maxHeight));
		return;
#endif
		vk::BufferUsageFlags usage = vk::BufferUsageFlagBits::eStorageBuffer;
		if (allocSize > maxStorageBufferRange) {
			if (context->SupportsBufferDeviceAddress()) {
				NOTICE_LOG(RENDERER, "PixelBuffer allocSize %" PRIu64 " > maxStorageBufferRange %lu; will use BDA", allocSize, (unsigned long)maxStorageBufferRange);
				usage |= vk::BufferUsageFlagBits::eShaderDeviceAddressKHR;
			} else {
				NOTICE_LOG(RENDERER, "PixelBuffer allocSize %" PRIu64 " > maxStorageBufferRange %lu; capping allocSize, as the GPU doesn't support BDA", allocSize, (unsigned long)maxStorageBufferRange);
				allocSize = maxStorageBufferRange;
			}
		}
		pixelBuffer = std::make_unique<BufferData>(allocSize, usage, vk::MemoryPropertyFlagBits::eDeviceLocal);
		pixelBufferAddress =
			(usage & vk::BufferUsageFlagBits::eShaderDeviceAddressKHR)
			? context->GetDevice().getBufferAddress(vk::BufferDeviceAddressInfo{*pixelBuffer->buffer}) : 0;
		DEBUG_LOG(RENDERER, "PixelBuffer allocated. size %" PRIu64 " address %" PRIu64 "", allocSize, pixelBufferAddress);
	}
};
