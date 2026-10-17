/*
    Created on: Nov 22, 2018

	Copyright 2018 flyinghead

	This file is part of reicast.

    reicast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    reicast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with reicast.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "7zArchive.h"
#include "lzma/7z.h"
#include "lzma/7zCrc.h"
#include "lzma/Alloc.h"

#include <cstring>

#define kInputBufSize ((size_t)1 << 18)

static bool crc_tables_generated;

SRes SzArchive::ArchiveStream::Read(const ISeekInStream *p, void *buf, size_t *size)
{
	auto file_archive = CONTAINER_FROM_VTBL(p, ArchiveStream, vt);
	*size = file_archive->file->read(buf, 1, *size);
	return file_archive->file->error() ? SZ_ERROR_READ : SZ_OK;
}

SRes SzArchive::ArchiveStream::Seek(const ISeekInStream *p, Int64 *pos, ESzSeek origin)
{
	auto file_archive = CONTAINER_FROM_VTBL(p, ArchiveStream, vt);
	if (file_archive->file->seek(*pos, origin) == 0) {
		*pos = file_archive->file->tell();
		return SZ_OK;
	}
	else {
		return SZ_ERROR_FAIL;
	}
}

bool SzArchive::Open(hostfs::File *file)
{
	SzArEx_Init(&szarchive);

	if (archiveStream.file != nullptr)
		delete archiveStream.file;
	archiveStream.vt.Read = ArchiveStream::Read;
	archiveStream.vt.Seek = ArchiveStream::Seek;
	archiveStream.file = file;

	LookToRead2_CreateVTable(&lookStream, 0);
	lookStream.buf = (Byte *)ISzAlloc_Alloc(&g_Alloc, kInputBufSize);
	if (lookStream.buf == NULL)
	{
		delete archiveStream.file;
		return false;
	}
	lookStream.bufSize = kInputBufSize;
	lookStream.realStream = &archiveStream.vt;
	LookToRead2_Init(&lookStream);

	if (!crc_tables_generated)
	{
		CrcGenerateTable();
		crc_tables_generated = true;
	}
	SRes res = SzArEx_Open(&szarchive, &lookStream.vt, &g_Alloc, &g_Alloc);

	return (res == SZ_OK);
}

ArchiveFile* SzArchive::OpenFile(const char* name)
{
	u16 fname[512];
	for (UInt32 i = 0; i < szarchive.NumFiles; i++)
	{
		if (SzArEx_IsDir(&szarchive, i))
			continue;

		size_t len = SzArEx_GetFileNameUtf16(&szarchive, i, fname);
		char szname[512];
		size_t j = 0;
		for (; j < len && j < sizeof(szname) - 1; j++)
			szname[j] = fname[j];
		szname[j] = 0;
		if (strcmp(name, szname))
			continue;

		size_t offset = 0;
		size_t out_size_processed = 0;
		SRes res = SzArEx_Extract(&szarchive, &lookStream.vt, i, &block_idx, &out_buffer, &out_buffer_size, &offset, &out_size_processed, &g_Alloc, &g_Alloc);
		if (res != SZ_OK)
			return NULL;

		return new SzArchiveFile(out_buffer, offset, (u32)out_size_processed);
	}
	return NULL;
}

ArchiveFile* SzArchive::OpenFileByCrc(u32 crc)
{
	if (crc == 0)
		return NULL;
	for (UInt32 i = 0; i < szarchive.NumFiles; i++)
	{
		unsigned isDir = SzArEx_IsDir(&szarchive, i);
		if (isDir)
			continue;

		if (crc != szarchive.CRCs.Vals[i])
			continue;

		size_t offset = 0;
		size_t out_size_processed = 0;
		SRes res = SzArEx_Extract(&szarchive, &lookStream.vt, i, &block_idx, &out_buffer, &out_buffer_size, &offset, &out_size_processed, &g_Alloc, &g_Alloc);
		if (res != SZ_OK)
			return NULL;

		return new SzArchiveFile(out_buffer, offset, (u32)out_size_processed);
	}
	return NULL;
}

SzArchive::~SzArchive()
{
	if (lookStream.buf != NULL)
	{
		delete archiveStream.file;
		ISzAlloc_Free(&g_Alloc, lookStream.buf);
		if (out_buffer != NULL)
			ISzAlloc_Free(&g_Alloc, out_buffer);
		SzArEx_Free(&szarchive, &g_Alloc);
	}
}

static std::string szName(CSzArEx *archive, UInt32 index)
{
	std::vector<u16> name(SzArEx_GetFileNameUtf16(archive, index, nullptr));
	if (name.empty())
		return std::string();
	SzArEx_GetFileNameUtf16(archive, index, name.data());
	// UTF-16 to UTF-8.
	std::string out;
	for (size_t i = 0; i + 1 < name.size(); i++)
	{
		u32 c = name[i];
		if (c >= 0xD800 && c < 0xDC00 && i + 2 < name.size())
			c = 0x10000 + ((c - 0xD800) << 10) + (name[++i] - 0xDC00);
		if (c == '\\')
			c = '/';
		if (c < 0x80)
			out += (char)c;
		else if (c < 0x800)
		{
			out += (char)(0xC0 | (c >> 6));
			out += (char)(0x80 | (c & 0x3F));
		}
		else if (c < 0x10000)
		{
			out += (char)(0xE0 | (c >> 12));
			out += (char)(0x80 | ((c >> 6) & 0x3F));
			out += (char)(0x80 | (c & 0x3F));
		}
		else
		{
			out += (char)(0xF0 | (c >> 18));
			out += (char)(0x80 | ((c >> 12) & 0x3F));
			out += (char)(0x80 | ((c >> 6) & 0x3F));
			out += (char)(0x80 | (c & 0x3F));
		}
	}
	return out;
}

bool SzArchive::list(std::vector<ArchiveEntry>& entries)
{
	for (UInt32 i = 0; i < szarchive.NumFiles; i++)
	{
		if (SzArEx_IsDir(&szarchive, i))
			continue;
		ArchiveEntry entry;
		entry.name = szName(&szarchive, i);
		entry.size = SzArEx_GetFileSize(&szarchive, i);
		if (!entry.name.empty())
			entries.push_back(entry);
	}
	return true;
}

bool SzArchive::extract(const std::string& name, const std::function<bool(const void *, size_t)>& sink)
{
	for (UInt32 i = 0; i < szarchive.NumFiles; i++)
	{
		if (SzArEx_IsDir(&szarchive, i) || szName(&szarchive, i) != name)
			continue;
		// The block the file is in (a solid archive: many files) is unpacked into memory at once.
		size_t offset = 0;
		size_t processed = 0;
		SRes res = SzArEx_Extract(&szarchive, &lookStream.vt, i, &block_idx, &out_buffer, &out_buffer_size, &offset, &processed, &g_Alloc, &g_Alloc);
		if (res != SZ_OK)
			return false;
		const size_t chunk = 1 << 20;
		for (size_t done = 0; done < processed; done += chunk)
			if (!sink(out_buffer + offset + done, std::min(chunk, processed - done)))
				return false;
		return true;
	}
	return false;
}
