/*
** nk_diskcache.cpp
** Simple decoded texture disk cache for Nakara.
**
** This cache stores CPU-side BGRA pixels only. It does not cache OpenGL or
** Vulkan GPU resources, so it is renderer independent.
*/

#include <stdio.h>
#include <string.h>
#include <vector>
#include <string>
#include <stdint.h>
#include <chrono>
#include <memory>
#include <mutex>
#include <atomic>
#include <miniz.h>

#include "c_cvars.h"
#include "c_dispatch.h"
#include "cmdlib.h"
#include "filesystem.h"
#include "fs_findfile.h"
#include "i_specialpaths.h"
#include "files.h"
#include "bitmap.h"
#include "m_crc32.h"
#include "printf.h"
#include "nk_diskcache.h"

#ifndef MAKE_ID
#ifndef __BIG_ENDIAN__
#define MAKE_ID(a,b,c,d)	((uint32_t)((a)|((b)<<8)|((c)<<16)|((d)<<24)))
#else
#define MAKE_ID(a,b,c,d)	((uint32_t)((d)|((c)<<8)|((b)<<16)|((a)<<24)))
#endif
#endif

CUSTOM_CVAR(Bool, nk_disk_cache, false, CVAR_GLOBALCONFIG)
{
	if (self) self = false;
}
CVAR(Bool, nk_disk_cache_debug, false, CVAR_GLOBALCONFIG)
CVAR(Bool, nk_disk_cache_profile, false, CVAR_GLOBALCONFIG)
CVAR(Bool, nk_disk_cache_sprites_only, true, CVAR_GLOBALCONFIG)
CVAR(Int, nk_disk_cache_min_kb, 256, CVAR_GLOBALCONFIG)

static const uint32_t NK_DISK_CACHE_MAGIC = MAKE_ID('N', 'K', 'D', 'C');
static const uint32_t NK_DISK_CACHE_FORMAT_VERSION = 3;

struct NKDiskCacheHeader
{
	uint32_t Magic;
	uint32_t Version;
	uint32_t Width;
	uint32_t Height;
	uint32_t Frame;
	uint32_t SourceHash;
	uint32_t NameHash;
	int32_t Trans;
	uint32_t RawSize;
	uint32_t StoredSize;
};

struct NKDiskCacheEntry
{
	std::string ResourceId;
	uint32_t SourceHash;
	uint32_t NameHash;
	int Width;
	int Height;
	int Frame;
	int Trans;
	uint32_t Version;
	std::string CachePath;
};

struct NKDiskCacheSourceHashEntry
{
	int SourceLump;
	uint32_t SourceHash;
};

static std::vector<NKDiskCacheSourceHashEntry> NKSourceHashCache;
static std::vector<NKDiskCacheEntry> NKCacheIndex;
static std::vector<std::string> NKDeleteQueue;
static bool NKCacheIndexLoaded = false;
static bool NKCacheIndexDirty = false;

// [NKS] Disk cache can now be touched by the experimental CPU warmup worker.
// Keep all cache index/hash/stat/file-state mutations behind one lock first.
// This intentionally favours correctness over parallel throughput. The worker
// remains optional and GPU upload is still main-thread only.
static std::recursive_mutex NKDiskCacheMutex;
static std::atomic<uint64_t> NKDiskCacheTempSerial(0);

struct NKDiskCacheStats
{
	uint64_t TryCalls;
	uint64_t TryHits;
	uint64_t TryMisses;
	uint64_t SaveCalls;
	uint64_t SaveWrites;
	uint64_t SaveSkippedExisting;
	uint64_t SourceHashCalls;
	uint64_t SourceHashCacheHits;
	uint64_t SourceHashReads;

	uint64_t HitRawBytes;
	uint64_t HitStoredBytes;
	uint64_t SavedRawBytes;
	uint64_t SavedStoredBytes;

	uint64_t SourceHashUS;
	uint64_t HitReadUS;
	uint64_t HitUncompressUS;
	uint64_t HitTotalUS;
	uint64_t SaveCompressUS;
	uint64_t SaveWriteUS;
	uint64_t SaveTotalUS;
	uint64_t CleanupUS;
};

static NKDiskCacheStats NKStats;

static uint64_t NKDiskCache_NowUS()
{
	using namespace std::chrono;
	return (uint64_t)duration_cast<microseconds>(high_resolution_clock::now().time_since_epoch()).count();
}

static double NKDiskCache_ToMS(uint64_t us)
{
	return (double)us / 1000.0;
}

static void NKDiskCache_PrintStats()
{
	Printf("\n[NKDiskCache] ===== profile stats =====\n");
	Printf("[NKDiskCache] try: calls=%llu hits=%llu misses=%llu\n",
		(unsigned long long)NKStats.TryCalls,
		(unsigned long long)NKStats.TryHits,
		(unsigned long long)NKStats.TryMisses);
	Printf("[NKDiskCache] save: calls=%llu writes=%llu skipped_existing=%llu\n",
		(unsigned long long)NKStats.SaveCalls,
		(unsigned long long)NKStats.SaveWrites,
		(unsigned long long)NKStats.SaveSkippedExisting);
	Printf("[NKDiskCache] source hash: calls=%llu cache_hits=%llu reads=%llu time=%.3f ms\n",
		(unsigned long long)NKStats.SourceHashCalls,
		(unsigned long long)NKStats.SourceHashCacheHits,
		(unsigned long long)NKStats.SourceHashReads,
		NKDiskCache_ToMS(NKStats.SourceHashUS));
	Printf("[NKDiskCache] hit read: stored=%llu KB raw=%llu KB read=%.3f ms uncompress=%.3f ms total=%.3f ms\n",
		(unsigned long long)((NKStats.HitStoredBytes + 1023ull) / 1024ull),
		(unsigned long long)((NKStats.HitRawBytes + 1023ull) / 1024ull),
		NKDiskCache_ToMS(NKStats.HitReadUS),
		NKDiskCache_ToMS(NKStats.HitUncompressUS),
		NKDiskCache_ToMS(NKStats.HitTotalUS));
	Printf("[NKDiskCache] save write: stored=%llu KB raw=%llu KB compress=%.3f ms write=%.3f ms total=%.3f ms\n",
		(unsigned long long)((NKStats.SavedStoredBytes + 1023ull) / 1024ull),
		(unsigned long long)((NKStats.SavedRawBytes + 1023ull) / 1024ull),
		NKDiskCache_ToMS(NKStats.SaveCompressUS),
		NKDiskCache_ToMS(NKStats.SaveWriteUS),
		NKDiskCache_ToMS(NKStats.SaveTotalUS));
	Printf("[NKDiskCache] cleanup: %.3f ms\n", NKDiskCache_ToMS(NKStats.CleanupUS));
	Printf("[NKDiskCache] =========================\n\n");
}

CCMD(nk_disk_cache_stats)
{
	std::lock_guard<std::recursive_mutex> lock(NKDiskCacheMutex);
	NKDiskCache_PrintStats();
}

CCMD(nk_disk_cache_reset_stats)
{
	std::lock_guard<std::recursive_mutex> lock(NKDiskCacheMutex);
	NKStats = NKDiskCacheStats();
	Printf("[NKDiskCache] profile stats reset.\n");
}


static FString NKDiskCache_GetBasePath(bool create)
{
	FString path = NicePath("$PROGDIR/cache/nakara/texturecache");
	FixPathSeperator(path);
	if (create)
	{
		CreatePath(path.GetChars());
	}
	return path;
}

static FString NKDiskCache_GetIndexPath(bool create)
{
	FString path = NKDiskCache_GetBasePath(create);
	path += "/index.tsv";
	return path;
}

static uint32_t NKDiskCache_HashString(const char *text)
{
	if (text == nullptr) return 0;
	return CalcCRC32((const uint8_t*)text, (unsigned int)strlen(text));
}

static bool NKDiskCache_GetResourceId(int sourceLump, FString &resourceId)
{
	if (sourceLump < 0) return false;

	const char *container = fileSystem.GetResourceFileFullName(fileSystem.GetFileContainer(sourceLump));
	const char *fullname = fileSystem.GetFileFullName(sourceLump, false);
	if (container == nullptr || fullname == nullptr) return false;

	resourceId.Format("%s|%s", container, fullname);
	FixPathSeperator(resourceId);
	return true;
}


static bool NKDiskCache_IsAllowedResource(const FString &resourceId, int width, int height)
{
	uint64_t byteSize = (uint64_t)width * (uint64_t)height * 4ull;
	int minKb = nk_disk_cache_min_kb;
	if (minKb < 0) minKb = 0;
	if (byteSize < (uint64_t)minKb * 1024ull)
	{
		return false;
	}

	if (!nk_disk_cache_sprites_only)
	{
		return true;
	}

	const char *path = resourceId.GetChars();
	const char *separator = strchr(path, '|');
	if (separator != nullptr)
	{
		path = separator + 1;
	}

	while (*path == '/' || *path == '\\')
	{
		path++;
	}

	// Keep the first pass intentionally narrow. The current cache target is
	// large sprite frames, not UI, fonts, debug graphics, status bar graphics,
	// or general textures.
	return strnicmp(path, "sprites/", 8) == 0 || strnicmp(path, "sprites\\", 8) == 0;
}

static bool NKDiskCache_GetSourceHash(int sourceLump, uint32_t &sourceHash)
{
	if (sourceLump < 0) return false;

	if (nk_disk_cache_profile)
	{
		NKStats.SourceHashCalls++;
	}

	for (auto &entry : NKSourceHashCache)
	{
		if (entry.SourceLump == sourceLump)
		{
			sourceHash = entry.SourceHash;
			if (nk_disk_cache_profile)
			{
				NKStats.SourceHashCacheHits++;
			}
			return true;
		}
	}

	uint64_t startUS = NKDiskCache_NowUS();

	auto reader = fileSystem.OpenFileReader(sourceLump);
	if (!reader.isOpen()) return false;

	auto length = reader.GetLength();
	if (length <= 0 || length > 1024 * 1024 * 256) return false;

	std::vector<uint8_t> data;
	data.resize((size_t)length);
	if (reader.Read(data.data(), length) != length) return false;

	sourceHash = CalcCRC32(data.data(), (unsigned int)data.size());

	if (nk_disk_cache_profile)
	{
		NKStats.SourceHashReads++;
		NKStats.SourceHashUS += NKDiskCache_NowUS() - startUS;
	}

	NKDiskCacheSourceHashEntry cacheEntry;
	cacheEntry.SourceLump = sourceLump;
	cacheEntry.SourceHash = sourceHash;
	NKSourceHashCache.push_back(cacheEntry);
	return true;
}

static FString NKDiskCache_MakeCachePath(uint32_t nameHash, uint32_t sourceHash, int width, int height, int frame)
{
	FString path = NKDiskCache_GetBasePath(true);
	FString file;
	file.Format("/%08x_%08x_%dx%d_f%d.nktex", nameHash, sourceHash, width, height, frame);
	path += file;
	return path;
}

static void NKDiskCache_QueueDelete(const std::string &path)
{
	if (path.empty()) return;
	for (auto &queued : NKDeleteQueue)
	{
		if (queued == path) return;
	}
	NKDeleteQueue.push_back(path);
	if (nk_disk_cache_debug)
	{
		Printf("[NKDiskCache] queued old cache for shutdown delete: %s\n", path.c_str());
	}
}


static bool NKDiskCache_IsIndexedCachePath(const char *path)
{
	if (path == nullptr || *path == 0) return false;

	FString normalized = path;
	FixPathSeperator(normalized);

	for (auto &entry : NKCacheIndex)
	{
		FString entryPath = entry.CachePath.c_str();
		FixPathSeperator(entryPath);
		if (stricmp(entryPath.GetChars(), normalized.GetChars()) == 0)
		{
			return true;
		}
	}
	return false;
}

static bool NKDiskCache_ReadHeaderVersion(const char *path, uint32_t &version)
{
	version = 0;
	FileReader reader;
	if (!reader.OpenFile(path))
	{
		if (nk_disk_cache_debug)
		{
			Printf("[NKDiskCache] header open failed: %s\n", path);
		}
		return false;
	}

	NKDiskCacheHeader header;
	bool ok = reader.Read(&header, sizeof(header)) == sizeof(header);
	reader.Close();

	if (!ok || header.Magic != NK_DISK_CACHE_MAGIC)
	{
		return false;
	}

	version = header.Version;
	return true;
}

static void NKDiskCache_CleanupStartupCacheFiles()
{
	uint64_t cleanupStartUS = NKDiskCache_NowUS();

	FString basePath = NKDiskCache_GetBasePath(false);
	if (!DirExists(basePath.GetChars()))
	{
		return;
	}

	FileSys::FileList tempFiles;
	if (FileSys::ScanDirectory(tempFiles, basePath.GetChars(), "*.tmp*", true, true))
	{
		for (auto &entry : tempFiles)
		{
			RemoveFile(entry.FilePath.c_str());
			if (nk_disk_cache_debug)
			{
				Printf("[NKDiskCache] deleted temp cache file on startup: %s\n", entry.FilePath.c_str());
			}
		}
	}

	FileSys::FileList cacheFiles;
	if (!FileSys::ScanDirectory(cacheFiles, basePath.GetChars(), "*.nktex", true, true))
	{
		return;
	}

	for (auto &entry : cacheFiles)
	{
		uint32_t version = 0;
		bool hasValidHeader = NKDiskCache_ReadHeaderVersion(entry.FilePath.c_str(), version);
		bool indexed = NKDiskCache_IsIndexedCachePath(entry.FilePath.c_str());

		if (!hasValidHeader || version != NK_DISK_CACHE_FORMAT_VERSION || !indexed)
		{
			RemoveFile(entry.FilePath.c_str());
			if (nk_disk_cache_debug)
			{
				if (!hasValidHeader)
				{
					Printf("[NKDiskCache] deleted invalid cache file on startup: %s\n", entry.FilePath.c_str());
				}
				else if (version != NK_DISK_CACHE_FORMAT_VERSION)
				{
					Printf("[NKDiskCache] deleted old v%u cache file on startup: %s\n", version, entry.FilePath.c_str());
				}
				else
				{
					Printf("[NKDiskCache] deleted orphan cache file on startup: %s\n", entry.FilePath.c_str());
				}
			}
		}
	}

	if (nk_disk_cache_profile)
	{
		NKStats.CleanupUS += NKDiskCache_NowUS() - cleanupStartUS;
	}
}

static void NKDiskCache_LoadIndex()
{
	if (NKCacheIndexLoaded) return;
	NKCacheIndexLoaded = true;

	FString indexPath = NKDiskCache_GetIndexPath(false);
	FileReader reader;
	if (!reader.OpenFile(indexPath.GetChars()))
	{
		if (nk_disk_cache_debug)
		{
			Printf("[NKDiskCache] index open failed: %s\n", indexPath.GetChars());
		}
		NKDiskCache_CleanupStartupCacheFiles();
		return;
	}

	auto length = reader.GetLength();
	if (length <= 0 || length > 1024 * 1024 * 64)
	{
		reader.Close();
		NKDiskCache_CleanupStartupCacheFiles();
		return;
	}

	std::vector<char> fileData;
	fileData.resize((size_t)length + 1);
	if (reader.Read(fileData.data(), length) != length)
	{
		reader.Close();
		NKDiskCache_CleanupStartupCacheFiles();
		return;
	}
	reader.Close();
	fileData[(size_t)length] = 0;

	char *line = fileData.data();
	while (line != nullptr && *line != 0)
	{
		char *nextLine = strchr(line, '\n');
		if (nextLine != nullptr)
		{
			*nextLine = 0;
			nextLine++;
		}

		char *p = strchr(line, '\r');
		if (p) *p = 0;

		if (*line != 0)
		{
			std::vector<char*> fields;
			char *cursor = line;
			fields.push_back(cursor);
			while ((p = strchr(cursor, '\t')) != nullptr)
			{
				*p = 0;
				cursor = p + 1;
				fields.push_back(cursor);
			}

			if (fields.size() == 9)
			{
				NKDiskCacheEntry entry;
				entry.Version = (uint32_t)strtoul(fields[0], nullptr, 10);
				entry.SourceHash = (uint32_t)strtoul(fields[1], nullptr, 16);
				entry.NameHash = (uint32_t)strtoul(fields[2], nullptr, 16);
				entry.Width = atoi(fields[3]);
				entry.Height = atoi(fields[4]);
				entry.Frame = atoi(fields[5]);
				entry.Trans = atoi(fields[6]);
				entry.CachePath = fields[7];
				entry.ResourceId = fields[8];

				if (!FileExists(entry.CachePath.c_str()))
				{
					NKCacheIndexDirty = true;
				}
				else if (entry.Version != NK_DISK_CACHE_FORMAT_VERSION)
				{
					NKDiskCache_QueueDelete(entry.CachePath);
					NKCacheIndexDirty = true;
				}
				else
				{
					NKCacheIndex.push_back(entry);
				}
			}
		}

		line = nextLine;
	}

	NKDiskCache_CleanupStartupCacheFiles();
}

static FString NKDiskCache_MakeTempPath(const char *finalPath)
{
	FString tempPath;
	uint64_t serial = NKDiskCacheTempSerial.fetch_add(1);
	tempPath.Format("%s.tmp.%llu", finalPath, (unsigned long long)serial);
	return tempPath;
}

static bool NKDiskCache_ReplaceFile(const char *tempPath, const char *finalPath)
{
	RemoveFile(finalPath);
	if (rename(tempPath, finalPath) != 0)
	{
		RemoveFile(tempPath);
		if (nk_disk_cache_debug)
		{
			Printf("[NKDiskCache] rename failed: %s -> %s\n", tempPath, finalPath);
		}
		return false;
	}
	return true;
}

static void NKDiskCache_SaveIndex()
{
	if (!NKCacheIndexDirty) return;

	FString indexPath = NKDiskCache_GetIndexPath(true);
	FString tempPath = NKDiskCache_MakeTempPath(indexPath.GetChars());
	std::unique_ptr<FileWriter> fp(FileWriter::Open(tempPath.GetChars()));
	if (fp == nullptr)
	{
		if (nk_disk_cache_debug)
		{
			Printf("[NKDiskCache] index write open failed: %s\n", tempPath.GetChars());
		}
		return;
	}

	for (auto &entry : NKCacheIndex)
	{
		fp->Printf("%u\t%08x\t%08x\t%d\t%d\t%d\t%d\t%s\t%s\n",
			entry.Version,
			entry.SourceHash,
			entry.NameHash,
			entry.Width,
			entry.Height,
			entry.Frame,
			entry.Trans,
			entry.CachePath.c_str(),
			entry.ResourceId.c_str());
	}
	fp->Close();

	if (!NKDiskCache_ReplaceFile(tempPath.GetChars(), indexPath.GetChars()))
	{
		return;
	}

	NKCacheIndexDirty = false;
}

static int NKDiskCache_FindEntry(const std::string &resourceId, int width, int height, int frame)
{
	for (unsigned i = 0; i < NKCacheIndex.size(); i++)
	{
		auto &entry = NKCacheIndex[i];
		if (entry.ResourceId == resourceId && entry.Width == width && entry.Height == height && entry.Frame == frame)
		{
			return (int)i;
		}
	}
	return -1;
}

static bool NKDiskCache_BuildIdentity(int sourceLump, int width, int height, int frame, FString &resourceId, uint32_t &sourceHash, uint32_t &nameHash, FString &cachePath)
{
	if (!nk_disk_cache) return false;
	if (width <= 0 || height <= 0) return false;
	if (!NKDiskCache_GetResourceId(sourceLump, resourceId)) return false;
	if (!NKDiskCache_IsAllowedResource(resourceId, width, height)) return false;
	if (!NKDiskCache_GetSourceHash(sourceLump, sourceHash)) return false;

	nameHash = NKDiskCache_HashString(resourceId.GetChars());
	cachePath = NKDiskCache_MakeCachePath(nameHash, sourceHash, width, height, frame);
	return true;
}

void NKDiskCache_Init()
{
	std::lock_guard<std::recursive_mutex> lock(NKDiskCacheMutex);
	if (!nk_disk_cache)
	{
		return;
	}
	NKDiskCache_LoadIndex();
}

bool NKDiskCache_TryLoadBitmap(int sourceLump, int width, int height, int frame, FBitmap &bitmap, int *trans)
{
	std::lock_guard<std::recursive_mutex> lock(NKDiskCacheMutex);
	uint64_t totalStartUS = NKDiskCache_NowUS();
	if (nk_disk_cache_profile)
	{
		NKStats.TryCalls++;
	}

	FString resourceId;
	FString cachePath;
	uint32_t sourceHash = 0;
	uint32_t nameHash = 0;

	if (!NKDiskCache_BuildIdentity(sourceLump, width, height, frame, resourceId, sourceHash, nameHash, cachePath))
	{
		if (nk_disk_cache_profile) NKStats.TryMisses++;
		return false;
	}
	NKDiskCache_LoadIndex();

	int index = NKDiskCache_FindEntry(resourceId.GetChars(), width, height, frame);
	if (index >= 0)
	{
		auto &entry = NKCacheIndex[index];
		if (entry.Version != NK_DISK_CACHE_FORMAT_VERSION || entry.SourceHash != sourceHash || entry.NameHash != nameHash)
		{
			NKDiskCache_QueueDelete(entry.CachePath);
			NKCacheIndex.erase(NKCacheIndex.begin() + index);
			NKCacheIndexDirty = true;
			return false;
		}
		cachePath = entry.CachePath.c_str();
	}

	FileReader reader;
	if (!reader.OpenFile(cachePath.GetChars()))
	{
		if (index >= 0)
		{
			NKCacheIndex.erase(NKCacheIndex.begin() + index);
			NKCacheIndexDirty = true;
		}
		if (nk_disk_cache_debug)
		{
			Printf("[NKDiskCache] cache read open failed: %s\n", cachePath.GetChars());
		}
		if (nk_disk_cache_profile) NKStats.TryMisses++;
		return false;
	}

	uint64_t readStartUS = NKDiskCache_NowUS();

	NKDiskCacheHeader header;
	if (reader.Read(&header, sizeof(header)) != sizeof(header))
	{
		reader.Close();
		NKDiskCache_QueueDelete(cachePath.GetChars());
		return false;
	}

	uint32_t expectedSize = (uint32_t)(width * height * 4);
	if (header.Magic != NK_DISK_CACHE_MAGIC || header.Version != NK_DISK_CACHE_FORMAT_VERSION ||
		header.Width != (uint32_t)width || header.Height != (uint32_t)height || header.Frame != (uint32_t)frame ||
		header.SourceHash != sourceHash || header.NameHash != nameHash || header.RawSize != expectedSize ||
		header.StoredSize == 0)
	{
		reader.Close();
		NKDiskCache_QueueDelete(cachePath.GetChars());
		return false;
	}

	std::vector<uint8_t> compressed;
	compressed.resize(header.StoredSize);
	if (reader.Read(compressed.data(), header.StoredSize) != header.StoredSize)
	{
		reader.Close();
		NKDiskCache_QueueDelete(cachePath.GetChars());
		return false;
	}
	reader.Close();

	if (nk_disk_cache_profile)
	{
		NKStats.HitReadUS += NKDiskCache_NowUS() - readStartUS;
	}

	bitmap.Create(width, height);
	mz_ulong rawSize = (mz_ulong)expectedSize;
	uint64_t uncompressStartUS = NKDiskCache_NowUS();
	if (mz_uncompress((unsigned char*)bitmap.GetPixels(), &rawSize, compressed.data(), (mz_ulong)header.StoredSize) != MZ_OK || rawSize != expectedSize)
	{
		bitmap = FBitmap();
		NKDiskCache_QueueDelete(cachePath.GetChars());
		if (nk_disk_cache_profile) NKStats.TryMisses++;
		return false;
	}

	if (nk_disk_cache_profile)
	{
		NKStats.HitUncompressUS += NKDiskCache_NowUS() - uncompressStartUS;
		NKStats.HitTotalUS += NKDiskCache_NowUS() - totalStartUS;
		NKStats.HitRawBytes += header.RawSize;
		NKStats.HitStoredBytes += header.StoredSize;
		NKStats.TryHits++;
	}

	if (trans) *trans = header.Trans;
	if (nk_disk_cache_debug)
	{
		Printf("[NKDiskCache] hit: %s\n", resourceId.GetChars());
	}
	return true;
}

void NKDiskCache_SaveBitmap(int sourceLump, int width, int height, int frame, const FBitmap &bitmap, int trans)
{
	std::lock_guard<std::recursive_mutex> lock(NKDiskCacheMutex);
	uint64_t totalStartUS = NKDiskCache_NowUS();
	if (nk_disk_cache_profile)
	{
		NKStats.SaveCalls++;
	}

	FString resourceId;
	FString cachePath;
	uint32_t sourceHash = 0;
	uint32_t nameHash = 0;

	if (!NKDiskCache_BuildIdentity(sourceLump, width, height, frame, resourceId, sourceHash, nameHash, cachePath)) return;
	if (bitmap.GetPixels() == nullptr) return;

	NKDiskCache_LoadIndex();

	int oldIndex = NKDiskCache_FindEntry(resourceId.GetChars(), width, height, frame);
	std::string oldPath;
	if (oldIndex >= 0)
	{
		auto &oldEntry = NKCacheIndex[oldIndex];
		if (oldEntry.Version == NK_DISK_CACHE_FORMAT_VERSION && oldEntry.SourceHash == sourceHash && oldEntry.NameHash == nameHash && oldEntry.CachePath == cachePath.GetChars() && FileExists(cachePath.GetChars()))
		{
			if (nk_disk_cache_profile)
			{
				NKStats.SaveSkippedExisting++;
			}
			return;
		}
		oldPath = oldEntry.CachePath;
	}

	NKDiskCacheHeader header;
	header.Magic = NK_DISK_CACHE_MAGIC;
	header.Version = NK_DISK_CACHE_FORMAT_VERSION;
	header.Width = (uint32_t)width;
	header.Height = (uint32_t)height;
	header.Frame = (uint32_t)frame;
	header.SourceHash = sourceHash;
	header.NameHash = nameHash;
	header.Trans = trans;
	header.RawSize = (uint32_t)(width * height * 4);
	header.StoredSize = 0;

	std::vector<uint8_t> compressed;
	compressed.resize(mz_compressBound((mz_ulong)header.RawSize));
	mz_ulong compressedSize = (mz_ulong)compressed.size();
	uint64_t compressStartUS = NKDiskCache_NowUS();
	if (mz_compress2(compressed.data(), &compressedSize, (const unsigned char*)bitmap.GetPixels(), (mz_ulong)header.RawSize, MZ_BEST_SPEED) != MZ_OK)
	{
		return;
	}
	header.StoredSize = (uint32_t)compressedSize;

	if (nk_disk_cache_profile)
	{
		NKStats.SaveCompressUS += NKDiskCache_NowUS() - compressStartUS;
	}

	uint64_t writeStartUS = NKDiskCache_NowUS();
	FString tempPath = NKDiskCache_MakeTempPath(cachePath.GetChars());
	std::unique_ptr<FileWriter> fp(FileWriter::Open(tempPath.GetChars()));
	if (fp == nullptr)
	{
		if (nk_disk_cache_debug)
		{
			Printf("[NKDiskCache] cache write open failed: %s\n", tempPath.GetChars());
		}
		return;
	}

	bool ok = fp->Write(&header, sizeof(header)) == sizeof(header);
	if (ok) ok = fp->Write(compressed.data(), header.StoredSize) == header.StoredSize;
	fp->Close();

	if (nk_disk_cache_profile)
	{
		NKStats.SaveWriteUS += NKDiskCache_NowUS() - writeStartUS;
	}

	if (!ok)
	{
		RemoveFile(tempPath.GetChars());
		return;
	}

	if (!NKDiskCache_ReplaceFile(tempPath.GetChars(), cachePath.GetChars()))
	{
		return;
	}

	if (oldIndex >= 0)
	{
		NKCacheIndex.erase(NKCacheIndex.begin() + oldIndex);
	}

	NKDiskCacheEntry entry;
	entry.ResourceId = resourceId.GetChars();
	entry.SourceHash = sourceHash;
	entry.NameHash = nameHash;
	entry.Width = width;
	entry.Height = height;
	entry.Frame = frame;
	entry.Trans = trans;
	entry.Version = NK_DISK_CACHE_FORMAT_VERSION;
	entry.CachePath = cachePath.GetChars();
	NKCacheIndex.push_back(entry);
	NKCacheIndexDirty = true;

	if (!oldPath.empty() && oldPath != entry.CachePath)
	{
		NKDiskCache_QueueDelete(oldPath);
	}

	if (nk_disk_cache_profile)
	{
		NKStats.SaveWrites++;
		NKStats.SavedRawBytes += header.RawSize;
		NKStats.SavedStoredBytes += header.StoredSize;
		NKStats.SaveTotalUS += NKDiskCache_NowUS() - totalStartUS;
	}

	if (nk_disk_cache_debug)
	{
		Printf("[NKDiskCache] saved: %s raw=%uKB compressed=%uKB ratio=%u%%\n",
			resourceId.GetChars(),
			(header.RawSize + 1023) / 1024,
			(header.StoredSize + 1023) / 1024,
			header.RawSize > 0 ? (unsigned)((uint64_t)header.StoredSize * 100ull / (uint64_t)header.RawSize) : 0u);
	}
}

void NKDiskCache_Shutdown()
{
	std::lock_guard<std::recursive_mutex> lock(NKDiskCacheMutex);
	if (!NKCacheIndexLoaded) return;

	for (auto &path : NKDeleteQueue)
	{
		if (!path.empty())
		{
			RemoveFile(path.c_str());
			if (nk_disk_cache_debug)
			{
				Printf("[NKDiskCache] deleted old cache: %s\n", path.c_str());
			}
		}
	}
	NKDeleteQueue.clear();
	NKDiskCache_SaveIndex();

	if (nk_disk_cache_profile)
	{
		NKDiskCache_PrintStats();
	}

	NKSourceHashCache.clear();
}
