#pragma once

#include <stdint.h>
#include "tarray.h"

typedef TMap<int, bool> SpriteHits;
class FTexture;

class IHardwareTexture
{
public:
	enum
	{
		MAX_TEXTURES = 16
	};

	enum HardwareState
	{
		NONE      = 0, // Uninitialized
		LOADING   = 1, // Waiting on high-priority background load
		CACHING   = 2, // Waiting on low-priority background cache
		UPLOADING = 3, // Waiting on transfer/upload completion
		READY     = 4  // Fully ready for rendering
	};

	IHardwareTexture() = default;
	virtual ~IHardwareTexture() = default;

	virtual HardwareState GetState(int texUnit = 0) { return hwState; }
	virtual void SetHardwareState(HardwareState hws, int texUnit = 0) { hwState = hws; }
	virtual bool IsValid(int texUnit = 0) { return GetState(texUnit) == READY; }

	virtual void AllocateBuffer(int w, int h, int texelsize) = 0;
	virtual uint8_t *MapBuffer() = 0;
	virtual unsigned int CreateTexture(unsigned char * buffer, int w, int h, int texunit, bool mipmap, const char *name) = 0;

	void Resize(int swidth, int sheight, int width, int height, unsigned char *src_data, unsigned char *dst_data);

	int GetBufferPitch() const { return bufferpitch; }

protected:
	int bufferpitch = -1;
	HardwareState hwState = NONE;
};
