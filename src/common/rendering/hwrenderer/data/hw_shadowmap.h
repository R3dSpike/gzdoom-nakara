
#pragma once

#include "hw_aabbtree.h"
#include "stats.h"
#include <memory>
#include <functional>
#include <stdint.h>

class IDataBuffer;



// [Nakara] Receiver-shader sprite projector data.
// This is sampled by main.fp on world receiver fragments. It is not a thinker, actor, or decal.
struct NkSpriteShadowProjector
{
	// Shader coordinate order is (map X, height Z, map Y), matching dynamic light uploads.
	float lightX, lightY, lightZ, radius;
	float originX, originY, originZ, strength;
	float rightX, rightY, rightZ, softness;
	float upX, upY, upZ, maxLength;
	// x = side fade width in UV space.
	// y = contact-hardening additional softness.
	// z = receiver distance where that additional softness is fully applied.
	// w = distance-based opacity fade amount.
	float sideFade, reserved0, reserved1, reserved2;
	// x = receiver surface angle fade amount.
	// y = minimum abs(N dot L) before the fade starts.
	// z = angle response power.
	// w = reserved.
	float angleFade, angleMinDot, anglePower, angleReserved;
	float mask[1024];
};
class IShadowMap
{
public:
	IShadowMap() { }
	virtual ~IShadowMap();

	void Reset();

	// Test if a world position is in shadow relative to the specified light and returns false if it is
	bool ShadowTest(const DVector3 &lpos, const DVector3 &pos);

	static cycle_t UpdateCycles;
	static int LightsProcessed;
	static int LightsShadowmapped;
	static int NkSpriteProjectors;

	bool PerformUpdate();
	void FinishUpdate()
	{
		UpdateCycles.Clock();
	}

	unsigned int NodesCount() const
	{
		assert(mAABBTree);
		return mAABBTree->NodesCount();
	}

	void SetAABBTree(hwrenderer::LevelAABBTree* tree)
	{
		if (mAABBTree != tree)
		{
			mAABBTree = tree;
			mNewTree = true;
		}
	}

	void SetCollectLights(std::function<void()> func)
	{
		CollectLights = std::move(func);
	}

	void SetLight(int index, float x, float y, float z, float r)
	{
		index *= 4;
		mLights[index] = x;
		mLights[index + 1] = y;
		mLights[index + 2] = z;
		mLights[index + 3] = r;
	}

	void ClearNkSpriteShadowProjectors()
	{
		mNkSpriteShadowProjectors.Clear();
	}

	void AddNkSpriteShadowProjector(const NkSpriteShadowProjector &projector)
	{
		mNkSpriteShadowProjectors.Push(projector);
	}

	int NkSpriteShadowProjectorCount() const
	{
		return (int)mNkSpriteShadowProjectors.Size();
	}

	bool Enabled() const
	{
		return mAABBTree != nullptr;
	}

	// [Nakara] Clear the receiver-projector SSBO immediately. This prevents stale
	// receiver shadows from remaining visible after nk_receiver_sprite_shadows is toggled off.
	void DisableNkSpriteShadowProjectors();

protected:
	// Upload the AABB-tree to the GPU
	void UploadAABBTree();
	void UploadLights();
	void UploadNkSpriteShadowProjectors();

	// Working buffer for creating the list of lights. Stored here to avoid allocating memory each frame
	TArray<float> mLights;
	TArray<NkSpriteShadowProjector> mNkSpriteShadowProjectors;

	// AABB-tree of the level, used for ray tests, owned by the playsim, not the renderer.
	hwrenderer::LevelAABBTree* mAABBTree = nullptr;
	bool mNewTree = false;

	IShadowMap(const IShadowMap &) = delete;
	IShadowMap &operator=(IShadowMap &) = delete;

	// OpenGL storage buffer with the list of lights in the shadow map texture
	// These buffers need to be accessed by the OpenGL backend directly so that they can be bound.
public:
	IDataBuffer *mLightList = nullptr;
	IDataBuffer *mNkSpriteShadowProjectorsBuffer = nullptr;

	// OpenGL storage buffers for the AABB tree
	IDataBuffer *mNodesBuffer = nullptr;
	IDataBuffer *mLinesBuffer = nullptr;

	std::function<void()> CollectLights = nullptr;

};
