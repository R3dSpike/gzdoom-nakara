#pragma once

#include "vectors.h"
#include <cstdint>

// Runtime-only history shared by actor-owned and detached visual ribbon trails.
// It is intentionally not serialized: saved games resume without transient
// renderer-only ribbon geometry.
enum EParticleTrailHistorySampleFlags : uint8_t
{
	PTHSF_None = 0,
	PTHSF_PortalEntry = 1 << 0, // [Nakara] Last source-space node before a line-portal split.
	PTHSF_PortalExit = 1 << 1,  // [Nakara] First destination-space node after a line-portal split.
};

struct FParticleTrailHistorySample
{
	DVector3 Pos;
	DRotator Angles; // [Nakara] Legacy sample orientation retained for source compatibility; ribbon rendering ignores it.
	double SpawnTime; // [Nakara] Exact sub-tic path time: maptime + movement fraction.
	double PathDistance = 0.0; // [Nakara V25.2] Stable cumulative center-line distance used as the wave phase coordinate.
	int PortalGroup; // [Nakara] Coordinate space where Pos was recorded.
	uint32_t Generation; // [Nakara] Ribbon chain ID. Never connect samples from another projectile/retired carrier.
	DVector3 PortalSeamTangent = DVector3(0.0, 0.0, 0.0); // [Nakara] Source/destination path direction at a portal seam.
	uint8_t PortalSeamFlags = PTHSF_None; // [Nakara] Marks exact line-portal entry/exit endpoints for renderer frame continuity.
};
