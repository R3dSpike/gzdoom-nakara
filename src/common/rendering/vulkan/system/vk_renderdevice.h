#pragma once

#include "gl_sysfb.h"
#include "engineerrors.h"
#include <zvulkan/vulkandevice.h>
#include <zvulkan/vulkanobjects.h>
#include <stdint.h>
#include "tarray.h"

struct FRenderViewpoint;
class VkSamplerManager;
class VkBufferManager;
class VkTextureManager;
class VkShaderManager;
class VkCommandBufferManager;
class VkDescriptorSetManager;
class VkRenderPassManager;
class VkFramebufferManager;
class VkRaytrace;
class VkRenderState;
class VkStreamBuffer;
class VkHardwareDataBuffer;
class VkHardwareTexture;
class VkRenderBuffers;
class VkPostprocess;
class SWSceneDrawer;

class VulkanRenderDevice : public SystemBaseFrameBuffer
{
	typedef SystemBaseFrameBuffer Super;


public:
	std::shared_ptr<VulkanDevice> device;

	VkCommandBufferManager* GetCommands() { return mCommands.get(); }
	VkShaderManager *GetShaderManager() { return mShaderManager.get(); }
	VkSamplerManager *GetSamplerManager() { return mSamplerManager.get(); }
	VkBufferManager* GetBufferManager() { return mBufferManager.get(); }
	VkTextureManager* GetTextureManager() { return mTextureManager.get(); }
	VkFramebufferManager* GetFramebufferManager() { return mFramebufferManager.get(); }
	VkDescriptorSetManager* GetDescriptorSetManager() { return mDescriptorSetManager.get(); }
	VkRenderPassManager *GetRenderPassManager() { return mRenderPassManager.get(); }
	VkRaytrace* GetRaytrace() { return mRaytrace.get(); }
	VkRenderState *GetRenderState() { return mRenderState.get(); }
	VkPostprocess *GetPostprocess() { return mPostprocess.get(); }
	VkRenderBuffers *GetBuffers() { return mActiveRenderBuffers; }
	FRenderState* RenderState() override;

	unsigned int GetLightBufferBlockSize() const;

	VulkanRenderDevice(void *hMonitor, bool fullscreen, std::shared_ptr<VulkanSurface> surface);
	~VulkanRenderDevice();
	bool IsVulkan() override { return true; }

	void Update() override;

	void InitializeState() override;
	bool CompileNextShader() override;
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
	void UpdatePalette() override;
	const char* DeviceName() const override;
	int Backend() override { return 1; }
	void SetTextureFilterMode() override;
	void StartPrecaching() override;
	void BeginFrame() override;
	void InitLightmap(int LMTextureSize, int LMTextureCount, TArray<uint16_t>& LMTextureData) override;
	void BlurScene(float amount) override;
	void PostProcessScene(bool swscene, int fixedcm, float flash, const std::function<void()> &afterBloomDrawEndScene2D) override;
	void AmbientOccludeScene(float m5) override;
	void SetSceneRenderTarget(bool useSSAO) override;
	void SetLevelMesh(hwrenderer::LevelMesh* mesh) override;
	void UpdateShadowMap() override;
	void SetSaveBuffers(bool yes) override;
	void ImageTransitionScene(bool unknown) override;
	void SetActiveRenderTarget() override;

	IHardwareTexture *CreateHardwareTexture(int numchannels) override;
	FMaterial* CreateMaterial(FGameTexture* tex, int scaleflags) override;
	IVertexBuffer *CreateVertexBuffer() override;
	IIndexBuffer *CreateIndexBuffer() override;
	IDataBuffer *CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize) override;

	FTexture *WipeStartScreen() override;
	FTexture *WipeEndScreen() override;

	TArray<uint8_t> GetScreenshotBuffer(int &pitch, ESSType &color_type, float &gamma) override;

	bool GetVSync() { return mVSync; }
	void SetVSync(bool vsync) override;

	void Draw2D() override;

	void WaitForCommands(bool finish) override;

	bool RaytracingEnabled();

private:
	struct NKVKQueuedMaterial
	{
		FMaterial *Material;
		int Translation;
		bool MakeSPI;
		bool Secondary;
	};

	TArray<NKVKQueuedMaterial> NKBGCachedMaterials;
	unsigned int NKBGSubmitted = 0;
	unsigned int NKBGConsumed = 0;
	unsigned int NKBGCollisions = 0;
	unsigned int NKBGSlowUploads = 0;
	unsigned int NKBGSlowBatches = 0;
	unsigned int NKBGBudgetHits = 0;
	unsigned int NKBGProcessedBatches = 0;
	unsigned int NKBGDeferredByBudget = 0;
	unsigned int NKBGDeferredByCooldown = 0;
	unsigned int NKBGFramesWithUploads = 0;
	unsigned int NKBGFlushes = 0;
	uint64_t NKBGUploadTotalUS = 0;
	uint64_t NKBGUploadMaxUS = 0;
	uint64_t NKBGPeakBatchUS = 0;
	int NKBGPeakBatchJobs = 0;
	unsigned int NKBGPeakAtSubmitted = 0;
	unsigned int NKBGPeakAtConsumed = 0;
	uint64_t NKBGLastBatchUS = 0;
	uint64_t NKBGLastMaxUS = 0;
	int NKBGThrottleFramesLeft = 0;
	int NKBGLastProcessed = 0;
	int NKBGLastLimit = 0;
	int NKBGLastBudgetMS = 0;
	bool NKBGLastBudgetHit = false;

	bool NKBGQueueMaterial(FMaterial *mat, int translation, bool makeSPI, bool secondary);
	int NKBGEffectiveLimit(bool flush) const;
	int NKBGEffectiveBudgetMS(bool flush) const;
	int NKBGEffectiveSlowMS() const;
	int NKBGEffectiveCooldownFrames() const;

	void RenderTextureView(FCanvasTexture* tex, std::function<void(IntRect &)> renderFunc) override;
	void PrintStartupLog();
	void CopyScreenToBuffer(int w, int h, uint8_t *data) override;

	std::unique_ptr<VkCommandBufferManager> mCommands;
	std::unique_ptr<VkBufferManager> mBufferManager;
	std::unique_ptr<VkSamplerManager> mSamplerManager;
	std::unique_ptr<VkTextureManager> mTextureManager;
	std::unique_ptr<VkFramebufferManager> mFramebufferManager;
	std::unique_ptr<VkShaderManager> mShaderManager;
	std::unique_ptr<VkRenderBuffers> mScreenBuffers;
	std::unique_ptr<VkRenderBuffers> mSaveBuffers;
	std::unique_ptr<VkPostprocess> mPostprocess;
	std::unique_ptr<VkDescriptorSetManager> mDescriptorSetManager;
	std::unique_ptr<VkRenderPassManager> mRenderPassManager;
	std::unique_ptr<VkRaytrace> mRaytrace;
	std::unique_ptr<VkRenderState> mRenderState;

	VkRenderBuffers *mActiveRenderBuffers = nullptr;

	bool mVSync = false;
};

class CVulkanError : public CEngineError
{
public:
	CVulkanError() : CEngineError() {}
	CVulkanError(const char* message) : CEngineError(message) {}
};
