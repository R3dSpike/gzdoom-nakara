/*
** gl_hwtexture.cpp
** GL texture abstraction
**
**---------------------------------------------------------------------------
** Copyright 2019 Christoph Oelckers
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
**
*/

#include "gl_system.h"

#include "c_cvars.h"
#include "hw_material.h"

#include "gl_interface.h"
#include "hw_cvars.h"
#include "gl_debug.h"
#include "gl_renderer.h"
#include "gl_renderstate.h"
#include "gl_samplers.h"
#include "gl_hwtexture.h"
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

namespace OpenGLRenderer
{


#ifndef MAKE_ID
#ifndef __BIG_ENDIAN__
#define MAKE_ID(a,b,c,d)	((uint32_t)((a)|((b)<<8)|((c)<<16)|((d)<<24)))
#else
#define MAKE_ID(a,b,c,d)	((uint32_t)((d)|((c)<<8)|((b)<<16)|((a)<<24)))
#endif
#endif

// [NKS] Experimental OpenGL sprite mip-chain cache.
// Keeps sprite mipmaps enabled, but tries to avoid driver-side glGenerateMipmap
// by uploading cached CPU-generated mip levels.
CUSTOM_CVAR(Bool, nk_gl_mipchain_cache, false, CVAR_GLOBALCONFIG)
{
	if (self) self = false;
}
CVAR(Bool, nk_gl_mipchain_cache_debug, false, CVAR_GLOBALCONFIG)

static const uint32_t NK_GL_MIPCHAIN_CACHE_MAGIC = MAKE_ID('N', 'K', 'M', 'P');
static const uint32_t NK_GL_MIPCHAIN_CACHE_VERSION = 1;

struct NKGLMipChainCacheHeader
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

struct NKGLMipChainSourceHashEntry
{
	int SourceLump;
	uint32_t SourceHash;
};

struct NKGLMipLevelInfo
{
	int Width;
	int Height;
	uint32_t Offset;
	uint32_t Size;
};

static std::vector<NKGLMipChainSourceHashEntry> NKMipSourceHashCache;

static FString NKGLMipCache_GetBasePath(bool create)
{
	FString path = NicePath("$PROGDIR/cache/nakara/mipchaincache");
	FixPathSeperator(path);
	if (create)
	{
		CreatePath(path.GetChars());
	}
	return path;
}

static uint32_t NKGLMipCache_HashString(const char *text)
{
	if (text == nullptr) return 0;
	return CalcCRC32((const uint8_t*)text, (unsigned int)strlen(text));
}

static bool NKGLMipCache_GetResourceId(int sourceLump, FString &resourceId)
{
	if (sourceLump < 0) return false;

	const char *container = fileSystem.GetResourceFileFullName(fileSystem.GetFileContainer(sourceLump));
	const char *fullname = fileSystem.GetFileFullName(sourceLump, false);
	if (container == nullptr || fullname == nullptr) return false;

	resourceId.Format("%s|%s", container, fullname);
	FixPathSeperator(resourceId);
	return true;
}

static bool NKGLMipCache_GetSourceHash(int sourceLump, uint32_t &sourceHash)
{
	if (sourceLump < 0) return false;

	for (auto &entry : NKMipSourceHashCache)
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

	NKGLMipChainSourceHashEntry cacheEntry;
	cacheEntry.SourceLump = sourceLump;
	cacheEntry.SourceHash = sourceHash;
	NKMipSourceHashCache.push_back(cacheEntry);
	return true;
}

static bool NKGLMipCache_IsAllowed(FTexture *sourceTex, const unsigned char *buffer, int width, int height, int bpp, int flags)
{
	if (!nk_gl_mipchain_cache) return false;
	if (sourceTex == nullptr) return false;
	if (buffer == nullptr) return false;
	if (width <= 1 && height <= 1) return false;
	if (bpp != 4) return false;

	// Keep first version narrow: only expanded sprite BGRA textures.
	if (!(flags & CTF_Expand)) return false;
	return true;
}

static FString NKGLMipCache_MakePath(uint32_t nameHash, uint32_t sourceHash, uint64_t contentId, int width, int height, int flags, int translation)
{
	FString path = NKGLMipCache_GetBasePath(true);
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

static bool NKGLMipCache_BuildIdentity(FTexture *sourceTex, int width, int height, int bpp, int flags, int translation, uint64_t contentId, FString &resourceId, uint32_t &sourceHash, uint32_t &nameHash, FString &cachePath)
{
	if (!NKGLMipCache_IsAllowed(sourceTex, (const unsigned char*)1, width, height, bpp, flags)) return false;

	int sourceLump = sourceTex->GetSourceLump();
	if (!NKGLMipCache_GetResourceId(sourceLump, resourceId)) return false;
	if (!NKGLMipCache_GetSourceHash(sourceLump, sourceHash)) return false;

	nameHash = NKGLMipCache_HashString(resourceId.GetChars());
	cachePath = NKGLMipCache_MakePath(nameHash, sourceHash, contentId, width, height, flags, translation);
	return true;
}

static int NKGLMipCache_CountLevels(int width, int height)
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

static void NKGLMipCache_BuildMipChain(const unsigned char *base, int width, int height, int bpp, std::vector<uint8_t> &mipData, std::vector<NKGLMipLevelInfo> &levels)
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

		NKGLMipLevelInfo info;
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

static bool NKGLMipCache_Load(FTexture *sourceTex, int width, int height, int bpp, int flags, int translation, uint64_t contentId, std::vector<uint8_t> &mipData, std::vector<NKGLMipLevelInfo> &levels)
{
	FString resourceId;
	FString cachePath;
	uint32_t sourceHash = 0;
	uint32_t nameHash = 0;

	if (!NKGLMipCache_BuildIdentity(sourceTex, width, height, bpp, flags, translation, contentId, resourceId, sourceHash, nameHash, cachePath)) return false;

	FileReader fp;
	if (!fp.OpenFile(cachePath.GetChars()))
	{
		if (nk_gl_mipchain_cache_debug)
		{
			Printf("[NKMipChain] load open failed: %s\n", cachePath.GetChars());
		}
		return false;
	}

	NKGLMipChainCacheHeader header;
	if (fp.Read(&header, sizeof(header)) != sizeof(header))
	{
		RemoveFile(cachePath.GetChars());
		return false;
	}

	int expectedLevels = NKGLMipCache_CountLevels(width, height);
	if (header.Magic != NK_GL_MIPCHAIN_CACHE_MAGIC || header.Version != NK_GL_MIPCHAIN_CACHE_VERSION ||
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

		NKGLMipLevelInfo info;
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

	if (nk_gl_mipchain_cache_debug)
	{
		Printf("[NKMipChain] hit: %s raw=%uKB stored=%uKB levels=%u\n",
			resourceId.GetChars(),
			(header.RawSize + 1023) / 1024,
			(header.StoredSize + 1023) / 1024,
			header.LevelCount);
	}
	return true;
}

static void NKGLMipCache_Save(FTexture *sourceTex, int width, int height, int bpp, int flags, int translation, uint64_t contentId, const std::vector<uint8_t> &mipData, const std::vector<NKGLMipLevelInfo> &levels)
{
	if (mipData.empty() || levels.empty()) return;

	FString resourceId;
	FString cachePath;
	uint32_t sourceHash = 0;
	uint32_t nameHash = 0;

	if (!NKGLMipCache_BuildIdentity(sourceTex, width, height, bpp, flags, translation, contentId, resourceId, sourceHash, nameHash, cachePath)) return;
	if (FileExists(cachePath.GetChars())) return;

	NKGLMipChainCacheHeader header;
	header.Magic = NK_GL_MIPCHAIN_CACHE_MAGIC;
	header.Version = NK_GL_MIPCHAIN_CACHE_VERSION;
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
		if (nk_gl_mipchain_cache_debug)
		{
			Printf("[NKMipChain] save open failed: %s\n", cachePath.GetChars());
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

	if (nk_gl_mipchain_cache_debug)
	{
		Printf("[NKMipChain] saved: %s raw=%uKB stored=%uKB levels=%u ratio=%u%%\n",
			resourceId.GetChars(),
			(header.RawSize + 1023) / 1024,
			(header.StoredSize + 1023) / 1024,
			header.LevelCount,
			header.RawSize > 0 ? (unsigned)((uint64_t)header.StoredSize * 100ull / (uint64_t)header.RawSize) : 0u);
	}
}

static void NKGLMipCache_UploadLevels(const std::vector<uint8_t> &mipData, const std::vector<NKGLMipLevelInfo> &levels, int texformat, int sourcetype)
{
	for (unsigned int i = 0; i < levels.size(); i++)
	{
		const NKGLMipLevelInfo &info = levels[i];
		glTexImage2D(GL_TEXTURE_2D, (GLint)(i + 1), texformat, info.Width, info.Height, 0, sourcetype, GL_UNSIGNED_BYTE, mipData.data() + info.Offset);
	}
}


TexFilter_s TexFilter[] = {
	{GL_NEAREST,					GL_NEAREST,		false},
	{GL_NEAREST_MIPMAP_NEAREST,		GL_NEAREST,		true},
	{GL_LINEAR,						GL_LINEAR,		false},
	{GL_LINEAR_MIPMAP_NEAREST,		GL_LINEAR,		true},
	{GL_LINEAR_MIPMAP_LINEAR,		GL_LINEAR,		true},
	{GL_NEAREST_MIPMAP_LINEAR,		GL_NEAREST,		true},
	{GL_LINEAR_MIPMAP_LINEAR,		GL_NEAREST,		true},
};

//===========================================================================
// 
//	Static texture data
//
//===========================================================================
unsigned int FHardwareTexture::lastbound[FHardwareTexture::MAX_TEXTURES];

//===========================================================================
// 
//	Loads the texture image into the hardware
//
// NOTE: For some strange reason I was unable to find the source buffer
// should be one line higher than the actual texture. I got extremely
// strange crashes deep inside the GL driver when I didn't do it!
//
//===========================================================================

unsigned int FHardwareTexture::CreateTexture(unsigned char * buffer, int w, int h, int texunit, bool mipmap, const char *name)
{
	int rh,rw;
	int texformat = GL_RGBA8;// TexFormat[gl_texture_format];
	bool deletebuffer=false;

	/*
	if (forcenocompression)
	{
		texformat = GL_RGBA8;
	}
	*/
	bool firstCall = glTexID == 0;
	if (firstCall)
	{
		glGenTextures(1, &glTexID);
	}

	int textureBinding = UINT_MAX;
	if (texunit == -1)	glGetIntegerv(GL_TEXTURE_BINDING_2D, &textureBinding);
	if (texunit > 0) glActiveTexture(GL_TEXTURE0+texunit);
	if (texunit >= 0) lastbound[texunit] = glTexID;
	glBindTexture(GL_TEXTURE_2D, glTexID);

	FGLDebug::LabelObject(GL_TEXTURE, glTexID, name);

	rw = GetTexDimension(w);
	rh = GetTexDimension(h);
	if (glBufferID > 0)
	{
		glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
		buffer = nullptr;
	}
	else if (!buffer)
	{
		// The texture must at least be initialized if no data is present.
		mipmapped = false;
		buffer=(unsigned char *)calloc(4,rw * (rh+1));
		deletebuffer=true;
		//texheight=-h;	
	}
	else
	{
		if (rw < w || rh < h)
		{
			// The texture is larger than what the hardware can handle so scale it down.
			unsigned char * scaledbuffer=(unsigned char *)calloc(4,rw * (rh+1));
			if (scaledbuffer)
			{
				Resize(w, h, rw, rh, buffer, scaledbuffer);
				deletebuffer=true;
				buffer=scaledbuffer;
			}
		}
	}
	// store the physical size.

	int sourcetype;
	if (glTextureBytes > 0)
	{
		if (glTextureBytes < 4) glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		static const int ITypes[] = { GL_R8, GL_RG8, GL_RGB8, GL_RGBA8 };
		static const int STypes[] = { GL_RED, GL_RG, GL_BGR, GL_BGRA };

		texformat = ITypes[glTextureBytes - 1];
		sourcetype = STypes[glTextureBytes - 1];
	}
	else
	{
		sourcetype = GL_BGRA;
	}

	if (!firstCall && glBufferID > 0)
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, rw, rh, sourcetype, GL_UNSIGNED_BYTE, buffer);
	else
		glTexImage2D(GL_TEXTURE_2D, 0, texformat, rw, rh, 0, sourcetype, GL_UNSIGNED_BYTE, buffer);

	if (deletebuffer && buffer) free(buffer);
	else if (glBufferID)
	{
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	}

	if (mipmap && TexFilter[gl_texture_filter].mipmapping)
	{
		bool uploadedCachedMipChain = false;
		int bpp = glTextureBytes > 0 ? glTextureBytes : 4;
		if (NKGLMipCache_IsAllowed(mipChainSourceTex, buffer, rw, rh, bpp, mipChainFlags))
		{
			std::vector<uint8_t> mipData;
			std::vector<NKGLMipLevelInfo> levels;

			if (!NKGLMipCache_Load(mipChainSourceTex, rw, rh, bpp, mipChainFlags, mipChainTranslation, mipChainContentId, mipData, levels))
			{
				NKGLMipCache_BuildMipChain(buffer, rw, rh, bpp, mipData, levels);
				NKGLMipCache_Save(mipChainSourceTex, rw, rh, bpp, mipChainFlags, mipChainTranslation, mipChainContentId, mipData, levels);
			}
			if (!mipData.empty() && !levels.empty())
			{
				NKGLMipCache_UploadLevels(mipData, levels, texformat, sourcetype);
				uploadedCachedMipChain = true;
				mipmapped = true;
			}
		}

		if (!uploadedCachedMipChain)
		{
			glGenerateMipmap(GL_TEXTURE_2D);
			mipmapped = true;
		}
	}

	if (texunit > 0) glActiveTexture(GL_TEXTURE0);
	else if (texunit == -1) glBindTexture(GL_TEXTURE_2D, textureBinding);
	return glTexID;
}


//===========================================================================
// 
//
//
//===========================================================================
void FHardwareTexture::AllocateBuffer(int w, int h, int texelsize)
{
	int rw = GetTexDimension(w);
	int rh = GetTexDimension(h);
	if (texelsize < 1 || texelsize > 4) texelsize = 4;
	glTextureBytes = texelsize;
	bufferpitch = w;
	if (rw == w || rh == h)
	{
		glGenBuffers(1, &glBufferID);
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, glBufferID);
		glBufferData(GL_PIXEL_UNPACK_BUFFER, w*h*texelsize, nullptr, GL_STREAM_DRAW);
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	}
}


uint8_t *FHardwareTexture::MapBuffer()
{
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, glBufferID);
	return (uint8_t*)glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
}

//===========================================================================
// 
//	Destroys the texture
//
//===========================================================================
FHardwareTexture::~FHardwareTexture() 
{ 
	if (glTexID != 0) glDeleteTextures(1, &glTexID);
	if (glBufferID != 0) glDeleteBuffers(1, &glBufferID);
}


//===========================================================================
// 
//	Binds this patch
//
//===========================================================================
unsigned int FHardwareTexture::Bind(int texunit, bool needmipmap)
{
	if (glTexID != 0)
	{
		if (lastbound[texunit] == glTexID) return glTexID;
		lastbound[texunit] = glTexID;
		if (texunit != 0) glActiveTexture(GL_TEXTURE0 + texunit);
		glBindTexture(GL_TEXTURE_2D, glTexID);
		// Check if we need mipmaps on a texture that was creted without them.
		if (needmipmap && !mipmapped && TexFilter[gl_texture_filter].mipmapping)
		{
			glGenerateMipmap(GL_TEXTURE_2D);
			mipmapped = true;
		}
		if (texunit != 0) glActiveTexture(GL_TEXTURE0);
		return glTexID;
	}
	return 0;
}

void FHardwareTexture::Unbind(int texunit)
{
	if (lastbound[texunit] != 0)
	{
		if (texunit != 0) glActiveTexture(GL_TEXTURE0+texunit);
		glBindTexture(GL_TEXTURE_2D, 0);
		if (texunit != 0) glActiveTexture(GL_TEXTURE0);
		lastbound[texunit] = 0;
	}
}

void FHardwareTexture::UnbindAll()
{
	for(int texunit = 0; texunit < 16; texunit++)
	{
		Unbind(texunit);
	}
}

//===========================================================================
// 
//	Creates a depth buffer for this texture
//
//===========================================================================

int FHardwareTexture::GetDepthBuffer(int width, int height)
{
	if (glDepthID == 0)
	{
		glGenRenderbuffers(1, &glDepthID);
		glBindRenderbuffer(GL_RENDERBUFFER, glDepthID);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 
			GetTexDimension(width), GetTexDimension(height));
		glBindRenderbuffer(GL_RENDERBUFFER, 0);
	}
	return glDepthID;
}


//===========================================================================
// 
//	Binds this texture's surfaces to the current framrbuffer
//
//===========================================================================

void FHardwareTexture::BindToFrameBuffer(int width, int height)
{
	width = GetTexDimension(width);
	height = GetTexDimension(height);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, glTexID, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, GetDepthBuffer(width, height));
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, GetDepthBuffer(width, height));
}


//===========================================================================
// 
//	Binds a texture to the renderer
//
//===========================================================================

bool FHardwareTexture::BindOrCreate(FTexture *tex, int texunit, int clampmode, int translation, int flags)
{
	bool needmipmap = (clampmode <= CLAMP_XY) && !forcenofilter;

	// Bind it to the system.
	if (!Bind(texunit, needmipmap))
	{
		if (flags & CTF_Indexed)
		{
			glTextureBytes = 1;
			forcenofilter = true;
			needmipmap = false;
		}
		int w = 0, h = 0;

		// Create this texture

		FTextureBuffer texbuffer;

		if (!tex->isHardwareCanvas())
		{
			if (!::NKPreparedTexture_TryTake(tex, translation, flags, texbuffer))
			{
				texbuffer = tex->CreateTexBuffer(translation, flags | CTF_ProcessData);
			}
			w = texbuffer.mWidth;
			h = texbuffer.mHeight;
		}
		else
		{
			w = tex->GetWidth();
			h = tex->GetHeight();
		}
		SetMipChainCacheContext(tex, translation, flags, texbuffer.mContentId);
		bool createdTexture = CreateTexture(texbuffer.mBuffer, w, h, texunit, needmipmap, "FHardwareTexture.BindOrCreate") != 0;
		ClearMipChainCacheContext();
		if (!createdTexture)
		{
			// could not create texture
			return false;
		}
	}
	if (forcenofilter && clampmode <= CLAMP_XY) clampmode += CLAMP_NOFILTER - CLAMP_NONE;
	GLRenderer->mSamplerManager->Bind(texunit, clampmode, 255);
	return true;
}

}
