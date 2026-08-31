/*
** p_effect.cpp
** Particle effects
**
**---------------------------------------------------------------------------
** Copyright 1998-2006 Randy Heit
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
** If particles used real sprites instead of blocks, they could be much
** more useful.
*/

#include "doomtype.h"
#include "doomstat.h"
#include "c_cvars.h"

#include "actor.h"
#include "m_argv.h"
#include "p_effect.h"
#include "p_local.h"
#include "p_maputl.h"
#include "r_defs.h"
#include "p_3dfloors.h"
#include "gi.h"
#include "d_player.h"
#include "r_utility.h"
#include "g_levellocals.h"
#include "vm.h"
#include "actorinlines.h"
#include "g_game.h"
#include "serializer_doom.h"
#include "p_visualthinker.h"
#include "texturemanager.h"
#include "models.h"
#include "info.h"

#include <cmath>

#include "hwrenderer/scene/hw_drawstructs.h"

#ifdef _MSC_VER
#pragma warning(disable: 6011) // dereference null pointer in thinker iterator
#endif

CVAR (Int, cl_rockettrails, 1, CVAR_ARCHIVE);
CVAR (Bool, r_rail_smartspiral, false, CVAR_ARCHIVE);
CVAR (Int, r_rail_spiralsparsity, 1, CVAR_ARCHIVE);
CVAR (Int, r_rail_trailsparsity, 1, CVAR_ARCHIVE);
CVAR (Bool, r_particles, true, 0);
// [Nakara] Draw/simulation distance for decorative fish schools. Kept separate
// from bubbles because fish should remain visible across the large underwater map.
CVAR (Int, r_fishdist, 6500, CVAR_ARCHIVE);
// [Nakara Debug] Ribbon diagnostic logging switch. Keep disabled during normal
// play; enable from the console with: nk_ribbon_debug 1
CVAR (Bool, nk_ribbon_debug, false, 0);
EXTERN_CVAR(Int, r_maxparticles);

FCRandom pr_railtrail("RailTrail");
// [Nakara] Client-side RNG: underwater bubbles are visual-only and must not
// perturb gameplay RNG streams.
static FCRandom pr_nkunderwaterambient("NKUnderwaterAmbient");
// [Nakara] Visual-only RNG for the lightweight underwater fish school system.
static FCRandom pr_nkfishschool("NKFishSchool");

// Fish use the existing particle pool but keep their simulation state outside
// particle_t so the core particle structure remains 128 bytes. No Actor or
// Thinker is created per fish.
static constexpr uint16_t NKFISH_NO_SCHOOL = 0xffff;
static constexpr int NKFISH_MAX_FLOCK_COUNT = 26;

struct FNakaraFishParticleState
{
	DVector3 AnchorOffset;
	DVector3 RoamOffset;
	DVector3 FleeDirection;
	DVector3 CuriosityDirection;
	float Phase = 0.f;
	double SwimPhase = 0.0;
	float SpeedScale = 1.f;
	float SpriteScale = 1.f;
	double CollisionRadius = 12.0;
	float Bank = 0.f;
	float FlockAffinity = 1.f;
	int32_t NextRoamTic = 0;
	int32_t FleeUntilTic = 0;
	int32_t NextCuriosityTic = 0;
	int32_t CuriosityUntilTic = 0;
	int32_t ObstacleRecoveryUntilTic = 0;
	uint8_t RejoinAfterFlee = 0;
	uint8_t RejoinAfterCuriosity = 0;
	uint16_t School = NKFISH_NO_SCHOOL;
	int16_t SpriteIndex = -1;
	uint8_t Flock = 0;
	uint8_t FirstFrame = 0;
	uint8_t FrameCount = 1;
	uint8_t AnimStep = 4;
	uint8_t UpdatePhase = 0;
};

struct FNakaraFishFlockState
{
	DVector3 Center;
	DVector3 Velocity;
	DVector3 Target;
	DVector3 AveragePos;
	DVector3 AverageVel;
	float Phase = 0.f;
	int32_t NextTargetTic = 0;
	int Count = 0;
};

struct FNakaraFishSchoolState
{
	DVector3 Origin;
	DVector3 Center;
	DVector3 AveragePos;
	DVector3 AverageVel;
	float Radius = 256.f;
	float Height = 128.f;
	double BaseSpeed = 0.75;
	float AvoidRadius = 176.f;
	float CohesionScale = 1.40f;
	double FleeSpeedScale = 1.80;
	double FleeHoldSeconds = 1.50;
	double SwimBeatScale = 1.00;
	double CollisionRadius = 12.0;
	float Phase = 0.f;
	uint8_t FlockCount = 1;
	FNakaraFishFlockState Flocks[NKFISH_MAX_FLOCK_COUNT];
	int Count = 0;
};

struct FNakaraFishLevelState
{
	FLevelLocals *Level = nullptr;
	TArray<FNakaraFishParticleState> Fish;
	TArray<FNakaraFishSchoolState> Schools;
};

static TArray<FNakaraFishLevelState> NakaraFishLevels;

static FNakaraFishLevelState *P_NakaraFishLevel(FLevelLocals *Level, bool create)
{
	if (Level == nullptr) return nullptr;
	for (auto &state : NakaraFishLevels)
	{
		if (state.Level == Level) return &state;
	}
	if (!create) return nullptr;
	FNakaraFishLevelState state;
	state.Level = Level;
	NakaraFishLevels.Push(state);
	return &NakaraFishLevels.Last();
}

static void P_NakaraFishReset(FLevelLocals *Level)
{
	for (unsigned i = 0; i < NakaraFishLevels.Size(); ++i)
	{
		if (NakaraFishLevels[i].Level == Level)
		{
			NakaraFishLevels.Delete(i);
			return;
		}
	}
}

void P_ClearNakaraFishLevelState(FLevelLocals *Level)
{
	P_NakaraFishReset(Level);
}

static void P_NakaraFishClearParticleState(FLevelLocals *Level, int particleIndex)
{
	auto state = P_NakaraFishLevel(Level, false);
	if (state == nullptr || particleIndex < 0 || (unsigned)particleIndex >= state->Fish.Size()) return;
	state->Fish[particleIndex] = FNakaraFishParticleState();
}

#define FADEFROMTTL(a)	(1.f/(a))

static int grey1, grey2, grey3, grey4, red, green, blue, yellow, black,
		   red1, green1, blue1, yellow1, purple, purple1, white,
		   rblue1, rblue2, rblue3, rblue4, orange, yorange, dred, grey5,
		   maroon1, maroon2, blood1, blood2;

static const struct ColorList {
	int *color;
	uint8_t r, g, b;
} Colors[] = {
	{&grey1,	85,  85,  85 },
	{&grey2,	171, 171, 171},
	{&grey3,	50,  50,  50 },
	{&grey4,	210, 210, 210},
	{&grey5,	128, 128, 128},
	{&red,		255, 0,   0  },  
	{&green,	0,   200, 0  },  
	{&blue,		0,   0,   255},
	{&yellow,	255, 255, 0  },  
	{&black,	0,   0,   0  },  
	{&red1,		255, 127, 127},
	{&green1,	127, 255, 127},
	{&blue1,	127, 127, 255},
	{&yellow1,	255, 255, 180},
	{&purple,	120, 0,   160},
	{&purple1,	200, 30,  255},
	{&white, 	255, 255, 255},
	{&rblue1,	81,  81,  255},
	{&rblue2,	0,   0,   227},
	{&rblue3,	0,   0,   130},
	{&rblue4,	0,   0,   80 },
	{&orange,	255, 120, 0  },
	{&yorange,	255, 170, 0  },
	{&dred,		80,  0,   0  },
	{&maroon1,	154, 49,  49 },
	{&maroon2,	125, 24,  24 },
	{NULL, 0, 0, 0 }
};

static void FreeParticle(FLevelLocals* Level, particle_t* particle)
{
	auto prev = particle->tprev == NO_PARTICLE? nullptr : &Level->Particles[particle->tprev];
	int pindex = (int)(particle - Level->Particles.Data());
	if ((unsigned)pindex < Level->NakaraParticleGroups.Size())
	{
		Level->NakaraParticleGroups[pindex] = NPG_None;
	}
	P_NakaraFishClearParticleState(Level, pindex);
	auto tnext = particle->tnext;
	assert(!prev || (prev->tnext == pindex));
	if (prev)
		prev->tnext = tnext;
	else
		Level->ActiveParticles = tnext;

	if (tnext != NO_PARTICLE)
	{
		particle_t* next = &Level->Particles[tnext];
		assert(next->tprev == pindex);
		next->tprev = particle->tprev;
	}
	if (Level->OldestParticle == pindex)
	{
		assert(tnext == NO_PARTICLE);
		Level->OldestParticle = particle->tprev;
	}
	memset(particle, 0, sizeof(particle_t));
	particle->tnext = Level->InactiveParticles;
	Level->InactiveParticles = pindex;
}

static particle_t *NewParticle (FLevelLocals *Level, bool replace = false)
{
	// Array's filled up
	if (Level->InactiveParticles == NO_PARTICLE && Level->OldestParticle != NO_PARTICLE)
	{
		if (!replace) return nullptr;
		FreeParticle(Level, &Level->Particles[Level->OldestParticle]);
	}
	
	// Array isn't full.
	uint32_t current = Level->ActiveParticles;
	auto result = &Level->Particles[Level->InactiveParticles];
	Level->InactiveParticles = result->tnext;
	result->tnext = current;
	result->tprev = NO_PARTICLE;
	Level->ActiveParticles = uint32_t(result - Level->Particles.Data());

	if (current != NO_PARTICLE) // More than one active particles
	{
		particle_t* next = &Level->Particles[current];
		next->tprev = Level->ActiveParticles;
	}
	else // Just one active particle
	{
		Level->OldestParticle = Level->ActiveParticles;
	}
	return result;
}

//
// [RH] Particle functions
//
void P_InitParticles (FLevelLocals *);

void P_InitParticles (FLevelLocals *Level)
{
	const char *i;
	int num;

	if ((i = Args->CheckValue ("-numparticles")))
		num = atoi (i);
	// [BC] Use r_maxparticles now.
	else
		num = r_maxparticles;

	// This should be good, but eh...
	int NumParticles = clamp<int>(num, 100, 65535);

	Level->Particles.Resize(NumParticles);
	Level->NakaraParticleGroups.Resize(NumParticles);
	P_ClearParticles (Level);
}

void P_ClearParticles (FLevelLocals *Level)
{
	int i = 0;
	if (Level->NakaraParticleGroups.Size() != Level->Particles.Size())
	{
		Level->NakaraParticleGroups.Resize(Level->Particles.Size());
	}
	for (auto &group : Level->NakaraParticleGroups)
	{
		group = NPG_None;
	}
	P_NakaraFishReset(Level);
	Level->OldestParticle = NO_PARTICLE;
	Level->ActiveParticles = NO_PARTICLE;
	Level->InactiveParticles = 0;
	for (auto &p : Level->Particles)
	{
		p = {};
		p.tprev = i - 1;
		p.tnext = ++i;
	}
	Level->Particles.Last().tnext = NO_PARTICLE;
	Level->Particles.Data()->tprev = NO_PARTICLE;
}

// Group particles by subsectors. Because particles are always
// in motion, there is little benefit to caching this information
// from one frame to the next.
// [MC] VisualThinkers hitches a ride here

void P_FindParticleSubsectors (FLevelLocals *Level)
{
	// [MC] Hitch a ride on particle subsectors since VisualThinkers are effectively using the same kind of system.
	for (uint32_t i = 0; i < Level->subsectors.Size(); i++)
	{
		Level->subsectors[i].sprites.Clear();
	}
	auto sp = Level->VisualThinkerHead;
	while (sp != nullptr)
	{
		if (!sp->PT.subsector) sp->PT.subsector = Level->PointInRenderSubsector(sp->PT.Pos);

		sp->PT.subsector->sprites.Push(sp);
		sp = sp->GetNext();
	}
	// End VisualThinker hitching. Now onto the particles. 
	if (Level->ParticlesInSubsec.Size() < Level->subsectors.Size())
	{
		Level->ParticlesInSubsec.Reserve (Level->subsectors.Size() - Level->ParticlesInSubsec.Size());
	}

	fillshort (&Level->ParticlesInSubsec[0], Level->subsectors.Size(), NO_PARTICLE);

	if (!r_particles)
	{
		return;
	}
	for (uint16_t i = Level->ActiveParticles; i != NO_PARTICLE; i = Level->Particles[i].tnext)
	{
		 // Try to reuse the subsector from the last portal check, if still valid.
		if (Level->Particles[i].subsector == nullptr) Level->Particles[i].subsector = Level->PointInRenderSubsector(Level->Particles[i].Pos);
		int ssnum = Level->Particles[i].subsector->Index();
		Level->Particles[i].snext = Level->ParticlesInSubsec[ssnum];
		Level->ParticlesInSubsec[ssnum] = i;
	}
}

static TMap<int, int> ColorSaver;

static uint32_t ParticleColor(int rgb)
{
	int *val;
	int stuff;

	val = ColorSaver.CheckKey(rgb);
	if (val != NULL)
	{
		return *val;
	}
	stuff = rgb | (ColorMatcher.Pick(RPART(rgb), GPART(rgb), BPART(rgb)) << 24);
	ColorSaver[rgb] = stuff;
	return stuff;
}

static uint32_t ParticleColor(int r, int g, int b)
{
	return ParticleColor(MAKERGB(r, g, b));
}

void P_InitEffects ()
{
	const struct ColorList *color = Colors;

	while (color->color)
	{
		*(color->color) = ParticleColor(color->r, color->g, color->b);
		color++;
	}

	int kind = gameinfo.defaultbloodparticlecolor;
	blood1 = ParticleColor(kind);
	blood2 = ParticleColor(RPART(kind)/3, GPART(kind)/3, BPART(kind)/3);
}

// [Nakara] Underwater ambient bubble LOD uses the same mod CVAR that the
// former BubbleFX Actor used through DistanceCheck "r_bubbledist". The CVAR
// can be supplied by CVARINFO, so resolve it dynamically instead of declaring
// a duplicate engine CVAR here.
static int P_NakaraBubbleLodDistance()
{
	FBaseCVar *cvar = FindCVar("r_bubbledist", nullptr);
	if (cvar == nullptr)
	{
		return -1;
	}
	return cvar->GetGenericRep(CVAR_Int).Int;
}

static AActor *P_NakaraBubbleLodViewer(FLevelLocals *Level)
{
	if (Level == nullptr)
	{
		return nullptr;
	}

	player_t *player = Level->GetConsolePlayer();
	if (player == nullptr)
	{
		return nullptr;
	}

	return player->camera != nullptr ? player->camera : player->mo;
}

static bool P_NakaraBubbleOutsideLod(const DVector3 &pos, AActor *viewer, int distance)
{
	if (viewer == nullptr || distance < 0)
	{
		return false;
	}

	const DVector3 delta = pos - viewer->Pos();
	const double distanceSquared = double(distance) * double(distance);
	return delta.LengthSquared() >= distanceSquared;
}

static int P_NakaraFishLodDistance()
{
	// Fish have their own large draw/simulation distance. A negative value keeps
	// them unlimited; zero hides them immediately. Default is 6500 map units.
	return int(r_fishdist);
}

static AActor *P_NakaraFishAvoidActor(FLevelLocals *Level)
{
	if (Level == nullptr) return nullptr;
	player_t *player = Level->GetConsolePlayer();
	if (player == nullptr) return nullptr;
	return player->mo != nullptr ? player->mo : player->camera;
}

static double P_NakaraFishRandomRange(double minValue, double maxValue)
{
	return minValue + (maxValue - minValue) * pr_nkfishschool.GenRand_Real2();
}

static DVector3 P_NakaraFishSafeUnit(const DVector3 &v, const DVector3 &fallback)
{
	const double len2 = v.LengthSquared();
	if (len2 <= 0.000001) return fallback;
	return v / sqrt(len2);
}

static DVector3 P_NakaraFishTurnToward(const DVector3 &from, const DVector3 &to, double maxRadians)
{
	const DVector3 fromDir = P_NakaraFishSafeUnit(from, DVector3(1.0, 0.0, 0.0));
	const DVector3 toDir = P_NakaraFishSafeUnit(to, fromDir);
	const double dot = clamp(fromDir | toDir, -1.0, 1.0);
	const double angle = acos(dot);
	if (angle <= maxRadians || angle <= 0.000001) return toDir;

	const double blend = clamp(maxRadians / angle, 0.0, 1.0);
	return P_NakaraFishSafeUnit(fromDir * (1.0 - blend) + toDir * blend, toDir);
}

// Return the navigable vertical interval around a fish center, including solid
// 3D floors. A fish above a solid 3D floor treats its top plane as the local
// floor; a fish below it treats the bottom plane as the local ceiling. If a
// center somehow starts inside a solid slab, choose the nearest side so the
// normal vertical steering pushes it back out instead of leaving it trapped.
static void P_NakaraFishVerticalBounds(sector_t *sec, const DVector2 &xy, double centerZ,
	double *outFloor, double *outCeiling)
{
	if (outFloor == nullptr || outCeiling == nullptr) return;
	if (sec == nullptr)
	{
		*outFloor = -FLT_MAX;
		*outCeiling = FLT_MAX;
		return;
	}

	double floorZ = sec->floorplane.ZatPoint(xy);
	double ceilZ = sec->ceilingplane.ZatPoint(xy);
	if (sec->e != nullptr)
	{
		for (auto rover : sec->e->XFloor.ffloors)
		{
			if (rover == nullptr ||
				(rover->flags & (FF_EXISTS | FF_SOLID)) != (FF_EXISTS | FF_SOLID)) continue;

			double bottomZ = rover->bottom.plane->ZatPoint(xy);
			double topZ = rover->top.plane->ZatPoint(xy);
			if (topZ < bottomZ)
			{
				const double swapZ = topZ;
				topZ = bottomZ;
				bottomZ = swapZ;
			}

			if (centerZ >= topZ)
			{
				floorZ = max(floorZ, topZ);
			}
			else if (centerZ <= bottomZ)
			{
				ceilZ = min(ceilZ, bottomZ);
			}
			else
			{
				// Center is inside the solid slab. Select the nearest escape side.
				if ((topZ - centerZ) <= (centerZ - bottomZ)) floorZ = max(floorZ, topZ);
				else ceilZ = min(ceilZ, bottomZ);
			}
		}
	}

	*outFloor = floorZ;
	*outCeiling = ceilZ;
}

// Predict solid 3D-floor planes along the fish's own heading. This handles the
// top/bottom faces of bridges, platforms and other solid extrafloors without
// making the particle itself solid. Lateral entry through a sector boundary is
// also caught by P_NakaraFishBlockingLine(), which now uses the same 3D-floor
// vertical bounds when evaluating a two-sided line opening.
static bool P_NakaraFish3DFloorGuide(FLevelLocals *Level, const DVector3 &pos,
	const DVector3 &forward, double lookAhead, double collisionRadius, DVector3 *outGuide)
{
	if (Level == nullptr || outGuide == nullptr) return false;
	const DVector3 forwardDir = P_NakaraFishSafeUnit(forward, DVector3(1.0, 0.0, 0.0));
	const double radius = max(1.0, collisionRadius);
	const double maxDistance = max(radius + 4.0, lookAhead);
	const double sampleDistance[4] =
	{
		max(radius + 4.0, maxDistance * 0.20),
		max(radius + 6.0, maxDistance * 0.45),
		max(radius + 8.0, maxDistance * 0.72),
		maxDistance
	};

	DVector3 guideSum(0.0, 0.0, 0.0);
	double guideWeight = 0.0;
	for (int sampleIndex = 0; sampleIndex < 4; ++sampleIndex)
	{
		const double distance = sampleDistance[sampleIndex];
		const DVector3 sample = pos + forwardDir * distance;
		sector_t *sec = Level->PointInSector(sample);
		if (sec == nullptr || sec->e == nullptr || sec->e->XFloor.ffloors.Size() == 0) continue;

		for (auto rover : sec->e->XFloor.ffloors)
		{
			if (rover == nullptr ||
				(rover->flags & (FF_EXISTS | FF_SOLID)) != (FF_EXISTS | FF_SOLID)) continue;

			double bottomZ = rover->bottom.plane->ZatPoint(sample.XY());
			double topZ = rover->top.plane->ZatPoint(sample.XY());
			if (topZ < bottomZ)
			{
				const double swapZ = topZ;
				topZ = bottomZ;
				bottomZ = swapZ;
			}

			// A small anticipatory shell starts the turn before the virtual body
			// actually touches the 3D floor. FF_SWIMMABLE-only water volumes are
			// ignored because the test above requires FF_SOLID.
			const double anticipation = clamp(radius * 0.28, 3.0, 24.0);
			const double bodyBottom = sample.Z - radius;
			const double bodyTop = sample.Z + radius;
			if (bodyTop < bottomZ - anticipation || bodyBottom > topZ + anticipation) continue;

			int verticalSign;
			if (pos.Z - radius >= topZ - anticipation) verticalSign = 1;
			else if (pos.Z + radius <= bottomZ + anticipation) verticalSign = -1;
			else if (forwardDir.Z > 0.10) verticalSign = 1;
			else if (forwardDir.Z < -0.10) verticalSign = -1;
			else
			{
				const double upCost = fabs((topZ + radius + 2.0) - sample.Z);
				const double downCost = fabs(sample.Z - (bottomZ - radius - 2.0));
				verticalSign = upCost <= downCost ? 1 : -1;
			}

			const DVector3 verticalGuide = P_NakaraFishSafeUnit(
				DVector3(forwardDir.X * 0.58, forwardDir.Y * 0.58, double(verticalSign) * 1.35),
				DVector3(0.0, 0.0, double(verticalSign)));
			const double weight = 1.0 + (1.0 - clamp(distance / maxDistance, 0.0, 1.0)) * 1.25;
			guideSum += verticalGuide * weight;
			guideWeight += weight;
		}
	}

	if (guideWeight <= 0.0) return false;
	*outGuide = P_NakaraFishSafeUnit(guideSum / guideWeight, forwardDir);
	return true;
}

static bool P_NakaraFishBlockingLine(FLevelLocals *Level, const DVector3 &pos,
	const DVector3 &vel, double lookAhead, line_t **hitLine, double verticalClearance = 8.0)
{
	if (Level == nullptr || hitLine == nullptr) return false;
	const DVector2 start(pos.X, pos.Y);
	const DVector2 end(pos.X + vel.X * lookAhead, pos.Y + vel.Y * lookAhead);
	if ((end - start).LengthSquared() < 0.0001) return false;

	FPathTraverse trace(Level, start.X, start.Y, end.X, end.Y, PT_ADDLINES);
	intercept_t *in;
	while ((in = trace.Next()) != nullptr)
	{
		if (!in->isaline) continue;
		line_t *line = in->d.line;
		if (line == nullptr) continue;
		if (line->isLinePortal()) continue;
		if (!(line->flags & ML_TWOSIDED) || line->backsector == nullptr || (line->flags & ML_BLOCKING))
		{
			*hitLine = line;
			return true;
		}

		// A nominally two-sided line can still be vertically closed at the
		// fish's Z (door, height change, solid 3D floor, etc.). Evaluate the
		// predicted Z at this intercept and include each side's solid extrafloors
		// in the available opening. This is what stops a fish entering the side
		// of a solid 3D-floor bridge through an otherwise open two-sided line.
		const DVector2 hit = trace.InterceptPoint(in);
		const double hitZ = pos.Z + vel.Z * lookAhead * in->frac;
		double frontFloor, frontCeil, backFloor, backCeil;
		P_NakaraFishVerticalBounds(line->frontsector, hit, hitZ, &frontFloor, &frontCeil);
		P_NakaraFishVerticalBounds(line->backsector, hit, hitZ, &backFloor, &backCeil);
		const double floorZ = max(frontFloor, backFloor);
		const double ceilZ = min(frontCeil, backCeil);
		const double clearance = max(0.0, verticalClearance);
		if (hitZ <= floorZ + clearance || hitZ >= ceilZ - clearance)
		{
			*hitLine = line;
			return true;
		}
	}
	return false;
}

// Build a smooth along-wall steering direction for a fish (or its lightweight
// flock guide). The tangent is chosen to preserve as much of the incoming
// heading as possible, while a small outward component keeps the particle from
// repeatedly steering back into the same line.
static DVector3 P_NakaraFishWallGuide(line_t *line, const DVector3 &pos,
	const DVector3 &forward, double phase, double outwardWeight = 0.30)
{
	if (line == nullptr) return P_NakaraFishSafeUnit(forward, DVector3(1.0, 0.0, 0.0));

	DVector2 tangent(line->Delta().X, line->Delta().Y);
	const double tangentLen = tangent.Length();
	if (tangentLen <= 0.0001) return P_NakaraFishSafeUnit(forward, DVector3(1.0, 0.0, 0.0));
	tangent /= tangentLen;

	DVector2 forwardXY(forward.X, forward.Y);
	const double forwardXYLen = forwardXY.Length();
	if (forwardXYLen > 0.0001) forwardXY /= forwardXYLen;
	else forwardXY = tangent;

	double along = forwardXY | tangent;
	if (fabs(along) < 0.08)
	{
		// Nearly head-on encounters can legitimately choose either side. A stable
		// per-fish phase avoids every member making the same turn at once.
		if (sin(phase * 1.731) < 0.0) tangent = -tangent;
	}
	else if (along < 0.0)
	{
		tangent = -tangent;
	}

	DVector2 outward(tangent.Y, -tangent.X);
	const DVector2 relative(pos.X - line->v1->fX(), pos.Y - line->v1->fY());
	if ((relative | outward) < 0.0) outward = -outward;

	DVector3 guide(tangent.X + outward.X * outwardWeight,
		tangent.Y + outward.Y * outwardWeight, forward.Z * 0.35);
	return P_NakaraFishSafeUnit(guide, forward);
}

// Treat each fish as a small virtual sphere for AI wall avoidance. The particle
// itself remains non-solid; these probes only keep the visible sprite plane away
// from map geometry. A forward probe predicts the next turn while two short side
// probes maintain body clearance when a fish is swimming almost parallel to a wall.
static bool P_NakaraFishClearanceGuide(FLevelLocals *Level, const DVector3 &pos,
	const DVector3 &forward, double lookAhead, double collisionRadius, double phase,
	const DVector3 &steerPreference, DVector3 *outGuide, line_t **firstHit)
{
	if (Level == nullptr || outGuide == nullptr) return false;
	const DVector3 forwardDir = P_NakaraFishSafeUnit(forward, DVector3(1.0, 0.0, 0.0));
	// Use the FlockID guide as the tangent preference while still probing from the
	// individual fish heading. When a whole flock meets a wall head-on, this keeps
	// members choosing the same side instead of splitting according to per-fish phase.
	const DVector3 preferredDir = P_NakaraFishSafeUnit(steerPreference, forwardDir);
	const double radius = max(1.0, collisionRadius);

	DVector3 side(-forwardDir.Y, forwardDir.X, 0.0);
	side = P_NakaraFishSafeUnit(side, DVector3(0.0, 1.0, 0.0));
	DVector3 guideSum(0.0, 0.0, 0.0);
	double guideWeight = 0.0;
	line_t *first = nullptr;

	auto AddProbe = [&](const DVector3 &probeDir, double probeDistance, double weight, double outwardWeight)
	{
		line_t *line = nullptr;
		if (!P_NakaraFishBlockingLine(Level, pos, probeDir, probeDistance, &line, radius)) return;
		if (first == nullptr) first = line;
		guideSum += P_NakaraFishWallGuide(line, pos, preferredDir, phase, outwardWeight) * weight;
		guideWeight += weight;
	};

	// The body radius is added to the normal look-ahead distance, so the center
	// starts its turn before the nose of the sprite reaches the wall.
	AddProbe(forwardDir, lookAhead + radius, 1.55, 0.42);

	// Short lateral feelers are what prevent the flat sprite from clipping into a
	// wall while travelling alongside it. Keep them only slightly larger than the
	// requested body radius so corridors do not feel artificially narrow.
	const double sideDistance = radius + max(2.0, radius * 0.10);
	AddProbe(side, sideDistance, 1.05, 0.62);
	AddProbe(-side, sideDistance, 1.05, 0.62);

	if (guideWeight <= 0.0) return false;
	*outGuide = P_NakaraFishSafeUnit(guideSum / guideWeight, forwardDir);
	if (firstHit != nullptr) *firstHit = first;
	return true;
}

// The detailed boid steering is distance-LOD'd, but particles still integrate
// their velocity every tic. This tiny guard therefore runs every visible fish
// tic and prevents a fast/distant fish from crossing a wall between AI updates.
static void P_NakaraFishImmediateWallGuard(FLevelLocals *Level, particle_t *particle,
	FNakaraFishLevelState *state, int particleIndex)
{
	if (Level == nullptr || particle == nullptr || state == nullptr ||
		particleIndex < 0 || (unsigned)particleIndex >= state->Fish.Size()) return;

	auto &fish = state->Fish[particleIndex];
	if (fish.School == NKFISH_NO_SCHOOL || (unsigned)fish.School >= state->Schools.Size()) return;
	const auto &school = state->Schools[fish.School];
	const int flockCount = clamp<int>(int(school.FlockCount), 1, NKFISH_MAX_FLOCK_COUNT);
	const auto &flock = school.Flocks[min<int>(fish.Flock, flockCount - 1)];
	const double collisionRadius = max(1.0, fish.CollisionRadius > 0.0 ? fish.CollisionRadius : school.CollisionRadius);

	DVector3 vel(particle->Vel);
	const double speed = sqrt(max(0.0, vel.LengthSquared()));
	if (speed <= 0.0001) return;

	line_t *hitLine = nullptr;
	// Add one body radius to the normal 1.35-tic prediction. This is the cheap
	// every-tic guard that prevents a fast fish nose from tunnelling into a wall
	// between the more detailed AI updates. Solid 3D-floor planes get the same
	// every-tic treatment so a high flee multiplier cannot tunnel through them.
	const double guardTics = 1.35 + collisionRadius / max(0.0001, speed);
	DVector3 safeDir = P_NakaraFishSafeUnit(vel, DVector3(1.0, 0.0, 0.0));
	bool redirected = false;
	if (P_NakaraFishBlockingLine(Level, particle->Pos, vel, guardTics, &hitLine, collisionRadius))
	{
		const DVector3 flockPreference = P_NakaraFishSafeUnit(flock.Velocity, safeDir);
		const DVector3 guide = P_NakaraFishWallGuide(hitLine, particle->Pos, flockPreference,
			double(flock.Phase), 0.52);
		safeDir = P_NakaraFishSafeUnit(safeDir * 0.06 + guide * 1.94, guide);
		redirected = true;
	}

	DVector3 floorGuide;
	const double floorPrediction = speed * guardTics + collisionRadius;
	if (P_NakaraFish3DFloorGuide(Level, particle->Pos, safeDir, floorPrediction,
		collisionRadius, &floorGuide))
	{
		safeDir = P_NakaraFishSafeUnit(safeDir * 0.16 + floorGuide * 1.84, floorGuide);
		redirected = true;
	}

	if (redirected)
	{
		particle->Vel = FVector3(safeDir * speed * 0.96);
		// Keep a short memory of the detour. The detailed AI then gets an aggressive
		// curved catch-up turn budget after clearing the obstacle instead of drifting
		// away while the flock has already continued around the wall.
		fish.ObstacleRecoveryUntilTic = max(fish.ObstacleRecoveryUntilTic, Level->maptime + 42);
	}
}

static void P_NakaraFishPrepareSchools(FLevelLocals *Level, FNakaraFishLevelState *state)
{
	if (Level == nullptr || state == nullptr || state->Schools.Size() == 0) return;

	// Lightweight FlockID agents reproduce the old ZScript's broad group
	// flow without creating Actor/Thinker objects. Each requested species frame
	// owns one independent flock, up to the A-Z sprite-frame range. The flock agent is a heading
	// guide, not a position anchor: actual fish averages are allowed to pull it
	// along so the visible group moves as one body instead of occupying fixed
	// slots around the spawner.
	for (auto &school : state->Schools)
	{
		const double t = double(Level->maptime) * 0.010;
		const double p = double(school.Phase);

		// The school origin only defines the broad allowed territory. It drifts a
		// little so long-running groups do not repeatedly trace the exact same box.
		school.Center = school.Origin + DVector3(
			sin(t * 0.21 + p) * double(school.Radius) * 0.04,
			cos(t * 0.17 + p * 1.37) * double(school.Radius) * 0.04,
			(sin(t * 0.13 + p * 0.73) * 0.045 + cos(t * 0.09 + p * 1.91) * 0.025) * double(school.Height));

		const int flockCount = clamp<int>(int(school.FlockCount), 1, NKFISH_MAX_FLOCK_COUNT);
		DVector3 sharedFlow(0.0, 0.0, 0.0);
		for (int g = 0; g < flockCount; ++g)
		{
			sharedFlow += P_NakaraFishSafeUnit(school.Flocks[g].Velocity, DVector3(1.0, 0.0, 0.0));
		}
		sharedFlow /= double(flockCount);

		const double flockTravelRadius = max(96.0, double(school.Radius) * 0.92);
		const double flockTravelHeight = max(48.0, double(school.Height) * 0.82);
		const double flockSepRadius = max(96.0, min(900.0, double(school.Radius) * 0.16));

		for (int g = 0; g < flockCount; ++g)
		{
			auto &flock = school.Flocks[g];

			// Let the guide follow where its fish actually are. This is deliberately
			// gentle; it prevents a fixed-position "plant bed" feel while preserving
			// a coherent group flow.
			if (flock.Count > 0)
			{
				flock.Center = flock.Center * 0.90 + flock.AveragePos * 0.10;
			}

			const double speedFactor = g == 0 ? (12.0 / 14.0) : (g == 2 ? (16.0 / 14.0) : 1.0);
			const double flockSpeed = max(0.04, double(school.BaseSpeed) * 0.78 * speedFactor);

			DVector3 toTarget = flock.Target - flock.Center;
			const double reach = max(48.0, min(320.0, double(school.Radius) * 0.055));
			if (flock.NextTargetTic <= Level->maptime || toTarget.LengthSquared() < reach * reach)
			{
				const double a = P_NakaraFishRandomRange(0.0, 6.283185307179586);
				const double r = sqrt(pr_nkfishschool.GenRand_Real2()) * flockTravelRadius;
				flock.Target = school.Center + DVector3(cos(a) * r, sin(a) * r,
					P_NakaraFishRandomRange(-flockTravelHeight, flockTravelHeight));
				// Long targets give each flock a recognizable travel direction rather
				// than constantly choosing nearby positions.
				flock.NextTargetTic = Level->maptime + 175 + int(pr_nkfishschool(281));
				toTarget = flock.Target - flock.Center;
			}

			const DVector3 currentDir = P_NakaraFishSafeUnit(flock.Velocity,
				DVector3(cos(double(flock.Phase)), sin(double(flock.Phase)), 0.0));
			DVector3 desired = currentDir * 0.90;
			desired += P_NakaraFishSafeUnit(toTarget, currentDir) * 0.72;
			// Feed the previous frame's real fish velocity back into the group agent.
			// This makes the flock a moving consensus rather than a leader pulling
			// fish toward a marker.
			if (flock.Count > 0 && flock.AverageVel.LengthSquared() > 0.0001)
			{
				desired += P_NakaraFishSafeUnit(flock.AverageVel, currentDir) * 0.58;
			}
			desired += P_NakaraFishSafeUnit(sharedFlow, currentDir) * 0.035;

			// Flock-to-flock separation prevents species groups from permanently
			// stacking. They can still cross through each other.
			for (int other = 0; other < flockCount; ++other)
			{
				if (other == g) continue;
				DVector3 apart = flock.Center - school.Flocks[other].Center;
				const double d2 = apart.LengthSquared();
				if (d2 > 0.0001 && d2 < flockSepRadius * flockSepRadius)
				{
					const double d = sqrt(d2);
					const double x = 1.0 - d / flockSepRadius;
					desired += (apart / d) * (0.18 + x * 0.62);
				}
			}

			// Only the broad activity territory is anchored to the spawner. Inside it
			// the flock is free to travel continuously.
			DVector3 fromSchool = flock.Center - school.Center;
			const double schoolDist = sqrt(max(0.0, fromSchool.LengthSquared()));
			const double leashStart = max(128.0, double(school.Radius) * 0.94);
			if (schoolDist > leashStart)
			{
				const double leash = clamp((schoolDist - leashStart) /
					max(96.0, double(school.Radius) * 0.20), 0.0, 1.0);
				desired += P_NakaraFishSafeUnit(-fromSchool, currentDir) * (0.35 + leash * 1.75);
			}

			desired = P_NakaraFishSafeUnit(desired, currentDir);
			const DVector3 desiredVel = desired * flockSpeed;
			// Slow group steering produces long, readable arcs like the old Boids.
			flock.Velocity = flock.Velocity * 0.972 + desiredVel * 0.028;

			// Flock guides also respect walls. Probe by map distance rather than by
			// velocity * an arbitrary tic count, then flow along the blocking surface.
			line_t *hitLine = nullptr;
			const DVector3 flockDirNow = P_NakaraFishSafeUnit(flock.Velocity, currentDir);
			const double flockLookAhead = clamp(128.0 + flockSpeed * 18.0, 128.0, 420.0);
			if (P_NakaraFishBlockingLine(Level, flock.Center, flockDirNow,
				flockLookAhead, &hitLine))
			{
				const DVector3 wallGuide = P_NakaraFishWallGuide(hitLine, flock.Center,
					flockDirNow, double(flock.Phase), 0.28);
				const double wallTurn = 11.0 * (M_PI / 180.0);
				const DVector3 diverted = P_NakaraFishTurnToward(flockDirNow, wallGuide, wallTurn);
				flock.Velocity = diverted * flockSpeed;
				flock.Target = flock.Center + wallGuide * min(720.0, max(220.0, double(school.Radius) * 0.18));
				flock.NextTargetTic = Level->maptime + 70 + int(pr_nkfishschool(106));
			}

			// The lightweight flock guide also sees solid 3D-floor planes. Otherwise
			// individual fish would correctly avoid a bridge while their invisible
			// group target kept pulling them through it.
			DVector3 flock3DGuide;
			const DVector3 flockDirAfterWall = P_NakaraFishSafeUnit(flock.Velocity, flockDirNow);
			if (P_NakaraFish3DFloorGuide(Level, flock.Center, flockDirAfterWall,
				flockLookAhead, 20.0, &flock3DGuide))
			{
				const double floorTurn = 11.0 * (M_PI / 180.0);
				const DVector3 diverted = P_NakaraFishTurnToward(flockDirAfterWall, flock3DGuide, floorTurn);
				flock.Velocity = diverted * flockSpeed;
				flock.Target = flock.Center + flock3DGuide * min(720.0, max(220.0, double(school.Radius) * 0.18));
				flock.NextTargetTic = Level->maptime + 70 + int(pr_nkfishschool(106));
			}

			// Hard one-tic guard: the guide itself must never cross a blocking line.
			line_t *immediateLine = nullptr;
			if (P_NakaraFishBlockingLine(Level, flock.Center, flock.Velocity, 1.25, &immediateLine))
			{
				const DVector3 immediateGuide = P_NakaraFishWallGuide(immediateLine, flock.Center,
					P_NakaraFishSafeUnit(flock.Velocity, currentDir), double(flock.Phase), 0.46);
				flock.Velocity = immediateGuide * flockSpeed;
				flock.Target = flock.Center + immediateGuide * 320.0;
			}

			DVector3 immediate3DGuide;
			const DVector3 immediate3DDir = P_NakaraFishSafeUnit(flock.Velocity, currentDir);
			if (P_NakaraFish3DFloorGuide(Level, flock.Center, immediate3DDir,
				flockSpeed * 1.25 + 20.0, 20.0, &immediate3DGuide))
			{
				flock.Velocity = P_NakaraFishSafeUnit(immediate3DDir * 0.12 + immediate3DGuide * 1.88,
					immediate3DGuide) * flockSpeed;
				flock.Target = flock.Center + immediate3DGuide * 320.0;
			}

			sector_t *flockSector = Level->PointInSector(flock.Center);
			if (flockSector != nullptr)
			{
				double floorZ, ceilZ;
				P_NakaraFishVerticalBounds(flockSector, flock.Center.XY(), flock.Center.Z, &floorZ, &ceilZ);
				floorZ += 20.0;
				ceilZ -= 20.0;
				if (ceilZ > floorZ)
				{
					if (flock.Center.Z < floorZ + 48.0) flock.Velocity.Z = max(flock.Velocity.Z, 0.16);
					if (flock.Center.Z > ceilZ - 48.0) flock.Velocity.Z = min(flock.Velocity.Z, -0.16);
				}
			}

			flock.Center += flock.Velocity;
			flock.AveragePos = DVector3();
			flock.AverageVel = DVector3();
			flock.Count = 0;
		}

		school.AveragePos = DVector3();
		school.AverageVel = DVector3();
		school.Count = 0;
	}

	// One linear pass computes all active FlockID averages. This replaces the old
	// per-Actor neighbor iteration and keeps the flocking cost O(N).
	int particleIndex = Level->ActiveParticles;
	while (particleIndex != NO_PARTICLE)
	{
		particle_t *particle = &Level->Particles[particleIndex];
		const int next = particle->tnext;
		if ((unsigned)particleIndex < Level->NakaraParticleGroups.Size() &&
			Level->NakaraParticleGroups[particleIndex] == NPG_UnderwaterFish &&
			(unsigned)particleIndex < state->Fish.Size())
		{
			auto &fish = state->Fish[particleIndex];
			if (fish.School != NKFISH_NO_SCHOOL && (unsigned)fish.School < state->Schools.Size())
			{
				auto &school = state->Schools[fish.School];
				const int flockCount = clamp<int>(int(school.FlockCount), 1, NKFISH_MAX_FLOCK_COUNT);
				const int flockIndex = min<int>(fish.Flock, flockCount - 1);
				auto &flock = school.Flocks[flockIndex];
				flock.AveragePos += particle->Pos;
				flock.AverageVel += DVector3(particle->Vel);
				++flock.Count;
				school.AveragePos += particle->Pos;
				school.AverageVel += DVector3(particle->Vel);
				++school.Count;
			}
		}
		particleIndex = next;
	}

	for (auto &school : state->Schools)
	{
		const int flockCount = clamp<int>(int(school.FlockCount), 1, NKFISH_MAX_FLOCK_COUNT);
		for (int g = 0; g < flockCount; ++g)
		{
			auto &flock = school.Flocks[g];
			if (flock.Count > 0)
			{
				flock.AveragePos /= double(flock.Count);
				flock.AverageVel /= double(flock.Count);
			}
			else
			{
				flock.AveragePos = flock.Center;
				flock.AverageVel = flock.Velocity;
			}
		}

		if (school.Count > 0)
		{
			school.AveragePos /= double(school.Count);
			school.AverageVel /= double(school.Count);
		}
		else
		{
			school.AveragePos = school.Center;
			school.AverageVel = school.Flocks[0].Velocity;
		}
	}
}

static int P_NakaraFishUpdateInterval(const DVector3 &pos, AActor *viewer, int lodDistance)
{
	if (viewer == nullptr) return 2;
	const double d2 = (pos - viewer->Pos()).LengthSquared();
	if (lodDistance > 0)
	{
		const double nearDist = double(lodDistance) * 0.25;
		const double midDist = double(lodDistance) * 0.50;
		const double farDist = double(lodDistance) * 0.75;
		if (d2 >= double(lodDistance) * double(lodDistance)) return 16;
		if (d2 < nearDist * nearDist) return 1;
		if (d2 < midDist * midDist) return 2;
		if (d2 < farDist * farDist) return 4;
		return 8;
	}

	// Unlimited draw distance still keeps a conservative simulation LOD.
	if (d2 < 512.0 * 512.0) return 1;
	if (d2 < 1024.0 * 1024.0) return 2;
	if (d2 < 1536.0 * 1536.0) return 4;
	return 8;
}

static void P_NakaraUpdateFishParticle(FLevelLocals *Level, particle_t *particle,
	int particleIndex, FNakaraFishLevelState *state, AActor *viewer, AActor *avoidActor, int lodDistance)
{
	if (Level == nullptr || particle == nullptr || state == nullptr ||
		particleIndex < 0 || (unsigned)particleIndex >= state->Fish.Size()) return;

	auto &fish = state->Fish[particleIndex];
	if (fish.School == NKFISH_NO_SCHOOL || (unsigned)fish.School >= state->Schools.Size()) return;
	auto &school = state->Schools[fish.School];
	const int flockCount = clamp<int>(int(school.FlockCount), 1, NKFISH_MAX_FLOCK_COUNT);
	const int flockIndex = min<int>(fish.Flock, flockCount - 1);
	auto &flock = school.Flocks[flockIndex];
	const double cohesionScale = clamp(double(school.CohesionScale), 0.25, 10.0);
	const double normalAffinity = clamp(double(fish.FlockAffinity), 0.72, 1.20);
	const double collisionRadius = max(1.0, fish.CollisionRadius > 0.0 ? fish.CollisionRadius : school.CollisionRadius);

	const double viewerDist2 = viewer != nullptr ? (particle->Pos - viewer->Pos()).LengthSquared() : 0.0;
	const bool outsideLod = viewer != nullptr && lodDistance >= 0 &&
		viewerDist2 >= double(lodDistance) * double(lodDistance);
	particle->alpha = outsideLod ? 0.f : 1.f;
	if (outsideLod) return;

	const int interval = P_NakaraFishUpdateInterval(particle->Pos, viewer, lodDistance);
	if (((Level->maptime + fish.UpdatePhase) % interval) != 0) return;

	DVector3 vel(particle->Vel);
	const DVector3 currentDir = P_NakaraFishSafeUnit(vel, DVector3(1.0, 0.0, 0.0));
	const double phase = double(fish.Phase);

	// Curiosity is now a temporary behavior instead of a permanent low-affinity
	// personality. Small flocks (10 members or fewer) never start a curiosity
	// excursion, because losing even one or two members makes those groups look
	// unintentionally broken apart. Larger flocks occasionally let one member peel
	// off for 2..4 seconds, then that fish is gently guided back into its FlockID.
	const bool curiosityAllowed = flock.Count > 10;
	if (!curiosityAllowed && fish.CuriosityUntilTic > 0)
	{
		fish.CuriosityUntilTic = 0;
		fish.RejoinAfterCuriosity = 1;
	}

	if (fish.CuriosityUntilTic > 0 && fish.CuriosityUntilTic <= Level->maptime)
	{
		fish.CuriosityUntilTic = 0;
		fish.RejoinAfterCuriosity = 1;
	}

	const bool canStartCuriosity = curiosityAllowed && fish.CuriosityUntilTic <= 0 &&
		fish.RejoinAfterCuriosity == 0 && fish.RejoinAfterFlee == 0 &&
		fish.FleeUntilTic <= Level->maptime;
	if (canStartCuriosity && fish.NextCuriosityTic <= Level->maptime)
	{
		// Trial every 8..16 seconds, with a 1-in-5 activation chance. At 40 fish
		// per flock this normally leaves only about one or two curious members out
		// at once instead of permanently assigning several stragglers.
		if (pr_nkfishschool(5) == 0)
		{
			const DVector3 baseFlow = P_NakaraFishSafeUnit(flock.AverageVel,
				P_NakaraFishSafeUnit(flock.Velocity, currentDir));
			DVector3 side(-baseFlow.Y, baseFlow.X, 0.0);
			side = P_NakaraFishSafeUnit(side, DVector3(0.0, 1.0, 0.0));
			const double sideSign = pr_nkfishschool(2) == 0 ? -1.0 : 1.0;
			const double yaw = P_NakaraFishRandomRange(0.62, 1.08) * sideSign;
			const double z = P_NakaraFishRandomRange(-0.28, 0.28);
			fish.CuriosityDirection = P_NakaraFishSafeUnit(
				baseFlow * cos(yaw) + side * sin(yaw) + DVector3(0.0, 0.0, z), baseFlow);
			fish.CuriosityUntilTic = Level->maptime + 70 + int(pr_nkfishschool(71));
			fish.NextCuriosityTic = fish.CuriosityUntilTic + 280 + int(pr_nkfishschool(281));
		}
		else
		{
			fish.NextCuriosityTic = Level->maptime + 280 + int(pr_nkfishschool(281));
		}
	}

	const bool curiosityActive = curiosityAllowed && fish.CuriosityUntilTic > Level->maptime &&
		fish.RejoinAfterFlee == 0 && fish.FleeUntilTic <= Level->maptime;
	const double affinity = curiosityActive ? clamp(normalAffinity * 0.68, 0.58, 0.76) : normalAffinity;

	// Persistent personal flow replaces the old position waypoint. Each fish has
	// a small independent heading that changes only every few seconds, while the
	// flock's average velocity remains the dominant signal. This mirrors the old
	// ZScript: members generally travel together, with occasional loose stragglers.
	if (fish.NextRoamTic <= Level->maptime || fish.RoamOffset.LengthSquared() < 0.0001)
	{
		const double yaw = P_NakaraFishRandomRange(-0.78, 0.78);
		const double z = P_NakaraFishRandomRange(-0.38, 0.38);
		const DVector3 baseFlow = P_NakaraFishSafeUnit(flock.AverageVel,
			P_NakaraFishSafeUnit(flock.Velocity, currentDir));
		DVector3 side(-baseFlow.Y, baseFlow.X, 0.0);
		side = P_NakaraFishSafeUnit(side, DVector3(0.0, 1.0, 0.0));
		DVector3 personal = baseFlow * cos(yaw) + side * sin(yaw) + DVector3(0.0, 0.0, z);
		fish.RoamOffset = P_NakaraFishSafeUnit(personal, currentDir);
		fish.NextRoamTic = Level->maptime + 105 + int(pr_nkfishschool(211));
	}

	const DVector3 flockGuide = P_NakaraFishSafeUnit(flock.Velocity, currentDir);
	const DVector3 flockAverage = P_NakaraFishSafeUnit(flock.AverageVel, flockGuide);
	DVector3 desired = currentDir * 0.48;
	// Directional alignment is now the core of cohesion.
	desired += flockGuide * (0.58 + 0.20 * cohesionScale) * affinity;
	desired += flockAverage * (0.72 + 0.34 * cohesionScale) * affinity;
	// The whole-school average is only a faint environmental current.
	desired += P_NakaraFishSafeUnit(school.AverageVel, flockGuide) * 0.025;

	// Position cohesion only activates meaningfully when a member has drifted away
	// from its FlockID average. Nearby fish are free to occupy different positions.
	DVector3 toFlock = flock.AveragePos - particle->Pos;
	const double flockDist = sqrt(max(0.0, toFlock.LengthSquared()));
	// High cohesion used to saturate early because the minimum return radius was
	// clamped to 260 map units. At 6x and 10x that produced almost the same
	// behavior. Let strong schools keep progressively tighter formation instead.
	const double rejoinStart = clamp(620.0 / sqrt(cohesionScale), 140.0, 980.0);
	const double rejoinFull = max(rejoinStart + 150.0, 1180.0 / sqrt(cohesionScale));
	const double flockCatchupUrgency = flockDist > rejoinStart ?
		clamp((flockDist - rejoinStart) / max(140.0, rejoinFull - rejoinStart), 0.0, 1.0) : 0.0;
	if (flockDist > rejoinStart)
	{
		desired += P_NakaraFishSafeUnit(toFlock, currentDir) *
			((0.18 + flockCatchupUrgency * 1.55) * cohesionScale * affinity);
	}

	// Ordinary members keep only a subtle personal heading. During a temporary
	// curiosity excursion the stored side-heading becomes the dominant personal
	// signal, then disappears again when the event ends.
	const double wanderWeight = 0.10 + (1.15 - affinity) * 0.42;
	desired += P_NakaraFishSafeUnit(fish.RoamOffset, currentDir) * wanderWeight;
	if (curiosityActive)
	{
		desired += P_NakaraFishSafeUnit(fish.CuriosityDirection, currentDir) * 0.78;
	}

	// Tiny phase-based 3D noise prevents a mathematically perfect formation without
	// turning the fish into stationary sine-orbit decorations.
	DVector3 microWander(
		cos(double(Level->maptime) * 0.011 + phase * 1.73),
		sin(double(Level->maptime) * 0.009 + phase * 1.19),
		sin(double(Level->maptime) * 0.007 + phase * 2.11) * 0.55);
	desired += P_NakaraFishSafeUnit(microWander, currentDir) * (0.035 + (1.0 - affinity) * 0.055);

	// The spawner radius is only the broad regional territory. It no longer scales
	// the internal size of a flock. This leash is an emergency return only.
	const DVector3 fromSchool = particle->Pos - school.Center;
	const double schoolDist = sqrt(max(0.0, fromSchool.LengthSquared()));
	const double territoryStart = max(256.0, double(school.Radius) * 0.96);
	if (schoolDist > territoryStart)
	{
		const double x = clamp((schoolDist - territoryStart) /
			max(128.0, double(school.Radius) * 0.18), 0.0, 1.0);
		desired += P_NakaraFishSafeUnit(-fromSchool, currentDir) * (0.42 + x * 1.85);
	}

	// Player avoidance uses the real player Actor rather than the render camera.
	// Once startled, a fish keeps its full burst speed for FleeHoldSeconds after
	// the last threat contact. This prevents the old immediate "calm down" as soon
	// as the fish barely crossed AvoidRadius. The last escape heading is retained
	// during the hold so it continues to clear the danger instead of snapping back
	// toward flock cohesion.
	const double fleeScale = clamp(school.FleeSpeedScale, 1.0, 10.0);
	double avoidSpeedBoost = 1.0;
	bool fleeTriggeredThisUpdate = false;
	if (avoidActor != nullptr)
	{
		const double avoidRadius = max(32.0, double(school.AvoidRadius));
		DVector3 playerVel(avoidActor->Vel);
		DVector3 playerForward(0.0, 0.0, 0.0);
		const double playerSpeed2 = playerVel.LengthSquared();
		const bool playerMoving = playerSpeed2 > 0.01;
		if (playerMoving)
		{
			// Fish avoidance must react to the player's physical travel direction,
			// never the render/view yaw. Mouse-look changes Actor.Angles.Yaw even while
			// standing still, which previously rotated the split steering every frame
			// and could drive an entire nearby flock away just by moving the mouse.
			playerForward = P_NakaraFishSafeUnit(playerVel, DVector3(1.0, 0.0, 0.0));
		}

		DVector3 threatPos = avoidActor->Pos();
		threatPos.Z += double(avoidActor->Height) * 0.50;
		if (playerMoving)
		{
			const double predict = min(avoidRadius * 0.42, sqrt(playerSpeed2) * 10.0);
			threatPos += playerForward * predict;
		}

		DVector3 away = particle->Pos - threatPos;
		const double dist2 = away.LengthSquared();
		if (dist2 < avoidRadius * avoidRadius && dist2 > 0.0001)
		{
			const double dist = sqrt(dist2);
			const double x = clamp(1.0 - dist / avoidRadius, 0.0, 1.0);
			const DVector3 awayDir = away / dist;
			desired += awayDir * (1.75 + x * x * 4.25);

			DVector3 right;
			double sideSign;
			if (playerMoving)
			{
				// Moving player: split around the actual movement path.
				DVector3 horizontalForward(playerForward.X, playerForward.Y, 0.0);
				DVector3 horizontalAway(away.X, away.Y, 0.0);
				horizontalForward = P_NakaraFishSafeUnit(horizontalForward,
					P_NakaraFishSafeUnit(horizontalAway, DVector3(1.0, 0.0, 0.0)));
				right = DVector3(-horizontalForward.Y, horizontalForward.X, 0.0);
				sideSign = (away | right) >= 0.0 ? 1.0 : -1.0;
				if (fabs(away | right) < 2.0) sideSign = sin(phase * 3.17) >= 0.0 ? 1.0 : -1.0;
			}
			else
			{
				// Stationary player: use the fish-to-player radial geometry only.
				// This makes mouse-look completely irrelevant to fish simulation.
				DVector3 horizontalAway(away.X, away.Y, 0.0);
				horizontalAway = P_NakaraFishSafeUnit(horizontalAway, DVector3(1.0, 0.0, 0.0));
				right = DVector3(-horizontalAway.Y, horizontalAway.X, 0.0);
				sideSign = sin(phase * 3.17) >= 0.0 ? 1.0 : -1.0;
			}
			double verticalSign = away.Z >= 0.0 ? 1.0 : -1.0;
			if (fabs(away.Z) < 4.0) verticalSign = cos(phase * 2.71) >= 0.0 ? 1.0 : -1.0;
			DVector3 split = right * sideSign + DVector3(0.0, 0.0, verticalSign * 0.72);
			const DVector3 splitDir = P_NakaraFishSafeUnit(split, awayDir);
			desired += splitDir * (x * x * 3.1);

			// Remember a stable escape heading for the post-contact burst. The radial
			// component dominates while the split component keeps neighboring fish from
			// stacking on exactly the same path.
			fish.FleeDirection = P_NakaraFishSafeUnit(awayDir * 1.35 + splitDir * 0.65, awayDir);
			const double holdSeconds = clamp(school.FleeHoldSeconds, 0.0, 10.0);
			const int holdTics = int(holdSeconds * 35.0 + 0.5);
			if (holdTics > 0)
				fish.FleeUntilTic = max(fish.FleeUntilTic, Level->maptime + holdTics);
			// Player panic always overrides a curiosity excursion. Once the burst ends,
			// the fish follows the curved rejoin path back to its own FlockID before
			// normal roaming or a future curiosity event is allowed again.
			fish.CuriosityUntilTic = 0;
			fish.RejoinAfterCuriosity = 0;
			fish.RejoinAfterFlee = 1;

			// Startle is a burst response: while fleeing, use the configured multiplier
			// at full strength instead of scaling it down by distance to the radius edge.
			avoidSpeedBoost = fleeScale;
			fleeTriggeredThisUpdate = true;
		}
	}

	if (!fleeTriggeredThisUpdate && fish.FleeUntilTic > Level->maptime)
	{
		const DVector3 heldFleeDir = P_NakaraFishSafeUnit(fish.FleeDirection, currentDir);
		// During the hold, escape intent temporarily overrides flock cohesion. Wall
		// clearance is applied later, so geometry avoidance can still bend this path.
		desired = currentDir * 0.35 + heldFleeDir * 4.0;
		avoidSpeedBoost = fleeScale;
	}

	// Rejoin along a broad spiral/arc instead of pointing straight at the flock
	// center. Fish keep some forward momentum, circle toward a side of the moving
	// group, and gradually blend into its heading. This removes the unnatural
	// radial "all lines point to the center" look after a player scatters them.
	bool isRejoining = false;
	double rejoinSpeedBoost = 1.0;
	const bool wantsFleeRejoin = !fleeTriggeredThisUpdate && fish.FleeUntilTic <= Level->maptime &&
		fish.RejoinAfterFlee != 0;
	const bool wantsCuriosityRejoin = !fleeTriggeredThisUpdate && fish.FleeUntilTic <= Level->maptime &&
		fish.RejoinAfterFlee == 0 && fish.RejoinAfterCuriosity != 0;
	if (wantsFleeRejoin || wantsCuriosityRejoin)
	{
		const bool fromPlayerFlee = wantsFleeRejoin;
		const DVector3 rejoinTarget = flock.Center * 0.70 + flock.AveragePos * 0.30;
		const DVector3 toRejoin = rejoinTarget - particle->Pos;
		const double rejoinDist = sqrt(max(0.0, toRejoin.LengthSquared()));
		const double settleRadius = clamp(190.0 / sqrt(cohesionScale), 105.0, 240.0);

		if (rejoinDist <= settleRadius)
		{
			if (fromPlayerFlee) fish.RejoinAfterFlee = 0;
			else fish.RejoinAfterCuriosity = 0;
		}
		else
		{
			isRejoining = true;
			const DVector3 inwardDir = P_NakaraFishSafeUnit(toRejoin, flockGuide);
			DVector3 horizontalInward(inwardDir.X, inwardDir.Y, 0.0);
			horizontalInward = P_NakaraFishSafeUnit(horizontalInward,
				DVector3(flockGuide.X, flockGuide.Y, 0.0));
			DVector3 tangent(-horizontalInward.Y, horizontalInward.X, 0.0);
			tangent = P_NakaraFishSafeUnit(tangent, DVector3(0.0, 1.0, 0.0));
			const double orbitSign = sin(phase * 3.17) >= 0.0 ? 1.0 : -1.0;
			const double urgency = clamp((rejoinDist - settleRadius) / 820.0, 0.0, 1.0);
			const double orbitWeight = (fromPlayerFlee ? 0.82 : 0.62) * (0.34 + urgency * 0.66);
			const double verticalArc = sin(phase * 2.11 + double(Level->maptime) * 0.012) *
				(0.08 + urgency * 0.10);
			DVector3 spiralDir = inwardDir * 1.12 + tangent * (orbitSign * orbitWeight) +
				flockGuide * 0.72 + flockAverage * 0.28 + DVector3(0.0, 0.0, verticalArc);
			spiralDir = P_NakaraFishSafeUnit(spiralDir, inwardDir);

			// Keep more of the fish's current heading than the old direct-return code.
			// Geometry avoidance below still has priority and can bend the arc around
			// walls and solid 3D floors. Player-scattered fish receive a little more
			// catch-up speed than curiosity fish because the former may be much farther.
			desired = currentDir * (fromPlayerFlee ? 0.62 : 0.72);
			desired += spiralDir * (2.25 + urgency * (fromPlayerFlee ? 1.35 : 0.95));
			desired += flockGuide * (fromPlayerFlee ? 0.70 : 0.82);
			rejoinSpeedBoost = fromPlayerFlee ? 1.35 : 1.18;
		}
	}

	const bool isFleeing = avoidSpeedBoost > 1.0001;
	const bool obstacleRecoveryActive = !isFleeing && !isRejoining && !curiosityActive &&
		fish.ObstacleRecoveryUntilTic > Level->maptime;

	// Ordinary cohesion also gets a curved catch-up mode. This is especially
	// important after wall/3D-floor steering: avoidance can turn a fish quickly,
	// while the old 0.72-degree normal turn budget made it take several times
	// longer to undo that detour, leaving apparent lost fish behind the flock.
	// Do not activate this during curiosity, fleeing, or an explicit rejoin.
	const bool isFlockCatchup = !isFleeing && !isRejoining && !curiosityActive &&
		(flockCatchupUrgency > 0.0 || obstacleRecoveryActive);
	double flockCatchupSpeedBoost = 1.0;
	if (isFlockCatchup)
	{
		const DVector3 inwardDir = P_NakaraFishSafeUnit(toFlock, flockGuide);
		DVector3 horizontalInward(inwardDir.X, inwardDir.Y, 0.0);
		horizontalInward = P_NakaraFishSafeUnit(horizontalInward,
			DVector3(flockGuide.X, flockGuide.Y, 0.0));
		DVector3 tangent(-horizontalInward.Y, horizontalInward.X, 0.0);
		tangent = P_NakaraFishSafeUnit(tangent, DVector3(0.0, 1.0, 0.0));
		const double orbitSign = sin(phase * 3.17) >= 0.0 ? 1.0 : -1.0;
		// Far members prioritize closing distance; nearer members keep a little more
		// tangential motion so they merge into the school instead of flying straight
		// at its center.
		const double effectiveUrgency = max(flockCatchupUrgency, obstacleRecoveryActive ? 0.58 : 0.0);
		const double tangentWeight = obstacleRecoveryActive ?
			(0.22 - effectiveUrgency * 0.08) : (0.34 - effectiveUrgency * 0.16);
		DVector3 catchupDir = inwardDir * (obstacleRecoveryActive ? 1.48 : 1.32) +
			flockGuide * (obstacleRecoveryActive ? 1.42 : 0.92) +
			flockAverage * (obstacleRecoveryActive ? 0.52 : 0.34) +
			tangent * (orbitSign * tangentWeight);
		catchupDir = P_NakaraFishSafeUnit(catchupDir, inwardDir);
		desired = currentDir * (obstacleRecoveryActive ? 0.22 : 0.44) +
			catchupDir * (2.15 + effectiveUrgency * (obstacleRecoveryActive ? 3.20 : 2.35)) +
			flockGuide * (obstacleRecoveryActive ? 1.10 : 0.62);

		// After geometry avoidance, prioritize catching the flock's new heading. This
		// is intentionally less dependent on cohesion so a loose 2x large school does
		// not leave permanent orphans after the whole group turns around a wall.
		flockCatchupSpeedBoost = 1.0 + sqrt(effectiveUrgency) *
			(obstacleRecoveryActive ? (0.48 + 0.020 * cohesionScale) :
			 (0.30 + 0.035 * cohesionScale));
	}

	const double movementSpeedBoost = isFleeing ? avoidSpeedBoost :
		(isRejoining ? rejoinSpeedBoost : (isFlockCatchup ? flockCatchupSpeedBoost : 1.0));
	const double speed = max(0.05, school.BaseSpeed * double(fish.SpeedScale) * movementSpeedBoost);

	// Tail/body beat frequency follows the fish's actual target speed. This means
	// a fleeing fish automatically beats faster because its movement speed rises.
	// SwimBeatScale is an independent user multiplier (1.0 = natural/default).
	// Keep an accumulated phase per fish so changing speed cannot cause phase jumps.
	const double beatMultiplier = clamp(school.SwimBeatScale, 0.10, 4.0);
	// Keep beat frequency proportional at higher flee multipliers as well. The
	// multiplier itself stays a separate artistic control on top of movement speed.
	const double swimRate = 0.35 * clamp(speed / 8.0, 0.20, 10.0) * beatMultiplier;
	fish.SwimPhase += swimRate * double(interval);
	if (fish.SwimPhase > 6.283185307179586) fish.SwimPhase = fmod(fish.SwimPhase, 6.283185307179586);
	const double swimPhase = fish.SwimPhase;
	DVector3 swimSide(-currentDir.Y, currentDir.X, 0.0);
	swimSide = P_NakaraFishSafeUnit(swimSide, DVector3(0.0, 1.0, 0.0));
	desired += swimSide * sin(swimPhase) * (0.10 + (1.0 - affinity) * 0.035);

	// Each fish now predicts walls from its own position. The guide turns along the
	// wall instead of reflecting like a billiard ball, so flock consensus cannot
	// continuously pull an individual through geometry on the other side.
	line_t *approachLine = nullptr;
	const DVector3 preWallDir = P_NakaraFishSafeUnit(desired, currentDir);
	const double wallLookAhead = clamp(96.0 + speed * 16.0, 96.0, 360.0);
	DVector3 clearanceGuide;
	if (P_NakaraFishClearanceGuide(Level, particle->Pos, preWallDir, wallLookAhead,
		collisionRadius, double(flock.Phase), flockGuide, &clearanceGuide, &approachLine))
	{
		// Geometry must dominate even at 10x cohesion. The previous additive weight
		// could still leave a normalized result partly aimed through the wall, while
		// individual phase choices made a head-on flock burst apart.
		desired = P_NakaraFishSafeUnit(desired, currentDir) * 0.24 + clearanceGuide * 2.76;
		fish.ObstacleRecoveryUntilTic = max(fish.ObstacleRecoveryUntilTic, Level->maptime + 42);
	}

	DVector3 floor3DGuide;
	const bool approach3DFloor = P_NakaraFish3DFloorGuide(Level, particle->Pos,
		preWallDir, wallLookAhead + collisionRadius, collisionRadius, &floor3DGuide);
	if (approach3DFloor)
	{
		desired = P_NakaraFishSafeUnit(desired, currentDir) * 0.24 + floor3DGuide * 2.76;
		fish.ObstacleRecoveryUntilTic = max(fish.ObstacleRecoveryUntilTic, Level->maptime + 42);
	}

	desired = P_NakaraFishSafeUnit(desired, currentDir);

	// Normal swimming deliberately has a low angular turn rate so flock-flow/wander
	// changes produce broad arcs instead of sudden steering snaps. Wall avoidance
	// receives a larger budget, while player escape remains the most responsive.
	// Cohesion must affect how quickly a fish can actually align, not just the
	// magnitude of a vector that is normalized later. This makes 6x and 10x
	// meaningfully different. Catch-up can turn nearly as quickly as avoidance,
	// so a wall detour does not create a long-lived orphan.
	const double normalTurnDegreesPerTic = clamp(0.55 + cohesionScale * 0.11, 0.58, 1.75);
	const double effectiveCatchupUrgency = max(flockCatchupUrgency, obstacleRecoveryActive ? 0.58 : 0.0);
	const double catchupTurnDegreesPerTic = obstacleRecoveryActive ?
		clamp(4.05 + effectiveCatchupUrgency * 1.05 + cohesionScale * 0.035, 4.20, 5.35) :
		clamp(normalTurnDegreesPerTic + 0.55 +
			effectiveCatchupUrgency * (0.65 + cohesionScale * 0.04), 1.20, 3.00);
	const double obstacleTurnDegreesPerTic = 5.20;
	const double maxTurnDegreesPerTic = isFleeing ? 6.0 :
		((approachLine != nullptr || approach3DFloor) ? obstacleTurnDegreesPerTic :
			(isRejoining ? 1.45 : (isFlockCatchup ? catchupTurnDegreesPerTic : normalTurnDegreesPerTic)));
	const double maxTurnRadians = maxTurnDegreesPerTic * (M_PI / 180.0) * double(interval);
	const DVector3 steeredDir = P_NakaraFishTurnToward(currentDir, desired, maxTurnRadians);
	const double oldSpeed = sqrt(max(0.0, vel.LengthSquared()));
	const double speedBlend = 1.0 - pow(0.90, double(interval));
	const double swimPulse = 1.0 + sin(swimPhase + 1.5707963267948966) * 0.025;
	// A startled fish reaches its burst speed immediately and holds it. Once the
	// hold expires, normal blending resumes so the fish settles back down gradually.
	const double steeredSpeed = (isFleeing ? speed :
		(oldSpeed * (1.0 - speedBlend) + speed * speedBlend)) * swimPulse;
	vel = steeredDir * steeredSpeed;

	// Keep fish comfortably inside the current navigable vertical volume. Solid
	// 3D floors participate exactly like local floors/ceilings here, while
	// non-solid swimmable volumes remain transparent to fish movement.
	sector_t *sec = Level->PointInSector(particle->Pos);
	if (sec != nullptr)
	{
		double floorZ, ceilZ;
		P_NakaraFishVerticalBounds(sec, particle->Pos.XY(), particle->Pos.Z, &floorZ, &ceilZ);
		// The configured activity height can be much larger than the local sector.
		// The virtual body radius is also part of the margin, so the sprite plane
		// turns before its top/bottom edge reaches floor or ceiling geometry.
		const double opening = max(0.0, ceilZ - floorZ);
		const double maxMargin = opening * 0.45;
		const double desiredMargin = max(max(18.0, double(school.Height) * 0.10), collisionRadius + 4.0);
		const double verticalMargin = min(desiredMargin, maxMargin);
		if (particle->Pos.Z < floorZ + verticalMargin)
		{
			const double x = clamp((floorZ + verticalMargin - particle->Pos.Z) / verticalMargin, 0.0, 1.0);
			vel.Z = max(vel.Z, 0.10 + x * 0.32);
		}
		if (particle->Pos.Z > ceilZ - verticalMargin)
		{
			const double x = clamp((particle->Pos.Z - (ceilZ - verticalMargin)) / verticalMargin, 0.0, 1.0);
			vel.Z = min(vel.Z, -0.10 - x * 0.32);
		}
	}

	particle->Vel = FVector3(vel);

	// World-oriented fish: the image plane's horizontal axis follows velocity.
	// RollVel/RollAcc are repurposed as yaw/pitch only for underwater fish;
	// this preserves particle_t at 128 bytes. Roll is a real bank angle.
	const double horizontalSpeed = sqrt(vel.X * vel.X + vel.Y * vel.Y);
	const double yawDeg = atan2(vel.Y, vel.X) * (180.0 / M_PI);
	const double pitchDeg = atan2(vel.Z, max(0.0001, horizontalSpeed)) * (180.0 / M_PI);
	const DVector3 newDir = P_NakaraFishSafeUnit(vel, currentDir);
	const double signedTurn = currentDir.X * newDir.Y - currentDir.Y * newDir.X;
	const float targetBank = float(clamp(-signedTurn * 70.0, -30.0, 30.0));
	const float bankBlend = float(1.0 - pow(0.76, double(interval)));
	fish.Bank += (targetBank - fish.Bank) * bankBlend;

	// A static fish sprite cannot bend an individual fin, but yaw/roll oscillation
	// reads as body + tail propulsion. Frequency already follows actual speed, so
	// fleeing accelerates the beat naturally instead of using a hard-coded state rate.
	const double fleeAmount = clamp(avoidSpeedBoost - 1.0, 0.0, 1.0);
	const double visualBeat = sin(swimPhase);
	const double visualYaw = yawDeg + visualBeat * (4.8 + fleeAmount * 1.6);
	const double visualPitch = pitchDeg + sin(swimPhase * 0.52 + phase * 0.46) * 0.9;
	const double visualRoll = double(fish.Bank) + cos(swimPhase + phase * 0.27) * 1.6;

	particle->RollVel = float(visualYaw);
	particle->RollAcc = float(visualPitch);
	particle->Roll = float(visualRoll);
	particle->size = fish.SpriteScale;
	particle->spriteScaleY = fish.SpriteScale;

	if (fish.SpriteIndex >= 0 && fish.FrameCount > 0)
	{
		const int frame = fish.FirstFrame + ((Level->maptime / max(1, int(fish.AnimStep)) + fish.UpdatePhase) % fish.FrameCount);
		bool frameMirror = false;
		FTextureID tex = sprites[fish.SpriteIndex].GetSpriteFrame(frame, 0, nullAngle, &frameMirror, false);
		if (tex.isValid()) particle->texture = tex;
		particle->spriteScaleY = frameMirror ? -fish.SpriteScale : fish.SpriteScale;
	}
}

void P_ThinkParticles (FLevelLocals *Level)
{
	// Resolve the CVAR/viewer once per particle tick, not once per particle.
	// r_bubbledist < 0 keeps the old unlimited behavior.
	const int bubbleLodDistance = P_NakaraBubbleLodDistance();
	AActor *bubbleLodViewer = P_NakaraBubbleLodViewer(Level);
	AActor *fishAvoidActor = P_NakaraFishAvoidActor(Level);
	const int fishLodDistance = P_NakaraFishLodDistance();
	auto fishState = P_NakaraFishLevel(Level, false);
	P_NakaraFishPrepareSchools(Level, fishState);

	int i = Level->ActiveParticles;
	particle_t *particle = nullptr, *prev = nullptr;
	while (i != NO_PARTICLE)
	{
		particle = &Level->Particles[i];
		i = particle->tnext;

		const int particleIndex = int(particle - Level->Particles.Data());
		const bool isUnderwaterAmbient =
			(unsigned)particleIndex < Level->NakaraParticleGroups.Size() &&
			Level->NakaraParticleGroups[particleIndex] == NPG_UnderwaterAmbient;
		const bool isUnderwaterFish =
			(unsigned)particleIndex < Level->NakaraParticleGroups.Size() &&
			Level->NakaraParticleGroups[particleIndex] == NPG_UnderwaterFish;

		// [Nakara] LOD: once a bubble is outside r_bubbledist, return it to the
		// particle pool immediately. This saves both renderer work and all future
		// P_ThinkParticles updates for that bubble. The check also runs while time
		// is frozen so distant bubbles cannot remain rendered indefinitely.
		if (isUnderwaterAmbient &&
			P_NakaraBubbleOutsideLod(particle->Pos, bubbleLodViewer, bubbleLodDistance))
		{
			FreeParticle(Level, particle);
			continue;
		}

		if (Level->isFrozen() && !(particle->flags &SPF_NOTIMEFREEZE))
		{
			if(particle->flags & SPF_LOCAL_ANIM)
			{
				particle->animData.SwitchTic++;
			}

			prev = particle;
			continue;
		}
		
		// [Nakara] Underwater ambient bubbles reproduce the old BubbleFX
		// fade/scale envelope without becoming Actors/Thinkers:
		//   50 tics fade in (alpha 0 -> 1)
		//   10..35 tics hold
		//   50 tics fade out (alpha 1 -> 0)
		// The signed target sprite scale is stored in sizestep for this group.
		// This keeps particle_t at 128 bytes and needs no extra per-particle state.
		if (isUnderwaterFish)
		{
			P_NakaraUpdateFishParticle(Level, particle, particleIndex, fishState, bubbleLodViewer, fishAvoidActor, fishLodDistance);
			// Fish are persistent visual particles. When outside LOD they remain in
			// the pool at alpha 0 so returning to the area does not require respawn.
			if (particle->alpha <= 0.f)
			{
				prev = particle;
				continue;
			}
			// Unlike the full boid update, this collision guard is intentionally run
			// every visible tic so an LOD-skipped fish cannot tunnel through a wall.
			P_NakaraFishImmediateWallGuard(Level, particle, fishState, particleIndex);
		}
		else
		{
			if (isUnderwaterAmbient)
			{
				constexpr float BubbleFadeStep = 0.02f;
				constexpr int BubbleFadeOutTics = 50;

				if (particle->ttl > BubbleFadeOutTics)
				{
					// Fade in until fully visible, then naturally hold at 1.0.
					particle->alpha = min(1.0f, particle->alpha + BubbleFadeStep);
				}
				else
				{
					// The last 50 tics are the fade-out phase.
					particle->alpha = max(0.0f, particle->alpha - BubbleFadeStep);
				}

				const float signedMaxScale = particle->sizestep;
				particle->size = fabsf(signedMaxScale) * particle->alpha;
				particle->spriteScaleY = signedMaxScale * particle->alpha;
			}
			else
			{
				particle->alpha -= particle->fadestep;
				particle->size += particle->sizestep;
			}

			if (particle->alpha <= 0 || --particle->ttl <= 0 || (particle->size <= 0))
			{ // The particle has expired, so free it
				FreeParticle(Level, particle);
				continue;
			}
		}

		// Handle crossing a line portal
		DVector2 newxy = Level->GetPortalOffsetPosition(particle->Pos.X, particle->Pos.Y, particle->Vel.X, particle->Vel.Y);
		particle->Pos.X = newxy.X;
		particle->Pos.Y = newxy.Y;
		particle->Pos.Z += particle->Vel.Z;
		particle->Vel += particle->Acc;

		if ((particle->flags & SPF_ROLL) && !isUnderwaterFish)
		{
			particle->Roll += particle->RollVel;
			particle->RollVel += particle->RollAcc;
		}
		
		particle->subsector = Level->PointInRenderSubsector(particle->Pos);
		sector_t *s = particle->subsector->sector;
		// Handle crossing a sector portal.
		if (!s->PortalBlocksMovement(sector_t::ceiling))
		{
			if (particle->Pos.Z > s->GetPortalPlaneZ(sector_t::ceiling))
			{
				particle->Pos += s->GetPortalDisplacement(sector_t::ceiling);
				particle->subsector = NULL;
			}
		}
		else if (!s->PortalBlocksMovement(sector_t::floor))
		{
			if (particle->Pos.Z < s->GetPortalPlaneZ(sector_t::floor))
			{
				particle->Pos += s->GetPortalDisplacement(sector_t::floor);
				particle->subsector = NULL;
			}
		}
		prev = particle;
	}
}

void P_SpawnParticle(FLevelLocals *Level, const DVector3 &pos, const DVector3 &vel, const DVector3 &accel, PalEntry color, double startalpha, int lifetime, double size,
	double fadestep, double sizestep, int flags, FTextureID texture, ERenderStyle style, double startroll, double rollvel, double rollacc, double spriteScaleY, uint8_t nakaraGroup)
{
	particle_t *particle = NewParticle(Level, !!(flags & SPF_REPLACE));

	if (particle)
	{
		const int particleIndex = int(particle - Level->Particles.Data());
		if ((unsigned)particleIndex < Level->NakaraParticleGroups.Size())
		{
			Level->NakaraParticleGroups[particleIndex] = nakaraGroup;
		}
		particle->Pos = pos;
		particle->Vel = FVector3(vel);
		particle->Acc = FVector3(accel);
		particle->color = ParticleColor(color);
		particle->alpha = float(startalpha);
		if ((fadestep < 0 && !(flags & SPF_NEGATIVE_FADESTEP)) || fadestep <= -1.0) particle->fadestep = FADEFROMTTL(lifetime);
		else particle->fadestep = float(fadestep);
		particle->ttl = lifetime;
		particle->size = size;
		particle->sizestep = sizestep;
		particle->texture = texture;
		particle->style = style;
		particle->Roll = (float)startroll;
		particle->RollVel = (float)rollvel;
		particle->RollAcc = (float)rollacc;
		particle->flags = flags;
		particle->spriteScaleY = (float)spriteScaleY;
		if(flags & SPF_LOCAL_ANIM)
		{
			TexAnim.InitStandaloneAnimation(particle->animData, texture, Level->maptime);
		}
	}
}

//==========================================================================
//
// [Nakara] Underwater ambient particles
//
// Replaces the old ACS -> SpawnForced("BubbleFX") Actor path with a direct
// particle pool emitter. The spawn distribution intentionally mirrors the old
// particlegen ACS script:
//   center = actor position + actor velocity * 50
//   radial offset = 200..400 around actor yaw with the same random angle range
//   vertical offset = -200..200, plus BubbleFX's former +56 start offset
//
// The old BubbleFX's one-unit circular wobble is approximated with a tiny XY
// drift. This keeps P_ThinkParticles on its cheapest normal movement path.
//
//==========================================================================

static double P_NakaraAmbientRandomRange(double minValue, double maxValue)
{
	return minValue + (maxValue - minValue) * pr_nkunderwaterambient.GenRand_Real2();
}

int P_SpawnUnderwaterAmbientParticles(AActor *actor, int amount, const char *spriteName, int spriteFrame)
{
	if (actor == nullptr || actor->Level == nullptr || amount <= 0)
	{
		return 0;
	}

	// One ScriptCall may request a denser burst, but never allow an accidental
	// ACS value to consume the entire particle pool in one tic.
	amount = clamp<int>(amount, 1, 64);

	// Resolve the sprite only once for the whole batch. This reuses the textured
	// particle path already used by Nakara's ParticleTrail afterimages, so every
	// spawned bubble remains a lightweight particle_t rather than an Actor.
	if (spriteName == nullptr || spriteName[0] == 0)
	{
		spriteName = "BUBL";
	}
	const int spriteIndex = GetSpriteIndex(spriteName, false);
	if (spriteIndex < 0 || spriteIndex >= (int)sprites.Size())
	{
		return 0;
	}

	bool frameMirror = false;
	const FTextureID bubbleTexture = sprites[spriteIndex].GetSpriteFrame(
		spriteFrame, 0, nullAngle, &frameMirror, false);
	if (!bubbleTexture.isValid())
	{
		return 0;
	}

	auto bubbleTex = TexMan.GetGameTexture(bubbleTexture, false);
	if (bubbleTex == nullptr || !bubbleTex->isValid())
	{
		return 0;
	}

	FLevelLocals *Level = actor->Level;
	const DVector3 center = actor->Pos() + actor->Vel * 50.0;
	const int bubbleLodDistance = P_NakaraBubbleLodDistance();
	AActor *bubbleLodViewer = P_NakaraBubbleLodViewer(Level);
	int spawned = 0;

	for (int i = 0; i < amount; ++i)
	{
		if (Level->InactiveParticles == NO_PARTICLE)
		{
			break;
		}

		// The ACS source generated X and Y with independent angle/radius rolls:
		//   Cos(yaw + random angle) * random(200,400)
		//   Sin(yaw + random angle) * random(200,400)
		// Keep that slightly irregular distribution instead of turning it into a
		// mathematically perfect ring. ACS angles are turns, hence * 360 here.
		const int angleStepX = pr_nkunderwaterambient(47) - 16;
		const int angleStepY = pr_nkunderwaterambient(47) - 16;
		const double angleSignX = pr_nkunderwaterambient(2) == 0 ? 1.0 : -1.0;
		const double angleSignY = pr_nkunderwaterambient(2) == 0 ? 1.0 : -1.0;
		const DAngle spawnAngleX = actor->Angles.Yaw +
			DAngle::fromDeg(double(angleStepX) * 0.01 * angleSignX * 360.0);
		const DAngle spawnAngleY = actor->Angles.Yaw +
			DAngle::fromDeg(double(angleStepY) * 0.01 * angleSignY * 360.0);
		const double radiusX = 200.0 + double(pr_nkunderwaterambient(201));
		const double radiusY = 200.0 + double(pr_nkunderwaterambient(201));

		DVector3 pos(
			center.X + spawnAngleX.Cos() * radiusX,
			center.Y + spawnAngleY.Sin() * radiusY,
			center.Z + double(pr_nkunderwaterambient(401) - 200) + 56.0);

		// Do not allocate a particle at all if this randomized spawn point is
		// already beyond the BubbleFX render distance. This matters when the
		// player's velocity * 50 lead offset pushes the ambient field forward.
		if (P_NakaraBubbleOutsideLod(pos, bubbleLodViewer, bubbleLodDistance))
		{
			continue;
		}

		// BubbleFX used Vel.Z 0.1..0.3 and a one-map-unit circular XY wobble.
		// A very small linear drift gives a similar visual result without adding
		// any special per-particle thinker/update branch.
		DVector3 vel(
			P_NakaraAmbientRandomRange(-0.015, 0.015),
			P_NakaraAmbientRandomRange(-0.015, 0.015),
			P_NakaraAmbientRandomRange(0.10, 0.30));

		// BubbleFX used heartfullsize 0.010..0.15 as its sprite scale. Use that
		// same range with the textured-particle sprite geometry instead of the
		// old textureless particle point size. The negative Y scale marker mirrors
		// the frame if the selected sprite frame requests it.
		// Match the old BubbleFX envelope exactly:
		//   alpha/scale 0 -> 1 over 50 tics (0.02 per tic),
		//   hold for 10..35 tics, then 1 -> 0 over the final 50 tics.
		// Total lifetime is therefore 110..135 tics. For this particle group,
		// sizestep stores the signed maximum sprite scale (including mirror).
		const int lifetime = 110 + pr_nkunderwaterambient(26);
		const double spriteScale = P_NakaraAmbientRandomRange(0.010, 0.15);
		const double encodedMaxScale = frameMirror ? -spriteScale : spriteScale;

		P_SpawnParticle(Level, pos, vel, DVector3(), PalEntry(255, 255, 255),
			0.0, lifetime, 0.0, 0.0, encodedMaxScale, SPF_FULLBRIGHT,
			bubbleTexture, STYLE_TranslucentStencil, 0.0, 0.0, 0.0,
			0.0, NPG_UnderwaterAmbient);
		++spawned;
	}

	return spawned;
}

int P_SpawnUnderwaterFishSchool(AActor *actor, int amount, const char *spriteName,
	int firstFrame, int frameCount, int radius, int height, int scalePercent,
	double baseSpeed, int avoidRadius, double cohesionScale, double fleeSpeedScale, double swimBeatScale,
	double collisionRadius, double fleeHoldSeconds, double sizeVariation)
{
	if (actor == nullptr || actor->Level == nullptr || amount <= 0) return 0;
	FLevelLocals *Level = actor->Level;
	// Fish are still bounded by the engine particle pool (r_maxparticles), but a
	// single school call may now request a larger population for dense scenes.
	amount = clamp<int>(amount, 1, 1024);
	radius = clamp<int>(radius, 32, 16384);
	height = clamp<int>(height, 16, 16384);
	// Each consecutive sprite frame owns one independent flock. For example,
	// firstFrame=A and frameCount=6 creates six flocks using FISH A through F.
	frameCount = clamp<int>(frameCount, 1, NKFISH_MAX_FLOCK_COUNT);
	firstFrame = clamp<int>(firstFrame, 0, 25);
	if (firstFrame + frameCount > 26) frameCount = 26 - firstFrame;
	scalePercent = clamp<int>(scalePercent, 1, 1000);
	baseSpeed = clamp<double>(baseSpeed, 0.05, 32.0);
	avoidRadius = clamp<int>(avoidRadius, 32, 4096);
	cohesionScale = clamp<double>(cohesionScale, 0.25, 10.0);
	fleeSpeedScale = clamp<double>(fleeSpeedScale, 1.0, 10.0);
	swimBeatScale = clamp<double>(swimBeatScale, 0.10, 4.0);
	fleeHoldSeconds = clamp<double>(fleeHoldSeconds, 0.0, 10.0);
	// 0.20 means each fish is between 80% and 120% of scalePercent. Keep the
	// lower bound positive even at the maximum supported variation.
	sizeVariation = clamp<double>(sizeVariation, 0.0, 0.90);

	if (spriteName == nullptr || spriteName[0] == 0) spriteName = "FISH";
	const int spriteIndex = GetSpriteIndex(spriteName, false);
	if (spriteIndex < 0 || spriteIndex >= (int)sprites.Size()) return 0;

	bool initialMirror = false;
	FTextureID initialTexture = sprites[spriteIndex].GetSpriteFrame(firstFrame, 0, nullAngle, &initialMirror, false);
	if (!initialTexture.isValid()) return 0;
	auto texture = TexMan.GetGameTexture(initialTexture, false);
	if (texture == nullptr || !texture->isValid()) return 0;

	const double spriteScaleValue = double(scalePercent) * 0.01;
	const bool autoCollisionRadius = collisionRadius <= 0.0;
	double speciesCollisionRadius[NKFISH_MAX_FLOCK_COUNT];
	for (int i = 0; i < NKFISH_MAX_FLOCK_COUNT; ++i) speciesCollisionRadius[i] = 1.0;

	// Cache a base virtual radius for each species frame. In auto mode this uses
	// that frame's own display extent, rather than a school-wide maximum,
	// so differently sized species frames keep matching clearance. Explicit
	// collisionRadius is the radius at the average (scalePercent) fish size.
	for (int frameOffset = 0; frameOffset < frameCount; ++frameOffset)
	{
		const int speciesOffset = frameOffset;
		double baseRadius = collisionRadius;
		if (autoCollisionRadius)
		{
			bool frameMirror = false;
			FTextureID frameTexture = sprites[spriteIndex].GetSpriteFrame(firstFrame + speciesOffset, 0, nullAngle, &frameMirror, false);
			auto frameGameTexture = frameTexture.isValid() ? TexMan.GetGameTexture(frameTexture, false) : nullptr;
			if (frameGameTexture == nullptr || !frameGameTexture->isValid()) frameGameTexture = texture;
			const double displayExtent = max(double(frameGameTexture->GetDisplayWidth()),
				double(frameGameTexture->GetDisplayHeight()));
			baseRadius = displayExtent * spriteScaleValue * 0.50;
		}
		speciesCollisionRadius[frameOffset] = clamp<double>(baseRadius, 1.0, 512.0);
	}
	// Retain a conservative school fallback for old/uninitialized fish state.
	collisionRadius = speciesCollisionRadius[0];
	for (int g = 1; g < frameCount; ++g)
	{
		collisionRadius = max(collisionRadius, speciesCollisionRadius[g]);
	}

	auto state = P_NakaraFishLevel(Level, true);
	if (state == nullptr) return 0;
	if (state->Fish.Size() != Level->Particles.Size()) state->Fish.Resize(Level->Particles.Size());
	if (state->Schools.Size() >= 65534) return 0;

	FNakaraFishSchoolState school;
	school.Origin = actor->Pos();
	school.Center = school.Origin;
	school.Radius = float(radius);
	school.Height = float(height);
	school.BaseSpeed = baseSpeed;
	school.AvoidRadius = float(avoidRadius);
	school.CohesionScale = float(cohesionScale);
	school.FleeSpeedScale = fleeSpeedScale;
	school.FleeHoldSeconds = fleeHoldSeconds;
	school.SwimBeatScale = swimBeatScale;
	school.CollisionRadius = collisionRadius;
	school.FlockCount = uint8_t(frameCount);
	school.Phase = float(P_NakaraFishRandomRange(0.0, 6.283185307179586));

	for (int g = 0; g < frameCount; ++g)
	{
		auto &flock = school.Flocks[g];
		const double a = (6.283185307179586 * double(g) / double(frameCount)) +
			P_NakaraFishRandomRange(-0.30, 0.30);
		const double r = double(radius) * P_NakaraFishRandomRange(0.20, 0.36);
		flock.Center = school.Origin + DVector3(cos(a) * r, sin(a) * r,
			P_NakaraFishRandomRange(-double(height) * 0.24, double(height) * 0.24));
		flock.Phase = float(P_NakaraFishRandomRange(0.0, 6.283185307179586));
		DVector3 dir(cos(double(flock.Phase)), sin(double(flock.Phase)),
			P_NakaraFishRandomRange(-0.32, 0.32));
		dir = P_NakaraFishSafeUnit(dir, DVector3(1.0, 0.0, 0.0));
		const double speedFactor = g == 0 ? (12.0 / 14.0) : (g == 2 ? (16.0 / 14.0) : 1.0);
		flock.Velocity = dir * max(0.04, double(school.BaseSpeed) * 0.78 * speedFactor);
		const double ta = P_NakaraFishRandomRange(0.0, 6.283185307179586);
		const double tr = sqrt(pr_nkfishschool.GenRand_Real2()) * double(radius) * 0.70;
		flock.Target = school.Origin + DVector3(cos(ta) * tr, sin(ta) * tr,
			P_NakaraFishRandomRange(-double(height) * 0.50, double(height) * 0.50));
		flock.NextTargetTic = Level->maptime + 175 + int(pr_nkfishschool(281));
	}

	const uint16_t schoolIndex = uint16_t(state->Schools.Size());
	state->Schools.Push(school);

	const float baseSpriteScale = float(spriteScaleValue);
	int spawned = 0;
	for (int i = 0; i < amount; ++i)
	{
		if (Level->InactiveParticles == NO_PARTICLE) break;

		// Round-robin assignment keeps every requested species flock balanced.
		// With 90 fish and frameCount=6, each FISH A-F flock receives about 15.
		const int flockIndex = i % frameCount;
		const auto &spawnFlock = school.Flocks[flockIndex];

		// Per-fish body size. scalePercent remains the average size; sizeVariation
		// is a symmetric multiplier range (0.20 -> 0.80..1.20). The same factor
		// scales the virtual collision radius, keeping large fish farther from walls
		// while small fish can naturally use tighter gaps.
		const double sizeFactor = sizeVariation > 0.000001 ?
			1.0 + P_NakaraFishRandomRange(-sizeVariation, sizeVariation) : 1.0;
		const float spriteScale = float(double(baseSpriteScale) * sizeFactor);
		const double fishCollisionRadius = clamp<double>(speciesCollisionRadius[flockIndex] * sizeFactor, 1.0, 512.0);

		const double angle = P_NakaraFishRandomRange(0.0, 6.283185307179586);
		// Keep high-cohesion spawn formations visibly tighter as well. The previous
		// minimums made 6x..10x converge toward almost the same initial spread.
		const double spawnClusterRadius = clamp(720.0 / sqrt(clamp(double(school.CohesionScale), 0.25, 10.0)), 180.0, 900.0);
		const double spawnClusterHeight = clamp(420.0 / sqrt(clamp(double(school.CohesionScale), 0.25, 10.0)), 110.0, 620.0);
		const double radial = sqrt(pr_nkfishschool.GenRand_Real2()) * spawnClusterRadius;
		const double zoff = P_NakaraFishRandomRange(-spawnClusterHeight * 0.50, spawnClusterHeight * 0.50);
		DVector3 anchor(cos(angle) * radial, sin(angle) * radial, zoff);
		DVector3 pos = spawnFlock.Center + anchor;

		// Do not seed a fish through a nearby wall. Pull the personal anchor
		// inward a few times if the direct path from the school marker is blocked.
		for (int attempt = 0; attempt < 3; ++attempt)
		{
			line_t *spawnBlock = nullptr;
			const DVector3 spawnDelta = pos - spawnFlock.Center;
			if (!P_NakaraFishBlockingLine(Level, spawnFlock.Center, spawnDelta, 1.0, &spawnBlock)) break;
			anchor *= 0.55;
			pos = spawnFlock.Center + anchor;
		}

		sector_t *spawnSector = Level->PointInSector(pos);
		if (spawnSector != nullptr)
		{
			double floorZ, ceilZ;
			P_NakaraFishVerticalBounds(spawnSector, pos.XY(), pos.Z, &floorZ, &ceilZ);
			const double opening = max(0.0, ceilZ - floorZ);
			const double spawnMargin = min(fishCollisionRadius + 2.0, opening * 0.45);
			if (ceilZ > floorZ) pos.Z = clamp(pos.Z, floorZ + spawnMargin, ceilZ - spawnMargin);
		}

		// Each flock owns exactly one consecutive species frame.
		// firstFrame=A, frameCount=6 maps Flock 0..5 to FISH A..F.
		const int speciesFrame = firstFrame + flockIndex;
		bool speciesMirror = false;
		FTextureID speciesTexture = sprites[spriteIndex].GetSpriteFrame(speciesFrame, 0, nullAngle, &speciesMirror, false);
		if (!speciesTexture.isValid())
		{
			speciesTexture = initialTexture;
			speciesMirror = initialMirror;
		}

		const DVector3 flockDir = P_NakaraFishSafeUnit(spawnFlock.Velocity, DVector3(1.0, 0.0, 0.0));
		const double flockYaw = atan2(flockDir.Y, flockDir.X);
		const double dirAngle = flockYaw + P_NakaraFishRandomRange(-0.90, 0.90);
		DVector3 dir(cos(dirAngle), sin(dirAngle), flockDir.Z + P_NakaraFishRandomRange(-0.34, 0.34));
		dir = P_NakaraFishSafeUnit(dir, DVector3(1.0, 0.0, 0.0));
		const float speedScale = float(P_NakaraFishRandomRange(0.72, 1.24));
		DVector3 vel = dir * (double(school.BaseSpeed) * double(speedScale));
		const double initialYaw = atan2(vel.Y, vel.X) * (180.0 / M_PI);
		const double initialPitch = atan2(vel.Z, max(0.0001, sqrt(vel.X * vel.X + vel.Y * vel.Y))) * (180.0 / M_PI);

		P_SpawnParticle(Level, pos, vel, DVector3(), PalEntry(255, 255, 255),
			1.0, 0x3fffffff, spriteScale, 0.0, 0.0,
			SPF_ROLL | SPF_NO_XY_BILLBOARD | SPF_NOFACECAMERA | SPF_FULLBRIGHT, speciesTexture, STYLE_Translucent,
			0.0, initialYaw, initialPitch, speciesMirror ? -spriteScale : spriteScale,
			NPG_UnderwaterFish);

		const int particleIndex = Level->ActiveParticles;
		if (particleIndex == NO_PARTICLE || (unsigned)particleIndex >= state->Fish.Size()) break;
		auto &fish = state->Fish[particleIndex];
		fish.AnchorOffset = anchor;
		const double roamYaw = P_NakaraFishRandomRange(-0.72, 0.72);
		DVector3 roamSide(-flockDir.Y, flockDir.X, 0.0);
		roamSide = P_NakaraFishSafeUnit(roamSide, DVector3(0.0, 1.0, 0.0));
		fish.RoamOffset = P_NakaraFishSafeUnit(
			flockDir * cos(roamYaw) + roamSide * sin(roamYaw) +
			DVector3(0.0, 0.0, P_NakaraFishRandomRange(-0.30, 0.30)), flockDir);
		fish.NextRoamTic = Level->maptime + 105 + int(pr_nkfishschool(211));
		fish.NextCuriosityTic = Level->maptime + 280 + int(pr_nkfishschool(281));
		fish.Phase = float(P_NakaraFishRandomRange(0.0, 6.283185307179586));
		fish.SwimPhase = P_NakaraFishRandomRange(0.0, 6.283185307179586);
		fish.SpeedScale = speedScale;
		// All fish now start with normal flock affinity. Short-lived curiosity events
		// temporarily reduce effective affinity later; no member is a permanent
		// straggler anymore.
		fish.FlockAffinity = float(P_NakaraFishRandomRange(0.86, 1.12));
		fish.SpriteScale = spriteScale;
		fish.CollisionRadius = fishCollisionRadius;
		fish.School = schoolIndex;
		fish.Flock = uint8_t(flockIndex);
		fish.SpriteIndex = int16_t(spriteIndex);
		fish.FirstFrame = uint8_t(speciesFrame);
		fish.FrameCount = 1;
		fish.AnimStep = uint8_t(3 + pr_nkfishschool(4));
		fish.UpdatePhase = uint8_t(pr_nkfishschool(16));
		++spawned;
	}

	return spawned;
}

int P_ClearNakaraFishSchools(FLevelLocals *Level)
{
	if (Level == nullptr) return 0;
	const int removed = P_ClearNakaraParticleGroup(Level, NPG_UnderwaterFish);
	auto state = P_NakaraFishLevel(Level, false);
	if (state != nullptr)
	{
		state->Schools.Clear();
		for (auto &fish : state->Fish) fish = FNakaraFishParticleState();
	}
	return removed;
}

int P_ClearNakaraParticleGroup(FLevelLocals *Level, uint8_t group)
{
	if (Level == nullptr || group == NPG_None ||
		Level->NakaraParticleGroups.Size() != Level->Particles.Size())
	{
		return 0;
	}

	int removed = 0;
	int particleIndex = Level->ActiveParticles;
	while (particleIndex != NO_PARTICLE)
	{
		particle_t *particle = &Level->Particles[particleIndex];
		const int nextIndex = particle->tnext;

		if (Level->NakaraParticleGroups[particleIndex] == group)
		{
			FreeParticle(Level, particle);
			++removed;
		}

		particleIndex = nextIndex;
	}

	return removed;
}

//
// JitterParticle
//
// Creates a particle with "jitter"
//
particle_t *JitterParticle (FLevelLocals *Level, int ttl)
{
	return JitterParticle (Level, ttl, 1.0);
}
// [XA] Added "drift speed" multiplier setting for enhanced railgun stuffs.
particle_t *JitterParticle (FLevelLocals *Level, int ttl, double drift)
{
	particle_t *particle = NewParticle (Level);

	if (particle) {
		int i;

		// Set initial velocities
		for (i = 3; i; i--)
			particle->Vel[i] = ((1./4096) * (M_Random () - 128) * drift);
		// Set initial accelerations
		for (i = 3; i; i--)
			particle->Acc[i] = ((1./16384) * (M_Random () - 128) * drift);

		particle->alpha = 1.f;	// fully opaque
		particle->ttl = ttl;
		particle->fadestep = FADEFROMTTL(ttl);
	}
	return particle;
}

static void MakeFountain (AActor *actor, int color1, int color2)
{
	particle_t *particle;

	if (!(actor->Level->time & 1))
		return;

	particle = JitterParticle (actor->Level, 51);

	if (particle)
	{
		DAngle an = DAngle::fromDeg(M_Random() * (360. / 256));
		double out = actor->radius * M_Random() / 256.;

		particle->Pos = actor->Vec3Angle(out, an, actor->Height + 1);
		if (out < actor->radius/8)
			particle->Vel.Z += 10.f/3;
		else
			particle->Vel.Z += 3;
		particle->Acc.Z -= 1.f/11;
		if (M_Random() < 30) {
			particle->size = 4;
			particle->color = color2;
		} else {
			particle->size = 6;
			particle->color = color1;
		}
	}
}


//==========================================================================
//
// [Nakara] Textured sprite afterimage trail.
//
// Each sample is a normal particle_t that carries the actor's current sprite
// texture. The hardware renderer draws these special textured particles using
// the texture's native sprite rectangle instead of the normal square particle
// quad. This keeps the trail out of the Thinker system while preserving the
// projectile sprite silhouette and offsets.
//
//==========================================================================

static constexpr int PARTICLETRAIL_MAX_SPAWNS_PER_TIC = 16;
static constexpr int PARTICLETRAIL_MAX_HISTORY_SAMPLES_PER_TIC = 8;
static constexpr unsigned PARTICLETRAIL_MAX_HISTORY_SAMPLES = 128; // Per generation emergency cap; lifetime cleanup normally keeps this lower.
static constexpr unsigned PARTICLETRAIL_MAX_RETAINED_HISTORY_SAMPLES = 128; // Across split generations.

// [Nakara] Every ribbon activation gets a unique chain ID. The history array is
// already owned by the actor, but an explicit generation tag makes the renderer
// refuse to bridge samples belonging to another projectile/activation even if a
// stale sample survives an unusual actor lifecycle or mode transition.
static uint32_t ParticleTrailNextGeneration = 1;

static uint32_t P_NextParticleTrailGeneration()
{
	uint32_t generation = ParticleTrailNextGeneration++;
	if (generation == 0)
	{
		generation = ParticleTrailNextGeneration++;
	}
	if (ParticleTrailNextGeneration == 0)
	{
		ParticleTrailNextGeneration = 1;
	}
	return generation;
}

static const FParticleTrailHistorySample *P_GetLastParticleTrailSample(AActor *actor)
{
	if (actor == nullptr || actor->ParticleTrailGeneration == 0) return nullptr;
	for (int i = (int)actor->ParticleTrailHistory.Size() - 1; i >= 0; --i)
	{
		const auto &sample = actor->ParticleTrailHistory[i];
		if (sample.Generation == actor->ParticleTrailGeneration)
		{
			return &sample;
		}
	}
	return nullptr;
}


// [Nakara] Move one completed ribbon generation out of its projectile and into
// a transient VisualThinker. This is deliberately a *render ownership* change,
// not a coordinate transform: the copied samples stay in the portal space in
// which they were recorded. Keeping the entrance-side strip attached to an
// actor that has already crossed the portal lets later draw passes reinterpret
// that old strip through the actor's new portal context, producing the large
// triangular/wedge artifacts visible at portal crossings.
static bool P_DetachParticleTrailGeneration(AActor *actor, uint32_t generation)
{
	if (actor == nullptr || actor->Level == nullptr || generation == 0 || actor->ParticleTrailHistory.Size() == 0)
	{
		return false;
	}

	int runStart = -1;
	int runEnd = -1;
	for (int i = 0; i < (int)actor->ParticleTrailHistory.Size(); ++i)
	{
		if (actor->ParticleTrailHistory[i].Generation == generation)
		{
			if (runStart < 0) runStart = i;
			runEnd = i;
		}
		else if (runStart >= 0)
		{
			break;
		}
	}
	if (runStart < 0 || runEnd < runStart) return false;

	const unsigned runCount = (unsigned)(runEnd - runStart + 1);
	if (runCount < 2)
	{
		actor->ParticleTrailHistory.Delete((unsigned)runStart, runCount);
		return false;
	}

	DVisualThinker *carrier = DVisualThinker::NewVisualThinker(actor->Level, RUNTIME_CLASS(DVisualThinker));
	if (carrier == nullptr)
	{
		// Never leave a completed portal-side generation on the moved actor. If a
		// visual carrier cannot be allocated, dropping the old strip is safer than
		// drawing a cross-space bridge/spike.
		actor->ParticleTrailHistory.Delete((unsigned)runStart, runCount);
		return false;
	}

	carrier->ObjectFlags |= OF_Transient;
	carrier->bParticleTrailRibbonCarrier = true;
	carrier->ParticleTrailGeneration = generation;
	carrier->ParticleTrailLifetime = actor->ParticleTrailLifetime > 0.0 ? actor->ParticleTrailLifetime : 0.35;
	carrier->ParticleTrailScale = actor->ParticleTrailScale > 0.0 ? actor->ParticleTrailScale : 1.0;
	carrier->ParticleTrailAlpha = actor->ParticleTrailAlpha > 0.0 ?
		clamp<double>(actor->ParticleTrailAlpha, 0.0, 1.0) : 1.0;
	carrier->ParticleTrailTailAlphaFade = clamp<double>(actor->ParticleTrailTailAlphaFade, 0.0, 1.0);
	carrier->ParticleTrailHeadFeather = clamp<double>(actor->ParticleTrailHeadFeather, 0.0, 1.0);
	carrier->ParticleTrailWaveAmplitude = max<double>(actor->ParticleTrailWaveAmplitude, 0.0);
	carrier->ParticleTrailWaveFrequency = max<double>(actor->ParticleTrailWaveFrequency, 0.0);
	carrier->ParticleTrailWaveSpeed = actor->ParticleTrailWaveSpeed;
	carrier->ParticleTrailGlowScale = clamp<double>(actor->ParticleTrailGlowScale, 0.0, 8.0);
	carrier->ParticleTrailGlowAlpha = clamp<double>(actor->ParticleTrailGlowAlpha, 0.0, 1.0);
	carrier->ParticleTrailRadius = max<double>(fabs(actor->radius), 1.0);
	carrier->ParticleTrailColor = actor->bParticleTrailColorSet ? actor->ParticleTrailColor : PalEntry(255, 255, 255);
	carrier->ParticleTrailGlowColor = actor->bParticleTrailGlowColorSet ? actor->ParticleTrailGlowColor : carrier->ParticleTrailColor;

	carrier->ParticleTrailHistory.Clear();
	for (int i = runStart; i <= runEnd; ++i)
	{
		carrier->ParticleTrailHistory.Push(actor->ParticleTrailHistory[i]);
	}

	// [Nakara V21] The newest point is often exactly on a portal plane, blocking
	// line, or missile impact surface. PointInRenderSubsector() is ambiguous at such
	// boundaries and can register the carrier in the wrong/hidden subsector, making
	// an otherwise valid retired ribbon disappear. The carrier position is only a
	// render-registration anchor; ribbon vertices still use their exact saved path.
	// Therefore anchor one real sample behind the terminal point whenever possible.
	int anchorIndex = (int)carrier->ParticleTrailHistory.Size() - 1;
	if (anchorIndex > 0)
	{
		--anchorIndex;
	}
	const auto &anchor = carrier->ParticleTrailHistory[anchorIndex];
	carrier->ParticleTrailPortalGroup = anchor.PortalGroup;
	carrier->PT.Pos = anchor.Pos;
	carrier->Prev = carrier->PT.Pos;
	carrier->PT.Vel = FVector3(0.f, 0.f, 0.f);
	carrier->PT.Acc = FVector3(0.f, 0.f, 0.f);
	carrier->PT.alpha = 1.f;
	carrier->PT.color = int(carrier->ParticleTrailColor);
	carrier->flags |= VTF_DontInterpolate;
	carrier->UpdateSector();

	if (nk_ribbon_debug)
	{
		Printf("[RIBDBG:DETACH] tic=%d actor=%p class=%s gen=%u samples=%u anchor=(%.3f %.3f %.3f) group=%d\n",
			actor->Level->maptime, (void*)actor, actor->GetClass()->TypeName.GetChars(),
			(unsigned)generation, runCount, carrier->PT.Pos.X, carrier->PT.Pos.Y, carrier->PT.Pos.Z,
			carrier->ParticleTrailPortalGroup);
	}

	actor->ParticleTrailHistory.Delete((unsigned)runStart, runCount);
	return true;
}

void P_RetireParticleTrailRibbon(AActor *actor)
{
	if (actor == nullptr || actor->Level == nullptr || actor->ParticleTrailHistory.Size() == 0)
	{
		return;
	}

	// Capture the projectile's final visible point if its current generation has
	// not already ended there. This lets the detached trail finish all the way to
	// the destruction/explosion point instead of ending at the previous tic.
	if (actor->ParticleTrailHistoryMode == 2 && actor->ParticleTrailGeneration != 0)
	{
		const FParticleTrailHistorySample *last = P_GetLastParticleTrailSample(actor);
		const DVector3 finalPos = actor->Pos() + actor->WorldOffset;
		const int finalGroup = actor->Sector != nullptr ? actor->Sector->PortalGroup : actor->PrevPortalGroup;
		if (last != nullptr && finalGroup >= 0 && finalGroup < actor->Level->Displacements.size &&
			std::isfinite(finalPos.X) && std::isfinite(finalPos.Y) && std::isfinite(finalPos.Z) &&
			(last->PortalGroup != finalGroup || (last->Pos - finalPos).LengthSquared() > 0.000001))
		{
			FParticleTrailHistorySample finalSample;
			finalSample.Pos = finalPos;
			finalSample.SpawnTime = max<double>(last->SpawnTime, (double)actor->Level->maptime);
			DVector3 lastPathPos = last->Pos;
			if (last->PortalGroup != finalGroup && last->PortalGroup >= 0 &&
				last->PortalGroup < actor->Level->Displacements.size)
			{
				lastPathPos.XY() += actor->Level->Displacements.getOffset(last->PortalGroup, finalGroup);
			}
			const double finalPathStep = (finalPos - lastPathPos).Length();
			finalSample.PathDistance = max<double>(0.0, last->PathDistance) +
				(std::isfinite(finalPathStep) ? finalPathStep : 0.0);
			finalSample.PortalGroup = finalGroup;
			finalSample.Generation = actor->ParticleTrailGeneration;
			actor->ParticleTrailHistory.Push(finalSample);
		}
	}

	// Normally there is only one actor-owned generation after the portal fix, but
	// consume every run defensively so stale state from an older build cannot pop.
	while (actor->ParticleTrailHistory.Size() > 0)
	{
		const uint32_t generation = actor->ParticleTrailHistory[0].Generation;
		if (generation == 0)
		{
			actor->ParticleTrailHistory.Delete(0, 1);
			continue;
		}
		P_DetachParticleTrailGeneration(actor, generation);
	}

	actor->ParticleTrailGeneration = 0;
	actor->ParticleTrailHistoryMode = 0xff;
}

// [Nakara] Convert a stored ribbon endpoint into the actor's current portal
// coordinate space before continuing the per-actor center-line chain.
static bool P_GetParticleTrailHistoryPosInCurrentGroup(AActor *actor, const FParticleTrailHistorySample &sample, DVector3 &out)
{
	if (actor == nullptr || actor->Level == nullptr) return false;
	const int targetGroup = actor->Sector != nullptr ? actor->Sector->PortalGroup : actor->PrevPortalGroup;
	if (sample.PortalGroup < 0 || targetGroup < 0 ||
		sample.PortalGroup >= actor->Level->Displacements.size || targetGroup >= actor->Level->Displacements.size)
	{
		return false;
	}

	out = sample.Pos;
	out.XY() += actor->Level->Displacements.getOffset(sample.PortalGroup, targetGroup);
	return std::isfinite(out.X) && std::isfinite(out.Y) && std::isfinite(out.Z);
}

static FSpriteModelFrame *P_GetParticleTrailModelFrame(AActor *actor, bool respectPicnum)
{
	if (actor == nullptr) return nullptr;
	if (respectPicnum && actor->picnum.isValid()) return nullptr;
	if (actor->sprite < 0) return nullptr;
	return FindModelFrame(actor, actor->sprite, actor->frame, !!(actor->flags & MF_DROPPED));
}

static bool P_ResolveParticleTrailModelSource(AActor *actor, FSpriteModelFrame **outModelFrame)
{
	if (outModelFrame) *outModelFrame = nullptr;
	if (actor == nullptr) return false;

	if (actor->ParticleTrailSourceMode == PTTRL_Sprite)
	{
		return false;
	}

	if (actor->ParticleTrailSourceMode == PTTRL_Model)
	{
		auto *model = P_GetParticleTrailModelFrame(actor, false);
		if (outModelFrame) *outModelFrame = model;
		return model != nullptr;
	}

	// Auto follows what HWSprite::Process would actually render: direct picnum
	// overrides stay sprites, otherwise a valid MODELDEF frame selects the model.
	auto *model = P_GetParticleTrailModelFrame(actor, true);
	if (outModelFrame) *outModelFrame = model;
	return model != nullptr;
}

static void P_UpdateParticleTrailRibbonHistory(AActor *actor)
{
	// [Nakara] Ribbon history is deliberately based on this actor's own recorded
	// center-line endpoints, never AActor::Prev. Prev is renderer interpolation
	// state and can be reset/relocated independently of the visual trail. Using it
	// as the ribbon source can inject (0,0,0) or another unrelated interpolation
	// position into an otherwise valid chain.
	const DVector3 currentPos = actor->Pos() + actor->WorldOffset;
	const int currentGroup = actor->Sector != nullptr ? actor->Sector->PortalGroup : actor->PrevPortalGroup;
	if (!std::isfinite(currentPos.X) || !std::isfinite(currentPos.Y) || !std::isfinite(currentPos.Z) ||
		currentGroup < 0 || currentGroup >= actor->Level->Displacements.size)
	{
		actor->ParticleTrailHistory.Clear();
		actor->ParticleTrailGeneration = 0;
		actor->ParticleTrailHistoryMode = 0xff;
		return;
	}

	bool newGeneration = actor->ParticleTrailHistoryMode != 2 || actor->ParticleTrailGeneration == 0;
	const FParticleTrailHistorySample *lastSample = nullptr;
	if (!newGeneration)
	{
		lastSample = P_GetLastParticleTrailSample(actor);
		if (lastSample == nullptr || !std::isfinite(lastSample->SpawnTime))
		{
			newGeneration = true;
		}
		else
		{
			// Ordinary end-of-tic endpoints arrive here with SpawnTime == maptime.
			// An exact portal-exit anchor may instead lie *inside this tic* at
			// maptime + crossingFraction. Keep that generation alive so P_RunEffect
			// can continue from the exit crossing to the final actor position.
			const double ticStart = (double)actor->Level->maptime;
			const double gapToThisTic = ticStart - lastSample->SpawnTime;
			if (gapToThisTic > 1.000001 || lastSample->SpawnTime > ticStart + 1.000001)
			{
				newGeneration = true;
			}
		}
	}

	auto beginGenerationAtPosition = [&](bool preservePreviousGenerations,
		const DVector3 &anchorPos, double anchorTime, int anchorGroup, const char *anchorSource)
	{
		// A portal split keeps the entrance-side generation alive so it can fade
		// naturally. Normal activation/discontinuity resets still start from a clean
		// history. The new generation is always independent from every older strip.
		if (!preservePreviousGenerations)
		{
			actor->ParticleTrailHistory.Clear();
		}
		actor->ParticleTrailGeneration = P_NextParticleTrailGeneration();
		actor->ParticleTrailHistoryMode = 2;

		FParticleTrailHistorySample anchor;
		anchor.Pos = anchorPos;
		anchor.SpawnTime = anchorTime;
		anchor.PathDistance = 0.0;
		anchor.PortalGroup = anchorGroup;
		anchor.Generation = actor->ParticleTrailGeneration;
		actor->ParticleTrailHistory.Push(anchor);

		if (nk_ribbon_debug && anchor.Pos.LengthSquared() <= 0.01)
		{
			Printf("[RIBDBG:PUSH-ORIGIN] stage=anchor tic=%d actor=%p class=%s gen=%u size=%u "
				"source=%s rawpos=(%.6f %.6f %.6f) actorpos=(%.6f %.6f %.6f) worldoff=(%.6f %.6f %.6f) group=%d\n",
				actor->Level->maptime, (void*)actor, actor->GetClass()->TypeName.GetChars(),
				(unsigned)actor->ParticleTrailGeneration, actor->ParticleTrailHistory.Size(), anchorSource,
				anchor.Pos.X, anchor.Pos.Y, anchor.Pos.Z, actor->Pos().X, actor->Pos().Y, actor->Pos().Z,
				actor->WorldOffset.X, actor->WorldOffset.Y, actor->WorldOffset.Z, anchorGroup);
		}

		if (nk_ribbon_debug)
		{
			Printf("[RIBDBG:HIST-BEGIN] tic=%d actor=%p class=%s gen=%u source=%s "
				"anchor=(%.3f %.3f %.3f) time=%.6f current=(%.3f %.3f %.3f) "
				"prev=(%.3f %.3f %.3f) worldoff=(%.3f %.3f %.3f) group=%d prevgroup=%d\n",
				actor->Level->maptime, (void*)actor, actor->GetClass()->TypeName.GetChars(),
				(unsigned)actor->ParticleTrailGeneration, anchorSource,
				anchor.Pos.X, anchor.Pos.Y, anchor.Pos.Z, anchor.SpawnTime,
				currentPos.X, currentPos.Y, currentPos.Z,
				actor->Prev.X, actor->Prev.Y, actor->Prev.Z,
				actor->WorldOffset.X, actor->WorldOffset.Y, actor->WorldOffset.Z,
				anchorGroup, actor->PrevPortalGroup);
		}
	};

	auto beginGenerationAtCurrentPosition = [&](bool preservePreviousGenerations)
	{
		beginGenerationAtPosition(preservePreviousGenerations, currentPos,
			(double)actor->Level->maptime + 1.0, currentGroup, "current");
	};

	if (newGeneration)
	{
		// If this actor was created during the tic that is currently being sampled,
		// begin the ribbon at the exact pre-first-Tick visual position captured by
		// CallPostBeginPlay(). This restores the projectile's real launch segment
		// without reintroducing AActor::Prev as a trail source. Older/stale launch
		// anchors are ignored when an effect is enabled later in the actor's life.
		const bool preservePreviousGenerations = actor->ParticleTrailHistoryMode == 2 &&
			actor->ParticleTrailGeneration == 0 && actor->ParticleTrailHistory.Size() > 0;
		const bool launchAnchorValid = !preservePreviousGenerations && actor->bParticleTrailLaunchAnchorValid &&
			actor->ParticleTrailLaunchMapTime == actor->Level->maptime &&
			actor->ParticleTrailLaunchPortalGroup >= 0 &&
			actor->ParticleTrailLaunchPortalGroup < actor->Level->Displacements.size &&
			std::isfinite(actor->ParticleTrailLaunchPosition.X) &&
			std::isfinite(actor->ParticleTrailLaunchPosition.Y) &&
			std::isfinite(actor->ParticleTrailLaunchPosition.Z);

		if (launchAnchorValid && actor->ParticleTrailLaunchPortalGroup == currentGroup)
		{
			beginGenerationAtPosition(false, actor->ParticleTrailLaunchPosition,
				(double)actor->Level->maptime, actor->ParticleTrailLaunchPortalGroup, "launch");
			actor->bParticleTrailLaunchAnchorValid = false;
			lastSample = P_GetLastParticleTrailSample(actor);
			if (lastSample == nullptr) return;
			// Continue below and sample launch -> currentPos during this same tic.
		}
		else
		{
			// A first-tic portal crossing is normally initialized by the explicit
			// portal split hook before P_RunEffect. If a launch anchor is in another
			// coordinate space here, do not bridge it implicitly.
			if (actor->ParticleTrailLaunchMapTime <= actor->Level->maptime)
			{
				actor->bParticleTrailLaunchAnchorValid = false;
			}
			beginGenerationAtCurrentPosition(preservePreviousGenerations);
			return;
		}
	}

	// [Nakara] A line/linked portal transition is a split ribbon boundary. Even
	// though linked portal groups have a displacement that can express the old
	// point in the destination coordinate system, joining those two endpoints as
	// one strip makes the entrance-side tail snap across to the exit space. Start
	// a fresh generation instead; the ribbon resumes from the actor's real position
	// on the destination side and never draws a segment across portal spaces.
	if (lastSample->PortalGroup != currentGroup)
	{
		if (nk_ribbon_debug)
		{
			Printf("[RIBDBG:PORTAL-SPLIT] tic=%d actor=%p class=%s gen=%u "
				"last=(%.3f %.3f %.3f) lastgroup=%d current=(%.3f %.3f %.3f) currentgroup=%d\n",
				actor->Level->maptime, (void*)actor, actor->GetClass()->TypeName.GetChars(),
				(unsigned)actor->ParticleTrailGeneration,
				lastSample->Pos.X, lastSample->Pos.Y, lastSample->Pos.Z, lastSample->PortalGroup,
				currentPos.X, currentPos.Y, currentPos.Z, currentGroup);
		}
		const uint32_t completedGeneration = actor->ParticleTrailGeneration;
		P_DetachParticleTrailGeneration(actor, completedGeneration);
		beginGenerationAtCurrentPosition(false);
		return;
	}

	// Resolve the previous endpoint into the actor's current portal group. This is
	// the only start point accepted for a continued ribbon chain.
	DVector3 start;
	if (!P_GetParticleTrailHistoryPosInCurrentGroup(actor, *lastSample, start))
	{
		if (nk_ribbon_debug)
		{
			Printf("[RIBDBG:HIST-PORTAL-FAIL] tic=%d actor=%p class=%s gen=%u "
				"raw=(%.3f %.3f %.3f) rawgroup=%d current=(%.3f %.3f %.3f) currentgroup=%d\n",
				actor->Level->maptime, (void*)actor, actor->GetClass()->TypeName.GetChars(),
				(unsigned)actor->ParticleTrailGeneration,
				lastSample->Pos.X, lastSample->Pos.Y, lastSample->Pos.Z, lastSample->PortalGroup,
				currentPos.X, currentPos.Y, currentPos.Z, currentGroup);
		}
		beginGenerationAtCurrentPosition(false);
		return;
	}

	// Defensive origin guard. If an older build/state managed to leave an exact
	// world-origin endpoint in the current chain, never bridge from it to an actor
	// that is actually elsewhere. This is intentionally secondary to the main fix:
	// new V4 samples themselves never use Prev, so they cannot manufacture this node.
	constexpr double ORIGIN_EPSILON_SQUARED = 0.000001;
	if (start.LengthSquared() <= ORIGIN_EPSILON_SQUARED && currentPos.LengthSquared() > ORIGIN_EPSILON_SQUARED)
	{
		beginGenerationAtCurrentPosition(false);
		return;
	}

	const DVector3 movement = currentPos - start;
	const double distance = movement.Length();
	if (nk_ribbon_debug)
	{
		const double originEpsilonSquared = 0.01;
		const double expectedStep = max<double>(8.0, actor->Vel.Length() * 2.0 + 4.0);
		const bool startAtOrigin = start.LengthSquared() <= originEpsilonSquared && currentPos.LengthSquared() > originEpsilonSquared;
		const bool currentAtOrigin = currentPos.LengthSquared() <= originEpsilonSquared && start.LengthSquared() > originEpsilonSquared;
		const bool longStep = distance > expectedStep;
		if (startAtOrigin || currentAtOrigin || longStep)
		{
			Printf("[RIBDBG:HIST-SEG] tic=%d actor=%p class=%s gen=%u reason=%s%s%s "
				"start=(%.3f %.3f %.3f) current=(%.3f %.3f %.3f) dist=%.3f "
				"vel=(%.3f %.3f %.3f) expected=%.3f rawlast=(%.3f %.3f %.3f) groups=%d->%d\n",
				actor->Level->maptime, (void*)actor, actor->GetClass()->TypeName.GetChars(),
				(unsigned)actor->ParticleTrailGeneration,
				startAtOrigin ? "START_ORIGIN " : "", currentAtOrigin ? "CURRENT_ORIGIN " : "", longStep ? "LONG " : "",
				start.X, start.Y, start.Z, currentPos.X, currentPos.Y, currentPos.Z, distance,
				actor->Vel.X, actor->Vel.Y, actor->Vel.Z, expectedStep,
				lastSample->Pos.X, lastSample->Pos.Y, lastSample->Pos.Z, lastSample->PortalGroup, currentGroup);
		}
	}
	if (distance <= 0.0001)
	{
		return;
	}

	const double density = actor->ParticleTrailDensity > 0.0 ? clamp<double>(actor->ParticleTrailDensity, 0.05, 16.0) : 1.0;

	// History-driven trails do not need overlapping copies to look continuous.
	// Density controls path resolution only: higher values reduce the distance
	// between path samples and therefore smooth curves without darkening them.
	const double visualRadius = max<double>(actor->radius, actor->Height * 0.25);
	const double baseSpacing = clamp<double>(visualRadius * 0.45, 3.0, 16.0);
	const double sampleSpacing = baseSpacing / density;
	int sampleCount = clamp<int>((int)ceil(distance / sampleSpacing), 1, PARTICLETRAIL_MAX_HISTORY_SAMPLES_PER_TIC);

	// Sample from the previous *ribbon endpoint* to the actor's current Pos. Using
	// (i + 1) rather than midpoint fractions guarantees that the final history
	// sample is the exact current position. That endpoint becomes the sole start
	// point for the next tic, so the chain is self-contained per actor.
	const int startGroup = lastSample->PortalGroup;
	// Normally the previous endpoint is exactly at this tic's start. After a
	// line-portal split it is the exact exit crossing time within this tic. Map
	// the post-portal samples only across the remaining sub-tic interval so sample
	// timestamps stay monotonic and match the renderer's TicFrac interpolation.
	const double ticStart = (double)actor->Level->maptime;
	const double ticEnd = ticStart + 1.0;
	const double segmentStartTime = clamp<double>(lastSample->SpawnTime, ticStart, ticEnd);
	for (int i = 0; i < sampleCount; ++i)
	{
		const double frac = ((double)i + 1.0) / sampleCount;
		FParticleTrailHistorySample sample;
		sample.Pos = start + movement * frac;
		sample.SpawnTime = segmentStartTime + (ticEnd - segmentStartTime) * frac;
		// [Nakara V25.2] Wave phase follows a distance stamped at emission time.
		// Never derive this from the current head during rendering: growing the trail
		// would then re-phase every older node and make moving waves fold together.
		sample.PathDistance = max<double>(0.0, lastSample->PathDistance) + distance * frac;
		sample.PortalGroup = currentGroup;
		sample.Generation = actor->ParticleTrailGeneration;
		actor->ParticleTrailHistory.Push(sample);
		if (nk_ribbon_debug && sample.Pos.LengthSquared() <= 0.01)
		{
			Printf("[RIBDBG:PUSH-ORIGIN] stage=sample tic=%d actor=%p class=%s gen=%u sample=%d/%d size=%u "
				"start=(%.6f %.6f %.6f) current=(%.6f %.6f %.6f) samplepos=(%.6f %.6f %.6f) "
				"rawactor=(%.6f %.6f %.6f) worldoff=(%.6f %.6f %.6f) groups=%d->%d\n",
				actor->Level->maptime, (void*)actor, actor->GetClass()->TypeName.GetChars(),
				(unsigned)actor->ParticleTrailGeneration, i, sampleCount, actor->ParticleTrailHistory.Size(),
				start.X, start.Y, start.Z, currentPos.X, currentPos.Y, currentPos.Z,
				sample.Pos.X, sample.Pos.Y, sample.Pos.Z, actor->Pos().X, actor->Pos().Y, actor->Pos().Z,
				actor->WorldOffset.X, actor->WorldOffset.Y, actor->WorldOffset.Z, startGroup, currentGroup);
		}
	}

	// Keep enough path samples for each generation that a high-density ribbon does
	// not evict its exact portal-exit seam a few tics after crossing. Expired samples
	// from older generations are removed by time, with a 128-sample emergency cap
	// for actors that cross many portals in a very short period.
	unsigned currentGenerationStart = actor->ParticleTrailHistory.Size();
	while (currentGenerationStart > 0 &&
		actor->ParticleTrailHistory[currentGenerationStart - 1].Generation == actor->ParticleTrailGeneration)
	{
		--currentGenerationStart;
	}
	const unsigned currentGenerationCount = actor->ParticleTrailHistory.Size() - currentGenerationStart;
	if (currentGenerationCount > PARTICLETRAIL_MAX_HISTORY_SAMPLES)
	{
		const unsigned deleteCount = currentGenerationCount - PARTICLETRAIL_MAX_HISTORY_SAMPLES;
		actor->ParticleTrailHistory.Delete(currentGenerationStart, deleteCount);
		if (nk_ribbon_debug)
		{
			Printf("[RIBDBG:HIST-TRIM-GEN] tic=%d actor=%p class=%s gen=%u delete=%u remainGen=%u total=%u\n",
				actor->Level->maptime, (void*)actor, actor->GetClass()->TypeName.GetChars(),
				(unsigned)actor->ParticleTrailGeneration, deleteCount,
				(unsigned)PARTICLETRAIL_MAX_HISTORY_SAMPLES, actor->ParticleTrailHistory.Size());
		}
	}

	const double lifetimeSeconds = actor->ParticleTrailLifetime > 0.0 ? actor->ParticleTrailLifetime : 0.35;
	const double lifetimeTicks = max<double>(1.0, lifetimeSeconds * TICRATE);
	unsigned expiredCount = 0;
	while (expiredCount < actor->ParticleTrailHistory.Size())
	{
		const auto &sample = actor->ParticleTrailHistory[expiredCount];
		if (!std::isfinite(sample.SpawnTime) ||
			(double)actor->Level->maptime - sample.SpawnTime >= lifetimeTicks)
		{
			++expiredCount;
			continue;
		}
		break;
	}
	if (expiredCount > 0)
	{
		actor->ParticleTrailHistory.Delete(0, expiredCount);
	}

	if (actor->ParticleTrailHistory.Size() > PARTICLETRAIL_MAX_RETAINED_HISTORY_SAMPLES)
	{
		const unsigned deleteCount = actor->ParticleTrailHistory.Size() - PARTICLETRAIL_MAX_RETAINED_HISTORY_SAMPLES;
		actor->ParticleTrailHistory.Delete(0, deleteCount);
		if (nk_ribbon_debug)
		{
			Printf("[RIBDBG:HIST-TRIM-TOTAL] tic=%d actor=%p class=%s gen=%u delete=%u total=%u cap=%u\n",
				actor->Level->maptime, (void*)actor, actor->GetClass()->TypeName.GetChars(),
				(unsigned)actor->ParticleTrailGeneration, deleteCount, actor->ParticleTrailHistory.Size(),
				(unsigned)PARTICLETRAIL_MAX_RETAINED_HISTORY_SAMPLES);
		}
	}

}

static void P_RunParticleTrailAfterImage(AActor *actor, const DVector3 &movement, double distance)
{
	FTextureID patch;
	bool frameMirror = false;
	if (actor->picnum.isValid())
	{
		patch = actor->picnum;
	}
	else
	{
		if (actor->sprite < 0 || actor->sprite >= (int)sprites.Size())
		{
			return;
		}
		patch = sprites[actor->sprite].GetSpriteFrame(actor->frame, 0, nullAngle, &frameMirror,
			!!(actor->renderflags & RF_SPRITEFLIP));
	}

	if (!patch.isValid())
	{
		return;
	}

	auto tex = TexMan.GetGameTexture(patch, false);
	if (tex == nullptr || !tex->isValid())
	{
		return;
	}

	const double lifetimeSeconds = actor->ParticleTrailLifetime > 0.0 ? actor->ParticleTrailLifetime : 0.35;
	const int lifetime = clamp<int>((int)(lifetimeSeconds * TICRATE + 0.5), 1, TICRATE * 30);
	const double alpha = actor->ParticleTrailAlpha > 0.0 ? clamp<double>(actor->ParticleTrailAlpha, 0.0, 1.0) : 1.0;
	const double trailScale = actor->ParticleTrailScale > 0.0 ? actor->ParticleTrailScale : 1.0;
	const double density = actor->ParticleTrailDensity > 0.0 ? clamp<double>(actor->ParticleTrailDensity, 0.05, 16.0) : 1.0;
	const PalEntry color = actor->bParticleTrailColorSet ? actor->ParticleTrailColor : PalEntry(255, 255, 255);
	const ERenderStyle trailStyle = actor->ParticleTrailColorMode == PTTRL_ColorTint
		? STYLE_Translucent
		: STYLE_TranslucentStencil;

	const double spriteScaleX = fabs(actor->Scale.X) * trailScale;
	const double spriteScaleY = fabs(actor->Scale.Y) * trailScale;
	const bool flipX = frameMirror ^ !!(actor->renderflags & RF_XFLIP) ^ !!(actor->renderflags & RF_SPRITEFLIP);
	const double encodedScaleY = flipX ? -spriteScaleY : spriteScaleY;
	if (spriteScaleX <= 0.0001 || spriteScaleY <= 0.0001)
	{
		return;
	}

	const double visualWidth = fabs(tex->GetDisplayWidth() * spriteScaleX);
	const double visualHeight = fabs(tex->GetDisplayHeight() * spriteScaleY);
	const double visualSize = max<double>(visualWidth, visualHeight);
	const double baseSpacing = clamp<double>(visualSize * 0.25, 3.0, 16.0);
	const double sampleSpacing = baseSpacing / density;

	int spawnCount = (int)ceil(distance / sampleSpacing);
	spawnCount = clamp<int>(spawnCount, 1, PARTICLETRAIL_MAX_SPAWNS_PER_TIC);

	const double fadeStep = alpha / lifetime;
	const DVector3 start = actor->Prev + actor->WorldOffset;

	for (int i = 0; i < spawnCount; ++i)
	{
		const double pathFrac = ((double)i + 0.5) / spawnCount;
		const DVector3 pos = start + movement * pathFrac;

		P_SpawnParticle(actor->Level, pos, DVector3(), DVector3(), color,
			alpha, lifetime, spriteScaleX, fadeStep, 0.0, SPF_FULLBRIGHT,
			patch, trailStyle, 0.0, 0.0, 0.0, encodedScaleY);
	}
}

void P_SplitParticleTrailRibbonForPortal(AActor *actor,
	const DVector3 &entryPos, int entryPortalGroup, const DVector3 &entryTangent,
	const DVector3 &exitPos, int exitPortalGroup, const DVector3 &exitTangent,
	double crossingTime)
{
	if (actor == nullptr || actor->Level == nullptr) return;
	if (!(actor->effects & FX_PARTICLETRAIL)) return;

	// Afterimage trails are independent particle_t objects. Ribbon mode keeps the
	// source-side generation alive, terminates it at the exact portal crossing,
	// and immediately starts a new generation at the transformed exit crossing.
	// This gives both strips a real endpoint on their own side of the portal while
	// making it structurally impossible for one strip to bridge the two spaces.
	if (!std::isfinite(entryPos.X) || !std::isfinite(entryPos.Y) || !std::isfinite(entryPos.Z) ||
		!std::isfinite(exitPos.X) || !std::isfinite(exitPos.Y) || !std::isfinite(exitPos.Z) ||
		!std::isfinite(crossingTime)) return;
	if (entryPortalGroup < 0 || exitPortalGroup < 0 ||
		entryPortalGroup >= actor->Level->Displacements.size ||
		exitPortalGroup >= actor->Level->Displacements.size) return;

	// The actor can cross a line portal during its very first simulation tic,
	// before the end-of-tic P_RunEffect pass has created any ribbon history. Seed
	// that first generation from the exact launch anchor so the entrance-side strip
	// still reaches from launch -> portal entry instead of silently losing a tic.
	if (actor->ParticleTrailHistoryMode != 2 || actor->ParticleTrailHistory.Size() == 0 ||
		actor->ParticleTrailGeneration == 0)
	{
		const bool launchAnchorValid = actor->bParticleTrailLaunchAnchorValid &&
			actor->ParticleTrailLaunchMapTime == actor->Level->maptime &&
			actor->ParticleTrailLaunchPortalGroup == entryPortalGroup &&
			entryPortalGroup >= 0 && entryPortalGroup < actor->Level->Displacements.size &&
			std::isfinite(actor->ParticleTrailLaunchPosition.X) &&
			std::isfinite(actor->ParticleTrailLaunchPosition.Y) &&
			std::isfinite(actor->ParticleTrailLaunchPosition.Z);
		if (!launchAnchorValid) return;

		actor->ParticleTrailHistory.Clear();
		actor->ParticleTrailGeneration = P_NextParticleTrailGeneration();
		actor->ParticleTrailHistoryMode = 2;

		FParticleTrailHistorySample launchSample;
		launchSample.Pos = actor->ParticleTrailLaunchPosition;
		launchSample.SpawnTime = (double)actor->Level->maptime;
		launchSample.PathDistance = 0.0;
		launchSample.PortalGroup = actor->ParticleTrailLaunchPortalGroup;
		launchSample.Generation = actor->ParticleTrailGeneration;
		actor->ParticleTrailHistory.Push(launchSample);
		actor->bParticleTrailLaunchAnchorValid = false;

		if (nk_ribbon_debug)
		{
			Printf("[RIBDBG:LAUNCH-PORTAL-SEED] tic=%d actor=%p class=%s gen=%u "
				"launch=(%.3f %.3f %.3f) group=%d\n",
				actor->Level->maptime, (void*)actor, actor->GetClass()->TypeName.GetChars(),
				(unsigned)actor->ParticleTrailGeneration,
				launchSample.Pos.X, launchSample.Pos.Y, launchSample.Pos.Z, launchSample.PortalGroup);
		}
	}

	const uint32_t oldGeneration = actor->ParticleTrailGeneration;
	int lastSampleIndex = -1;
	for (int i = (int)actor->ParticleTrailHistory.Size() - 1; i >= 0; --i)
	{
		if (actor->ParticleTrailHistory[i].Generation == oldGeneration)
		{
			lastSampleIndex = i;
			break;
		}
	}
	if (lastSampleIndex < 0) return;

	const FParticleTrailHistorySample lastSampleBeforeSplit = actor->ParticleTrailHistory[lastSampleIndex];
	double crossingPathDistance = max<double>(0.0, lastSampleBeforeSplit.PathDistance);
	DVector3 lastPathPos = lastSampleBeforeSplit.Pos;
	if (lastSampleBeforeSplit.PortalGroup != entryPortalGroup &&
		lastSampleBeforeSplit.PortalGroup >= 0 &&
		lastSampleBeforeSplit.PortalGroup < actor->Level->Displacements.size)
	{
		lastPathPos.XY() += actor->Level->Displacements.getOffset(lastSampleBeforeSplit.PortalGroup, entryPortalGroup);
	}
	const double crossingPathStep = (entryPos - lastPathPos).Length();
	if (std::isfinite(crossingPathStep)) crossingPathDistance += crossingPathStep;

	// A TryMove can cross more than one portal in one game tic. bestfrac is local
	// to each crossing attempt, so never allow a later split timestamp to move
	// backwards relative to the current generation's latest endpoint.
	const double ticStart = (double)actor->Level->maptime;
	const double ticEnd = ticStart + 1.0;
	double exactCrossingTime = clamp<double>(crossingTime, ticStart, ticEnd);
	if (std::isfinite(lastSampleBeforeSplit.SpawnTime))
	{
		exactCrossingTime = max<double>(exactCrossingTime, lastSampleBeforeSplit.SpawnTime);
	}

	auto normalizeSeamTangent = [](DVector3 tangent)
	{
		if (!std::isfinite(tangent.X) || !std::isfinite(tangent.Y) || !std::isfinite(tangent.Z) ||
			tangent.LengthSquared() <= 0.000001)
		{
			return DVector3(0.0, 0.0, 0.0);
		}
		tangent.MakeUnit();
		return tangent;
	};

	DVector3 sourceSeamTangent = normalizeSeamTangent(entryTangent);
	if (sourceSeamTangent.LengthSquared() <= 0.000001)
	{
		sourceSeamTangent = normalizeSeamTangent(entryPos - lastSampleBeforeSplit.Pos);
	}
	const DVector3 destinationSeamTangent = normalizeSeamTangent(exitTangent);

	// Finish the entrance strip at the real source-line intersection. The endpoint
	// also carries the source-space tangent so the renderer can lock the ribbon
	// frame to the portal seam instead of independently billboard-seeding each
	// split generation. If the exact crossing is already the last node, upgrade
	// that node in place rather than creating a zero-length segment.
	if (lastSampleBeforeSplit.PortalGroup != entryPortalGroup ||
		(lastSampleBeforeSplit.Pos - entryPos).LengthSquared() > 0.000001)
	{
		FParticleTrailHistorySample entrySample;
		entrySample.Pos = entryPos;
		entrySample.SpawnTime = exactCrossingTime;
		entrySample.PathDistance = crossingPathDistance;
		entrySample.PortalGroup = entryPortalGroup;
		entrySample.Generation = oldGeneration;
		entrySample.PortalSeamTangent = sourceSeamTangent;
		entrySample.PortalSeamFlags = PTHSF_PortalEntry;
		actor->ParticleTrailHistory.Push(entrySample);
	}
	else
	{
		auto &entrySample = actor->ParticleTrailHistory[lastSampleIndex];
		entrySample.SpawnTime = exactCrossingTime;
		crossingPathDistance = max<double>(0.0, entrySample.PathDistance);
		entrySample.PortalGroup = entryPortalGroup;
		entrySample.PortalSeamTangent = sourceSeamTangent;
		entrySample.PortalSeamFlags |= PTHSF_PortalEntry;
	}

	// The source-side generation is now complete. Detach it *before* the actor
	// continues in the destination space, so later renderer passes cannot reinterpret
	// the old center-line through the actor's new portal context.
	P_DetachParticleTrailGeneration(actor, oldGeneration);

	// The exact exit point is the first node of a new independent generation.
	// Store the portal-rotated destination tangent on the matching endpoint. The
	// entry and exit nodes intentionally share the exact same SpawnTime so their
	// full-width seam endpoints expire together.
	actor->ParticleTrailGeneration = P_NextParticleTrailGeneration();
	actor->ParticleTrailHistoryMode = 2;

	FParticleTrailHistorySample exitSample;
	exitSample.Pos = exitPos;
	exitSample.SpawnTime = exactCrossingTime;
	// Entry and exit are the same point in path-distance space. Keeping the
	// cumulative coordinate continuous also keeps Wave phase continuous through
	// rotated line portals without tying it to either generation's moving head.
	exitSample.PathDistance = crossingPathDistance;
	exitSample.PortalGroup = exitPortalGroup;
	exitSample.Generation = actor->ParticleTrailGeneration;
	exitSample.PortalSeamTangent = destinationSeamTangent;
	exitSample.PortalSeamFlags = PTHSF_PortalExit;
	actor->ParticleTrailHistory.Push(exitSample);

	if (nk_ribbon_debug)
	{
		Printf("[RIBDBG:LINEPORTAL-SPLIT-EXACT] tic=%d actor=%p class=%s oldgen=%u newgen=%u history=%u "
			"time=%.6f entry=(%.3f %.3f %.3f) entrygroup=%d entrytan=(%.4f %.4f %.4f) "
			"exit=(%.3f %.3f %.3f) exitgroup=%d exittan=(%.4f %.4f %.4f)\n",
			actor->Level->maptime, (void*)actor, actor->GetClass()->TypeName.GetChars(),
			(unsigned)oldGeneration, (unsigned)actor->ParticleTrailGeneration, actor->ParticleTrailHistory.Size(),
			exactCrossingTime, entryPos.X, entryPos.Y, entryPos.Z, entryPortalGroup,
			sourceSeamTangent.X, sourceSeamTangent.Y, sourceSeamTangent.Z,
			exitPos.X, exitPos.Y, exitPos.Z, exitPortalGroup,
			destinationSeamTangent.X, destinationSeamTangent.Y, destinationSeamTangent.Z);
	}
}


static void P_RunParticleTrail(AActor *actor)
{
	if (actor == nullptr || actor->Level == nullptr || !r_particles)
	{
		return;
	}

	FSpriteModelFrame *modelFrame = nullptr;
	const bool modelSource = P_ResolveParticleTrailModelSource(actor, &modelFrame);

	// Forced model mode intentionally has no sprite fallback.
	if (actor->ParticleTrailSourceMode == PTTRL_Model && modelFrame == nullptr)
	{
		actor->ParticleTrailHistory.Clear();
		actor->ParticleTrailGeneration = 0;
		actor->ParticleTrailHistoryMode = 0xff;
		return;
	}

	const bool ribbon = modelSource || actor->ParticleTrailSpriteMode == PTTRL2D_Ribbon;
	if (ribbon)
	{
		// Ribbon mode owns its own per-actor endpoint history. Do not calculate or
		// validate the path through AActor::Prev anywhere on this code path.
		P_UpdateParticleTrailRibbonHistory(actor);
		return;
	}

	// Switching back to afterimages discards ribbon history immediately so a
	// stationary actor cannot leave the old connected mesh active for another tic.
	if (actor->ParticleTrailHistory.Size() != 0)
	{
		actor->ParticleTrailHistory.Clear();
	}
	actor->ParticleTrailGeneration = 0;
	actor->ParticleTrailHistoryMode = 0;

	// AfterImage mode intentionally keeps the engine's ordinary Prev->Pos sampling;
	// it spawns independent particle_t instances and is not a connected ribbon.
	DVector3 movement = actor->Pos() - actor->Prev;
	const double distance = movement.Length();
	if (distance <= 0.0001)
	{
		return;
	}

	P_RunParticleTrailAfterImage(actor, movement, distance);
}

void P_RunEffect (AActor *actor, int effects)
{
	DAngle moveangle = actor->Vel.Angle();

	particle_t *particle;
	int i;

	if (effects & FX_PARTICLETRAIL)
	{
		P_RunParticleTrail(actor);
	}

	if ((effects & FX_ROCKET) && (cl_rockettrails & 1))
	{
		// Rocket trail
		double backx = -actor->radius * 2 * moveangle.Cos();
		double backy = -actor->radius * 2 * moveangle.Sin();
		double backz = actor->Height * ((2. / 3) - actor->Vel.Z / 8);

		DAngle an = moveangle + DAngle::fromDeg(90.);
		double speed;

		particle = JitterParticle (actor->Level, 3 + (M_Random() & 31));
		if (particle) {
			double pathdist = M_Random() / 256.;
			DVector3 pos = actor->Vec3Offset(
				backx - actor->Vel.X * pathdist,
				backy - actor->Vel.Y * pathdist,
				backz - actor->Vel.Z * pathdist);
			particle->Pos = pos;
			speed = (M_Random () - 128) * (1./200);
			particle->Vel.X += speed * an.Cos();
			particle->Vel.Y += speed * an.Sin();
			particle->Vel.Z -= 1.f/36;
			particle->Acc.Z -= 1.f/20;
			particle->color = yellow;
			particle->size = 2;
		}
		for (i = 6; i; i--) {
			particle_t *particle = JitterParticle (actor->Level, 3 + (M_Random() & 31));
			if (particle) {
				double pathdist = M_Random() / 256.;
				DVector3 pos = actor->Vec3Offset(
					backx - actor->Vel.X * pathdist,
					backy - actor->Vel.Y * pathdist,
					backz - actor->Vel.Z * pathdist + (M_Random() / 64.));
				particle->Pos = pos;

				speed = (M_Random () - 128) * (1./200);
				particle->Vel.X += speed * an.Cos();
				particle->Vel.Y += speed * an.Sin();
				particle->Vel.Z += 1.f / 80;
				particle->Acc.Z += 1.f / 40;
				if (M_Random () & 7)
					particle->color = grey2;
				else
					particle->color = grey1;
				particle->size = 3;
			} else
				break;
		}
	}
	if ((effects & FX_GRENADE) && (cl_rockettrails & 1))
	{
		// Grenade trail

		DVector3 pos = actor->Vec3Angle(-actor->radius * 2, moveangle, -actor->Height * actor->Vel.Z / 8 + actor->Height * (2. / 3));

		P_DrawSplash2 (actor->Level, 6, pos, moveangle + DAngle::fromDeg(180), 2, 2);
	}
	if (actor->fountaincolor)
	{
		// Particle fountain

		static const int *fountainColors[16] = 
			{ &black,	&black,
			  &red,		&red1,
			  &green,	&green1,
			  &blue,	&blue1,
			  &yellow,	&yellow1,
			  &purple,	&purple1,
			  &black,	&grey3,
			  &grey4,	&white
			};
		int color = actor->fountaincolor*2;
		if (color < 0 || color >= 16) color = 0;
		MakeFountain (actor, *fountainColors[color], *fountainColors[color+1]);
	}
	if (effects & FX_RESPAWNINVUL)
	{
		// Respawn protection

		static const int *protectColors[2] = { &yellow1, &white };

		for (i = 3; i > 0; i--)
		{
			particle = JitterParticle (actor->Level, 16);
			if (particle != NULL)
			{
				DAngle ang = DAngle::fromDeg(M_Random() * (360 / 256.));
				DVector3 pos = actor->Vec3Angle(actor->radius, ang, 0);
				particle->Pos = pos;
				particle->color = *protectColors[M_Random() & 1];
				particle->Vel.Z = 1;
				particle->Acc.Z = M_Random () / 512.;
				particle->size = 1;
				if (M_Random () < 128)
				{ // make particle fall from top of actor
					particle->Pos.Z += actor->Height;
					particle->Vel.Z = -particle->Vel.Z;
					particle->Acc.Z = -particle->Acc.Z;
				}
			}
		}
	}
}

void P_DrawSplash (FLevelLocals *Level, int count, const DVector3 &pos, DAngle angle, int kind)
{
	int color1, color2;

	switch (kind)
	{
	case 1:		// Spark
		color1 = orange;
		color2 = yorange;
		break;
	default:
		return;
	}

	for (; count; count--)
	{
		particle_t *p = JitterParticle (Level, 10);

		if (!p)
			break;

		p->size = 2;
		p->color = M_Random() & 0x80 ? color1 : color2;
		p->Vel.Z -= M_Random () / 128.;
		p->Acc.Z -= 1./8;
		p->Acc.X += (M_Random () - 128) / 8192.;
		p->Acc.Y += (M_Random () - 128) / 8192.;
		p->Pos.Z = pos.Z - M_Random () / 64.;
		angle += DAngle::fromDeg(M_Random() * (45./256));
		p->Pos.X = pos.X + (M_Random() & 15)*angle.Cos();
		p->Pos.Y = pos.Y + (M_Random() & 15)*angle.Sin();
	}
}

void P_DrawSplash2 (FLevelLocals *Level, int count, const DVector3 &pos, DAngle angle, int updown, int kind)
{
	int color1, color2, zadd;
	double zvel, zspread;

	switch (kind)
	{
	case 0:		// Blood
		color1 = blood1;
		color2 = blood2;
		break;
	case 1:		// Gunshot
		color1 = grey3;
		color2 = grey5;
		break;
	case 2:		// Smoke
		color1 = grey3;
		color2 = grey1;
		break;
	default:	// colorized blood
		color1 = ParticleColor(kind);
		color2 = ParticleColor(RPART(kind)/3, GPART(kind)/3, BPART(kind)/3);
		break;
	}

	zvel = -1./512.;
	zspread = updown ? -6000 / 65536. : 6000 / 65536.;
	zadd = (updown == 2) ? -128 : 0;

	for (; count; count--)
	{
		particle_t *p = NewParticle (Level);
		DAngle an;

		if (!p)
			break;

		p->ttl = 12;
		p->fadestep = FADEFROMTTL(12);
		p->alpha = 1.f;
		p->size = 4;
		p->color = M_Random() & 0x80 ? color1 : color2;
		p->Vel.Z = M_Random() * zvel;
		p->Acc.Z = -1 / 22.f;
		if (kind) 
		{
			an = angle + DAngle::fromDeg((M_Random() - 128) * (180 / 256.));
			p->Vel.X = M_Random() * an.Cos() / 2048.;
			p->Vel.Y = M_Random() * an.Sin() / 2048.;
			p->Acc.X = p->Vel.X / 16.;
			p->Acc.Y = p->Vel.Y / 16.;
		}
		an = angle + DAngle::fromDeg((M_Random() - 128) * (90 / 256.));
		p->Pos.X = pos.X + ((M_Random() & 31) - 15) * an.Cos();
		p->Pos.Y = pos.Y + ((M_Random() & 31) - 15) * an.Sin();
		p->Pos.Z = pos.Z + (M_Random() + zadd - 128) * zspread;
	}
}

struct TrailSegment
{
	DVector3 start;
	DVector3 dir;
	DVector3 extend;
	DVector2 soundpos;
	double length;
	double sounddist;
};



void P_DrawRailTrail(AActor *source, TArray<SPortalHit> &portalhits, int color1, int color2, double maxdiff, int flags, PClassActor *spawnclass, DAngle angle, int duration, double sparsity, double drift, int SpiralOffset, DAngle pitch)
{
	double length = 0;
	int steps, i;
	TArray<TrailSegment> trail;
	TAngle<double> deg;
	DVector3 pos;
	bool fullbright;
	unsigned segment;
	double lencount;

	for (unsigned i = 0; i < portalhits.Size() - 1; i++)
	{
		TrailSegment seg;

		seg.start = portalhits[i].ContPos;
		seg.dir = portalhits[i].OutDir;
		seg.length = (portalhits[i + 1].HitPos - seg.start).Length();

		//Calculate PerpendicularVector (extend, dir):
		double minelem = 1;
		int epos;
		int ii;
		for (epos = 0, ii = 0; ii < 3; ++ii)
		{
			if (fabs(seg.dir[ii]) < minelem)
			{
				epos = ii;
				minelem = fabs(seg.dir[ii]);
			}
		}
		DVector3 tempvec(0, 0, 0);
		tempvec[epos] = 1;
		seg.extend = (tempvec - (seg.dir | tempvec) * seg.dir) * 3;
		length += seg.length;

		auto player = source->Level->GetConsolePlayer();
		if (player)
		{
			// Only consider sound in 2D (for now, anyway)
			// [BB] You have to divide by lengthsquared here, not multiply with it.
			AActor *mo = player->camera;
			double r = ((seg.start.Y - mo->Y()) * (-seg.dir.Y) - (seg.start.X - mo->X()) * (seg.dir.X)) / (seg.length * seg.length);
			r = clamp<double>(r, 0., 1.);
			seg.soundpos = seg.start.XY() + r * seg.dir.XY();
			seg.sounddist = (seg.soundpos - mo->Pos()).LengthSquared();
		}
		else
		{
			// Set to invalid for secondary levels.
			seg.soundpos = {0,0};
			seg.sounddist = -1;
		}
		trail.Push(seg);
	}

	steps = xs_FloorToInt(length / 3);
	fullbright = !!(flags & RAF_FULLBRIGHT);

	if (steps)
	{
		if (!(flags & RAF_SILENT))
		{
			auto player = source->Level->GetConsolePlayer();
			if (player)
			{
				FSoundID sound;
				
				// Allow other sounds than 'weapons/railgf'!
				if (!source->player) sound = source->AttackSound;
				else if (source->player->ReadyWeapon) sound = source->player->ReadyWeapon->AttackSound;
				else sound = NO_SOUND;
				if (!sound.isvalid()) sound = S_FindSound("weapons/railgf");
				
				// The railgun's sound is special. It gets played from the
				// point on the slug's trail that is closest to the hearing player.
				AActor *mo = player->camera;
				
				if (fabs(mo->X() - trail[0].start.X) < 20 && fabs(mo->Y() - trail[0].start.Y) < 20)
				{ // This player (probably) fired the railgun
					S_Sound (mo, CHAN_WEAPON, 0, sound, 1, ATTN_NORM);
				}
				else
				{
					TrailSegment *shortest = NULL;
					for (auto &seg : trail)
					{
						if (shortest == NULL || shortest->sounddist > seg.sounddist) shortest = &seg;
					}
					S_Sound (source->Level, DVector3(shortest->soundpos, r_viewpoint.Pos.Z), CHAN_WEAPON, 0, sound, 1, ATTN_NORM);
				}
			}
		}
	}
	else
	{
		// line is 0 length, so nothing to do
		return;
	}

	// Create the outer spiral.
	if (color1 != -1 && (!r_rail_smartspiral || color2 == -1) && r_rail_spiralsparsity > 0 && (spawnclass == NULL))
	{
		double stepsize = 3 * r_rail_spiralsparsity * sparsity;
		int spiral_steps = (int)(steps * r_rail_spiralsparsity / sparsity);
		segment = 0;
		lencount = trail[0].length;
		
		color1 = color1 == 0 ? -1 : ParticleColor(color1);
		pos = trail[0].start;
		deg = DAngle::fromDeg(SpiralOffset);
		for (i = spiral_steps; i; i--)
		{
			particle_t *p = NewParticle (source->Level);
			DVector3 tempvec;

			if (!p)
				return;

			int spiralduration = (duration == 0) ? TICRATE : duration;

			p->alpha = 1.f;
			p->ttl = spiralduration;
			p->fadestep = FADEFROMTTL(spiralduration);
			p->size = 3;
			if(fullbright)
			{
				p->flags |= SPF_FULLBRIGHT;
			}

			tempvec = DMatrix3x3(trail[segment].dir, deg) * trail[segment].extend;
			p->Vel = FVector3(tempvec * drift / 16.);
			p->Pos = tempvec + pos;
			pos += trail[segment].dir * stepsize;
			deg += DAngle::fromDeg(r_rail_spiralsparsity * 14);
			lencount -= stepsize;
			if (color1 == -1)
			{
				int rand = M_Random();

				if (rand < 155)
					p->color = rblue2;
				else if (rand < 188)
					p->color = rblue1;
				else if (rand < 222)
					p->color = rblue3;
				else
					p->color = rblue4;
			}
			else 
			{
				p->color = color1;
			}

			if (lencount <= 0)
			{
				segment++;
				if (segment < trail.Size())
				{
					pos = trail[segment].start - trail[segment].dir * lencount;
					lencount += trail[segment].length;
				}
				else
				{
					// should never happen but if something goes wrong, just terminate the loop.
					break;
				}
			}
		}
	}

	// Create the inner trail.
	if (color2 != -1 && r_rail_trailsparsity > 0 && spawnclass == NULL)
	{
		double stepsize = 3 * r_rail_trailsparsity * sparsity;
		int trail_steps = xs_FloorToInt(steps * r_rail_trailsparsity / sparsity);

		color2 = color2 == 0 ? -1 : ParticleColor(color2);
		DVector3 diff(0, 0, 0);

		pos = trail[0].start;
		lencount = trail[0].length;
		segment = 0;
		for (i = trail_steps; i; i--)
		{
			// [XA] inner trail uses a different default duration (33).
			int innerduration = (duration == 0) ? 33 : duration;
			particle_t *p = JitterParticle (source->Level, innerduration, (float)drift);

			if (!p)
				return;

			if (maxdiff > 0)
			{
				int rnd = M_Random ();
				if (rnd & 1)
					diff.X = clamp<double>(diff.X + ((rnd & 8) ? 1 : -1), -maxdiff, maxdiff);
				if (rnd & 2)
					diff.Y = clamp<double>(diff.Y + ((rnd & 16) ? 1 : -1), -maxdiff, maxdiff);
				if (rnd & 4)
					diff.Z = clamp<double>(diff.Z + ((rnd & 32) ? 1 : -1), -maxdiff, maxdiff);
			}

			DVector3 postmp = pos + diff;

			p->size = 2;
			p->Pos = postmp;
			if (color1 != -1)
				p->Acc.Z -= 1./4096;
			pos += trail[segment].dir * stepsize;
			lencount -= stepsize;
			if(fullbright)
			{
				p->flags |= SPF_FULLBRIGHT;
			}

			if (color2 == -1)
			{
				int rand = M_Random();

				if (rand < 85)
					p->color = grey4;
				else if (rand < 170)
					p->color = grey2;
				else
					p->color = grey1;
			}
			else 
			{
				p->color = color2;
			}
			if (lencount <= 0)
			{
				segment++;
				if (segment < trail.Size())
				{
					pos = trail[segment].start - trail[segment].dir * lencount;
					lencount += trail[segment].length;
				}
				else
				{
					// should never happen but if something goes wrong, just terminate the loop.
					break;
				}
			}

		}
	}
	// create actors
	if (spawnclass != NULL)
	{
		if (sparsity < 1)
			sparsity = 32;

		double stepsize = sparsity;
		int trail_steps = (int)((steps * 3) / sparsity);
		DVector3 diff(0, 0, 0);

		pos = trail[0].start;
		lencount = trail[0].length;
		segment = 0;

		for (i = trail_steps; i; i--)
		{
			if (maxdiff > 0)
			{
				int rnd = pr_railtrail();
				if (rnd & 1)
					diff.X = clamp<double>(diff.X + ((rnd & 8) ? 1 : -1), -maxdiff, maxdiff);
				if (rnd & 2)
					diff.Y = clamp<double>(diff.Y + ((rnd & 16) ? 1 : -1), -maxdiff, maxdiff);
				if (rnd & 4)
					diff.Z = clamp<double>(diff.Z + ((rnd & 32) ? 1 : -1), -maxdiff, maxdiff);
			}			
			AActor *thing = Spawn (source->Level, spawnclass, pos + diff, ALLOW_REPLACE);
			if (thing)
			{
				if (source)	thing->target = source;
				thing->Angles.Pitch = pitch;
				thing->Angles.Yaw = angle;
			}
			pos += trail[segment].dir * stepsize;
			lencount -= stepsize;
			if (lencount <= 0)
			{
				segment++;
				if (segment < trail.Size())
				{
					pos = trail[segment].start - trail[segment].dir * lencount;
					lencount += trail[segment].length;
				}
				else
				{
					// should never happen but if something goes wrong, just terminate the loop.
					break;
				}
			}
		}
	}
}

void P_DisconnectEffect (AActor *actor)
{
	int i;

	if (actor == NULL)
		return;

	for (i = 64; i; i--)
	{
		particle_t *p = JitterParticle (actor->Level, TICRATE*2);

		if (!p)
			break;

		double xo = (M_Random() - 128)*actor->radius / 128;
		double yo = (M_Random() - 128)*actor->radius / 128;
		double zo = M_Random()*actor->Height / 256;

		DVector3 pos = actor->Vec3Offset(xo, yo, zo);
		p->Pos = pos;
		p->Acc.Z -= 1./4096;
		p->color = M_Random() < 128 ? maroon1 : maroon2;
		p->size = 4;
	}
}

//===========================================================================
// 
// ZScript Sprite (DVisualThinker)
// Concept by Major Cooke
// Most code borrowed by Actor and particles above
// 
//===========================================================================

void DVisualThinker::Construct()
{
	PT = {};
	PT.Pos = { 0,0,0 };
	PT.Vel = { 0,0,0 };
	Offset = { 0,0 };
	Scale = { 1,1 };
	PT.Roll = 0.0;
	PT.alpha = 1.0;
	LightLevel = -1;
	PT.texture = FTextureID();
	PT.style = STYLE_Normal;
	PT.flags = 0;
	Translation = NO_TRANSLATION;
	PT.subsector = nullptr;
	cursector = nullptr;
	PT.color = 0xffffff;
	AnimatedTexture.SetNull();

	_prev = _next = nullptr;
	if (Level->VisualThinkerHead != nullptr)
	{
		Level->VisualThinkerHead->_prev = this;
		_next = Level->VisualThinkerHead;
	}
	Level->VisualThinkerHead = this;
}

void DVisualThinker::OnDestroy()
{
	if (_prev != nullptr)
		_prev->_next = _next;
	if (_next != nullptr)
		_next->_prev = _prev;
	if (Level->VisualThinkerHead == this)
		Level->VisualThinkerHead = _next;

	PT.alpha = 0.0; // stops all rendering.
	Super::OnDestroy();
}

DVisualThinker* DVisualThinker::GetNext() const
{
	return _next;
}

DVisualThinker* DVisualThinker::NewVisualThinker(FLevelLocals* Level, PClass* type)
{
	if (type == nullptr)
	{
		return nullptr;
	}
	else if (!type->IsDescendantOf(RUNTIME_CLASS(DVisualThinker)))
	{
		Printf("Attempt to spawn class not inherent to VisualThinker: %s\n", type->TypeName.GetChars());
		return nullptr;
	}
	else if (type->bAbstract)
	{
		Printf("Attempt to spawn an instance of abstract VisualThinker class %s\n", type->TypeName.GetChars());
		return nullptr;
	}

	auto zs = static_cast<DVisualThinker*>(Level->CreateThinker(type, DVisualThinker::DEFAULT_STAT));
	zs->Construct();

	IFOVERRIDENVIRTUALPTRNAME(zs, NAME_VisualThinker, BeginPlay)
	{
		VMValue params[] = { zs };
		VMCall(func, params, 1, nullptr, 0);

		if (zs->ObjectFlags & OF_EuthanizeMe)
			return nullptr;
	}

	return zs;
}

static DVisualThinker* SpawnVisualThinker(FLevelLocals* Level, PClass* type)
{
	return DVisualThinker::NewVisualThinker(Level, type);
}

void DVisualThinker::UpdateSector(subsector_t * newSubsector)
{
	assert(newSubsector);
	if(PT.subsector != newSubsector)
	{
		PT.subsector = newSubsector;
		cursector = newSubsector->sector;
	}
}

void DVisualThinker::UpdateSector()
{
	UpdateSector(Level->PointInRenderSubsector(PT.Pos));
}

static void UpdateSector(DVisualThinker * self)
{
	self->UpdateSector();
}

DEFINE_ACTION_FUNCTION_NATIVE(DVisualThinker, UpdateSector, UpdateSector)
{
	PARAM_SELF_PROLOGUE(DVisualThinker);
	self->UpdateSector();
	return 0;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SpawnVisualThinker, SpawnVisualThinker)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_CLASS_NOT_NULL(type, DVisualThinker);
	DVisualThinker* zs = SpawnVisualThinker(self, type);
	ACTION_RETURN_OBJECT(zs);
}

void DVisualThinker::UpdateSpriteInfo()
{
	PT.style = ERenderStyle(GetRenderStyle());

	if ((PT.flags & SPF_LOCAL_ANIM) && PT.texture != AnimatedTexture)
	{
		AnimatedTexture = PT.texture;
		TexAnim.InitStandaloneAnimation(PT.animData, PT.texture, Level->maptime);
	}
}

static void UpdateSpriteInfo(DVisualThinker * self)
{
	self->UpdateSpriteInfo();
}

DEFINE_ACTION_FUNCTION_NATIVE(DVisualThinker, UpdateSpriteInfo, UpdateSpriteInfo)
{
	PARAM_SELF_PROLOGUE(DVisualThinker);
	self->UpdateSpriteInfo();
	return 0;
}

bool DVisualThinker::ValidTexture()
{
	return ((flags & VTF_IsParticle) || PT.texture.isValid());
}

// This runs just like Actor's, make sure to call Super.Tick() in ZScript.
void DVisualThinker::Tick()
{
	if (ObjectFlags & OF_EuthanizeMe)
		return;

	// [Nakara] Detached ribbon carriers have no sprite texture and do not move.
	// Keep them in their source-side subsector until the newest sample has lived
	// through the full trail lifetime. A small two-tic grace period prevents the
	// thinker from being destroyed before the final interpolated render frame.
	if (bParticleTrailRibbonCarrier)
	{
		if (Level == nullptr || ParticleTrailHistory.Size() < 2 || ParticleTrailGeneration == 0)
		{
			Destroy();
			return;
		}

		const double lifetimeTicks = max<double>(1.0, ParticleTrailLifetime * TICRATE);
		const double newestTime = ParticleTrailHistory.Last().SpawnTime;
		if (!std::isfinite(newestTime) || (double)Level->maptime - newestTime > lifetimeTicks + 2.0)
		{
			Destroy();
			return;
		}

		Prev = PT.Pos;
		PrevRoll = PT.Roll;
		if (PT.subsector == nullptr) UpdateSector();
		return;
	}

	if (!ValidTexture())
	{
		Destroy();
		return;
	}

	if (isFrozen())
	{	// needed here because it won't retroactively update like actors do.
		PT.subsector = Level->PointInRenderSubsector(PT.Pos);
		cursector = PT.subsector->sector;
		UpdateSpriteInfo(); 
		return;
	}
	Prev = PT.Pos;
	PrevRoll = PT.Roll;
	// Handle crossing a line portal
	DVector2 newxy = Level->GetPortalOffsetPosition(PT.Pos.X, PT.Pos.Y, PT.Vel.X, PT.Vel.Y);
	PT.Pos.X = newxy.X;
	PT.Pos.Y = newxy.Y;
	PT.Pos.Z += PT.Vel.Z;
	subsector_t * ss = Level->PointInRenderSubsector(PT.Pos);

	// Handle crossing a sector portal.
	if (!ss->sector->PortalBlocksMovement(sector_t::ceiling))
	{
		if (PT.Pos.Z > ss->sector->GetPortalPlaneZ(sector_t::ceiling))
		{
			PT.Pos += ss->sector->GetPortalDisplacement(sector_t::ceiling);
			ss = Level->PointInRenderSubsector(PT.Pos);
		}
	}
	else if (!ss->sector->PortalBlocksMovement(sector_t::floor))
	{
		if (PT.Pos.Z < ss->sector->GetPortalPlaneZ(sector_t::floor))
		{
			PT.Pos += ss->sector->GetPortalDisplacement(sector_t::floor);
			ss = Level->PointInRenderSubsector(PT.Pos);
		}
	}
    
	UpdateSector(ss);
	UpdateSpriteInfo();
}

int DVisualThinker::GetLightLevel(sector_t* rendersector) const
{
	int lightlevel = rendersector->GetSpriteLight();

	if (flags & VTF_AddLightLevel)
	{
		lightlevel += LightLevel;
	}
	else if (LightLevel > -1)
	{
		lightlevel = LightLevel;
	}
	return lightlevel;
}

FVector3 DVisualThinker::InterpolatedPosition(double ticFrac) const
{
	if (flags & VTF_DontInterpolate) return FVector3(PT.Pos);

	DVector3 proc = Prev + (ticFrac * (PT.Pos - Prev));
	return FVector3(proc);

}

float DVisualThinker::InterpolatedRoll(double ticFrac) const
{
	if (flags & VTF_DontInterpolate) return PT.Roll;

	return float(PrevRoll + (PT.Roll - PrevRoll) * ticFrac);
}



void DVisualThinker::SetTranslation(FName trname)
{
	// There is no constant for the empty name...
	if (trname.GetChars()[0] == 0)
	{
		// '' removes it
		Translation = NO_TRANSLATION;
		return;
	}

	auto tnum = R_FindCustomTranslation(trname);
	if (tnum != INVALID_TRANSLATION)
	{
		Translation = tnum;
	}
	// silently ignore if the name does not exist, this would create some insane message spam otherwise.
}

void SetTranslation(DVisualThinker * self, int i_trans)
{
	FName trans {ENamedName(i_trans)};
	self->SetTranslation(trans);
}

DEFINE_ACTION_FUNCTION_NATIVE(DVisualThinker, SetTranslation, SetTranslation)
{
	PARAM_SELF_PROLOGUE(DVisualThinker);
	PARAM_NAME(trans);
	self->SetTranslation(trans);
	return 0;
}

int DVisualThinker::GetParticleType() const
{
	int flag = (flags & VTF_IsParticle);
	switch (flag)
	{
	case VTF_ParticleSquare:
		return PT_SQUARE;
	case VTF_ParticleRound:
		return PT_ROUND;
	case VTF_ParticleSmooth:
		return PT_SMOOTH;
	}
	return PT_DEFAULT;
}

static int GetParticleType(DVisualThinker* self)
{
	return self->GetParticleType();
}

DEFINE_ACTION_FUNCTION_NATIVE(DVisualThinker, GetParticleType, GetParticleType)
{
	PARAM_SELF_PROLOGUE(DVisualThinker);
	ACTION_RETURN_INT(self->GetParticleType());
}

static int IsFrozen(DVisualThinker* self)
{
	return !!(self->Level->isFrozen() && !(self->PT.flags & SPF_NOTIMEFREEZE));
}

bool DVisualThinker::isFrozen()
{
	return IsFrozen(this);
}

DEFINE_ACTION_FUNCTION_NATIVE(DVisualThinker, IsFrozen, IsFrozen)
{
	PARAM_SELF_PROLOGUE(DVisualThinker);
	ACTION_RETURN_BOOL(self->isFrozen());
}

static void SetRenderStyle(DVisualThinker *self, int mode)
{
	if(mode >= 0 && mode < STYLE_Count)
	{
		self->PT.style = ERenderStyle(mode);
	}
}

DEFINE_ACTION_FUNCTION_NATIVE(DVisualThinker, SetRenderStyle, SetRenderStyle)
{
	PARAM_SELF_PROLOGUE(DVisualThinker);
	PARAM_INT(mode);

	self->PT.style = ERenderStyle(mode);
	return 0;
}

int DVisualThinker::GetRenderStyle() const
{
	return PT.style;
}

static int GetRenderStyle(DVisualThinker* self)
{
	return self->GetRenderStyle();
}

DEFINE_ACTION_FUNCTION_NATIVE(DVisualThinker, GetRenderStyle, GetRenderStyle)
{
	PARAM_SELF_PROLOGUE(DVisualThinker);
	ACTION_RETURN_INT(self->GetRenderStyle());
}

float DVisualThinker::GetOffset(bool y) const // Needed for the renderer.
{
	if (y)
		return (float)((flags & VTF_FlipOffsetY) ? Offset.Y : -Offset.Y);
	else
		return (float)((flags & VTF_FlipOffsetX) ? Offset.X : -Offset.X);
}


FSerializer& Serialize(FSerializer& arc, const char* key, FStandaloneAnimation& value, FStandaloneAnimation* defval)
{
	arc.BeginObject(key);
	arc("SwitchTic",	value.SwitchTic);
	arc("AnimIndex",	value.AnimIndex);
	arc("CurFrame",		value.CurFrame);
	arc("Ok",			value.ok);
	arc("AnimType",		value.AnimType);
	arc.EndObject();
	return arc;
}

void DVisualThinker::Serialize(FSerializer& arc)
{
	Super::Serialize(arc);

	arc("pos", PT.Pos)
		("vel", PT.Vel)
		("prev", Prev)
		("scale", Scale)
		("roll", PT.Roll)
		("prevroll", PrevRoll)
		("offset", Offset)
		("alpha", PT.alpha)
		("texture", PT.texture)
		("style", *reinterpret_cast<int*>(&PT.style))
		("translation", Translation)
		("cursector", cursector)
		("scolor", PT.color)
		("lightlevel", LightLevel)
		("animData", PT.animData)
		("flags", PT.flags)
		("visualThinkerFlags", flags)
		("next", _next)
		("prev", _prev);
    
    if(arc.isReading())
    {
        UpdateSector();
    }
}

IMPLEMENT_CLASS(DVisualThinker, false, false);
DEFINE_FIELD_NAMED(DVisualThinker, PT.color, SColor);
DEFINE_FIELD_NAMED(DVisualThinker, PT.Pos, Pos);
DEFINE_FIELD_NAMED(DVisualThinker, PT.Vel, Vel);
DEFINE_FIELD_NAMED(DVisualThinker, PT.Roll, Roll);
DEFINE_FIELD_NAMED(DVisualThinker, PT.alpha, Alpha);
DEFINE_FIELD_NAMED(DVisualThinker, PT.texture, Texture);
DEFINE_FIELD_NAMED(DVisualThinker, PT.flags, Flags);
DEFINE_FIELD_NAMED(DVisualThinker, flags, VisualThinkerFlags);

DEFINE_FIELD(DVisualThinker, Prev);
DEFINE_FIELD(DVisualThinker, Scale);
DEFINE_FIELD(DVisualThinker, Offset);
DEFINE_FIELD(DVisualThinker, PrevRoll);
DEFINE_FIELD(DVisualThinker, Translation);
DEFINE_FIELD(DVisualThinker, LightLevel);
DEFINE_FIELD(DVisualThinker, cursector);
