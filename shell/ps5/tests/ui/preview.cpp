// PSFlyCast - pictures of the new interface (shell/ps5/ui) on a PC: the real screens, drawn by
// Dear ImGui into a picture with a small software rasteriser, from invented games and the
// settings table's own values. No console, no Flycast.
//
//   preview <font folder> <out folder>      writes one PNG per screen
//
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-2.0-or-later

#include "ui/ui.h"
#include "ui/IconsFontAwesome6.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace
{
const int Width = 1920, Height = 1080;

struct Image
{
	int w = 0, h = 0;
	std::vector<uint8_t> rgba;
};
std::map<ImTextureID, Image> textures;
ImTextureID nextId = 1;
std::vector<float> frameRgb;	// linear 0..1, 3 floats a pixel

void serveTextures(ImDrawData *dd)
{
	if (!dd->Textures)
		return;
	for (ImTextureData *tex : *dd->Textures)
	{
		if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates)
		{
			ImTextureID id = tex->TexID;
			if (tex->Status == ImTextureStatus_WantCreate)
				id = nextId++;
			Image& img = textures[id];
			img.w = tex->Width;
			img.h = tex->Height;
			img.rgba.resize((size_t)img.w * img.h * 4);
			const uint8_t *src = (const uint8_t *)tex->GetPixels();
			for (int i = 0; i < img.w * img.h; i++)
			{
				if (tex->Format == ImTextureFormat_RGBA32)
					std::memcpy(&img.rgba[(size_t)i * 4], src + (size_t)i * 4, 4);
				else
				{
					img.rgba[(size_t)i * 4 + 0] = img.rgba[(size_t)i * 4 + 1] = img.rgba[(size_t)i * 4 + 2] = 255;
					img.rgba[(size_t)i * 4 + 3] = src[i];
				}
			}
			tex->SetTexID(id);
			tex->SetStatus(ImTextureStatus_OK);
		}
		else if (tex->Status == ImTextureStatus_WantDestroy)
		{
			textures.erase(tex->TexID);
			tex->SetTexID(ImTextureID_Invalid);
			tex->SetStatus(ImTextureStatus_Destroyed);
		}
	}
}

void sample(const Image& img, float u, float v, float out[4])
{
	const float x = u * img.w - 0.5f, y = v * img.h - 0.5f;
	const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
	const float fx = x - x0, fy = y - y0;
	float acc[4] = { 0, 0, 0, 0 };
	for (int j = 0; j < 2; j++)
		for (int i = 0; i < 2; i++)
		{
			const int px = std::clamp(x0 + i, 0, img.w - 1), py = std::clamp(y0 + j, 0, img.h - 1);
			const float wgt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
			const uint8_t *p = &img.rgba[((size_t)py * img.w + px) * 4];
			for (int c = 0; c < 4; c++)
				acc[c] += wgt * p[c] / 255.f;
		}
	std::memcpy(out, acc, sizeof(acc));
}

void rasterise(ImDrawData *dd)
{
	for (const ImDrawList *list : dd->CmdLists)
	{
		const ImDrawVert *vtx = list->VtxBuffer.Data;
		const ImDrawIdx *idx = list->IdxBuffer.Data;
		for (const ImDrawCmd& cmd : list->CmdBuffer)
		{
			if (cmd.UserCallback)
				continue;
			const auto it = textures.find(cmd.GetTexID());
			const Image *img = it == textures.end() ? nullptr : &it->second;
			const int cx0 = std::max(0, (int)cmd.ClipRect.x), cy0 = std::max(0, (int)cmd.ClipRect.y);
			const int cx1 = std::min(Width, (int)std::ceil(cmd.ClipRect.z)), cy1 = std::min(Height, (int)std::ceil(cmd.ClipRect.w));
			for (unsigned t = 0; t < cmd.ElemCount; t += 3)
			{
				const ImDrawVert& a = vtx[cmd.VtxOffset + idx[cmd.IdxOffset + t]];
				const ImDrawVert& b = vtx[cmd.VtxOffset + idx[cmd.IdxOffset + t + 1]];
				const ImDrawVert& c = vtx[cmd.VtxOffset + idx[cmd.IdxOffset + t + 2]];
				const float area = (b.pos.x - a.pos.x) * (c.pos.y - a.pos.y) - (b.pos.y - a.pos.y) * (c.pos.x - a.pos.x);
				if (std::fabs(area) < 1e-6f)
					continue;
				const int x0 = std::max(cx0, (int)std::floor(std::min({ a.pos.x, b.pos.x, c.pos.x })));
				const int x1 = std::min(cx1, (int)std::ceil(std::max({ a.pos.x, b.pos.x, c.pos.x })));
				const int y0 = std::max(cy0, (int)std::floor(std::min({ a.pos.y, b.pos.y, c.pos.y })));
				const int y1 = std::min(cy1, (int)std::ceil(std::max({ a.pos.y, b.pos.y, c.pos.y })));
				for (int y = y0; y < y1; y++)
					for (int x = x0; x < x1; x++)
					{
						const double px = x + 0.5, py = y + 0.5;
						// The edges, positive inside whatever the winding. A pixel on an edge is
						// the triangle's on one side of it only, so that two triangles that share
						// it do not both blend it (no seams along the diagonals of a box).
						const double sg = area > 0 ? 1 : -1;
						const auto edge = [&](const ImVec2& p, const ImVec2& q) {
							const double e = sg * (((double)p.x - px) * ((double)q.y - py) - ((double)p.y - py) * ((double)q.x - px));
							if (e != 0)
								return e > 0;
							const double dx = sg * ((double)q.x - p.x), dy = sg * ((double)q.y - p.y);
							return dy < 0 || (dy == 0 && dx > 0);
						};
						if (!edge(b.pos, c.pos) || !edge(c.pos, a.pos) || !edge(a.pos, b.pos))
							continue;
						const float w0 = (float)((((double)b.pos.x - px) * ((double)c.pos.y - py) - ((double)b.pos.y - py) * ((double)c.pos.x - px)) / area);
						const float w1 = (float)((((double)c.pos.x - px) * ((double)a.pos.y - py) - ((double)c.pos.y - py) * ((double)a.pos.x - px)) / area);
						const float w2 = 1 - w0 - w1;
						float col[4];
						for (int k = 0; k < 4; k++)
						{
							const float ca = ((a.col >> (8 * k)) & 0xff) / 255.f, cb = ((b.col >> (8 * k)) & 0xff) / 255.f,
									cc = ((c.col >> (8 * k)) & 0xff) / 255.f;
							col[k] = w0 * ca + w1 * cb + w2 * cc;
						}
						if (img)
						{
							float tx[4];
							sample(*img, w0 * a.uv.x + w1 * b.uv.x + w2 * c.uv.x, w0 * a.uv.y + w1 * b.uv.y + w2 * c.uv.y, tx);
							for (int k = 0; k < 4; k++)
								col[k] *= tx[k];
						}
						float *d = &frameRgb[((size_t)y * Width + x) * 3];
						for (int k = 0; k < 3; k++)
							d[k] = d[k] * (1 - col[3]) + col[k] * col[3];
					}
			}
		}
	}
}

void save(const std::string& file)
{
	std::vector<uint8_t> out((size_t)Width * Height * 3);
	for (size_t i = 0; i < out.size(); i++)
		out[i] = (uint8_t)std::clamp((int)std::lround(frameRgb[i] * 255.f), 0, 255);
	stbi_write_png(file.c_str(), Width, Height, 3, out.data(), Width * 3);
}

// ---- the invented library

pui::Game game(const char *title, ImU32 colour, const char *place, const char *platform = "Dreamcast")
{
	pui::Game g;
	g.title = title;
	g.colour = colour;
	g.place = place;
	g.platform = platform;
	g.line = "Played 3 days ago  \xC2\xB7  2 h 10 min";
	g.lastPlayed = "3 days ago";
	g.played = "2 h 10 min";
	g.about = "A made-up game, for the picture.";
	g.file = std::string("smb://nas/Multimedia/ISO/roms/dreamcast/") + title + ".chd";
	return g;
}

std::map<std::string, int> values;
}

int main(int argc, char **argv)
{
	if (argc < 3)
	{
		std::fprintf(stderr, "usage: preview <font folder> <out folder>\n");
		return 2;
	}
	const std::string fonts = argv[1], out = argv[2];
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.DisplaySize = ImVec2((float)Width, (float)Height);
	io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
	io.IniFilename = nullptr;
	ImFont *regular = io.Fonts->AddFontFromFileTTF((fonts + "/Roboto-Medium.ttf").c_str(), 24.f);
	ImFont *bold = io.Fonts->AddFontFromFileTTF((fonts + "/Roboto-Bold.ttf").c_str(), 24.f);
	// Font Awesome's symbols, merged into both, as core/ui/gui_font.cpp has them on the console.
	{
		ImFontConfig merge;
		merge.MergeMode = true;
		merge.DstFont = regular;
		io.Fonts->AddFontFromFileTTF((fonts + "/fa-solid-900.ttf").c_str(), 24.f, &merge);
		merge.DstFont = bold;
		io.Fonts->AddFontFromFileTTF((fonts + "/fa-solid-900.ttf").c_str(), 24.f, &merge);
	}
	if (!regular || !bold)
	{
		std::fprintf(stderr, "preview: the fonts are not in %s\n", fonts.c_str());
		return 1;
	}

	pui::Host host;
	host.regular = regular;
	host.bold = bold;
	host.clock = "21:04";
	host.pads = 2;
	host.version = "v1.3.9  \xC2\xB7  build 105";
	host.games = {
		game("Harbor Drift", IM_COL32(31, 74, 117, 255), "Share"), game("Neon Courier", IM_COL32(180, 83, 42, 255), "Console"),
		game("Skyline Rally", IM_COL32(35, 105, 74, 255), "USB"), game("Orbit Arena", IM_COL32(90, 58, 168, 255), "Share", "Naomi"),
		game("Paper Samurai", IM_COL32(156, 47, 107, 255), "Share"), game("Tidal Force", IM_COL32(31, 106, 112, 255), "Console"),
		game("Dust Circuit", IM_COL32(125, 84, 23, 255), "USB", "Atomiswave"), game("Night Ferry", IM_COL32(59, 69, 88, 255), "Share"),
		game("Glass Tower", IM_COL32(31, 106, 112, 255), "Share"), game("Signal Nine", IM_COL32(59, 69, 88, 255), "Console"),
		game("Ember Road", IM_COL32(180, 83, 42, 255), "Share", "Naomi"), game("Polar Run", IM_COL32(31, 74, 117, 255), "USB"),
		game("Violet Hour", IM_COL32(90, 58, 168, 255), "Share"), game("Green Mile GP", IM_COL32(35, 105, 74, 255), "Share"),
		game("Rose Ronin", IM_COL32(156, 47, 107, 255), "Console"), game("Brass Wings", IM_COL32(125, 84, 23, 255), "Share", "Naomi"),
		game("Moss Garden", IM_COL32(35, 105, 74, 255), "Share"), game("Pulse Fighter", IM_COL32(156, 47, 107, 255), "USB", "Naomi"),
		game("Iron Lantern", IM_COL32(59, 69, 88, 255), "Share"), game("Sandline", IM_COL32(125, 84, 23, 255), "Console"),
		game("Blue Meridian", IM_COL32(31, 74, 117, 255), "Share"), game("Kiln", IM_COL32(180, 83, 42, 255), "Share"),
		game("Reef Patrol", IM_COL32(31, 106, 112, 255), "USB"), game("Arcadia Loop", IM_COL32(90, 58, 168, 255), "Share", "Atomiswave"),
		game("Wideway Rally", IM_COL32(35, 105, 74, 255), "USB"),
	};
	pui::Game& hero = host.games[0];
	hero.line = "2 discs  \xC2\xB7  Played yesterday  \xC2\xB7  6 h 20 min  \xC2\xB7  3 save states";
	hero.lastPlayed = "yesterday";
	hero.played = "6 h 20 min";
	hero.stateWhen = "yesterday 22:41";
	hero.discs = 2;
	hero.states = 3;
	hero.achieved = 12;
	hero.achievements = 40;
	hero.ownSettings = 2;
	hero.cheats = 1;
	hero.about = "A harbour city at night, a courier's bike and a clock that never stops. Race the tide through twelve districts, "
			"alone or with a second player on the same screen.";
	hero.file = "smb://nas/Multimedia/ISO/roms/dreamcast/Harbor Drift (USA) (Disc 1).chd";
	host.games[24].neverPlayed = true;
	host.recent = { 0, 1, 2, 3, 4, 5, 6, 7 };
	host.now = { { "Network Share", "Connected  \xC2\xB7  4 folders  \xC2\xB7  212 games" },
			{ "VMUs", "In sync with the share  \xC2\xB7  34 VMU files" }, { "Achievements", "Signed in  \xC2\xB7  12 of 40 in this game" } };
	host.runningLine = "Playing for 42 min  \xC2\xB7  60 fps  \xC2\xB7  100 % speed  \xC2\xB7  Disc 1 of 2";
	host.pins = { "rend.Resolution", "rend.WideScreen", "aica.Volume" };
	host.query = "wide";
	host.foundPlace = "USB Drive 1 (/mnt/usb0)";
	host.found = {
		{ "BIOS Folders", "2 BIOS files", "/mnt/usb0/BIOS", "", true, false },
		{ "Game Folders", "41 games", "/mnt/usb0/Games/Sega/Dreamcast", "", true, false },
		{ "Game Folders", "18 games", "/mnt/usb0/Games/Sega/Naomi", "", true, false },
		{ "Savestate Folders", "31 save states", "/mnt/usb0/flycast/data", "", true, false },
		{ "Box Art Folder", "180 pictures", "/mnt/usb0/flycast/boxart", "", true, false },
		{ "Cheat Folders", "26 cheat files", "/mnt/usb0/flycast/cheats", "", true, false },
		{ "Texture Pack Folders", "None found", "Stays at the default", "", false, true },
	};
	for (const pui::Setting& s : pui::settings())
		values[s.id] = s.demo;
	// A few settings changed from their default, for the dots and the Changed card.
	values["rend.Resolution"] = 4;
	values["ps5.smaa"] = 1;
	values["rend.WideScreen"] = 0;
	values["ps5.upscaling"] = 1;
	values["ps5.circleConfirms"] = 0;
	std::map<std::string, int> defaults;
	for (const pui::Setting& s : pui::settings())
		defaults[s.id] = s.demo;
	defaults["rend.Resolution"] = 3;
	defaults["ps5.smaa"] = 0;
	host.get = [](const pui::Setting& s) { return values[s.id]; };
	host.set = [](const pui::Setting& s, int v) { values[s.id] = v; };
	host.changed = [&](const pui::Setting& s) { return s.kind != pui::Kind::Action && s.kind != pui::Kind::Info && values[s.id] != defaults[s.id]; };
	host.text = [](const pui::Setting& s) -> std::string {
		const std::string id = s.id;
		if (id == "ps5.freeSpace") return "412 GB free";
		if (id == "about.version") return "v1.3.9  \xC2\xB7  build 105";
		if (id == "folders.bios") return "/mnt/usb0/BIOS";
		if (id == "folders.state") return "Default (data/savestates/)";
		if (id == "ps5.vmuManager") return "34 VMU files";
		if (id == "ps5.controllers") return "2 connected";
		if (id == "achievements.account") return "Signed in";
		return "";
	};

	struct Shot { const char *name; pui::Screen screen; int home, lib, area, row, level; };
	const Shot shots[] = {
		{ "01-home", pui::Screen::Home, 0, 0, 0, 0, 0 },
		{ "02-library", pui::Screen::Library, 0, 0, 0, 0, 0 },
		{ "03-game-page", pui::Screen::GamePage, 0, 0, 0, 0, 0 },
		{ "04-quick-menu", pui::Screen::Quick, 0, 0, 0, 0, 0 },
		{ "05-settings-home", pui::Screen::SettingsHome, 0, 0, 0, 0, 0 },
		{ "06-settings-picture-more", pui::Screen::SettingsArea, 0, 0, 0, 0, 1 },
		{ "07-search", pui::Screen::Search, 0, 0, 0, 0, 0 },
		{ "08-look-for-my-files", pui::Screen::Found, 0, 0, 0, 0, 0 },
		{ "09-settings-home-everything", pui::Screen::SettingsHome, 0, 0, 2, 0, 2 },
		{ "10-settings-controls-everything", pui::Screen::SettingsArea, 0, 0, 2, 3, 2 },
		{ "11-settings-games-and-saves", pui::Screen::SettingsArea, 0, 0, 3, 9, 1 },
		{ "12-settings-system-essentials", pui::Screen::SettingsArea, 0, 0, 7, 0, 0 },
	};
	frameRgb.assign((size_t)Width * Height * 3, 0.f);
	for (const Shot& shot : shots)
	{
		host.running = shot.screen == pui::Screen::Quick ? 0 : -1;
		for (int pass = 0; pass < 2; pass++)
		{
			pui::focusForPicture(shot.home, shot.lib, shot.area, shot.row, shot.level);
			pui::show(shot.screen);
			io.DeltaTime = 1.f / 60.f;
			ImGui::NewFrame();
			if (shot.screen == pui::Screen::Quick)
			{
				// A made-up game picture behind the quick menu.
				ImDrawList *bg = ImGui::GetBackgroundDrawList();
				bg->AddRectFilled(ImVec2(0, 0), ImVec2(1920, 1080), IM_COL32(16, 32, 43, 255));
				bg->AddQuadFilled(ImVec2(-120, 640), ImVec2(2040, 480), ImVec2(2040, 1080), ImVec2(-120, 1080), IM_COL32(27, 58, 44, 255));
				bg->AddCircleFilled(ImVec2(1300, 360), 180, IM_COL32(240, 138, 60, 255), 96);
				bg->AddRectFilled(ImVec2(0, 0), ImVec2(1920, 1080), IM_COL32(11, 13, 18, 150));
			}
			pui::frame(host, pui::Input{});
			ImGui::Render();
			ImDrawData *dd = ImGui::GetDrawData();
			serveTextures(dd);
			std::fill(frameRgb.begin(), frameRgb.end(), 0.f);
			rasterise(dd);
		}
		save(out + "/" + shot.name + ".png");
		std::printf("%s\n", shot.name);
	}
	// The screens whose logic is the console's: a game's page with a list open, and the quick menu.
	const auto picture = [&](const char *name, const std::function<void()>& draw, bool gameBehind) {
		for (int pass = 0; pass < 2; pass++)
		{
			io.DeltaTime = 1.f / 60.f;
			ImGui::NewFrame();
			if (gameBehind)
			{
				ImDrawList *bg = ImGui::GetBackgroundDrawList();
				bg->AddRectFilled(ImVec2(0, 0), ImVec2(1920, 1080), IM_COL32(16, 32, 43, 255));
				bg->AddQuadFilled(ImVec2(-120, 640), ImVec2(2040, 480), ImVec2(2040, 1080), ImVec2(-120, 1080), IM_COL32(27, 58, 44, 255));
				bg->AddCircleFilled(ImVec2(700, 360), 180, IM_COL32(240, 138, 60, 255), 96);
			}
			draw();
			ImGui::Render();
			ImDrawData *dd = ImGui::GetDrawData();
			serveTextures(dd);
			std::fill(frameRgb.begin(), frameRgb.end(), 0.f);
			rasterise(dd);
		}
		save(out + "/" + name + ".png");
		std::printf("%s\n", name);
	};
	picture("13-game-page-states", [&] {
		pui::PageView p;
		p.game = host.games[0];
		p.actions = { { ICON_FA_PLAY, "Play" }, { ICON_FA_CLOCK_ROTATE_LEFT, "Load State" }, { ICON_FA_SLIDERS, "Its Settings" },
				{ ICON_FA_BOLT, "Cheats" }, { ICON_FA_GEAR, "Manage" } };
		p.action = 1;
		p.inList = true;
		p.listTitle = "Start from a saved state";
		const char *when[] = { "Yesterday 22:41", "12 Nov 2026 19:03", "9 Nov 2026 21:15" };
		for (int i = 0; i < 3; i++)
		{
			pui::kit::Line l;
			l.icon = ICON_FA_FLOPPY_DISK;
			l.label = "Slot " + std::to_string(i + 1);
			l.value = when[i];
			p.lines.push_back(l);
		}
		p.line = 0;
		p.hints = { { { "circle" }, "Back" }, { { "triangle" }, "Delete" }, { { "cross" }, "Load" } };
		pui::drawPage(host, p);
	}, false);
	picture("14-game-page-manage", [&] {
		pui::PageView p;
		p.game = host.games[1];
		p.actions = { { ICON_FA_PLAY, "Play" }, { ICON_FA_CLOCK_ROTATE_LEFT, "Load State" }, { ICON_FA_SLIDERS, "Its Settings" },
				{ ICON_FA_BOLT, "Cheats" }, { ICON_FA_GEAR, "Manage" } };
		p.action = 4;
		p.inList = true;
		p.listTitle = "Manage";
		const char *labels[] = { "Favourite", "Hide from the library", "Change cover", "Time played" };
		const char *icons[] = { ICON_FA_STAR, ICON_FA_EYE_SLASH, ICON_FA_IMAGE, ICON_FA_CLOCK };
		for (int i = 0; i < 4; i++)
		{
			pui::kit::Line l;
			l.icon = icons[i];
			l.label = labels[i];
			l.kind = i == 0 ? pui::kit::Line::Toggle : i == 2 ? pui::kit::Line::Opens : pui::kit::Line::Info;
			l.on = true;
			l.value = i == 2 ? "Box art" : i == 3 ? "2 h 10 min" : "";
			p.lines.push_back(l);
		}
		p.line = 2;
		p.hints = { { { "circle" }, "Back" }, { { "cross" }, "Change" } };
		pui::drawPage(host, p);
	}, false);
	host.forGame = host.games[0].title;
	host.available = [](const pui::Setting& s) { return s.has('g') || s.has('G'); };
	host.running = 0;
	picture("15-game-settings", [&] {
		pui::focusForPicture(0, 0, 0, 0, 0);
		pui::show(pui::Screen::SettingsHome);
		pui::frame(host, pui::Input{});
	}, false);
	picture("16-game-settings-picture", [&] {
		pui::focusForPicture(0, 0, 0, 1, 0);
		pui::show(pui::Screen::SettingsArea);
		pui::frame(host, pui::Input{});
	}, false);
	host.forGame.clear();
	host.available = nullptr;
	picture("17-quick-menu-cheats-page", [&] {
		pui::QuickView q;
		q.game = host.games[0];
		q.status = "Paused";
		q.page = true;
		q.pageTitle = "Cheats";
		q.width = 900;
		q.hints = { { { "circle" }, "Back" }, { { "cross" }, "On / Off" } };
		const ImVec4 r = pui::drawQuick(host, q);
		pui::kit::Line l;
		l.kind = pui::kit::Line::Toggle;
		const char *names[] = { "Infinite time", "All cars", "All tracks", "Max money" };
		for (int i = 0; i < 4; i++)
		{
			l.label = names[i];
			l.on = i == 1;
			pui::kit::line(r.x + 48, r.y + i * 76, r.z - 96, 70, l, i == 0);
		}
	}, true);
	// The skins: Ember and Arcade.
	host.running = -1;
	const struct { const char *name; pui::Palette p; } skinsShown[] = {
		{ "18-skin-ember", { IM_COL32(22, 15, 12, 255), IM_COL32(40, 25, 18, 255), IM_COL32(255, 255, 255, 255), IM_COL32(222, 208, 199, 255),
				IM_COL32(170, 150, 138, 255), IM_COL32(49, 35, 28, 255), IM_COL32(68, 48, 38, 255), IM_COL32(255, 255, 255, 46),
				IM_COL32(255, 196, 84, 255), IM_COL32(236, 100, 20, 255), IM_COL32(255, 255, 255, 255) } },
		{ "19-skin-arcade", { IM_COL32(15, 9, 27, 255), IM_COL32(35, 16, 50, 255), IM_COL32(255, 255, 255, 255), IM_COL32(216, 202, 230, 255),
				IM_COL32(162, 142, 183, 255), IM_COL32(45, 26, 63, 255), IM_COL32(62, 38, 86, 255), IM_COL32(255, 255, 255, 46),
				IM_COL32(255, 180, 40, 255), IM_COL32(224, 46, 138, 255), IM_COL32(255, 255, 255, 255) } },
	};
	for (const auto& sk : skinsShown)
	{
		host.palette = sk.p;
		picture(sk.name, [&] {
			pui::focusForPicture(0, 0, 0, 0, 1);
			pui::show(pui::Screen::SettingsArea);
			pui::frame(host, pui::Input{});
		}, false);
		std::string home = std::string(sk.name) + "-home";
		picture(home.c_str(), [&] {
			pui::focusForPicture(0, 0, 0, 0, 0);
			pui::show(pui::Screen::Home);
			pui::frame(host, pui::Input{});
		}, false);
	}
	ImGui::DestroyContext();
	return 0;
}
