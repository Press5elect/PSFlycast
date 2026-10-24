/*
    Created on: Oct 2, 2019

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
#include "vulkan.h"
#include "vulkan_renderer.h"
#ifdef USE_PS5
#include "ps5_texdump.h"
#endif
#include "drawer.h"
#include "hw/pvr/ta.h"
#include "rend/transform_matrix.h"
#ifdef USE_PS5
#include "rend/soft/soft_renderer.h"
#include "cfg/option.h"
#include "ps5_diag.h"
#include "ps5_frontend.h"
#endif

bool BaseVulkanRenderer::BaseInit(vk::RenderPass renderPass, int subpass)
{
	custom_texture.setCapabilities(Texture::getCustomTextureCapabilities());
	texCommandPool.Init();
	fbCommandPool.Init();
	quadPipeline = std::make_unique<QuadPipeline>(false, false);
	quadPipeline->Init(&shaderManager, renderPass, subpass);
	framebufferDrawer = std::make_unique<QuadDrawer>();
	framebufferDrawer->Init(quadPipeline.get());
#ifdef USE_PS5
	renderThread = std::this_thread::get_id();
	if (!listening)
	{
		listening = true;
		EventManager::listen(Event::Start, onGameEvent, this);
		EventManager::listen(Event::Terminate, onGameEvent, this);
	}
#endif

	return true;
}

void BaseVulkanRenderer::Term()
{
#ifdef USE_PS5
	if (listening)
	{
		listening = false;
		EventManager::unlisten(Event::Start, onGameEvent, this);
		EventManager::unlisten(Event::Terminate, onGameEvent, this);
	}
#endif
	GetContext()->WaitIdle();
	GetContext()->PresentFrame(nullptr, nullptr, vk::Extent2D(), 0);
#if defined(VIDEO_ROUTING) && defined(TARGET_MAC)
	os_VideoRoutingTermVk();
#endif
	framebufferDrawer.reset();
	quadPipeline.reset();
	textureCache.Clear();
	clearGpuPreloadedTextures();
	fogTexture = nullptr;
	paletteTexture = nullptr;
	texCommandPool.Term();
	fbCommandPool.Term();
	framebufferTextures.clear();
	framebufferTexIndex = 0;
	shaderManager.term();
#ifdef USE_PS5
	ps5::soft::term();
	softwareFrame = false;
#endif
}

#ifdef USE_PS5
/*
	PSFlyCast: a game's start and end.

	The renderer is made when the title starts and lives from one game to the
	next (the big-picture interface keeps the device). Flycast frees a closed
	game's textures only at the next game's first frame; here they go when the
	game ends, with the framebuffer copies and what FSR, the side picture and
	frame generation made for it, so the library and the next game's loading
	have that memory (Crux2's lesson: games played one after another slowed
	down while the last one's textures stayed). The boot log has the memory at
	the game's start and after it, and the settings the game actually ran
	with: said at its first frame, after its own settings were read and on the
	renderer they chose.
*/
void BaseVulkanRenderer::onGameEvent(Event event, void *arg)
{
	BaseVulkanRenderer *self = static_cast<BaseVulkanRenderer *>(arg);
	if (event == Event::Start)
	{
		ps5::diag::memory("at the game's start");
		settingsToSay = true;
	}
	else if (event == Event::Terminate)
		self->gameEnded();
}

void BaseVulkanRenderer::gameEnded()
{
	settingsToSay = false;
	// Unloading is the interface's, on this thread, with the emulator stopped
	// and no frame of the game's open. Called elsewhere (a load that failed),
	// the textures go at the next game's first frame, as Flycast does it.
	if (std::this_thread::get_id() != renderThread)
	{
		ps5::diag::mark("graphics: the game closed off the render thread: its textures go with the next game's first frame");
		ps5::diag::memory("after the game");
		return;
	}
	try {
		// The drawers end what they have open first (VulkanRenderer, OITVulkanRenderer).
		clearTextureCache();
		clearGpuPreloadedTextures();
		// Copies of the frame buffer: kept until the frames that may show them are done.
		const size_t copies = framebufferTextures.size();
		if (!framebufferTextures.empty())
			GetContext()->addToFlight(new Deleter(std::move(framebufferTextures)));
		framebufferTextures.clear();
		framebufferTexIndex = 0;
		GetContext()->ReleaseGameResources();
		ps5::diag::mark("graphics: the game's textures, %zu frame buffer cop%s and the picture's steps let go",
				copies, copies == 1 ? "y" : "ies");
	} catch (const vk::SystemError& e) {
		ps5::diag::mark("graphics: letting go of the game's textures failed: %s", e.what());
	}
}

void BaseVulkanRenderer::saySettings()
{
	static const char *const upscaling[] = { "off", "FSR", "FSR sharp", "FSR soft", "scanlines", "CRT", "sharp bilinear",
			"NIS", "CAS" };
	static const char *const generation[] = { "off", "light", "full" };
	static const char *const filtering[] = { "the game's", "nearest", "linear" };
	auto onOff = [](bool on) { return on ? "on" : "off"; };
	const int lines = config::RenderResolution;
	ps5::diag::mark("settings: %s renderer%s, %d lines (x%.2f), texture filtering %s, anisotropic %dx, accurate mipmaps %s, "
			"widescreen %s", name(), rend_software_frame() ? " with the software renderer's frames" : "", lines,
			lines / 480.f, filtering[std::clamp((int)config::TextureFiltering, 0, 2)], (int)config::AnisotropicFiltering,
			onOff(config::AccurateMipmaps), onOff(config::Widescreen));
	ps5::diag::mark("settings: 16-bit colour %s, flicker filter %s, framebuffer emulation %s, upscaling %s, frame generation %s, "
			"vsync %s, threaded %s, duplicate frames %s, sync to display %s", onOff(config::Original16Bit),
			onOff(config::FlickerFilter), onOff(config::EmulateFramebuffer),
			upscaling[std::clamp((int)ps5::Upscaling, 0, 8)], generation[std::clamp((int)ps5::FrameGeneration, 0, 2)],
			onOff(config::VSync), onOff(config::ThreadedRendering), onOff(config::DupeFrames), onOff(ps5::SyncToDisplay));
}

/*
	PSFlyCast: the software renderer (rend/soft, the option ps5.SoftwareRenderer).

	When a frame is its own (rend_software_frame(), decided when the frame was
	queued), Process only parses the frame, Render has the software renderer
	draw it into the emulated VRAM, and Present shows what RenderFramebuffer
	read back from VRAM at the vertical blank, as in full framebuffer
	emulation. Nothing of the frame goes to the graphics processor.

	softwareProcess is called first by each renderer's Process, which has
	ended what its screen drawer had open if the renderer changes with this
	frame. True: the frame is parsed, and Process is done.
*/
bool BaseVulkanRenderer::softwareProcess(TA_context *ctx)
{
	// A game's first frame: the settings it runs with (onGameEvent).
	if (settingsToSay.exchange(false))
		saySettings();
	const bool software = rend_software_frame();
	if (software != softwareFrame)
	{
		softwareFrame = software;
		if (software)
		{
			// The texture cache write-protects the pages of VRAM its textures
			// came from, and the software renderer's threads write frame
			// buffers and rendered textures into VRAM. It reads textures from
			// VRAM itself: the cache is emptied, which lifts the protection,
			// and stays empty (GetTexture).
			GetContext()->WaitIdle();
			textureCache.Clear();
		}
		else
		{
			// its threads and buffers are not needed any more
			ps5::soft::term();
		}
	}
	if (!software)
		return false;

	rendContext = &ctx->rend;
	if (!ctx->rend.isRTT)
		framebufferRendered = false;
	if (!ctx->rend.isRTT && ctx->rend.swapInterval > 0)
		GetContext()->setSwapInterval(ctx->rend.swapInterval);

	// ta_parse moves the texture coordinates of flat textured polygons half a
	// texel inwards when the picture is rendered larger than 480 lines ("fix
	// upscale bleeding edge"). This picture is not: the option is off while
	// this frame is parsed. Only ta_parse reads it, on this thread.
	struct NoBleedingFix
	{
		NoBleedingFix() : saved(config::FixUpscaleBleedingEdge) {
			config::FixUpscaleBleedingEdge.set(false);
		}
		~NoBleedingFix() {
			config::FixUpscaleBleedingEdge.set(saved);
		}
		const bool saved;
	} noBleedingFix;
	ta_parse(ctx, true);
#ifdef USE_PS5
	// Textures the game dumper decoded in this frame are sorted by how they were drawn.
	ps5::texdump::frame(ctx->rend);
#endif

	return true;
}

bool BaseVulkanRenderer::softwareRender()
{
	ps5::soft::render(*rendContext);
	return !rendContext->isRTT;
}
#endif

void BaseVulkanRenderer::processCustomTexturePreloads()
{
	custom_texture.processGpuPreloads([this](u32 hash,
			const PreparedCustomTexture& texture) {
		GpuPreloadedTexture::Ptr gpuTexture;
		try
		{
			texCommandPool.BeginFrame();
			vk::CommandBuffer commandBuffer = texCommandPool.Allocate();
			commandBuffer.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
			gpuTexture = Texture::createGpuPreloadedTexture(texture, commandBuffer);
			commandBuffer.end();
			texCommandPool.EndFrameAndWait();
		}
		catch (const vk::SystemError& exception)
		{
			texCommandPool.abortFrame();
			throw FlycastException(exception.what());
		}
		catch (...)
		{
			texCommandPool.abortFrame();
			throw;
		}
		if (!gpuTexture)
			return false;
		Texture::releaseGpuPreloadStaging(gpuTexture);
		addGpuPreloadedTexture(hash, std::move(gpuTexture));
		return true;
	});
}

BaseTextureCacheData *BaseVulkanRenderer::GetTexture(TSP tsp, TCW tcw, int area)
{
#ifdef USE_PS5
	if (softwareFrame)
	{
		// The software renderer reads textures from VRAM. ta_parse only has
		// to hear that the polygon has one: nothing is decoded or uploaded.
		static Texture *anyTexture = new Texture();
		return anyTexture;
	}
#endif
	Texture* tf = textureCache.getTextureCacheData(tsp, tcw, area);

	//update if needed
	if (tf->NeedsUpdate())
	{
		// This kills performance when a frame is skipped and lots of texture updated each frame
		//if (textureCache.IsInFlight(tf, true))
		//	textureCache.DestroyLater(tf);
		tf->SetCommandBuffer(texCommandBuffer, &texCommandPool);
		if (!tf->Update())
		{
			tf->SetCommandBuffer(nullptr);
			return nullptr;
		}
	}
	else if (tf->IsCustomTextureAvailable())
	{
		tf->SetCommandBuffer(texCommandBuffer, &texCommandPool);
		tf->CheckCustomTexture();
	}
	tf->SetCommandBuffer(nullptr, nullptr);
	textureCache.SetInFlight(tf);

	return tf;
}

void BaseVulkanRenderer::Process(TA_context* ctx)
{
	rendContext = &ctx->rend;
	if (!ctx->rend.isRTT) {
		framebufferRendered = false;
		if (!config::EmulateFramebuffer)
			clearLastFrame = false;
	}
	if (resetTextureCache) {
		textureCache.Clear();
		resetTextureCache = false;
	}

	texCommandPool.BeginFrame();
	textureCache.SetCurrentIndex(texCommandPool.GetIndex());
	textureCache.Cleanup();

	texCommandBuffer = texCommandPool.Allocate();
	texCommandBuffer.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

	if (!ctx->rend.isRTT && ctx->rend.swapInterval > 0)
		GetContext()->setSwapInterval(ctx->rend.swapInterval);

	ta_parse(ctx, true);

	// TODO can't update fog or palette twice in multi render
	CheckFogTexture();
	CheckPaletteTexture();
	texCommandBuffer.end();
}

void BaseVulkanRenderer::ReInitOSD()
{
	texCommandPool.Init();
	fbCommandPool.Init();
}

void BaseVulkanRenderer::RenderFramebuffer(const FramebufferInfo& info)
{
	rendContext = nullptr;
	framebufferTexIndex = (framebufferTexIndex + 1) % GetContext()->GetSwapChainSize();

	if (framebufferTextures.size() != GetContext()->GetSwapChainSize())
		framebufferTextures.resize(GetContext()->GetSwapChainSize());
	std::unique_ptr<Texture>& curTexture = framebufferTextures[framebufferTexIndex];
	if (!curTexture)
	{
		curTexture = std::make_unique<Texture>();
		curTexture->tex_type = TextureType::_8888;
	}

	fbCommandPool.BeginFrame();
	vk::CommandBuffer commandBuffer = fbCommandPool.Allocate();

	commandBuffer.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
	curTexture->SetCommandBuffer(commandBuffer);
	{
		static const float scopeColor[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
		CommandBufferDebugScope _(commandBuffer, "RenderFramebuffer", scopeColor);

		if (info.fb_r_ctrl.fb_enable == 0 || info.vo_control.blank_video == 1)
		{
			// Video output disabled
			u8 rgba[]{ (u8)info.vo_border_col._red, (u8)info.vo_border_col._green, (u8)info.vo_border_col._blue, 255 };
			curTexture->UploadToGPU(1, 1, rgba, false);
		}
		else
		{
			PixelBuffer<u32> pb;
			int width;
			int height;
			ReadFramebuffer(info, pb, width, height);

			curTexture->UploadToGPU(width, height, (u8*)pb.data(), false);
		}

	}
	curTexture->SetCommandBuffer(nullptr);
	commandBuffer.end();
	fbCommandPool.EndFrame();
	framebufferRendered = true;
	clearLastFrame = false;
}

void BaseVulkanRenderer::RenderVideoRouting()
{
#if defined(VIDEO_ROUTING) && defined(TARGET_MAC)
	if (config::VideoRouting)
	{
		auto device = GetContext()->GetDevice();
		auto srcImage = device.getSwapchainImagesKHR(GetContext()->GetSwapChain())[GetContext()->GetCurrentImageIndex()];
		auto graphicsQueue = device.getQueue(GetContext()->GetGraphicsQueueFamilyIndex(), 0);

		int targetWidth = (config::VideoRoutingScale ? config::VideoRoutingVRes * settings.display.width / settings.display.height : settings.display.width);
		int targetHeight = (config::VideoRoutingScale ? config::VideoRoutingVRes : settings.display.height);

		extern void os_VideoRoutingPublishFrameTexture(const vk::Device& device, const vk::Image& image, const vk::Queue& queue, float x, float y, float w, float h);
		os_VideoRoutingPublishFrameTexture(device, srcImage, graphicsQueue, 0, 0, targetWidth, targetHeight);
	}
	else
	{
		os_VideoRoutingTermVk();
	}
#endif
}

void BaseVulkanRenderer::CheckFogTexture()
{
	if (!fogTexture)
	{
		fogTexture = std::make_unique<Texture>();
		fogTexture->tex_type = TextureType::_8;
		updateFogTable = true;
	}
	if (!updateFogTable || !config::Fog)
		return;
	updateFogTable = false;
	u8 texData[256];
	MakeFogTexture(texData);

	fogTexture->SetCommandBuffer(texCommandBuffer);
	fogTexture->UploadToGPU(128, 2, texData, false);
	fogTexture->SetCommandBuffer(nullptr);
}

void BaseVulkanRenderer::CheckPaletteTexture()
{
	if (!paletteTexture) {
		paletteTexture = std::make_unique<Texture>();
		paletteTexture->tex_type = TextureType::_8888;
	}
	else if (!updatePalette) {
		return;
	}
	updatePalette = false;

	paletteTexture->SetCommandBuffer(texCommandBuffer);
	paletteTexture->UploadToGPU(1024, 1, (u8 *)palette32_ram, false);
	paletteTexture->SetCommandBuffer(nullptr);
}

bool BaseVulkanRenderer::presentFramebuffer()
{
	if (framebufferTexIndex >= (int)framebufferTextures.size())
		return false;
	Texture *fbTexture = framebufferTextures[framebufferTexIndex].get();
	if (fbTexture == nullptr)
		return false;
	GetContext()->PresentFrame(fbTexture->GetImage(), fbTexture->GetImageView(), fbTexture->getSize(),
			getDCFramebufferAspectRatio());
	framebufferRendered = false;
	return true;
}

class VulkanRenderer final : public BaseVulkanRenderer
{
public:
	bool Init() override
	{
		NOTICE_LOG(RENDERER, "VulkanRenderer::Init");
		try {
			textureDrawer.Init(&samplerManager, &shaderManager, &textureCache);
			textureDrawer.SetCommandPool(&texCommandPool);

			screenDrawer.Init(&samplerManager, &shaderManager, viewport);
			screenDrawer.SetCommandPool(&texCommandPool);
			BaseInit(screenDrawer.GetRenderPass());
			emulateFramebuffer = config::EmulateFramebuffer;

			return true;
		} catch (const vk::SystemError& e) {
			ERROR_LOG(RENDERER, "Vulkan system error %s", e.what());
			return false;
		}
	}

	void Term() override
	{
		try {
			DEBUG_LOG(RENDERER, "VulkanRenderer::Term");
			GetContext()->WaitIdle();
			texCommandPool.Term(); // make sure all in-flight buffers are returned
			screenDrawer.Term();
			textureDrawer.Term();
			samplerManager.term();
			BaseVulkanRenderer::Term();
		} catch (const vk::SystemError& e) {
			ERROR_LOG(RENDERER, "Vulkan system error %s", e.what());
		}
	}

	void Process(TA_context* ctx) override
	{
		try {
#ifdef USE_PS5
			// PSFlyCast: the software renderer's frames (softwareProcess)
			if (rend_software_frame() != softwareFrame)
				screenDrawer.EndRenderPass();
			if (softwareProcess(ctx))
				return;
#endif
			if (emulateFramebuffer != config::EmulateFramebuffer)
			{
				screenDrawer.EndRenderPass();
				VulkanContext::Instance()->WaitIdle();
				screenDrawer.Term();
				screenDrawer.Init(&samplerManager, &shaderManager, viewport);
				BaseInit(screenDrawer.GetRenderPass());
				emulateFramebuffer = config::EmulateFramebuffer;
			}
			else if (ctx->rend.isRTT || resetTextureCache) {
				screenDrawer.EndRenderPass();
			}
			BaseVulkanRenderer::Process(ctx);
		} catch (const vk::OutOfDeviceMemoryError& e) {
			ERROR_LOG(RENDERER, "Vulkan out of device memory: %s", e.what());
			throw RendererException("Out of device memory");
		} catch (const vk::SystemError& e) {
			ERROR_LOG(RENDERER, "Vulkan system error %s", e.what());
			throw RendererException("Vulkan system error");
		}
	}

	bool Render() override
	{
		try {
#ifdef USE_PS5
			if (softwareFrame)
				return softwareRender();
#endif
			Drawer *drawer;
			if (rendContext->isRTT)
				drawer = &textureDrawer;
			else {
				resize(rendContext->framebufferWidth, rendContext->framebufferHeight);
				drawer = &screenDrawer;
			}
			drawer->setRendContext(rendContext);
			drawer->Draw(fogTexture.get(), paletteTexture.get());
			if (config::EmulateFramebuffer || rendContext->isRTT)
				// delay ending the render pass in case of multi render
				drawer->EndRenderPass();

			return !rendContext->isRTT;
		} catch (const vk::OutOfDeviceMemoryError& e) {
			ERROR_LOG(RENDERER, "Vulkan out of device memory: %s", e.what());
			throw RendererException("Out of device memory");
		} catch (const vk::SystemError& e) {
			// Sometimes happens when resizing the window
			ERROR_LOG(RENDERER, "Vulkan system error %s", e.what());
			throw RendererException("Vulkan system error");
		}
	}

	bool Present() override
	{
		if (clearLastFrame)
			return false;
		try {
#ifdef USE_PS5
			// PSFlyCast: the software renderer's frames are in VRAM
			if (softwareFrame)
				return presentFramebuffer();
#endif
			if (config::EmulateFramebuffer || framebufferRendered)
				return presentFramebuffer();
			else
				return screenDrawer.PresentFrame();
		} catch (const vk::SystemError& e) {
			ERROR_LOG(RENDERER, "Vulkan system error %s", e.what());
			throw RendererException("Vulkan system error");
		}
	}

protected:
	void resize(int w, int h) override
	{
		if ((u32)w == viewport.width && (u32)h == viewport.height)
			return;
		BaseVulkanRenderer::resize(w, h);
		GetContext()->WaitIdle();
		screenDrawer.Init(&samplerManager, &shaderManager, viewport);
	}


	void clearTextureCache() override {
		screenDrawer.EndRenderPass();
		BaseVulkanRenderer::clearTextureCache();
	}
#ifdef USE_PS5
	const char *name() const override {
		return config::PerStripSorting ? "Vulkan per-strip" : "Vulkan per-triangle";
	}
#endif

private:
	SamplerManager samplerManager;
	ScreenDrawer screenDrawer;
	TextureDrawer textureDrawer;
	bool emulateFramebuffer = false;
};

Renderer* rend_Vulkan()
{
	return new VulkanRenderer();
}

void ReInitOSD()
{
	if (renderer != nullptr) {
		BaseVulkanRenderer *vkrenderer = dynamic_cast<BaseVulkanRenderer*>(renderer);
		if (vkrenderer != nullptr)
			vkrenderer->ReInitOSD();
	}
}
