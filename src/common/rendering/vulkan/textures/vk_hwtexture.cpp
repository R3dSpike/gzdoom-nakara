/*
**  Vulkan backend
**  Copyright (c) 2016-2020 Magnus Norddahl
**
**  This software is provided 'as-is', without any express or implied
**  warranty.  In no event will the authors be held liable for any damages
**  arising from the use of this software.
**
**  Permission is granted to anyone to use this software for any purpose,
**  including commercial applications, and to alter it and redistribute it
**  freely, subject to the following restrictions:
**
**  1. The origin of this software must not be misrepresented; you must not
**     claim that you wrote the original software. If you use this software
**     in a product, an acknowledgment in the product documentation would be
**     appreciated but is not required.
**  2. Altered source versions must be plainly marked as such, and must not be
**     misrepresented as being the original software.
**  3. This notice may not be removed or altered from any source distribution.
**
*/


#include "c_cvars.h"
#include "hw_material.h"
#include "hw_cvars.h"
#include "hw_renderstate.h"
#include <zvulkan/vulkanobjects.h>
#include <zvulkan/vulkanbuilders.h>
#include "vulkan/system/vk_renderdevice.h"
#include "vulkan/system/vk_commandbuffer.h"
#include "vulkan/textures/vk_samplers.h"
#include "vulkan/textures/vk_renderbuffers.h"
#include "vulkan/textures/vk_texture.h"
#include "vulkan/renderer/vk_descriptorset.h"
#include "vulkan/renderer/vk_postprocess.h"
#include "vulkan/shaders/vk_shader.h"
#include "vk_hwtexture.h"
#include "filesystem.h"
#include "files.h"
#include "i_specialpaths.h"
#include "cmdlib.h"
#include "m_crc32.h"
#include "printf.h"
#include <miniz.h>
#include <vector>
#include <string>
#include <stdint.h>
#include <memory>


#ifndef MAKE_ID
#ifndef __BIG_ENDIAN__
#define MAKE_ID(a,b,c,d)	((uint32_t)((a)|((b)<<8)|((c)<<16)|((d)<<24)))
#else
#define MAKE_ID(a,b,c,d)	((uint32_t)((d)|((c)<<8)|((b)<<16)|((a)<<24)))
#endif
#endif

// [NKS] Experimental Vulkan sprite mip-chain cache.
// Reuses the same .nkmip cache path/format as the OpenGL experiment.
CUSTOM_CVAR(Bool, nk_vk_mipchain_cache, false, CVAR_GLOBALCONFIG)
{
	if (self) self = false;
}
CVAR(Bool, nk_vk_mipchain_cache_debug, false, CVAR_GLOBALCONFIG)

static const uint32_t NK_VK_MIPCHAIN_CACHE_MAGIC = MAKE_ID('N', 'K', 'M', 'P');
static const uint32_t NK_VK_MIPCHAIN_CACHE_VERSION = 1;

struct NKVKMipChainCacheHeader
{
	uint32_t Magic;
	uint32_t Version;
	uint32_t Width;
	uint32_t Height;
	uint32_t SourceHash;
	uint32_t NameHash;
	uint32_t Flags;
	int32_t Translation;
	uint64_t ContentId;
	uint32_t BytesPerPixel;
	uint32_t LevelCount;
	uint32_t RawSize;
	uint32_t StoredSize;
};

struct NKVKMipChainSourceHashEntry
{
	int SourceLump;
	uint32_t SourceHash;
};

struct NKVKMipLevelInfo
{
	int Width;
	int Height;
	uint32_t Offset;
	uint32_t Size;
};

static std::vector<NKVKMipChainSourceHashEntry> NKVKMipSourceHashCache;

static FString NKVKMipCache_GetBasePath(bool create)
{
	FString path = NicePath("$PROGDIR/cache/nakara/mipchaincache");
	FixPathSeperator(path);
	if (create)
	{
		CreatePath(path.GetChars());
	}
	return path;
}

static uint32_t NKVKMipCache_HashString(const char *text)
{
	if (text == nullptr) return 0;
	return CalcCRC32((const uint8_t*)text, (unsigned int)strlen(text));
}

static bool NKVKMipCache_GetResourceId(int sourceLump, FString &resourceId)
{
	if (sourceLump < 0) return false;

	const char *container = fileSystem.GetResourceFileFullName(fileSystem.GetFileContainer(sourceLump));
	const char *fullname = fileSystem.GetFileFullName(sourceLump, false);
	if (container == nullptr || fullname == nullptr) return false;

	resourceId.Format("%s|%s", container, fullname);
	FixPathSeperator(resourceId);
	return true;
}

static bool NKVKMipCache_GetSourceHash(int sourceLump, uint32_t &sourceHash)
{
	if (sourceLump < 0) return false;

	for (auto &entry : NKVKMipSourceHashCache)
	{
		if (entry.SourceLump == sourceLump)
		{
			sourceHash = entry.SourceHash;
			return true;
		}
	}

	auto reader = fileSystem.OpenFileReader(sourceLump);
	if (!reader.isOpen()) return false;

	auto length = reader.GetLength();
	if (length <= 0 || length > 1024 * 1024 * 256) return false;

	std::vector<uint8_t> data;
	data.resize((size_t)length);
	if (reader.Read(data.data(), length) != length) return false;

	sourceHash = CalcCRC32(data.data(), (unsigned int)data.size());

	NKVKMipChainSourceHashEntry cacheEntry;
	cacheEntry.SourceLump = sourceLump;
	cacheEntry.SourceHash = sourceHash;
	NKVKMipSourceHashCache.push_back(cacheEntry);
	return true;
}

static bool NKVKMipCache_IsAllowed(FTexture *sourceTex, const void *pixels, int width, int height, int bpp, int flags)
{
	if (!nk_vk_mipchain_cache) return false;
	if (sourceTex == nullptr) return false;
	if (pixels == nullptr) return false;
	if (width <= 1 && height <= 1) return false;
	if (bpp != 4) return false;
	if (!(flags & CTF_Expand)) return false;
	return true;
}

static FString NKVKMipCache_MakePath(uint32_t nameHash, uint32_t sourceHash, uint64_t contentId, int width, int height, int flags, int translation)
{
	FString path = NKVKMipCache_GetBasePath(true);
	FString file;
	file.Format("/%08x_%08x_%016llx_%dx%d_f%08x_t%d.nkmip",
		nameHash,
		sourceHash,
		(unsigned long long)contentId,
		width,
		height,
		flags,
		translation);
	path += file;
	return path;
}

static bool NKVKMipCache_BuildIdentity(FTexture *sourceTex, int width, int height, int bpp, int flags, int translation, uint64_t contentId, FString &resourceId, uint32_t &sourceHash, uint32_t &nameHash, FString &cachePath)
{
	if (!NKVKMipCache_IsAllowed(sourceTex, (const void*)1, width, height, bpp, flags)) return false;

	int sourceLump = sourceTex->GetSourceLump();
	if (!NKVKMipCache_GetResourceId(sourceLump, resourceId)) return false;
	if (!NKVKMipCache_GetSourceHash(sourceLump, sourceHash)) return false;

	nameHash = NKVKMipCache_HashString(resourceId.GetChars());
	cachePath = NKVKMipCache_MakePath(nameHash, sourceHash, contentId, width, height, flags, translation);
	return true;
}

static int NKVKMipCache_CountLevels(int width, int height)
{
	int levels = 0;
	while (width > 1 || height > 1)
	{
		width = width > 1 ? width >> 1 : 1;
		height = height > 1 ? height >> 1 : 1;
		levels++;
	}
	return levels;
}

static void NKVKMipCache_BuildMipChain(const uint8_t *base, int width, int height, int bpp, std::vector<uint8_t> &mipData, std::vector<NKVKMipLevelInfo> &levels)
{
	mipData.clear();
	levels.clear();

	if (base == nullptr || (width <= 1 && height <= 1) || bpp != 4)
	{
		return;
	}

	std::vector<uint8_t> current;
	current.resize((size_t)width * (size_t)height * (size_t)bpp);
	memcpy(current.data(), base, current.size());

	int srcW = width;
	int srcH = height;

	while (srcW > 1 || srcH > 1)
	{
		int dstW = srcW > 1 ? srcW >> 1 : 1;
		int dstH = srcH > 1 ? srcH >> 1 : 1;

		std::vector<uint8_t> next;
		next.resize((size_t)dstW * (size_t)dstH * (size_t)bpp);

		for (int y = 0; y < dstH; y++)
		{
			for (int x = 0; x < dstW; x++)
			{
				int sx0 = x * 2;
				int sy0 = y * 2;
				int sx1 = sx0 + 1 < srcW ? sx0 + 1 : sx0;
				int sy1 = sy0 + 1 < srcH ? sy0 + 1 : sy0;

				const uint8_t *p00 = &current[((size_t)sy0 * srcW + sx0) * bpp];
				const uint8_t *p10 = &current[((size_t)sy0 * srcW + sx1) * bpp];
				const uint8_t *p01 = &current[((size_t)sy1 * srcW + sx0) * bpp];
				const uint8_t *p11 = &current[((size_t)sy1 * srcW + sx1) * bpp];

				uint8_t *out = &next[((size_t)y * dstW + x) * bpp];

				for (int c = 0; c < bpp; c++)
				{
					out[c] = (uint8_t)(((int)p00[c] + (int)p10[c] + (int)p01[c] + (int)p11[c] + 2) >> 2);
				}
			}
		}

		NKVKMipLevelInfo info;
		info.Width = dstW;
		info.Height = dstH;
		info.Offset = (uint32_t)mipData.size();
		info.Size = (uint32_t)next.size();

		mipData.insert(mipData.end(), next.begin(), next.end());
		levels.push_back(info);

		current = std::move(next);
		srcW = dstW;
		srcH = dstH;
	}
}

static bool NKVKMipCache_Load(FTexture *sourceTex, int width, int height, int bpp, int flags, int translation, uint64_t contentId, std::vector<uint8_t> &mipData, std::vector<NKVKMipLevelInfo> &levels)
{
	FString resourceId;
	FString cachePath;
	uint32_t sourceHash = 0;
	uint32_t nameHash = 0;

	if (!NKVKMipCache_BuildIdentity(sourceTex, width, height, bpp, flags, translation, contentId, resourceId, sourceHash, nameHash, cachePath)) return false;

	FileReader fp;
	if (!fp.OpenFile(cachePath.GetChars()))
	{
		if (nk_vk_mipchain_cache_debug)
		{
			Printf("[NKVKMipChain] load open failed: %s\n", cachePath.GetChars());
		}
		return false;
	}

	NKVKMipChainCacheHeader header;
	if (fp.Read(&header, sizeof(header)) != sizeof(header))
	{
		RemoveFile(cachePath.GetChars());
		return false;
	}

	int expectedLevels = NKVKMipCache_CountLevels(width, height);
	if (header.Magic != NK_VK_MIPCHAIN_CACHE_MAGIC || header.Version != NK_VK_MIPCHAIN_CACHE_VERSION ||
		header.Width != (uint32_t)width || header.Height != (uint32_t)height ||
		header.SourceHash != sourceHash || header.NameHash != nameHash ||
		header.Flags != (uint32_t)flags || header.Translation != translation ||
		header.ContentId != contentId || header.BytesPerPixel != (uint32_t)bpp ||
		header.LevelCount != (uint32_t)expectedLevels || header.RawSize == 0 || header.StoredSize == 0)
	{
		RemoveFile(cachePath.GetChars());
		return false;
	}

	std::vector<uint8_t> compressed;
	compressed.resize(header.StoredSize);
	if (fp.Read(compressed.data(), header.StoredSize) != header.StoredSize)
	{
		RemoveFile(cachePath.GetChars());
		return false;
	}

	mipData.resize(header.RawSize);
	mz_ulong rawSize = (mz_ulong)header.RawSize;
	if (mz_uncompress(mipData.data(), &rawSize, compressed.data(), (mz_ulong)header.StoredSize) != MZ_OK || rawSize != header.RawSize)
	{
		mipData.clear();
		RemoveFile(cachePath.GetChars());
		return false;
	}

	levels.clear();
	int levelW = width;
	int levelH = height;
	uint32_t offset = 0;
	for (int i = 0; i < expectedLevels; i++)
	{
		levelW = levelW > 1 ? levelW >> 1 : 1;
		levelH = levelH > 1 ? levelH >> 1 : 1;

		NKVKMipLevelInfo info;
		info.Width = levelW;
		info.Height = levelH;
		info.Offset = offset;
		info.Size = (uint32_t)(levelW * levelH * bpp);
		levels.push_back(info);
		offset += info.Size;
	}

	if (offset != header.RawSize)
	{
		mipData.clear();
		levels.clear();
		RemoveFile(cachePath.GetChars());
		return false;
	}

	if (nk_vk_mipchain_cache_debug)
	{
		Printf("[NKVKMipChain] hit: %s raw=%uKB stored=%uKB levels=%u\n",
			resourceId.GetChars(),
			(header.RawSize + 1023) / 1024,
			(header.StoredSize + 1023) / 1024,
			header.LevelCount);
	}
	return true;
}

static void NKVKMipCache_Save(FTexture *sourceTex, int width, int height, int bpp, int flags, int translation, uint64_t contentId, const std::vector<uint8_t> &mipData, const std::vector<NKVKMipLevelInfo> &levels)
{
	if (mipData.empty() || levels.empty()) return;

	FString resourceId;
	FString cachePath;
	uint32_t sourceHash = 0;
	uint32_t nameHash = 0;

	if (!NKVKMipCache_BuildIdentity(sourceTex, width, height, bpp, flags, translation, contentId, resourceId, sourceHash, nameHash, cachePath)) return;
	if (FileExists(cachePath.GetChars())) return;

	NKVKMipChainCacheHeader header;
	header.Magic = NK_VK_MIPCHAIN_CACHE_MAGIC;
	header.Version = NK_VK_MIPCHAIN_CACHE_VERSION;
	header.Width = (uint32_t)width;
	header.Height = (uint32_t)height;
	header.SourceHash = sourceHash;
	header.NameHash = nameHash;
	header.Flags = (uint32_t)flags;
	header.Translation = translation;
	header.ContentId = contentId;
	header.BytesPerPixel = (uint32_t)bpp;
	header.LevelCount = (uint32_t)levels.size();
	header.RawSize = (uint32_t)mipData.size();
	header.StoredSize = 0;

	std::vector<uint8_t> compressed;
	compressed.resize(mz_compressBound((mz_ulong)header.RawSize));
	mz_ulong compressedSize = (mz_ulong)compressed.size();
	if (mz_compress2(compressed.data(), &compressedSize, mipData.data(), (mz_ulong)header.RawSize, MZ_BEST_SPEED) != MZ_OK)
	{
		return;
	}
	header.StoredSize = (uint32_t)compressedSize;

	std::unique_ptr<FileWriter> fp(FileWriter::Open(cachePath.GetChars()));
	if (fp == nullptr)
	{
		if (nk_vk_mipchain_cache_debug)
		{
			Printf("[NKVKMipChain] save open failed: %s\n", cachePath.GetChars());
		}
		return;
	}

	bool ok = fp->Write(&header, sizeof(header)) == sizeof(header);
	if (ok) ok = fp->Write(compressed.data(), header.StoredSize) == header.StoredSize;
	fp->Close();

	if (!ok)
	{
		RemoveFile(cachePath.GetChars());
		return;
	}

	if (nk_vk_mipchain_cache_debug)
	{
		Printf("[NKVKMipChain] saved: %s raw=%uKB stored=%uKB levels=%u ratio=%u%%\n",
			resourceId.GetChars(),
			(header.RawSize + 1023) / 1024,
			(header.StoredSize + 1023) / 1024,
			header.LevelCount,
			header.RawSize > 0 ? (unsigned)((uint64_t)header.StoredSize * 100ull / (uint64_t)header.RawSize) : 0u);
	}
}

VkHardwareTexture::VkHardwareTexture(VulkanRenderDevice* fb, int numchannels) : fb(fb)
{
	mTexelsize = numchannels;
	fb->GetTextureManager()->AddTexture(this);
}

VkHardwareTexture::~VkHardwareTexture()
{
	if (fb)
		fb->GetTextureManager()->RemoveTexture(this);
}

void VkHardwareTexture::Reset()
{
	if (fb)
	{
		if (mappedSWFB)
		{
			mImage.Image->Unmap();
			mappedSWFB = nullptr;
		}

		mImage.Reset(fb);
		mDepthStencil.Reset(fb);
	}
}

VkTextureImage *VkHardwareTexture::GetImage(FTexture *tex, int translation, int flags)
{
	if (!mImage.Image)
	{
		CreateImage(tex, translation, flags);
	}
	return &mImage;
}

VkTextureImage *VkHardwareTexture::GetDepthStencil(FTexture *tex)
{
	if (!mDepthStencil.View)
	{
		VkFormat format = fb->GetBuffers()->SceneDepthStencilFormat;
		int w = tex->GetWidth();
		int h = tex->GetHeight();

		mDepthStencil.Image = ImageBuilder()
			.Size(w, h)
			.Samples(VK_SAMPLE_COUNT_1_BIT)
			.Format(format)
			.Usage(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
			.DebugName("VkHardwareTexture.DepthStencil")
			.Create(fb->device.get());

		mDepthStencil.AspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

		mDepthStencil.View = ImageViewBuilder()
			.Image(mDepthStencil.Image.get(), format, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
			.DebugName("VkHardwareTexture.DepthStencilView")
			.Create(fb->device.get());

		VkImageTransition()
			.AddImage(&mDepthStencil, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, true)
			.Execute(fb->GetCommands()->GetTransferCommands());
	}
	return &mDepthStencil;
}

void VkHardwareTexture::CreateImage(FTexture *tex, int translation, int flags)
{
	if (!tex->isHardwareCanvas())
	{
		FTextureBuffer texbuffer;
		if (!NKPreparedTexture_TryTake(tex, translation, flags, texbuffer))
		{
			texbuffer = tex->CreateTexBuffer(translation, flags | CTF_ProcessData);
		}
		bool indexed = flags & CTF_Indexed;
		CreateTexture(texbuffer.mWidth, texbuffer.mHeight,indexed? 1 : 4, indexed? VK_FORMAT_R8_UNORM : VK_FORMAT_B8G8R8A8_UNORM, texbuffer.mBuffer, !indexed, tex, translation, flags, texbuffer.mContentId);
	}
	else
	{
		VkFormat format = tex->IsHDR() ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
		int w = tex->GetWidth();
		int h = tex->GetHeight();

		mImage.Image = ImageBuilder()
			.Format(format)
			.Size(w, h)
			.Usage(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
			.DebugName("VkHardwareTexture.mImage")
			.Create(fb->device.get());

		mImage.View = ImageViewBuilder()
			.Image(mImage.Image.get(), format)
			.DebugName("VkHardwareTexture.mImageView")
			.Create(fb->device.get());

		VkImageTransition()
			.AddImage(&mImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true)
			.Execute(fb->GetCommands()->GetTransferCommands());
	}
}

void VkHardwareTexture::CreateTexture(int w, int h, int pixelsize, VkFormat format, const void *pixels, bool mipmap, FTexture *sourceTex, int translation, int flags, uint64_t contentId)
{
	if (w <= 0 || h <= 0)
		throw CVulkanError("Trying to create zero size texture");

	int totalSize = w * h * pixelsize;

	std::vector<uint8_t> mipData;
	std::vector<NKVKMipLevelInfo> mipLevels;
	bool uploadCachedMipChain = false;

	if (mipmap && NKVKMipCache_IsAllowed(sourceTex, pixels, w, h, pixelsize, flags))
	{
		if (!NKVKMipCache_Load(sourceTex, w, h, pixelsize, flags, translation, contentId, mipData, mipLevels))
		{
			NKVKMipCache_BuildMipChain((const uint8_t*)pixels, w, h, pixelsize, mipData, mipLevels);
			NKVKMipCache_Save(sourceTex, w, h, pixelsize, flags, translation, contentId, mipData, mipLevels);
		}
		uploadCachedMipChain = !mipData.empty() && !mipLevels.empty();
	}

	int stagingSize = totalSize + (uploadCachedMipChain ? (int)mipData.size() : 0);

	auto stagingBuffer = BufferBuilder()
		.Size(stagingSize)
		.Usage(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY)
		.DebugName("VkHardwareTexture.mStagingBuffer")
		.Create(fb->device.get());

	uint8_t *data = (uint8_t*)stagingBuffer->Map(0, stagingSize);
	memcpy(data, pixels, totalSize);
	if (uploadCachedMipChain)
	{
		memcpy(data + totalSize, mipData.data(), mipData.size());
	}
	stagingBuffer->Unmap();

	mImage.Image = ImageBuilder()
		.Format(format)
		.Size(w, h, !mipmap ? 1 : GetMipLevels(w, h))
		.Usage(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
		.DebugName("VkHardwareTexture.mImage")
		.Create(fb->device.get());

	mImage.View = ImageViewBuilder()
		.Image(mImage.Image.get(), format)
		.DebugName("VkHardwareTexture.mImageView")
		.Create(fb->device.get());

	auto cmdbuffer = fb->GetCommands()->GetTransferCommands();

	VkImageTransition()
		.AddImage(&mImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, true, 0, !mipmap ? 1 : GetMipLevels(w, h))
		.Execute(cmdbuffer);

	std::vector<VkBufferImageCopy> regions;
	VkBufferImageCopy baseRegion = {};
	baseRegion.bufferOffset = 0;
	baseRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	baseRegion.imageSubresource.mipLevel = 0;
	baseRegion.imageSubresource.layerCount = 1;
	baseRegion.imageExtent.depth = 1;
	baseRegion.imageExtent.width = w;
	baseRegion.imageExtent.height = h;
	regions.push_back(baseRegion);

	if (uploadCachedMipChain)
	{
		for (unsigned int i = 0; i < mipLevels.size(); i++)
		{
			const NKVKMipLevelInfo &level = mipLevels[i];

			VkBufferImageCopy region = {};
			region.bufferOffset = totalSize + level.Offset;
			region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			region.imageSubresource.mipLevel = i + 1;
			region.imageSubresource.layerCount = 1;
			region.imageExtent.depth = 1;
			region.imageExtent.width = level.Width;
			region.imageExtent.height = level.Height;
			regions.push_back(region);
		}
	}

	cmdbuffer->copyBufferToImage(stagingBuffer->buffer, mImage.Image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)regions.size(), regions.data());

	if (mipmap && uploadCachedMipChain)
	{
		VkImageTransition()
			.AddImage(&mImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false, 0, GetMipLevels(w, h))
			.Execute(cmdbuffer);
	}
	else if (mipmap)
	{
		mImage.GenerateMipmaps(cmdbuffer);
	}
	else
	{
		VkImageTransition()
			.AddImage(&mImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false, 0, 1)
			.Execute(cmdbuffer);
	}

	// If we queued more than 64 MB of data already: wait until the uploads finish before continuing
	fb->GetCommands()->TransferDeleteList->Add(std::move(stagingBuffer));
	if (fb->GetCommands()->TransferDeleteList->TotalSize > 64 * 1024 * 1024)
		fb->GetCommands()->WaitForCommands(false, true);
}

int VkHardwareTexture::GetMipLevels(int w, int h)
{
	int levels = 1;
	while (w > 1 || h > 1)
	{
		w = max(w >> 1, 1);
		h = max(h >> 1, 1);
		levels++;
	}
	return levels;
}

void VkHardwareTexture::AllocateBuffer(int w, int h, int texelsize)
{
	if (mImage.Image && (mImage.Image->width != w || mImage.Image->height != h || mTexelsize != texelsize))
	{
		Reset();
	}

	if (!mImage.Image)
	{
		VkFormat format = texelsize == 4 ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8_UNORM;

		VkDeviceSize allocatedBytes = 0;
		mImage.Image = ImageBuilder()
			.Format(format)
			.Size(w, h)
			.LinearTiling()
			.Usage(VK_IMAGE_USAGE_SAMPLED_BIT, VMA_MEMORY_USAGE_UNKNOWN, VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT)
			.MemoryType(
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
			.DebugName("VkHardwareTexture.mImage")
			.Create(fb->device.get(), &allocatedBytes);

		mTexelsize = texelsize;

		mImage.View = ImageViewBuilder()
			.Image(mImage.Image.get(), format)
			.DebugName("VkHardwareTexture.mImageView")
			.Create(fb->device.get());

		VkImageTransition()
			.AddImage(&mImage, VK_IMAGE_LAYOUT_GENERAL, true)
			.Execute(fb->GetCommands()->GetTransferCommands());

		bufferpitch = int(allocatedBytes / h / texelsize);
	}
}

uint8_t *VkHardwareTexture::MapBuffer()
{
	if (!mappedSWFB)
		mappedSWFB = (uint8_t*)mImage.Image->Map(0, mImage.Image->width * mImage.Image->height * mTexelsize);
	return mappedSWFB;
}

unsigned int VkHardwareTexture::CreateTexture(unsigned char * buffer, int w, int h, int texunit, bool mipmap, const char *name)
{
	// CreateTexture is used by the software renderer to create a screen output but without any screen data.
	if (buffer)
		CreateTexture(w, h, mTexelsize, mTexelsize == 4 ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8_UNORM, buffer, mipmap, nullptr, 0, 0, 0);
	return 0;
}

void VkHardwareTexture::CreateWipeTexture(int w, int h, const char *name)
{
	VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;

	mImage.Image = ImageBuilder()
		.Format(format)
		.Size(w, h)
		.Usage(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY)
		.DebugName(name)
		.Create(fb->device.get());

	mTexelsize = 4;

	mImage.View = ImageViewBuilder()
		.Image(mImage.Image.get(), format)
		.DebugName(name)
		.Create(fb->device.get());

	if (fb->GetBuffers()->GetWidth() > 0 && fb->GetBuffers()->GetHeight() > 0)
	{
		fb->GetPostprocess()->BlitCurrentToImage(&mImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}
	else
	{
		// hwrenderer asked image data from a frame buffer that was never written into. Let's give it that..
		// (ideally the hwrenderer wouldn't do this, but the calling code is too complex for me to fix)

		VkImageTransition()
			.AddImage(&mImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, true)
			.Execute(fb->GetCommands()->GetTransferCommands());

		VkImageSubresourceRange range = {};
		range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		range.layerCount = 1;
		range.levelCount = 1;

		VkClearColorValue value = {};
		value.float32[0] = 0.0f;
		value.float32[1] = 0.0f;
		value.float32[2] = 0.0f;
		value.float32[3] = 1.0f;
		fb->GetCommands()->GetTransferCommands()->clearColorImage(mImage.Image->image, mImage.Layout, &value, 1, &range);

		VkImageTransition()
			.AddImage(&mImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
			.Execute(fb->GetCommands()->GetTransferCommands());
	}
}

/////////////////////////////////////////////////////////////////////////////

VkMaterial::VkMaterial(VulkanRenderDevice* fb, FGameTexture* tex, int scaleflags) : FMaterial(tex, scaleflags), fb(fb)
{
	fb->GetDescriptorSetManager()->AddMaterial(this);
}

VkMaterial::~VkMaterial()
{
	if (fb)
		fb->GetDescriptorSetManager()->RemoveMaterial(this);
}

void VkMaterial::DeleteDescriptors()
{
	if (fb)
	{
		auto deleteList = fb->GetCommands()->DrawDeleteList.get();
		for (auto& it : mDescriptorSets)
		{
			deleteList->Add(std::move(it.descriptor));
		}
		mDescriptorSets.clear();
	}
}

VulkanDescriptorSet* VkMaterial::GetDescriptorSet(const FMaterialState& state)
{
	auto base = Source();
	int clampmode = state.mClampMode;
	int translation = state.mTranslation;
	auto translationp = IsLuminosityTranslation(translation)? translation : intptr_t(GPalette.GetTranslation(GetTranslationType(translation), GetTranslationIndex(translation)));

	clampmode = base->GetClampMode(clampmode);

	for (auto& set : mDescriptorSets)
	{
		if (set.descriptor && set.clampmode == clampmode && set.remap == translationp) return set.descriptor.get();
	}

	int numLayers = NumLayers();

	auto descriptor = fb->GetDescriptorSetManager()->AllocateTextureDescriptorSet(max(numLayers, SHADER_MIN_REQUIRED_TEXTURE_LAYERS));

	descriptor->SetDebugName("VkHardwareTexture.mDescriptorSets");

	VulkanSampler* sampler = fb->GetSamplerManager()->Get(clampmode);

	WriteDescriptors update;
	MaterialLayerInfo *layer;
	auto systex = static_cast<VkHardwareTexture*>(GetLayer(0, state.mTranslation, &layer));
	auto systeximage = systex->GetImage(layer->layerTexture, state.mTranslation, layer->scaleFlags);
	update.AddCombinedImageSampler(descriptor.get(), 0, systeximage->View.get(), sampler, systeximage->Layout);

	if (!(layer->scaleFlags & CTF_Indexed))
	{
		for (int i = 1; i < numLayers; i++)
		{
			auto syslayer = static_cast<VkHardwareTexture*>(GetLayer(i, 0, &layer));
			auto syslayerimage = syslayer->GetImage(layer->layerTexture, 0, layer->scaleFlags);
			update.AddCombinedImageSampler(descriptor.get(), i, syslayerimage->View.get(), sampler, syslayerimage->Layout);
		}
	}
	else
	{
		for (int i = 1; i < 3; i++)
		{
			auto syslayer = static_cast<VkHardwareTexture*>(GetLayer(i, translation, &layer));
			auto syslayerimage = syslayer->GetImage(layer->layerTexture, 0, layer->scaleFlags);
			update.AddCombinedImageSampler(descriptor.get(), i, syslayerimage->View.get(), sampler, syslayerimage->Layout);
		}
		numLayers = 3;
	}

	auto dummyImage = fb->GetTextureManager()->GetNullTextureView();
	for (int i = numLayers; i < SHADER_MIN_REQUIRED_TEXTURE_LAYERS; i++)
	{
		update.AddCombinedImageSampler(descriptor.get(), i, dummyImage, sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}

	update.Execute(fb->device.get());
	mDescriptorSets.emplace_back(clampmode, translationp, std::move(descriptor));
	return mDescriptorSets.back().descriptor.get();
}

