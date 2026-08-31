// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2004-2016 Christoph Oelckers
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
//--------------------------------------------------------------------------
//
/*
** Texture precaching
**
*/

#include "c_cvars.h"
#include "filesystem.h"
#include "r_data/r_translate.h"
#include "c_dispatch.h"
#include "r_state.h"
#include "actor.h"
#include "models.h"
#include "skyboxtexture.h"
#include "hw_material.h"
#include "image.h"
#include "v_video.h"
#include "v_font.h"
#include "texturemanager.h"
#include "modelrenderer.h"
#include "hw_models.h"
#include "d_main.h"
#include "printf.h"
#include "vm.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <chrono>
#include <stdint.h>
#include <utility>
#include <string.h>

EXTERN_CVAR(Bool, gl_precache)
EXTERN_CVAR(Bool, gl_texture_thread)
EXTERN_CVAR(Bool, nk_gpu_upload_pacing)

// [NKS] Actor-targeted warmup queue, stage 1.
// This is intentionally main-thread only. It proves that we can collect and warm up
// only selected actor classes before adding any worker thread.
struct NKWarmupSpriteJob
{
	FTextureID TextureID;
	int Translation;
	int ScaleFlags;
	int PriorityPixels;
};

static TArray<NKWarmupSpriteJob> NKWarmupJobs;
static TMap<int, bool> NKWarmupQueuedKeys;
static TMap<int, bool> NKWarmupWarmedKeys;
static unsigned int NKWarmupCursor = 0;
static unsigned int NKWarmupProcessed = 0;
static unsigned int NKWarmupSkippedInPrecache = 0;
static unsigned int NKWarmupAddedActors = 0;

CVAR(Bool, nk_warmup_debug, false, CVAR_GLOBALCONFIG)
CVAR(Int, nk_warmup_step_default, 2, CVAR_GLOBALCONFIG)
CVAR(Bool, nk_warmup_auto, false, CVAR_GLOBALCONFIG)
CVAR(Int, nk_warmup_auto_step, 1, CVAR_GLOBALCONFIG)
CVAR(Int, nk_warmup_auto_interval, 3, CVAR_GLOBALCONFIG)
CVAR(Bool, nk_warmup_auto_defer_large, true, CVAR_GLOBALCONFIG)
CVAR(Int, nk_warmup_auto_max_pixels, 524288, CVAR_GLOBALCONFIG)
// [NKS] Stage 5D: optional large-texture policy for loading-screen maps.
// Default keeps the current safe behavior. Enable only for loading maps that
// intentionally prepare a boss/large actor before changing to the real map.
CVAR(Bool, nk_warmup_loading_allow_large, false, CVAR_GLOBALCONFIG)
CVAR(Int, nk_warmup_loading_max_pixels, 2097152, CVAR_GLOBALCONFIG)
// [NKS] Runtime status flag for map-script/loading-screen gating.
// It is intentionally not archived; each new map resets it to 0.
CVAR(Bool, nk_warmup_complete, false, 0)

// [NKS] Stage 4: conservative warmup pacing. This still keeps GPU uploads on
// the main/render thread; it only controls how much automatic warmup work is
// allowed to happen in one visible moment. The loading-screen mode is non-
// archived so each new map starts in the normal conservative mode again.
CVAR(Bool, nk_warmup_pacing, true, CVAR_GLOBALCONFIG)
CVAR(Int, nk_warmup_pacing_budget_ms, 4, CVAR_GLOBALCONFIG)
CVAR(Int, nk_warmup_pacing_slow_ms, 4, CVAR_GLOBALCONFIG)
CVAR(Int, nk_warmup_pacing_cooldown_tics, 2, CVAR_GLOBALCONFIG)
CVAR(Bool, nk_warmup_loading_screen, false, 0)
CVAR(Int, nk_warmup_loading_step, 4, CVAR_GLOBALCONFIG)
CVAR(Int, nk_warmup_loading_interval, 1, CVAR_GLOBALCONFIG)
CVAR(Int, nk_warmup_loading_budget_ms, 8, CVAR_GLOBALCONFIG)
// Reuse textures already warmed earlier in this process when a loading preset
// clears and rebuilds its actor queue. This makes revisiting the same stage much
// shorter without changing the legacy NKWarmupClear behavior outside loading mode.
CVAR(Bool, nk_warmup_loading_reuse_warmed, true, CVAR_GLOBALCONFIG)
// [NKS] Stage 5F: legacy/manual warmup must block until GPU uploads are done.
// This restores the old StageLoad/RunWarmupNow behavior: longer loading is preferred
// over early gameplay stutter. Auto/loading-screen warmup can still use pacing.
CVAR(Bool, nk_warmup_manual_blocking_upload, true, CVAR_GLOBALCONFIG)
CVAR(Bool, nk_warmup_flush_drain_uploads, true, CVAR_GLOBALCONFIG)

// [NKS] Diagnostic profiling for warmup/full precache stalls.
// These CVARs do not change defer policy or warmup target selection.
CVAR(Bool, nk_warmup_profile, false, CVAR_GLOBALCONFIG)
CVAR(Bool, nk_precache_profile, false, CVAR_GLOBALCONFIG)
CVAR(Int, nk_precache_profile_slow_ms, 4, CVAR_GLOBALCONFIG)

// [NKS] Selaco-style CPU prepare worker, stage 2.
// This does NOT upload GPU textures. It only touches CreateTexBuffer ahead of time.
// Default is enabled, but the runtime guard below disables it on single-core CPUs
// or when thread creation fails. GPU uploads are still kept on the main/render thread.
CVAR(Bool, nk_warmup_cpu_worker, true, CVAR_GLOBALCONFIG)
CVAR(Bool, nk_warmup_cpu_worker_debug, false, CVAR_GLOBALCONFIG)
CVAR(Int, nk_warmup_cpu_worker_min_threads, 2, CVAR_GLOBALCONFIG)
CVAR(Int, nk_warmup_cpu_worker_max_queue, 2048, CVAR_GLOBALCONFIG)
CVAR(Bool, nk_warmup_cpu_worker_large_first, true, CVAR_GLOBALCONFIG)
CVAR(Int, nk_warmup_cpu_worker_large_pixels, 524288, CVAR_GLOBALCONFIG)
// Keep CPU prepared-buffer work aligned with nk_warmup_auto_defer_large.
// This avoids large boss sprites being prepared in RAM when the main warmup path
// is deliberately deferring them. Disable only for manual stress testing.
CVAR(Bool, nk_warmup_cpu_worker_respect_defer_large, true, CVAR_GLOBALCONFIG)

// [NKS] Stage 3C diagnostics. These counters are intentionally lightweight and
// do not change warmup behavior. They explain why a manual warmup may report
// "+0 jobs" or why the CPU worker did not receive anything.
enum
{
	NKWARMUP_ADD_INVALID = 0,
	NKWARMUP_ADD_ADDED = 1,
	NKWARMUP_ADD_DUPLICATE = 2,
	NKWARMUP_ADD_ALREADY_WARMED = 3
};

enum
{
	NKWARMUP_KEY_QUEUED = 1,
	NKWARMUP_KEY_WARMED = 2,
	NKWARMUP_KEY_PREPARED = 4,
	NKWARMUP_KEY_WORKERQUEUED = 8
};

enum
{
	NKWARMUP_WORKERQUEUE_DISABLED = 0,
	NKWARMUP_WORKERQUEUE_DUPLICATE = 1,
	NKWARMUP_WORKERQUEUE_STARTFAILED = 2,
	NKWARMUP_WORKERQUEUE_FULL = 3,
	NKWARMUP_WORKERQUEUE_DEFERRED_LARGE = 4,
	NKWARMUP_WORKERQUEUE_QUEUED = 5
};

struct NKWarmupActorDiag
{
	char Name[128];
	unsigned int States = 0;
	unsigned int Frames = 0;
	unsigned int TextureRefs = 0;
	unsigned int ValidRefs = 0;
	unsigned int InvalidRefs = 0;
	unsigned int Added = 0;
	unsigned int DuplicateQueued = 0;
	unsigned int AlreadyWarmedRefs = 0;
	unsigned int AlreadyPreparedRefs = 0;
	unsigned int AlreadyWorkerQueuedRefs = 0;
	unsigned int LargeWorkerRefs = 0;
	unsigned int AutoDeferCandidateRefs = 0;
	unsigned int MaxPixelsSeen = 0;
	unsigned int JobsBefore = 0;
	unsigned int JobsAfter = 0;
};

static NKWarmupActorDiag NKWarmupLastActorDiag;
static unsigned int NKWarmupWorkerQueueAttempts = 0;
static unsigned int NKWarmupWorkerQueueSubmitted = 0;
static unsigned int NKWarmupWorkerQueueSkippedDisabled = 0;
static unsigned int NKWarmupWorkerQueueSkippedDuplicate = 0;
static unsigned int NKWarmupWorkerQueueSkippedStartFailed = 0;
static unsigned int NKWarmupWorkerQueueSkippedFull = 0;
static unsigned int NKWarmupWorkerQueueSkippedDeferredLarge = 0;
static unsigned int NKWarmupLastQueueAllConsidered = 0;
static unsigned int NKWarmupLastQueueAllSubmitted = 0;
static unsigned int NKWarmupLastQueueAllSkippedDisabled = 0;
static unsigned int NKWarmupLastQueueAllSkippedDuplicate = 0;
static unsigned int NKWarmupLastQueueAllSkippedStartFailed = 0;
static unsigned int NKWarmupLastQueueAllSkippedFull = 0;
static unsigned int NKWarmupLastQueueAllSkippedDeferredLarge = 0;
static unsigned int NKWarmupLastQueueAllCursor = 0;
static unsigned int NKWarmupLastQueueAllJobs = 0;

static void NKWarmup_ResetActorDiag(const char *name, unsigned int jobsBefore)
{
	memset(&NKWarmupLastActorDiag, 0, sizeof(NKWarmupLastActorDiag));
	if (name != nullptr)
	{
		strncpy(NKWarmupLastActorDiag.Name, name, sizeof(NKWarmupLastActorDiag.Name) - 1);
		NKWarmupLastActorDiag.Name[sizeof(NKWarmupLastActorDiag.Name) - 1] = 0;
	}
	NKWarmupLastActorDiag.JobsBefore = jobsBefore;
}

static void NKWarmup_ResetLastQueueAllDiag(unsigned int cursor, unsigned int jobs)
{
	NKWarmupLastQueueAllConsidered = cursor <= jobs ? jobs - cursor : 0;
	NKWarmupLastQueueAllSubmitted = 0;
	NKWarmupLastQueueAllSkippedDisabled = 0;
	NKWarmupLastQueueAllSkippedDuplicate = 0;
	NKWarmupLastQueueAllSkippedStartFailed = 0;
	NKWarmupLastQueueAllSkippedFull = 0;
	NKWarmupLastQueueAllSkippedDeferredLarge = 0;
	NKWarmupLastQueueAllCursor = cursor;
	NKWarmupLastQueueAllJobs = jobs;
}

static void NKWarmup_RecordLastQueueAllResult(int result)
{
	switch (result)
	{
	case NKWARMUP_WORKERQUEUE_QUEUED:
		NKWarmupLastQueueAllSubmitted++;
		break;
	case NKWARMUP_WORKERQUEUE_DISABLED:
		NKWarmupLastQueueAllSkippedDisabled++;
		break;
	case NKWARMUP_WORKERQUEUE_DUPLICATE:
		NKWarmupLastQueueAllSkippedDuplicate++;
		break;
	case NKWARMUP_WORKERQUEUE_STARTFAILED:
		NKWarmupLastQueueAllSkippedStartFailed++;
		break;
	case NKWARMUP_WORKERQUEUE_FULL:
		NKWarmupLastQueueAllSkippedFull++;
		break;
	case NKWARMUP_WORKERQUEUE_DEFERRED_LARGE:
		NKWarmupLastQueueAllSkippedDeferredLarge++;
		break;
	}
}

static int NKWarmupAutoCounter = 0;
static unsigned int NKWarmupAutoDeferredLarge = 0;
static unsigned int NKWarmupProfileJobs = 0;
static unsigned int NKWarmupProfileSlowJobs = 0;
static uint64_t NKWarmupProfileTotalUS = 0;
static uint64_t NKWarmupProfileMaxUS = 0;
static unsigned int NKPrecacheProfileCalls = 0;
static unsigned int NKPrecacheProfileSlowCalls = 0;
static uint64_t NKPrecacheProfileTotalUS = 0;
static uint64_t NKPrecacheProfileMaxUS = 0;
static int NKWarmupPacingCooldown = 0;
static unsigned int NKWarmupPacingSkippedTicks = 0;
static unsigned int NKWarmupPacingBatches = 0;
static unsigned int NKWarmupPacingBudgetHits = 0;
static unsigned int NKWarmupPacingCooldowns = 0;
static int NKWarmupPacingLastProcessed = 0;
static int NKWarmupPacingLastStep = 0;
static int NKWarmupPacingLastInterval = 0;
static int NKWarmupPacingLastBudgetMS = 0;
static uint64_t NKWarmupPacingLastBatchUS = 0;
static uint64_t NKWarmupPacingLastMaxJobUS = 0;
static uint64_t NKWarmupPacingMaxBatchUS = 0;
static int NKWarmupManualFlushDepth = 0;

static uint64_t NKWarmup_NowUS(void)
{
	using namespace std::chrono;
	return (uint64_t)duration_cast<microseconds>(high_resolution_clock::now().time_since_epoch()).count();
}

static double NKWarmup_ToMS(uint64_t us)
{
	return (double)us / 1000.0;
}

static int NKWarmup_ProfileThresholdMS(void)
{
	int threshold = nk_precache_profile_slow_ms;
	if (threshold < 0) threshold = 0;
	return threshold;
}

static int NKWarmup_ClampPositiveInt(int value, int fallback)
{
	if (value < 1) return fallback;
	return value;
}

static unsigned int NKWarmup_RemainingJobs(void)
{
	return NKWarmupCursor <= NKWarmupJobs.Size() ? (unsigned int)(NKWarmupJobs.Size() - NKWarmupCursor) : 0;
}

static int NKWarmup_EffectiveAutoStep(void)
{
	int step = nk_warmup_loading_screen ? nk_warmup_loading_step : nk_warmup_auto_step;
	return NKWarmup_ClampPositiveInt(step, 1);
}

static int NKWarmup_EffectiveAutoInterval(void)
{
	int interval = nk_warmup_loading_screen ? nk_warmup_loading_interval : nk_warmup_auto_interval;
	return NKWarmup_ClampPositiveInt(interval, 1);
}

static int NKWarmup_EffectiveBudgetMS(void)
{
	if (!nk_warmup_pacing) return 0;
	int budget = nk_warmup_loading_screen ? nk_warmup_loading_budget_ms : nk_warmup_pacing_budget_ms;
	if (budget < 0) budget = 0;
	return budget;
}

static int NKWarmup_PacingSlowMS(void)
{
	int slow = nk_warmup_pacing_slow_ms;
	if (slow < 0) slow = 0;
	return slow;
}

static int NKWarmup_PacingCooldownTics(void)
{
	int cooldown = nk_warmup_pacing_cooldown_tics;
	if (cooldown < 0) cooldown = 0;
	return cooldown;
}

static void NKWarmup_ResetPacingRuntime(bool clearTotals)
{
	NKWarmupPacingCooldown = 0;
	NKWarmupPacingLastProcessed = 0;
	NKWarmupPacingLastStep = 0;
	NKWarmupPacingLastInterval = 0;
	NKWarmupPacingLastBudgetMS = 0;
	NKWarmupPacingLastBatchUS = 0;
	NKWarmupPacingLastMaxJobUS = 0;
	if (clearTotals)
	{
		NKWarmupPacingSkippedTicks = 0;
		NKWarmupPacingBatches = 0;
		NKWarmupPacingBudgetHits = 0;
		NKWarmupPacingCooldowns = 0;
		NKWarmupPacingMaxBatchUS = 0;
	}
}

static void NKWarmup_RecordWarmupProfile(FGameTexture *gtex, int translation, const char *source, uint64_t registerUS, uint64_t validateUS, uint64_t uploadUS, uint64_t totalUS)
{
	NKWarmupProfileJobs++;
	NKWarmupProfileTotalUS += totalUS;
	if (totalUS > NKWarmupProfileMaxUS) NKWarmupProfileMaxUS = totalUS;

	int thresholdMS = NKWarmup_ProfileThresholdMS();
	if (!nk_warmup_profile || totalUS < (uint64_t)thresholdMS * 1000ull)
	{
		return;
	}

	NKWarmupProfileSlowJobs++;
	const char *name = gtex != nullptr ? gtex->GetName().GetChars() : "<null>";
	int width = 0;
	int height = 0;
	if (gtex != nullptr && gtex->isValid() && gtex->GetTexture() != nullptr)
	{
		width = gtex->GetTexture()->GetWidth();
		height = gtex->GetTexture()->GetHeight();
	}
	Printf("[NKWarmupProfile] source=%s tex=%s size=%dx%d trans=%d register=%.3fms validate=%.3fms upload=%.3fms total=%.3fms progress=%u/%u\n",
		source != nullptr ? source : "warmup",
		name, width, height, translation,
		NKWarmup_ToMS(registerUS),
		NKWarmup_ToMS(validateUS),
		NKWarmup_ToMS(uploadUS),
		NKWarmup_ToMS(totalUS),
		NKWarmupCursor,
		(unsigned int)NKWarmupJobs.Size());
}

static void NKWarmup_RecordPrecacheProfile(FGameTexture *gtex, int translation, const char *source, uint64_t totalUS)
{
	NKPrecacheProfileCalls++;
	NKPrecacheProfileTotalUS += totalUS;
	if (totalUS > NKPrecacheProfileMaxUS) NKPrecacheProfileMaxUS = totalUS;

	int thresholdMS = NKWarmup_ProfileThresholdMS();
	if (!nk_precache_profile || totalUS < (uint64_t)thresholdMS * 1000ull)
	{
		return;
	}

	NKPrecacheProfileSlowCalls++;
	const char *name = gtex != nullptr ? gtex->GetName().GetChars() : "<null>";
	int width = 0;
	int height = 0;
	if (gtex != nullptr && gtex->isValid() && gtex->GetTexture() != nullptr)
	{
		width = gtex->GetTexture()->GetWidth();
		height = gtex->GetTexture()->GetHeight();
	}
	Printf("[NKPrecacheProfile] source=%s tex=%s size=%dx%d trans=%d total=%.3fms\n",
		source != nullptr ? source : "precache",
		name, width, height, translation,
		NKWarmup_ToMS(totalUS));
}

static std::thread *NKWarmupWorkerThread = nullptr;
static std::mutex NKWarmupWorkerMutex;
static std::mutex NKWarmupKeyMutex;
static std::condition_variable NKWarmupWorkerCV;
static std::deque<NKWarmupSpriteJob> NKWarmupWorkerQueue;
static TMap<int, bool> NKWarmupPreparedKeys;
static TMap<int, bool> NKWarmupWorkerQueuedKeys;
static std::atomic<bool> NKWarmupWorkerStop(false);
static std::atomic<bool> NKWarmupWorkerRunning(false);
static std::atomic<int> NKWarmupWorkerQueued(0);
static std::atomic<int> NKWarmupWorkerDone(0);
static std::atomic<bool> NKWarmupWorkerThreadDenied(false);
static std::atomic<bool> NKWarmupWorkerThreadFailed(false);
static std::atomic<int> NKWarmupWorkerLargeQueued(0);
static std::atomic<int> NKWarmupWorkerLargeDone(0);

static int NKWarmup_MakeKey(FTextureID texid, int translation)
{
	return (texid.GetIndex() << 12) ^ (translation & 0xfff);
}

static int NKWarmup_HardwareThreads(void)
{
	unsigned int hw = std::thread::hardware_concurrency();
	if (hw > 0x7fffffff) hw = 0x7fffffff;
	return (int)hw;
}

static int NKWarmup_MinWorkerThreads(void)
{
	int minThreads = nk_warmup_cpu_worker_min_threads;
	if (minThreads < 1) minThreads = 1;
	return minThreads;
}

static bool NKWarmup_CpuWorkerAllowed(void)
{
	if (!nk_warmup_cpu_worker)
	{
		NKWarmupWorkerThreadDenied.store(false);
		return false;
	}

	int hw = NKWarmup_HardwareThreads();
	int minThreads = NKWarmup_MinWorkerThreads();
	if (hw > 0 && hw < minThreads)
	{
		NKWarmupWorkerThreadDenied.store(true);
		return false;
	}

	NKWarmupWorkerThreadDenied.store(false);
	return true;
}

static int NKWarmup_EffectiveMaxPixels(void)
{
	int maxPixels = nk_warmup_auto_max_pixels;
	if (nk_warmup_loading_screen && nk_warmup_loading_allow_large)
	{
		maxPixels = nk_warmup_loading_max_pixels;
	}
	if (maxPixels < 0) maxPixels = 0;
	return maxPixels;
}

static int NKWarmup_WorkerQueueLimit(void)
{
	int limit = nk_warmup_cpu_worker_max_queue;
	if (limit < 0) limit = 0;
	return limit;
}

static int NKWarmup_LargeTexturePixels(void)
{
	int pixels = nk_warmup_cpu_worker_large_pixels;
	if (pixels < 0) pixels = 0;
	return pixels;
}

static int NKWarmup_TexturePixels(FGameTexture *gtex)
{
	if (gtex == nullptr || !gtex->isValid()) return 0;
	auto tex = gtex->GetTexture();
	if (tex == nullptr) return 0;
	int64_t pixels = (int64_t)tex->GetWidth() * (int64_t)tex->GetHeight();
	if (pixels <= 0) return 0;
	if (pixels > 0x7fffffff) return 0x7fffffff;
	return (int)pixels;
}

static bool NKWarmup_IsLargeWorkerJob(const NKWarmupSpriteJob &job)
{
	int largePixels = NKWarmup_LargeTexturePixels();
	return largePixels > 0 && job.PriorityPixels >= largePixels;
}

static unsigned int NKWarmup_WorkerQueuedKeyCount(void);

static bool NKWarmup_ShouldDeferWorkerJob(const NKWarmupSpriteJob &job)
{
	if (!nk_warmup_cpu_worker_respect_defer_large) return false;
	if (!nk_warmup_auto_defer_large) return false;

	int maxPixels = NKWarmup_EffectiveMaxPixels();
	if (maxPixels <= 0) return false;

	// Use the same strict comparison as NKWarmup_ShouldAutoDeferTexture().
	return job.PriorityPixels > maxPixels;
}

static bool NKWarmup_WorkerIdle(void)
{
	return NKWarmupWorkerQueued.load() <= NKWarmupWorkerDone.load() && NKWarmup_WorkerQueuedKeyCount() == 0;
}

static void NKWarmup_SetComplete(bool complete)
{
	nk_warmup_complete = complete;
}


static bool NKWarmup_IsWarmed(FTextureID texid, int translation)
{
	if (!texid.isValid()) return false;
	std::lock_guard<std::mutex> lock(NKWarmupKeyMutex);
	return NKWarmupWarmedKeys.CheckKey(NKWarmup_MakeKey(texid, translation));
}

static void NKWarmup_MarkWarmed(FTextureID texid, int translation)
{
	if (!texid.isValid()) return;
	std::lock_guard<std::mutex> lock(NKWarmupKeyMutex);
	NKWarmupWarmedKeys.Insert(NKWarmup_MakeKey(texid, translation), true);
}

static void NKWarmup_AdvanceCursorPastWarmed(void)
{
	while (NKWarmupCursor < NKWarmupJobs.Size())
	{
		NKWarmupSpriteJob &job = NKWarmupJobs[NKWarmupCursor];
		if (!NKWarmup_IsWarmed(job.TextureID, job.Translation))
		{
			break;
		}
		NKWarmupCursor++;
	}
}

static bool NKWarmup_ShouldAutoDeferTexture(FGameTexture *gtex)
{
	if (!nk_warmup_auto_defer_large) return false;

	int maxPixels = NKWarmup_EffectiveMaxPixels();
	if (maxPixels <= 0) return false;
	if (gtex == nullptr || !gtex->isValid()) return false;

	auto tex = gtex->GetTexture();
	if (tex == nullptr) return false;

	int pixels = tex->GetWidth() * tex->GetHeight();
	return pixels > maxPixels;
}

static bool NKWarmup_UseBackgroundCache(void)
{
	// Stage 5A: nk_gpu_upload_pacing enables the safe main-thread upload queue
	// even when the old gl_texture_thread switch is left off. This still does
	// not enable background/worker GPU uploads.
	return (gl_texture_thread || nk_gpu_upload_pacing) && screen != nullptr && screen->SupportsBackgroundCache();
}

static bool NKWarmup_BackgroundCacheIdle(void)
{
	if (!NKWarmup_UseBackgroundCache()) return true;
	return !screen->CachingActive();
}

static void NKWarmup_SubmitMaterial(FMaterial *mat, int translation, bool autoMode)
{
	if (mat == nullptr) return;

	// Stage 5F: manual/legacy warmup paths such as StageLoad -> RunWarmupNow
	// must behave like the old blocking preload. They should finish GPU upload work
	// before returning, even when nk_gpu_upload_pacing enables the safe background
	// queue for automatic/loading-screen warmup.
	bool forceBlocking = !autoMode && (nk_warmup_manual_blocking_upload || NKWarmupManualFlushDepth > 0);
	if (!forceBlocking && NKWarmup_UseBackgroundCache())
	{
		screen->BackgroundCacheMaterial(mat, FTranslationID::fromInt(translation), true, autoMode);
	}
	else
	{
		screen->PrecacheMaterial(mat, translation);
	}
}

static int NKWarmup_AddTextureJob(FTextureID texid, int translation, int *outPixels = nullptr, int *outKeyFlags = nullptr)
{
	if (outPixels != nullptr) *outPixels = 0;
	if (outKeyFlags != nullptr) *outKeyFlags = 0;
	if (!texid.isValid()) return NKWARMUP_ADD_INVALID;

	int key = NKWarmup_MakeKey(texid, translation);

	int scaleflags = CTF_Expand;
	auto gtex = TexMan.GameByIndex(texid.GetIndex());
	if (gtex != nullptr && shouldUpscale(gtex, UF_Sprite))
	{
		scaleflags |= CTF_Upscale;
	}

	NKWarmupSpriteJob job;
	job.TextureID = texid;
	job.Translation = translation;
	job.ScaleFlags = scaleflags;
	job.PriorityPixels = NKWarmup_TexturePixels(gtex);
	if (outPixels != nullptr) *outPixels = job.PriorityPixels;

	std::lock_guard<std::mutex> lock(NKWarmupKeyMutex);
	int keyFlags = 0;
	if (NKWarmupQueuedKeys.CheckKey(key)) keyFlags |= NKWARMUP_KEY_QUEUED;
	if (NKWarmupWarmedKeys.CheckKey(key)) keyFlags |= NKWARMUP_KEY_WARMED;
	if (NKWarmupPreparedKeys.CheckKey(key)) keyFlags |= NKWARMUP_KEY_PREPARED;
	if (NKWarmupWorkerQueuedKeys.CheckKey(key)) keyFlags |= NKWARMUP_KEY_WORKERQUEUED;
	if (outKeyFlags != nullptr) *outKeyFlags = keyFlags;

	if (keyFlags & NKWARMUP_KEY_QUEUED)
	{
		return NKWARMUP_ADD_DUPLICATE;
	}
	if (keyFlags & NKWARMUP_KEY_WARMED)
	{
		// Loading-screen queue rebuilds can retain warm residency records. Do not
		// spend dozens of paced ticks walking jobs that are already complete.
		return NKWARMUP_ADD_ALREADY_WARMED;
	}

	NKWarmupJobs.Push(job);
	NKWarmupQueuedKeys.Insert(key, true);
	return NKWARMUP_ADD_ADDED;
}


static bool NKWarmup_IsPrepared(FTextureID texid, int translation)
{
	if (!texid.isValid()) return false;
	std::lock_guard<std::mutex> lock(NKWarmupKeyMutex);
	return NKWarmupPreparedKeys.CheckKey(NKWarmup_MakeKey(texid, translation));
}

static void NKWarmup_MarkPrepared(FTextureID texid, int translation)
{
	if (!texid.isValid()) return;
	std::lock_guard<std::mutex> lock(NKWarmupKeyMutex);
	NKWarmupPreparedKeys.Insert(NKWarmup_MakeKey(texid, translation), true);
}

static unsigned int NKWarmup_WarmedCount(void)
{
	std::lock_guard<std::mutex> lock(NKWarmupKeyMutex);
	return (unsigned int)NKWarmupWarmedKeys.CountUsed();
}

static unsigned int NKWarmup_PreparedCount(void)
{
	std::lock_guard<std::mutex> lock(NKWarmupKeyMutex);
	return (unsigned int)NKWarmupPreparedKeys.CountUsed();
}

static unsigned int NKWarmup_WorkerQueuedKeyCount(void)
{
	std::lock_guard<std::mutex> lock(NKWarmupKeyMutex);
	return (unsigned int)NKWarmupWorkerQueuedKeys.CountUsed();
}

static bool NKWarmup_TryMarkWorkerQueued(FTextureID texid, int translation)
{
	if (!texid.isValid()) return false;

	int key = NKWarmup_MakeKey(texid, translation);
	std::lock_guard<std::mutex> lock(NKWarmupKeyMutex);
	if (NKWarmupWarmedKeys.CheckKey(key)) return false;
	if (NKWarmupPreparedKeys.CheckKey(key)) return false;
	if (NKWarmupWorkerQueuedKeys.CheckKey(key)) return false;

	NKWarmupWorkerQueuedKeys.Insert(key, true);
	return true;
}

static void NKWarmup_UnmarkWorkerQueued(FTextureID texid, int translation)
{
	if (!texid.isValid()) return;

	std::lock_guard<std::mutex> lock(NKWarmupKeyMutex);
	NKWarmupWorkerQueuedKeys.Remove(NKWarmup_MakeKey(texid, translation));
}

static void NKWarmup_WorkerMain(void)
{
	while (!NKWarmupWorkerStop.load())
	{
		NKWarmupSpriteJob job;
		bool hasJob = false;

		{
			std::unique_lock<std::mutex> lock(NKWarmupWorkerMutex);
			NKWarmupWorkerCV.wait(lock, []() {
				return NKWarmupWorkerStop.load() || !NKWarmupWorkerQueue.empty();
			});

			if (NKWarmupWorkerStop.load())
			{
				break;
			}

			if (!NKWarmupWorkerQueue.empty())
			{
				job = NKWarmupWorkerQueue.front();
				NKWarmupWorkerQueue.pop_front();
				hasJob = true;
			}
		}

		if (!hasJob)
		{
			continue;
		}

		if (NKWarmup_ShouldDeferWorkerJob(job))
		{
			// The job may have been queued before the defer policy changed. Do not
			// build a large prepared buffer that the main warmup path is deferring.
		}
		else if (!NKWarmup_IsWarmed(job.TextureID, job.Translation) && !NKWarmup_IsPrepared(job.TextureID, job.Translation))
		{
			auto gtex = TexMan.GameByIndex(job.TextureID.GetIndex());
			if (gtex != nullptr && gtex->isValid())
			{
				auto tex = gtex->GetTexture();
				if (tex != nullptr && tex->GetImage() != nullptr)
				{
					// CPU-only warmup. No GPU resource creation here.
					// Stage 3: keep the prepared software buffer briefly so the main/render
					// thread can upload it without rebuilding it during BindOrCreate.
					FTextureBuffer prepared = tex->CreateTexBuffer(job.Translation, job.ScaleFlags | CTF_ProcessData);
					NKPreparedTexture_Store(tex, job.Translation, job.ScaleFlags, std::move(prepared));
					NKWarmup_MarkPrepared(job.TextureID, job.Translation);
				}
			}
		}

		if (NKWarmup_IsLargeWorkerJob(job))
		{
			NKWarmupWorkerLargeDone.fetch_add(1);
		}

		NKWarmup_UnmarkWorkerQueued(job.TextureID, job.Translation);
		NKWarmupWorkerDone.fetch_add(1);
	}

	NKWarmupWorkerRunning.store(false);
}

static void NKWarmup_StopWorker(void)
{
	NKWarmupWorkerStop.store(true);
	NKWarmupWorkerCV.notify_all();

	if (NKWarmupWorkerThread != nullptr)
	{
		if (NKWarmupWorkerThread->joinable())
		{
			NKWarmupWorkerThread->join();
		}
		delete NKWarmupWorkerThread;
		NKWarmupWorkerThread = nullptr;
	}

	{
		std::lock_guard<std::mutex> lock(NKWarmupWorkerMutex);
		NKWarmupWorkerQueue.clear();
	}
	{
		std::lock_guard<std::mutex> lock(NKWarmupKeyMutex);
		NKWarmupWorkerQueuedKeys.Clear();
	}

	NKWarmupWorkerStop.store(false);
	NKWarmupWorkerRunning.store(false);
}

static bool NKWarmup_StartWorker(void)
{
	if (!NKWarmup_CpuWorkerAllowed())
	{
		return false;
	}

	if (NKWarmupWorkerThread != nullptr)
	{
		if (NKWarmupWorkerThread->joinable())
		{
			return true;
		}
		delete NKWarmupWorkerThread;
		NKWarmupWorkerThread = nullptr;
	}

	NKWarmupWorkerStop.store(false);
	NKWarmupWorkerRunning.store(true);
	NKWarmupWorkerThreadFailed.store(false);

	try
	{
		NKWarmupWorkerThread = new std::thread([]() { NKWarmup_WorkerMain(); });
	}
	catch (...)
	{
		NKWarmupWorkerThread = nullptr;
		NKWarmupWorkerRunning.store(false);
		NKWarmupWorkerThreadFailed.store(true);
		if (nk_warmup_cpu_worker_debug)
		{
			Printf("[NKWarmupWorker] thread creation failed; CPU worker disabled for this run.\n");
		}
		return false;
	}

	if (nk_warmup_cpu_worker_debug)
	{
		Printf("[NKWarmupWorker] started. hw_threads=%d min_threads=%d max_queue=%d\n",
			NKWarmup_HardwareThreads(),
			NKWarmup_MinWorkerThreads(),
			NKWarmup_WorkerQueueLimit());
	}

	return true;
}

static void NKWarmup_InsertWorkerJobLocked(const NKWarmupSpriteJob &job)
{
	if (!nk_warmup_cpu_worker_large_first || job.PriorityPixels <= 0 || NKWarmupWorkerQueue.empty())
	{
		NKWarmupWorkerQueue.push_back(job);
		return;
	}

	for (auto it = NKWarmupWorkerQueue.begin(); it != NKWarmupWorkerQueue.end(); ++it)
	{
		if (it->PriorityPixels < job.PriorityPixels)
		{
			NKWarmupWorkerQueue.insert(it, job);
			return;
		}
	}

	NKWarmupWorkerQueue.push_back(job);
}

static int NKWarmup_QueueWorkerJob(const NKWarmupSpriteJob &job)
{
	NKWarmupWorkerQueueAttempts++;

	if (!NKWarmup_CpuWorkerAllowed())
	{
		NKWarmupWorkerQueueSkippedDisabled++;
		return NKWARMUP_WORKERQUEUE_DISABLED;
	}

	if (NKWarmup_ShouldDeferWorkerJob(job))
	{
		NKWarmupWorkerQueueSkippedDeferredLarge++;
		return NKWARMUP_WORKERQUEUE_DEFERRED_LARGE;
	}

	if (!NKWarmup_TryMarkWorkerQueued(job.TextureID, job.Translation))
	{
		NKWarmupWorkerQueueSkippedDuplicate++;
		return NKWARMUP_WORKERQUEUE_DUPLICATE;
	}

	if (!NKWarmup_StartWorker())
	{
		NKWarmup_UnmarkWorkerQueued(job.TextureID, job.Translation);
		NKWarmupWorkerQueueSkippedStartFailed++;
		return NKWARMUP_WORKERQUEUE_STARTFAILED;
	}

	{
		std::lock_guard<std::mutex> lock(NKWarmupWorkerMutex);
		int queueLimit = NKWarmup_WorkerQueueLimit();
		if (queueLimit > 0 && (int)NKWarmupWorkerQueue.size() >= queueLimit)
		{
			NKWarmup_UnmarkWorkerQueued(job.TextureID, job.Translation);
			NKWarmupWorkerQueueSkippedFull++;
			return NKWARMUP_WORKERQUEUE_FULL;
		}

		NKWarmup_InsertWorkerJobLocked(job);
		NKWarmupWorkerQueued.fetch_add(1);
		NKWarmupWorkerQueueSubmitted++;
		if (NKWarmup_IsLargeWorkerJob(job))
		{
			NKWarmupWorkerLargeQueued.fetch_add(1);
		}
	}
	NKWarmupWorkerCV.notify_one();
	return NKWARMUP_WORKERQUEUE_QUEUED;
}

static void NKWarmup_QueueAllForWorker(void)
{
	unsigned int cursor = NKWarmupCursor;
	unsigned int jobs = (unsigned int)NKWarmupJobs.Size();
	NKWarmup_ResetLastQueueAllDiag(cursor, jobs);

	if (!NKWarmup_CpuWorkerAllowed())
	{
		NKWarmupLastQueueAllSkippedDisabled = NKWarmupLastQueueAllConsidered;
		return;
	}

	for (unsigned int i = NKWarmupCursor; i < NKWarmupJobs.Size(); i++)
	{
		int result = NKWarmup_QueueWorkerJob(NKWarmupJobs[i]);
		NKWarmup_RecordLastQueueAllResult(result);
	}

	if (nk_warmup_cpu_worker_debug)
	{
		Printf("[NKWarmupWorker] queued=%d done=%d large=%d/%d pending_keys=%u jobs=%u cursor=%u considered=%u submitted=%u skipped(disabled=%u duplicate=%u startfailed=%u full=%u defer_large=%u) hw_threads=%d allowed=%d large_first=%d large_pixels=%d respect_defer=%d\n",
			NKWarmupWorkerQueued.load(),
			NKWarmupWorkerDone.load(),
			NKWarmupWorkerLargeDone.load(),
			NKWarmupWorkerLargeQueued.load(),
			NKWarmup_WorkerQueuedKeyCount(),
			(unsigned int)NKWarmupJobs.Size(),
			NKWarmupLastQueueAllCursor,
			NKWarmupLastQueueAllConsidered,
			NKWarmupLastQueueAllSubmitted,
			NKWarmupLastQueueAllSkippedDisabled,
			NKWarmupLastQueueAllSkippedDuplicate,
			NKWarmupLastQueueAllSkippedStartFailed,
			NKWarmupLastQueueAllSkippedFull,
			NKWarmupLastQueueAllSkippedDeferredLarge,
			NKWarmup_HardwareThreads(),
			(int)NKWarmup_CpuWorkerAllowed(),
			(int)nk_warmup_cpu_worker_large_first,
			NKWarmup_LargeTexturePixels(),
			(int)nk_warmup_cpu_worker_respect_defer_large);
	}
}

static bool NKWarmup_AddActorClass(PClassActor *cls)
{
	if (cls == nullptr) return false;

	NKWarmup_ResetActorDiag(cls->TypeName.GetChars(), (unsigned int)NKWarmupJobs.Size());

	auto defaults = GetDefaultByType(cls);
	auto remap = defaults != nullptr ? GPalette.TranslationToTable(defaults->Translation.index()) : nullptr;
	int gltrans = remap == nullptr ? 0 : remap->Index;

	int largeWorkerPixels = NKWarmup_LargeTexturePixels();
	int autoMaxPixels = NKWarmup_EffectiveMaxPixels();
	if (autoMaxPixels < 0) autoMaxPixels = 0;

	for (unsigned i = 0; i < cls->GetStateCount(); i++)
	{
		NKWarmupLastActorDiag.States++;
		auto &state = cls->GetStates()[i];
		if ((unsigned)state.sprite >= sprites.Size()) continue;

		const auto &sprdef = sprites[state.sprite];
		for (int j = 0; j < sprdef.numframes; j++)
		{
			NKWarmupLastActorDiag.Frames++;
			const spriteframe_t *frame = &SpriteFrames[sprdef.spriteframes + j];
			for (int k = 0; k < 16; k++)
			{
				NKWarmupLastActorDiag.TextureRefs++;
				int pixels = 0;
				int keyFlags = 0;
				int result = NKWarmup_AddTextureJob(frame->Texture[k], gltrans, &pixels, &keyFlags);
				if (result == NKWARMUP_ADD_INVALID)
				{
					NKWarmupLastActorDiag.InvalidRefs++;
					continue;
				}
				NKWarmupLastActorDiag.ValidRefs++;
				if ((unsigned int)pixels > NKWarmupLastActorDiag.MaxPixelsSeen)
				{
					NKWarmupLastActorDiag.MaxPixelsSeen = (unsigned int)pixels;
				}
				if (largeWorkerPixels > 0 && pixels >= largeWorkerPixels)
				{
					NKWarmupLastActorDiag.LargeWorkerRefs++;
				}
				if (nk_warmup_auto_defer_large && autoMaxPixels > 0 && pixels > autoMaxPixels)
				{
					NKWarmupLastActorDiag.AutoDeferCandidateRefs++;
				}
				if (keyFlags & NKWARMUP_KEY_WARMED) NKWarmupLastActorDiag.AlreadyWarmedRefs++;
				if (keyFlags & NKWARMUP_KEY_PREPARED) NKWarmupLastActorDiag.AlreadyPreparedRefs++;
				if (keyFlags & NKWARMUP_KEY_WORKERQUEUED) NKWarmupLastActorDiag.AlreadyWorkerQueuedRefs++;
				if (result == NKWARMUP_ADD_ADDED)
				{
					NKWarmupLastActorDiag.Added++;
				}
				else if (result == NKWARMUP_ADD_DUPLICATE)
				{
					NKWarmupLastActorDiag.DuplicateQueued++;
				}
				else if (result == NKWARMUP_ADD_ALREADY_WARMED)
				{
					// AlreadyWarmedRefs was counted from keyFlags above.
				}
			}
		}
	}

	NKWarmupLastActorDiag.JobsAfter = (unsigned int)NKWarmupJobs.Size();
	if (NKWarmupLastActorDiag.Added > 0)
	{
		NKWarmup_SetComplete(false);
	}
	NKWarmupAddedActors++;
	NKWarmup_QueueAllForWorker();

	if (nk_warmup_debug)
	{
		Printf("[NKWarmup] queued actor %s, refs=%u valid=%u added=%u duplicate=%u total jobs=%u\n",
			cls->TypeName.GetChars(),
			NKWarmupLastActorDiag.TextureRefs,
			NKWarmupLastActorDiag.ValidRefs,
			NKWarmupLastActorDiag.Added,
			NKWarmupLastActorDiag.DuplicateQueued,
			(unsigned)NKWarmupJobs.Size());
	}
	return true;
}

static int NKWarmup_ProcessJobs(int maxJobs, bool autoMode = false, int budgetMS = 0, bool *outBudgetHit = nullptr, uint64_t *outBatchUS = nullptr, uint64_t *outMaxJobUS = nullptr)
{
	if (outBudgetHit != nullptr) *outBudgetHit = false;
	if (outBatchUS != nullptr) *outBatchUS = 0;
	if (outMaxJobUS != nullptr) *outMaxJobUS = 0;
	if (maxJobs <= 0) return 0;
	if (budgetMS < 0) budgetMS = 0;

	uint64_t budgetUS = (uint64_t)budgetMS * 1000ull;
	uint64_t batchUS = 0;
	uint64_t maxJobUS = 0;
	bool budgetHit = false;
	int processed = 0;

	screen->StartPrecaching();

	FImageSource::BeginPrecaching();

	while (processed < maxJobs && NKWarmupCursor < NKWarmupJobs.Size())
	{
		uint64_t jobStartUS = NKWarmup_NowUS();
		NKWarmupSpriteJob &job = NKWarmupJobs[NKWarmupCursor++];

		// If a later full precache already warmed this texture, consume the queue item
		// without doing the expensive work again.
		if (!NKWarmup_IsWarmed(job.TextureID, job.Translation))
		{
			auto gtex = TexMan.GameByIndex(job.TextureID.GetIndex());
			if (gtex != nullptr && gtex->isValid())
			{
				if (autoMode && NKWarmup_ShouldAutoDeferTexture(gtex))
				{
					// Large sprite textures can still cause a visible frame spike even with step=1.
					// Leave them for explicit/manual precache or a loading/fade moment.
					NKWarmupAutoDeferredLarge++;
				}
				else
				{
					auto tex = gtex->GetTexture();
					if (tex != nullptr && tex->GetImage() != nullptr)
					{
						uint64_t t0 = NKWarmup_NowUS();
						FImageSource::RegisterForPrecache(tex->GetImage(), V_IsTrueColor());
						uint64_t t1 = NKWarmup_NowUS();

						FMaterial *mat = FMaterial::ValidateTexture(gtex, job.ScaleFlags);
						uint64_t t2 = NKWarmup_NowUS();
						if (mat != nullptr)
						{
							NKWarmup_SubmitMaterial(mat, job.Translation, autoMode);

							// With current fallback renderers this means uploaded. With a future
							// background renderer override this means submitted/queued; renderer
							// texture state will prevent duplicate work once the upload completes.
							NKWarmup_MarkWarmed(job.TextureID, job.Translation);
						}
						uint64_t t3 = NKWarmup_NowUS();

						NKWarmup_RecordWarmupProfile(gtex, job.Translation, autoMode ? "warmup_auto" : "warmup_manual", t1 - t0, t2 - t1, t3 - t2, t3 - t0);
					}
				}
			}
		}

		processed++;
		NKWarmupProcessed++;

		uint64_t jobUS = NKWarmup_NowUS() - jobStartUS;
		batchUS += jobUS;
		if (jobUS > maxJobUS) maxJobUS = jobUS;

		// Stage 4: always allow at least one job, then stop if this batch spent
		// its time budget. One single heavy GPU upload cannot be split here, but
		// follow-up jobs can be deferred to a later tick/loading-screen frame.
		if (budgetUS > 0 && batchUS >= budgetUS)
		{
			budgetHit = true;
			break;
		}
	}

	FImageSource::EndPrecaching();

	if (outBudgetHit != nullptr) *outBudgetHit = budgetHit;
	if (outBatchUS != nullptr) *outBatchUS = batchUS;
	if (outMaxJobUS != nullptr) *outMaxJobUS = maxJobUS;

	if (nk_warmup_debug && processed > 0)
	{
		Printf("[NKWarmup] processed %d jobs, progress=%u/%u batch=%.3fms maxjob=%.3fms budget_hit=%d\n",
			processed,
			NKWarmupCursor,
			(unsigned)NKWarmupJobs.Size(),
			NKWarmup_ToMS(batchUS),
			NKWarmup_ToMS(maxJobUS),
			(int)budgetHit);
	}
	return processed;
}

static bool NKWarmup_IsDone(void);

static int NKWarmup_FlushNowInternal(int batchSize = 64, int safetyMax = 100000, bool drainUploads = true)
{
	if (batchSize < 1) batchSize = 64;
	if (safetyMax < 1) safetyMax = 100000;

	// This is the legacy blocking finish path. It is intended for StageLoad or
	// map-travel gates, where a longer loading pause is better than gameplay
	// stutter after entering the map.
	NKWarmup_SetComplete(false);
	nk_warmup_auto = false;
	NKWarmupAutoCounter = 0;
	NKWarmup_ResetPacingRuntime(false);

	int totalProcessed = 0;
	NKWarmupManualFlushDepth++;
	while (NKWarmupCursor < NKWarmupJobs.Size() && totalProcessed < safetyMax)
	{
		int todo = batchSize;
		if (totalProcessed + todo > safetyMax) todo = safetyMax - totalProcessed;
		int processed = NKWarmup_ProcessJobs(todo, false, 0);
		if (processed <= 0)
		{
			break;
		}
		totalProcessed += processed;
	}
	NKWarmupManualFlushDepth--;

	if (drainUploads && screen != nullptr)
	{
		// Drain uploads that may already have been queued by earlier auto warmup.
		// One flush call should drain the queue, but keep a guard for future renderer
		// implementations that may only make partial progress per call.
		int drainGuard = 0;
		while (NKWarmup_UseBackgroundCache() && !NKWarmup_BackgroundCacheIdle() && drainGuard < 1024)
		{
			screen->UpdateBackgroundCache(true);
			drainGuard++;
		}
	}

	if (NKWarmup_IsDone() && NKWarmup_WorkerIdle() && NKWarmup_BackgroundCacheIdle())
	{
		NKWarmup_SetComplete(true);
	}
	return totalProcessed;
}

static void NKWarmup_Clear(void)
{
	bool keepWarmed = nk_warmup_loading_screen && nk_warmup_loading_reuse_warmed;
	NKWarmup_StopWorker();
	{
		std::lock_guard<std::mutex> lock(NKWarmupWorkerMutex);
		NKWarmupWorkerQueue.clear();
	}
	NKWarmupWorkerQueued.store(0);
	NKWarmupWorkerDone.store(0);
	NKWarmupWorkerLargeQueued.store(0);
	NKWarmupWorkerLargeDone.store(0);
	NKWarmupWorkerThreadFailed.store(false);
	NKWarmupWorkerThreadDenied.store(false);
	NKPreparedTexture_Clear();

	{
		std::lock_guard<std::mutex> lock(NKWarmupKeyMutex);
		NKWarmupJobs.Clear();
		NKWarmupQueuedKeys.Clear();
		if (!keepWarmed) NKWarmupWarmedKeys.Clear();
		NKWarmupPreparedKeys.Clear();
		NKWarmupWorkerQueuedKeys.Clear();
	}
	NKWarmupCursor = 0;
	NKWarmupProcessed = 0;
	NKWarmupSkippedInPrecache = 0;
	NKWarmupAutoDeferredLarge = 0;
	NKWarmupProfileJobs = 0;
	NKWarmupProfileSlowJobs = 0;
	NKWarmupProfileTotalUS = 0;
	NKWarmupProfileMaxUS = 0;
	NKPrecacheProfileCalls = 0;
	NKPrecacheProfileSlowCalls = 0;
	NKPrecacheProfileTotalUS = 0;
	NKPrecacheProfileMaxUS = 0;
	NKWarmup_ResetPacingRuntime(true);
	NKWarmupAddedActors = 0;
	memset(&NKWarmupLastActorDiag, 0, sizeof(NKWarmupLastActorDiag));
	NKWarmupWorkerQueueAttempts = 0;
	NKWarmupWorkerQueueSubmitted = 0;
	NKWarmupWorkerQueueSkippedDisabled = 0;
	NKWarmupWorkerQueueSkippedDuplicate = 0;
	NKWarmupWorkerQueueSkippedStartFailed = 0;
	NKWarmupWorkerQueueSkippedFull = 0;
	NKWarmupWorkerQueueSkippedDeferredLarge = 0;
	nk_warmup_auto = false;
	NKWarmupAutoCounter = 0;
	NKWarmup_SetComplete(false);
	NKWarmup_ResetLastQueueAllDiag(0, 0);
}

static bool NKWarmup_AddActorByName(const char *actorName);
static void NKWarmup_StartAuto(void);
static void NKWarmup_StopAuto(void);
static bool NKWarmup_IsDone(void);

CCMD(nk_warmup_clear)
{
	NKWarmup_Clear();
	Printf("[NKWarmup] cleared.\n");
}

CCMD(nk_warmup_start)
{
	NKWarmup_StartAuto();
	Printf("[NKWarmup] auto warmup started. step=%d interval=%d tic(s), budget=%dms loading=%d, defer_large=%d max_pixels=%d loading_allow_large=%d loading_max_pixels=%d\n",
		NKWarmup_EffectiveAutoStep(),
		NKWarmup_EffectiveAutoInterval(),
		NKWarmup_EffectiveBudgetMS(),
		(int)nk_warmup_loading_screen,
		(int)nk_warmup_auto_defer_large,
		NKWarmup_EffectiveMaxPixels(),
		(int)nk_warmup_loading_allow_large,
		(int)nk_warmup_loading_max_pixels);
}

CCMD(nk_warmup_stop)
{
	NKWarmup_StopAuto();
	Printf("[NKWarmup] auto warmup stopped.\n");
}

CCMD(nk_warmup_actor)
{
	if (argv.argc() < 2)
	{
		Printf("Usage: nk_warmup_actor <ActorClassName>\n");
		return;
	}

	PClassActor *cls = PClass::FindActor(argv[1]);
	if (cls == nullptr)
	{
		Printf("[NKWarmup] actor class not found: %s\n", argv[1]);
		return;
	}

	unsigned int before = (unsigned int)NKWarmupJobs.Size();
	if (NKWarmup_AddActorClass(cls))
	{
		Printf("[NKWarmup] queued %s: +%u jobs, total=%u\n", argv[1], (unsigned int)NKWarmupJobs.Size() - before, (unsigned int)NKWarmupJobs.Size());
		Printf("[NKWarmupDiag] actor=%s states=%u frames=%u refs=%u valid=%u invalid=%u added=%u duplicate=%u warmed_refs=%u prepared_refs=%u workerqueued_refs=%u large_refs=%u auto_defer_candidates=%u max_pixels_seen=%u effective_max_pixels=%d loading_allow_large=%d jobs_before=%u jobs_after=%u\n",
			NKWarmupLastActorDiag.Name,
			NKWarmupLastActorDiag.States,
			NKWarmupLastActorDiag.Frames,
			NKWarmupLastActorDiag.TextureRefs,
			NKWarmupLastActorDiag.ValidRefs,
			NKWarmupLastActorDiag.InvalidRefs,
			NKWarmupLastActorDiag.Added,
			NKWarmupLastActorDiag.DuplicateQueued,
			NKWarmupLastActorDiag.AlreadyWarmedRefs,
			NKWarmupLastActorDiag.AlreadyPreparedRefs,
			NKWarmupLastActorDiag.AlreadyWorkerQueuedRefs,
			NKWarmupLastActorDiag.LargeWorkerRefs,
			NKWarmupLastActorDiag.AutoDeferCandidateRefs,
			NKWarmupLastActorDiag.MaxPixelsSeen,
			NKWarmupLastActorDiag.JobsBefore,
			NKWarmupLastActorDiag.JobsAfter);
	}
}

CCMD(nk_warmup_step)
{
	int count = nk_warmup_step_default;
	if (argv.argc() >= 2)
	{
		count = atoi(argv[1]);
	}
	if (count < 1) count = 1;

	int processed = NKWarmup_ProcessJobs(count);
	if (NKWarmup_IsDone() && NKWarmup_WorkerIdle() && NKWarmup_BackgroundCacheIdle())
	{
		NKWarmup_SetComplete(true);
	}
	Printf("[NKWarmup] step processed=%d progress=%u/%u complete=%d\n", processed, NKWarmupCursor, (unsigned int)NKWarmupJobs.Size(), (int)nk_warmup_complete);
}

CCMD(nk_warmup_run)
{
	int processed = NKWarmup_FlushNowInternal(64, 100000, nk_warmup_flush_drain_uploads);
	Printf("[NKWarmup] run/flush complete processed=%d progress=%u/%u complete=%d bgcache_idle=%d\n",
		processed,
		NKWarmupCursor,
		(unsigned int)NKWarmupJobs.Size(),
		(int)nk_warmup_complete,
		(int)NKWarmup_BackgroundCacheIdle());
}

CCMD(nk_warmup_flush_now)
{
	int processed = NKWarmup_FlushNowInternal(64, 100000, nk_warmup_flush_drain_uploads);
	Printf("[NKWarmup] flush_now processed=%d progress=%u/%u complete=%d bgcache_idle=%d\n",
		processed,
		NKWarmupCursor,
		(unsigned int)NKWarmupJobs.Size(),
		(int)nk_warmup_complete,
		(int)NKWarmup_BackgroundCacheIdle());
}

CCMD(nk_warmup_stats)
{
	unsigned int remaining = NKWarmupCursor <= NKWarmupJobs.Size() ? (unsigned int)(NKWarmupJobs.Size() - NKWarmupCursor) : 0;
	Printf("[NKWarmup] actors=%u jobs=%u cursor=%u processed=%u warmed=%u prepared=%u worker_q=%d worker_done=%d worker_large=%d/%d worker_pending=%u skipped_in_precache=%u deferred_large=%u remaining=%u auto=%d complete=%d worker_idle=%d gl_precache=%d bgcache=%d step=%d interval=%d budget=%dms loading=%d pacing=%d cooldown=%d/%d batches=%u budget_hits=%u cooldowns=%u skipped_tics=%u last_jobs=%d last_batch=%.3fms last_job=%.3fms max_batch=%.3fms max_pixels=%d cpu_worker=%d worker_allowed=%d hw_threads=%d min_threads=%d max_queue=%d large_first=%d large_pixels=%d respect_defer=%d loading_allow_large=%d effective_max_pixels=%d manual_blocking=%d drain_uploads=%d failed=%d denied=%d\n",
		NKWarmupAddedActors,
		(unsigned int)NKWarmupJobs.Size(),
		NKWarmupCursor,
		NKWarmupProcessed,
		NKWarmup_WarmedCount(),
		NKWarmup_PreparedCount(),
		NKWarmupWorkerQueued.load(),
		NKWarmupWorkerDone.load(),
		NKWarmupWorkerLargeDone.load(),
		NKWarmupWorkerLargeQueued.load(),
		NKWarmup_WorkerQueuedKeyCount(),
		NKWarmupSkippedInPrecache,
		NKWarmupAutoDeferredLarge,
		remaining,
		(int)nk_warmup_auto,
		(int)nk_warmup_complete,
		(int)NKWarmup_WorkerIdle(),
		(int)gl_precache,
		(int)NKWarmup_UseBackgroundCache(),
		NKWarmup_EffectiveAutoStep(),
		NKWarmup_EffectiveAutoInterval(),
		NKWarmup_EffectiveBudgetMS(),
		(int)nk_warmup_loading_screen,
		(int)nk_warmup_pacing,
		NKWarmupPacingCooldown,
		NKWarmup_PacingCooldownTics(),
		NKWarmupPacingBatches,
		NKWarmupPacingBudgetHits,
		NKWarmupPacingCooldowns,
		NKWarmupPacingSkippedTicks,
		NKWarmupPacingLastProcessed,
		NKWarmup_ToMS(NKWarmupPacingLastBatchUS),
		NKWarmup_ToMS(NKWarmupPacingLastMaxJobUS),
		NKWarmup_ToMS(NKWarmupPacingMaxBatchUS),
		NKWarmup_EffectiveMaxPixels(),
		(int)nk_warmup_cpu_worker,
		(int)NKWarmup_CpuWorkerAllowed(),
		NKWarmup_HardwareThreads(),
		NKWarmup_MinWorkerThreads(),
		NKWarmup_WorkerQueueLimit(),
		(int)nk_warmup_cpu_worker_large_first,
		NKWarmup_LargeTexturePixels(),
		(int)nk_warmup_cpu_worker_respect_defer_large,
		(int)nk_warmup_loading_allow_large,
		NKWarmup_EffectiveMaxPixels(),
		(int)nk_warmup_manual_blocking_upload,
		(int)nk_warmup_flush_drain_uploads,
		(int)NKWarmupWorkerThreadFailed.load(),
		(int)NKWarmupWorkerThreadDenied.load());
	Printf("[NKWarmupDiag] last_actor=%s refs=%u valid=%u invalid=%u added=%u duplicate=%u warmed_refs=%u prepared_refs=%u workerqueued_refs=%u large_refs=%u auto_defer_candidates=%u max_pixels_seen=%u effective_max_pixels=%d loading_allow_large=%d jobs_before=%u jobs_after=%u\n",
		NKWarmupLastActorDiag.Name[0] ? NKWarmupLastActorDiag.Name : "<none>",
		NKWarmupLastActorDiag.TextureRefs,
		NKWarmupLastActorDiag.ValidRefs,
		NKWarmupLastActorDiag.InvalidRefs,
		NKWarmupLastActorDiag.Added,
		NKWarmupLastActorDiag.DuplicateQueued,
		NKWarmupLastActorDiag.AlreadyWarmedRefs,
		NKWarmupLastActorDiag.AlreadyPreparedRefs,
		NKWarmupLastActorDiag.AlreadyWorkerQueuedRefs,
		NKWarmupLastActorDiag.LargeWorkerRefs,
		NKWarmupLastActorDiag.AutoDeferCandidateRefs,
		NKWarmupLastActorDiag.MaxPixelsSeen,
		NKWarmup_EffectiveMaxPixels(),
		(int)nk_warmup_loading_allow_large,
		NKWarmupLastActorDiag.JobsBefore,
		NKWarmupLastActorDiag.JobsAfter);
	Printf("[NKWarmupDiag] last_queue_all cursor=%u jobs=%u considered=%u submitted=%u skipped_disabled=%u skipped_duplicate=%u skipped_startfailed=%u skipped_full=%u skipped_defer_large=%u lifetime_attempts=%u lifetime_submitted=%u lifetime_skipped(disabled=%u duplicate=%u startfailed=%u full=%u defer_large=%u)\n",
		NKWarmupLastQueueAllCursor,
		NKWarmupLastQueueAllJobs,
		NKWarmupLastQueueAllConsidered,
		NKWarmupLastQueueAllSubmitted,
		NKWarmupLastQueueAllSkippedDisabled,
		NKWarmupLastQueueAllSkippedDuplicate,
		NKWarmupLastQueueAllSkippedStartFailed,
		NKWarmupLastQueueAllSkippedFull,
		NKWarmupLastQueueAllSkippedDeferredLarge,
		NKWarmupWorkerQueueAttempts,
		NKWarmupWorkerQueueSubmitted,
		NKWarmupWorkerQueueSkippedDisabled,
		NKWarmupWorkerQueueSkippedDuplicate,
		NKWarmupWorkerQueueSkippedStartFailed,
		NKWarmupWorkerQueueSkippedFull,
		NKWarmupWorkerQueueSkippedDeferredLarge);
	Printf("[NKWarmup] profile warmup_jobs=%u slow=%u total=%.3fms max=%.3fms full_precache_calls=%u full_slow=%u full_total=%.3fms full_max=%.3fms threshold=%dms\n",
		NKWarmupProfileJobs,
		NKWarmupProfileSlowJobs,
		NKWarmup_ToMS(NKWarmupProfileTotalUS),
		NKWarmup_ToMS(NKWarmupProfileMaxUS),
		NKPrecacheProfileCalls,
		NKPrecacheProfileSlowCalls,
		NKWarmup_ToMS(NKPrecacheProfileTotalUS),
		NKWarmup_ToMS(NKPrecacheProfileMaxUS),
		NKWarmup_ProfileThresholdMS());
}

CCMD(nk_warmup_worker_status)
{
	Printf("[NKWarmupWorker] cvar=%d allowed=%d running=%d queued=%d done=%d large=%d/%d pending=%u idle=%d complete=%d hw_threads=%d min_threads=%d max_queue=%d large_first=%d large_pixels=%d respect_defer=%d loading_allow_large=%d effective_max_pixels=%d skipped_defer_large=%u failed=%d denied=%d\n",
		(int)nk_warmup_cpu_worker,
		(int)NKWarmup_CpuWorkerAllowed(),
		(int)NKWarmupWorkerRunning.load(),
		NKWarmupWorkerQueued.load(),
		NKWarmupWorkerDone.load(),
		NKWarmupWorkerLargeDone.load(),
		NKWarmupWorkerLargeQueued.load(),
		NKWarmup_WorkerQueuedKeyCount(),
		(int)NKWarmup_WorkerIdle(),
		(int)nk_warmup_complete,
		NKWarmup_HardwareThreads(),
		NKWarmup_MinWorkerThreads(),
		NKWarmup_WorkerQueueLimit(),
		(int)nk_warmup_cpu_worker_large_first,
		NKWarmup_LargeTexturePixels(),
		(int)nk_warmup_cpu_worker_respect_defer_large,
		(int)nk_warmup_loading_allow_large,
		NKWarmup_EffectiveMaxPixels(),
		NKWarmupWorkerQueueSkippedDeferredLarge,
		(int)NKWarmupWorkerThreadFailed.load(),
		(int)NKWarmupWorkerThreadDenied.load());
}

CCMD(nk_warmup_complete_status)
{
	Printf("[NKWarmupComplete] complete=%d auto=%d done=%d worker_idle=%d bgcache_idle=%d jobs=%u cursor=%u remaining=%u worker_q=%d worker_done=%d pending=%u loading=%d cooldown=%d budget_hits=%u last_batch=%.3fms last_job=%.3fms deferred_large=%u skipped_defer_large=%u loading_allow_large=%d effective_max_pixels=%d\n",
		(int)nk_warmup_complete,
		(int)nk_warmup_auto,
		(int)NKWarmup_IsDone(),
		(int)NKWarmup_WorkerIdle(),
		(int)NKWarmup_BackgroundCacheIdle(),
		(unsigned int)NKWarmupJobs.Size(),
		NKWarmupCursor,
		NKWarmupCursor <= NKWarmupJobs.Size() ? (unsigned int)(NKWarmupJobs.Size() - NKWarmupCursor) : 0,
		NKWarmupWorkerQueued.load(),
		NKWarmupWorkerDone.load(),
		NKWarmup_WorkerQueuedKeyCount(),
		(int)nk_warmup_loading_screen,
		NKWarmupPacingCooldown,
		NKWarmupPacingBudgetHits,
		NKWarmup_ToMS(NKWarmupPacingLastBatchUS),
		NKWarmup_ToMS(NKWarmupPacingLastMaxJobUS),
		NKWarmupAutoDeferredLarge,
		NKWarmupWorkerQueueSkippedDeferredLarge,
		(int)nk_warmup_loading_allow_large,
		NKWarmup_EffectiveMaxPixels());
}

CCMD(nk_warmup_loading)
{
	if (argv.argc() >= 2)
	{
		nk_warmup_loading_screen = atoi(argv[1]) != 0;
		NKWarmup_ResetPacingRuntime(false);
	}
	Printf("[NKWarmupLoading] loading=%d remaining=%u step=%d interval=%d budget=%dms complete=%d auto=%d reuse_warmed=%d warmed=%u\n",
		(int)nk_warmup_loading_screen,
		NKWarmup_RemainingJobs(),
		NKWarmup_EffectiveAutoStep(),
		NKWarmup_EffectiveAutoInterval(),
		NKWarmup_EffectiveBudgetMS(),
		(int)nk_warmup_complete,
		(int)nk_warmup_auto,
		(int)nk_warmup_loading_reuse_warmed,
		NKWarmup_WarmedCount());
}

CCMD(nk_loading_patch_status)
{
	Printf("[NKLoadingPatch] stage=5G2R active=1 acceleration=0 loading=%d remaining=%u step=%d budget=%dms reuse_warmed=%d warmed=%u\n",
		(int)nk_warmup_loading_screen,
		NKWarmup_RemainingJobs(),
		NKWarmup_EffectiveAutoStep(),
		NKWarmup_EffectiveBudgetMS(),
		(int)nk_warmup_loading_reuse_warmed,
		NKWarmup_WarmedCount());
}

CCMD(nk_warmup_pacing_status)
{
	Printf("[NKWarmupPacing] pacing=%d loading=%d step=%d interval=%d budget=%dms slow=%dms cooldown=%d/%d batches=%u budget_hits=%u cooldowns=%u skipped_ticks=%u last_jobs=%d last_batch=%.3fms last_job=%.3fms max_batch=%.3fms complete=%d auto=%d\n",
		(int)nk_warmup_pacing,
		(int)nk_warmup_loading_screen,
		NKWarmup_EffectiveAutoStep(),
		NKWarmup_EffectiveAutoInterval(),
		NKWarmup_EffectiveBudgetMS(),
		NKWarmup_PacingSlowMS(),
		NKWarmupPacingCooldown,
		NKWarmup_PacingCooldownTics(),
		NKWarmupPacingBatches,
		NKWarmupPacingBudgetHits,
		NKWarmupPacingCooldowns,
		NKWarmupPacingSkippedTicks,
		NKWarmupPacingLastProcessed,
		NKWarmup_ToMS(NKWarmupPacingLastBatchUS),
		NKWarmup_ToMS(NKWarmupPacingLastMaxJobUS),
		NKWarmup_ToMS(NKWarmupPacingMaxBatchUS),
		(int)nk_warmup_complete,
		(int)nk_warmup_auto);
}

CCMD(nk_gpu_upload_status)
{
	if (screen != nullptr)
	{
		screen->PrintBackgroundCacheStatus();
	}
	else
	{
		Printf("[NKGPUUpload] no active screen.\n");
	}
}

CCMD(nk_gpu_upload_reset_stats)
{
	if (screen != nullptr)
	{
		screen->ResetBackgroundCacheStats();
		Printf("[NKGPUUpload] stats reset.\n");
	}
	else
	{
		Printf("[NKGPUUpload] no active screen.\n");
	}
}

CCMD(nk_warmup_diag)
{
	Printf("[NKWarmupDiag] actor=%s states=%u frames=%u refs=%u valid=%u invalid=%u added=%u duplicate=%u warmed_refs=%u prepared_refs=%u workerqueued_refs=%u large_refs=%u auto_defer_candidates=%u max_pixels_seen=%u effective_max_pixels=%d loading_allow_large=%d jobs_before=%u jobs_after=%u\n",
		NKWarmupLastActorDiag.Name[0] ? NKWarmupLastActorDiag.Name : "<none>",
		NKWarmupLastActorDiag.States,
		NKWarmupLastActorDiag.Frames,
		NKWarmupLastActorDiag.TextureRefs,
		NKWarmupLastActorDiag.ValidRefs,
		NKWarmupLastActorDiag.InvalidRefs,
		NKWarmupLastActorDiag.Added,
		NKWarmupLastActorDiag.DuplicateQueued,
		NKWarmupLastActorDiag.AlreadyWarmedRefs,
		NKWarmupLastActorDiag.AlreadyPreparedRefs,
		NKWarmupLastActorDiag.AlreadyWorkerQueuedRefs,
		NKWarmupLastActorDiag.LargeWorkerRefs,
		NKWarmupLastActorDiag.AutoDeferCandidateRefs,
		NKWarmupLastActorDiag.MaxPixelsSeen,
		NKWarmup_EffectiveMaxPixels(),
		(int)nk_warmup_loading_allow_large,
		NKWarmupLastActorDiag.JobsBefore,
		NKWarmupLastActorDiag.JobsAfter);
	Printf("[NKWarmupDiag] queue cursor=%u jobs=%u remaining=%u last_considered=%u last_submitted=%u skipped(disabled=%u duplicate=%u startfailed=%u full=%u defer_large=%u) worker_pending=%u complete=%d worker_idle=%d\n",
		NKWarmupCursor,
		(unsigned int)NKWarmupJobs.Size(),
		NKWarmupCursor <= NKWarmupJobs.Size() ? (unsigned int)(NKWarmupJobs.Size() - NKWarmupCursor) : 0,
		NKWarmupLastQueueAllConsidered,
		NKWarmupLastQueueAllSubmitted,
		NKWarmupLastQueueAllSkippedDisabled,
		NKWarmupLastQueueAllSkippedDuplicate,
		NKWarmupLastQueueAllSkippedStartFailed,
		NKWarmupLastQueueAllSkippedFull,
		NKWarmupLastQueueAllSkippedDeferredLarge,
		NKWarmup_WorkerQueuedKeyCount(),
		(int)nk_warmup_complete,
		(int)NKWarmup_WorkerIdle());
	Printf("[NKWarmupDiag] worker lifetime attempts=%u submitted=%u skipped_disabled=%u skipped_duplicate=%u skipped_startfailed=%u skipped_full=%u skipped_defer_large=%u queued=%d done=%d large=%d/%d allowed=%d failed=%d denied=%d\n",
		NKWarmupWorkerQueueAttempts,
		NKWarmupWorkerQueueSubmitted,
		NKWarmupWorkerQueueSkippedDisabled,
		NKWarmupWorkerQueueSkippedDuplicate,
		NKWarmupWorkerQueueSkippedStartFailed,
		NKWarmupWorkerQueueSkippedFull,
		NKWarmupWorkerQueueSkippedDeferredLarge,
		NKWarmupWorkerQueued.load(),
		NKWarmupWorkerDone.load(),
		NKWarmupWorkerLargeDone.load(),
		NKWarmupWorkerLargeQueued.load(),
		(int)NKWarmup_CpuWorkerAllowed(),
		(int)NKWarmupWorkerThreadFailed.load(),
		(int)NKWarmupWorkerThreadDenied.load());
	if (NKWarmupLastActorDiag.Added == 0 && NKWarmupLastActorDiag.DuplicateQueued > 0)
	{
		Printf("[NKWarmupDiag] hint: +0 jobs is mostly duplicate queued refs. Use nk_warmup_clear for a full reset, or nk_warmup_forget_queue if you want to keep warmed/prepared records.\n");
	}
	if (NKWarmupLastQueueAllConsidered == 0 && NKWarmupJobs.Size() > 0)
	{
		Printf("[NKWarmupDiag] hint: worker queue-all considered 0 jobs because cursor is already at the end. Restart/clear the warmup queue before retesting worker preparation.\n");
	}
}

CCMD(nk_warmup_profile_reset)
{
	NKWarmupProfileJobs = 0;
	NKWarmupProfileSlowJobs = 0;
	NKWarmupProfileTotalUS = 0;
	NKWarmupProfileMaxUS = 0;
	NKPrecacheProfileCalls = 0;
	NKPrecacheProfileSlowCalls = 0;
	NKPrecacheProfileTotalUS = 0;
	NKPrecacheProfileMaxUS = 0;
	NKWarmup_ResetPacingRuntime(true);
	Printf("[NKWarmup] profile stats reset.\n");
}

CCMD(nk_warmup_forget_queue)
{
	NKWarmup_SetComplete(false);
	NKWarmup_StopWorker();
	{
		std::lock_guard<std::mutex> lock(NKWarmupWorkerMutex);
		NKWarmupWorkerQueue.clear();
	}
	NKWarmupWorkerQueued.store(0);
	NKWarmupWorkerDone.store(0);
	NKWarmupWorkerLargeQueued.store(0);
	NKWarmupWorkerLargeDone.store(0);
	NKPreparedTexture_Clear();

	unsigned int warmed = 0;
	unsigned int prepared = 0;
	{
		std::lock_guard<std::mutex> lock(NKWarmupKeyMutex);
		NKWarmupJobs.Clear();
		NKWarmupQueuedKeys.Clear();
		NKWarmupWorkerQueuedKeys.Clear();
		warmed = (unsigned int)NKWarmupWarmedKeys.CountUsed();
		prepared = (unsigned int)NKWarmupPreparedKeys.CountUsed();
	}
	NKWarmupCursor = 0;
	Printf("[NKWarmup] queue cleared, warmed/prepared texture records kept=%u/%u.\n",
		warmed,
		prepared);
}

static bool NKWarmup_AddActorByName(const char *actorName)
{
	if (actorName == nullptr || actorName[0] == 0)
	{
		return false;
	}

	PClassActor *cls = PClass::FindActor(actorName);
	if (cls == nullptr)
	{
		return false;
	}

	return NKWarmup_AddActorClass(cls);
}

static void NKWarmup_StartAuto(void)
{
	NKWarmup_SetComplete(false);

	if (!gl_precache)
	{
		nk_warmup_auto = false;
		NKWarmupAutoCounter = 0;
		NKWarmup_StopWorker();
		if (nk_warmup_debug)
		{
			Printf("[NKWarmup] auto warmup not started because gl_precache is off.\n");
		}
		return;
	}

	nk_warmup_auto = true;
	NKWarmupAutoCounter = 0;
	NKWarmup_ResetPacingRuntime(false);
	NKWarmup_QueueAllForWorker();
}

static void NKWarmup_StopAuto(void)
{
	NKWarmup_SetComplete(false);
	nk_warmup_auto = false;
	NKWarmupAutoCounter = 0;
	NKWarmup_ResetPacingRuntime(false);
	NKWarmup_StopWorker();
}

static bool NKWarmup_IsDone(void)
{
	return NKWarmupCursor >= NKWarmupJobs.Size();
}

DEFINE_ACTION_FUNCTION(DObject, NKWarmupActor)
{
	PARAM_PROLOGUE;
	PARAM_STRING(actorName);

	bool result = NKWarmup_AddActorByName(actorName.GetChars());
	ACTION_RETURN_BOOL(result);
}

DEFINE_ACTION_FUNCTION(DObject, NKWarmupClear)
{
	PARAM_PROLOGUE;
	NKWarmup_Clear();
	return 0;
}

DEFINE_ACTION_FUNCTION(DObject, NKWarmupStart)
{
	PARAM_PROLOGUE;
	NKWarmup_StartAuto();
	return 0;
}

DEFINE_ACTION_FUNCTION(DObject, NKWarmupStop)
{
	PARAM_PROLOGUE;
	NKWarmup_StopAuto();
	return 0;
}

DEFINE_ACTION_FUNCTION(DObject, NKWarmupLoadingScreen)
{
	PARAM_PROLOGUE;
	PARAM_BOOL(enabled);
	nk_warmup_loading_screen = enabled;
	NKWarmup_ResetPacingRuntime(false);
	ACTION_RETURN_BOOL(nk_warmup_loading_screen);
}

DEFINE_ACTION_FUNCTION(DObject, NKWarmupBurst)
{
	PARAM_PROLOGUE;
	PARAM_INT(count);

	if (count < 1) count = 1;
	int processed = NKWarmup_ProcessJobs(count);
	if (NKWarmup_IsDone() && NKWarmup_WorkerIdle() && NKWarmup_BackgroundCacheIdle())
	{
		NKWarmup_SetComplete(true);
	}
	ACTION_RETURN_INT(processed);
}

DEFINE_ACTION_FUNCTION(DObject, NKWarmupFlushNow)
{
	PARAM_PROLOGUE;
	int processed = NKWarmup_FlushNowInternal(64, 100000, nk_warmup_flush_drain_uploads);
	ACTION_RETURN_INT(processed);
}

DEFINE_ACTION_FUNCTION(DObject, NKWarmupIsDone)
{
	PARAM_PROLOGUE;
	ACTION_RETURN_BOOL(NKWarmup_IsDone());
}

DEFINE_ACTION_FUNCTION(DObject, NKWarmupJobsLeft)
{
	PARAM_PROLOGUE;
	int left = (int)(NKWarmupJobs.Size() - NKWarmupCursor);
	if (left < 0) left = 0;
	ACTION_RETURN_INT(left);
}

void NKWarmup_AutoTicker(void)
{
	if (!nk_warmup_auto)
	{
		return;
	}

	if (!gl_precache)
	{
		nk_warmup_auto = false;
		NKWarmupAutoCounter = 0;
		NKWarmup_StopWorker();
		if (nk_warmup_debug)
		{
			Printf("[NKWarmup] auto warmup stopped because gl_precache is off.\n");
		}
		return;
	}

	if (NKWarmup_UseBackgroundCache())
	{
		screen->UpdateBackgroundCache(false);
	}

	if (NKWarmupCursor >= NKWarmupJobs.Size())
	{
		// Main-thread warmup submission is done. Keep the loading-screen latch at 0
		// until the CPU prepare worker and the renderer background upload queue have
		// both drained, so scripts do not leave the loading map too early.
		if (!NKWarmup_WorkerIdle() || !NKWarmup_BackgroundCacheIdle())
		{
			return;
		}

		nk_warmup_auto = false;
		NKWarmupAutoCounter = 0;
		NKWarmup_SetComplete(true);
		NKWarmup_StopWorker();
		if (nk_warmup_debug)
		{
			Printf("[NKWarmup] auto warmup completed. jobs=%u warmed=%u prepared=%u deferred_large=%u worker_done=%d skipped_defer_large=%u complete=%d\n",
				(unsigned int)NKWarmupJobs.Size(),
				(unsigned int)NKWarmupWarmedKeys.CountUsed(),
				(unsigned int)NKWarmupPreparedKeys.CountUsed(),
				NKWarmupAutoDeferredLarge,
				NKWarmupWorkerDone.load(),
				NKWarmupWorkerQueueSkippedDeferredLarge,
				(int)nk_warmup_complete);
		}
		return;
	}

	if (NKWarmupPacingCooldown > 0)
	{
		NKWarmupPacingCooldown--;
		NKWarmupPacingSkippedTicks++;
		return;
	}

	int interval = NKWarmup_EffectiveAutoInterval();

	NKWarmupAutoCounter++;
	if (NKWarmupAutoCounter < interval)
	{
		return;
	}
	NKWarmupAutoCounter = 0;

	int step = NKWarmup_EffectiveAutoStep();
	int budgetMS = NKWarmup_EffectiveBudgetMS();
	bool budgetHit = false;
	uint64_t batchUS = 0;
	uint64_t maxJobUS = 0;
	int processed = NKWarmup_ProcessJobs(step, true, budgetMS, &budgetHit, &batchUS, &maxJobUS);

	NKWarmupPacingLastProcessed = processed;
	NKWarmupPacingLastStep = step;
	NKWarmupPacingLastInterval = interval;
	NKWarmupPacingLastBudgetMS = budgetMS;
	NKWarmupPacingLastBatchUS = batchUS;
	NKWarmupPacingLastMaxJobUS = maxJobUS;
	if (batchUS > NKWarmupPacingMaxBatchUS) NKWarmupPacingMaxBatchUS = batchUS;
	if (processed > 0) NKWarmupPacingBatches++;

	if (budgetHit)
	{
		NKWarmupPacingBudgetHits++;
	}

	if (nk_warmup_pacing && processed > 0)
	{
		int cooldown = NKWarmup_PacingCooldownTics();
		int slowMS = NKWarmup_PacingSlowMS();
		bool slowBatch = slowMS > 0 && maxJobUS >= (uint64_t)slowMS * 1000ull;
		if (cooldown > 0 && (budgetHit || slowBatch))
		{
			NKWarmupPacingCooldown = cooldown;
			NKWarmupPacingCooldowns++;
		}
	}
}


void NKWarmup_OnLevelStart(void)
{
	// New map/loading-screen entry must always start with the public latch at 0.
	// Do not clear NKWarmupJobs or NKPreparedTexture here: a loading-screen map
	// may have just prepared buffers for the destination map, and those buffers
	// should remain available for the first real render of that map.
	NKWarmup_SetComplete(false);
	nk_warmup_auto = false;
	nk_warmup_loading_screen = false;
	NKWarmupAutoCounter = 0;
	NKWarmup_ResetPacingRuntime(false);
}


//==========================================================================
//
// DFrameBuffer :: PrecacheTexture
//
//==========================================================================

static void PrecacheTexture(FGameTexture *tex, int cache)
{
	if (cache & (FTextureManager::HIT_Wall | FTextureManager::HIT_Flat | FTextureManager::HIT_Sky))
	{
		int scaleflags = 0;
		if (shouldUpscale(tex, UF_Texture)) scaleflags |= CTF_Upscale;

		uint64_t t0 = NKWarmup_NowUS();
		FMaterial * gltex = FMaterial::ValidateTexture(tex, scaleflags);
		if (gltex)
		{
			if (NKWarmup_UseBackgroundCache()) screen->BackgroundCacheMaterial(gltex, FTranslationID::fromInt(0), false, false);
			else screen->PrecacheMaterial(gltex, 0);
		}
		uint64_t t1 = NKWarmup_NowUS();
		NKWarmup_RecordPrecacheProfile(tex, 0, "full_texture", t1 - t0);
	}
}

//===========================================================================
//
//
//
//===========================================================================
static void PrecacheList(FGameTexture *tex, FMaterial *gltex, SpriteHits& translations)
{
	SpriteHits::Iterator it(translations);
	SpriteHits::Pair* pair;
	while (it.NextPair(pair))
	{
		if (NKWarmup_IsWarmed(tex->GetID(), pair->Key))
		{
			NKWarmupSkippedInPrecache++;
			continue;
		}
		uint64_t t0 = NKWarmup_NowUS();
		if (NKWarmup_UseBackgroundCache()) screen->PrequeueMaterial(gltex, pair->Key);
		else screen->PrecacheMaterial(gltex, pair->Key);
		uint64_t t1 = NKWarmup_NowUS();
		NKWarmup_RecordPrecacheProfile(tex, pair->Key, "full_sprite", t1 - t0);

		// A normal/full precache has now done or submitted the same expensive work,
		// so sync the warmup queue state with it.
		NKWarmup_MarkWarmed(tex->GetID(), pair->Key);
	}
}

//==========================================================================
//
// DFrameBuffer :: PrecacheSprite
//
//==========================================================================

static void PrecacheSprite(FGameTexture *tex, SpriteHits &hits)
{
	int scaleflags = CTF_Expand;
	if (shouldUpscale(tex, UF_Sprite)) scaleflags |= CTF_Upscale;

	FMaterial * gltex = FMaterial::ValidateTexture(tex, scaleflags);
	if (gltex) PrecacheList(tex, gltex, hits);
}


//==========================================================================
//
// DFrameBuffer :: Precache
//
//==========================================================================

void hw_PrecacheTexture(uint8_t *texhitlist, TMap<PClassActor*, bool> &actorhitlist)
{
	TMap<FTexture*, bool> allTextures;
	TArray<FTexture*> layers;

	// First collect the potential max. texture set 
	for (int i = 1; i < TexMan.NumTextures(); i++)
	{
		auto gametex = TexMan.GameByIndex(i);
		if (gametex && gametex->isValid() &&
			gametex->GetTexture()->GetImage() &&	// only image textures are subject to precaching
			gametex->GetUseType() != ETextureType::FontChar &&	// We do not want to delete font characters here as they are very likely to be needed constantly.
			gametex->GetUseType() < ETextureType::Special)		// Any texture marked as 'special' is also out.
		{
			gametex->GetLayers(layers);
			for (auto layer : layers)
			{
				allTextures.Insert(layer, true);
				layer->CleanPrecacheMarker();
			}
		}

		// Mark the faces of a skybox as used.
		// This isn't done by the main code so it needs to be done here.
		// MBF sky transfers are being checked by the calling code to add HIT_Sky for them.
		if (texhitlist[i] & (FTextureManager::HIT_Sky))
		{
			auto tex = TexMan.GameByIndex(i);
			auto sb = dynamic_cast<FSkyBox*>(tex->GetTexture());
			if (sb)
			{
				for (int i = 0; i < 6; i++)
				{
					if (sb->faces[i])
					{
						int index = sb->faces[i]->GetID().GetIndex();
						texhitlist[index] |= FTextureManager::HIT_Flat;
					}
				}
			}
		}
	}

	SpriteHits *spritelist = new SpriteHits[sprites.Size()];
	SpriteHits **spritehitlist = new SpriteHits*[TexMan.NumTextures()];
	TMap<PClassActor*, bool>::Iterator it(actorhitlist);
	TMap<PClassActor*, bool>::Pair *pair;
	uint8_t *modellist = new uint8_t[Models.Size()];
	memset(modellist, 0, Models.Size());
	memset(spritehitlist, 0, sizeof(SpriteHits**) * TexMan.NumTextures());

	// Check all used actors.
	// 1. mark all sprites associated with its states
	// 2. mark all model data and skins associated with its states
	while (it.NextPair(pair))
	{
		PClassActor *cls = pair->Key;
		auto remap = GPalette.TranslationToTable(GetDefaultByType(cls)->Translation.index());
		int gltrans = remap == nullptr ? 0 : remap->Index;

		for (unsigned i = 0; i < cls->GetStateCount(); i++)
		{
			auto &state = cls->GetStates()[i];
			spritelist[state.sprite].Insert(gltrans, true);
			FSpriteModelFrame * smf = FindModelFrame(cls, state.sprite, state.Frame, false);
			if (smf != NULL)
			{
				for (int i = 0; i < smf->modelsAmount; i++)
				{
					if (smf->skinIDs[i].isValid())
					{
						texhitlist[smf->skinIDs[i].GetIndex()] |= FTextureManager::HIT_Flat;
					}
					else if (smf->modelIDs[i] != -1)
					{
						Models[smf->modelIDs[i]]->AddSkins(texhitlist, (unsigned)(i * MD3_MAX_SURFACES) < smf->surfaceskinIDs.Size()? &smf->surfaceskinIDs[i * MD3_MAX_SURFACES] : nullptr);
					}
					if (smf->modelIDs[i] != -1)
					{
						modellist[smf->modelIDs[i]] = 1;
					}
				}
			}
		}
	}

	// mark all sprite textures belonging to the marked sprites.
	for (int i = (int)(sprites.Size() - 1); i >= 0; i--)
	{
		if (spritelist[i].CountUsed())
		{
			int j, k;
			for (j = 0; j < sprites[i].numframes; j++)
			{
				const spriteframe_t *frame = &SpriteFrames[sprites[i].spriteframes + j];

				for (k = 0; k < 16; k++)
				{
					FTextureID pic = frame->Texture[k];
					if (pic.isValid())
					{
						spritehitlist[pic.GetIndex()] = &spritelist[i];
					}
				}
			}
		}
	}

	// delete everything unused before creating any new resources to avoid memory usage peaks.

	// delete unused models
	for (unsigned i = 0; i < Models.Size(); i++)
	{
		if (!modellist[i]) Models[i]->DestroyVertexBuffer();
	}

	TMap<FTexture *, bool> usedTextures, usedSprites;

	screen->StartPrecaching();
	int cnt = TexMan.NumTextures();

	// prepare the textures for precaching. First mark all used layer textures so that we know which ones should not be deleted.
	for (int i = cnt - 1; i >= 0; i--)
	{
		auto tex = TexMan.GameByIndex(i);
		if (tex != nullptr)
		{
			if (texhitlist[i] & (FTextureManager::HIT_Wall | FTextureManager::HIT_Flat | FTextureManager::HIT_Sky))
			{
				int scaleflags = 0;
				if (shouldUpscale(tex, UF_Texture)) scaleflags |= CTF_Upscale;

				FMaterial* mat = FMaterial::ValidateTexture(tex, scaleflags, true);
				if (mat != nullptr)
				{
					for (auto &layer : mat->GetLayerArray())
					{
						if (layer.layerTexture) layer.layerTexture->MarkForPrecache(0, layer.scaleFlags);
					}
				}
			}
			if (spritehitlist[i] != nullptr && (*spritehitlist[i]).CountUsed() > 0)
			{
				int scaleflags = CTF_Expand;
				if (shouldUpscale(tex, UF_Sprite)) scaleflags |= CTF_Upscale;

				FMaterial *mat = FMaterial::ValidateTexture(tex, true, true);
				if (mat != nullptr)
				{
					SpriteHits::Iterator it(*spritehitlist[i]);
					SpriteHits::Pair* pair;
					while (it.NextPair(pair))
					{
						for (auto& layer : mat->GetLayerArray())
						{
							if (layer.layerTexture) layer.layerTexture->MarkForPrecache(pair->Key, layer.scaleFlags);
						}
					}
				}
			}
		}
	}

	// delete unused hardware textures (i.e. those which didn't get referenced by any material in the cache list.)
	decltype(allTextures)::Iterator ita(allTextures);
	decltype(allTextures)::Pair* paira;
	while (ita.NextPair(paira))
	{
		paira->Key->CleanUnused();
	}

	if (gl_precache)
	{
		cycle_t precache;
		precache.Reset();
		precache.Clock();

		FImageSource::BeginPrecaching();

		// cache all used images
		for (int i = cnt - 1; i >= 0; i--)
		{
			auto gtex = TexMan.GameByIndex(i);
			auto tex = gtex->GetTexture();
			if (tex != nullptr && tex->GetImage() != nullptr)
			{
				if (texhitlist[i] & (FTextureManager::HIT_Wall | FTextureManager::HIT_Flat | FTextureManager::HIT_Sky))
				{
					int flags = shouldUpscale(gtex, UF_Texture);
					if (tex->GetImage() && tex->GetHardwareTexture(0, flags) == nullptr)
					{
						FImageSource::RegisterForPrecache(tex->GetImage(), V_IsTrueColor());
					}
				}

				// Only register untranslated sprite images. Translated ones are very unlikely to require data that can be reused so they can just be created on demand.
				if (spritehitlist[i] != nullptr && (*spritehitlist[i]).CheckKey(0))
				{
					FImageSource::RegisterForPrecache(tex->GetImage(), V_IsTrueColor());
				}
			}
		}

		// cache all used textures
		for (int i = cnt - 1; i >= 0; i--)
		{
			auto gtex = TexMan.GameByIndex(i);
			if (gtex != nullptr)
			{
				PrecacheTexture(gtex, texhitlist[i]);
				if (spritehitlist[i] != nullptr && (*spritehitlist[i]).CountUsed() > 0)
				{
					PrecacheSprite(gtex, *spritehitlist[i]);
				}
			}
		}

		// If the OpenGL background-cache queue is active during a normal/full
		// precache pass, flush it before EndPrecaching so the registered image
		// sources can still be reused by the normal texture creation path.
		if (NKWarmup_UseBackgroundCache())
		{
			screen->UpdateBackgroundCache(true);
		}

		FImageSource::EndPrecaching();

		// cache all used models
		FModelRenderer* renderer = new FHWModelRenderer(nullptr, *screen->RenderState(), -1);
		for (unsigned i = 0; i < Models.Size(); i++)
		{
			if (modellist[i]) 
				Models[i]->BuildVertexBuffer(renderer);
		}
		delete renderer;

		precache.Unclock();
		DPrintf(DMSG_NOTIFY, "Textures precached in %.3f ms\n", precache.TimeMS());
	}

	NKWarmup_AdvanceCursorPastWarmed();

	delete[] spritehitlist;
	delete[] spritelist;
	delete[] modellist;
}

