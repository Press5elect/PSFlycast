/*
	PSFlyCast - game videos: the video of the game in the library, played in
	the interface where its cover is, with its sound mixed in. The way
	PSSwanStation plays them.

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-3.0-or-later

	The console refuses a homebrew title its own player (libSceAvPlayer), so the
	videos are decoded here, on the processor, with FFmpeg's H.264, AAC and MP3
	decoders (libavformat, libavcodec and libavutil 9.0.2, LGPL-2.1-or-later,
	built by shell/ps5/tools/build-ffmpeg.sh with only the MP4 demuxer and those
	decoders and their parsers). The file is read through the same storage the
	games are, so it may be on the console, a USB drive or a network share.

	A thread of its own reads and decodes, and keeps a few pictures ahead, each
	as NV12 with its time, and the sound as 16-bit samples with theirs. The
	interface's thread takes the picture whose time has come and the sound up to
	a little past it. A video that loops is read again from its start.

	Where the videos are: ES-DE's layout, and a plain folder of the title's own.
	In <root>, and in each games folder (the console's, USB drives', the
	network share's), these are tried, with the game's file name or its title
	and .mp4, .m4v or .mov:
	  videos/   media/videos/   media/dreamcast/videos/
	  downloaded_media/dreamcast/videos/   and ../downloaded_media/dreamcast/videos/
*/
#include "ps5_frontend.h"
#include "ps5_diag.h"
#include "cfg/option.h"
#include "oslib/storage.h"
#include "log/Log.h"
#include "ui/imgui_driver.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace ps5
{
config::Option<bool> GameVideos("GameVideos", true, "ps5");
config::Option<int> GameVideoVolume("GameVideoVolume", 50, "ps5");
}

namespace ps5::video
{
namespace backend
{

struct Picture
{
	const uint8_t *y = nullptr, *uv = nullptr;
	int width = 0, height = 0;
	int pitch = 0, uvPitch = 0;
};

namespace
{

double nowSeconds()
{
	using namespace std::chrono;
	return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

constexpr size_t PicturesAhead = 6;
constexpr double SoundAhead = 2.0;		// seconds decoded ahead at most
constexpr double SoundLead = 0.08;		// handed to the output this far ahead of the picture
constexpr int IoBuffer = 64 * 1024;

struct Frame
{
	double time = 0;
	int width = 0, height = 0;
	std::vector<uint8_t> nv12;		// width x height brightness, then the colour at half the size
};

struct Sound
{
	double time = 0;
	int channels = 2, rate = 48000;
	std::vector<int16_t> samples;
};

// The file being read: local, or on a share.
struct Source
{
	hostfs::File *file = nullptr;
	int64_t size = -1;

	~Source()
	{
		delete file;
	}
};

int readPacket(void *opaque, uint8_t *buffer, int bytes)
{
	Source *s = (Source *)opaque;
	const size_t got = s->file->read(buffer, 1, (size_t)bytes);
	return got == 0 ? AVERROR_EOF : (int)got;
}

int64_t seekTo(void *opaque, int64_t offset, int whence)
{
	Source *s = (Source *)opaque;
	if (whence & AVSEEK_SIZE)
		return s->size;
	whence &= ~AVSEEK_FORCE;
	if (s->file->seek(offset, whence) != 0)
		return -1;
	return s->file->tell();
}

// One video being played.
struct Player
{
	std::string path;
	bool loop = false;

	std::thread thread;
	std::mutex mutex;
	std::condition_variable room;		// the decoder waits here for room ahead
	std::atomic<bool> quit{false};
	bool done = false;		// the decoder has stopped (its end, or a failure)
	std::string failure;
	std::deque<Frame> frames;
	std::deque<Sound> sounds;
	std::vector<std::vector<uint8_t>> spare;		// pictures' buffers to use again

	// The frame's thread's own.
	Frame shown;
	bool clockStarted = false;
	double startedAt = 0, firstTime = 0;

	double soundAhead() const
	{
		double seconds = 0;
		for (const Sound& s : sounds)
			seconds += (double)s.samples.size() / s.channels / s.rate;
		return seconds;
	}
	bool full() const
	{
		return frames.size() >= PicturesAhead || soundAhead() >= SoundAhead;
	}

	void run();
	void decode();
};

std::unique_ptr<Player> player;
std::atomic<int> logged{0};

void logLine(void *, int level, const char *format, va_list args)
{
	if (level > AV_LOG_ERROR || logged.fetch_add(1) >= 20)
		return;
	char line[512];
	vsnprintf(line, sizeof(line), format, args);
	size_t n = strlen(line);
	while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
		line[--n] = 0;
	ps5::diag::mark("video: ffmpeg: %s", line);
}

std::string errorText(int code)
{
	char text[AV_ERROR_MAX_STRING_SIZE] = "";
	av_strerror(code, text, sizeof(text));
	return text;
}

// A decoded picture into NV12: 8-bit 4:2:0 only (what such videos are).
bool toNv12(const AVFrame *f, std::vector<uint8_t>& out, int& width, int& height)
{
	width = f->width & ~1;
	height = f->height & ~1;
	if (width <= 0 || height <= 0)
		return false;
	out.resize((size_t)width * height * 3 / 2);
	uint8_t *y = out.data(), *uv = out.data() + (size_t)width * height;
	switch (f->format)
	{
	case AV_PIX_FMT_YUV420P:
	case AV_PIX_FMT_YUVJ420P:
		av_image_copy_plane(y, width, f->data[0], f->linesize[0], width, height);
		for (int row = 0; row < height / 2; row++)
		{
			const uint8_t *u = f->data[1] + (size_t)row * f->linesize[1];
			const uint8_t *v = f->data[2] + (size_t)row * f->linesize[2];
			uint8_t *dst = uv + (size_t)row * width;
			for (int x = 0; x < width / 2; x++)
			{
				dst[x * 2] = u[x];
				dst[x * 2 + 1] = v[x];
			}
		}
		return true;
	case AV_PIX_FMT_NV12:
		av_image_copy_plane(y, width, f->data[0], f->linesize[0], width, height);
		av_image_copy_plane(uv, width, f->data[1], f->linesize[1], width, height / 2);
		return true;
	default:
		return false;
	}
}

inline int16_t toSample(float v)
{
	v *= 32767.0f;
	return (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int)v);
}

// Decoded sound into interleaved 16-bit samples.
bool toSamples(const AVFrame *f, std::vector<int16_t>& out, int channels)
{
	const int n = f->nb_samples;
	out.resize((size_t)n * channels);
	int16_t *dst = out.data();
	switch (f->format)
	{
	case AV_SAMPLE_FMT_FLTP:
		for (int c = 0; c < channels; c++)
		{
			const float *src = (const float *)f->extended_data[c];
			for (int i = 0; i < n; i++)
				dst[i * channels + c] = toSample(src[i]);
		}
		return true;
	case AV_SAMPLE_FMT_FLT:
		for (int i = 0; i < n * channels; i++)
			dst[i] = toSample(((const float *)f->extended_data[0])[i]);
		return true;
	case AV_SAMPLE_FMT_S16P:
		for (int c = 0; c < channels; c++)
		{
			const int16_t *src = (const int16_t *)f->extended_data[c];
			for (int i = 0; i < n; i++)
				dst[i * channels + c] = src[i];
		}
		return true;
	case AV_SAMPLE_FMT_S16:
		memcpy(dst, f->extended_data[0], (size_t)n * channels * 2);
		return true;
	case AV_SAMPLE_FMT_S32P:
		for (int c = 0; c < channels; c++)
		{
			const int32_t *src = (const int32_t *)f->extended_data[c];
			for (int i = 0; i < n; i++)
				dst[i * channels + c] = (int16_t)(src[i] >> 16);
		}
		return true;
	default:
		return false;
	}
}

void Player::run()
{
	decode();
	std::lock_guard<std::mutex> lock(mutex);
	done = true;
}

void Player::decode()
{
	Source source;
	source.file = hostfs::storage().openFile(path, "rb");
	if (source.file == nullptr)
	{
		std::lock_guard<std::mutex> lock(mutex);
		failure = "it would not open";
		return;
	}
	source.size = source.file->size();

	uint8_t *ioBuffer = (uint8_t *)av_malloc(IoBuffer);
	AVIOContext *io = avio_alloc_context(ioBuffer, IoBuffer, 0, &source, readPacket, nullptr, seekTo);
	AVFormatContext *format = avformat_alloc_context();
	AVCodecContext *video = nullptr, *audio = nullptr;
	AVPacket *packet = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	int videoStream = -1, audioStream = -1;
	std::string why;

	const auto openDecoder = [&](int stream) -> AVCodecContext * {
		const AVCodecParameters *par = format->streams[stream]->codecpar;
		const AVCodec *codec = avcodec_find_decoder(par->codec_id);
		if (codec == nullptr)
			return nullptr;
		AVCodecContext *ctx = avcodec_alloc_context3(codec);
		avcodec_parameters_to_context(ctx, par);
		ctx->pkt_timebase = format->streams[stream]->time_base;
		if (par->codec_type == AVMEDIA_TYPE_VIDEO)
		{
			// Three threads: the console's processor has eight cores, the
			// emulator is not running while the library is shown.
			ctx->thread_count = 3;
			ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
		}
		if (avcodec_open2(ctx, codec, nullptr) < 0)
		{
			avcodec_free_context(&ctx);
			return nullptr;
		}
		return ctx;
	};

	format->pb = io;
	format->flags |= AVFMT_FLAG_CUSTOM_IO;
	int err = avformat_open_input(&format, nullptr, nullptr, nullptr);
	if (err < 0)
	{
		why = "not a video FFmpeg reads (" + errorText(err) + ")";
		format = nullptr;		// freed by avformat_open_input
	}
	else if ((err = avformat_find_stream_info(format, nullptr)) < 0)
		why = "its streams could not be read (" + errorText(err) + ")";
	else
	{
		videoStream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
		audioStream = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, videoStream, nullptr, 0);
		if (videoStream < 0)
			why = "no picture in it";
		else if ((video = openDecoder(videoStream)) == nullptr)
			why = std::string("no decoder for its pictures (") + avcodec_get_name(format->streams[videoStream]->codecpar->codec_id) + ")";
		else if (audioStream >= 0 && (audio = openDecoder(audioStream)) == nullptr)
			ps5::diag::mark("video: no decoder for its sound (%s); played without it",
					avcodec_get_name(format->streams[audioStream]->codecpar->codec_id));
	}

	const AVRational videoBase = videoStream >= 0 ? format->streams[videoStream]->time_base : AVRational{1, 1};
	const AVRational audioBase = audioStream >= 0 ? format->streams[audioStream]->time_base : AVRational{1, 1};
	double offset = 0, lastEnd = 0;		// a loop's times carry on from the end of the last round
	bool anyPicture = false;

	// Waits for room ahead; false when the player is closing.
	const auto waitForRoom = [&]() {
		std::unique_lock<std::mutex> lock(mutex);
		room.wait(lock, [&] { return quit.load() || !full(); });
		return !quit.load();
	};

	const auto receive = [&](AVCodecContext *ctx, bool isVideo) {
		while (avcodec_receive_frame(ctx, frame) == 0)
		{
			const int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
			if (isVideo)
			{
				Frame f;
				f.time = offset + (pts == AV_NOPTS_VALUE ? lastEnd - offset : pts * av_q2d(videoBase));
				{
					std::lock_guard<std::mutex> lock(mutex);
					if (!spare.empty())
					{
						f.nv12 = std::move(spare.back());
						spare.pop_back();
					}
				}
				if (toNv12(frame, f.nv12, f.width, f.height))
				{
					const double length = frame->duration > 0 ? frame->duration * av_q2d(videoBase) : 1.0 / 30;
					lastEnd = std::max(lastEnd, f.time + length);
					anyPicture = true;
					if (!waitForRoom())
						return false;
					std::lock_guard<std::mutex> lock(mutex);
					frames.push_back(std::move(f));
				}
				else if (!anyPicture)
				{
					why = std::string("its pictures are ") + (av_get_pix_fmt_name((AVPixelFormat)frame->format) ?: "?")
							+ ", not 8-bit 4:2:0";
					return false;
				}
			}
			else
			{
				Sound s;
				s.channels = std::clamp(frame->ch_layout.nb_channels, 1, 8);
				s.rate = frame->sample_rate > 0 ? frame->sample_rate : 48000;
				s.time = offset + (pts == AV_NOPTS_VALUE ? 0 : pts * av_q2d(audioBase));
				if (toSamples(frame, s.samples, s.channels))
				{
					lastEnd = std::max(lastEnd, s.time + (double)frame->nb_samples / s.rate);
					if (!waitForRoom())
						return false;
					std::lock_guard<std::mutex> lock(mutex);
					sounds.push_back(std::move(s));
				}
			}
			av_frame_unref(frame);
		}
		return true;
	};

	if (why.empty())
		ps5::diag::mark("video: %s, %d x %d, %s%s", avcodec_get_name(video->codec_id), video->width, video->height,
				audio != nullptr ? avcodec_get_name(audio->codec_id) : "no sound", loop ? ", looped" : "");

	bool going = why.empty();
	while (going && !quit.load())
	{
		err = av_read_frame(format, packet);
		if (err == AVERROR_EOF || (err < 0 && anyPicture))
		{
			// The end: what the decoders still hold, then again from the start or stop.
			avcodec_send_packet(video, nullptr);
			going = receive(video, true);
			if (audio != nullptr)
			{
				avcodec_send_packet(audio, nullptr);
				going = going && receive(audio, false);
			}
			if (!going || !loop || !anyPicture)
				break;
			if (av_seek_frame(format, -1, 0, AVSEEK_FLAG_BACKWARD) < 0)
				break;
			avcodec_flush_buffers(video);
			if (audio != nullptr)
				avcodec_flush_buffers(audio);
			offset = lastEnd;
			continue;
		}
		if (err < 0)
		{
			why = "it could not be read (" + errorText(err) + ")";
			break;
		}
		if (packet->stream_index == videoStream)
		{
			if (avcodec_send_packet(video, packet) == AVERROR(EAGAIN))
				going = receive(video, true) && avcodec_send_packet(video, packet) >= 0;
			going = going && receive(video, true);
		}
		else if (audio != nullptr && packet->stream_index == audioStream)
		{
			if (avcodec_send_packet(audio, packet) >= 0)
				going = receive(audio, false);
		}
		av_packet_unref(packet);
	}

	av_frame_free(&frame);
	av_packet_free(&packet);
	avcodec_free_context(&video);
	avcodec_free_context(&audio);
	if (format != nullptr)
		avformat_close_input(&format);
	if (io != nullptr)
	{
		av_freep(&io->buffer);
		avio_context_free(&io);
	}
	if (!why.empty())
	{
		std::lock_guard<std::mutex> lock(mutex);
		failure = why;
	}
}

} // namespace

const char *name()
{
	static const std::string text = std::string("FFmpeg ") + av_version_info();
	return text.c_str();
}

bool broken()
{
	return false;
}

bool load(std::string&)
{
	av_log_set_level(AV_LOG_ERROR);
	av_log_set_callback(logLine);
	return true;
}

void close();

bool open(const std::string& path, bool loop, std::string& why)
{
	close();
	if (path.empty())
	{
		why = "no file";
		return false;
	}
	player = std::make_unique<Player>();
	player->path = path;
	player->loop = loop;
	Player *p = player.get();
	player->thread = std::thread([p] { p->run(); });
	return true;
}

void close()
{
	if (!player)
		return;
	player->quit = true;
	player->room.notify_all();
	if (player->thread.joinable())
		player->thread.join();
	player.reset();
}

bool take(Picture& out)
{
	if (!player)
		return false;
	Player& p = *player;
	std::unique_lock<std::mutex> lock(p.mutex);
	if (!p.failure.empty())
	{
		ps5::diag::mark("video: %s cannot be played: %s", p.path.substr(p.path.rfind('/') + 1).c_str(), p.failure.c_str());
		p.failure.clear();
	}
	if (p.frames.empty())
		return false;
	if (!p.clockStarted)
	{
		p.clockStarted = true;
		p.firstTime = p.frames.front().time;
		p.startedAt = nowSeconds();
	}
	const double clock = p.firstTime + (nowSeconds() - p.startedAt);
	if (p.frames.front().time > clock && !p.shown.nv12.empty())
		return false;
	// The newest picture whose time has come; those before it are passed over.
	do
	{
		if (!p.shown.nv12.empty())
			p.spare.push_back(std::move(p.shown.nv12));
		p.shown = std::move(p.frames.front());
		p.frames.pop_front();
	} while (!p.frames.empty() && p.frames.front().time <= clock);
	lock.unlock();
	p.room.notify_all();
	out.y = p.shown.nv12.data();
	out.uv = p.shown.nv12.data() + (size_t)p.shown.width * p.shown.height;
	out.width = p.shown.width;
	out.height = p.shown.height;
	out.pitch = out.uvPitch = p.shown.width;
	return true;
}

void drainAudio(const std::function<void(const int16_t *, size_t, int, int)>& sink)
{
	if (!player)
		return;
	Player& p = *player;
	std::vector<Sound> due;
	{
		std::lock_guard<std::mutex> lock(p.mutex);
		if (!p.clockStarted)
			return;
		const double clock = p.firstTime + (nowSeconds() - p.startedAt) + SoundLead;
		while (!p.sounds.empty() && p.sounds.front().time <= clock)
		{
			due.push_back(std::move(p.sounds.front()));
			p.sounds.pop_front();
		}
	}
	if (due.empty())
		return;
	p.room.notify_all();
	if (sink)
		for (const Sound& s : due)
			sink(s.samples.data(), s.samples.size() / s.channels, s.channels, s.rate);
}

bool active()
{
	if (!player)
		return false;
	std::lock_guard<std::mutex> lock(player->mutex);
	return !(player->done && player->frames.empty());
}

} // namespace backend

// ---------------------------------------------------------------- the interface's side

namespace
{

struct State
{
	bool loaded = false, usable = false;
	std::string why;
	std::string path;
	bool loop = false, open = false, finished = false;
	float volume = 0.f;
	unsigned long long wantedAt = 0, frames = 0;
	int width = 0, height = 0;
	bool hasPicture = false;
	ImTextureID texture = ImTextureID();
	bool textureMade = false;
	std::vector<uint8_t> pixels;
	unsigned pictures = 0;
};
State state;

constexpr const char *TextureName = "ps5.gamevideo";

bool ensureLoaded()
{
	if (!state.loaded)
	{
		state.loaded = true;
		state.usable = backend::load(state.why);
		ps5::diag::mark("video: %s", state.usable ? backend::name() : ("no videos: " + state.why).c_str());
	}
	return state.usable;
}

inline uint8_t clampByte(int v)
{
	return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

// NV12 to RGBA, BT.601 with video levels (16-235).
void convert(const backend::Picture& p, std::vector<uint8_t>& out)
{
	out.resize((size_t)p.width * p.height * 4);
	for (int y = 0; y < p.height; y++)
	{
		const uint8_t *row = p.y + (size_t)y * p.pitch;
		const uint8_t *uv = p.uv + (size_t)(y / 2) * p.uvPitch;
		uint8_t *dst = &out[(size_t)y * p.width * 4];
		for (int x = 0; x < p.width; x++)
		{
			const int c = ((int)row[x] - 16) * 298;
			const int u = (int)uv[(x & ~1)] - 128, v = (int)uv[(x & ~1) + 1] - 128;
			dst[x * 4] = clampByte((c + 409 * v + 128) >> 8);
			dst[x * 4 + 1] = clampByte((c - 100 * u - 208 * v + 128) >> 8);
			dst[x * 4 + 2] = clampByte((c + 516 * u + 128) >> 8);
			dst[x * 4 + 3] = 255;
		}
	}
}

void closeNow()
{
	if (state.open)
		backend::close();
	state.open = false;
	state.hasPicture = false;
	state.finished = false;
	ps5::sound::clearStream();
	if (state.textureMade && imguiDriver != nullptr)
		imguiDriver->deleteTexture(TextureName);
	state.textureMade = false;
	state.texture = ImTextureID();
	state.width = state.height = 0;
}

// ---- finding the file, on a thread of its own: a share can be slow

std::mutex findMutex;
std::condition_variable findWake;
struct Found
{
	std::string file;		// "" for none
	double at = 0;
};
std::map<std::string, Found> found;		// stem -> the answer, a "none" kept for a minute
std::deque<std::pair<std::string, std::string>> wanted;	// stem, title
std::thread finder;
bool finding = false;

// A name as ES-DE and a games folder may spell it differently: lower case, what is in
// brackets left out, letters and digits only.
std::string plain(const std::string& name)
{
	std::string out;
	int depth = 0;
	for (char c : name)
	{
		if (c == '(' || c == '[')
			depth++;
		else if (c == ')' || c == ']')
			depth = std::max(0, depth - 1);
		else if (depth == 0 && std::isalnum((unsigned char)c))
			out += (char)std::tolower((unsigned char)c);
	}
	return out;
}

std::string lowerCase(std::string text)
{
	for (char& c : text)
		c = (char)std::tolower((unsigned char)c);
	return text;
}

std::string clean(std::string text)
{
	for (char& c : text)
		if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
			c = '_';
	return text;
}

// The folders that may hold videos: beside the games and in every folder above them
// up to the share's or drive's top, in the layouts ES-DE and plain folders use.
std::vector<std::string> videoFolders()
{
	std::vector<std::string> bases{ ps5::rootDir };
	for (const std::string& dir : config::ContentPath.get())
		bases.push_back(dir);
	for (const std::string& dir : ps5::usbDirs)
		bases.push_back(dir);
	for (const std::string& dir : ps5::smb::gameFolders())
		bases.push_back(dir);
	std::vector<std::string> out;
	std::set<std::string> seen;
	auto add = [&](std::string dir) {
		while (dir.size() > 1 && dir.back() == '/')
			dir.pop_back();
		if (!dir.empty() && seen.insert(dir).second)
			out.push_back(dir);
	};
	static const char *const subs[] = {
		"videos", "media/videos", "media/dreamcast/videos", "media/naomi/videos", "media/atomiswave/videos",
		"downloaded_media/dreamcast/videos", "downloaded_media/naomi/videos", "downloaded_media/atomiswave/videos",
		"ES-DE/downloaded_media/dreamcast/videos", "ES-DE/downloaded_media/naomi/videos",
		"ES-DE/downloaded_media/atomiswave/videos", "PSFlyCast/videos"
	};
	for (std::string base : bases)
	{
		while (base.size() > 1 && base.back() == '/')
			base.pop_back();
		// smb://server/share is as far up as a network folder goes.
		size_t top = 1;
		if (base.rfind("smb://", 0) == 0)
		{
			const size_t serverEnd = base.find('/', 6);
			top = serverEnd == std::string::npos ? base.size() : base.find('/', serverEnd + 1);
			if (top == std::string::npos)
				top = base.size();
		}
		std::string dir = base;
		for (int level = 0; level < 5 && dir.size() >= top; level++)
		{
			for (const char *sub : subs)
				add(dir + "/" + sub);
			const size_t slash = dir.find_last_of('/');
			if (slash == std::string::npos || slash < top)
				break;
			dir.resize(slash);
		}
	}
	return out;
}

struct Index
{
	std::map<std::string, std::string> exact;		// lower case name without extension -> file
	std::map<std::string, std::string> loose;		// plain() of it -> file
	double at = 0;
	bool any = false;
};
std::map<std::string, Index> indexes;		// the finder's thread only

double secondsNow()
{
	using namespace std::chrono;
	return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

Index& indexOf(const std::string& folder)
{
	Index& index = indexes[folder];
	if (index.at != 0 && secondsNow() - index.at < 300)
		return index;
	index = Index();
	index.at = secondsNow();
	try
	{
		for (const hostfs::FileInfo& entry : hostfs::storage().listContent(folder))
		{
			if (entry.isDirectory)
				continue;
			const std::string lower = lowerCase(entry.name);
			const size_t dot = lower.find_last_of('.');
			if (dot == std::string::npos)
				continue;
			const std::string ext = lower.substr(dot);
			if (ext != ".mp4" && ext != ".m4v" && ext != ".mov")
				continue;
			const std::string stem = lower.substr(0, dot);
			const std::string file = folder + "/" + entry.name;
			index.exact.emplace(stem, file);
			const std::string loose = plain(entry.name.substr(0, dot));
			if (!loose.empty())
				index.loose.emplace(loose, file);
			index.any = true;
		}
	}
	catch (...) {}
	return index;
}

void finderRun()
{
	for (;;)
	{
		std::pair<std::string, std::string> job;
		{
			std::unique_lock<std::mutex> lock(findMutex);
			if (wanted.empty())
			{
				finding = false;
				return;
			}
			job = wanted.front();
			wanted.pop_front();
		}
		std::string result;
		int withVideos = 0;
		const std::vector<std::string> folders = videoFolders();
		const std::string names[2] = { job.first, job.second };
		for (const std::string& folder : folders)
		{
			const Index& index = indexOf(folder);
			if (index.any)
				withVideos++;
			for (int pass = 0; pass < 2 && result.empty(); pass++)
				for (const std::string& name : names)
				{
					if (name.empty())
						continue;
					const auto& table = pass == 0 ? index.exact : index.loose;
					const auto at = table.find(pass == 0 ? lowerCase(clean(name)) : plain(name));
					if (at != table.end())
					{
						result = at->second;
						break;
					}
				}
			if (!result.empty())
				break;
		}
		if (!result.empty())
			ps5::diag::mark("video: %s -> %s", job.first.c_str(), result.c_str());
		else
			ps5::diag::mark("video: none for \"%s\" / \"%s\" (%d of %d folders have videos; first: %s)", job.first.c_str(),
					job.second.c_str(), withVideos, (int)folders.size(), folders.empty() ? "-" : folders[0].c_str());
		std::lock_guard<std::mutex> lock(findMutex);
		found[job.first] = Found{ result, secondsNow() };
	}
}

} // namespace

bool available()
{
	return ensureLoaded();
}

std::string find(const std::string& stem, const std::string& title)
{
	std::unique_lock<std::mutex> lock(findMutex);
	const auto at = found.find(stem);
	if (at != found.end() && (!at->second.file.empty() || secondsNow() - at->second.at < 60))
		return at->second.file;
	for (const auto& job : wanted)
		if (job.first == stem)
			return "";
	wanted.emplace_back(stem, title);
	if (!finding)
	{
		finding = true;
		std::thread old = std::move(finder);
		lock.unlock();
		if (old.joinable())
			old.join();
		lock.lock();
		finder = std::thread(finderRun);
	}
	return "";
}

void play(const std::string& path, bool loop, float volume)
{
	state.wantedAt = state.frames;
	if (path == state.path && state.open)
	{
		if (volume != state.volume)
		{
			state.volume = volume;
			if (volume <= 0.f)
				ps5::sound::clearStream();
		}
		return;
	}
	closeNow();
	state.path = path;
	state.loop = loop;
	state.volume = volume;
	if (path.empty() || !ensureLoaded())
		return;
	std::string why;
	state.open = backend::open(path, loop, why);
	if (!state.open)
	{
		ps5::diag::mark("video: %s cannot be played: %s", path.substr(path.rfind('/') + 1).c_str(), why.c_str());
		state.finished = true;
	}
}

void stop()
{
	closeNow();
	state.path.clear();
}

bool ended()
{
	return state.finished;
}

void *picture(int& width, int& height)
{
	state.wantedAt = state.frames;
	if (!state.open || imguiDriver == nullptr)
		return nullptr;
	backend::Picture p;
	if (backend::take(p) && p.width > 0 && p.height > 0 && p.y != nullptr && p.uv != nullptr)
	{
		convert(p, state.pixels);
		if (state.textureMade)
			imguiDriver->deleteTexture(TextureName);
		state.texture = imguiDriver->updateTextureAndAspectRatio(TextureName, state.pixels.data(), p.width, p.height, false);
		state.textureMade = state.texture != ImTextureID();
		state.width = p.width;
		state.height = p.height;
		state.hasPicture = state.textureMade;
		if (state.hasPicture && state.pictures++ == 0)
			ps5::diag::mark("video: the first picture (%d x %d, %s)", p.width, p.height, backend::name());
	}
	if (state.volume > 0.f)
		backend::drainAudio([](const int16_t *samples, size_t frames, int channels, int rate) {
			ps5::sound::pushStream(samples, frames, channels, rate, state.volume);
		});
	else
		backend::drainAudio({});
	if (!backend::active())
		state.finished = true;
	width = state.width;
	height = state.height;
	return state.hasPicture ? (void *)state.texture : nullptr;
}

void frame()
{
	state.frames++;
	if (state.open && state.wantedAt + 2 < state.frames)
		stop();
}

std::string status()
{
	if (!state.loaded)
		return "Not used yet";
	if (!state.usable)
		return "Not available: " + state.why;
	return backend::name();
}

}
