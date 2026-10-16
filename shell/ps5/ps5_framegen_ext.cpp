/*
	PSFlyCast - frame generation, the other two ways: AMD FSR3's optical
	flow and MihawkGen.

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	Beside the title's own way of making the pictures in between the game's
	(ps5_framegen.cpp), two made by others are built in unmodified
	(third_party/):

	  1. AMD FidelityFX SDK's FSR3 optical flow, run on plain Vulkan by
	     swordpdf's ps5-framegen, whose interpolation tries several movements
	     for every 2 x 2 pixels and keeps what has not changed for 8 frames
	     (a HUD, text) where it is.
	  2. MihawkGen: Mihawk's trained network (432,308 parameters), which looks
	     at the game's two pictures alone, estimates the movement and a mask at
	     four scales, warps both pictures and mixes them.

	Both make all the pictures of a game frame at once, when the game's next
	picture comes: n of them (1 to 3) between it and the one before, 1 / (n +
	1) of the way apart, as many as the frame has presents that would repeat
	a picture (less one for the game's own, which is shown last). Each present
	then shows the one nearest its moment. They work on copies of the game's
	picture that ps5_framegen.cpp makes at its size, and their pictures are
	copied into images it keeps, so the rest of the title (the filters, FSR,
	the pictures' look) takes them as it takes the game's own.

	A way that cannot start (the device lacks something, memory runs out) is
	a line in the log, and the title's own is used instead.
*/
#define PS5_VKC_NO_MACROS
#define PS5_FRAMEGEN_VULKAN_HEADER "ps5_vkc.h"
#include "ps5_vkc.h"
#include "ps5_fgflow.h"
#include "third_party/mihawkgen/include/framegen.h"
#include "ps5_framegen_internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <string>

#include "ps5_diag.h"
#include "ps5_frontend.h"
#include "rend/vulkan/vulkan_context.h"

// The table the generators' Vulkan calls go through (ps5_vkc.h).
Ps5Vkc ps5vkc;

namespace ps5::mihawkgen
{
const void *model();
size_t modelSize();
}

namespace ps5::framegen::ext
{
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
	ps5::diag::mark("%s", line);
}

double seconds()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

vk::Device device() { return VulkanContext::Instance()->GetDevice(); }
vk::PhysicalDevice gpu() { return VulkanContext::Instance()->GetPhysicalDevice(); }

// The generators' Vulkan calls, from the driver (once for a device).
VkDevice tableFor = VK_NULL_HANDLE;
void loadTable()
{
	const VkDevice d = static_cast<VkDevice>(device());
	if (tableFor == d)
		return;
	const VkInstance instance = VulkanContext::Instance()->GetInstance();
	PFN_vkGetInstanceProcAddr getInstance = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr;
	PFN_vkGetDeviceProcAddr getDevice = reinterpret_cast<PFN_vkGetDeviceProcAddr>(getInstance(instance, "vkGetDeviceProcAddr"));
#define X(name) \
	{ \
		PFN_vkVoidFunction p = getDevice != nullptr ? getDevice(d, #name) : nullptr; \
		if (p == nullptr) \
			p = getInstance(instance, #name); \
		ps5vkc.name = reinterpret_cast<PFN_##name>(p); \
	}
	PS5_VKC_FUNCTIONS(X)
#undef X
	tableFor = d;
}

unsigned madeCount = 0;

const vk::ImageSubresourceRange Colour(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);

void imageBarrier(vk::CommandBuffer cb, vk::Image image, vk::ImageLayout from, vk::ImageLayout to,
		vk::PipelineStageFlags srcStage, vk::PipelineStageFlags dstStage, vk::AccessFlags srcAccess, vk::AccessFlags dstAccess)
{
	cb.pipelineBarrier(srcStage, dstStage, {}, nullptr, nullptr,
			vk::ImageMemoryBarrier(srcAccess, dstAccess, from, to, vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, image, Colour));
}

void memoryBarrier(vk::CommandBuffer cb, vk::PipelineStageFlags srcStage, vk::AccessFlags srcAccess,
		vk::PipelineStageFlags dstStage, vk::AccessFlags dstAccess)
{
	cb.pipelineBarrier(srcStage, dstStage, {}, vk::MemoryBarrier(srcAccess, dstAccess), nullptr, nullptr);
}

// The picture `source` (in `sourceLayout`) copied whole into made picture `index`, which ends where the title
// reads its pictures from.
void copyToMade(vk::CommandBuffer cb, vk::Image source, vk::ImageLayout sourceLayout, int index, const vk::Extent2D& extent)
{
	const internal::Picture target = internal::madeFrame(index);
	imageBarrier(cb, target.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
			vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eTransfer,
			vk::PipelineStageFlagBits::eTransfer, vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eTransferWrite,
			vk::AccessFlagBits::eTransferWrite);
	const vk::ImageSubresourceLayers layers(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
	cb.copyImage(source, sourceLayout, target.image, vk::ImageLayout::eTransferDstOptimal,
			vk::ImageCopy(layers, vk::Offset3D(), layers, vk::Offset3D(), vk::Extent3D(extent.width, extent.height, 1)));
	imageBarrier(cb, target.image, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
			vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader,
			vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead);
}

// Which of a game frame's presents shows which picture. `picture` is -1 for the game's own.
int pictureOf(int slot, int slots, int pictures, int generated, int made)
{
	if (pictures <= 1 || made <= 0)
		return -1;
	const int p = slot * pictures / std::max(slots, 1);
	if (p >= pictures - 1)
		return -1;
	// The made picture nearest this present's moment.
	const float phase = (float)(p + 1) / (float)pictures;
	return std::clamp((int)std::lround(phase * (float)(generated + 1)), 1, made) - 1;
}

// ---- 1. AMD FSR3 optical flow

struct Flow
{
	ps5fg::framegen::interpolator fg;
	bool failed = false;
	vk::Extent2D size;
	bool haveNow = false;
	int made = 0;
	int generated = 1;
	double reportedAt = 0;
};
std::unique_ptr<Flow> flow;

bool showFlow(vk::CommandBuffer cb, bool fresh, vk::ImageView view, const vk::Extent2D& extent, int slot, int slots,
		int pictures, vk::ImageView& shown)
{
	if (!flow)
		flow = std::make_unique<Flow>();
	Flow& f = *flow;
	if (f.failed)
		return false;
	loadTable();
	if (!f.fg.ready() || f.size != extent)
	{
		if (f.fg.ready())
		{
			VulkanContext::Instance()->WaitIdle();
			f.fg.destroy();
		}
		std::string error;
		if (!f.fg.create(static_cast<VkPhysicalDevice>(gpu()), static_cast<VkDevice>(device()), extent.width, extent.height, error))
		{
			mark("frame generation (optical flow): %s; the title's own is used", error.c_str());
			f.failed = true;
			return false;
		}
		f.size = extent;
		f.haveNow = false;
		f.made = 0;
		mark("frame generation (optical flow): pictures of %u x %u", extent.width, extent.height);
	}
	if (fresh)
	{
		const internal::Picture frame = internal::copyFrame(cb, view, 0);
		f.generated = std::clamp(pictures - 1, 1, 3);
		f.fg.set_generated((uint32_t)f.generated);
		f.fg.set_debug_view(ps5::FrameGenDebug);
		f.fg.prepare(cb);
		// The picture, at its size, into the frame the interpolator reads.
		imageBarrier(cb, frame.image, vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eTransferSrcOptimal,
				vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eFragmentShader,
				vk::PipelineStageFlagBits::eTransfer, vk::AccessFlagBits::eColorAttachmentWrite,
				vk::AccessFlagBits::eTransferRead);
		const vk::ImageSubresourceLayers layers(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
		cb.copyImage(frame.image, vk::ImageLayout::eTransferSrcOptimal, vk::Image(f.fg.frame_image()), vk::ImageLayout::eGeneral,
				vk::ImageCopy(layers, vk::Offset3D(), layers, vk::Offset3D(), vk::Extent3D(extent.width, extent.height, 1)));
		// The movement since the frame before and the pictures between them.
		f.made = 0;
		if (f.fg.record(cb, !f.haveNow))
		{
			for (int i = 0; i < f.generated; i++)
			{
				if (i > 0 && !f.fg.interpolate_next(cb, (uint32_t)i))
					break;
				copyToMade(cb, vk::Image(f.fg.output_image()), vk::ImageLayout::eGeneral, i, extent);
				f.made = i + 1;
			}
		}
		f.fg.advance();
		f.haveNow = true;
		// Its cost, now and then, in the boot log.
		double mean = 0, most = 0, flowMs = 0;
		uint32_t count = 0;
		if (seconds() - f.reportedAt > 10.0 && f.fg.take_gpu_time(mean, most, count, flowMs))
		{
			mark("frame generation (optical flow): %.2f ms a frame (%.2f optical flow), most %.2f, %u frames, %d made between two",
					mean, flowMs, most, count, f.generated);
			f.reportedAt = seconds();
		}
	}
	if (!f.haveNow)
		return false;
	const int k = pictureOf(slot, slots, pictures, f.generated, f.made);
	if (k < 0)
	{
		shown = view;
		return true;
	}
	madeCount++;
	shown = internal::madeFrame(k).view;
	return true;
}

// ---- 2. MihawkGen

struct Mihawk
{
	fg_context *ctx = nullptr;
	bool failed = false;
	vk::Extent2D size;
	vk::UniqueImage output;				// what the network writes (a storage image, kept in GENERAL)
	vk::UniqueDeviceMemory memory;
	vk::UniqueImageView outputView;
	bool outputReady = false;
	int cur = 0;						// which of the two copies is the latest
	bool haveNow = false;
	int made = 0;
	int generated = 1;

	~Mihawk()
	{
		if (ctx != nullptr)
			fg_context_destroy(ctx);
	}
};
std::unique_ptr<Mihawk> mihawk;

bool makeOutput(Mihawk& m, const vk::Extent2D& extent)
{
	const vk::Device d = device();
	m.output = d.createImageUnique(vk::ImageCreateInfo(vk::ImageCreateFlags(), vk::ImageType::e2D, vk::Format::eR8G8B8A8Unorm,
			vk::Extent3D(extent.width, extent.height, 1), 1, 1, vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal,
			vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
			vk::SharingMode::eExclusive, 0, nullptr, vk::ImageLayout::eUndefined));
	const vk::MemoryRequirements needs = d.getImageMemoryRequirements(*m.output);
	const vk::PhysicalDeviceMemoryProperties memory = gpu().getMemoryProperties();
	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; i < memory.memoryTypeCount && type == UINT32_MAX; i++)
		if ((needs.memoryTypeBits & (1u << i)) != 0
				&& (memory.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal))
			type = i;
	if (type == UINT32_MAX)
		return false;
	m.memory = d.allocateMemoryUnique(vk::MemoryAllocateInfo(needs.size, type));
	d.bindImageMemory(*m.output, *m.memory, 0);
	m.outputView = d.createImageViewUnique(vk::ImageViewCreateInfo(vk::ImageViewCreateFlags(), *m.output, vk::ImageViewType::e2D,
			vk::Format::eR8G8B8A8Unorm, vk::ComponentMapping(), Colour));
	m.outputReady = false;
	return true;
}

bool showMihawk(vk::CommandBuffer cb, bool fresh, vk::ImageView view, const vk::Extent2D& extent, int slot, int slots,
		int pictures, vk::ImageView& shown)
{
	if (!mihawk)
		mihawk = std::make_unique<Mihawk>();
	Mihawk& m = *mihawk;
	if (m.failed)
		return false;
	loadTable();
	if (m.ctx == nullptr || m.size != extent)
	{
		if (m.ctx != nullptr)
		{
			VulkanContext::Instance()->WaitIdle();
			fg_context_destroy(m.ctx);
			m.ctx = nullptr;
		}
		fg_create_info info{};
		info.device = static_cast<VkDevice>(device());
		info.physical_device = static_cast<VkPhysicalDevice>(gpu());
		info.width = extent.width;
		info.height = extent.height;
		info.slots = 8;
		info.output_format = VK_FORMAT_R8G8B8A8_UNORM;
		char error[256] = {};
		VkResult result = fg_context_create(&info, &m.ctx, error, sizeof(error));
		if (result == VK_SUCCESS)
			result = fg_context_load_network(m.ctx, ps5::mihawkgen::model(), ps5::mihawkgen::modelSize(), error, sizeof(error));
		bool output = false;
		if (result == VK_SUCCESS && fg_method_available(m.ctx, FG_METHOD_NEURAL))
		{
			try {
				output = makeOutput(m, extent);
			} catch (const std::exception&) {
			}
		}
		if (!output)
		{
			mark("frame generation (MihawkGen): %s (%d); the title's own is used", error, (int)result);
			m.failed = true;
			return false;
		}
		m.size = extent;
		m.haveNow = false;
		m.made = 0;
		mark("frame generation (MihawkGen): pictures of %u x %u", extent.width, extent.height);
	}
	if (fresh)
	{
		m.cur ^= 1;
		internal::copyFrame(cb, view, m.cur);
		const internal::Picture now = internal::frame(m.cur);
		const internal::Picture before = internal::frame(m.cur ^ 1);
		m.generated = std::clamp(pictures - 1, 1, 3);
		m.made = 0;
		if (m.haveNow && pictures > 1)
		{
			if (!m.outputReady)
			{
				imageBarrier(cb, *m.output, vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
						vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eComputeShader, {},
						vk::AccessFlagBits::eShaderWrite);
				m.outputReady = true;
			}
			const uint32_t frameSlot = (uint32_t)VulkanContext::Instance()->GetCurrentImageIndex() & 7u;
			for (int i = 0; i < m.generated; i++)
			{
				// The pictures written (and the output last read by a copy) are visible to the network.
				memoryBarrier(cb,
						vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eFragmentShader
								| vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eTransfer,
						vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eShaderRead
								| vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eTransferRead,
						vk::PipelineStageFlagBits::eComputeShader,
						vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite);
				fg_frame_pair pair{};
				pair.prev_color = static_cast<VkImageView>(before.view);
				pair.next_color = static_cast<VkImageView>(now.view);
				pair.t = (float)(i + 1) / (float)(m.generated + 1);
				fg_record_generate(m.ctx, static_cast<VkCommandBuffer>(cb), frameSlot, FG_METHOD_NEURAL, &pair,
						static_cast<VkImageView>(*m.outputView));
				memoryBarrier(cb, vk::PipelineStageFlagBits::eComputeShader, vk::AccessFlagBits::eShaderWrite,
						vk::PipelineStageFlagBits::eTransfer, vk::AccessFlagBits::eTransferRead);
				copyToMade(cb, *m.output, vk::ImageLayout::eGeneral, i, extent);
				m.made = i + 1;
			}
			// The network has read the picture before this one: a later frame may draw over it.
			memoryBarrier(cb,
					vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eFragmentShader
							| vk::PipelineStageFlagBits::eTransfer,
					vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eTransferRead,
					vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::AccessFlagBits::eColorAttachmentWrite);
		}
		m.haveNow = true;
	}
	if (!m.haveNow)
		return false;
	const int k = pictureOf(slot, slots, pictures, m.generated, m.made);
	if (k < 0)
	{
		shown = view;
		return true;
	}
	madeCount++;
	shown = internal::madeFrame(k).view;
	return true;
}

} // namespace

bool show(vk::CommandBuffer commandBuffer, bool fresh, vk::ImageView view, const vk::Extent2D& extent, int slot, int slots,
		int pictures, int method, vk::ImageView& shown)
{
	if (pictures <= 1)
	{
		// A game that draws as often as the screen needs nothing made: and the next picture has none before it.
		forget();
		shown = view;
		return true;
	}
	try {
		if (method == 1)
			return showFlow(commandBuffer, fresh, view, extent, slot, slots, pictures, shown);
		if (method == 2)
			return showMihawk(commandBuffer, fresh, view, extent, slot, slots, pictures, shown);
	} catch (const std::exception& e) {
		mark("frame generation (method %d): failed (%s); the title's own is used", method, e.what());
		if (method == 1 && flow)
			flow->failed = true;
		if (method == 2 && mihawk)
			mihawk->failed = true;
	}
	return false;
}

void forget()
{
	if (flow)
		flow->haveNow = false;
	if (mihawk)
		mihawk->haveNow = false;
}

void reset()
{
	// Whoever calls has waited for the device to be idle.
	flow.reset();
	mihawk.reset();
	tableFor = VK_NULL_HANDLE;
}

unsigned made()
{
	return madeCount;
}

} // namespace ps5::framegen::ext
