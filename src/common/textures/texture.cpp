/*
** texture.cpp
** The base texture class
**
**---------------------------------------------------------------------------
** Copyright 2004-2007 Randy Heit
** Copyright 2006-2018 Christoph Oelckers
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

#include "printf.h"
#include "files.h"
#include "filesystem.h"

#include "textures.h"
#include "bitmap.h"
#include "colormatcher.h"
#include "c_dispatch.h"
#include "m_fixed.h"
#include "imagehelpers.h"
#include "image.h"
#include "formats/multipatchtexture.h"
#include "texturemanager.h"
#include "c_cvars.h"
#include "imagehelpers.h"
#include "v_video.h"
#include "v_font.h"
#include <deque>
#include <mutex>
#include <chrono>
#include <stdint.h>
#include <utility>

// Wrappers to keep the definitions of these classes out of here.
IHardwareTexture* CreateHardwareTexture(int numchannels);

// Make sprite offset adjustment user-configurable per renderer.
int r_spriteadjustSW, r_spriteadjustHW;

// [NKS] Prepared texture buffer queue, stage 3-A.
//
// The CPU worker may create a FTextureBuffer in the background. This queue keeps
// that software buffer alive briefly so the render backend can upload it without
// repeating image decode / disk-cache read / SmoothEdges on the main thread.
// GPU calls are intentionally not made here.
CVAR(Bool, nk_warmup_prepared_buffers, true, CVAR_GLOBALCONFIG)
CVAR(Bool, nk_warmup_prepared_debug, false, CVAR_GLOBALCONFIG)
CVAR(Int, nk_warmup_prepared_max_mb, 128, CVAR_GLOBALCONFIG)
CVAR(Int, nk_warmup_prepared_max_items, 512, CVAR_GLOBALCONFIG)
CVAR(Int, nk_warmup_prepared_expire_seconds, 30, CVAR_GLOBALCONFIG)

struct NKPreparedTextureEntry
{
	FTexture *Texture = nullptr;
	int Translation = 0;
	int Flags = 0;
	FTextureBuffer Buffer;
	uint64_t Bytes = 0;
	uint64_t CreatedMS = 0;

	NKPreparedTextureEntry() = default;
	NKPreparedTextureEntry(const NKPreparedTextureEntry&) = delete;
	NKPreparedTextureEntry& operator=(const NKPreparedTextureEntry&) = delete;
	NKPreparedTextureEntry(NKPreparedTextureEntry&&) noexcept = default;
	NKPreparedTextureEntry& operator=(NKPreparedTextureEntry&&) noexcept = default;
};

static std::mutex NKPreparedTextureMutex;
static std::deque<NKPreparedTextureEntry> NKPreparedTextureQueue;
static uint64_t NKPreparedTextureBytes = 0;
static uint64_t NKPreparedTexturePeakBytes = 0;
static uint64_t NKPreparedTextureStored = 0;
static uint64_t NKPreparedTextureTaken = 0;
static uint64_t NKPreparedTextureMissed = 0;
static uint64_t NKPreparedTextureEvicted = 0;
static uint64_t NKPreparedTextureExpired = 0;
static uint64_t NKPreparedTextureStoreDisabled = 0;
static uint64_t NKPreparedTextureStoreInvalid = 0;
static uint64_t NKPreparedTextureStoreIndexed = 0;
static uint64_t NKPreparedTextureStoreZero = 0;
static uint64_t NKPreparedTextureStoreOversized = 0;
static uint64_t NKPreparedTextureStoreDuplicateReplaced = 0;
static uint64_t NKPreparedTextureMissDisabled = 0;
static uint64_t NKPreparedTextureMissInvalid = 0;
static uint64_t NKPreparedTextureMissIndexed = 0;
static uint64_t NKPreparedTextureMissEmpty = 0;
static uint64_t NKPreparedTextureMissNoTexture = 0;
static uint64_t NKPreparedTextureMissTranslation = 0;
static uint64_t NKPreparedTextureMissFlags = 0;
static uint64_t NKPreparedTextureMissOther = 0;

static uint64_t NKPreparedTexture_NowMS()
{
	using namespace std::chrono;
	return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static int NKPreparedTexture_CanonicalFlags(int flags)
{
	return flags & (CTF_Expand | CTF_Upscale | CTF_Indexed);
}

static uint64_t NKPreparedTexture_BufferBytes(const FTextureBuffer &buffer, int flags)
{
	if (buffer.mBuffer == nullptr || buffer.mWidth <= 0 || buffer.mHeight <= 0) return 0;
	int bytesPerPixel = (flags & CTF_Indexed) ? 1 : 4;
	return (uint64_t)buffer.mWidth * (uint64_t)buffer.mHeight * (uint64_t)bytesPerPixel;
}

static uint64_t NKPreparedTexture_MaxBytes()
{
	int maxMB = nk_warmup_prepared_max_mb;
	if (maxMB < 1) maxMB = 1;
	return (uint64_t)maxMB * 1024ull * 1024ull;
}

static int NKPreparedTexture_MaxItems()
{
	int maxItems = nk_warmup_prepared_max_items;
	if (maxItems < 1) maxItems = 1;
	return maxItems;
}

static void NKPreparedTexture_EvictOldestLocked(const char *reason)
{
	if (NKPreparedTextureQueue.empty()) return;
	NKPreparedTextureBytes -= NKPreparedTextureQueue.front().Bytes;
	NKPreparedTextureQueue.pop_front();
	NKPreparedTextureEvicted++;
	if (nk_warmup_prepared_debug)
	{
		Printf("[NKPreparedTexture] evict reason=%s items=%u bytes=%.2fMB\n",
			reason != nullptr ? reason : "limit",
			(unsigned int)NKPreparedTextureQueue.size(),
			(double)NKPreparedTextureBytes / (1024.0 * 1024.0));
	}
}

static void NKPreparedTexture_ExpireLocked(uint64_t nowMS)
{
	int expireSeconds = nk_warmup_prepared_expire_seconds;
	if (expireSeconds <= 0) return;

	uint64_t expireMS = (uint64_t)expireSeconds * 1000ull;
	while (!NKPreparedTextureQueue.empty())
	{
		NKPreparedTextureEntry &entry = NKPreparedTextureQueue.front();
		if (nowMS < entry.CreatedMS || nowMS - entry.CreatedMS < expireMS)
		{
			break;
		}
		NKPreparedTextureBytes -= entry.Bytes;
		NKPreparedTextureQueue.pop_front();
		NKPreparedTextureExpired++;
	}
}

static void NKPreparedTexture_EnforceLimitsLocked()
{
	int maxItems = NKPreparedTexture_MaxItems();
	uint64_t maxBytes = NKPreparedTexture_MaxBytes();

	while ((int)NKPreparedTextureQueue.size() > maxItems)
	{
		NKPreparedTexture_EvictOldestLocked("items");
	}
	while (NKPreparedTextureBytes > maxBytes && !NKPreparedTextureQueue.empty())
	{
		NKPreparedTexture_EvictOldestLocked("bytes");
	}
}

bool NKPreparedTexture_Store(FTexture *tex, int translation, int flags, FTextureBuffer &&buffer)
{
	if (!nk_warmup_prepared_buffers)
	{
		NKPreparedTextureStoreDisabled++;
		return false;
	}
	if (tex == nullptr)
	{
		NKPreparedTextureStoreInvalid++;
		return false;
	}

	int keyFlags = NKPreparedTexture_CanonicalFlags(flags);
	if (keyFlags & CTF_Indexed)
	{
		NKPreparedTextureStoreIndexed++;
		return false; // Stage 3 is for BGRA hardware textures only.
	}

	uint64_t bytes = NKPreparedTexture_BufferBytes(buffer, keyFlags);
	if (bytes == 0)
	{
		NKPreparedTextureStoreZero++;
		return false;
	}

	uint64_t maxBytes = NKPreparedTexture_MaxBytes();
	if (bytes > maxBytes)
	{
		NKPreparedTextureStoreOversized++;
		// A single oversized texture would evict everything and still exceed the cap.
		if (nk_warmup_prepared_debug)
		{
			Printf("[NKPreparedTexture] skip oversized buffer %.2fMB > cap %.2fMB\n",
				(double)bytes / (1024.0 * 1024.0),
				(double)maxBytes / (1024.0 * 1024.0));
		}
		return false;
	}

	std::lock_guard<std::mutex> lock(NKPreparedTextureMutex);
	uint64_t nowMS = NKPreparedTexture_NowMS();
	NKPreparedTexture_ExpireLocked(nowMS);

	// Replace any stale duplicate for the same texture/translation/flags.
	for (auto it = NKPreparedTextureQueue.begin(); it != NKPreparedTextureQueue.end(); )
	{
		if (it->Texture == tex && it->Translation == translation && it->Flags == keyFlags)
		{
			NKPreparedTextureBytes -= it->Bytes;
			it = NKPreparedTextureQueue.erase(it);
			NKPreparedTextureEvicted++;
			NKPreparedTextureStoreDuplicateReplaced++;
		}
		else
		{
			++it;
		}
	}

	NKPreparedTextureEntry entry;
	entry.Texture = tex;
	entry.Translation = translation;
	entry.Flags = keyFlags;
	entry.Bytes = bytes;
	entry.CreatedMS = nowMS;
	entry.Buffer = std::move(buffer);

	NKPreparedTextureBytes += bytes;
	if (NKPreparedTextureBytes > NKPreparedTexturePeakBytes) NKPreparedTexturePeakBytes = NKPreparedTextureBytes;
	NKPreparedTextureQueue.push_back(std::move(entry));
	NKPreparedTextureStored++;
	NKPreparedTexture_EnforceLimitsLocked();

	if (nk_warmup_prepared_debug)
	{
		Printf("[NKPreparedTexture] store %dx%d trans=%d flags=%d bytes=%.2fMB items=%u total=%.2fMB\n",
			NKPreparedTextureQueue.back().Buffer.mWidth,
			NKPreparedTextureQueue.back().Buffer.mHeight,
			translation, keyFlags,
			(double)bytes / (1024.0 * 1024.0),
			(unsigned int)NKPreparedTextureQueue.size(),
			(double)NKPreparedTextureBytes / (1024.0 * 1024.0));
	}

	return true;
}

bool NKPreparedTexture_TryTake(FTexture *tex, int translation, int flags, FTextureBuffer &out)
{
	if (!nk_warmup_prepared_buffers)
	{
		NKPreparedTextureMissDisabled++;
		return false;
	}
	if (tex == nullptr)
	{
		NKPreparedTextureMissInvalid++;
		return false;
	}

	int keyFlags = NKPreparedTexture_CanonicalFlags(flags);
	if (keyFlags & CTF_Indexed)
	{
		NKPreparedTextureMissIndexed++;
		return false;
	}

	std::lock_guard<std::mutex> lock(NKPreparedTextureMutex);
	uint64_t nowMS = NKPreparedTexture_NowMS();
	NKPreparedTexture_ExpireLocked(nowMS);

	bool sawSameTexture = false;
	bool sawSameTextureTranslation = false;
	bool sawSameTextureFlags = false;

	for (auto it = NKPreparedTextureQueue.begin(); it != NKPreparedTextureQueue.end(); ++it)
	{
		if (it->Texture == tex)
		{
			sawSameTexture = true;
			if (it->Translation == translation) sawSameTextureTranslation = true;
			if (it->Flags == keyFlags) sawSameTextureFlags = true;
		}

		if (it->Texture == tex && it->Translation == translation && it->Flags == keyFlags)
		{
			uint64_t bytes = it->Bytes;
			out = std::move(it->Buffer);
			NKPreparedTextureBytes -= bytes;
			NKPreparedTextureQueue.erase(it);
			NKPreparedTextureTaken++;
			if (nk_warmup_prepared_debug)
			{
				Printf("[NKPreparedTexture] take trans=%d flags=%d bytes=%.2fMB items=%u total=%.2fMB\n",
					translation, keyFlags,
					(double)bytes / (1024.0 * 1024.0),
					(unsigned int)NKPreparedTextureQueue.size(),
					(double)NKPreparedTextureBytes / (1024.0 * 1024.0));
			}
			return out.mBuffer != nullptr;
		}
	}

	NKPreparedTextureMissed++;
	if (NKPreparedTextureQueue.empty())
	{
		NKPreparedTextureMissEmpty++;
	}
	else if (!sawSameTexture)
	{
		NKPreparedTextureMissNoTexture++;
	}
	else if (!sawSameTextureTranslation && sawSameTextureFlags)
	{
		NKPreparedTextureMissTranslation++;
	}
	else if (sawSameTextureTranslation && !sawSameTextureFlags)
	{
		NKPreparedTextureMissFlags++;
	}
	else
	{
		NKPreparedTextureMissOther++;
	}
	return false;
}

void NKPreparedTexture_Clear()
{
	std::lock_guard<std::mutex> lock(NKPreparedTextureMutex);
	NKPreparedTextureQueue.clear();
	NKPreparedTextureBytes = 0;
}

static void NKPreparedTexture_ResetStatsLocked()
{
	NKPreparedTexturePeakBytes = NKPreparedTextureBytes;
	NKPreparedTextureStored = 0;
	NKPreparedTextureTaken = 0;
	NKPreparedTextureMissed = 0;
	NKPreparedTextureEvicted = 0;
	NKPreparedTextureExpired = 0;
	NKPreparedTextureStoreDisabled = 0;
	NKPreparedTextureStoreInvalid = 0;
	NKPreparedTextureStoreIndexed = 0;
	NKPreparedTextureStoreZero = 0;
	NKPreparedTextureStoreOversized = 0;
	NKPreparedTextureStoreDuplicateReplaced = 0;
	NKPreparedTextureMissDisabled = 0;
	NKPreparedTextureMissInvalid = 0;
	NKPreparedTextureMissIndexed = 0;
	NKPreparedTextureMissEmpty = 0;
	NKPreparedTextureMissNoTexture = 0;
	NKPreparedTextureMissTranslation = 0;
	NKPreparedTextureMissFlags = 0;
	NKPreparedTextureMissOther = 0;
}

CCMD(nk_warmup_prepared_status)
{
	std::lock_guard<std::mutex> lock(NKPreparedTextureMutex);
	uint64_t lookups = NKPreparedTextureTaken + NKPreparedTextureMissed;
	double hitPct = lookups > 0 ? ((double)NKPreparedTextureTaken * 100.0 / (double)lookups) : 0.0;
	double usePct = NKPreparedTextureStored > 0 ? ((double)NKPreparedTextureTaken * 100.0 / (double)NKPreparedTextureStored) : 0.0;
	Printf("[NKPreparedTexture] enabled=%d items=%u bytes=%.2fMB peak=%.2fMB cap=%dMB max_items=%d expire=%ds stored=%llu taken=%llu missed=%llu hit=%.1f%% use=%.1f%% evicted=%llu expired=%llu\n",
		(int)nk_warmup_prepared_buffers,
		(unsigned int)NKPreparedTextureQueue.size(),
		(double)NKPreparedTextureBytes / (1024.0 * 1024.0),
		(double)NKPreparedTexturePeakBytes / (1024.0 * 1024.0),
		(int)nk_warmup_prepared_max_mb,
		(int)nk_warmup_prepared_max_items,
		(int)nk_warmup_prepared_expire_seconds,
		(unsigned long long)NKPreparedTextureStored,
		(unsigned long long)NKPreparedTextureTaken,
		(unsigned long long)NKPreparedTextureMissed,
		hitPct,
		usePct,
		(unsigned long long)NKPreparedTextureEvicted,
		(unsigned long long)NKPreparedTextureExpired);
	Printf("[NKPreparedTextureDiag] store_skip(disabled=%llu invalid=%llu indexed=%llu zero=%llu oversized=%llu dup_replace=%llu) miss_early(disabled=%llu invalid=%llu indexed=%llu) miss_reason(empty=%llu no_texture=%llu translation=%llu flags=%llu other=%llu)\n",
		(unsigned long long)NKPreparedTextureStoreDisabled,
		(unsigned long long)NKPreparedTextureStoreInvalid,
		(unsigned long long)NKPreparedTextureStoreIndexed,
		(unsigned long long)NKPreparedTextureStoreZero,
		(unsigned long long)NKPreparedTextureStoreOversized,
		(unsigned long long)NKPreparedTextureStoreDuplicateReplaced,
		(unsigned long long)NKPreparedTextureMissDisabled,
		(unsigned long long)NKPreparedTextureMissInvalid,
		(unsigned long long)NKPreparedTextureMissIndexed,
		(unsigned long long)NKPreparedTextureMissEmpty,
		(unsigned long long)NKPreparedTextureMissNoTexture,
		(unsigned long long)NKPreparedTextureMissTranslation,
		(unsigned long long)NKPreparedTextureMissFlags,
		(unsigned long long)NKPreparedTextureMissOther);
}

CCMD(nk_warmup_prepared_clear)
{
	NKPreparedTexture_Clear();
	Printf("[NKPreparedTexture] cleared.\n");
}

CCMD(nk_warmup_prepared_reset_stats)
{
	std::lock_guard<std::mutex> lock(NKPreparedTextureMutex);
	NKPreparedTexture_ResetStatsLocked();
	Printf("[NKPreparedTexture] stats reset.\n");
}

//==========================================================================
//
// 
//
//==========================================================================

FTexture::FTexture (int lumpnum)
	:  SourceLump(lumpnum), bHasCanvas(false)
{
	bTranslucent = -1;
}

//===========================================================================
//
// FTexture::GetBgraBitmap
//
// Default returns just an empty bitmap. This needs to be overridden by
// any subclass that actually does return a software pixel buffer.
//
//===========================================================================

FBitmap FTexture::GetBgraBitmap(const PalEntry* remap, int* ptrans)
{
	FBitmap bmp;
	bmp.Create(Width, Height);
	return bmp;
}

//====================================================================
//
// CheckRealHeight
//
// Checks the posts in a texture and returns the lowest row (plus one)
// of the texture that is actually used.
//
//====================================================================

int FTexture::CheckRealHeight()
{
	auto pixels = Get8BitPixels(false);

	for(int h = GetHeight()-1; h>= 0; h--)
	{
		for(int w = 0; w < GetWidth(); w++)
		{
			if (pixels[h + w * GetHeight()] != 0)
			{
				return h;
			}
		}
	}
	return 0;
}

//===========================================================================
// 
//	Finds gaps in the texture which can be skipped by the renderer
//  This was mainly added to speed up one area in E4M6 of 007LTSD
//
//===========================================================================

bool FTexture::FindHoles(const unsigned char* buffer, int w, int h)
{
	const unsigned char* li;
	int y, x;
	int startdraw, lendraw;
	int gaps[5][2];
	int gapc = 0;


	// already done!
	if (areacount) return false;
	areacount = -1;	//whatever happens next, it shouldn't be done twice!

							// large textures and non-images are excluded for performance reasons
	if (h>512 || !GetImage()) return false;

	startdraw = -1;
	lendraw = 0;
	for (y = 0; y < h; y++)
	{
		li = buffer + w * y * 4 + 3;

		for (x = 0; x < w; x++, li += 4)
		{
			if (*li != 0) break;
		}

		if (x != w)
		{
			// non - transparent
			if (startdraw == -1)
			{
				startdraw = y;
				// merge transparent gaps of less than 16 pixels into the last drawing block
				if (gapc && y <= gaps[gapc - 1][0] + gaps[gapc - 1][1] + 16)
				{
					gapc--;
					startdraw = gaps[gapc][0];
					lendraw = y - startdraw;
				}
				if (gapc == 4) return false;	// too many splits - this isn't worth it
			}
			lendraw++;
		}
		else if (startdraw != -1)
		{
			if (lendraw == 1) lendraw = 2;
			gaps[gapc][0] = startdraw;
			gaps[gapc][1] = lendraw;
			gapc++;

			startdraw = -1;
			lendraw = 0;
		}
	}
	if (startdraw != -1)
	{
		gaps[gapc][0] = startdraw;
		gaps[gapc][1] = lendraw;
		gapc++;
	}
	if (startdraw == 0 && lendraw == h) return false;	// nothing saved so don't create a split list

	if (gapc > 0)
	{
		FloatRect* rcs = (FloatRect*)ImageArena.Alloc(gapc * sizeof(FloatRect));	// allocate this on the image arena

		for (x = 0; x < gapc; x++)
		{
			// gaps are stored as texture (u/v) coordinates
			rcs[x].width = rcs[x].left = -1.0f;
			rcs[x].top = (float)gaps[x][0] / (float)h;
			rcs[x].height = (float)gaps[x][1] / (float)h;
		}
		areas = rcs;
	}
	else areas = nullptr;
	areacount = gapc;

	return true;
}

//----------------------------------------------------------------------------
//
//
//
//----------------------------------------------------------------------------

void FTexture::CheckTrans(unsigned char* buffer, int size, int trans)
{
	if (bTranslucent == -1)
	{
		bTranslucent = trans;
		if (trans == -1)
		{
			uint32_t* dwbuf = (uint32_t*)buffer;
			for (int i = 0; i < size; i++)
			{
				uint32_t alpha = dwbuf[i] >> 24;

				if (alpha != 0xff && alpha != 0)
				{
					bTranslucent = 1;
					return;
				}
			}
			bTranslucent = 0;
		}
	}
}


//===========================================================================
// 
// smooth the edges of transparent fields in the texture
//
//===========================================================================

#ifdef WORDS_BIGENDIAN
#define MSB 0
#define SOME_MASK 0xffffff00
#else
#define MSB 3
#define SOME_MASK 0x00ffffff
#endif

#define CHKPIX(ofs) (l1[(ofs)*4+MSB]==255 ? (( ((uint32_t*)l1)[0] = ((uint32_t*)l1)[ofs]&SOME_MASK), trans=true ) : false)

bool FTexture::SmoothEdges(unsigned char* buffer, int w, int h)
{
	int x, y;
	bool trans = buffer[MSB] == 0; // If I set this to false here the code won't detect textures 
								   // that only contain transparent pixels.
	bool semitrans = false;
	unsigned char* l1;

	if (h <= 1 || w <= 1) return false;  // makes (a) no sense and (b) doesn't work with this code!

	l1 = buffer;


	if (l1[MSB] == 0 && !CHKPIX(1)) CHKPIX(w);
	else if (l1[MSB] < 255) semitrans = true;
	l1 += 4;
	for (x = 1; x < w - 1; x++, l1 += 4)
	{
		if (l1[MSB] == 0 && !CHKPIX(-1) && !CHKPIX(1)) CHKPIX(w);
		else if (l1[MSB] < 255) semitrans = true;
	}
	if (l1[MSB] == 0 && !CHKPIX(-1)) CHKPIX(w);
	else if (l1[MSB] < 255) semitrans = true;
	l1 += 4;

	for (y = 1; y < h - 1; y++)
	{
		if (l1[MSB] == 0 && !CHKPIX(-w) && !CHKPIX(1)) CHKPIX(w);
		else if (l1[MSB] < 255) semitrans = true;
		l1 += 4;
		for (x = 1; x < w - 1; x++, l1 += 4)
		{
			if (l1[MSB] == 0 && !CHKPIX(-w) && !CHKPIX(-1) && !CHKPIX(1) && !CHKPIX(-w - 1) && !CHKPIX(-w + 1) && !CHKPIX(w - 1) && !CHKPIX(w + 1)) CHKPIX(w);
			else if (l1[MSB] < 255) semitrans = true;
		}
		if (l1[MSB] == 0 && !CHKPIX(-w) && !CHKPIX(-1)) CHKPIX(w);
		else if (l1[MSB] < 255) semitrans = true;
		l1 += 4;
	}

	if (l1[MSB] == 0 && !CHKPIX(-w)) CHKPIX(1);
	else if (l1[MSB] < 255) semitrans = true;
	l1 += 4;
	for (x = 1; x < w - 1; x++, l1 += 4)
	{
		if (l1[MSB] == 0 && !CHKPIX(-w) && !CHKPIX(-1)) CHKPIX(1);
		else if (l1[MSB] < 255) semitrans = true;
	}
	if (l1[MSB] == 0 && !CHKPIX(-w)) CHKPIX(-1);
	else if (l1[MSB] < 255) semitrans = true;

	return trans || semitrans;
}

//===========================================================================
// 
// Post-process the texture data after the buffer has been created
//
//===========================================================================

bool FTexture::ProcessData(unsigned char* buffer, int w, int h, bool ispatch)
{
	if (Masked)
	{
		Masked = SmoothEdges(buffer, w, h);
		if (Masked && !ispatch) FindHoles(buffer, w, h);
	}
	return true;
}

//===========================================================================
// 
//	Initializes the buffer for the texture data
//
//===========================================================================

FTextureBuffer FTexture::CreateTexBuffer(int translation, int flags)
{
	FTextureBuffer result;
	if (flags & CTF_Indexed)
	{
		// Indexed textures will never be translated and never be scaled.
		int w = GetWidth(), h = GetHeight();

		auto store = Get8BitPixels(false);
		const uint8_t* p = store.Data();

		result.mBuffer = new uint8_t[w * h];
		result.mWidth = w;
		result.mHeight = h;
		result.mContentId = 0;
		ImageHelpers::FlipNonSquareBlock(result.mBuffer, p, h, w, h);
	}
	else
	{
		unsigned char* buffer = nullptr;
		int W, H;
		int isTransparent = -1;
		bool checkonly = !!(flags & CTF_CheckOnly);

		int exx = !!(flags & CTF_Expand);

		W = GetWidth() + 2 * exx;
		H = GetHeight() + 2 * exx;

		if (!checkonly)
		{
			auto remap = translation <= 0 || IsLuminosityTranslation(translation) ? nullptr : GPalette.TranslationToTable(translation);
			if (remap && remap->Inactive) remap = nullptr;
			if (remap) translation = remap->Index;

			int trans;
			auto Pixels = GetBgraBitmap(remap ? remap->Palette : nullptr, &trans);
			
			if(!exx && Pixels.ClipRect.x == 0 && Pixels.ClipRect.y == 0 && Pixels.ClipRect.width == Pixels.Width && Pixels.ClipRect.height == Pixels.Height && (Pixels.FreeBuffer || !IsLuminosityTranslation(translation)))
			{
				buffer = Pixels.data;
				result.mFreeBuffer = Pixels.FreeBuffer;
				Pixels.FreeBuffer = false;
			}
			else
			{
				buffer = new unsigned char[W * (H + 1) * 4];
				memset(buffer, 0, W * (H + 1) * 4);

				FBitmap bmp(buffer, W * 4, W, H);

				bmp.Blit(exx, exx, Pixels);
			}
			
			if (IsLuminosityTranslation(translation))
			{
				V_ApplyLuminosityTranslation(LuminosityTranslationDesc::fromInt(translation), buffer, W * H);
			}

			if (remap == nullptr)
			{
				CheckTrans(buffer, W * H, trans);
				isTransparent = bTranslucent;
			}
			else
			{
				isTransparent = 0;
				// A translated image is not conclusive for setting the texture's transparency info.
			}
		}

		if (GetImage())
		{
			FContentIdBuilder builder;
			builder.id = 0;
			builder.imageID = GetImage()->GetId();
			builder.translation = max(0, translation);
			builder.expand = exx;
			result.mContentId = builder.id;
		}
		else result.mContentId = 0;	// for non-image backed textures this has no meaning so leave it at 0.

		result.mBuffer = buffer;
		result.mWidth = W;
		result.mHeight = H;

		// Only do postprocessing for image-backed textures. (i.e. not for the burn texture which can also pass through here.)
		if (GetImage() && flags & CTF_ProcessData)
		{
			if (flags & CTF_Upscale) CreateUpsampledTextureBuffer(result, !!isTransparent, checkonly);

			if (!checkonly) ProcessData(result.mBuffer, result.mWidth, result.mHeight, false);
		}
	}
	return result;

}

//===========================================================================
// 
// Dummy texture for the 0-entry.
//
//===========================================================================

bool FTexture::DetermineTranslucency()
{
		// This will calculate all we need, so just discard the result.
		CreateTexBuffer(0);
	return !!bTranslucent;
}

//===========================================================================
// 
// the default just returns an empty texture.
//
//===========================================================================

TArray<uint8_t> FTexture::Get8BitPixels(bool alphatex)
{
	TArray<uint8_t> Pixels(Width * Height, true);
	memset(Pixels.Data(), 0, Width * Height);
	return Pixels;
}

//===========================================================================
// 
//  Finds empty space around the texture. 
//  Used for sprites that got placed into a huge empty frame.
//
//===========================================================================

bool FTexture::TrimBorders(uint16_t* rect)
{

	auto texbuffer = CreateTexBuffer(0);
	int w = texbuffer.mWidth;
	int h = texbuffer.mHeight;
	auto Buffer = texbuffer.mBuffer;

	if (texbuffer.mBuffer == nullptr)
	{
		return false;
	}
	if (w != Width || h != Height)
	{
		// external Hires replacements cannot be trimmed.
		return false;
	}

	int size = w * h;
	if (size == 1)
	{
		// nothing to be done here.
		rect[0] = 0;
		rect[1] = 0;
		rect[2] = 1;
		rect[3] = 1;
		return true;
	}
	int first, last;

	for (first = 0; first < size; first++)
	{
		if (Buffer[first * 4 + 3] != 0) break;
	}
	if (first >= size)
	{
		// completely empty
		rect[0] = 0;
		rect[1] = 0;
		rect[2] = 1;
		rect[3] = 1;
		return true;
	}

	for (last = size - 1; last >= first; last--)
	{
		if (Buffer[last * 4 + 3] != 0) break;
	}

	rect[1] = first / w;
	rect[3] = 1 + last / w - rect[1];

	rect[0] = 0;
	rect[2] = w;

	unsigned char* bufferoff = Buffer + (rect[1] * w * 4);
	h = rect[3];

	for (int x = 0; x < w; x++)
	{
		for (int y = 0; y < h; y++)
		{
			if (bufferoff[(x + y * w) * 4 + 3] != 0) goto outl;
		}
		rect[0]++;
	}
outl:
	rect[2] -= rect[0];

	for (int x = w - 1; rect[2] > 1; x--)
	{
		for (int y = 0; y < h; y++)
		{
			if (bufferoff[(x + y * w) * 4 + 3] != 0)
			{
				return true;
			}
		}
		rect[2]--;
	}
	return true;
}

//===========================================================================
//
// Create a hardware texture for this texture image.
//
//===========================================================================

IHardwareTexture* FTexture::GetHardwareTexture(int translation, int scaleflags)
{
	int indexed = scaleflags & CTF_Indexed;
	if (indexed) translation = -1;
	IHardwareTexture* hwtex = SystemTextures.GetHardwareTexture(translation, scaleflags);
	if (hwtex == nullptr)
	{
		hwtex = screen->CreateHardwareTexture(indexed? 1 : 4);
		SystemTextures.AddHardwareTexture(translation, scaleflags, hwtex);
	}
	return hwtex;
}


//==========================================================================
//
// this must be copied back to textures.cpp later.
//
//==========================================================================

FWrapperTexture::FWrapperTexture(int w, int h, int bits)
{
	Width = w;
	Height = h;
	Format = bits;
	//bNoCompress = true;
	auto hwtex = screen->CreateHardwareTexture(4);
	// todo: Initialize here.
	SystemTextures.AddHardwareTexture(0, false, hwtex);
}

