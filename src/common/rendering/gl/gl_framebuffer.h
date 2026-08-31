#ifndef __GL_FRAMEBUFFER
#define __GL_FRAMEBUFFER

#include "gl_sysfb.h"
#include "m_png.h"
#include "tarray.h"

#include <memory>

namespace OpenGLRenderer
{

class FHardwareTexture;
class FGLDebug;

class OpenGLFrameBuffer : public SystemGLFrameBuffer
{
	typedef SystemGLFrameBuffer Super;

	void RenderTextureView(FCanvasTexture* tex, std::function<void(IntRect &)> renderFunc) override;

public:

	OpenGLFrameBuffer(void *hMonitor, bool fullscreen) ;
	~OpenGLFrameBuffer();
	int Backend() override { return 2; }
	bool CompileNextShader() override;
	void InitializeState() override;
	void Update() override;

	void AmbientOccludeScene(float m5) override;
	void FirstEye() override;
	void NextEye(int eyecount) override;
	void SetSceneRenderTarget(bool useSSAO) override;
	void UpdateShadowMap() override;
	void WaitForCommands(bool finish) override;
	void SetSaveBuffers(bool yes) override;
	void CopyScreenToBuffer(int width, int height, uint8_t* buffer) override;
	bool FlipSavePic() const override { return true; }

	FRenderState* RenderState() override;
	void UpdatePalette() override;
	const char* DeviceName() const override;
	void SetTextureFilterMode() override;
	IHardwareTexture *CreateHardwareTexture(int numchannels) override;
	void PrecacheMaterial(FMaterial *mat, int translation) override;
	void PrequeueMaterial(FMaterial *mat, int translation) override;
	bool BackgroundCacheMaterial(FMaterial *mat, FTranslationID translation, bool makeSPI = false, bool secondary = false) override;
	bool BackgroundCacheTextureMaterial(FGameTexture *tex, FTranslationID translation, int scaleFlags, bool makeSPI = false) override;
	bool SupportsBackgroundCache() override;
	bool CachingActive() override;
	void UpdateBackgroundCache(bool flush = false) override;
	void StopBackgroundCache() override;
	void FlushBackground() override;
	void PrintBackgroundCacheStatus() override;
	void ResetBackgroundCacheStats() override;
	float CacheProgress() override;
	void BeginFrame() override;
	void SetViewportRects(IntRect *bounds) override;
	void BlurScene(float amount) override;
	IVertexBuffer *CreateVertexBuffer() override;
	IIndexBuffer *CreateIndexBuffer() override;
	IDataBuffer *CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize) override;

	void InitLightmap(int LMTextureSize, int LMTextureCount, TArray<uint16_t>& LMTextureData) override;

	// Retrieves a buffer containing image data for a screenshot.
	// Hint: Pitch can be negative for upside-down images, in which case buffer
	// points to the last row in the buffer, which will be the first row output.
	virtual TArray<uint8_t> GetScreenshotBuffer(int &pitch, ESSType &color_type, float &gamma) override;

	void Swap();
	bool IsHWGammaActive() const { return HWGammaActive; }

	void SetVSync(bool vsync) override;

	void Draw2D() override;
	void PostProcessScene(bool swscene, int fixedcm, float flash, const std::function<void()> &afterBloomDrawEndScene2D) override;

	bool HWGammaActive = false;			// Are we using hardware or software gamma?
	std::unique_ptr<FGLDebug> mDebug;	// Debug API

    FTexture *WipeStartScreen() override;
    FTexture *WipeEndScreen() override;

	int camtexcount = 0;

private:
	struct NKGLQueuedMaterial
	{
		FMaterial *Material;
		int Translation;
		bool MakeSPI;
		bool Secondary;
	};

	TArray<NKGLQueuedMaterial> NKBGCachedMaterials;
	unsigned int NKBGSubmitted = 0;
	unsigned int NKBGConsumed = 0;
	unsigned int NKBGCollisions = 0;

	// NKS: Main-thread queue pacing. This does not move GL uploads to another
	// thread yet; it avoids draining the queue every frame after a slow upload.
	int NKBGThrottleFramesLeft = 0;
	uint64_t NKBGUploadTotalUS = 0;
	uint64_t NKBGUploadMaxUS = 0;
	uint64_t NKBGPeakBatchUS = 0;
	int NKBGPeakBatchJobs = 0;
	unsigned int NKBGPeakAtSubmitted = 0;
	unsigned int NKBGPeakAtConsumed = 0;
	uint64_t NKBGLastBatchUS = 0;
	uint64_t NKBGLastMaxUS = 0;
	unsigned int NKBGSlowUploads = 0;
	unsigned int NKBGSlowBatches = 0;
	unsigned int NKBGBudgetHits = 0;
	unsigned int NKBGProcessedBatches = 0;
	unsigned int NKBGDeferredByBudget = 0;
	unsigned int NKBGDeferredByCooldown = 0;
	unsigned int NKBGFramesWithUploads = 0;
	unsigned int NKBGFlushes = 0;
	int NKBGLastProcessed = 0;
	int NKBGLastLimit = 0;
	int NKBGLastBudgetMS = 0;
	bool NKBGLastBudgetHit = false;

	bool NKBGQueueMaterial(FMaterial *mat, int translation, bool makeSPI, bool secondary);
	int NKBGEffectiveLimit(bool flush) const;
	int NKBGEffectiveBudgetMS(bool flush) const;
	int NKBGEffectiveSlowMS() const;
	int NKBGEffectiveCooldownFrames() const;
};

}

#endif //__GL_FRAMEBUFFER
