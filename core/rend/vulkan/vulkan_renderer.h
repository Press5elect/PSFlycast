/*
	Copyright 2020 flyinghead

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
#include "hw/pvr/Renderer_if.h"
#include "hw/pvr/ta_ctx.h"
#include "commandpool.h"
#include "pipeline.h"
#include "shaders.h"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

void os_VideoRoutingTermVk();

class BaseVulkanRenderer : public Renderer
{
protected:
	bool BaseInit(vk::RenderPass renderPass, int subpass = 0);

public:
	void Term() override;
	BaseTextureCacheData *GetTexture(TSP tsp, TCW tcw, int area) override;
	void Process(TA_context* ctx) override;
	void processCustomTexturePreloads() override;
	bool supportsGpuTexturePreload() const override { return true; }
	void ReInitOSD();
	void RenderFramebuffer(const FramebufferInfo& info) override;
	void RenderVideoRouting();

	bool RenderLastFrame() override {
		return !clearLastFrame;
	}
	bool GetLastFrame(std::vector<u8>& data, int& width, int& height) override {
		return GetContext()->GetLastFrame(data, width, height);
	}

protected:
	BaseVulkanRenderer() : viewport(640, 480) {}
	void clearTextureCache() override { textureCache.Clear(); }

	VulkanContext *GetContext() const { return VulkanContext::Instance(); }

	virtual void resize(int w, int h)
	{
		viewport.width = w;
		viewport.height = h;
	}

	void CheckFogTexture();
	void CheckPaletteTexture();
	bool presentFramebuffer();
#ifdef USE_PS5
	// PSFlyCast: the software renderer (rend/soft). See vulkan_renderer.cpp.
	bool softwareProcess(TA_context *ctx);
	bool softwareRender();
	// PSFlyCast: a game's start and end (vulkan_renderer.cpp). The renderer
	// lives from one game to the next here: what a game made goes when it ends,
	// and the settings it ran with are said at its first frame.
	static void onGameEvent(Event event, void *arg);
	void gameEnded();
	void saySettings();
	virtual const char *name() const = 0;
	std::thread::id renderThread;
	bool listening = false;
	// Shared: a game whose own settings change the renderer starts with another one.
	static inline std::atomic<bool> settingsToSay{false};
#endif

	ShaderManager shaderManager;
	std::unique_ptr<Texture> fogTexture;
	std::unique_ptr<Texture> paletteTexture;
	CommandPool texCommandPool;
	std::vector<std::unique_ptr<Texture>> framebufferTextures;
	int framebufferTexIndex = 0;
	TextureCache textureCache;
	vk::Extent2D viewport;
	vk::CommandBuffer texCommandBuffer;
	std::unique_ptr<QuadPipeline> quadPipeline;
	std::unique_ptr<QuadDrawer> framebufferDrawer;
	CommandPool fbCommandPool;
	bool framebufferRendered = false;
	rend_context *rendContext = nullptr;
#ifdef USE_PS5
	// PSFlyCast: the frame in hand is the software renderer's
	bool softwareFrame = false;
#endif
};
