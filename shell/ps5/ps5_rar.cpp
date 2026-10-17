/*
	PSFlyCast - RAR archives, for Flycast's archive reader (core/archive/archive.h).

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-3.0-or-later

	The unpacking is libunarr (LGPL-3.0, third_party/unarr); this file hands it
	Flycast's file stream and gives Flycast's Archive interface over it.
*/
#include "archive/archive.h"
#include "oslib/storage.h"

extern "C" {
#include "unarr.h"
#include "unarr-imp.h"
}

#include <cstring>

namespace
{
void streamClose(void *data)
{
	delete static_cast<hostfs::File *>(data);
}

size_t streamRead(void *data, void *buffer, size_t count)
{
	return static_cast<hostfs::File *>(data)->read(buffer, 1, count);
}

bool streamSeek(void *data, off64_t offset, int origin)
{
	return static_cast<hostfs::File *>(data)->seek(offset, origin) == 0;
}

off64_t streamTell(void *data)
{
	return static_cast<hostfs::File *>(data)->tell();
}

std::string normalised(const char *name)
{
	std::string s = name != nullptr ? name : "";
	for (char& c : s)
		if (c == '\\')
			c = '/';
	return s;
}

class RarFile final : public ArchiveFile
{
public:
	RarFile(std::vector<u8>&& data, const std::string& name) : data(std::move(data)), name(name) {}
	u32 Read(void *buffer, u32 length) override
	{
		length = (u32)std::min<size_t>(length, data.size() - position);
		memcpy(buffer, data.data() + position, length);
		position += length;
		return length;
	}
	size_t length() override { return data.size(); }
	const char *getName() override { return name.c_str(); }

private:
	std::vector<u8> data;
	std::string name;
	size_t position = 0;
};

class RarArchive final : public Archive
{
public:
	~RarArchive() override
	{
		if (archive != nullptr)
			ar_close_archive(archive);
		if (stream != nullptr)
			ar_close(stream);
	}

	bool list(std::vector<ArchiveEntry>& entries) override
	{
		if (!ar_parse_entry_at(archive, 0) && !ar_at_eof(archive))
			return false;
		while (!ar_at_eof(archive))
		{
			const std::string name = normalised(ar_entry_get_name(archive));
			if (!name.empty() && name.back() != '/')
			{
				ArchiveEntry entry;
				entry.name = name;
				entry.size = ar_entry_get_size(archive);
				entries.push_back(entry);
			}
			if (!ar_parse_entry(archive))
				break;
		}
		return true;
	}

	bool extract(const std::string& name, const std::function<bool(const void *, size_t)>& sink) override
	{
		if (!seekTo(name))
			return false;
		size_t left = ar_entry_get_size(archive);
		std::vector<u8> buffer(1 << 20);
		while (left > 0)
		{
			const size_t n = std::min(left, buffer.size());
			if (!ar_entry_uncompress(archive, buffer.data(), n))
				return false;
			if (!sink(buffer.data(), n))
				return false;
			left -= n;
		}
		return true;
	}

	ArchiveFile *OpenFile(const char *name) override
	{
		if (name == nullptr || !seekTo(name))
			return nullptr;
		std::vector<u8> data(ar_entry_get_size(archive));
		if (!data.empty() && !ar_entry_uncompress(archive, data.data(), data.size()))
			return nullptr;
		return new RarFile(std::move(data), name);
	}

	ArchiveFile *OpenFileByCrc(u32) override { return nullptr; }

protected:
	bool Open(hostfs::File *file) override
	{
		stream = ar_open_stream(file, streamClose, streamRead, streamSeek, streamTell);
		if (stream == nullptr)
			return false;
		archive = ar_open_rar_archive(stream);
		return archive != nullptr;
	}

private:
	bool seekTo(const std::string& name)
	{
		if (!ar_parse_entry_at(archive, 0) && !ar_at_eof(archive))
			return false;
		while (!ar_at_eof(archive))
		{
			if (normalised(ar_entry_get_name(archive)) == name)
				return true;
			if (!ar_parse_entry(archive))
				break;
		}
		return false;
	}

	ar_stream *stream = nullptr;
	ar_archive *archive = nullptr;
};
}

Archive *ps5RarArchive()
{
	return new RarArchive();
}
