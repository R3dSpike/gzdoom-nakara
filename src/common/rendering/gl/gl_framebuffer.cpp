/*
** gl_framebuffer.cpp
** Implementation of the non-hardware specific parts of the
** OpenGL frame buffer
**
**---------------------------------------------------------------------------
** Copyright 2010-2020 Christoph Oelckers
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
*/ 

#include "gl_system.h"
#include "v_video.h"
#include "m_png.h"

#include "i_time.h"

#include "gl_interface.h"
#include "gl_framebuffer.h"
#include "gl_renderer.h"
#include "gl_renderbuffers.h"
#include "gl_samplers.h"
#include "hw_clock.h"
#include "hw_vrmodes.h"
#include "hw_skydome.h"
#include "hw_viewpointbuffer.h"
#include "hw_lightbuffer.h"
#include "hw_bonebuffer.h"
#include "gl_shaderprogram.h"
#include "gl_debug.h"
#include "r_videoscale.h"
#include "gl_buffers.h"
#include "gl_postprocessstate.h"
#include "v_draw.h"
#include "printf.h"
#include "gl_hwtexture.h"

#include "flatvertices.h"
#include "hw_cvars.h"

EXTERN_CVAR (Bool, vid_vsync)
EXTERN_CVAR(Int, gl_tonemap)
EXTERN_CVAR(Bool, cl_capfps)
EXTERN_CVAR(Int, gl_pipeline_depth);
EXTERN_CVAR(Bool, gl_texture_thread)
EXTERN_CVAR(Bool, gl_texture_thread_upload)
EXTERN_CVAR(Int, gl_background_flush_count)
EXTERN_CVAR(Bool, gl_background_auto_throttle)
EXTERN_CVAR(Int, gl_background_throttle_slow_ms)
EXTERN_CVAR(Int, gl_background_throttle_frames)
EXTERN_CVAR(Bool, nk_gpu_upload_pacing)
EXTERN_CVAR(Int, nk_gpu_upload_max_per_frame)
EXTERN_CVAR(Int, nk_gpu_upload_budget_ms)
EXTERN_CVAR(Int, nk_gpu_upload_slow_ms)
EXTERN_CVAR(Int, nk_gpu_upload_cooldown_frames)
EXTERN_CVAR(Int, nk_gpu_upload_loading_max_per_frame)
EXTERN_CVAR(Int, nk_gpu_upload_loading_budget_ms)
EXTERN_CVAR(Bool, nk_gpu_upload_debug)
EXTERN_CVAR(Bool, nk_warmup_loading_screen)
EXTERN_CVAR(Bool, nk_warmup_debug)

void gl_LoadExtensions();
void gl_PrintStartupLog();

extern bool vid_hdr_active;

namespace OpenGLRenderer
{
	FGLRenderer *GLRenderer;

//==========================================================================
//
//
//
//==========================================================================

OpenGLFrameBuffer::OpenGLFrameBuffer(void *hMonitor, bool fullscreen) : 
	Super(hMonitor, fullscreen) 
{
	// SetVSync needs to be at the very top to workaround a bug in Nvidia's OpenGL driver.
	// If wglSwapIntervalEXT is called after glBindFramebuffer in a frame the setting is not changed!
	Super::SetVSync(vid_vsync);
	FHardwareTexture::InitGlobalState();

	// Make sure all global variables tracking OpenGL context state are reset..
	gl_RenderState.Reset();

	GLRenderer = nullptr;
}

OpenGLFrameBuffer::~OpenGLFrameBuffer()
{
	StopBackgroundCache();
	PPResource::ResetAll();

	if (mVertexData != nullptr) delete mVertexData;
	if (mSkyData != nullptr) delete mSkyData;
	if (mViewpoints != nullptr) delete mViewpoints;
	if (mLights != nullptr) delete mLights;
	if (mBones != nullptr) delete mBones;
	mShadowMap.Reset();

	if (GLRenderer)
	{
		delete GLRenderer;
		GLRenderer = nullptr;
	}
}

//==========================================================================
//
// Initializes the GL renderer
//
//==========================================================================

void OpenGLFrameBuffer::InitializeState()
{
	static bool first=true;

	if (first)
	{
		if (ogl_LoadFunctions() == ogl_LOAD_FAILED)
		{
			I_FatalError("Failed to load OpenGL functions.");
		}
	}

	gl_LoadExtensions();

	mPipelineNbr = clamp(*gl_pipeline_depth, 1, HW_MAX_PIPELINE_BUFFERS);
	mPipelineType = gl_pipeline_depth > 0;

	// Move some state to the framebuffer object for easier access.
	hwcaps = gl.flags;
	glslversion = gl.glslversion;
	uniformblockalignment = gl.uniformblockalignment;
	maxuniformblock = gl.maxuniformblock;
	vendorstring = gl.vendorstring;

	if (first)
	{
		first=false;
		gl_PrintStartupLog();
	}

	glDepthFunc(GL_LESS);

	glEnable(GL_DITHER);
	glDisable(GL_CULL_FACE);
	glDisable(GL_POLYGON_OFFSET_FILL);
	glEnable(GL_POLYGON_OFFSET_LINE);
	glEnable(GL_BLEND);
	glEnable(GL_DEPTH_CLAMP);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_LINE_SMOOTH);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClearDepth(1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	SetViewportRects(nullptr);

	mVertexData = new FFlatVertexBuffer(GetWidth(), GetHeight(), screen->mPipelineNbr);
	mSkyData = new FSkyVertexBuffer;
	mViewpoints = new HWViewpointBuffer(screen->mPipelineNbr);
	mLights = new FLightBuffer(screen->mPipelineNbr);
	mBones = new BoneBuffer(screen->mPipelineNbr);
	GLRenderer = new FGLRenderer(this);
	GLRenderer->Initialize(GetWidth(), GetHeight());
	static_cast<GLDataBuffer*>(mLights->GetBuffer())->BindBase();
	static_cast<GLDataBuffer*>(mBones->GetBuffer())->BindBase();

	mDebug = std::make_unique<FGLDebug>();
	mDebug->Update();
}

//==========================================================================
//
// Updates the screen
//
//==========================================================================

void OpenGLFrameBuffer::Update()
{
	twoD.Reset();
	Flush3D.Reset();

	Flush3D.Clock();
	GLRenderer->Flush();
	Flush3D.Unclock();

	Swap();
	Super::Update();
}

void OpenGLFrameBuffer::CopyScreenToBuffer(int width, int height, uint8_t* scr)
{
	IntRect bounds;
	bounds.left = 0;
	bounds.top = 0;
	bounds.width = width;
	bounds.height = height;
	GLRenderer->CopyToBackbuffer(&bounds, false);

	// strictly speaking not needed as the glReadPixels should block until the scene is rendered, but this is to safeguard against shitty drivers
	glFinish();
	glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, scr);
}

//===========================================================================
//
// Camera texture rendering
//
//===========================================================================

void OpenGLFrameBuffer::RenderTextureView(FCanvasTexture* tex, std::function<void(IntRect &)> renderFunc)
{
	GLRenderer->StartOffscreen();
	GLRenderer->BindToFrameBuffer(tex);

	IntRect bounds;
	bounds.left = bounds.top = 0;
	bounds.width = FHardwareTexture::GetTexDimension(tex->GetWidth());
	bounds.height = FHardwareTexture::GetTexDimension(tex->GetHeight());

	renderFunc(bounds);
	GLRenderer->EndOffscreen();

	tex->SetUpdated(true);
	static_cast<OpenGLFrameBuffer*>(screen)->camtexcount++;
}

//===========================================================================
//
// 
//
//===========================================================================

const char* OpenGLFrameBuffer::DeviceName() const 
{
	return gl.modelstring;
}

//==========================================================================
//
// Swap the buffers
//
//==========================================================================

CVAR(Bool, gl_finishbeforeswap, false, CVAR_ARCHIVE|CVAR_GLOBALCONFIG);

void OpenGLFrameBuffer::Swap()
{
	bool swapbefore = gl_finishbeforeswap && camtexcount == 0;
	Finish.Reset();
	Finish.Clock();
	if (gl_pipeline_depth < 1)
	{
		if (swapbefore) glFinish();
		FPSLimit();
		SwapBuffers();
		if (!swapbefore) glFinish();
	}
	else
	{
		mVertexData->DropSync();

		FPSLimit();
		SwapBuffers();

		mVertexData->NextPipelineBuffer();
		mVertexData->WaitSync();

		RenderState()->SetVertexBuffer(screen->mVertexData); // Needed for Raze because it does not reset it
	}
	Finish.Unclock();
	camtexcount = 0;
	FHardwareTexture::UnbindAll();
	gl_RenderState.ClearLastMaterial();
	mDebug->Update();
}

//==========================================================================
//
// Enable/disable vertical sync
//
//==========================================================================

void OpenGLFrameBuffer::SetVSync(bool vsync)
{
	// Switch to the default frame buffer because some drivers associate the vsync state with the bound FB object.
	GLint oldDrawFramebufferBinding = 0, oldReadFramebufferBinding = 0;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &oldDrawFramebufferBinding);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldReadFramebufferBinding);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

	Super::SetVSync(vsync);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, oldDrawFramebufferBinding);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, oldReadFramebufferBinding);
}

//===========================================================================
//
//
//===========================================================================

void OpenGLFrameBuffer::SetTextureFilterMode()
{
	if (GLRenderer != nullptr && GLRenderer->mSamplerManager != nullptr) GLRenderer->mSamplerManager->SetTextureFilterMode();
}

IHardwareTexture *OpenGLFrameBuffer::CreateHardwareTexture(int numchannels) 
{ 
	return new FHardwareTexture(numchannels);
}

void OpenGLFrameBuffer::PrecacheMaterial(FMaterial *mat, int translation)
{
	if (mat->Source()->GetUseType() == ETextureType::SWCanvas) return;

	int numLayers = mat->NumLayers();
	MaterialLayerInfo* layer;
	auto base = static_cast<FHardwareTexture*>(mat->GetLayer(0, translation, &layer));

	if (base->BindOrCreate(layer->layerTexture, 0, CLAMP_NONE, translation, layer->scaleFlags))
	{
		for (int i = 1; i < numLayers; i++)
		{
			auto systex = static_cast<FHardwareTexture*>(mat->GetLayer(i, 0, &layer));
			systex->BindOrCreate(layer->layerTexture, i, CLAMP_NONE, 0, layer->scaleFlags);
		}
	}
	// unbind everything. 
	FHardwareTexture::UnbindAll();
	gl_RenderState.ClearLastMaterial();
}

//===========================================================================
//
// NKS background cache stage 1.
// This is intentionally conservative: it queues OpenGL material precache jobs
// and drains them through the normal main-thread PrecacheMaterial path.
// It does not create an auxiliary GL context yet. The goal is to verify the
// queue/state path before experimenting with real background GPU uploads.
//
//===========================================================================

static bool NKGPUUploadQueueEnabled()
{
	return gl_texture_thread || nk_gpu_upload_pacing;
}

bool OpenGLFrameBuffer::SupportsBackgroundCache()
{
	return NKGPUUploadQueueEnabled();
}

bool OpenGLFrameBuffer::CachingActive()
{
	return NKBGCachedMaterials.Size() > 0;
}

float OpenGLFrameBuffer::CacheProgress()
{
	unsigned int total = NKBGSubmitted;
	if (total == 0) return 1.f;
	float progress = float(NKBGConsumed) / float(total);
	if (progress < 0.f) progress = 0.f;
	if (progress > 1.f) progress = 1.f;
	return progress;
}

bool OpenGLFrameBuffer::NKBGQueueMaterial(FMaterial *mat, int translation, bool makeSPI, bool secondary)
{
	if (mat == nullptr) return false;
	if (mat->Source()->GetUseType() == ETextureType::SWCanvas) return false;

	for (unsigned int i = 0; i < NKBGCachedMaterials.Size(); i++)
	{
		if (NKBGCachedMaterials[i].Material == mat && NKBGCachedMaterials[i].Translation == translation)
		{
			NKBGCollisions++;
			return true;
		}
	}

	NKGLQueuedMaterial job;
	job.Material = mat;
	job.Translation = translation;
	job.MakeSPI = makeSPI;
	job.Secondary = secondary;
	NKBGCachedMaterials.Push(job);
	NKBGSubmitted++;

	if (nk_warmup_debug || nk_gpu_upload_debug)
	{
		Printf("[NKBGCache] queued mat=%p translation=%d queue=%u upload_thread=%d pacing=%d\n",
			mat, translation, (unsigned int)NKBGCachedMaterials.Size(), (int)gl_texture_thread_upload, (int)nk_gpu_upload_pacing);
	}
	return true;
}

void OpenGLFrameBuffer::PrequeueMaterial(FMaterial *mat, int translation)
{
	BackgroundCacheMaterial(mat, FTranslationID::fromInt(translation), true, true);
}

bool OpenGLFrameBuffer::BackgroundCacheMaterial(FMaterial *mat, FTranslationID translation, bool makeSPI, bool secondary)
{
	if (!NKGPUUploadQueueEnabled())
	{
		PrecacheMaterial(mat, translation.index());
		return true;
	}
	return NKBGQueueMaterial(mat, translation.index(), makeSPI, secondary);
}

bool OpenGLFrameBuffer::BackgroundCacheTextureMaterial(FGameTexture *tex, FTranslationID translation, int scaleFlags, bool makeSPI)
{
	if (tex == nullptr) return false;

	FMaterial *mat = FMaterial::ValidateTexture(tex, scaleFlags, true);
	if (mat == nullptr) return false;

	return BackgroundCacheMaterial(mat, translation, makeSPI, false);
}

int OpenGLFrameBuffer::NKBGEffectiveLimit(bool flush) const
{
	if (flush) return 0;
	int limit = nk_gpu_upload_pacing ?
		(nk_warmup_loading_screen ? nk_gpu_upload_loading_max_per_frame : nk_gpu_upload_max_per_frame) :
		gl_background_flush_count;
	if (limit < 1) limit = 1;
	return limit;
}

int OpenGLFrameBuffer::NKBGEffectiveBudgetMS(bool flush) const
{
	if (flush || !nk_gpu_upload_pacing) return 0;
	int budget = nk_warmup_loading_screen ? nk_gpu_upload_loading_budget_ms : nk_gpu_upload_budget_ms;
	if (budget < 0) budget = 0;
	return budget;
}

int OpenGLFrameBuffer::NKBGEffectiveSlowMS() const
{
	int slow = nk_gpu_upload_pacing ? nk_gpu_upload_slow_ms : gl_background_throttle_slow_ms;
	if (slow < 0) slow = 0;
	return slow;
}

int OpenGLFrameBuffer::NKBGEffectiveCooldownFrames() const
{
	int cooldown = nk_gpu_upload_pacing ? nk_gpu_upload_cooldown_frames : gl_background_throttle_frames;
	if (cooldown < 0) cooldown = 0;
	return cooldown;
}

void OpenGLFrameBuffer::UpdateBackgroundCache(bool flush)
{
	if (!NKGPUUploadQueueEnabled() && !flush) return;

	NKBGLastProcessed = 0;
	NKBGLastBatchUS = 0;
	NKBGLastMaxUS = 0;
	NKBGLastBudgetHit = false;
	NKBGLastLimit = flush ? -1 : NKBGEffectiveLimit(false);
	NKBGLastBudgetMS = NKBGEffectiveBudgetMS(flush);

	// Stage 5A: this still uses the normal main/render-thread PrecacheMaterial
	// path. The only change is stricter pacing and profiling for the queued GPU
	// upload work, with a faster budget while a NKS loading screen is active.
	if (!flush && gl_background_auto_throttle && NKBGThrottleFramesLeft > 0)
	{
		NKBGThrottleFramesLeft--;
		NKBGDeferredByCooldown++;
		return;
	}

	int limit = flush ? 0 : NKBGLastLimit;
	uint64_t budgetUS = (uint64_t)NKBGLastBudgetMS * 1000ull;
	int slowMS = NKBGEffectiveSlowMS();
	uint64_t slowUS = (uint64_t)slowMS * 1000ull;
	int cooldownFrames = NKBGEffectiveCooldownFrames();

	int processed = 0;
	uint64_t batchUS = 0;
	uint64_t batchMaxUS = 0;
	bool slowUpload = false;
	bool budgetHit = false;

	if (flush)
	{
		NKBGFlushes++;
	}

	while (NKBGCachedMaterials.Size() > 0 && (flush || processed < limit))
	{
		NKGLQueuedMaterial job = NKBGCachedMaterials[0];
		NKBGCachedMaterials.Delete(0);

		if (job.Material != nullptr)
		{
			uint64_t startNS = I_nsTime();
			PrecacheMaterial(job.Material, job.Translation);
			uint64_t elapsedUS = (I_nsTime() - startNS) / 1000ull;

			NKBGUploadTotalUS += elapsedUS;
			if (elapsedUS > NKBGUploadMaxUS) NKBGUploadMaxUS = elapsedUS;
			batchUS += elapsedUS;
			if (elapsedUS > batchMaxUS) batchMaxUS = elapsedUS;

			if (slowUS > 0 && elapsedUS >= slowUS)
			{
				NKBGSlowUploads++;
				slowUpload = true;
			}

			NKBGConsumed++;
			processed++;

			// Always allow at least one upload. After that, stop if this frame's
			// upload budget is spent. One single GL upload cannot be split here.
			if (!flush && budgetUS > 0 && batchUS >= budgetUS)
			{
				budgetHit = true;
				break;
			}

			if (!flush && gl_background_auto_throttle && slowUpload)
			{
				break;
			}
		}
	}

	NKBGLastProcessed = processed;
	NKBGLastBatchUS = batchUS;
	NKBGLastMaxUS = batchMaxUS;
	NKBGLastBudgetHit = budgetHit;
	if (processed > 0)
	{
		NKBGFramesWithUploads++;
		NKBGProcessedBatches++;
		if (slowUpload)
		{
			NKBGSlowBatches++;
		}
		if (budgetHit)
		{
			NKBGBudgetHits++;
		}
		if (batchUS > NKBGPeakBatchUS)
		{
			NKBGPeakBatchUS = batchUS;
			NKBGPeakBatchJobs = processed;
			NKBGPeakAtSubmitted = NKBGSubmitted;
			NKBGPeakAtConsumed = NKBGConsumed;
		}
	}

	if (!flush && NKBGCachedMaterials.Size() > 0)
	{
		if (budgetHit)
		{
			NKBGDeferredByBudget++;
		}
		if (gl_background_auto_throttle && (budgetHit || slowUpload) && cooldownFrames > 0)
		{
			NKBGThrottleFramesLeft = cooldownFrames;
		}
	}

	if ((nk_warmup_debug || nk_gpu_upload_debug) && processed > 0)
	{
		Printf("[NKGPUUpload/GL] processed=%d remaining=%u submitted=%u consumed=%u flush=%d loading=%d limit=%d budget=%dms batch=%.3fms max=%.3fms slow=%u budget_hit=%d cooldown=%d\n",
			processed,
			(unsigned int)NKBGCachedMaterials.Size(),
			NKBGSubmitted,
			NKBGConsumed,
			(int)flush,
			(int)nk_warmup_loading_screen,
			NKBGLastLimit,
			NKBGLastBudgetMS,
			double(batchUS) / 1000.0,
			double(batchMaxUS) / 1000.0,
			NKBGSlowUploads,
			(int)budgetHit,
			NKBGThrottleFramesLeft);
	}
}

void OpenGLFrameBuffer::PrintBackgroundCacheStatus()
{
	bool active = NKBGCachedMaterials.Size() > 0 || NKBGThrottleFramesLeft > 0 || NKBGLastProcessed > 0;
	Printf("[NKGPUUpload/GL] enabled=%d pacing=%d loading=%d active=%d queue=%u submitted=%u consumed=%u collisions=%u upload_thread=%d limit=%d budget=%dms slow_ms=%d cooldown=%d/%d upload_frames=%u batches=%u slow_jobs=%u slow_batches=%u budget_hits=%u def_budget=%u def_cooldown=%u flushes=%u total=%.3fms peak_job=%.3fms peak_batch=%.3fms peak_batch_jobs=%d peak_at=%u/%u last_jobs=%d last_batch=%.3fms last_job=%.3fms last_budget_hit=%d\n",
		(int)NKGPUUploadQueueEnabled(),
		(int)nk_gpu_upload_pacing,
		(int)nk_warmup_loading_screen,
		(int)active,
		(unsigned int)NKBGCachedMaterials.Size(),
		NKBGSubmitted,
		NKBGConsumed,
		NKBGCollisions,
		(int)gl_texture_thread_upload,
		NKBGEffectiveLimit(false),
		NKBGEffectiveBudgetMS(false),
		NKBGEffectiveSlowMS(),
		NKBGThrottleFramesLeft,
		NKBGEffectiveCooldownFrames(),
		NKBGFramesWithUploads,
		NKBGProcessedBatches,
		NKBGSlowUploads,
		NKBGSlowBatches,
		NKBGBudgetHits,
		NKBGDeferredByBudget,
		NKBGDeferredByCooldown,
		NKBGFlushes,
		double(NKBGUploadTotalUS) / 1000.0,
		double(NKBGUploadMaxUS) / 1000.0,
		double(NKBGPeakBatchUS) / 1000.0,
		NKBGPeakBatchJobs,
		NKBGPeakAtSubmitted,
		NKBGPeakAtConsumed,
		NKBGLastProcessed,
		double(NKBGLastBatchUS) / 1000.0,
		double(NKBGLastMaxUS) / 1000.0,
		(int)NKBGLastBudgetHit);
}

void OpenGLFrameBuffer::ResetBackgroundCacheStats()
{
	NKBGSubmitted = (unsigned int)NKBGCachedMaterials.Size();
	NKBGConsumed = 0;
	NKBGCollisions = 0;
	NKBGThrottleFramesLeft = 0;
	NKBGUploadTotalUS = 0;
	NKBGUploadMaxUS = 0;
	NKBGLastBatchUS = 0;
	NKBGLastMaxUS = 0;
	NKBGSlowUploads = 0;
	NKBGSlowBatches = 0;
	NKBGBudgetHits = 0;
	NKBGProcessedBatches = 0;
	NKBGPeakBatchUS = 0;
	NKBGPeakBatchJobs = 0;
	NKBGPeakAtSubmitted = NKBGSubmitted;
	NKBGPeakAtConsumed = NKBGConsumed;
	NKBGDeferredByBudget = 0;
	NKBGDeferredByCooldown = 0;
	NKBGFramesWithUploads = 0;
	NKBGFlushes = 0;
	NKBGLastProcessed = 0;
	NKBGLastLimit = 0;
	NKBGLastBudgetMS = 0;
	NKBGLastBudgetHit = false;
}

void OpenGLFrameBuffer::StopBackgroundCache()
{
	if (NKBGCachedMaterials.Size() > 0 && nk_warmup_debug)
	{
		Printf("[NKBGCache] stop cleared remaining=%u submitted=%u consumed=%u collisions=%u\n",
			(unsigned int)NKBGCachedMaterials.Size(), NKBGSubmitted, NKBGConsumed, NKBGCollisions);
	}
	NKBGCachedMaterials.Clear();
	NKBGThrottleFramesLeft = 0;
}

void OpenGLFrameBuffer::FlushBackground()
{
	UpdateBackgroundCache(true);
}

IVertexBuffer *OpenGLFrameBuffer::CreateVertexBuffer()
{ 
	return new GLVertexBuffer; 
}

IIndexBuffer *OpenGLFrameBuffer::CreateIndexBuffer()
{ 
	return new GLIndexBuffer; 
}

IDataBuffer *OpenGLFrameBuffer::CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize)
{
	return new GLDataBuffer(bindingpoint, ssbo);
}

void OpenGLFrameBuffer::BlurScene(float amount)
{
	GLRenderer->BlurScene(amount);
}

void OpenGLFrameBuffer::InitLightmap(int LMTextureSize, int LMTextureCount, TArray<uint16_t>& LMTextureData)
{
	if (LMTextureData.Size() > 0)
	{
		GLint activeTex = 0;
		glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTex);
		glActiveTexture(GL_TEXTURE0 + 17);

		if (GLRenderer->mLightMapID == 0)
			glGenTextures(1, (GLuint*)&GLRenderer->mLightMapID);

		glBindTexture(GL_TEXTURE_2D_ARRAY, GLRenderer->mLightMapID);
		glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGB16F, LMTextureSize, LMTextureSize, LMTextureCount, 0, GL_RGB, GL_HALF_FLOAT, &LMTextureData[0]);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glGenerateMipmap(GL_TEXTURE_2D_ARRAY);

		glActiveTexture(activeTex);

		LMTextureData.Reset(); // We no longer need this, release the memory
	}
}

void OpenGLFrameBuffer::SetViewportRects(IntRect *bounds)
{
	Super::SetViewportRects(bounds);
	if (!bounds)
	{
		auto vrmode = VRMode::GetVRMode(true);
		vrmode->AdjustViewport(this);
	}
}

void OpenGLFrameBuffer::UpdatePalette()
{
	if (GLRenderer)
		GLRenderer->ClearTonemapPalette();
}

FRenderState* OpenGLFrameBuffer::RenderState()
{
	return &gl_RenderState;
}

void OpenGLFrameBuffer::AmbientOccludeScene(float m5)
{
	gl_RenderState.EnableDrawBuffers(1);
	GLRenderer->AmbientOccludeScene(m5);
	glViewport(screen->mSceneViewport.left, mSceneViewport.top, mSceneViewport.width, mSceneViewport.height);
	GLRenderer->mBuffers->BindSceneFB(true);
	gl_RenderState.EnableDrawBuffers(gl_RenderState.GetPassDrawBufferCount());
	gl_RenderState.Apply();
}

void OpenGLFrameBuffer::FirstEye()
{
	GLRenderer->mBuffers->CurrentEye() = 0;  // always begin at zero, in case eye count changed
}

void OpenGLFrameBuffer::NextEye(int eyecount)
{
	GLRenderer->mBuffers->NextEye(eyecount);
}

void OpenGLFrameBuffer::SetSceneRenderTarget(bool useSSAO)
{
	GLRenderer->mBuffers->BindSceneFB(useSSAO);
}

void OpenGLFrameBuffer::UpdateShadowMap()
{
	if (mShadowMap.PerformUpdate())
	{
		FGLDebug::PushGroup("ShadowMap");

		FGLPostProcessState savedState;

		static_cast<GLDataBuffer*>(screen->mShadowMap.mLightList)->BindBase();
		if (screen->mShadowMap.mNkSpriteShadowProjectorsBuffer != nullptr)
		{
			static_cast<GLDataBuffer*>(screen->mShadowMap.mNkSpriteShadowProjectorsBuffer)->BindBase();
		}
		static_cast<GLDataBuffer*>(screen->mShadowMap.mNodesBuffer)->BindBase();
		static_cast<GLDataBuffer*>(screen->mShadowMap.mLinesBuffer)->BindBase();

		GLRenderer->mBuffers->BindShadowMapFB();

		GLRenderer->mShadowMapShader->Bind();
		GLRenderer->mShadowMapShader->Uniforms->ShadowmapQuality = gl_shadowmap_quality;
		GLRenderer->mShadowMapShader->Uniforms->NodesCount = screen->mShadowMap.NodesCount();
		GLRenderer->mShadowMapShader->Uniforms.SetData();
		static_cast<GLDataBuffer*>(GLRenderer->mShadowMapShader->Uniforms.GetBuffer())->BindBase();

		glViewport(0, 0, gl_shadowmap_quality, 1024);
		GLRenderer->RenderScreenQuad();

		const auto& viewport = screen->mScreenViewport;
		glViewport(viewport.left, viewport.top, viewport.width, viewport.height);

		GLRenderer->mBuffers->BindShadowMapTexture(16);
		FGLDebug::PopGroup();
		screen->mShadowMap.FinishUpdate();
	}
}

void OpenGLFrameBuffer::WaitForCommands(bool finish)
{
	glFinish();
}

void OpenGLFrameBuffer::SetSaveBuffers(bool yes)
{
	if (!GLRenderer) return;
	if (yes) GLRenderer->mBuffers = GLRenderer->mSaveBuffers;
	else GLRenderer->mBuffers = GLRenderer->mScreenBuffers;
}

//===========================================================================
//
// 
//
//===========================================================================

void OpenGLFrameBuffer::BeginFrame()
{
	UpdateBackgroundCache(false);
	SetViewportRects(nullptr);
	mViewpoints->Clear();
	if (GLRenderer != nullptr)
		GLRenderer->BeginFrame();
}

//===========================================================================
// 
//	Takes a screenshot
//
//===========================================================================

TArray<uint8_t> OpenGLFrameBuffer::GetScreenshotBuffer(int &pitch, ESSType &color_type, float &gamma)
{
	const auto &viewport = mOutputLetterbox;

	// Grab what is in the back buffer.
	// We cannot rely on SCREENWIDTH/HEIGHT here because the output may have been scaled.
	TArray<uint8_t> pixels;
	pixels.Resize(viewport.width * viewport.height * 3);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(viewport.left, viewport.top, viewport.width, viewport.height, GL_RGB, GL_UNSIGNED_BYTE, &pixels[0]);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);

	// Copy to screenshot buffer:
	int w = SCREENWIDTH;
	int h = SCREENHEIGHT;

	TArray<uint8_t> ScreenshotBuffer(w * h * 3, true);

	float rcpWidth = 1.0f / w;
	float rcpHeight = 1.0f / h;
	for (int y = 0; y < h; y++)
	{
		for (int x = 0; x < w; x++)
		{
			float u = (x + 0.5f) * rcpWidth;
			float v = (y + 0.5f) * rcpHeight;
			int sx = u * viewport.width;
			int sy = v * viewport.height;
			int sindex = (sx + sy * viewport.width) * 3;
			int dindex = (x + (h - y - 1) * w) * 3;
			ScreenshotBuffer[dindex] = pixels[sindex];
			ScreenshotBuffer[dindex + 1] = pixels[sindex + 1];
			ScreenshotBuffer[dindex + 2] = pixels[sindex + 2];
		}
	}

	pitch = w * 3;
	color_type = SS_RGB;

	// Screenshot should not use gamma correction if it was already applied to rendered image
	gamma = 1;
	if (vid_hdr_active && vid_fullscreen)
		gamma *= 2.2f;
	return ScreenshotBuffer;
}

//===========================================================================
// 
// 2D drawing
//
//===========================================================================

void OpenGLFrameBuffer::Draw2D()
{
	if (GLRenderer != nullptr)
	{
		GLRenderer->mBuffers->BindCurrentFB();
		::Draw2D(twod, gl_RenderState);
	}
}

void OpenGLFrameBuffer::PostProcessScene(bool swscene, int fixedcm, float flash, const std::function<void()> &afterBloomDrawEndScene2D)
{
	if (!swscene) GLRenderer->mBuffers->BlitSceneToTexture(); // Copy the resulting scene to the current post process texture
	GLRenderer->PostProcessScene(fixedcm, flash, afterBloomDrawEndScene2D);
}

bool OpenGLFrameBuffer::CompileNextShader()
{
	return GLRenderer->mShaderManager->CompileNextShader();
}

//==========================================================================
//
// OpenGLFrameBuffer :: WipeStartScreen
//
// Called before the current screen has started rendering. This needs to
// save what was drawn the previous frame so that it can be animated into
// what gets drawn this frame.
//
//==========================================================================

FTexture *OpenGLFrameBuffer::WipeStartScreen()
{
	const auto &viewport = screen->mScreenViewport;

	auto tex = new FWrapperTexture(viewport.width, viewport.height, 1);
	tex->GetSystemTexture()->CreateTexture(nullptr, viewport.width, viewport.height, 0, false, "WipeStartScreen");
	glFinish();
	static_cast<FHardwareTexture*>(tex->GetSystemTexture())->Bind(0, false);

	GLRenderer->mBuffers->BindCurrentFB();
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, viewport.left, viewport.top, viewport.width, viewport.height);
	return tex;
}

//==========================================================================
//
// OpenGLFrameBuffer :: WipeEndScreen
//
// The screen we want to animate to has just been drawn.
//
//==========================================================================

FTexture *OpenGLFrameBuffer::WipeEndScreen()
{
	GLRenderer->Flush();
	const auto &viewport = screen->mScreenViewport;
	auto tex = new FWrapperTexture(viewport.width, viewport.height, 1);
	tex->GetSystemTexture()->CreateTexture(NULL, viewport.width, viewport.height, 0, false, "WipeEndScreen");
	glFinish();
	static_cast<FHardwareTexture*>(tex->GetSystemTexture())->Bind(0, false);
	GLRenderer->mBuffers->BindCurrentFB();
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, viewport.left, viewport.top, viewport.width, viewport.height);
	return tex;
}

}
