//-----------------------------------------------------------------------------
//
// Nakara navigation-aware chase movement.
//
// This deliberately keeps combat decisions in A_DoChase. It supplies a
// ground surface search or a sparse 3D flight search and follows the resulting
// waypoints with the engine's normal monster movement code.
//
//-----------------------------------------------------------------------------

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <queue>
#include <unordered_map>
#include <vector>

#include "actor.h"
#include "c_cvars.h"
#include "g_levellocals.h"
#include "m_random.h"
#include "p_checkposition.h"
#include "p_effect.h"
#include "p_enemy.h"
#include "p_local.h"
#include "p_maputl.h"
#include "p_spec.h"
#include "p_terrain.h"
#include "p_smartchase.h"
#include "printf.h"

CVAR(Int, nk_smartchase_debug, 0, 0)

namespace
{

constexpr int NK_GROUND_MAX_NODES = 3072;
constexpr int NK_FLY_MAX_NODES = 6144;
constexpr int NK_REPATH_TICS = 10;
constexpr int NK_FAILED_REPATH_TICS = 18;
constexpr int NK_STUCK_TICS = 6;
constexpr int NK_TACTICAL_MIN_TICS = 35;
constexpr int NK_TACTICAL_TIC_VARIATION = 36;
constexpr double NK_MAX_PREDICTION_TICS = 28.0;
constexpr double NK_MAX_PREDICTION_DISTANCE = 192.0;
constexpr double NK_DIRECT_PURSUIT_RANGE = 128.0;
constexpr double NK_FULL_TACTICAL_RANGE = 512.0;
constexpr double NK_TACTICAL_MIN_SPREAD = 160.0;
constexpr int NK_TACTICAL_SPREAD_VARIATION = 161;
constexpr double NK_WEIGHTED_HEURISTIC = 1.35;
constexpr double NK_ROUTE_BIAS = 0.20;
constexpr int NK_TACTICAL_SECTORS = 5;
constexpr int NK_GROUND_CONNECTION_TESTS_PER_TIC = 64;
constexpr int NK_GROUND_URGENT_CONNECTION_TESTS_PER_TIC = 128;
constexpr int NK_GROUND_URGENT_MIN_TESTS = 4;
constexpr size_t NK_GROUND_CONNECTION_CACHE_LIMIT = 131072;
constexpr int NK_GROUND_PASS_CACHE_TICS = 350;
constexpr int NK_GROUND_BLOCK_CACHE_TICS = 14;
constexpr double NK_GROUND_DIRECT_BUILD_RANGE = 512.0;
constexpr int NK_GROUND_SHORTCUT_CHECKS = 4;
constexpr size_t NK_GROUND_SHORTCUT_LOOKAHEAD = 4;
constexpr int NK_GROUND_NODE_EXPANSIONS_PER_SLICE = 128;
constexpr int NK_GROUND_GOAL_CHECKS_PER_SLICE = 4;
constexpr int NK_GROUND_EMERGENCY_CONNECTION_TESTS = 64;
constexpr int NK_GROUND_EMERGENCY_NODE_EXPANSIONS = 512;
constexpr int NK_GROUND_EMERGENCY_GOAL_CHECKS = 12;
constexpr int NK_GROUND_ROAM_NODE_EXPANSIONS_PER_SLICE = 64;
constexpr size_t NK_GROUND_COMPONENT_NODE_LIMIT = 65536;
constexpr int NK_ROAM_CANDIDATE_ATTEMPTS = 24;
constexpr double NK_ROAM_MIN_DISTANCE = 384.0;
constexpr double NK_ROAM_MAX_DISTANCE = 1024.0;
constexpr int NK_ROAM_MIN_TICS = 210;
constexpr int NK_ROAM_TIC_VARIATION = 141;
constexpr double NK_TARGET_WARP_DISTANCE = 512.0;
// Kitsune SpiritMove must never hammer an unreachable TeleportGroup destination.
// Once a complete sliced flight search proves that the player landing anchor is
// disconnected, the spirit reuses the reachable route it already discovered and
// only retries the player after a large target warp or this long fallback delay.
constexpr int NK_KITSUNE_SPIRIT_DISCONNECTED_RETRY_TICS = 350;
constexpr double NK_KITSUNE_SPIRIT_ROAM_MIN_DISTANCE = 128.0;
constexpr double NK_KITSUNE_SPIRIT_ROAM_PREFERRED_DISTANCE = 512.0;
constexpr double NK_KITSUNE_SPIRIT_RETRY_WARP_DISTANCE = 192.0;
constexpr int NK_KITSUNE_SPIRIT_FLY_MAX_NODES = 1024;
constexpr int NK_KITSUNE_SPIRIT_FLY_EXPANSIONS_PER_SLICE = 6;
constexpr double NK_GROUND_ANCHOR_QUANTUM = 8.0;
constexpr double NK_GROUND_CACHE_REUSE_DISTANCE = 1.0;
constexpr double NK_PATH_HANDOFF_DISTANCE = 128.0;
constexpr size_t NK_BLOCKED_PATH_RECONNECT_LOOKAHEAD = 4;
constexpr int NK_CROWD_BYPASS_TICS = 24;
constexpr int NK_CROWD_BYPASS_COMMIT_TICS = 6;
constexpr int NK_CROWD_DETOUR_BLOCK_COUNT = 3;
constexpr int NK_CROWD_DETOUR_WINDOW_TICS = 28;
constexpr int NK_CROWD_DETOUR_RETRIGGER_TICS = 18;
constexpr int NK_CROWD_DETOUR_ACTIVE_TICS = 105;
constexpr size_t NK_CROWD_DETOUR_MAX_SAMPLES = 48;
constexpr double NK_CROWD_DETOUR_SAMPLE_RANGE = 1024.0;
constexpr double NK_CROWD_DETOUR_INFLUENCE = 144.0;
constexpr double NK_CROWD_DETOUR_CORE_MARGIN = 48.0;
constexpr double NK_CROWD_DETOUR_STEP_COST = 260.0;
constexpr int NK_CROWD_STEERING_HOLD_TICS = 4;
constexpr double NK_CROWD_STEERING_MIN_DISTANCE = 40.0;
// Ground movement is quantized to eight directions. Without a dead-band, a
// route whose ideal heading sits near the 22.5-degree boundary can alternate
// E/NE (or an equivalent pair) every tic. Keep the already chosen adjacent
// direction while it remains within 30 degrees of the waypoint vector.
constexpr double NK_GROUND_STEERING_HYSTERESIS_DOT = 0.8660254037844386;
constexpr double NK_GROUND_STEERING_MIN_DISTANCE = 28.0;
// Vanilla P_DoNewChaseDir/P_TryWalk deliberately keeps a newly found escape
// direction for a short random movecount. SmartChase used to throw most of
// that persistence away on the following tic. Preserve a bounded part of it
// only for true geometry recovery.
constexpr int NK_RANDOM_RECOVERY_MIN_TICS = 4;
constexpr int NK_RANDOM_RECOVERY_MAX_TICS = 10;
// A successful geometry escape should not surrender control to the blocked
// waypoint on the very next tic. Keep the chosen wall/corner lane briefly,
// then let normal A* steering take over again.
constexpr int NK_WALL_ESCAPE_MIN_TICS = 5;
constexpr int NK_WALL_ESCAPE_MAX_TICS = 8;
constexpr int NK_HARD_BLOCK_STALL_LOG_TICS = 4;
// Crowd retreat is the physical escape phase of Dynamic Crowd Detour. When
// actor bypass cannot make forward progress, step into the rear half-plane for
// a short committed interval so a fresh detour can start outside the collision
// knot instead of oscillating left/right in place.
constexpr int NK_CROWD_RETREAT_MIN_TICS = 16;
constexpr int NK_CROWD_RETREAT_MAX_TICS = 28;
constexpr int NK_CROWD_RETREAT_MOMENTUM_TICS = 8;
constexpr double NK_CROWD_RETREAT_CLEARANCE_EXTRA = 32.0;
constexpr int NK_CROWD_RETREAT_TRIGGER_STEPS = 2;
constexpr int NK_CROWD_PROGRESS_WINDOW_TICS = 12;
constexpr double NK_CROWD_PROGRESS_MIN_GAIN = 24.0;
constexpr double NK_CROWD_FORWARD_PROBE_RANGE = 224.0;
constexpr int NK_CROWD_ROUTE_COMMIT_TICS = 35;
constexpr double NK_STATIONARY_SURROUND_SPEED_SQ = 0.25;
constexpr double NK_STATIONARY_SURROUND_MIN_RADIUS = 128.0;
constexpr double NK_STATIONARY_SURROUND_MAX_RADIUS = 192.0;
constexpr double NK_STATIONARY_SURROUND_PROBE_STEP = 8.0;
constexpr size_t NK_STATIONARY_SURROUND_RECONNECT_MAX_INDEX = 1;
constexpr int NK_CORNER_BYPASS_TICS = 5;
constexpr int NK_CORNER_BYPASS_COMMIT_TICS = 2;
constexpr int NK_HARD_BLOCK_ESCAPE_DELAY_TICS = 2;
// HypnotizeChase follows its A* route deterministically. A real actor blocker
// may start a short fixed-side bypass. Static geometry is handled side-first:
// try forward-facing 45/90-degree escapes before considering a vertical 3D-floor
// escape. No Doom random scan or 180-degree turnaround is used by this branch.
constexpr int NK_HYPNOTIZE_BYPASS_MIN_TICS = 4;
constexpr int NK_HYPNOTIZE_BYPASS_MAX_TICS = 28;
constexpr double NK_HYPNOTIZE_BYPASS_REPLAN_DISTANCE = 16.0;
// Ground HypnotizeChase used to notice dynamic actors only after P_SmartMove
// physically hit them and populated BlockingMobj. Probe a short distance ahead
// so a monster crossing the committed waypoint lane can start the same fixed-
// side bypass before contact. The future sample is intentionally short; this is
// local collision avoidance, not another tactical path planner.
constexpr double NK_HYPNOTIZE_GROUND_DYNAMIC_PROBE_RANGE = 96.0;
constexpr double NK_HYPNOTIZE_GROUND_DYNAMIC_LOOKAHEAD_TICS = 4.0;
constexpr double NK_HYPNOTIZE_GROUND_DYNAMIC_CLEARANCE_EXTRA = 8.0;
// Flying A* used to consume the entire 6144-node search in one game tic. Slice
// only HypnotizeChase flight searches so target warps cannot create a long
// single-frame hitch. A search without an old route gets a slightly larger
// slice so initial acquisition still starts promptly.
constexpr int NK_HYPNOTIZE_FLY_EXPANSIONS_PER_SLICE = 12;
constexpr int NK_HYPNOTIZE_FLY_URGENT_EXPANSIONS_PER_SLICE = 24;
constexpr size_t NK_HYPNOTIZE_FLY_SHORTCUT_LOOKAHEAD = 6;
constexpr int NK_HYPNOTIZE_FLY_SHORTCUT_TOTAL_CHECKS = 32;
constexpr int NK_HYPNOTIZE_FLY_RECONNECT_CHECKS = 5;
// Hypnotize flight steering keeps the previous adjacent 8-way heading near
// angular boundaries and eases vertical velocity instead of snapping it to each
// 3D-grid waypoint. Local 3D geometry steering is used only after the normal
// forward step is physically blocked.
constexpr double NK_HYPNOTIZE_STEERING_HYSTERESIS_DOT = 0.8660254037844386;
constexpr double NK_HYPNOTIZE_STEERING_MIN_DISTANCE = 8.0;
constexpr double NK_HYPNOTIZE_VERTICAL_RESPONSE = 0.20;
constexpr double NK_HYPNOTIZE_VERTICAL_ACCEL_RATIO = 0.35;
constexpr double NK_HYPNOTIZE_VERTICAL_MIN_ACCEL = 0.35;
// Kitsune Spirit moves much faster in XY than vanilla floating monsters. When
// P_Move reports a valid MF_FLOAT height adjustment (tm.floatok + P_TestMobjZ),
// preserve that engine-approved Z step and briefly bias the normal waypoint Z
// controller in the same direction. This lets the body acquire slope/step/3D-
// floor clearance before the next fast XY step without discarding a valid A*
// route. HypnotizeChase keeps its existing behavior; this latch is mode 3 only.
constexpr int NK_KITSUNE_SPIRIT_CLEARANCE_HOLD_TICS = 6;
constexpr double NK_KITSUNE_SPIRIT_CLEARANCE_MIN_DELTA = 24.0;
// When the live target is almost directly above/below the chaser, tiny XY
// offsets must not be quantized into an 8-way heading and fed into the lateral
// wall-recovery scan. If the vertical column itself is physically clear, fly
// straight in Z until the approach is no longer steep.
constexpr double NK_HYPNOTIZE_DIRECT_VERTICAL_MIN_XY = 128.0;
constexpr double NK_HYPNOTIZE_DIRECT_VERTICAL_RADIUS_SCALE = 8.0;
constexpr double NK_HYPNOTIZE_DIRECT_VERTICAL_MIN_Z = 40.0;
constexpr double NK_HYPNOTIZE_DIRECT_VERTICAL_Z_TO_XY_RATIO = 1.50;
// Once a near-vertical approach has started, keep it latched across small XY
// drift instead of dropping back into the side-first 8-way scan every time the
// target crosses the entry threshold. This wider exit envelope is deliberately
// asymmetric: enter only on a steep approach, but remain vertical until the
// target is clearly lateral or the vertical column becomes blocked.
constexpr double NK_HYPNOTIZE_DIRECT_VERTICAL_EXIT_MIN_XY = 192.0;
constexpr double NK_HYPNOTIZE_DIRECT_VERTICAL_EXIT_RADIUS_SCALE = 12.0;
constexpr double NK_HYPNOTIZE_DIRECT_VERTICAL_EXIT_MIN_Z = 20.0;
constexpr double NK_HYPNOTIZE_DIRECT_VERTICAL_EXIT_Z_TO_XY_RATIO = 0.50;
constexpr double NK_HYPNOTIZE_DIRECT_VERTICAL_PROBE_MIN = 24.0;
constexpr double NK_HYPNOTIZE_DIRECT_VERTICAL_PROBE_MAX = 64.0;
constexpr int NK_HYPNOTIZE_3D_STEER_HOLD_TICS = 10;
constexpr double NK_HYPNOTIZE_3D_STEER_MIN_VERTICAL_PROBE = 32.0;
constexpr double NK_HYPNOTIZE_3D_STEER_MAX_VERTICAL_PROBE = 80.0;
constexpr double NK_HYPNOTIZE_3D_STEER_MIN_FORWARD_PROBE = 28.0;
constexpr double NK_HYPNOTIZE_3D_STEER_MAX_FORWARD_PROBE = 64.0;
// A sliced 3D A* route can retain short alternating horizontal/vertical grid
// steps after the bounded completion-time compressor exhausts its shortcut
// budget. Re-test only a couple of forward nodes while the actor is already
// moving and skip the staircase when the combined 3D segment is currently
// traversable. This spreads smoothing cost across game tics instead of putting
// it back into the route-completion frame.
constexpr size_t NK_HYPNOTIZE_RUNTIME_SHORTCUT_LOOKAHEAD = 6;
constexpr int NK_HYPNOTIZE_RUNTIME_SHORTCUT_CHECKS = 2;

static FRandom pr_nksmartchase("NKSmartChase");

constexpr double NK_TacticalAngles[] = { 0.0, -35.0, 35.0, -75.0, 75.0 };
constexpr double NK_StationarySurroundAngles[] = { 90.0, -90.0, 135.0, -135.0, 180.0 };
constexpr int NK_STATIONARY_SURROUND_SECTORS = 5;

static FLevelLocals *NK_SearchBudgetLevel = nullptr;
static int NK_SearchBudgetTic = -1;
static int NK_SearchesThisTic = 0;

struct FNKGridKey
{
	int X;
	int Y;
	int Z;

	bool operator==(const FNKGridKey &other) const
	{
		return X == other.X && Y == other.Y && Z == other.Z;
	}
};

struct FNKGridKeyHash
{
	size_t operator()(const FNKGridKey &key) const
	{
		uint64_t x = uint32_t(key.X);
		uint64_t y = uint32_t(key.Y);
		uint64_t z = uint32_t(key.Z);
		uint64_t value = x * 0x9e3779b185ebca87ULL;
		value ^= y + 0x9e3779b97f4a7c15ULL + (value << 6) + (value >> 2);
		value ^= z + 0xc2b2ae3d27d4eb4fULL + (value << 6) + (value >> 2);
		return size_t(value);
	}
};


struct FNKGroundAnchorKey
{
	FNKGridKey Grid;
	int AnchorX;
	int AnchorY;

	bool operator==(const FNKGroundAnchorKey &other) const
	{
		return Grid == other.Grid && AnchorX == other.AnchorX && AnchorY == other.AnchorY;
	}
};

struct FNKGroundAnchorKeyHash
{
	size_t operator()(const FNKGroundAnchorKey &key) const
	{
		size_t value = FNKGridKeyHash{}(key.Grid);
		auto combine = [&value](uint32_t part)
		{
			value ^= size_t(part) + 0x9e3779b9u + (value << 6) + (value >> 2);
		};
		combine(uint32_t(key.AnchorX));
		combine(uint32_t(key.AnchorY));
		return value;
	}
};

struct FNKGroundProfile
{
	int Radius;
	int Height;
	int MaxStep;
	int MaxDrop;
	uint32_t InteractionFlags;

	bool operator==(const FNKGroundProfile &other) const
	{
		return Radius == other.Radius && Height == other.Height &&
			MaxStep == other.MaxStep && MaxDrop == other.MaxDrop &&
			InteractionFlags == other.InteractionFlags;
	}
};

struct FNKGroundConnectionKey
{
	FNKGroundAnchorKey From;
	int DX;
	int DY;
	FNKGroundProfile Profile;

	bool operator==(const FNKGroundConnectionKey &other) const
	{
		return From == other.From && DX == other.DX && DY == other.DY &&
			Profile == other.Profile;
	}
};

struct FNKGroundConnectionKeyHash
{
	size_t operator()(const FNKGroundConnectionKey &key) const
	{
		size_t value = FNKGroundAnchorKeyHash{}(key.From);
		auto combine = [&value](uint32_t part)
		{
			value ^= size_t(part) + 0x9e3779b9u + (value << 6) + (value >> 2);
		};
		combine(uint32_t(key.DX));
		combine(uint32_t(key.DY));
		combine(uint32_t(key.Profile.Radius));
		combine(uint32_t(key.Profile.Height));
		combine(uint32_t(key.Profile.MaxStep));
		combine(uint32_t(key.Profile.MaxDrop));
		combine(key.Profile.InteractionFlags);
		return value;
	}
};

struct FNKGroundConnection
{
	bool Passable;
	DVector3 Accepted;
	DVector2 FromXY;
	double FromZ;
	int CheckedAt;
};

struct FNKGroundNodeKey
{
	FNKGroundAnchorKey Anchor;
	FNKGroundProfile Profile;

	bool operator==(const FNKGroundNodeKey &other) const
	{
		return Anchor == other.Anchor && Profile == other.Profile;
	}
};

struct FNKGroundNodeKeyHash
{
	size_t operator()(const FNKGroundNodeKey &key) const
	{
		size_t value = FNKGroundAnchorKeyHash{}(key.Anchor);
		auto combine = [&value](uint32_t part)
		{
			value ^= size_t(part) + 0x9e3779b9u + (value << 6) + (value >> 2);
		};
		combine(uint32_t(key.Profile.Radius));
		combine(uint32_t(key.Profile.Height));
		combine(uint32_t(key.Profile.MaxStep));
		combine(uint32_t(key.Profile.MaxDrop));
		combine(key.Profile.InteractionFlags);
		return value;
	}
};

struct FNKGroundComponentNode
{
	FNKGroundAnchorKey Anchor;
	FNKGroundProfile Profile;
	DVector3 Pos;
	int Parent;
	std::vector<int> Members;
};

static FLevelLocals *NK_GroundCacheLevel = nullptr;
static int NK_GroundCacheLastTime = -1;
static int NK_GroundConnectionBudgetTic = -1;
static int NK_GroundConnectionTestsThisTic = 0;
static std::unordered_map<FNKGroundConnectionKey, FNKGroundConnection,
	FNKGroundConnectionKeyHash> NK_GroundConnections;
static std::unordered_map<FNKGroundNodeKey, int, FNKGroundNodeKeyHash>
	NK_GroundComponentLookup;
static std::vector<FNKGroundComponentNode> NK_GroundComponentNodes;

struct FNKSearchNode
{
	FNKGridKey Key;
	DVector3 Pos;
	double Cost;
	int Parent;
	bool Closed;
};

struct FNKOpenNode
{
	double Score;
	int Node;

	bool operator<(const FNKOpenNode &other) const
	{
		return Score > other.Score;
	}
};

struct FNKCrowdSample
{
	DVector2 Pos;
	double Clearance = 0.0;
	double Weight = 1.0;
};

struct FNKCrowdDetourState
{
	int BlockCount = 0;
	int WindowUntil = -1;
	int ActiveUntil = -1;
	int LastBlockTic = -1;
	int LastTriggerTic = -1;
	int LastTouched = -1;
	uint32_t Generation = 0;
	uint32_t AppliedGeneration = 0;
	DVector2 WindowOrigin;
	DVector2 LastBlockerCenter;
	int SteeringDirection = DI_NODIR;
	int SteeringHoldUntil = -1;
	int ProgressPathIndex = -1;
	int ProgressWindowStart = -1;
	int ProgressLowWindows = 0;
	int LastProgressSampleTic = -1;
	double ProgressDistance = 0.0;
	DVector2 ProgressWaypoint;
	int RouteCommitUntil = -1;
	bool SurroundGoalActive = false;
	int SurroundUntil = -1;
};

static std::unordered_map<AActor *, FNKCrowdDetourState> NK_CrowdDetours;

struct FNKPendingGroundSearch
{
	DVector3 Start;
	DVector3 Goal;
	FNKGroundProfile Profile;
	uint32_t RouteSeed = 0;
	uint32_t CrowdGeneration = 0;
	bool Weighted = false;
	bool CrowdDetour = false;
	bool SurroundGoal = false;
	bool Initialized = false;
	int LastTouched = -1;
	std::vector<FNKCrowdSample> CrowdSamples;
	std::vector<FNKSearchNode> Nodes;
	std::unordered_map<FNKGroundAnchorKey, int, FNKGroundAnchorKeyHash> NodeLookup;
	std::priority_queue<FNKOpenNode> Open;
};

static std::unordered_map<AActor *, FNKPendingGroundSearch> NK_PendingGroundSearches;

struct FNKPendingFlyingSearch
{
	DVector3 Start;
	DVector3 Goal;
	double CellSize = 0.0;
	double VerticalCellSize = 0.0;
	uint32_t RouteSeed = 0;
	bool Weighted = false;
	bool Initialized = false;
	int LastTouched = -1;
	std::vector<FNKSearchNode> Nodes;
	std::unordered_map<FNKGridKey, int, FNKGridKeyHash> NodeLookup;
	std::priority_queue<FNKOpenNode> Open;
};

static std::unordered_map<AActor *, FNKPendingFlyingSearch> NK_PendingFlyingSearches;

struct FNKKitsuneSpiritRoamState
{
	DVector3 Origin;
	std::vector<DVector3> ForwardRoute;
	bool Reverse = false;
	int LastTouched = -1;
};

static std::unordered_map<AActor *, FNKKitsuneSpiritRoamState> NK_KitsuneSpiritRoams;

struct FNKHypnotizeBypassState
{
	AActor *Blocker = nullptr;
	int Side = 0;
	int Until = -1;
	int CommitUntil = -1;
	int LastTouched = -1;
	int DesiredDirection = DI_NODIR;
	DVector2 Goal;
	DVector2 BlockerCenter;
	DVector2 Forward;
	double BlockerRadius = 0.0;
};

static std::unordered_map<AActor *, FNKHypnotizeBypassState> NK_HypnotizeBypasses;

struct FNKHypnotizeFlightSteeringState
{
	int Direction = DI_NODIR;
	int VerticalSign = 0;
	int VerticalUntil = -1;
	int ClearanceSign = 0;
	int ClearanceUntil = -1;
	bool DirectVertical = false;
	int DirectVerticalSign = 0;
	int LastTouched = -1;
};

static std::unordered_map<AActor *, FNKHypnotizeFlightSteeringState>
	NK_HypnotizeFlightSteeringStates;

struct FNKLocalBypassState
{
	int Side = 0;
	int Until = -1;
	int CommitUntil = -1;
	int LastTouched = -1;
	int Steps = 0;
	bool Crowd = false;
	bool GoalValid = false;
	bool RandomRecovery = false;
	bool WallEscape = false;
	bool CrowdRetreat = false;
	bool CrowdRetreatMomentum = false;
	bool CrowdRetreatDetourStarted = false;
	uint32_t CrowdRetreatGeneration = 0;
	int HeldDirection = DI_NODIR;
	DVector2 Goal;
	DVector2 BlockerCenter;
	DVector2 Forward;
	double BlockerRadius = 0.0;
};

static std::unordered_map<AActor *, FNKLocalBypassState> NK_LocalBypasses;

struct FNKGroundSteeringState
{
	int Direction = DI_NODIR;
	int LastTouched = -1;
};

static std::unordered_map<AActor *, FNKGroundSteeringState> NK_GroundSteeringStates;

struct FNKHardBlockStallDebugState
{
	int LastTic = -1;
	int Count = 0;
	int LastLog = -1000;
};

static std::unordered_map<AActor *, FNKHardBlockStallDebugState> NK_HardBlockStallDebug;

constexpr int NK_DEBUG_DRAW_INTERVAL = 3;
constexpr size_t NK_DEBUG_MAX_PATH_POINTS = 64;
constexpr size_t NK_DEBUG_MAX_SEARCH_NODES = 128;

static void NK_DebugSpawnParticle(FLevelLocals *level, const DVector3 &pos,
	PalEntry color, double size, double zOffset = 16.0)
{
	DVector3 drawPos = pos;
	drawPos.Z += zOffset;
	P_SpawnParticle(level, drawPos, DVector3(), DVector3(), color,
		1.0, NK_DEBUG_DRAW_INTERVAL + 2, size, 0.0, 0.0,
		SPF_FULLBRIGHT | SPF_NOTIMEFREEZE);
}

static void NK_DebugDrawMarker(FLevelLocals *level, const DVector3 &pos,
	PalEntry color, double size = 5.0)
{
	NK_DebugSpawnParticle(level, pos, color, size, 8.0);
	NK_DebugSpawnParticle(level, pos, color, size, 16.0);
	NK_DebugSpawnParticle(level, pos, color, size, 24.0);
}

static void NK_DebugDrawLine(FLevelLocals *level, const DVector3 &from,
	const DVector3 &to, PalEntry color, double spacing = 20.0,
	double size = 2.5)
{
	DVector3 delta = to - from;
	double distance = delta.Length();
	if (distance <= 0.001)
	{
		NK_DebugSpawnParticle(level, from, color, size);
		return;
	}

	int steps = std::clamp(int(std::ceil(distance / spacing)), 1, 64);
	for (int i = 0; i <= steps; ++i)
	{
		double fraction = double(i) / double(steps);
		NK_DebugSpawnParticle(level, from + delta * fraction, color, size);
	}
}

static const char *NK_DebugDirectionName(int direction)
{
	switch (direction)
	{
	case DI_EAST: return "E";
	case DI_NORTHEAST: return "NE";
	case DI_NORTH: return "N";
	case DI_NORTHWEST: return "NW";
	case DI_WEST: return "W";
	case DI_SOUTHWEST: return "SW";
	case DI_SOUTH: return "S";
	case DI_SOUTHEAST: return "SE";
	default: return "NONE";
	}
}

static void NK_DebugRecordHardBlockStall(AActor *actor)
{
	if (!actor || !actor->Level || nk_smartchase_debug < 3)
	{
		return;
	}

	int now = actor->Level->maptime;
	FNKHardBlockStallDebugState &state = NK_HardBlockStallDebug[actor];
	if (state.LastTic == now)
	{
		return;
	}
	state.Count = state.LastTic == now - 1 ? state.Count + 1 : 1;
	state.LastTic = now;

	if (state.Count < NK_HARD_BLOCK_STALL_LOG_TICS || now - state.LastLog < 35)
	{
		return;
	}

	bool pending = NK_PendingGroundSearches.find(actor) != NK_PendingGroundSearches.end();
	auto local = NK_LocalBypasses.find(actor);
	bool wallEscape = local != NK_LocalBypasses.end() && local->second.WallEscape;
	bool randomRecovery = local != NK_LocalBypasses.end() && local->second.RandomRecovery;
	Printf("SmartChase hard-block stall: actor=%p tics=%d pending=%d wallEscape=%d randomRecovery=%d path=%u/%u movedir=%s blocker=%p\n",
		actor, state.Count, pending ? 1 : 0, wallEscape ? 1 : 0,
		randomRecovery ? 1 : 0, unsigned(actor->nkSmartPathIndex),
		unsigned(actor->nkSmartPath.Size()), NK_DebugDirectionName(actor->movedir),
		actor->BlockingMobj);
	state.LastLog = now;
}

static std::unordered_map<AActor *, int> NK_DebugLastDivergenceLog;
static std::unordered_map<AActor *, int> NK_DebugLastRouteRejectLog;

static void NK_DebugDrawMoveDecision(AActor *actor, const DVector2 &beforeXY,
	const DVector3 &desiredGoal, int desiredDirection, bool usedNewChaseDir, bool moved,
	bool controlledPathRecovery = false)
{
	int mode = nk_smartchase_debug;
	if (!actor || !actor->Level || mode < 3)
	{
		return;
	}

	int now = actor->Level->maptime;
	bool desiredValid = desiredDirection >= DI_EAST && desiredDirection < DI_NODIR;
	bool actualValid = actor->movedir >= DI_EAST && actor->movedir < DI_NODIR;
	bool diverged = desiredValid && actualValid && actor->movedir != desiredDirection;
	bool unexpectedDivergence = diverged && !controlledPathRecovery;

	if (mode >= 4 && unexpectedDivergence && usedNewChaseDir)
	{
		int &lastLog = NK_DebugLastDivergenceLog[actor];
		if (lastLog == 0 || now - lastLog >= 7)
		{
			Printf("SmartChase diverged: desired=%s actual=%s path=%u/%u movecount=%d moved=%d\n",
				NK_DebugDirectionName(desiredDirection), NK_DebugDirectionName(actor->movedir),
				unsigned(actor->nkSmartPathIndex), unsigned(actor->nkSmartPath.Size()),
				actor->movecount, moved ? 1 : 0);
			lastLog = now;
		}
	}

	if (now % NK_DEBUG_DRAW_INTERVAL != 0)
	{
		return;
	}

	FLevelLocals *level = actor->Level;
	DVector3 baseOrigin(beforeXY.X, beforeXY.Y, actor->Z());
	DVector3 desiredOrigin = baseOrigin;
	DVector3 actualOrigin = baseOrigin;
	desiredOrigin.Z += 28.0;
	actualOrigin.Z += 60.0;

	const PalEntry desiredDirectionColor(96, 255, 96);
	const PalEntry actualDirectionColor(255, 144, 32);
	const PalEntry displacementColor(255, 255, 128);
	const PalEntry reselectColor(64, 220, 255);
	const PalEntry recoveryColor(32, 128, 255);
	const PalEntry divergenceColor(255, 0, 255);
	const PalEntry failureColor(255, 32, 32);

	if (desiredValid)
	{
		DVector3 desiredEnd(desiredOrigin.X + xspeed[desiredDirection] * 64.0,
			desiredOrigin.Y + yspeed[desiredDirection] * 64.0, desiredOrigin.Z);
		NK_DebugDrawLine(level, desiredOrigin, desiredEnd, desiredDirectionColor, 10.0, 3.5);
	}
	else
	{
		DVector2 desiredDelta = desiredGoal.XY() - beforeXY;
		if (desiredDelta.LengthSquared() > 0.0001)
		{
			desiredDelta.MakeUnit();
			DVector3 desiredEnd(desiredOrigin.X + desiredDelta.X * 64.0,
				desiredOrigin.Y + desiredDelta.Y * 64.0, desiredOrigin.Z);
			NK_DebugDrawLine(level, desiredOrigin, desiredEnd, desiredDirectionColor, 10.0, 3.5);
		}
	}

	if (actualValid)
	{
		DVector3 actualEnd(actualOrigin.X + xspeed[actor->movedir] * 52.0,
			actualOrigin.Y + yspeed[actor->movedir] * 52.0, actualOrigin.Z);
		NK_DebugDrawLine(level, actualOrigin, actualEnd, actualDirectionColor, 8.0, 4.0);
	}

	DVector3 after = actor->Pos();
	DVector3 displacementStart(baseOrigin.X, baseOrigin.Y, baseOrigin.Z + 8.0);
	DVector3 displacementEnd(after.X, after.Y, after.Z + 8.0);
	if (moved && (after.XY() - beforeXY).LengthSquared() > 0.0001)
	{
		NK_DebugDrawLine(level, displacementStart, displacementEnd,
			displacementColor, 8.0, 2.0);
	}

	if (usedNewChaseDir)
	{
		DVector3 reselectPos = baseOrigin;
		reselectPos.Z += 98.0;
		NK_DebugSpawnParticle(level, reselectPos, reselectColor, 5.0, 0.0);
	}

	if (mode >= 4 && controlledPathRecovery)
	{
		DVector3 recoveryPos = baseOrigin;
		recoveryPos.Z += 116.0;
		NK_DebugSpawnParticle(level, recoveryPos, recoveryColor, 6.0, 0.0);
	}

	if (mode >= 4 && unexpectedDivergence)
	{
		DVector3 marker = baseOrigin;
		marker.Z += 116.0;
		DVector3 firstA(marker.X - 10.0, marker.Y - 10.0, marker.Z);
		DVector3 firstB(marker.X + 10.0, marker.Y + 10.0, marker.Z);
		DVector3 secondA(marker.X - 10.0, marker.Y + 10.0, marker.Z);
		DVector3 secondB(marker.X + 10.0, marker.Y - 10.0, marker.Z);
		NK_DebugDrawLine(level, firstA, firstB, divergenceColor, 5.0, 5.0);
		NK_DebugDrawLine(level, secondA, secondB, divergenceColor, 5.0, 5.0);
		for (int i = 0; i < 4; ++i)
		{
			DVector3 pillar = marker;
			pillar.Z += double(i) * 10.0;
			NK_DebugSpawnParticle(level, pillar, divergenceColor, 5.0, 0.0);
		}
	}

	if (!moved)
	{
		DVector3 failureOrigin = baseOrigin;
		failureOrigin.Z += 12.0;
		DVector3 firstA(failureOrigin.X - 8.0, failureOrigin.Y - 8.0, failureOrigin.Z);
		DVector3 firstB(failureOrigin.X + 8.0, failureOrigin.Y + 8.0, failureOrigin.Z);
		DVector3 secondA(failureOrigin.X - 8.0, failureOrigin.Y + 8.0, failureOrigin.Z);
		DVector3 secondB(failureOrigin.X + 8.0, failureOrigin.Y - 8.0, failureOrigin.Z);
		NK_DebugDrawLine(level, firstA, firstB, failureColor, 6.0, 4.0);
		NK_DebugDrawLine(level, secondA, secondB, failureColor, 6.0, 4.0);
	}
}

static void NK_DebugDrawSmartChase(AActor *actor, const DVector3 &liveTarget,
	const DVector3 &predictedTarget)
{
	int mode = nk_smartchase_debug;
	if (!actor || !actor->Level || mode <= 0)
	{
		return;
	}

	int now = actor->Level->maptime;
	if (now % NK_DEBUG_DRAW_INTERVAL != 0)
	{
		return;
	}

	FLevelLocals *level = actor->Level;
	const PalEntry actorColor(160, 160, 160);
	const PalEntry liveTargetColor(255, 255, 255);
	const PalEntry predictedColor(255, 64, 64);
	const PalEntry tacticalColor(255, 220, 0);
	const PalEntry pathTargetColor(64, 128, 255);
	const PalEntry pathColor(0, 220, 255);
	const PalEntry waypointColor(64, 255, 64);
	const PalEntry searchStartColor(48, 96, 255);
	const PalEntry searchGoalColor(255, 144, 0);
	const PalEntry searchNodeColor(176, 96, 255);

	NK_DebugDrawMarker(level, actor->Pos(), actorColor, 4.0);
	NK_DebugDrawMarker(level, liveTarget, liveTargetColor, 5.0);
	NK_DebugDrawMarker(level, predictedTarget, predictedColor, 5.0);

	if (actor->nkSmartTacticalValid)
	{
		NK_DebugDrawMarker(level, actor->nkSmartTacticalGoal, tacticalColor, 6.0);
	}
	if (actor->nkSmartTargetValid)
	{
		NK_DebugDrawMarker(level, actor->nkSmartPathTarget, pathTargetColor, 5.0);
	}

	if (actor->nkSmartPathIndex < actor->nkSmartPath.Size())
	{
		DVector3 previous = actor->Pos();
		size_t drawn = 0;
		for (unsigned i = actor->nkSmartPathIndex;
			i < actor->nkSmartPath.Size() && drawn < NK_DEBUG_MAX_PATH_POINTS;
			++i, ++drawn)
		{
			const DVector3 &point = actor->nkSmartPath[i];
			NK_DebugDrawLine(level, previous, point, pathColor);
			previous = point;
		}
		NK_DebugDrawMarker(level,
			actor->nkSmartPath[actor->nkSmartPathIndex], waypointColor, 6.0);
	}

	if (mode < 2)
	{
		return;
	}

	// Mode 2+ also exposes the sliced Hypnotize flight search. This makes it
	// possible to distinguish a committed green/cyan path from the purple nodes
	// of a replacement route that is still being calculated after a hard block.
	auto pendingFlying = NK_PendingFlyingSearches.find(actor);
	if (pendingFlying != NK_PendingFlyingSearches.end() &&
		pendingFlying->second.Initialized)
	{
		const FNKPendingFlyingSearch &search = pendingFlying->second;
		NK_DebugDrawMarker(level, search.Start, searchStartColor, 5.0);
		NK_DebugDrawMarker(level, search.Goal, searchGoalColor, 6.0);
		size_t nodeCount = search.Nodes.size();
		size_t stride = std::max<size_t>(1,
			(nodeCount + NK_DEBUG_MAX_SEARCH_NODES - 1) / NK_DEBUG_MAX_SEARCH_NODES);
		for (size_t i = 0; i < nodeCount; i += stride)
		{
			if (search.Nodes[i].Closed)
			{
				NK_DebugSpawnParticle(level, search.Nodes[i].Pos,
					searchNodeColor, 2.0, 10.0);
			}
		}
		return;
	}

	auto pending = NK_PendingGroundSearches.find(actor);
	if (pending == NK_PendingGroundSearches.end() || !pending->second.Initialized)
	{
		return;
	}

	const FNKPendingGroundSearch &search = pending->second;
	NK_DebugDrawMarker(level, search.Start, searchStartColor, 5.0);
	NK_DebugDrawMarker(level, search.Goal, searchGoalColor, 6.0);

	size_t nodeCount = search.Nodes.size();
	size_t stride = std::max<size_t>(1,
		(nodeCount + NK_DEBUG_MAX_SEARCH_NODES - 1) / NK_DEBUG_MAX_SEARCH_NODES);
	for (size_t i = 0; i < nodeCount; i += stride)
	{
		if (search.Nodes[i].Closed)
		{
			NK_DebugSpawnParticle(level, search.Nodes[i].Pos,
				searchNodeColor, 2.0, 10.0);
		}
	}
}

struct FNKActorQueryRestore
{
	AActor *Actor;
	double Z;
	AActor *BlockingMobj;
	line_t *BlockingLine;
	line_t *MovementBlockingLine;
	sector_t *Blocking3DFloor;
	sector_t *BlockingCeiling;
	sector_t *BlockingFloor;

	explicit FNKActorQueryRestore(AActor *actor)
		: Actor(actor), Z(actor->Z()), BlockingMobj(actor->BlockingMobj),
		  BlockingLine(actor->BlockingLine), MovementBlockingLine(actor->MovementBlockingLine),
		  Blocking3DFloor(actor->Blocking3DFloor), BlockingCeiling(actor->BlockingCeiling),
		  BlockingFloor(actor->BlockingFloor)
	{
	}

	~FNKActorQueryRestore()
	{
		Actor->SetZ(Z);
		Actor->BlockingMobj = BlockingMobj;
		Actor->BlockingLine = BlockingLine;
		Actor->MovementBlockingLine = MovementBlockingLine;
		Actor->Blocking3DFloor = Blocking3DFloor;
		Actor->BlockingCeiling = BlockingCeiling;
		Actor->BlockingFloor = BlockingFloor;
	}
};

static bool NK_IsFlying(const AActor *actor)
{
	return (actor->flags & MF_NOGRAVITY) && (actor->flags & MF_FLOAT);
}

static double NK_CellSize(const AActor *actor, bool flying)
{
	if (!flying)
	{
		return 64.0;
	}

	double size = actor->radius * 2.0 + 20.0;
	return std::clamp(size, 32.0, 64.0);
}

static double NK_VerticalCellSize(const AActor *actor)
{
	// Keep the vertical lattice finer than the horizontal one. Using the same
	// spacing for both axes can skip valid flight bands between a 3D floor and
	// a ceiling, especially for actors with a small collision height.
	double size = actor->Height * 0.5 + 8.0;
	return std::clamp(size, 16.0, 40.0);
}

static DVector3 NK_TargetPosition(AActor *actor, bool flying)
{
	if (!actor->target)
	{
		return actor->Pos();
	}

	DVector3 result = actor->target->Pos();
	if (flying)
	{
		result.Z += actor->target->Height * 0.5 - actor->Height * 0.5;
	}
	return result;
}

static double NK_Heuristic(const DVector3 &from, const DVector3 &to, bool flying)
{
	DVector3 delta = to - from;
	if (!flying)
	{
		delta.Z = 0;
	}
	return delta.Length();
}

static bool NK_ClaimSearchBudget(AActor *actor)
{
	int now = actor->Level->maptime;
	if (NK_SearchBudgetLevel != actor->Level || NK_SearchBudgetTic != now)
	{
		NK_SearchBudgetLevel = actor->Level;
		NK_SearchBudgetTic = now;
		NK_SearchesThisTic = 0;
	}
	if (NK_SearchesThisTic >= 1)
	{
		return false;
	}
	NK_SearchesThisTic++;
	return true;
}

static uint32_t NK_RouteHash(const FNKGridKey &key, uint32_t seed)
{
	uint32_t value = seed ^ (uint32_t(key.X) * 0x9e3779b9u);
	value ^= uint32_t(key.Y) * 0x85ebca6bu;
	value ^= uint32_t(key.Z) * 0xc2b2ae35u;
	value ^= value >> 16;
	value *= 0x7feb352du;
	value ^= value >> 15;
	value *= 0x846ca68bu;
	value ^= value >> 16;
	return value;
}

static double NK_RouteBiasFactor(const FNKGridKey &key, uint32_t seed)
{
	if (seed == 0)
	{
		return 1.0;
	}
	double normalized = double(NK_RouteHash(key, seed) & 255u) / 255.0;
	return 1.0 + normalized * NK_ROUTE_BIAS;
}

static bool NK_CanUseBlockingLine(AActor *actor, line_t *line, const DVector3 &from)
{
	if (!line || !line->backsector || !line->special || (actor->flags6 & MF6_NOTRIGGER))
	{
		return false;
	}

	int side = P_PointOnLineSide(from.X, from.Y, line);
	if ((actor->flags4 & MF4_CANUSEWALLS) && P_TestActivateLine(line, actor, side, SPAC_Use))
	{
		return true;
	}
	if ((actor->flags2 & MF2_PUSHWALL) && P_TestActivateLine(line, actor, side, SPAC_Push))
	{
		return true;
	}
	return false;
}

static bool NK_CheckMoveAt(AActor *actor, const DVector3 &from, const DVector3 &candidate,
	bool flying, DVector3 &accepted)
{
	FNKActorQueryRestore restore(actor);
	actor->SetZ(flying ? candidate.Z : from.Z);

	FCheckPosition check;
	int flags = PCM_NOACTORS;
	if (!flying)
	{
		flags |= PCM_DROPOFF;
	}

	bool passable = P_CheckMove(actor, candidate.XY(), check, flags);
	line_t *blockingLine = actor->BlockingLine;
	if (!passable && NK_CanUseBlockingLine(actor, blockingLine, from))
	{
		accepted = candidate;
		accepted.Z = flying ? candidate.Z : from.Z;
		return true;
	}
	if (!passable)
	{
		return false;
	}

	accepted = candidate;
	if (!flying)
	{
		accepted.Z = check.floorz;
	}
	return true;
}

static bool NK_CanTraverse(AActor *actor, const DVector3 &from, const DVector3 &to,
	bool flying, DVector3 &accepted)
{
	DVector3 delta = to - from;
	double distance = flying ? delta.Length() : delta.XY().Length();
	double probeStep = flying
		? std::clamp(actor->radius * 0.75, 8.0, 24.0)
		: std::clamp(actor->radius * 1.5, 16.0, 32.0);
	int steps = std::max(1, int(std::ceil(distance / probeStep)));
	DVector3 current = from;

	for (int i = 1; i <= steps; ++i)
	{
		double fraction = double(i) / double(steps);
		DVector3 candidate = from + delta * fraction;
		if (!flying)
		{
			candidate.Z = current.Z;
		}

		DVector3 checked;
		if (!NK_CheckMoveAt(actor, current, candidate, flying, checked))
		{
			return false;
		}
		current = checked;
	}

	accepted = current;
	return true;
}

// Stationary surround goals may intentionally live on the far side of nearby
// walls so A* can discover a real side/rear route. Keep those searches more
// conservative than ordinary pursuit: sample the full actor volume every 8
// map units so a long shortcut cannot hop over a thin wall or clip a corner.
// This is deliberately surround-only; normal SmartChase keeps its established
// traversal behavior and performance.
static bool NK_CanTraverseStationarySurround(AActor *actor,
	const DVector3 &from, const DVector3 &to, DVector3 &accepted)
{
	DVector3 delta = to - from;
	double distance = delta.XY().Length();
	int steps = std::max(1, int(std::ceil(distance /
		NK_STATIONARY_SURROUND_PROBE_STEP)));
	DVector3 current = from;

	for (int i = 1; i <= steps; ++i)
	{
		double fraction = double(i) / double(steps);
		DVector3 candidate = from + delta * fraction;
		candidate.Z = current.Z;

		DVector3 checked;
		if (!NK_CheckMoveAt(actor, current, candidate, false, checked))
		{
			return false;
		}
		current = checked;
	}

	accepted = current;
	return true;
}

static FNKGroundProfile NK_MakeGroundProfile(const AActor *actor)
{
	FNKGroundProfile profile;
	profile.Radius = int(std::lround(actor->radius * 4.0));
	profile.Height = int(std::lround(actor->Height * 4.0));
	profile.MaxStep = int(std::lround(actor->MaxStepHeight * 4.0));
	profile.MaxDrop = int(std::lround(actor->MaxDropOffHeight * 4.0));
	profile.InteractionFlags = 0;
	if (actor->flags4 & MF4_CANUSEWALLS) profile.InteractionFlags |= 1u;
	if (actor->flags2 & MF2_PUSHWALL) profile.InteractionFlags |= 2u;
	if (actor->flags6 & MF6_NOTRIGGER) profile.InteractionFlags |= 4u;
	if (actor->flags8 & MF8_BLOCKASPLAYER) profile.InteractionFlags |= 8u;
	if (actor->player) profile.InteractionFlags |= 16u;
	if (actor->flags & MF_NOCLIP) profile.InteractionFlags |= 32u;
	return profile;
}

static void NK_PrepareGroundCache(AActor *actor)
{
	int now = actor->Level->maptime;
	if (NK_GroundCacheLevel != actor->Level || now < NK_GroundCacheLastTime)
	{
		NK_GroundConnections.clear();
		NK_GroundComponentLookup.clear();
		NK_GroundComponentNodes.clear();
		NK_PendingGroundSearches.clear();
		NK_PendingFlyingSearches.clear();
		NK_KitsuneSpiritRoams.clear();
		NK_HypnotizeBypasses.clear();
		NK_HypnotizeFlightSteeringStates.clear();
		NK_LocalBypasses.clear();
		NK_GroundSteeringStates.clear();
		NK_HardBlockStallDebug.clear();
		NK_CrowdDetours.clear();
		NK_GroundConnections.reserve(65536);
		NK_GroundComponentLookup.reserve(16384);
		NK_GroundComponentNodes.reserve(16384);
		NK_GroundCacheLevel = actor->Level;
		NK_GroundConnectionBudgetTic = -1;
		NK_GroundConnectionTestsThisTic = 0;
	}
	NK_GroundCacheLastTime = now;
	if (NK_GroundConnectionBudgetTic != now)
	{
		NK_GroundConnectionBudgetTic = now;
		NK_GroundConnectionTestsThisTic = 0;
		for (auto it = NK_PendingGroundSearches.begin();
			it != NK_PendingGroundSearches.end();)
		{
			if (now - it->second.LastTouched > 35)
			{
				it = NK_PendingGroundSearches.erase(it);
			}
			else
			{
				++it;
			}
		}
		for (auto it = NK_PendingFlyingSearches.begin();
			it != NK_PendingFlyingSearches.end();)
		{
			if (now - it->second.LastTouched > 35)
			{
				it = NK_PendingFlyingSearches.erase(it);
			}
			else
			{
				++it;
			}
		}
		for (auto it = NK_KitsuneSpiritRoams.begin();
			it != NK_KitsuneSpiritRoams.end();)
		{
			if (now - it->second.LastTouched > NK_KITSUNE_SPIRIT_DISCONNECTED_RETRY_TICS + 70)
			{
				it = NK_KitsuneSpiritRoams.erase(it);
			}
			else
			{
				++it;
			}
		}
		for (auto it = NK_HypnotizeBypasses.begin();
			it != NK_HypnotizeBypasses.end();)
		{
			if (now - it->second.LastTouched > 35)
			{
				it = NK_HypnotizeBypasses.erase(it);
			}
			else
			{
				++it;
			}
		}
		for (auto it = NK_HypnotizeFlightSteeringStates.begin();
			it != NK_HypnotizeFlightSteeringStates.end();)
		{
			if (now - it->second.LastTouched > 35)
			{
				it = NK_HypnotizeFlightSteeringStates.erase(it);
			}
			else
			{
				++it;
			}
		}
		for (auto it = NK_LocalBypasses.begin(); it != NK_LocalBypasses.end();)
		{
			if (now - it->second.LastTouched > 35)
			{
				it = NK_LocalBypasses.erase(it);
			}
			else
			{
				++it;
			}
		}
		for (auto it = NK_GroundSteeringStates.begin();
			it != NK_GroundSteeringStates.end();)
		{
			if (now - it->second.LastTouched > 35)
			{
				it = NK_GroundSteeringStates.erase(it);
			}
			else
			{
				++it;
			}
		}
		for (auto it = NK_HardBlockStallDebug.begin(); it != NK_HardBlockStallDebug.end();)
		{
			if (now - it->second.LastTic > 70)
			{
				it = NK_HardBlockStallDebug.erase(it);
			}
			else
			{
				++it;
			}
		}
		for (auto it = NK_CrowdDetours.begin(); it != NK_CrowdDetours.end();)
		{
			if (now - it->second.LastTouched > NK_CROWD_DETOUR_ACTIVE_TICS + 35)
			{
				it = NK_CrowdDetours.erase(it);
			}
			else
			{
				++it;
			}
		}
	}
}

static FNKGridKey NK_GroundWorldKey(const DVector3 &pos)
{
	FNKGridKey key;
	key.X = int(std::lround(pos.X / 64.0));
	key.Y = int(std::lround(pos.Y / 64.0));
	key.Z = int(std::lround(pos.Z / 8.0));
	return key;
}

static FNKGroundAnchorKey NK_GroundAnchorKey(const DVector3 &pos)
{
	FNKGroundAnchorKey key;
	key.Grid = NK_GroundWorldKey(pos);
	key.AnchorX = int(std::lround(pos.X / NK_GROUND_ANCHOR_QUANTUM));
	key.AnchorY = int(std::lround(pos.Y / NK_GROUND_ANCHOR_QUANTUM));
	return key;
}

static int NK_FindGroundComponentRoot(int index)
{
	int root = index;
	while (NK_GroundComponentNodes[root].Parent != root)
	{
		root = NK_GroundComponentNodes[root].Parent;
	}
	while (NK_GroundComponentNodes[index].Parent != index)
	{
		int parent = NK_GroundComponentNodes[index].Parent;
		NK_GroundComponentNodes[index].Parent = root;
		index = parent;
	}
	return root;
}

static int NK_AddGroundComponentNode(const FNKGroundProfile &profile,
	const DVector3 &pos)
{
	FNKGroundAnchorKey anchor = NK_GroundAnchorKey(pos);
	FNKGroundNodeKey key{ anchor, profile };
	auto found = NK_GroundComponentLookup.find(key);
	if (found != NK_GroundComponentLookup.end())
	{
		return found->second;
	}
	if (NK_GroundComponentNodes.size() >= NK_GROUND_COMPONENT_NODE_LIMIT)
	{
		return -1;
	}

	int index = int(NK_GroundComponentNodes.size());
	FNKGroundComponentNode node;
	node.Anchor = anchor;
	node.Profile = profile;
	node.Pos = pos;
	node.Parent = index;
	node.Members.push_back(index);
	NK_GroundComponentNodes.push_back(std::move(node));
	NK_GroundComponentLookup.emplace(key, index);
	return index;
}

static void NK_RecordGroundConnection(const FNKGroundProfile &profile,
	const DVector3 &from, const DVector3 &accepted)
{
	int first = NK_AddGroundComponentNode(profile, from);
	int second = NK_AddGroundComponentNode(profile, accepted);
	if (first < 0 || second < 0)
	{
		return;
	}

	int firstRoot = NK_FindGroundComponentRoot(first);
	int secondRoot = NK_FindGroundComponentRoot(second);
	if (firstRoot == secondRoot)
	{
		return;
	}
	if (NK_GroundComponentNodes[firstRoot].Members.size() <
		NK_GroundComponentNodes[secondRoot].Members.size())
	{
		std::swap(firstRoot, secondRoot);
	}

	FNKGroundComponentNode &destination = NK_GroundComponentNodes[firstRoot];
	FNKGroundComponentNode &source = NK_GroundComponentNodes[secondRoot];
	source.Parent = firstRoot;
	destination.Members.insert(destination.Members.end(),
		source.Members.begin(), source.Members.end());
	source.Members.clear();
}

static uint32_t NK_NewRouteSeed()
{
	uint32_t routeSeed = uint32_t(pr_nksmartchase()) |
		(uint32_t(pr_nksmartchase()) << 8) |
		(uint32_t(pr_nksmartchase()) << 16) |
		(uint32_t(pr_nksmartchase()) << 24);
	return routeSeed ? routeSeed : 1u;
}

static int NK_FindGroundComponentNodeNear(AActor *actor)
{
	FNKGroundProfile profile = NK_MakeGroundProfile(actor);
	int best = -1;
	double bestDistance = 96.0;
	double zTolerance = std::max(8.0, actor->MaxStepHeight + 8.0);
	for (size_t i = 0; i < NK_GroundComponentNodes.size(); ++i)
	{
		const FNKGroundComponentNode &node = NK_GroundComponentNodes[i];
		if (!(node.Profile == profile) || std::abs(node.Pos.Z - actor->Z()) > zTolerance)
		{
			continue;
		}
		double distance = (node.Pos.XY() - actor->Pos().XY()).Length();
		if (distance < bestDistance)
		{
			best = int(i);
			bestDistance = distance;
		}
	}
	return best;
}

static bool NK_SelectRandomRoamGoal(AActor *actor, DVector3 &goal)
{
	int current = NK_FindGroundComponentNodeNear(actor);
	if (current < 0)
	{
		return false;
	}
	int root = NK_FindGroundComponentRoot(current);
	const std::vector<int> &members = NK_GroundComponentNodes[root].Members;
	if (members.size() < 2)
	{
		return false;
	}

	int best = -1;
	double bestDistance = 0.0;
	for (int attempt = 0; attempt < NK_ROAM_CANDIDATE_ATTEMPTS; ++attempt)
	{
		int candidate = members[pr_nksmartchase(int(members.size()))];
		double distance = (NK_GroundComponentNodes[candidate].Pos.XY() -
			actor->Pos().XY()).Length();
		if (distance > bestDistance)
		{
			best = candidate;
			bestDistance = distance;
		}
		if (distance >= NK_ROAM_MIN_DISTANCE && distance <= NK_ROAM_MAX_DISTANCE &&
			(!actor->nkSmartTacticalValid ||
			(NK_GroundComponentNodes[candidate].Pos.XY() -
				actor->nkSmartTacticalGoal.XY()).Length() >= 192.0))
		{
			best = candidate;
			break;
		}
	}
	if (best < 0 || bestDistance < 128.0)
	{
		return false;
	}

	goal = NK_GroundComponentNodes[best].Pos;
	actor->nkSmartTacticalGoal = goal;
	actor->nkSmartTacticalValid = true;
	actor->nkSmartDisconnectedRoam = true;
	actor->nkSmartRoamNextTarget = actor->Level->maptime + NK_ROAM_MIN_TICS +
		pr_nksmartchase(NK_ROAM_TIC_VARIATION);
	actor->nkSmartRouteSeed = int(NK_NewRouteSeed());
	return true;
}

static bool NK_GetGroundConnection(AActor *actor, const FNKGridKey &fromKey,
	const DVector3 &from, int dx, int dy, DVector3 &accepted, bool &deferred,
	int &searchTests, int searchTestLimit, bool stationarySurround = false)
{
	NK_PrepareGroundCache(actor);
	FNKGroundProfile profile = NK_MakeGroundProfile(actor);
	FNKGroundConnectionKey key{ NK_GroundAnchorKey(from), dx, dy, profile };
	int now = actor->Level->maptime;
	auto found = NK_GroundConnections.find(key);
	// Ordinary pursuit can reuse the shared static connection cache. A stationary
	// surround search deliberately rechecks each edge with the denser wall probe
	// below so a permissive cached edge cannot become a wall-cutting shortcut.
	if (!stationarySurround && found != NK_GroundConnections.end())
	{
		int jitter = int(FNKGroundConnectionKeyHash{}(key) & 0x7fffffffu);
		int lifetime = found->second.Passable
			? NK_GROUND_PASS_CACHE_TICS + jitter % (NK_GROUND_PASS_CACHE_TICS / 2 + 1)
			: NK_GROUND_BLOCK_CACHE_TICS + jitter % (NK_GROUND_BLOCK_CACHE_TICS + 1);
		double originDrift = (from.XY() - found->second.FromXY).Length();
		if (originDrift <= NK_GROUND_CACHE_REUSE_DISTANCE &&
			std::abs(from.Z - found->second.FromZ) <= 1.0 &&
			now - found->second.CheckedAt < lifetime)
		{
			if (found->second.Passable)
			{
				accepted = found->second.Accepted;
			}
			return found->second.Passable;
		}
	}

	bool routeUnavailable = !actor->nkSmartTargetValid ||
		actor->nkSmartPathIndex >= actor->nkSmartPath.Size();
	bool baseBudgetExhausted =
		NK_GroundConnectionTestsThisTic >= NK_GROUND_CONNECTION_TESTS_PER_TIC;
	bool urgentAllowance = routeUnavailable && baseBudgetExhausted &&
		searchTests < NK_GROUND_URGENT_MIN_TESTS &&
		NK_GroundConnectionTestsThisTic < NK_GROUND_URGENT_CONNECTION_TESTS_PER_TIC;
	if (searchTests >= searchTestLimit || (baseBudgetExhausted && !urgentAllowance))
	{
		deferred = true;
		return false;
	}
	searchTests++;
	NK_GroundConnectionTestsThisTic++;

	DVector3 candidate((fromKey.X + dx) * 64.0,
		(fromKey.Y + dy) * 64.0, from.Z);
	DVector3 checked;
	bool passable = stationarySurround
		? NK_CanTraverseStationarySurround(actor, from, candidate, checked)
		: NK_CanTraverse(actor, from, candidate, false, checked);

	if (!stationarySurround)
	{
		FNKGroundConnection connection{ passable, passable ? checked : candidate,
			from.XY(), from.Z, now };
		if (found != NK_GroundConnections.end())
		{
			found->second = connection;
		}
		else if (NK_GroundConnections.size() < NK_GROUND_CONNECTION_CACHE_LIMIT)
		{
			NK_GroundConnections.emplace(key, connection);
		}
	}
	if (passable)
	{
		accepted = checked;
		if (!stationarySurround)
		{
			NK_RecordGroundConnection(profile, from, accepted);
		}
	}
	return passable;
}

static void NK_InvalidateGroundConnectionsNearPosition(AActor *actor,
	const DVector3 &position)
{
	NK_PrepareGroundCache(actor);
	FNKGridKey center = NK_GroundWorldKey(position);
	FNKGroundProfile profile = NK_MakeGroundProfile(actor);
	int zRange = std::clamp(int(std::ceil(actor->MaxStepHeight / 8.0)) + 1, 2, 8);
	for (auto it = NK_GroundConnections.begin(); it != NK_GroundConnections.end();)
	{
		const FNKGroundConnectionKey &key = it->first;
		const FNKGridKey &from = key.From.Grid;
		if (key.Profile == profile &&
			std::abs(from.X - center.X) <= 2 &&
			std::abs(from.Y - center.Y) <= 2 &&
			std::abs(from.Z - center.Z) <= zRange)
		{
			it = NK_GroundConnections.erase(it);
		}
		else
		{
			++it;
		}
	}
}

static void NK_InvalidateGroundConnectionsNear(AActor *actor)
{
	NK_InvalidateGroundConnectionsNearPosition(actor, actor->Pos());
}

static FNKGridKey NK_MakeKey(const DVector3 &origin, const DVector3 &pos,
	double cellSize, double verticalCellSize, bool flying)
{
	FNKGridKey key;
	key.X = int(std::lround((pos.X - origin.X) / cellSize));
	key.Y = int(std::lround((pos.Y - origin.Y) / cellSize));
	if (flying)
	{
		key.Z = int(std::lround((pos.Z - origin.Z) / verticalCellSize));
	}
	else
	{
		key.Z = int(std::lround(pos.Z / 8.0));
	}
	return key;
}

static DVector3 NK_GridPosition(const DVector3 &origin, const FNKGridKey &key,
	double cellSize, double verticalCellSize, bool flying, double groundZ)
{
	DVector3 result(origin.X + key.X * cellSize, origin.Y + key.Y * cellSize, groundZ);
	if (flying)
	{
		result.Z = origin.Z + key.Z * verticalCellSize;
	}
	return result;
}

static bool NK_CrowdDetourActive(AActor *actor, uint32_t *generation = nullptr)
{
	if (!actor || !actor->Level)
	{
		return false;
	}

	auto found = NK_CrowdDetours.find(actor);
	if (found == NK_CrowdDetours.end())
	{
		return false;
	}

	FNKCrowdDetourState &state = found->second;
	if (actor->Level->maptime > state.ActiveUntil)
	{
		return false;
	}
	state.LastTouched = actor->Level->maptime;
	if (generation)
	{
		*generation = state.Generation;
	}
	return state.Generation != 0;
}

static bool NK_CrowdDetourNeedsPath(AActor *actor)
{
	uint32_t generation = 0;
	if (!NK_CrowdDetourActive(actor, &generation))
	{
		return false;
	}
	auto found = NK_CrowdDetours.find(actor);
	return found != NK_CrowdDetours.end() &&
		found->second.AppliedGeneration != generation;
}

static void NK_MarkCrowdDetourPathApplied(AActor *actor)
{
	uint32_t generation = 0;
	if (!NK_CrowdDetourActive(actor, &generation))
	{
		return;
	}
	auto found = NK_CrowdDetours.find(actor);
	if (found != NK_CrowdDetours.end())
	{
		found->second.AppliedGeneration = generation;
		// Route commitment should begin when the replacement route is actually
		// available, not when its A* generation was requested. A crowded search can
		// take several tics, and consuming the commitment while it is still pending
		// makes an otherwise fresh route eligible for an immediate retrigger.
		if (actor->Level)
		{
			found->second.RouteCommitUntil =
				actor->Level->maptime + NK_CROWD_ROUTE_COMMIT_TICS;
		}
	}
}

static bool NK_CrowdGenerationInFlight(AActor *actor, uint32_t generation)
{
	if (!actor || generation == 0)
	{
		return false;
	}

	auto pending = NK_PendingGroundSearches.find(actor);
	return pending != NK_PendingGroundSearches.end() &&
		pending->second.Initialized && pending->second.CrowdDetour &&
		pending->second.CrowdGeneration == generation;
}

static bool NK_CrowdRetreatOwnsGeneration(AActor *actor, uint32_t generation)
{
	if (!actor || generation == 0)
	{
		return false;
	}

	auto local = NK_LocalBypasses.find(actor);
	return local != NK_LocalBypasses.end() &&
		(local->second.CrowdRetreat || local->second.CrowdRetreatMomentum) &&
		local->second.CrowdRetreatDetourStarted &&
		local->second.CrowdRetreatGeneration == generation;
}

static void NK_ActivateCrowdDetour(AActor *actor, FNKCrowdDetourState &state,
	int now, const DVector2 &hotspot, bool forceNewGeneration = false)
{
	if (!actor || !actor->Level)
	{
		return;
	}

	// Do not throw away a Crowd A* generation that is already doing useful work.
	// Repeated congestion samples can arrive while the sliced search is pending,
	// especially when several monsters retreat at once. Refresh the live crowd
	// metadata, but keep the same generation so the search can finish.
	if (!forceNewGeneration && state.Generation != 0 &&
		(NK_CrowdGenerationInFlight(actor, state.Generation) ||
		 NK_CrowdRetreatOwnsGeneration(actor, state.Generation)))
	{
		state.BlockCount = 0;
		state.LastTouched = now;
		state.LastBlockerCenter = hotspot;
		state.ActiveUntil = std::max(state.ActiveUntil,
			now + NK_CROWD_DETOUR_ACTIVE_TICS);
		return;
	}

	// Once a route from this generation has been committed, honor its full
	// hysteresis window before allowing congestion noise to replace it again.
	if (!forceNewGeneration && state.Generation != 0 &&
		state.AppliedGeneration == state.Generation && now <= state.RouteCommitUntil)
	{
		state.BlockCount = 0;
		state.LastTouched = now;
		state.LastBlockerCenter = hotspot;
		state.ActiveUntil = std::max(state.ActiveUntil,
			now + NK_CROWD_DETOUR_ACTIVE_TICS);
		return;
	}

	if (!forceNewGeneration && state.LastTriggerTic >= 0 &&
		now - state.LastTriggerTic < NK_CROWD_DETOUR_RETRIGGER_TICS)
	{
		return;
	}

	state.BlockCount = 0;
	state.LastTriggerTic = now;
	state.LastTouched = now;
	state.LastBlockerCenter = hotspot;
	state.ActiveUntil = now + NK_CROWD_DETOUR_ACTIVE_TICS;
	state.RouteCommitUntil = now + NK_CROWD_ROUTE_COMMIT_TICS;
	state.Generation++;
	if (state.Generation == 0)
	{
		state.Generation = 1;
	}
	state.SteeringDirection = DI_NODIR;
	state.SteeringHoldUntil = -1;
	state.ProgressLowWindows = 0;
	actor->nkSmartNextRepath = 0;
	NK_PendingGroundSearches.erase(actor);

	if (nk_smartchase_debug >= 3)
	{
		Printf("SmartChase crowd detour activated: actor=%p generation=%u\n",
			actor, unsigned(state.Generation));
	}
}

static uint32_t NK_ForceCrowdDetourAfterRetreat(AActor *actor,
	const DVector2 &hotspot)
{
	if (!actor || !actor->Level)
	{
		return 0;
	}

	int now = actor->Level->maptime;
	FNKCrowdDetourState &state = NK_CrowdDetours[actor];
	state.LastTouched = now;

	// A retreat cycle owns exactly one replacement generation. If that generation
	// is already being searched or has been applied, keep it instead of erasing the
	// open set and starting over from another nearly-identical retreat position.
	auto local = NK_LocalBypasses.find(actor);
	if (local != NK_LocalBypasses.end() &&
		local->second.CrowdRetreatDetourStarted &&
		local->second.CrowdRetreatGeneration != 0 &&
		local->second.CrowdRetreatGeneration == state.Generation)
	{
		state.LastBlockerCenter = hotspot;
		state.ActiveUntil = std::max(state.ActiveUntil,
			now + NK_CROWD_DETOUR_ACTIVE_TICS);
		return state.Generation;
	}

	// Retreat deliberately changes the physical start position. Force one fresh
	// generation from that new start even if the normal retrigger cooldown is
	// still active; otherwise the actor can immediately inherit the same crowded
	// route it just backed away from.
	state.LastTriggerTic = now - NK_CROWD_DETOUR_RETRIGGER_TICS;
	NK_ActivateCrowdDetour(actor, state, now, hotspot, true);
	return state.Generation;
}

static void NK_RecordCrowdBlock(AActor *actor, AActor *blocker)
{
	if (!actor || !actor->Level || !blocker || blocker == actor->target ||
		(blocker->ObjectFlags & OF_EuthanizeMe))
	{
		return;
	}

	int now = actor->Level->maptime;
	FNKCrowdDetourState &state = NK_CrowdDetours[actor];
	state.LastTouched = now;
	if (state.LastBlockTic == now)
	{
		state.LastBlockerCenter = blocker->Pos().XY();
		return;
	}

	bool newWindow = now > state.WindowUntil ||
		(state.WindowOrigin - actor->Pos().XY()).Length() > 384.0;
	if (newWindow)
	{
		state.BlockCount = 0;
		state.WindowOrigin = actor->Pos().XY();
	}
	state.WindowUntil = now + NK_CROWD_DETOUR_WINDOW_TICS;
	state.LastBlockTic = now;
	state.LastBlockerCenter = blocker->Pos().XY();
	state.BlockCount++;

	if (state.BlockCount >= NK_CROWD_DETOUR_BLOCK_COUNT)
	{
		NK_ActivateCrowdDetour(actor, state, now, blocker->Pos().XY());
	}
}

static double NK_DistanceToSegment2D(const DVector2 &point,
	const DVector2 &from, const DVector2 &to)
{
	DVector2 segment = to - from;
	double lengthSquared = segment.X * segment.X + segment.Y * segment.Y;
	if (lengthSquared <= 0.000001)
	{
		return (point - from).Length();
	}
	DVector2 offset = point - from;
	double t = (offset.X * segment.X + offset.Y * segment.Y) / lengthSquared;
	t = std::clamp(t, 0.0, 1.0);
	DVector2 closest = from + segment * t;
	return (point - closest).Length();
}

static bool NK_FindCrowdLaneObstruction(AActor *actor, const DVector2 &goal,
	AActor *&nearestBlocker, int &blockerCount)
{
	nearestBlocker = nullptr;
	blockerCount = 0;
	if (!actor || !actor->Level)
	{
		return false;
	}

	DVector2 start = actor->Pos().XY();
	DVector2 delta = goal - start;
	double fullLength = delta.Length();
	if (fullLength <= 0.0001)
	{
		return false;
	}

	double probeLength = std::min(fullLength, NK_CROWD_FORWARD_PROBE_RANGE);
	DVector2 end = start + delta * (probeLength / fullLength);
	DVector2 segment = end - start;
	double lengthSquared = segment.X * segment.X + segment.Y * segment.Y;
	double nearestAlong = probeLength + 1.0;
	double actorBottom = actor->Z();
	double actorTop = actorBottom + actor->Height;

	auto iterator = actor->Level->GetThinkerIterator<AActor>();
	AActor *other;
	while ((other = iterator.Next()) != nullptr)
	{
		if (other == actor || other == actor->target ||
			(other->ObjectFlags & OF_EuthanizeMe) ||
			!(other->flags & MF_SOLID) || (other->flags & MF_NOBLOCKMAP))
		{
			continue;
		}

		double otherBottom = other->Z();
		double otherTop = otherBottom + other->Height;
		if (actorTop <= otherBottom || otherTop <= actorBottom)
		{
			continue;
		}

		DVector2 offset = other->Pos().XY() - start;
		double t = (offset.X * segment.X + offset.Y * segment.Y) / lengthSquared;
		if (t <= 0.02 || t >= 0.995)
		{
			continue;
		}

		DVector2 closest = start + segment * t;
		double clearance = actor->radius + other->radius + 6.0;
		if ((other->Pos().XY() - closest).Length() > clearance)
		{
			continue;
		}

		blockerCount++;
		double along = probeLength * t;
		if (along < nearestAlong)
		{
			nearestAlong = along;
			nearestBlocker = other;
		}
	}

	return nearestBlocker != nullptr;
}

static void NK_RecordCrowdCongestion(AActor *actor, AActor *blocker,
	int blockerCount, bool lowProgress)
{
	if (!actor || !actor->Level || !blocker || blocker == actor->target)
	{
		return;
	}

	int now = actor->Level->maptime;
	FNKCrowdDetourState &state = NK_CrowdDetours[actor];
	state.LastTouched = now;
	state.LastBlockerCenter = blocker->Pos().XY();

	// Two or more actors occupying the forward lane are already strong evidence
	// that this is a congested passage rather than a momentary bump. A low-progress
	// window gives the same evidence even when only one actor is directly sampled.
	if (blockerCount >= 2 || (lowProgress && state.ProgressLowWindows >= 2))
	{
		NK_ActivateCrowdDetour(actor, state, now, blocker->Pos().XY());
		return;
	}

	NK_RecordCrowdBlock(actor, blocker);
}

static bool NK_ShouldCommitCrowdRoute(AActor *actor)
{
	if (!actor || !actor->Level)
	{
		return false;
	}

	auto found = NK_CrowdDetours.find(actor);
	if (found == NK_CrowdDetours.end())
	{
		return false;
	}

	FNKCrowdDetourState &state = found->second;
	return state.Generation != 0 && state.AppliedGeneration == state.Generation &&
		actor->Level->maptime <= state.RouteCommitUntil;
}

static void NK_UpdateCrowdRouteProgress(AActor *actor, const DVector2 &probeGoal)
{
	if (!actor || !actor->Level || actor->nkSmartPathIndex >= actor->nkSmartPath.Size())
	{
		return;
	}

	int now = actor->Level->maptime;
	FNKCrowdDetourState &state = NK_CrowdDetours[actor];
	state.LastTouched = now;
	if (state.LastProgressSampleTic == now)
	{
		return;
	}
	state.LastProgressSampleTic = now;

	int pathIndex = int(actor->nkSmartPathIndex);
	DVector2 waypoint = actor->nkSmartPath[actor->nkSmartPathIndex].XY();
	double distance = (waypoint - actor->Pos().XY()).Length();
	bool reset = state.ProgressWindowStart < 0 ||
		state.ProgressPathIndex != pathIndex ||
		(state.ProgressWaypoint - waypoint).Length() > 32.0;
	if (reset)
	{
		state.ProgressPathIndex = pathIndex;
		state.ProgressWaypoint = waypoint;
		state.ProgressDistance = distance;
		state.ProgressWindowStart = now;
		state.ProgressLowWindows = 0;
		return;
	}

	if (now - state.ProgressWindowStart < NK_CROWD_PROGRESS_WINDOW_TICS)
	{
		return;
	}

	double gain = state.ProgressDistance - distance;
	double requiredGain = std::max(NK_CROWD_PROGRESS_MIN_GAIN, actor->Speed * 2.0);
	AActor *blocker = nullptr;
	int blockerCount = 0;
	bool laneBlocked = NK_FindCrowdLaneObstruction(actor, probeGoal, blocker, blockerCount);

	if (gain < requiredGain && laneBlocked)
	{
		state.ProgressLowWindows++;
		NK_RecordCrowdCongestion(actor, blocker, blockerCount, true);
	}
	else
	{
		state.ProgressLowWindows = 0;
	}

	state.ProgressDistance = distance;
	state.ProgressWindowStart = now;
	state.ProgressPathIndex = pathIndex;
	state.ProgressWaypoint = waypoint;
}

static bool NK_ShouldDeferDirectPursuitForCrowd(AActor *actor, const DVector2 &goal)
{
	if (!actor || !actor->Level)
	{
		return false;
	}

	// A committed local crowd escape owns steering even inside the 128-unit
	// direct-pursuit range. Without this guard a single temporarily clear sample
	// can hand control straight back to Direct Pursuit while the actor is still
	// retreating or rounding a blocker.
	auto local = NK_LocalBypasses.find(actor);
	if (local != NK_LocalBypasses.end() && local->second.Crowd &&
		(local->second.CrowdRetreat || local->second.CrowdRetreatMomentum ||
			local->second.GoalValid))
	{
		return true;
	}

	AActor *blocker = nullptr;
	int blockerCount = 0;
	if (NK_FindCrowdLaneObstruction(actor, goal, blocker, blockerCount))
	{
		NK_RecordCrowdCongestion(actor, blocker, blockerCount, false);
		return true;
	}

	// After choosing a crowd-aware route, give it a short commitment window.
	// Without this hysteresis one sideways step can make the direct lane appear
	// clear for a single tic and pull the monster straight back into the same queue.
	return NK_ShouldCommitCrowdRoute(actor);
}

static void NK_CollectCrowdSamples(AActor *actor, const DVector3 &start,
	const DVector3 &goal, std::vector<FNKCrowdSample> &samples)
{
	samples.clear();
	if (!actor || !actor->Level)
	{
		return;
	}

	DVector2 startXY = start.XY();
	DVector2 goalXY = goal.XY();
	auto stateFound = NK_CrowdDetours.find(actor);
	DVector2 hotspot = stateFound != NK_CrowdDetours.end()
		? stateFound->second.LastBlockerCenter : startXY;

	struct FCandidate
	{
		FNKCrowdSample Sample;
		double Priority;
	};
	std::vector<FCandidate> candidates;
	auto iterator = actor->Level->GetThinkerIterator<AActor>();
	AActor *other;
	while ((other = iterator.Next()) != nullptr)
	{
		if (other == actor || other == actor->target ||
			(other->ObjectFlags & OF_EuthanizeMe) ||
			!(other->flags & MF_SOLID) || (other->flags & MF_NOBLOCKMAP))
		{
			continue;
		}

		DVector2 pos = other->Pos().XY();
		double fromStart = (pos - startXY).Length();
		double fromHotspot = (pos - hotspot).Length();
		double fromRoute = NK_DistanceToSegment2D(pos, startXY, goalXY);
		if (fromStart > NK_CROWD_DETOUR_SAMPLE_RANGE &&
			fromHotspot > NK_CROWD_DETOUR_SAMPLE_RANGE * 0.65 &&
			fromRoute > NK_CROWD_DETOUR_SAMPLE_RANGE * 0.35)
		{
			continue;
		}

		FNKCrowdSample sample;
		sample.Pos = pos;
		sample.Clearance = actor->radius + other->radius;
		sample.Weight = fromHotspot <= 192.0 ? 1.5 : 1.0;
		double priority = std::min(fromStart, fromHotspot * 0.8) + fromRoute * 0.35;
		candidates.push_back({ sample, priority });
	}

	std::sort(candidates.begin(), candidates.end(),
		[](const FCandidate &a, const FCandidate &b) { return a.Priority < b.Priority; });
	if (candidates.size() > NK_CROWD_DETOUR_MAX_SAMPLES)
	{
		candidates.resize(NK_CROWD_DETOUR_MAX_SAMPLES);
	}
	samples.reserve(candidates.size());
	for (const FCandidate &candidate : candidates)
	{
		samples.push_back(candidate.Sample);
	}
}

static double NK_CrowdPointCost(const std::vector<FNKCrowdSample> &samples,
	const DVector2 &point)
{
	double cost = 0.0;
	for (const FNKCrowdSample &sample : samples)
	{
		double distance = (point - sample.Pos).Length();
		double influence = sample.Clearance + NK_CROWD_DETOUR_INFLUENCE;
		if (distance >= influence)
		{
			continue;
		}
		double ratio = 1.0 - distance / std::max(1.0, influence);
		cost += NK_CROWD_DETOUR_STEP_COST * sample.Weight * ratio * ratio;
		if (distance < sample.Clearance + NK_CROWD_DETOUR_CORE_MARGIN)
		{
			cost += NK_CROWD_DETOUR_STEP_COST * 0.75 * sample.Weight;
		}
	}
	return cost;
}

static bool NK_CrowdShortcutClear(const std::vector<FNKCrowdSample> *samples,
	const DVector2 &from, const DVector2 &to)
{
	if (!samples || samples->empty())
	{
		return true;
	}
	for (const FNKCrowdSample &sample : *samples)
	{
		double core = sample.Clearance + NK_CROWD_DETOUR_CORE_MARGIN;
		if (NK_DistanceToSegment2D(sample.Pos, from, to) < core)
		{
			return false;
		}
	}
	return true;
}

static void NK_CompressGroundRoute(AActor *actor, const DVector3 &start,
	const std::vector<DVector3> &rawPath, TArray<DVector3> &outPath,
	const std::vector<FNKCrowdSample> *crowdSamples = nullptr)
{
	std::vector<DVector3> compactPath;
	compactPath.reserve(rawPath.size());
	for (size_t i = 0; i < rawPath.size(); ++i)
	{
		if (i > 0 && i + 1 < rawPath.size())
		{
			DVector2 firstDelta = rawPath[i].XY() - rawPath[i - 1].XY();
			DVector2 secondDelta = rawPath[i + 1].XY() - rawPath[i].XY();
			double cross = firstDelta.X * secondDelta.Y - firstDelta.Y * secondDelta.X;
			double dot = firstDelta.X * secondDelta.X + firstDelta.Y * secondDelta.Y;
			if (std::abs(cross) < 0.001 && dot > 0.0)
			{
				continue;
			}
		}
		compactPath.push_back(rawPath[i]);
	}

	DVector3 anchor = start;
	size_t first = 0;
	while (first < compactPath.size())
	{
		// The old code shared four shortcut probes across the entire route. Once
		// those probes were spent near the start, a long diagonal/stair-step route
		// could keep many unnecessary grid turns and make the actor visibly dither.
		// Give each committed anchor its own small probe budget instead.
		int shortcutChecks = 0;
		size_t chosen = first;
		DVector3 chosenPosition = compactPath[first];
		size_t lastTest = std::min(compactPath.size() - 1,
			first + NK_GROUND_SHORTCUT_LOOKAHEAD);
		for (size_t test = lastTest;
			test > first && shortcutChecks < NK_GROUND_SHORTCUT_CHECKS; --test)
		{
			shortcutChecks++;
			DVector3 smoothed;
			if (NK_CrowdShortcutClear(crowdSamples, anchor.XY(), compactPath[test].XY()) &&
				NK_CanTraverse(actor, anchor, compactPath[test], false, smoothed))
			{
				chosen = test;
				chosenPosition = smoothed;
				break;
			}
		}
		outPath.Push(chosenPosition);
		anchor = chosenPosition;
		first = chosen + 1;
	}
}

static bool NK_ValidateGroundRoute(AActor *actor, const DVector3 &start,
	TArray<DVector3> &path, DVector3 &blockedFrom)
{
	DVector3 anchor = start;
	for (unsigned i = 0; i < path.Size(); ++i)
	{
		DVector3 checked;
		if (!NK_CanTraverse(actor, anchor, path[i], false, checked))
		{
			blockedFrom = anchor;
			if (nk_smartchase_debug >= 4 && actor->Level)
			{
				int now = actor->Level->maptime;
				int &lastLog = NK_DebugLastRouteRejectLog[actor];
				if (lastLog == 0 || now - lastLog >= 7)
				{
					Printf("SmartChase route rejected: segment=%u/%u from=(%.1f %.1f) to=(%.1f %.1f)\n",
						unsigned(i), unsigned(path.Size()), anchor.X, anchor.Y, path[i].X, path[i].Y);
					lastLog = now;
				}
				const PalEntry rejectedRouteColor(255, 32, 32);
				NK_DebugDrawLine(actor->Level, anchor, path[i], rejectedRouteColor, 12.0, 3.5);
				NK_DebugDrawMarker(actor->Level, path[i], rejectedRouteColor, 6.0);
			}
			return false;
		}
		path[i] = checked;
		anchor = checked;
	}
	return path.Size() > 0;
}

static bool NK_ReconnectGroundRoute(AActor *actor,
	const std::vector<DVector3> &rawPath, TArray<DVector3> &outPath,
	DVector3 &blockedFrom, bool &reconnected,
	const std::vector<FNKCrowdSample> *crowdSamples = nullptr,
	bool stationarySurround = false)
{
	outPath.Clear();
	reconnected = false;
	if (rawPath.empty())
	{
		blockedFrom = actor->Pos();
		return false;
	}

	// A sliced A* search may finish several tics after it was started. During
	// that time the actor is intentionally allowed to keep following its old
	// committed route, so search.Start can be stale by the time the new route
	// is ready. Join the completed route at the furthest waypoint that is still
	// directly reachable from the actor's current position instead of forcing a
	// connection back to the old search origin.
	size_t reconnectIndex = rawPath.size();
	DVector3 reconnectPosition;
	// A surround search is intentionally willing to put its destination beyond a
	// wall and discover the long way around. Do not then erase that discovery by
	// reconnecting directly to a far waypoint. At most join the first or second
	// raw A* step, and validate that short join with the dense wall probe.
	size_t reconnectMax = rawPath.size() - 1;
	if (stationarySurround)
	{
		reconnectMax = std::min(reconnectMax,
			NK_STATIONARY_SURROUND_RECONNECT_MAX_INDEX);
	}
	for (size_t i = reconnectMax + 1; i-- > 0;)
	{
		DVector3 checked;
		bool reachable = stationarySurround
			? NK_CanTraverseStationarySurround(actor, actor->Pos(), rawPath[i], checked)
			: NK_CanTraverse(actor, actor->Pos(), rawPath[i], false, checked);
		if (reachable)
		{
			reconnectIndex = i;
			reconnectPosition = checked;
			reconnected = true;
			break;
		}
		if (i == 0)
		{
			break;
		}
	}

	if (!reconnected)
	{
		blockedFrom = actor->Pos();
		return false;
	}

	std::vector<DVector3> joinedPath;
	joinedPath.reserve(rawPath.size() - reconnectIndex);
	joinedPath.push_back(reconnectPosition);
	for (size_t i = reconnectIndex + 1; i < rawPath.size(); ++i)
	{
		joinedPath.push_back(rawPath[i]);
	}

	if (stationarySurround)
	{
		// Preserve every raw A* step for surround routes. These are the routes most
		// likely to wrap around nearby walls, so any-angle compression provides less
		// value than keeping the exact sequence of validated grid connections.
		for (const DVector3 &point : joinedPath)
		{
			outPath.Push(point);
		}
		DVector3 anchor = actor->Pos();
		for (unsigned i = 0; i < outPath.Size(); ++i)
		{
			DVector3 checked;
			if (!NK_CanTraverseStationarySurround(actor, anchor, outPath[i], checked))
			{
				blockedFrom = anchor;
				outPath.Clear();
				return false;
			}
			outPath[i] = checked;
			anchor = checked;
		}
		return outPath.Size() > 0;
	}

	NK_CompressGroundRoute(actor, actor->Pos(), joinedPath, outPath, crowdSamples);
	return NK_ValidateGroundRoute(actor, actor->Pos(), outPath, blockedFrom);
}

static int NK_GroundSearchTestSlice(AActor *actor)
{
	NK_PrepareGroundCache(actor);
	int now = actor->Level->maptime;
	int activeSearches = 0;
	for (const auto &entry : NK_PendingGroundSearches)
	{
		if (entry.second.Initialized && now - entry.second.LastTouched <= 1)
		{
			activeSearches++;
		}
	}
	return std::clamp(NK_GROUND_CONNECTION_TESTS_PER_TIC /
		std::max(1, activeSearches), 6, 32);
}

static void NK_StartPendingGroundSearch(FNKPendingGroundSearch &search,
	AActor *actor, const DVector3 &goal, uint32_t routeSeed, bool weighted)
{
	search.Start = actor->Pos();
	search.Goal = goal;
	search.Profile = NK_MakeGroundProfile(actor);
	search.RouteSeed = routeSeed;
	search.Weighted = weighted;
	search.CrowdDetour = NK_CrowdDetourActive(actor, &search.CrowdGeneration);
	search.SurroundGoal = false;
	auto crowdState = NK_CrowdDetours.find(actor);
	if (crowdState != NK_CrowdDetours.end() &&
		crowdState->second.SurroundGoalActive && actor->nkSmartTacticalValid &&
		(actor->nkSmartTacticalGoal.XY() - goal.XY()).LengthSquared() <= 1.0)
	{
		search.SurroundGoal = true;
	}
	search.CrowdSamples.clear();
	if (search.CrowdDetour)
	{
		NK_CollectCrowdSamples(actor, search.Start, search.Goal, search.CrowdSamples);
	}
	search.Initialized = true;
	search.LastTouched = actor->Level->maptime;
	search.Nodes.clear();
	search.Nodes.reserve(NK_GROUND_MAX_NODES);
	search.NodeLookup.clear();
	search.NodeLookup.reserve(NK_GROUND_MAX_NODES * 2);
	search.Open = std::priority_queue<FNKOpenNode>();

	FNKGridKey startKey = NK_GroundWorldKey(search.Start);
	NK_AddGroundComponentNode(search.Profile, search.Start);
	search.Nodes.push_back({ startKey, search.Start, 0.0, -1, false });
	search.NodeLookup.emplace(NK_GroundAnchorKey(search.Start), 0);
	search.Open.push({ NK_Heuristic(search.Start, goal, false) *
		(weighted ? NK_WEIGHTED_HEURISTIC : 1.0), 0 });
}

static bool NK_BuildRoamRouteFromSearch(AActor *actor,
	FNKPendingGroundSearch &search, TArray<DVector3> &outPath)
{
	if (search.Nodes.size() < 2)
	{
		return false;
	}

	int best = -1;
	double bestDistance = 0.0;
	for (int attempt = 0; attempt < NK_ROAM_CANDIDATE_ATTEMPTS; ++attempt)
	{
		int candidate = 1 + pr_nksmartchase(int(search.Nodes.size()) - 1);
		double distance = (search.Nodes[candidate].Pos.XY() - actor->Pos().XY()).Length();
		if (distance > bestDistance)
		{
			best = candidate;
			bestDistance = distance;
		}
		if (distance >= NK_ROAM_MIN_DISTANCE && distance <= NK_ROAM_MAX_DISTANCE)
		{
			best = candidate;
			bestDistance = distance;
			break;
		}
	}
	if (best < 0 || bestDistance < 128.0)
	{
		return false;
	}

	std::vector<DVector3> reversePath;
	for (int index = best; index > 0; index = search.Nodes[index].Parent)
	{
		reversePath.push_back(search.Nodes[index].Pos);
		if (search.Nodes[index].Parent < 0)
		{
			break;
		}
	}
	std::reverse(reversePath.begin(), reversePath.end());
	NK_CompressGroundRoute(actor, actor->Pos(), reversePath, outPath);
	DVector3 blockedFrom;
	if (!NK_ValidateGroundRoute(actor, actor->Pos(), outPath, blockedFrom))
	{
		outPath.Clear();
		NK_InvalidateGroundConnectionsNearPosition(actor, blockedFrom);
		return false;
	}

	actor->nkSmartTacticalGoal = search.Nodes[best].Pos;
	actor->nkSmartTacticalValid = true;
	actor->nkSmartDisconnectedRoam = true;
	actor->nkSmartRoamNextTarget = actor->Level->maptime + NK_ROAM_MIN_TICS +
		pr_nksmartchase(NK_ROAM_TIC_VARIATION);
	actor->nkSmartRouteSeed = int(NK_NewRouteSeed());
	return true;
}

static bool NK_ContinuePendingGroundSearch(AActor *actor, const DVector3 &goal,
	TArray<DVector3> &outPath, uint32_t routeSeed, bool weighted, bool &deferred)
{
	outPath.Clear();
	deferred = false;
	NK_PrepareGroundCache(actor);
	int now = actor->Level->maptime;
	FNKGroundProfile profile = NK_MakeGroundProfile(actor);
	uint32_t crowdGeneration = 0;
	bool crowdDetour = NK_CrowdDetourActive(actor, &crowdGeneration);
	bool stationarySurround = false;
	auto crowdState = NK_CrowdDetours.find(actor);
	if (crowdState != NK_CrowdDetours.end() &&
		crowdState->second.SurroundGoalActive && actor->nkSmartTacticalValid &&
		(actor->nkSmartTacticalGoal.XY() - goal.XY()).LengthSquared() <= 1.0)
	{
		stationarySurround = true;
	}
	auto inserted = NK_PendingGroundSearches.emplace(actor, FNKPendingGroundSearch{});
	FNKPendingGroundSearch &search = inserted.first->second;
	bool restart = !search.Initialized || !(search.Profile == profile) ||
		search.RouteSeed != routeSeed || search.Weighted != weighted ||
		search.CrowdDetour != crowdDetour || search.SurroundGoal != stationarySurround ||
		(crowdDetour && search.CrowdGeneration != crowdGeneration);
	if (restart)
	{
		DVector3 direct;
		if (!crowdDetour &&
			NK_Heuristic(actor->Pos(), goal, false) <= NK_GROUND_DIRECT_BUILD_RANGE &&
			NK_CanTraverse(actor, actor->Pos(), goal, false, direct))
		{
			outPath.Push(direct);
			NK_PendingGroundSearches.erase(inserted.first);
			return true;
		}
		NK_StartPendingGroundSearch(search, actor, goal, routeSeed, weighted);
	}
	search.LastTouched = now;

	// Once a sliced search has started, keep both ends of that search stable.
	// The live tactical anchor may continue to move with the player, but changing
	// the A* goal every tic makes the open set chase a moving destination and can
	// repeatedly throw away otherwise useful work. A completed snapshot route is
	// committed transactionally; if the tactical anchor moved far enough, the
	// normal target-moved check will begin the next snapshot search while this
	// route is being followed.
	const DVector3 searchGoal = search.Goal;

	bool emergencyReplacement = !actor->nkSmartTargetValid &&
		actor->nkSmartPathIndex < actor->nkSmartPath.Size();
	int searchTests = 0;
	int searchTestLimit = NK_GroundSearchTestSlice(actor);
	if (actor->nkSmartDisconnectedRoam)
	{
		searchTestLimit = std::max(3, searchTestLimit / 2);
	}
	else if (emergencyReplacement)
	{
		// A physically blocked committed route is no longer useful for motion.
		// Give its replacement search a short-lived priority boost so the actor
		// does not stand still for the normal sliced-search duration.
		searchTestLimit = std::max(searchTestLimit, NK_GROUND_EMERGENCY_CONNECTION_TESTS);
	}
	int expansionLimit = actor->nkSmartDisconnectedRoam
		? NK_GROUND_ROAM_NODE_EXPANSIONS_PER_SLICE
		: (emergencyReplacement ? NK_GROUND_EMERGENCY_NODE_EXPANSIONS
			: NK_GROUND_NODE_EXPANSIONS_PER_SLICE);
	int goalCheckLimit = emergencyReplacement
		? NK_GROUND_EMERGENCY_GOAL_CHECKS
		: NK_GROUND_GOAL_CHECKS_PER_SLICE;
	int expansions = 0;
	int goalChecks = 0;
	int goalNode = -1;
	bool nodeLimitReached = false;
	while (!search.Open.empty() && expansions < expansionLimit)
	{
		int currentIndex = search.Open.top().Node;
		search.Open.pop();
		FNKSearchNode &current = search.Nodes[currentIndex];
		if (current.Closed)
		{
			continue;
		}

		if (goalChecks < goalCheckLimit &&
			NK_Heuristic(current.Pos, searchGoal, false) <= 64.0 * 2.25)
		{
			goalChecks++;
			DVector3 reached;
			bool reachedGoal = search.SurroundGoal
				? NK_CanTraverseStationarySurround(actor, current.Pos, searchGoal, reached)
				: NK_CanTraverse(actor, current.Pos, searchGoal, false, reached);
			if (reachedGoal)
			{
				if (search.Nodes.size() >= NK_GROUND_MAX_NODES)
				{
					nodeLimitReached = true;
					break;
				}
				FNKGridKey goalKey = NK_GroundWorldKey(reached);
				goalNode = int(search.Nodes.size());
				double goalCost = current.Cost + NK_Heuristic(current.Pos, reached, true);
				if (search.CrowdDetour)
				{
					goalCost += NK_CrowdPointCost(search.CrowdSamples, reached.XY());
				}
				search.Nodes.push_back({ goalKey, reached, goalCost, currentIndex, false });
				break;
			}
		}

		for (int dy = -1; dy <= 1; ++dy)
		{
			for (int dx = -1; dx <= 1; ++dx)
			{
				if (dx == 0 && dy == 0)
				{
					continue;
				}

				DVector3 accepted;
				bool edgeDeferred = false;
				if (!NK_GetGroundConnection(actor, current.Key, current.Pos,
					dx, dy, accepted, edgeDeferred, searchTests, searchTestLimit,
					search.SurroundGoal))
				{
					if (edgeDeferred)
					{
						search.Open.push({ current.Cost + NK_Heuristic(current.Pos, searchGoal, false) *
							(weighted ? NK_WEIGHTED_HEURISTIC : 1.0), currentIndex });
						search.LastTouched = now;
						deferred = true;
						return false;
					}
					continue;
				}

				FNKGridKey key = NK_GroundWorldKey(accepted);
				FNKGroundAnchorKey anchorKey = NK_GroundAnchorKey(accepted);
				double moveCost = (accepted - current.Pos).Length() +
					std::abs(accepted.Z - current.Pos.Z) * 0.5;
				moveCost *= NK_RouteBiasFactor(key, routeSeed);
				if (search.CrowdDetour)
				{
					moveCost += NK_CrowdPointCost(search.CrowdSamples, accepted.XY());
				}
				double newCost = current.Cost + moveCost;
				auto found = search.NodeLookup.find(anchorKey);
				int nextIndex;
				if (found == search.NodeLookup.end())
				{
					if (search.Nodes.size() >= NK_GROUND_MAX_NODES)
					{
						nodeLimitReached = true;
						break;
					}
					nextIndex = int(search.Nodes.size());
					search.Nodes.push_back({ key, accepted, newCost, currentIndex, false });
					search.NodeLookup.emplace(anchorKey, nextIndex);
				}
				else
				{
					nextIndex = found->second;
					if (search.Nodes[nextIndex].Closed || newCost >= search.Nodes[nextIndex].Cost)
					{
						continue;
					}
					search.Nodes[nextIndex].Cost = newCost;
					search.Nodes[nextIndex].Parent = currentIndex;
					search.Nodes[nextIndex].Pos = accepted;
				}

				double score = newCost + NK_Heuristic(accepted, searchGoal, false) *
					(weighted ? NK_WEIGHTED_HEURISTIC : 1.0);
				search.Open.push({ score, nextIndex });
			}
			if (nodeLimitReached)
			{
				break;
			}
		}
		if (nodeLimitReached)
		{
			break;
		}
		current.Closed = true;
		expansions++;
	}

	if (goalNode >= 0)
	{
		std::vector<DVector3> reversePath;
		for (int index = goalNode; index > 0; index = search.Nodes[index].Parent)
		{
			reversePath.push_back(search.Nodes[index].Pos);
			if (search.Nodes[index].Parent < 0)
			{
				break;
			}
		}
		std::reverse(reversePath.begin(), reversePath.end());
		DVector3 blockedFrom;
		bool reconnected = false;
		const std::vector<FNKCrowdSample> *crowdSamples = search.CrowdDetour
			? &search.CrowdSamples : nullptr;
		bool routeValid = NK_ReconnectGroundRoute(actor, reversePath, outPath,
			blockedFrom, reconnected, crowdSamples, search.SurroundGoal);
		NK_PendingGroundSearches.erase(inserted.first);
		if (!routeValid)
		{
			outPath.Clear();
			if (reconnected)
			{
				// The actor successfully joined the completed search route, so a later
				// failed segment represents stale static connection data rather than
				// simple movement away from search.Start. Invalidate only that area.
				NK_InvalidateGroundConnectionsNearPosition(actor, blockedFrom);
			}
			// Keep the tactical anchor. The completed snapshot may simply be stale
			// relative to the actor's new position; retry from the current position on
			// the next tic instead of discarding the entire flank decision.
			deferred = true;
			return false;
		}
		return true;
	}

	if (nodeLimitReached || search.Open.empty())
	{
		// A failed SmartChase search must stay a pursuit failure. Do not turn it
		// into disconnected roaming, because that replaces pursuit with an unrelated
		// destination for several seconds. The caller may keep following its current
		// route and will request another tactical goal shortly.
		actor->nkSmartDisconnectedRoam = false;
		NK_PendingGroundSearches.erase(inserted.first);
		return false;
	}

	search.LastTouched = now;
	deferred = true;
	return false;
}


static void NK_StartPendingFlyingSearch(FNKPendingFlyingSearch &search,
	AActor *actor, const DVector3 &goal, uint32_t routeSeed, bool weighted)
{
	search.Start = actor->Pos();
	search.Goal = goal;
	search.CellSize = NK_CellSize(actor, true);
	search.VerticalCellSize = NK_VerticalCellSize(actor);
	search.RouteSeed = routeSeed;
	search.Weighted = weighted;
	search.Initialized = true;
	search.LastTouched = actor->Level->maptime;
	search.Nodes.clear();
	search.Nodes.reserve(NK_FLY_MAX_NODES);
	search.NodeLookup.clear();
	search.NodeLookup.reserve(NK_FLY_MAX_NODES * 2);
	search.Open = std::priority_queue<FNKOpenNode>();

	FNKGridKey startKey = NK_MakeKey(search.Start, search.Start,
		search.CellSize, search.VerticalCellSize, true);
	search.Nodes.push_back({ startKey, search.Start, 0.0, -1, false });
	search.NodeLookup.emplace(startKey, 0);
	search.Open.push({ NK_Heuristic(search.Start, search.Goal, true) *
		(weighted ? NK_WEIGHTED_HEURISTIC : 1.0), 0 });
}

static void NK_CompressHypnotizeFlyingRoute(AActor *actor, const DVector3 &start,
	const std::vector<DVector3> &rawPath, TArray<DVector3> &outPath)
{
	outPath.Clear();
	if (rawPath.empty())
	{
		return;
	}

	DVector3 anchor = start;
	size_t first = 0;
	int totalChecks = 0;
	while (first < rawPath.size())
	{
		size_t chosen = first;
		DVector3 chosenPosition = rawPath[first];
		size_t lastTest = std::min(rawPath.size() - 1,
			first + NK_HYPNOTIZE_FLY_SHORTCUT_LOOKAHEAD);
		for (size_t test = lastTest; test > first &&
			totalChecks < NK_HYPNOTIZE_FLY_SHORTCUT_TOTAL_CHECKS; --test)
		{
			totalChecks++;
			DVector3 smoothed;
			if (NK_CanTraverse(actor, anchor, rawPath[test], true, smoothed))
			{
				chosen = test;
				chosenPosition = smoothed;
				break;
			}
		}

		outPath.Push(chosenPosition);
		anchor = chosenPosition;
		first = chosen + 1;

		// Once the per-completion shortcut budget is consumed, keep the remaining
		// A* nodes verbatim. Every raw edge was already collision-tested during the
		// sliced search, so this avoids turning route completion into a new hitch.
		if (totalChecks >= NK_HYPNOTIZE_FLY_SHORTCUT_TOTAL_CHECKS)
		{
			for (; first < rawPath.size(); ++first)
			{
				outPath.Push(rawPath[first]);
			}
			break;
		}
	}
}

static bool NK_ReconnectHypnotizeFlyingRoute(AActor *actor,
	const std::vector<DVector3> &rawPath, TArray<DVector3> &outPath)
{
	outPath.Clear();
	if (rawPath.empty())
	{
		return false;
	}

	DVector3 current = actor->Pos();

	// A replacement flight search may finish while the actor is still consuming
	// its previous route. Rejoin near the closest forward node instead of sending
	// it back to the old search origin. Only a handful of local joins are tested
	// so route completion remains bounded.
	size_t nearest = 0;
	double nearestDistance = (rawPath[0] - current).LengthSquared();
	for (size_t i = 1; i < rawPath.size(); ++i)
	{
		double distance = (rawPath[i] - current).LengthSquared();
		if (distance < nearestDistance)
		{
			nearest = i;
			nearestDistance = distance;
		}
	}

	size_t candidate = std::min(rawPath.size() - 1,
		nearest + size_t(NK_HYPNOTIZE_FLY_RECONNECT_CHECKS - 1));
	int checks = 0;
	for (;;)
	{
		DVector3 accepted;
		checks++;
		if (NK_CanTraverse(actor, current, rawPath[candidate], true, accepted))
		{
			std::vector<DVector3> joined;
			joined.reserve(rawPath.size() - candidate);
			joined.push_back(accepted);
			for (size_t i = candidate + 1; i < rawPath.size(); ++i)
			{
				joined.push_back(rawPath[i]);
			}
			NK_CompressHypnotizeFlyingRoute(actor, current, joined, outPath);
			return outPath.Size() > 0;
		}

		if (candidate == 0 || checks >= NK_HYPNOTIZE_FLY_RECONNECT_CHECKS)
		{
			break;
		}
		candidate--;
	}
	return false;
}

static bool NK_BuildKitsuneSpiritRoamRouteFromFlyingSearch(AActor *actor,
	FNKPendingFlyingSearch &search, TArray<DVector3> &outPath)
{
	outPath.Clear();
	if (!actor || search.Nodes.size() < 2)
	{
		return false;
	}

	// The failed target search already mapped the reachable side of the current
	// TeleportGroup. Reuse that work instead of starting another search. Prefer a
	// reasonably distant closed node so the spirit visibly roams rather than
	// hovering in place, but keep the route local instead of selecting the most
	// distant node in a huge room.
	int best = -1;
	double bestScore = -1.0e30;
	for (size_t i = 1; i < search.Nodes.size(); ++i)
	{
		const FNKSearchNode &node = search.Nodes[i];
		if (!node.Closed)
		{
			continue;
		}
		double distance = (node.Pos - actor->Pos()).Length();
		if (distance < NK_KITSUNE_SPIRIT_ROAM_MIN_DISTANCE)
		{
			continue;
		}
		double score = distance <= NK_KITSUNE_SPIRIT_ROAM_PREFERRED_DISTANCE
			? distance
			: NK_KITSUNE_SPIRIT_ROAM_PREFERRED_DISTANCE -
				(distance - NK_KITSUNE_SPIRIT_ROAM_PREFERRED_DISTANCE) * 0.25;
		if (score > bestScore)
		{
			best = int(i);
			bestScore = score;
		}
	}
	if (best < 0)
	{
		return false;
	}

	std::vector<DVector3> reversePath;
	for (int index = best; index > 0; index = search.Nodes[index].Parent)
	{
		reversePath.push_back(search.Nodes[index].Pos);
		if (search.Nodes[index].Parent < 0)
		{
			break;
		}
	}
	std::reverse(reversePath.begin(), reversePath.end());
	if (!NK_ReconnectHypnotizeFlyingRoute(actor, reversePath, outPath) || outPath.Size() == 0)
	{
		outPath.Clear();
		return false;
	}

	FNKKitsuneSpiritRoamState &roam = NK_KitsuneSpiritRoams[actor];
	roam.Origin = actor->Pos();
	roam.ForwardRoute.clear();
	roam.ForwardRoute.reserve(outPath.Size());
	for (unsigned i = 0; i < outPath.Size(); ++i)
	{
		roam.ForwardRoute.push_back(outPath[i]);
	}
	roam.Reverse = false;
	roam.LastTouched = actor->Level->maptime;

	actor->nkSmartDisconnectedRoam = true;
	actor->nkSmartTacticalGoal = outPath[outPath.Size() - 1];
	actor->nkSmartTacticalValid = true;
	actor->nkSmartRoamNextTarget = actor->Level->maptime +
		NK_KITSUNE_SPIRIT_DISCONNECTED_RETRY_TICS;
	return true;
}

static bool NK_SetKitsuneSpiritRoamLeg(AActor *actor, bool reverse)
{
	auto found = NK_KitsuneSpiritRoams.find(actor);
	if (!actor || found == NK_KitsuneSpiritRoams.end())
	{
		return false;
	}
	FNKKitsuneSpiritRoamState &roam = found->second;
	roam.LastTouched = actor->Level->maptime;
	if (roam.ForwardRoute.empty())
	{
		return false;
	}

	TArray<DVector3> path;
	if (reverse)
	{
		// Current position is already the forward endpoint. Walk the known-safe
		// route backward and finish at the exact point where disconnected roaming
		// began. No A* is run here.
		if (roam.ForwardRoute.size() > 1)
		{
			for (size_t i = roam.ForwardRoute.size() - 1; i-- > 0;)
			{
				path.Push(roam.ForwardRoute[i]);
			}
		}
		path.Push(roam.Origin);
	}
	else
	{
		for (const DVector3 &point : roam.ForwardRoute)
		{
			path.Push(point);
		}
	}
	if (path.Size() == 0)
	{
		return false;
	}

	actor->nkSmartPath = std::move(path);
	actor->nkSmartPathIndex = 0;
	actor->nkSmartPathTarget = actor->nkSmartPath[actor->nkSmartPath.Size() - 1];
	actor->nkSmartTargetValid = true;
	actor->nkSmartTacticalGoal = actor->nkSmartPathTarget;
	actor->nkSmartTacticalValid = true;
	actor->nkSmartNextRepath = actor->Level->maptime +
		NK_KITSUNE_SPIRIT_DISCONNECTED_RETRY_TICS;
	roam.Reverse = reverse;
	return true;
}

static void NK_BeginKitsuneSpiritIdleRoam(AActor *actor)
{
	if (!actor || !actor->Level)
	{
		return;
	}
	actor->nkSmartDisconnectedRoam = true;
	actor->nkSmartPath.Clear();
	actor->nkSmartPathIndex = 0;
	actor->nkSmartTargetValid = false;
	actor->nkSmartTacticalValid = false;
	actor->nkSmartRoamNextTarget = actor->Level->maptime +
		NK_KITSUNE_SPIRIT_DISCONNECTED_RETRY_TICS;
	actor->nkSmartNextRepath = actor->nkSmartRoamNextTarget;
	actor->Vel.X = actor->Vel.Y = actor->Vel.Z = 0;
	actor->flags |= MF_INFLOAT;
}

static bool NK_ContinuePendingFlyingSearch(AActor *actor, const DVector3 &goal,
	TArray<DVector3> &outPath, uint32_t routeSeed, bool weighted, bool &deferred)
{
	outPath.Clear();
	deferred = false;
	NK_PrepareGroundCache(actor);
	int now = actor->Level->maptime;
	double cellSize = NK_CellSize(actor, true);
	double verticalCellSize = NK_VerticalCellSize(actor);

	auto inserted = NK_PendingFlyingSearches.emplace(actor, FNKPendingFlyingSearch{});
	FNKPendingFlyingSearch &search = inserted.first->second;
	double targetWarpDistance = actor->nkSmartNavigationMode == 3
		? NK_KITSUNE_SPIRIT_RETRY_WARP_DISTANCE
		: NK_TARGET_WARP_DISTANCE;
	bool targetWarped = search.Initialized &&
		(goal - search.Goal).Length() > targetWarpDistance;
	bool restart = !search.Initialized || search.RouteSeed != routeSeed ||
		search.Weighted != weighted ||
		std::abs(search.CellSize - cellSize) > 0.001 ||
		std::abs(search.VerticalCellSize - verticalCellSize) > 0.001 || targetWarped;

	if (restart)
	{
		// A short unobstructed flight is cheaper than starting A*. Do not perform
		// this full-cylinder direct probe across a long teleport, because that was
		// itself capable of producing hundreds of collision tests in one tic.
		double directDistance = (goal - actor->Pos()).Length();
		if (directDistance <= 512.0)
		{
			DVector3 direct;
			if (NK_CanTraverse(actor, actor->Pos(), goal, true, direct))
			{
				outPath.Push(direct);
				NK_PendingFlyingSearches.erase(inserted.first);
				return true;
			}
		}
		NK_StartPendingFlyingSearch(search, actor, goal, routeSeed, weighted);
	}
	search.LastTouched = now;

	const DVector3 searchGoal = search.Goal;
	bool urgent = actor->nkSmartPathIndex >= actor->nkSmartPath.Size();
	int expansionLimit = actor->nkSmartNavigationMode == 3
		? NK_KITSUNE_SPIRIT_FLY_EXPANSIONS_PER_SLICE
		: (urgent ? NK_HYPNOTIZE_FLY_URGENT_EXPANSIONS_PER_SLICE
			: NK_HYPNOTIZE_FLY_EXPANSIONS_PER_SLICE);
	int flyNodeLimit = actor->nkSmartNavigationMode == 3
		? NK_KITSUNE_SPIRIT_FLY_MAX_NODES
		: NK_FLY_MAX_NODES;
	int expansions = 0;
	int goalNode = -1;
	bool nodeLimitReached = false;

	while (!search.Open.empty() && expansions < expansionLimit)
	{
		int currentIndex = search.Open.top().Node;
		search.Open.pop();
		FNKSearchNode &current = search.Nodes[currentIndex];
		if (current.Closed)
		{
			continue;
		}
		current.Closed = true;

		if (NK_Heuristic(current.Pos, searchGoal, true) <=
			std::max(search.CellSize, search.VerticalCellSize) * 2.25)
		{
			DVector3 reached;
			if (NK_CanTraverse(actor, current.Pos, searchGoal, true, reached))
			{
				if (search.Nodes.size() >= size_t(flyNodeLimit))
				{
					nodeLimitReached = true;
					break;
				}
				FNKGridKey goalKey = NK_MakeKey(search.Start, reached,
					search.CellSize, search.VerticalCellSize, true);
				goalNode = int(search.Nodes.size());
				search.Nodes.push_back({ goalKey, reached,
					current.Cost + NK_Heuristic(current.Pos, reached, true),
					currentIndex, false });
				break;
			}
		}

		for (int dz = -1; dz <= 1 && !nodeLimitReached; ++dz)
		{
			for (int dy = -1; dy <= 1 && !nodeLimitReached; ++dy)
			{
				for (int dx = -1; dx <= 1; ++dx)
				{
					if (dx == 0 && dy == 0 && dz == 0)
					{
						continue;
					}

					FNKGridKey key{ current.Key.X + dx, current.Key.Y + dy,
						current.Key.Z + dz };
					DVector3 candidate = NK_GridPosition(search.Start, key,
						search.CellSize, search.VerticalCellSize, true, current.Pos.Z);
					DVector3 accepted;
					if (!NK_CanTraverse(actor, current.Pos, candidate, true, accepted))
					{
						continue;
					}

					key = NK_MakeKey(search.Start, accepted,
						search.CellSize, search.VerticalCellSize, true);
					double moveCost = (accepted - current.Pos).Length();
					moveCost *= NK_RouteBiasFactor(key, routeSeed);
					double newCost = current.Cost + moveCost;
					auto found = search.NodeLookup.find(key);
					int nextIndex;

					if (found == search.NodeLookup.end())
					{
						if (search.Nodes.size() >= size_t(flyNodeLimit))
						{
							nodeLimitReached = true;
							break;
						}
						nextIndex = int(search.Nodes.size());
						search.Nodes.push_back({ key, accepted, newCost,
							currentIndex, false });
						search.NodeLookup.emplace(key, nextIndex);
					}
					else
					{
						nextIndex = found->second;
						if (search.Nodes[nextIndex].Closed ||
							newCost >= search.Nodes[nextIndex].Cost)
						{
							continue;
						}
						search.Nodes[nextIndex].Cost = newCost;
						search.Nodes[nextIndex].Parent = currentIndex;
						search.Nodes[nextIndex].Pos = accepted;
					}

					double score = newCost + NK_Heuristic(accepted, searchGoal, true) *
						(weighted ? NK_WEIGHTED_HEURISTIC : 1.0);
					search.Open.push({ score, nextIndex });
				}
			}
		}
		expansions++;
	}

	if (goalNode >= 0)
	{
		std::vector<DVector3> reversePath;
		for (int index = goalNode; index > 0; index = search.Nodes[index].Parent)
		{
			reversePath.push_back(search.Nodes[index].Pos);
			if (search.Nodes[index].Parent < 0)
			{
				break;
			}
		}
		std::reverse(reversePath.begin(), reversePath.end());
		bool routeValid = NK_ReconnectHypnotizeFlyingRoute(actor, reversePath, outPath);
		NK_PendingFlyingSearches.erase(inserted.first);
		if (!routeValid)
		{
			deferred = true;
			return false;
		}
		return true;
	}

	if (nodeLimitReached || search.Open.empty())
	{
		if (actor->nkSmartNavigationMode == 3)
		{
			bool haveRoamRoute = NK_BuildKitsuneSpiritRoamRouteFromFlyingSearch(
				actor, search, outPath);
			NK_PendingFlyingSearches.erase(inserted.first);
			if (nk_smartchase_debug >= 1)
			{
				Printf("KitsuneSpiritMove: player landing unreachable; %s for %d tics\n",
					haveRoamRoute ? "roaming reachable local route" : "holding local area",
					NK_KITSUNE_SPIRIT_DISCONNECTED_RETRY_TICS);
			}
			if (haveRoamRoute)
			{
				return true;
			}
			NK_BeginKitsuneSpiritIdleRoam(actor);
			return false;
		}
		NK_PendingFlyingSearches.erase(inserted.first);
		return false;
	}

	search.LastTouched = now;
	deferred = true;
	return false;
}

static bool NK_BuildPath(AActor *actor, const DVector3 &goal, bool flying,
	TArray<DVector3> &outPath, uint32_t routeSeed, bool weighted,
	bool useSharedGround, bool &deferred)
{
	outPath.Clear();
	deferred = false;
	DVector3 start = actor->Pos();
	if (flying && (actor->nkSmartNavigationMode == 1 ||
		actor->nkSmartNavigationMode == 3))
	{
		return NK_ContinuePendingFlyingSearch(actor, goal, outPath,
			routeSeed, weighted, deferred);
	}
	bool sharedGround = useSharedGround && !flying;
	if (sharedGround)
	{
		return NK_ContinuePendingGroundSearch(actor, goal, outPath,
			routeSeed, weighted, deferred);
	}

	DVector3 direct;
	if (NK_CanTraverse(actor, start, goal, flying, direct))
	{
		outPath.Push(direct);
		return true;
	}

	double cellSize = NK_CellSize(actor, flying);
	double verticalCellSize = flying ? NK_VerticalCellSize(actor) : cellSize;
	DVector3 gridOrigin = start;
	int maxNodes = flying ? NK_FLY_MAX_NODES : NK_GROUND_MAX_NODES;
	std::vector<FNKSearchNode> nodes;
	nodes.reserve(maxNodes);
	std::unordered_map<FNKGridKey, int, FNKGridKeyHash> nodeLookup;
	nodeLookup.reserve(maxNodes * 2);
	std::priority_queue<FNKOpenNode> open;

	FNKGridKey startKey = NK_MakeKey(gridOrigin, start,
		cellSize, verticalCellSize, flying);
	nodes.push_back({ startKey, start, 0.0, -1, false });
	nodeLookup.emplace(startKey, 0);
	open.push({ NK_Heuristic(start, goal, flying) *
		(weighted ? NK_WEIGHTED_HEURISTIC : 1.0), 0 });

	int goalNode = -1;
	while (!open.empty() && int(nodes.size()) < maxNodes)
	{
		int currentIndex = open.top().Node;
		open.pop();
		FNKSearchNode &current = nodes[currentIndex];
		if (current.Closed)
		{
			continue;
		}
		current.Closed = true;

		if (NK_Heuristic(current.Pos, goal, flying) <= std::max(cellSize, verticalCellSize) * 2.25)
		{
			DVector3 reached;
			if (NK_CanTraverse(actor, current.Pos, goal, flying, reached))
			{
				FNKGridKey goalKey = NK_MakeKey(gridOrigin, reached,
					cellSize, verticalCellSize, flying);
				goalNode = int(nodes.size());
				nodes.push_back({ goalKey, reached,
					current.Cost + NK_Heuristic(current.Pos, reached, true), currentIndex, false });
				break;
			}
		}

		for (int dz = flying ? -1 : 0; dz <= (flying ? 1 : 0); ++dz)
		{
			for (int dy = -1; dy <= 1; ++dy)
			{
				for (int dx = -1; dx <= 1; ++dx)
				{
					if (dx == 0 && dy == 0 && dz == 0)
					{
						continue;
					}

					FNKGridKey key{ current.Key.X + dx, current.Key.Y + dy,
						flying ? current.Key.Z + dz : current.Key.Z };
					DVector3 candidate = NK_GridPosition(gridOrigin, key,
						cellSize, verticalCellSize, flying, current.Pos.Z);
					DVector3 accepted;
					if (!NK_CanTraverse(actor, current.Pos,
						candidate, flying, accepted))
					{
						continue;
					}

					key = NK_MakeKey(gridOrigin, accepted,
						cellSize, verticalCellSize, flying);
					double moveCost = (accepted - current.Pos).Length();
					if (!flying)
					{
						moveCost += std::abs(accepted.Z - current.Pos.Z) * 0.5;
					}
					moveCost *= NK_RouteBiasFactor(key, routeSeed);
					double newCost = current.Cost + moveCost;
					auto found = nodeLookup.find(key);
					int nextIndex;

					if (found == nodeLookup.end())
					{
						nextIndex = int(nodes.size());
						nodes.push_back({ key, accepted, newCost, currentIndex, false });
						nodeLookup.emplace(key, nextIndex);
					}
					else
					{
						nextIndex = found->second;
						if (nodes[nextIndex].Closed || newCost >= nodes[nextIndex].Cost)
						{
							continue;
						}
						nodes[nextIndex].Cost = newCost;
						nodes[nextIndex].Parent = currentIndex;
						nodes[nextIndex].Pos = accepted;
					}

					double score = newCost + NK_Heuristic(accepted, goal, flying) *
						(weighted ? NK_WEIGHTED_HEURISTIC : 1.0);
					open.push({ score, nextIndex });
				}
			}
		}
	}

	if (goalNode < 0)
	{
		return false;
	}

	std::vector<DVector3> reversePath;
	for (int index = goalNode; index > 0; index = nodes[index].Parent)
	{
		reversePath.push_back(nodes[index].Pos);
		if (nodes[index].Parent < 0)
		{
			break;
		}
	}
	std::reverse(reversePath.begin(), reversePath.end());

	// Preserve the existing any-angle smoothing for A_HypnotizeChase and
	// flying actors. Keep the farthest waypoint the full collision cylinder
	// can reach directly.
	DVector3 anchor = start;
	size_t first = 0;
	while (first < reversePath.size())
	{
		size_t chosen = first;
		DVector3 chosenPosition = reversePath[first];
		for (size_t test = reversePath.size(); test-- > first;)
		{
			DVector3 smoothed;
			if (NK_CanTraverse(actor, anchor, reversePath[test], flying, smoothed))
			{
				chosen = test;
				chosenPosition = smoothed;
				break;
			}
		}
		outPath.Push(chosenPosition);
		anchor = chosenPosition;
		first = chosen + 1;
	}

	return outPath.Size() > 0;
}

static DVector3 NK_SmartTargetPosition(AActor *actor, bool flying)
{
	DVector3 result = actor->target->PosRelative(actor);
	if (flying)
	{
		result.Z += actor->target->Height * 0.5 - actor->Height * 0.5;
	}
	return result;
}

static DVector3 NK_PredictTargetPosition(AActor *actor, bool flying)
{
	DVector3 targetPos = NK_SmartTargetPosition(actor, flying);
	DVector3 delta = targetPos - actor->Pos();
	if (!flying)
	{
		delta.Z = 0.0;
	}

	double distance = delta.Length();
	double predictionTics = std::clamp(distance / 20.0, 0.0, NK_MAX_PREDICTION_TICS);
	DVector3 prediction = actor->target->Vel * predictionTics;
	if (!flying)
	{
		prediction.Z = 0.0;
	}
	else
	{
		prediction.Z = std::clamp(prediction.Z, -96.0, 96.0);
	}

	double predictionLength = flying ? prediction.Length() : prediction.XY().Length();
	if (predictionLength > NK_MAX_PREDICTION_DISTANCE)
	{
		prediction *= NK_MAX_PREDICTION_DISTANCE / predictionLength;
	}
	return targetPos + prediction;
}

static bool NK_ValidateGoalPosition(AActor *actor, const DVector3 &candidate,
	bool flying, DVector3 &accepted)
{
	// Query the candidate as a destination volume. The full route is verified
	// later by NK_BuildPath.
	return NK_CheckMoveAt(actor, candidate, candidate, flying, accepted);
}

static bool NK_IsPursuitAnchoredGoal(AActor *actor, const DVector3 &goal,
	const DVector3 &targetPos)
{
	DVector2 toTarget = targetPos.XY() - actor->Pos().XY();
	DVector2 toGoal = goal.XY() - actor->Pos().XY();
	if (toTarget.LengthSquared() <= 0.0001 || toGoal.LengthSquared() <= 0.0001)
	{
		return true;
	}

	// Tactical offsets may move around the live target, but they must stay in
	// the forward pursuit half-plane. Once the actor has passed an old anchor,
	// do not turn around and patrol back to it merely because its timer remains.
	return toTarget.X * toGoal.X + toTarget.Y * toGoal.Y > 0.0;
}

static FNKCrowdDetourState *NK_GetStationaryCrowdSurroundState(AActor *actor,
	bool flying)
{
	if (!actor || !actor->target || flying ||
		actor->target->Vel.XY().LengthSquared() > NK_STATIONARY_SURROUND_SPEED_SQ ||
		!NK_CrowdDetourActive(actor))
	{
		return nullptr;
	}

	auto found = NK_CrowdDetours.find(actor);
	if (found == NK_CrowdDetours.end())
	{
		return nullptr;
	}
	return &found->second;
}

static bool NK_IsStationarySurroundGoalRelevant(AActor *actor,
	const DVector3 &goal, const DVector3 &targetPos, double goalReach)
{
	if (!actor)
	{
		return false;
	}

	double targetRadius = (goal.XY() - targetPos.XY()).Length();
	double actorDistance = (goal.XY() - actor->Pos().XY()).Length();
	return targetRadius >= NK_STATIONARY_SURROUND_MIN_RADIUS * 0.55 &&
		targetRadius <= NK_STATIONARY_SURROUND_MAX_RADIUS + 64.0 &&
		actorDistance > goalReach;
}

static bool NK_SelectStationaryCrowdSurroundGoal(AActor *actor,
	FNKCrowdDetourState &state, const DVector3 &targetPos, double spread,
	DVector3 &goal)
{
	DVector2 blockedSide = state.LastBlockerCenter - targetPos.XY();
	if (blockedSide.LengthSquared() <= 16.0)
	{
		blockedSide = actor->Pos().XY() - targetPos.XY();
	}
	if (blockedSide.LengthSquared() <= 0.0001)
	{
		blockedSide = DVector2(-actor->target->Angles.Yaw.Cos(),
			-actor->target->Angles.Yaw.Sin());
	}

	DAngle blockedApproach = blockedSide.Angle();
	double radius = std::clamp(spread * 0.70,
		NK_STATIONARY_SURROUND_MIN_RADIUS, NK_STATIONARY_SURROUND_MAX_RADIUS);

	// Spread different monsters across the two sides first, then progressively
	// farther around the target. The final 180-degree candidate approaches from
	// the opposite side of the congested lane. This changes only the destination;
	// the stable baseline A* geometry and handoff rules remain untouched.
	int sideSign = pr_nksmartchase(2) == 0 ? -1 : 1;
	for (int pass = 0; pass < NK_STATIONARY_SURROUND_SECTORS; ++pass)
	{
		int index = pass;
		double offset = NK_StationarySurroundAngles[index];
		if (index < 4)
		{
			offset *= sideSign;
		}

		DAngle candidateAngle = blockedApproach + DAngle::fromDeg(offset);
		DVector3 candidate = targetPos;
		candidate.X += candidateAngle.Cos() * radius;
		candidate.Y += candidateAngle.Sin() * radius;

		DVector3 accepted;
		if (!NK_ValidateGoalPosition(actor, candidate, false, accepted))
		{
			continue;
		}

		// Do not place the surround anchor back inside the congestion hotspot.
		double hotspotClearance = actor->radius + 48.0;
		if ((accepted.XY() - state.LastBlockerCenter).Length() < hotspotClearance)
		{
			continue;
		}

		goal = accepted;
		if (nk_smartchase_debug >= 3)
		{
			Printf("SmartChase stationary surround goal: actor=%p offset=%.0f radius=%.1f\n",
				actor, offset, radius);
		}
		return true;
	}

	return false;
}

static DVector3 NK_SelectTacticalGoal(AActor *actor, bool flying, int now)
{
	DVector3 targetPos = NK_SmartTargetPosition(actor, flying);
	double goalReach = std::max(64.0, actor->radius * 2.0);
	FNKCrowdDetourState *surroundState =
		NK_GetStationaryCrowdSurroundState(actor, flying);
	bool useStationarySurround = surroundState != nullptr;

	if (!useStationarySurround)
	{
		// A surround anchor exists only while the target is stationary and an actual
		// crowd detour is active. Once the player starts moving, immediately return
		// goal selection to the normal prediction/tactical system.
		auto found = NK_CrowdDetours.find(actor);
		if (found != NK_CrowdDetours.end())
		{
			found->second.SurroundGoalActive = false;
			found->second.SurroundUntil = -1;
		}
	}

	if (actor->nkSmartTacticalValid)
	{
		double goalDistance = flying
			? (actor->nkSmartTacticalGoal - actor->Pos()).Length()
			: (actor->nkSmartTacticalGoal.XY() - actor->Pos().XY()).Length();
		bool pendingGroundSearch = !flying &&
			NK_PendingGroundSearches.find(actor) != NK_PendingGroundSearches.end();

		if (useStationarySurround && surroundState->SurroundGoalActive)
		{
			if ((pendingGroundSearch || now < surroundState->SurroundUntil) &&
				NK_IsStationarySurroundGoalRelevant(actor, actor->nkSmartTacticalGoal,
					targetPos, goalReach))
			{
				return actor->nkSmartTacticalGoal;
			}
			surroundState->SurroundGoalActive = false;
		}
		else if (!useStationarySurround &&
			NK_IsPursuitAnchoredGoal(actor, actor->nkSmartTacticalGoal, targetPos) &&
			(pendingGroundSearch || now < actor->nkSmartNextTacticalUpdate) &&
			goalDistance > goalReach)
		{
			return actor->nkSmartTacticalGoal;
		}
	}

	int oldSector = actor->nkSmartTacticalSector;
	int sector = pr_nksmartchase(NK_TACTICAL_SECTORS);
	if (actor->nkSmartTacticalValid && sector == oldSector)
	{
		sector = (sector + 1 + pr_nksmartchase(NK_TACTICAL_SECTORS - 1)) %
			NK_TACTICAL_SECTORS;
	}
	actor->nkSmartTacticalSector = sector;
	actor->nkSmartTacticalSpread = NK_TACTICAL_MIN_SPREAD +
		pr_nksmartchase(NK_TACTICAL_SPREAD_VARIATION);
	actor->nkSmartNextTacticalUpdate = now + NK_TACTICAL_MIN_TICS +
		pr_nksmartchase(NK_TACTICAL_TIC_VARIATION);
	actor->nkSmartRouteSeed = int(NK_NewRouteSeed());

	if (useStationarySurround)
	{
		DVector3 surroundGoal;
		if (NK_SelectStationaryCrowdSurroundGoal(actor, *surroundState, targetPos,
			actor->nkSmartTacticalSpread, surroundGoal))
		{
			actor->nkSmartTacticalGoal = surroundGoal;
			actor->nkSmartTacticalValid = true;
			surroundState->SurroundGoalActive = true;
			surroundState->SurroundUntil = actor->nkSmartNextTacticalUpdate;
			return surroundGoal;
		}

		// If none of the side/rear landing points is physically valid, preserve the
		// stable baseline behavior instead of forcing an unsafe goal near geometry.
		surroundState->SurroundGoalActive = false;
		surroundState->SurroundUntil = -1;
	}

	DVector3 predicted = NK_PredictTargetPosition(actor, flying);
	DVector2 targetVelocity = actor->target->Vel.XY();
	DAngle forwardAngle = targetVelocity.LengthSquared() > 0.25
		? targetVelocity.Angle()
		: actor->target->Angles.Yaw;

	double distance = (targetPos.XY() - actor->Pos().XY()).Length();
	double tacticalStrength = 0.0;
	if (distance > NK_DIRECT_PURSUIT_RANGE)
	{
		// Fade tactical spread in continuously from direct-pursuit range instead
		// of jumping immediately to 55 percent just outside 128 map units.
		tacticalStrength = std::clamp(
			(distance - NK_DIRECT_PURSUIT_RANGE) /
			(NK_FULL_TACTICAL_RANGE - NK_DIRECT_PURSUIT_RANGE), 0.0, 1.0);
	}
	double spread = actor->nkSmartTacticalSpread * tacticalStrength;

	DVector3 best = predicted;
	bool foundGoal = false;
	for (int pass = -1; pass < NK_TACTICAL_SECTORS; ++pass)
	{
		int sector;
		if (pass < 0)
		{
			sector = actor->nkSmartTacticalSector;
		}
		else
		{
			sector = pass;
			if (sector == actor->nkSmartTacticalSector)
			{
				continue;
			}
		}

		DAngle candidateAngle = forwardAngle + DAngle::fromDeg(NK_TacticalAngles[sector]);
		DVector3 candidate = predicted;
		candidate.X += candidateAngle.Cos() * spread;
		candidate.Y += candidateAngle.Sin() * spread;

		DVector3 accepted;
		if (!NK_ValidateGoalPosition(actor, candidate, flying, accepted) ||
			!NK_IsPursuitAnchoredGoal(actor, accepted, targetPos))
		{
			continue;
		}

		best = accepted;
		foundGoal = true;
		break;
	}

	if (!foundGoal)
	{
		DVector3 accepted;
		if ((NK_ValidateGoalPosition(actor, predicted, flying, accepted) &&
			NK_IsPursuitAnchoredGoal(actor, accepted, targetPos)) ||
			NK_ValidateGoalPosition(actor, targetPos, flying, accepted))
		{
			best = accepted;
		}
		else
		{
			best = targetPos;
		}
	}

	actor->nkSmartTacticalGoal = best;
	actor->nkSmartTacticalValid = true;
	// Do not discard a usable route before the replacement A* route exists.
	// NK_FollowSmartPath notices the changed target and builds the replacement
	// transactionally while the actor continues consuming its current path.
	return best;
}

static int NK_DirectionTo(const DVector2 &delta)
{
	if (delta.LengthSquared() <= 0.0001)
	{
		return DI_NODIR;
	}
	double degrees = delta.Angle().Normalized360().Degrees();
	return int(std::floor((degrees + 22.5) / 45.0)) & 7;
}

static int NK_DirectionStepDistance(int first, int second)
{
	if (first < DI_EAST || first >= DI_NODIR ||
		second < DI_EAST || second >= DI_NODIR)
	{
		return 8;
	}
	int difference = std::abs(first - second);
	return std::min(difference, 8 - difference);
}

static int NK_StabilizeGroundSteeringDirection(AActor *actor, const DVector2 &delta,
	int desiredDirection)
{
	if (!actor || !actor->Level || desiredDirection < DI_EAST ||
		desiredDirection >= DI_NODIR)
	{
		return desiredDirection;
	}

	FNKGroundSteeringState &state = NK_GroundSteeringStates[actor];
	state.LastTouched = actor->Level->maptime;
	double distance = delta.Length();
	if (distance < std::max(NK_GROUND_STEERING_MIN_DISTANCE, actor->radius))
	{
		state.Direction = desiredDirection;
		return desiredDirection;
	}

	int heldDirection = state.Direction;
	if (heldDirection >= DI_EAST && heldDirection < DI_NODIR &&
		heldDirection != desiredDirection &&
		NK_DirectionStepDistance(heldDirection, desiredDirection) == 1)
	{
		double alignment = (delta.X * xspeed[heldDirection] +
			delta.Y * yspeed[heldDirection]) / distance;
		if (alignment >= NK_GROUND_STEERING_HYSTERESIS_DOT)
		{
			return heldDirection;
		}
	}

	state.Direction = desiredDirection;
	return desiredDirection;
}

static void NK_RecordGroundSteeringDirection(AActor *actor, int direction)
{
	if (!actor || !actor->Level || direction < DI_EAST || direction >= DI_NODIR)
	{
		return;
	}
	FNKGroundSteeringState &state = NK_GroundSteeringStates[actor];
	state.Direction = direction;
	state.LastTouched = actor->Level->maptime;
}

static int NK_StabilizeHypnotizeSteeringDirection(AActor *actor,
	const DVector2 &delta, int desiredDirection)
{
	if (!actor || !actor->Level || desiredDirection < DI_EAST ||
		desiredDirection >= DI_NODIR)
	{
		return desiredDirection;
	}

	FNKHypnotizeFlightSteeringState &state =
		NK_HypnotizeFlightSteeringStates[actor];
	state.LastTouched = actor->Level->maptime;
	double distance = delta.Length();
	if (distance < NK_HYPNOTIZE_STEERING_MIN_DISTANCE)
	{
		state.Direction = desiredDirection;
		return desiredDirection;
	}

	int heldDirection = state.Direction;
	if (heldDirection >= DI_EAST && heldDirection < DI_NODIR &&
		heldDirection != desiredDirection &&
		NK_DirectionStepDistance(heldDirection, desiredDirection) == 1)
	{
		double alignment = (delta.X * xspeed[heldDirection] +
			delta.Y * yspeed[heldDirection]) / distance;
		if (alignment >= NK_HYPNOTIZE_STEERING_HYSTERESIS_DOT)
		{
			return heldDirection;
		}
	}

	state.Direction = desiredDirection;
	return desiredDirection;
}

static void NK_RecordHypnotizeSteeringDirection(AActor *actor, int direction)
{
	if (!actor || !actor->Level || direction < DI_EAST || direction >= DI_NODIR)
	{
		return;
	}
	FNKHypnotizeFlightSteeringState &state =
		NK_HypnotizeFlightSteeringStates[actor];
	state.Direction = direction;
	state.LastTouched = actor->Level->maptime;
}

static double NK_HypnotizeVerticalSpeed(const AActor *actor)
{
	return std::max(actor->FloatSpeed, std::max(1.0, actor->Speed * 0.5));
}

static void NK_UpdateHypnotizeVerticalVelocity(AActor *actor, double zdelta)
{
	if (!actor)
	{
		return;
	}

	double maxSpeed = NK_HypnotizeVerticalSpeed(actor);
	double targetSpeed = std::clamp(zdelta * NK_HYPNOTIZE_VERTICAL_RESPONSE,
		-maxSpeed, maxSpeed);
	if (std::abs(zdelta) < 0.5)
	{
		targetSpeed = 0.0;
	}

	double maxChange = std::max(NK_HYPNOTIZE_VERTICAL_MIN_ACCEL,
		maxSpeed * NK_HYPNOTIZE_VERTICAL_ACCEL_RATIO);
	double change = std::clamp(targetSpeed - actor->Vel.Z, -maxChange, maxChange);
	actor->Vel.Z += change;
	if (targetSpeed == 0.0 && std::abs(actor->Vel.Z) <= maxChange)
	{
		actor->Vel.Z = 0.0;
	}
	actor->flags |= MF_INFLOAT;
}

static void NK_ClearHypnotizeVerticalSteer(AActor *actor)
{
	auto found = NK_HypnotizeFlightSteeringStates.find(actor);
	if (found == NK_HypnotizeFlightSteeringStates.end())
	{
		return;
	}
	found->second.VerticalSign = 0;
	found->second.VerticalUntil = -1;
	found->second.ClearanceSign = 0;
	found->second.ClearanceUntil = -1;
}

static bool NK_ApplyKitsuneSpiritClearanceVelocity(AActor *actor, double waypointZDelta)
{
	if (!actor || !actor->Level || actor->nkSmartNavigationMode != 3)
	{
		return false;
	}

	auto found = NK_HypnotizeFlightSteeringStates.find(actor);
	if (found == NK_HypnotizeFlightSteeringStates.end())
	{
		return false;
	}

	FNKHypnotizeFlightSteeringState &state = found->second;
	int now = actor->Level->maptime;
	if (state.ClearanceSign == 0 || now > state.ClearanceUntil)
	{
		state.ClearanceSign = 0;
		state.ClearanceUntil = -1;
		return false;
	}

	double clearanceDelta = std::max(NK_KITSUNE_SPIRIT_CLEARANCE_MIN_DELTA,
		std::max(actor->FloatSpeed * 4.0, actor->Height * 0.5));
	if ((waypointZDelta > 0.0 && state.ClearanceSign > 0) ||
		(waypointZDelta < 0.0 && state.ClearanceSign < 0))
	{
		clearanceDelta = std::max(clearanceDelta, std::abs(waypointZDelta));
	}

	NK_UpdateHypnotizeVerticalVelocity(actor, clearanceDelta * state.ClearanceSign);
	actor->flags |= MF_INFLOAT;
	return true;
}

static bool NK_TryHypnotizeDirectVerticalApproach(AActor *actor,
	const DVector3 &liveTarget)
{
	if (!actor || !actor->Level)
	{
		return false;
	}

	FNKHypnotizeFlightSteeringState &state =
		NK_HypnotizeFlightSteeringStates[actor];
	state.LastTouched = actor->Level->maptime;

	DVector3 delta = liveTarget - actor->Pos();
	double xyDistance = delta.XY().Length();
	double zDistance = std::abs(delta.Z);
	int verticalSign = delta.Z > 0.0 ? 1 : (delta.Z < 0.0 ? -1 : 0);

	double enterXYLimit = std::max(NK_HYPNOTIZE_DIRECT_VERTICAL_MIN_XY,
		actor->radius * NK_HYPNOTIZE_DIRECT_VERTICAL_RADIUS_SCALE);
	double exitXYLimit = std::max(NK_HYPNOTIZE_DIRECT_VERTICAL_EXIT_MIN_XY,
		actor->radius * NK_HYPNOTIZE_DIRECT_VERTICAL_EXIT_RADIUS_SCALE);

	bool continuing = state.DirectVertical && verticalSign != 0 &&
		verticalSign == state.DirectVerticalSign;
	bool enterVertical =
		xyDistance <= enterXYLimit &&
		zDistance >= NK_HYPNOTIZE_DIRECT_VERTICAL_MIN_Z &&
		zDistance >= xyDistance * NK_HYPNOTIZE_DIRECT_VERTICAL_Z_TO_XY_RATIO;
	bool keepVertical = continuing &&
		xyDistance <= exitXYLimit &&
		zDistance >= NK_HYPNOTIZE_DIRECT_VERTICAL_EXIT_MIN_Z &&
		zDistance >= xyDistance * NK_HYPNOTIZE_DIRECT_VERTICAL_EXIT_Z_TO_XY_RATIO;

	if (!enterVertical && !keepVertical)
	{
		state.DirectVertical = false;
		state.DirectVerticalSign = 0;
		return false;
	}

	// Probe only the next part of the vertical column. A latched approach still
	// has to yield immediately when a floor/ceiling/3D-floor really occupies the
	// column; only harmless XY threshold crossings are ignored by the hysteresis.
	double probeDistance = std::clamp(
		std::max(NK_VerticalCellSize(actor), actor->Height * 0.5),
		NK_HYPNOTIZE_DIRECT_VERTICAL_PROBE_MIN,
		NK_HYPNOTIZE_DIRECT_VERTICAL_PROBE_MAX);
	probeDistance = std::min(probeDistance, zDistance);

	DVector3 probeGoal = actor->Pos();
	probeGoal.Z += verticalSign > 0 ? probeDistance : -probeDistance;
	DVector3 accepted;
	if (!NK_CanTraverse(actor, actor->Pos(), probeGoal, true, accepted))
	{
		state.DirectVertical = false;
		state.DirectVerticalSign = 0;
		return false;
	}

	// Commit to pure Z motion while the vertical lane remains clear. The wider
	// exit envelope prevents a tiny XY drift from waking the 45/90-degree wall
	// recovery scan in the middle of an otherwise straight descent/ascent.
	state.DirectVertical = true;
	state.DirectVerticalSign = verticalSign;
	NK_HypnotizeBypasses.erase(actor);
	state.VerticalSign = 0;
	state.VerticalUntil = -1;
	actor->movedir = DI_NODIR;
	actor->movecount = 0;
	NK_UpdateHypnotizeVerticalVelocity(actor, delta.Z);
	actor->flags |= MF_INFLOAT;
	return std::abs(actor->Vel.Z) > 0.0001;
}

static int NK_StabilizeCrowdSteeringDirection(AActor *actor, const DVector2 &delta,
	int desiredDirection)
{
	if (!actor || !actor->Level || desiredDirection < DI_EAST ||
		desiredDirection >= DI_NODIR)
	{
		return desiredDirection;
	}

	auto found = NK_CrowdDetours.find(actor);
	if (found == NK_CrowdDetours.end())
	{
		return desiredDirection;
	}

	FNKCrowdDetourState &state = found->second;
	int now = actor->Level->maptime;
	if (now > state.ActiveUntil || state.Generation == 0 ||
		state.AppliedGeneration != state.Generation)
	{
		state.SteeringDirection = DI_NODIR;
		state.SteeringHoldUntil = -1;
		return desiredDirection;
	}

	int heldDirection = state.SteeringDirection;
	double distance = delta.Length();
	if (heldDirection >= DI_EAST && heldDirection < DI_NODIR &&
		heldDirection != desiredDirection && now <= state.SteeringHoldUntil &&
		NK_DirectionStepDistance(heldDirection, desiredDirection) == 1 &&
		distance >= std::max(NK_CROWD_STEERING_MIN_DISTANCE, actor->radius * 1.5))
	{
		double progress = delta.X * xspeed[heldDirection] + delta.Y * yspeed[heldDirection];
		if (progress > 0.0)
		{
			return heldDirection;
		}
	}

	state.SteeringDirection = desiredDirection;
	state.SteeringHoldUntil = now + NK_CROWD_STEERING_HOLD_TICS;
	return desiredDirection;
}

static bool NK_TryPlannedGroundDirection(AActor *actor, int direction,
	bool &softBlocked, bool &dynamicBlocked);

static int NK_LocalBypassSide(int desiredDirection, int actualDirection,
	uint32_t routeSeed)
{
	if (desiredDirection < DI_EAST || desiredDirection >= DI_NODIR ||
		actualDirection < DI_EAST || actualDirection >= DI_NODIR)
	{
		return (routeSeed & 1u) != 0 ? 1 : -1;
	}

	int difference = (actualDirection - desiredDirection) & 7;
	if (difference == 0 || difference == 4)
	{
		return (routeSeed & 1u) != 0 ? 1 : -1;
	}
	return difference < 4 ? 1 : -1;
}

static AActor *NK_GetCrowdBlocker(AActor *actor)
{
	if (!actor)
	{
		return nullptr;
	}

	AActor *blocker = actor->BlockingMobj;
	if (!blocker || blocker == actor || blocker == actor->target ||
		(blocker->ObjectFlags & OF_EuthanizeMe))
	{
		return nullptr;
	}
	return blocker;
}


static AActor *NK_ValidHypnotizeBlocker(AActor *actor, AActor *blocker)
{
	if (!actor || !blocker || blocker == actor || blocker == actor->target ||
		(blocker->ObjectFlags & OF_EuthanizeMe))
	{
		return nullptr;
	}
	return blocker;
}

static bool NK_SetHypnotizeBypassGoal(AActor *actor, AActor *blocker,
	int desiredDirection, bool flying, int preferredSide,
	FNKHypnotizeBypassState &state)
{
	if (!actor || !blocker || desiredDirection < DI_EAST ||
		desiredDirection >= DI_NODIR)
	{
		return false;
	}

	DVector2 forward(xspeed[desiredDirection], yspeed[desiredDirection]);
	DVector2 lateral(-forward.Y, forward.X);
	double clearance = actor->radius + blocker->radius +
		std::clamp(actor->radius * 0.5, 8.0, 20.0);
	double forwardLead = std::max(clearance * 0.75,
		std::max(36.0, actor->Speed * 4.0));

	if (preferredSide == 0)
	{
		DVector2 sideDelta = actor->Pos().XY() - blocker->Pos().XY();
		double sideDot = sideDelta.X * lateral.X + sideDelta.Y * lateral.Y;
		preferredSide = sideDot >= 0.0 ? 1 : -1;
	}
	const int sides[2] = { preferredSide, -preferredSide };

	for (int side : sides)
	{
		DVector2 candidateXY = blocker->Pos().XY() + lateral * (clearance * side) +
			forward * forwardLead;
		DVector3 candidate(candidateXY.X, candidateXY.Y, actor->Z());
		DVector3 accepted;
		if (!NK_CanTraverse(actor, actor->Pos(), candidate, flying, accepted))
		{
			continue;
		}

		state.Blocker = blocker;
		state.Side = side;
		state.DesiredDirection = desiredDirection;
		state.Goal = accepted.XY();
		state.BlockerCenter = blocker->Pos().XY();
		state.BlockerRadius = blocker->radius;
		state.Forward = forward;
		return true;
	}
	return false;
}

static bool NK_BeginHypnotizeActorBypass(AActor *actor, AActor *blocker,
	int desiredDirection, bool flying, int preferredSide = 0)
{
	if (!actor || !actor->Level ||
		!NK_ValidHypnotizeBlocker(actor, blocker))
	{
		return false;
	}

	FNKHypnotizeBypassState state;
	if (!NK_SetHypnotizeBypassGoal(actor, blocker, desiredDirection,
		flying, preferredSide, state))
	{
		return false;
	}

	int now = actor->Level->maptime;
	double distance = (state.Goal - actor->Pos().XY()).Length();
	int travelTics = int(std::ceil(distance / std::max(1.0, actor->Speed))) + 3;
	state.CommitUntil = now + NK_HYPNOTIZE_BYPASS_MIN_TICS;
	state.Until = now + std::clamp(travelTics,
		NK_HYPNOTIZE_BYPASS_MIN_TICS, NK_HYPNOTIZE_BYPASS_MAX_TICS);
	state.LastTouched = now;
	NK_HypnotizeBypasses[actor] = state;
	return true;
}

static bool NK_TryActiveHypnotizeBypass(AActor *actor, int desiredDirection,
	bool flying, bool &actuallyMoved);

static bool NK_FindHypnotizeGroundDynamicBlocker(AActor *actor,
	const DVector2 &goal, AActor *&nearestBlocker, DVector2 &predictedCenter)
{
	nearestBlocker = nullptr;
	predictedCenter = DVector2(0.0, 0.0);
	if (!actor || !actor->Level)
	{
		return false;
	}

	DVector2 start = actor->Pos().XY();
	DVector2 delta = goal - start;
	double fullLength = delta.Length();
	if (fullLength <= 0.0001)
	{
		return false;
	}

	double probeLength = std::min(fullLength,
		NK_HYPNOTIZE_GROUND_DYNAMIC_PROBE_RANGE);
	DVector2 end = start + delta * (probeLength / fullLength);
	DVector2 segment = end - start;
	double lengthSquared = segment.X * segment.X + segment.Y * segment.Y;
	if (lengthSquared <= 0.000001)
	{
		return false;
	}

	double actorBottom = actor->Z();
	double actorTop = actorBottom + actor->Height;
	double nearestAlong = probeLength + 1.0;

	auto iterator = actor->Level->GetThinkerIterator<AActor>();
	AActor *other;
	while ((other = iterator.Next()) != nullptr)
	{
		if (!NK_ValidHypnotizeBlocker(actor, other) ||
			!(other->flags & MF_SOLID) || (other->flags & MF_NOBLOCKMAP))
		{
			continue;
		}

		double otherBottom = other->Z();
		double otherTop = otherBottom + other->Height;
		if (actorTop <= otherBottom || otherTop <= actorBottom)
		{
			continue;
		}

		DVector2 current = other->Pos().XY();
		DVector2 future = current + other->Vel.XY() *
			NK_HYPNOTIZE_GROUND_DYNAMIC_LOOKAHEAD_TICS;
		const DVector2 samples[3] =
		{
			current,
			(current + future) * 0.5,
			future
		};

		double bestOtherAlong = probeLength + 1.0;
		bool intersectsLane = false;
		for (const DVector2 &sample : samples)
		{
			DVector2 offset = sample - start;
			double t = (offset.X * segment.X + offset.Y * segment.Y) /
				lengthSquared;
			if (t <= 0.02 || t >= 1.0)
			{
				continue;
			}

			DVector2 closest = start + segment * t;
			double clearance = actor->radius + other->radius +
				NK_HYPNOTIZE_GROUND_DYNAMIC_CLEARANCE_EXTRA;
			if ((sample - closest).Length() > clearance)
			{
				continue;
			}

			intersectsLane = true;
			bestOtherAlong = std::min(bestOtherAlong, probeLength * t);
		}

		if (!intersectsLane || bestOtherAlong >= nearestAlong)
		{
			continue;
		}

		nearestAlong = bestOtherAlong;
		nearestBlocker = other;
		predictedCenter = future;
	}

	return nearestBlocker != nullptr;
}

static bool NK_TryHypnotizeGroundDynamicDetour(AActor *actor,
	const DVector2 &delta, int desiredDirection, bool &actuallyMoved)
{
	actuallyMoved = false;
	if (!actor || desiredDirection < DI_EAST || desiredDirection >= DI_NODIR)
	{
		return false;
	}

	double distance = delta.Length();
	if (distance <= std::max(24.0, actor->radius * 2.0))
	{
		return false;
	}

	DVector2 forward(xspeed[desiredDirection], yspeed[desiredDirection]);
	double forwardLength = forward.Length();
	if (forwardLength <= 0.0001)
	{
		return false;
	}
	forward /= forwardLength;

	double probeLength = std::min(distance,
		NK_HYPNOTIZE_GROUND_DYNAMIC_PROBE_RANGE);
	DVector2 probeGoal = actor->Pos().XY() + forward * probeLength;
	AActor *blocker = nullptr;
	DVector2 predictedCenter;
	if (!NK_FindHypnotizeGroundDynamicBlocker(actor, probeGoal,
		blocker, predictedCenter))
	{
		return false;
	}

	// Choose the side away from where the crossing actor is expected to be a few
	// tics from now. Once committed, NK_TryActiveHypnotizeBypass keeps this side
	// stable and replans the bypass goal as the blocker itself moves.
	DVector2 lateral(-forward.Y, forward.X);
	double predictedSide = (predictedCenter - actor->Pos().XY()).X * lateral.X +
		(predictedCenter - actor->Pos().XY()).Y * lateral.Y;
	int preferredSide = 0;
	if (std::abs(predictedSide) > 1.0)
	{
		preferredSide = predictedSide > 0.0 ? -1 : 1;
	}

	if (!NK_BeginHypnotizeActorBypass(actor, blocker, desiredDirection,
		false, preferredSide))
	{
		return false;
	}

	return NK_TryActiveHypnotizeBypass(actor, desiredDirection,
		false, actuallyMoved);
}

static bool NK_TryHypnotizeBypassDirection(AActor *actor, int direction,
	bool flying, bool &actuallyMoved)
{
	actuallyMoved = false;
	if (!actor || direction < DI_EAST || direction >= DI_NODIR)
	{
		return false;
	}

	DVector2 before = actor->Pos().XY();
	double beforeZ = actor->Z();
	actor->movedir = direction;
	int moveResult = P_SmartMove(actor);
	actuallyMoved = actor->Pos().XY() != before;
	actor->movecount = 0;

	if (flying)
	{
		// Keep MF_FLOAT enabled while P_SmartMove tests the step. Its tm.floatok
		// result is important for entering stacked 3D-floor passages, so disabling
		// MF_FLOAT here (V10) made valid flight routes look physically blocked.
		//
		// The legacy recovery can report success with zero XY progress by nudging Z
		// by FloatSpeed after tm.floatok/P_TestMobjZ approved the height adjustment.
		// Hypnotize keeps the previous smooth-only behavior. Kitsune Spirit consumes
		// that validated Z step as a short terrain-clearance phase so its much faster
		// XY movement does not repeatedly hit the same slope/step edge.
		double floatOnlyDeltaZ = actor->Z() - beforeZ;
		bool floatOnlyRecovery = !actuallyMoved &&
			std::abs(floatOnlyDeltaZ) > 0.0001 && moveResult != 0;
		if (floatOnlyRecovery)
		{
			actor->flags |= MF_INFLOAT;

			// A real actor blocker should still use the committed actor bypass rather
			// than climbing over the actor. Restore the old Z in that case because the
			// MF_FLOAT adjustment was produced by a dynamic obstruction, not terrain.
			if (NK_ValidHypnotizeBlocker(actor, actor->BlockingMobj))
			{
				actor->SetZ(beforeZ);
				return false;
			}

			if (actor->nkSmartNavigationMode == 3 && actor->Level)
			{
				// P_Move already validated this exact Z nudge with tm.floatok and
				// P_TestMobjZ. Keep it instead of undoing it (V11/V12), remember its
				// direction, and bias the smooth Z controller for a few tics. The next
				// XY attempt still targets the same committed waypoint, so this is
				// clearance acquisition rather than a path or tactical replan.
				FNKHypnotizeFlightSteeringState &state =
					NK_HypnotizeFlightSteeringStates[actor];
				state.LastTouched = actor->Level->maptime;
				state.ClearanceSign = floatOnlyDeltaZ > 0.0 ? 1 : -1;
				state.ClearanceUntil = actor->Level->maptime +
					NK_KITSUNE_SPIRIT_CLEARANCE_HOLD_TICS;
				double clearanceDelta = std::max(
					NK_KITSUNE_SPIRIT_CLEARANCE_MIN_DELTA,
					std::max(actor->FloatSpeed * 4.0, actor->Height * 0.5));
				NK_UpdateHypnotizeVerticalVelocity(actor,
					clearanceDelta * state.ClearanceSign);
				return true;
			}

			// Preserve HypnotizeChase's existing smooth-flight behavior. Only
			// Kitsune Spirit consumes the engine-approved immediate clearance step.
			actor->SetZ(beforeZ);
			return true;
		}

		// P_SmartMove clears MF_INFLOAT after a normal XY move; flight Z steering is
		// owned by the waypoint follower, so restore it before P_ZMovement runs.
		actor->flags |= MF_INFLOAT;
	}
	return actuallyMoved || moveResult != 0;
}

static bool NK_TryActiveHypnotizeBypass(AActor *actor, int desiredDirection,
	bool flying, bool &actuallyMoved)
{
	actuallyMoved = false;
	auto found = NK_HypnotizeBypasses.find(actor);
	if (found == NK_HypnotizeBypasses.end() || !actor || !actor->Level)
	{
		return false;
	}

	int now = actor->Level->maptime;
	FNKHypnotizeBypassState &state = found->second;
	AActor *blocker = NK_ValidHypnotizeBlocker(actor, state.Blocker);
	if (!blocker || now > state.Until ||
		NK_DirectionStepDistance(state.DesiredDirection, desiredDirection) > 2)
	{
		NK_HypnotizeBypasses.erase(found);
		return false;
	}

	state.LastTouched = now;
	if ((blocker->Pos().XY() - state.BlockerCenter).Length() >
		NK_HYPNOTIZE_BYPASS_REPLAN_DISTANCE)
	{
		if (!NK_SetHypnotizeBypassGoal(actor, blocker, desiredDirection,
			flying, state.Side, state))
		{
			NK_HypnotizeBypasses.erase(found);
			return false;
		}
	}

	DVector2 fromBlocker = actor->Pos().XY() - state.BlockerCenter;
	double forwardProgress = fromBlocker.X * state.Forward.X +
		fromBlocker.Y * state.Forward.Y;
	double clearDistance = actor->radius + state.BlockerRadius + 6.0;
	if (now > state.CommitUntil && forwardProgress > clearDistance)
	{
		NK_HypnotizeBypasses.erase(found);
		return false;
	}

	DVector2 bypassDelta = state.Goal - actor->Pos().XY();
	if (bypassDelta.Length() <= std::max(8.0, actor->Speed * 1.5))
	{
		NK_HypnotizeBypasses.erase(found);
		return false;
	}

	int bypassDirection = NK_DirectionTo(bypassDelta);
	if (bypassDirection == DI_NODIR)
	{
		NK_HypnotizeBypasses.erase(found);
		return false;
	}

	bool moved = false;
	if (NK_TryHypnotizeBypassDirection(actor, bypassDirection, flying, moved))
	{
		actuallyMoved = moved;
		return true;
	}

	// The side is fixed for the lifetime of the bypass. If the exact vector is
	// temporarily blocked, try only the adjacent direction on that same side;
	// never alternate to the opposite side every tic.
	int sideDirection = (bypassDirection + state.Side + 8) & 7;
	if (NK_TryHypnotizeBypassDirection(actor, sideDirection, flying, moved))
	{
		actuallyMoved = moved;
		return true;
	}

	NK_HypnotizeBypasses.erase(found);
	return false;
}


static bool NK_TryHypnotizeLocal3DGeometrySteer(AActor *actor,
	const DVector3 &delta3, int desiredDirection)
{
	if (!actor || !actor->Level || desiredDirection < DI_EAST ||
		desiredDirection >= DI_NODIR)
	{
		return false;
	}

	FNKHypnotizeFlightSteeringState &state =
		NK_HypnotizeFlightSteeringStates[actor];
	int now = actor->Level->maptime;
	state.LastTouched = now;

	double verticalProbe = std::clamp(
		std::max(NK_VerticalCellSize(actor) * 1.5, actor->Height * 0.75),
		NK_HYPNOTIZE_3D_STEER_MIN_VERTICAL_PROBE,
		NK_HYPNOTIZE_3D_STEER_MAX_VERTICAL_PROBE);
	double forwardProbe = std::clamp(
		std::max(actor->radius * 2.0, actor->Speed * 6.0),
		NK_HYPNOTIZE_3D_STEER_MIN_FORWARD_PROBE,
		NK_HYPNOTIZE_3D_STEER_MAX_FORWARD_PROBE);

	int targetSign = delta3.Z > 4.0 ? 1 : (delta3.Z < -4.0 ? -1 : 0);
	int firstSign = 1;
	if (state.VerticalSign != 0 && now <= state.VerticalUntil)
	{
		firstSign = state.VerticalSign;
	}
	else if (targetSign != 0)
	{
		firstSign = targetSign;
	}
	int signs[2] = { firstSign, -firstSign };

	int sideOrder[3] = { desiredDirection, (desiredDirection + 1) & 7,
		(desiredDirection + 7) & 7 };
	if (state.Direction == sideOrder[2])
	{
		int temp = sideOrder[1];
		sideOrder[1] = sideOrder[2];
		sideOrder[2] = temp;
	}

	for (int sign : signs)
	{
		DVector3 verticalGoal = actor->Pos();
		verticalGoal.Z += verticalProbe * sign;
		DVector3 verticalAccepted;
		if (!NK_CanTraverse(actor, actor->Pos(), verticalGoal, true, verticalAccepted))
		{
			continue;
		}

		bool usefulLane = false;
		for (int direction : sideOrder)
		{
			DVector2 forward(xspeed[direction], yspeed[direction]);
			if (forward.LengthSquared() <= 0.0001)
			{
				continue;
			}
			forward.MakeUnit();

			DVector3 exitGoal = verticalAccepted;
			exitGoal.X += forward.X * forwardProbe;
			exitGoal.Y += forward.Y * forwardProbe;
			DVector3 exitAccepted;
			if (NK_CanTraverse(actor, verticalAccepted, exitGoal, true, exitAccepted))
			{
				usefulLane = true;
				state.Direction = direction;
				break;
			}
		}

		if (!usefulLane)
		{
			continue;
		}

		state.VerticalSign = sign;
		state.VerticalUntil = now + NK_HYPNOTIZE_3D_STEER_HOLD_TICS;
		NK_UpdateHypnotizeVerticalVelocity(actor, verticalProbe * sign);
		actor->movedir = DI_NODIR;
		actor->movecount = 0;
		return std::abs(actor->Vel.Z) > 0.0001;
	}

	state.VerticalSign = 0;
	state.VerticalUntil = -1;
	return false;
}

static bool NK_TryHypnotizeDeterministicMove(AActor *actor, const DVector3 &delta3,
	int desiredDirection, bool flying, bool &actuallyMoved,
	bool &usedFallbackDirection, bool &usedBypass)
{
	actuallyMoved = false;
	usedFallbackDirection = false;
	usedBypass = false;
	if (!actor || desiredDirection < DI_EAST || desiredDirection >= DI_NODIR)
	{
		return false;
	}

	DVector2 delta = delta3.XY();
	int previousDirection = actor->movedir;

	// Finish a committed actor bypass before making a new local steering choice.
	// The bypass side is fixed for its short lifetime, so it cannot alternate
	// left/right from one tic to the next.
	if (NK_TryActiveHypnotizeBypass(actor, desiredDirection, flying, actuallyMoved))
	{
		usedBypass = true;
		return true;
	}

	// Ground HypnotizeChase should not wait for physical contact before noticing
	// a monster that is crossing the committed waypoint lane. Probe only a short
	// distance ahead (including a few tics of blocker motion) and reuse the exact
	// same fixed-side Actor Bypass. Flight deliberately skips this path so the V11
	// 3D-floor/vertical behavior remains unchanged.
	if (!flying && NK_TryHypnotizeGroundDynamicDetour(actor, delta,
		desiredDirection, actuallyMoved))
	{
		usedBypass = true;
		return true;
	}

	bool moved = false;
	if (NK_TryHypnotizeBypassDirection(actor, desiredDirection, flying, moved))
	{
		actuallyMoved = moved;
		if (flying && moved)
		{
			// Real XY progress means any temporary terrain-clearance latch has done
			// its job. A vertical-only MF_FLOAT adjustment keeps the latch alive.
			NK_ClearHypnotizeVerticalSteer(actor);
		}
		return true;
	}

	// If the planned direction failed because a real actor occupies the next
	// step, bend around that actor using the committed-side bypass.
	AActor *blocker = NK_ValidHypnotizeBlocker(actor, actor->BlockingMobj);
	if (blocker && NK_BeginHypnotizeActorBypass(actor, blocker,
		desiredDirection, flying) &&
		NK_TryActiveHypnotizeBypass(actor, desiredDirection, flying, actuallyMoved))
	{
		usedBypass = true;
		return true;
	}

	// Static geometry is side-first. V5/V6 checked the vertical 3D-floor escape
	// here, before lateral motion, so a low wall or hill made the chaser climb even
	// when there was a clear route around its side. Pick one deterministic side
	// and try 45 then 90 degrees on that side before trying the opposite side.
	// A previous successful side is retained when possible to avoid left/right
	// oscillation along a wall.
	int preferredSide = 0;
	int plus45 = (desiredDirection + 1) & 7;
	int minus45 = (desiredDirection + 7) & 7;
	int plus90 = (desiredDirection + 2) & 7;
	int minus90 = (desiredDirection + 6) & 7;
	if (previousDirection == plus45 || previousDirection == plus90)
	{
		preferredSide = 1;
	}
	else if (previousDirection == minus45 || previousDirection == minus90)
	{
		preferredSide = -1;
	}
	else
	{
		double plusAlignment = delta.X * xspeed[plus45] + delta.Y * yspeed[plus45];
		double minusAlignment = delta.X * xspeed[minus45] + delta.Y * yspeed[minus45];
		preferredSide = plusAlignment >= minusAlignment ? 1 : -1;
	}

	int otherSide = -preferredSide;
	const int fallbackDirections[4] =
	{
		(desiredDirection + preferredSide + 8) & 7,
		(desiredDirection + preferredSide * 2 + 8) & 7,
		(desiredDirection + otherSide + 8) & 7,
		(desiredDirection + otherSide * 2 + 8) & 7
	};

	for (int direction : fallbackDirections)
	{
		if (NK_TryHypnotizeBypassDirection(actor, direction, flying, moved))
		{
			actuallyMoved = moved;
			usedFallbackDirection = true;
			if (flying && moved)
			{
				NK_ClearHypnotizeVerticalSteer(actor);
			}
			return true;
		}

		blocker = NK_ValidHypnotizeBlocker(actor, actor->BlockingMobj);
		if (blocker && NK_BeginHypnotizeActorBypass(actor, blocker,
			desiredDirection, flying) &&
			NK_TryActiveHypnotizeBypass(actor, desiredDirection, flying, actuallyMoved))
		{
			usedFallbackDirection = true;
			usedBypass = true;
			return true;
		}
	}

	// Only when no forward-facing lateral lane can be entered do we use the
	// local vertical probe. This remains useful for genuine stacked 3D-floor
	// passages where going above/below is necessary, but it no longer wins over
	// an ordinary side route around hills and walls.
	if (flying && NK_TryHypnotizeLocal3DGeometrySteer(actor, delta3, desiredDirection))
	{
		usedFallbackDirection = true;
		return true;
	}

	// No random scan and no turnaround. The path follower will invalidate this
	// blocked flight route and start a fresh sliced A* search from the real current
	// position instead of retrying a stale waypoint indefinitely.
	actor->movedir = DI_NODIR;
	actor->movecount = 0;
	return false;
}

static void NK_TryAdvanceHypnotizeFlyingPath(AActor *actor)
{
	if (!actor || (actor->nkSmartNavigationMode != 1 && actor->nkSmartNavigationMode != 3) ||
		actor->nkSmartPathIndex >= actor->nkSmartPath.Size() ||
		actor->nkSmartPathIndex + 1 >= actor->nkSmartPath.Size())
	{
		return;
	}

	unsigned current = actor->nkSmartPathIndex;
	unsigned last = std::min<unsigned>(unsigned(actor->nkSmartPath.Size() - 1),
		current + unsigned(NK_HYPNOTIZE_RUNTIME_SHORTCUT_LOOKAHEAD));
	if (last <= current + 1)
	{
		return;
	}

	// Test the farthest useful node first. If that segment is blocked, spend the
	// second and final check on a midpoint. A clear steep corridor therefore
	// becomes one continuous 3D leg, while geometry-required bends remain intact.
	unsigned candidates[NK_HYPNOTIZE_RUNTIME_SHORTCUT_CHECKS] = { last, 0 };
	candidates[1] = current + std::max<unsigned>(2u, (last - current) / 2u);
	if (candidates[1] >= last)
	{
		candidates[1] = last - 1;
	}

	for (int i = 0; i < NK_HYPNOTIZE_RUNTIME_SHORTCUT_CHECKS; ++i)
	{
		unsigned candidate = candidates[i];
		if (candidate <= current + 1 || candidate >= actor->nkSmartPath.Size())
		{
			continue;
		}
		if (i > 0 && candidate == candidates[0])
		{
			continue;
		}

		DVector3 accepted;
		if (NK_CanTraverse(actor, actor->Pos(), actor->nkSmartPath[candidate],
			true, accepted))
		{
			actor->nkSmartPath[candidate] = accepted;
			actor->nkSmartPathIndex = candidate;
			return;
		}
	}
}

static bool NK_CrowdBypassLandingFree(AActor *actor, const DVector3 &candidate)
{
	if (!actor)
	{
		return false;
	}

	FNKActorQueryRestore restore(actor);
	FCheckPosition check;
	return P_CheckMove(actor, candidate.XY(), check, PCM_DROPOFF);
}

static bool NK_SetCrowdBypassGoal(AActor *actor, const DVector2 &blockerCenter,
	double blockerRadius, int desiredDirection, int preferredSide,
	FNKLocalBypassState &state)
{
	if (!actor || desiredDirection < DI_EAST || desiredDirection >= DI_NODIR)
	{
		return false;
	}

	DVector2 forward(xspeed[desiredDirection], yspeed[desiredDirection]);
	DVector2 lateral(-forward.Y, forward.X);
	double clearance = actor->radius + blockerRadius +
		std::clamp(actor->radius * 0.75, 12.0, 32.0);
	double forwardLead = std::max(clearance * 0.75,
		std::max(48.0, actor->Speed * 5.0));

	int firstSide = preferredSide != 0 ? preferredSide :
		((uint32_t(actor->nkSmartRouteSeed) & 1u) != 0 ? 1 : -1);
	const int sides[2] = { firstSide, -firstSide };

	DVector3 geometryCandidates[2];
	bool geometryValid[2] = { false, false };
	for (int i = 0; i < 2; ++i)
	{
		DVector2 candidateXY = blockerCenter + lateral * (clearance * sides[i]) +
			forward * forwardLead;
		DVector3 candidate(candidateXY.X, candidateXY.Y, actor->Z());
		DVector3 accepted;
		if (!NK_CanTraverse(actor, actor->Pos(), candidate, false, accepted))
		{
			continue;
		}
		geometryCandidates[i] = accepted;
		geometryValid[i] = true;

		// Prefer a side whose landing point is not currently occupied. The
		// traversal test above deliberately ignores actors so a moving crowd does
		// not poison the static route; this second test is only a tie-breaker for
		// the short-lived local bypass goal.
		if (NK_CrowdBypassLandingFree(actor, accepted))
		{
			state.Side = sides[i];
			state.Goal = accepted.XY();
			state.BlockerCenter = blockerCenter;
			state.BlockerRadius = blockerRadius;
			state.Forward = forward;
			state.GoalValid = true;
			return true;
		}
	}

	// If both landing points are momentarily occupied, still keep a geometry-
	// valid side. The blocker may move before this actor gets there, and actual
	// movement remains actor-aware on every tic.
	for (int i = 0; i < 2; ++i)
	{
		if (geometryValid[i])
		{
			state.Side = sides[i];
			state.Goal = geometryCandidates[i].XY();
			state.BlockerCenter = blockerCenter;
			state.BlockerRadius = blockerRadius;
			state.Forward = forward;
			state.GoalValid = true;
			return true;
		}
	}

	state.GoalValid = false;
	return false;
}

static void NK_BeginLocalBypass(AActor *actor, int desiredDirection,
	int actualDirection, bool crowd, AActor *blocker = nullptr)
{
	if (!actor || !actor->Level)
	{
		return;
	}

	int now = actor->Level->maptime;
	FNKLocalBypassState &state = NK_LocalBypasses[actor];
	state.Side = NK_LocalBypassSide(desiredDirection, actualDirection,
		uint32_t(actor->nkSmartRouteSeed));
	state.Until = now + (crowd ? NK_CROWD_BYPASS_TICS : NK_CORNER_BYPASS_TICS);
	state.CommitUntil = now + (crowd ? NK_CROWD_BYPASS_COMMIT_TICS :
		NK_CORNER_BYPASS_COMMIT_TICS);
	state.LastTouched = now;
	state.Steps = 1;
	state.Crowd = crowd;
	state.GoalValid = false;
	state.RandomRecovery = false;
	state.WallEscape = false;
	state.CrowdRetreat = false;
	state.CrowdRetreatMomentum = false;
	state.CrowdRetreatDetourStarted = false;
	state.CrowdRetreatGeneration = 0;
	state.HeldDirection = DI_NODIR;

	if (crowd && blocker && blocker != actor->target &&
		!(blocker->ObjectFlags & OF_EuthanizeMe))
	{
		NK_RecordCrowdBlock(actor, blocker);
		if (NK_SetCrowdBypassGoal(actor, blocker->Pos().XY(), blocker->radius,
			desiredDirection, state.Side, state))
		{
			double distance = (state.Goal - actor->Pos().XY()).Length();
			int travelTics = int(std::ceil(distance / std::max(1.0, actor->Speed))) + 8;
			state.Until = now + std::clamp(travelTics, NK_CROWD_BYPASS_TICS, 70);
		}
	}
}

static void NK_BeginRandomRecovery(AActor *actor, int direction)
{
	if (!actor || !actor->Level || direction < DI_EAST || direction >= DI_NODIR)
	{
		return;
	}

	int now = actor->Level->maptime;
	int holdTics = std::clamp(int(actor->movecount),
		NK_RANDOM_RECOVERY_MIN_TICS, NK_RANDOM_RECOVERY_MAX_TICS);
	FNKLocalBypassState &state = NK_LocalBypasses[actor];
	state.Side = 0;
	state.Until = now + holdTics;
	state.CommitUntil = state.Until;
	state.LastTouched = now;
	state.Steps = 1;
	state.Crowd = false;
	state.GoalValid = false;
	state.RandomRecovery = true;
	state.WallEscape = false;
	state.CrowdRetreat = false;
	state.CrowdRetreatMomentum = false;
	state.CrowdRetreatDetourStarted = false;
	state.CrowdRetreatGeneration = 0;
	state.HeldDirection = direction;
	NK_RecordGroundSteeringDirection(actor, direction);
}

static void NK_BeginWallEscape(AActor *actor, int desiredDirection, int direction)
{
	if (!actor || !actor->Level || direction < DI_EAST || direction >= DI_NODIR)
	{
		return;
	}

	int now = actor->Level->maptime;
	int holdTics = NK_WALL_ESCAPE_MIN_TICS +
		pr_nksmartchase(NK_WALL_ESCAPE_MAX_TICS - NK_WALL_ESCAPE_MIN_TICS + 1);
	FNKLocalBypassState &state = NK_LocalBypasses[actor];
	state.Side = NK_LocalBypassSide(desiredDirection, direction,
		uint32_t(actor->nkSmartRouteSeed));
	state.Until = now + holdTics - 1;
	state.CommitUntil = state.Until;
	state.LastTouched = now;
	state.Steps = 1;
	state.Crowd = false;
	state.GoalValid = false;
	state.RandomRecovery = false;
	state.WallEscape = true;
	state.CrowdRetreat = false;
	state.CrowdRetreatMomentum = false;
	state.CrowdRetreatDetourStarted = false;
	state.CrowdRetreatGeneration = 0;
	state.HeldDirection = direction;
	NK_RecordGroundSteeringDirection(actor, direction);

	if (nk_smartchase_debug >= 4)
	{
		Printf("SmartChase wall escape: actor=%p direction=%s hold=%d\n",
			actor, NK_DebugDirectionName(direction), holdTics);
	}
}

static bool NK_TryBeginCrowdRetreat(AActor *actor, int desiredDirection,
	AActor *blocker, bool &actuallyMoved, bool &usedFallbackDirection,
	bool &usedRecoveryDirection, bool &softBlocked, bool &dynamicBlocked)
{
	if (!actor || !actor->Level || desiredDirection < DI_EAST ||
		desiredDirection >= DI_NODIR)
	{
		return false;
	}

	// Only choose from the rear half-plane. Shuffle the three candidates so
	// multiple actors caught in the same knot do not all back out identically.
	int retreatDirections[3] = {
		(desiredDirection + 3) & 7,
		(desiredDirection + 4) & 7,
		(desiredDirection + 5) & 7
	};
	for (int i = 2; i > 0; --i)
	{
		int j = pr_nksmartchase(i + 1);
		int temp = retreatDirections[i];
		retreatDirections[i] = retreatDirections[j];
		retreatDirections[j] = temp;
	}

	for (int direction : retreatDirections)
	{
		bool retreatSoftBlocked = false;
		bool retreatDynamicBlocked = false;
		if (!NK_TryPlannedGroundDirection(actor, direction,
			retreatSoftBlocked, retreatDynamicBlocked))
		{
			softBlocked = softBlocked || retreatSoftBlocked;
			dynamicBlocked = dynamicBlocked || retreatDynamicBlocked;
			continue;
		}

		int now = actor->Level->maptime;
		int holdTics = NK_CROWD_RETREAT_MAX_TICS;
		FNKLocalBypassState &state = NK_LocalBypasses[actor];
		state.Side = 0;
		state.Until = now + NK_CROWD_RETREAT_MAX_TICS - 1;
		state.CommitUntil = now + NK_CROWD_RETREAT_MIN_TICS - 1;
		state.LastTouched = now;
		state.Steps = 1;
		state.Crowd = true;
		state.GoalValid = false;
		state.RandomRecovery = false;
		state.WallEscape = false;
		state.CrowdRetreat = true;
		state.CrowdRetreatMomentum = false;
		state.CrowdRetreatDetourStarted = false;
		state.CrowdRetreatGeneration = 0;
		state.HeldDirection = direction;
		state.BlockerCenter = blocker ? blocker->Pos().XY() : actor->Pos().XY();
		state.BlockerRadius = blocker ? blocker->radius : 0.0;

		if (blocker)
		{
			NK_RecordCrowdBlock(actor, blocker);
		}
		actuallyMoved = true;
		usedFallbackDirection = true;
		usedRecoveryDirection = true;
		softBlocked = false;
		NK_RecordGroundSteeringDirection(actor, direction);

		if (nk_smartchase_debug >= 3)
		{
			Printf("SmartChase crowd retreat: actor=%p direction=%d hold=%d\n",
				actor, direction, holdTics);
		}
		return true;
	}

	return false;
}

static bool NK_TryAdvanceBlockedGroundPath(AActor *actor)
{
	if (!actor || actor->nkSmartPathIndex >= actor->nkSmartPath.Size() ||
		actor->nkSmartPathIndex + 1 >= actor->nkSmartPath.Size())
	{
		return false;
	}

	unsigned first = actor->nkSmartPathIndex + 1;
	bool stationarySurround = false;
	auto crowdState = NK_CrowdDetours.find(actor);
	if (crowdState != NK_CrowdDetours.end() && crowdState->second.SurroundGoalActive)
	{
		stationarySurround = true;
	}
	unsigned lookahead = stationarySurround ? 1u :
		unsigned(NK_BLOCKED_PATH_RECONNECT_LOOKAHEAD);
	unsigned last = std::min<unsigned>(unsigned(actor->nkSmartPath.Size() - 1),
		actor->nkSmartPathIndex + lookahead);
	for (unsigned i = last; i >= first; --i)
	{
		DVector3 checked;
		bool reachable = stationarySurround
			? NK_CanTraverseStationarySurround(actor, actor->Pos(),
				actor->nkSmartPath[i], checked)
			: NK_CanTraverse(actor, actor->Pos(), actor->nkSmartPath[i], false, checked);
		if (reachable)
		{
			actor->nkSmartPath[i] = checked;
			actor->nkSmartPathIndex = i;
			return true;
		}
		if (i == first)
		{
			break;
		}
	}
	return false;
}

static bool NK_CrowdRetreatForwardLaneClear(AActor *actor, int desiredDirection)
{
	if (!actor || desiredDirection < DI_EAST || desiredDirection >= DI_NODIR)
	{
		return false;
	}

	DVector2 forward(xspeed[desiredDirection], yspeed[desiredDirection]);
	double forwardLength = forward.Length();
	if (forwardLength <= 0.0001)
	{
		return false;
	}
	forward /= forwardLength;
	DVector2 probeGoal = actor->Pos().XY() + forward * NK_CROWD_FORWARD_PROBE_RANGE;
	AActor *blocker = nullptr;
	int blockerCount = 0;
	return !NK_FindCrowdLaneObstruction(actor, probeGoal, blocker, blockerCount);
}

static bool NK_CrowdRetreatHasSeparation(AActor *actor,
	const FNKLocalBypassState &state)
{
	if (!actor)
	{
		return false;
	}

	double needed = actor->radius + state.BlockerRadius +
		NK_CROWD_RETREAT_CLEARANCE_EXTRA;
	return (actor->Pos().XY() - state.BlockerCenter).Length() >= needed;
}

static bool NK_CrowdDetourRouteReady(AActor *actor)
{
	if (!actor || actor->nkSmartPathIndex >= actor->nkSmartPath.Size())
	{
		return false;
	}

	auto found = NK_CrowdDetours.find(actor);
	return found != NK_CrowdDetours.end() && found->second.Generation != 0 &&
		found->second.AppliedGeneration == found->second.Generation;
}

static bool NK_TryCrowdRetreatMovement(AActor *actor, int desiredDirection,
	FNKLocalBypassState &state, bool &softBlocked, bool &dynamicBlocked)
{
	if (!actor || desiredDirection < DI_EAST || desiredDirection >= DI_NODIR)
	{
		return false;
	}

	// Keep the current rearward direction first. If it closes, rotate through the
	// other rear-half-plane choices in the same tic instead of standing still for
	// the pending detour. This preserves retreat continuity without allowing the
	// old forward waypoint to take control again merely because one escape lane
	// became occupied.
	const int rearLeft = (desiredDirection + 3) & 7;
	const int rear = (desiredDirection + 4) & 7;
	const int rearRight = (desiredDirection + 5) & 7;
	int directions[3];
	int directionCount = 0;

	auto addDirection = [&](int direction)
	{
		for (int i = 0; i < directionCount; ++i)
		{
			if (directions[i] == direction)
			{
				return;
			}
		}
		if (directionCount < 3)
		{
			directions[directionCount++] = direction;
		}
	};

	if (state.HeldDirection == rearLeft || state.HeldDirection == rear ||
		state.HeldDirection == rearRight)
	{
		addDirection(state.HeldDirection);
	}

	if (state.HeldDirection == rearLeft)
	{
		addDirection(rear);
		addDirection(rearRight);
	}
	else if (state.HeldDirection == rearRight)
	{
		addDirection(rear);
		addDirection(rearLeft);
	}
	else if (state.HeldDirection == rear)
	{
		if ((uint32_t(actor->nkSmartRouteSeed) & 1u) != 0)
		{
			addDirection(rearLeft);
			addDirection(rearRight);
		}
		else
		{
			addDirection(rearRight);
			addDirection(rearLeft);
		}
	}
	else
	{
		// A previous emergency escape may have left HeldDirection outside the rear
		// trio. Re-establish a real retreat lane, with a stable per-actor side bias.
		addDirection(rear);
		if ((uint32_t(actor->nkSmartRouteSeed) & 1u) != 0)
		{
			addDirection(rearLeft);
			addDirection(rearRight);
		}
		else
		{
			addDirection(rearRight);
			addDirection(rearLeft);
		}
	}

	for (int i = 0; i < directionCount; ++i)
	{
		bool retreatSoftBlocked = false;
		bool retreatDynamicBlocked = false;
		if (NK_TryPlannedGroundDirection(actor, directions[i],
			retreatSoftBlocked, retreatDynamicBlocked))
		{
			state.HeldDirection = directions[i];
			state.Steps++;
			softBlocked = false;
			NK_RecordGroundSteeringDirection(actor, directions[i]);
			return true;
		}
		softBlocked = softBlocked || retreatSoftBlocked;
		dynamicBlocked = dynamicBlocked || retreatDynamicBlocked;
	}

	// Every rear lane is physically closed. Ask the vanilla chase direction
	// picker for one emergency movement, but bias it away from the crowd hotspot
	// rather than toward the old waypoint. Unlike the old momentum wait, this does
	// not report success unless XY really changed.
	DVector2 escapeDelta = actor->Pos().XY() - state.BlockerCenter;
	if (escapeDelta.LengthSquared() <= 0.0001)
	{
		escapeDelta = DVector2(-xspeed[desiredDirection], -yspeed[desiredDirection]);
	}
	DVector2 before = actor->Pos().XY();
	P_DoNewChaseDir(actor, escapeDelta.X, escapeDelta.Y);
	if (actor->Pos().XY() != before)
	{
		int emergencyDirection = actor->movedir;
		int relative = emergencyDirection >= DI_EAST && emergencyDirection < DI_NODIR
			? (emergencyDirection - desiredDirection + 8) & 7 : -1;
		if (relative >= 3 && relative <= 5)
		{
			state.HeldDirection = emergencyDirection;
		}
		state.Steps++;
		if (emergencyDirection >= DI_EAST && emergencyDirection < DI_NODIR)
		{
			NK_RecordGroundSteeringDirection(actor, emergencyDirection);
		}
		return true;
	}

	return false;
}

static bool NK_TryActiveLocalBypass(AActor *actor, int desiredDirection,
	bool &actuallyMoved, bool &usedFallbackDirection, bool &usedRecoveryDirection,
	bool &softBlocked, bool &dynamicBlocked)
{
	auto found = NK_LocalBypasses.find(actor);
	if (found == NK_LocalBypasses.end() || !actor->Level)
	{
		return false;
	}

	int now = actor->Level->maptime;
	FNKLocalBypassState &state = found->second;

	// Crowd retreat now overlaps movement with the replacement A* search. The
	// actor backs out for the full minimum interval first, then starts one fresh
	// Crowd Detour generation from the separated position while continuing to
	// retreat. As soon as that generation has produced an applied route, control
	// is handed directly back to SmartChase with no stop between retreat and route
	// following. The maximum interval is only a safety cap for unusually slow
	// searches.
	if (state.CrowdRetreat)
	{
		state.LastTouched = now;
		bool minimumSatisfied = now > state.CommitUntil;

		if (minimumSatisfied && !state.CrowdRetreatDetourStarted)
		{
			DVector2 hotspot = state.BlockerCenter;
			state.CrowdRetreatGeneration =
				NK_ForceCrowdDetourAfterRetreat(actor, hotspot);
			state.CrowdRetreatDetourStarted = state.CrowdRetreatGeneration != 0;
		}

		if (minimumSatisfied && state.CrowdRetreatDetourStarted &&
			NK_CrowdDetourRouteReady(actor))
		{
			NK_LocalBypasses.erase(found);
			return false;
		}

		if (now > state.Until)
		{
			// The replacement route is taking unusually long. Do not force another
			// generation; keep the same escape direction for a short grace period so
			// the actor still moves while the already-running search finishes.
			if (!state.CrowdRetreatDetourStarted)
			{
				DVector2 hotspot = state.BlockerCenter;
				state.CrowdRetreatGeneration =
					NK_ForceCrowdDetourAfterRetreat(actor, hotspot);
				state.CrowdRetreatDetourStarted = state.CrowdRetreatGeneration != 0;
			}
			state.CrowdRetreat = false;
			state.CrowdRetreatMomentum = true;
			state.Until = now + NK_CROWD_RETREAT_MOMENTUM_TICS - 1;
			state.CommitUntil = state.Until;
		}
		else if (NK_TryCrowdRetreatMovement(actor, desiredDirection, state,
			softBlocked, dynamicBlocked))
		{
			actuallyMoved = true;
			usedFallbackDirection = true;
			usedRecoveryDirection = true;
			return true;
		}
		else
		{
			// Do not create an intentional run-in-place wait. All rear lanes plus
			// the emergency local escape were physically unavailable this tic. Keep
			// the pending detour alive, but let normal SmartChase recovery continue
			// below instead of pretending that a movement succeeded.
			if (!state.CrowdRetreatDetourStarted)
			{
				DVector2 hotspot = state.BlockerCenter;
				state.CrowdRetreatGeneration =
					NK_ForceCrowdDetourAfterRetreat(actor, hotspot);
				state.CrowdRetreatDetourStarted = state.CrowdRetreatGeneration != 0;
			}
			return false;
		}
	}

	if (state.CrowdRetreatMomentum)
	{
		state.LastTouched = now;
		if (NK_CrowdDetourRouteReady(actor) || now > state.Until)
		{
			NK_LocalBypasses.erase(found);
			return false;
		}

		if (NK_TryCrowdRetreatMovement(actor, desiredDirection, state,
			softBlocked, dynamicBlocked))
		{
			actuallyMoved = true;
			usedFallbackDirection = true;
			usedRecoveryDirection = true;
			return true;
		}

		// No deliberate standstill while the generation is pending. If every
		// rearward and emergency escape is truly blocked, report failure honestly
		// so the ordinary movement/recovery pipeline gets one last chance this tic.
		return false;
	}

	if (state.WallEscape)
	{
		if (now > state.Until)
		{
			NK_LocalBypasses.erase(found);
			return false;
		}

		state.LastTouched = now;
		int side = state.Side == 0 ? 1 : state.Side;
		int candidates[3] = {
			state.HeldDirection,
			(desiredDirection + side * 2 + 8) & 7,
			(desiredDirection + side * 3 + 8) & 7
		};
		for (int i = 0; i < 3; ++i)
		{
			int direction = candidates[i];
			if (direction < DI_EAST || direction >= DI_NODIR)
			{
				continue;
			}
			bool duplicate = false;
			for (int j = 0; j < i; ++j)
			{
				duplicate = duplicate || candidates[j] == direction;
			}
			if (duplicate)
			{
				continue;
			}

			bool laneSoftBlocked = false;
			bool laneDynamicBlocked = false;
			if (NK_TryPlannedGroundDirection(actor, direction,
				laneSoftBlocked, laneDynamicBlocked))
			{
				state.HeldDirection = direction;
				state.Steps++;
				actuallyMoved = true;
				usedFallbackDirection = true;
				usedRecoveryDirection = true;
				NK_RecordGroundSteeringDirection(actor, direction);
				return true;
			}
			softBlocked = softBlocked || laneSoftBlocked;
			dynamicBlocked = dynamicBlocked || laneDynamicBlocked;
		}

		// The committed wall side is no longer usable. Release it instead of
		// immediately flipping to the opposite side; the normal pipeline below can
		// distinguish a new crowd blocker from a true geometry hard block.
		NK_LocalBypasses.erase(found);
		return false;
	}

	if ((!state.RandomRecovery && state.Side == 0) || now > state.Until)
	{
		NK_LocalBypasses.erase(found);
		return false;
	}

	state.LastTouched = now;

	// When a real geometry block forced the vanilla random-turn recovery, keep
	// the direction it found for a few tics. This mirrors Chase's movecount
	// behavior and prevents SmartChase from selecting a different left/right
	// escape every tic while the replacement route is being reconsidered.
	if (state.RandomRecovery)
	{
		if (state.HeldDirection >= DI_EAST && state.HeldDirection < DI_NODIR &&
			NK_TryPlannedGroundDirection(actor, state.HeldDirection,
				softBlocked, dynamicBlocked))
		{
			actuallyMoved = true;
			usedFallbackDirection = true;
			usedRecoveryDirection = true;
			state.Steps++;
			NK_RecordGroundSteeringDirection(actor, state.HeldDirection);
			return true;
		}

		// The held turn hit something new. Release it immediately so the normal
		// actor bypass or route recovery can choose a fresh escape.
		NK_LocalBypasses.erase(found);
		return false;
	}

	// Crowd bypasses use a real temporary point placed outside the blocking
	// actor's collision radius. This is intentionally separate from A*: moving
	// actors remain transient local obstacles and never invalidate the cached
	// static map graph.
	if (state.Crowd && state.GoalValid)
	{
		DVector2 toGoal = state.Goal - actor->Pos().XY();
		double reachDistance = std::max(actor->radius * 0.75,
			std::max(12.0, actor->Speed * 1.5));
		if (toGoal.Length() <= reachDistance)
		{
			NK_LocalBypasses.erase(found);
			NK_TryAdvanceBlockedGroundPath(actor);
			return false;
		}

		int goalDirection = NK_DirectionTo(toGoal);
		if (goalDirection != DI_NODIR)
		{
			const int directions[3] = {
				goalDirection,
				(goalDirection + state.Side + 8) & 7,
				(goalDirection - state.Side + 8) & 7
			};
			AActor *encounteredBlocker = nullptr;
			for (int direction : directions)
			{
				if (NK_TryPlannedGroundDirection(actor, direction,
					softBlocked, dynamicBlocked))
				{
					actuallyMoved = true;
					usedFallbackDirection = direction != desiredDirection;
					usedRecoveryDirection = true;
					state.Steps++;
					state.LastTouched = now;
					return true;
				}
				AActor *blocker = NK_GetCrowdBlocker(actor);
				if (blocker)
				{
					encounteredBlocker = blocker;
				}
			}

			if (encounteredBlocker)
			{
				NK_RecordCrowdBlock(actor, encounteredBlocker);
				// If the local bypass has already made a couple of committed steps
				// and another actor closes the lane, stop weaving inside the knot.
				// Back out into the rear half-plane, then rebuild the crowd detour
				// from the newly cleared position.
				if (state.Steps >= NK_CROWD_RETREAT_TRIGGER_STEPS &&
					NK_TryBeginCrowdRetreat(actor, desiredDirection, encounteredBlocker,
						actuallyMoved, usedFallbackDirection, usedRecoveryDirection,
						softBlocked, dynamicBlocked))
				{
					return true;
				}
				// A second actor entered the chosen lane. Keep the committed side
				// where possible, but build a new goal around the actor that is
				// actually blocking us now instead of oscillating back to the route.
				if (NK_SetCrowdBypassGoal(actor, encounteredBlocker->Pos().XY(),
					encounteredBlocker->radius, desiredDirection, state.Side, state))
				{
					double distance = (state.Goal - actor->Pos().XY()).Length();
					int travelTics = int(std::ceil(distance /
						std::max(1.0, actor->Speed))) + 8;
					state.Until = now + std::clamp(travelTics,
						NK_CROWD_BYPASS_TICS, 70);
					state.CommitUntil = std::max(state.CommitUntil,
						now + NK_CROWD_BYPASS_COMMIT_TICS);
					softBlocked = true;
					dynamicBlocked = true;
					return false;
				}
			}
		}

		// The selected bypass point itself became geometrically unusable. Once
		// the initial commitment has elapsed, try the opposite side around the
		// same blocker snapshot before falling back to the generic local escape.
		if (now >= state.CommitUntil &&
			NK_SetCrowdBypassGoal(actor, state.BlockerCenter, state.BlockerRadius,
				desiredDirection, -state.Side, state))
		{
			state.CommitUntil = now + NK_CROWD_BYPASS_COMMIT_TICS;
			softBlocked = true;
			return false;
		}

		state.GoalValid = false;
	}

	bool committed = now < state.CommitUntil || state.Steps < 2;
	int sideDirection = (desiredDirection + state.Side * 2 + 8) & 7;
	int diagonalDirection = (desiredDirection + state.Side + 8) & 7;

	int directions[3];
	int directionCount = 0;
	if (committed)
	{
		directions[directionCount++] = sideDirection;
		directions[directionCount++] = diagonalDirection;
	}
	else
	{
		directions[directionCount++] = diagonalDirection;
		directions[directionCount++] = desiredDirection;
		directions[directionCount++] = sideDirection;
	}

	for (int i = 0; i < directionCount; ++i)
	{
		int direction = directions[i];
		if (NK_TryPlannedGroundDirection(actor, direction, softBlocked, dynamicBlocked))
		{
			actuallyMoved = true;
			usedFallbackDirection = direction != desiredDirection;
			usedRecoveryDirection = direction != desiredDirection;
			if (direction == desiredDirection && !committed)
			{
				NK_LocalBypasses.erase(actor);
			}
			else
			{
				auto active = NK_LocalBypasses.find(actor);
				if (active != NK_LocalBypasses.end())
				{
					active->second.Steps++;
					active->second.LastTouched = now;
					if (active->second.Crowd && dynamicBlocked)
					{
						active->second.Until = std::max(active->second.Until,
							now + NK_CROWD_BYPASS_COMMIT_TICS);
					}
				}
			}
			return true;
		}
	}

	// If the committed side itself is occupied or runs into a wall, allow one
	// side swap instead of bouncing back toward the blocked waypoint.
	int oppositeSideDirection = (desiredDirection - state.Side * 2 + 8) & 7;
	if (NK_TryPlannedGroundDirection(actor, oppositeSideDirection,
		softBlocked, dynamicBlocked))
	{
		state.Side = -state.Side;
		state.Steps++;
		state.LastTouched = now;
		actuallyMoved = true;
		usedFallbackDirection = true;
		usedRecoveryDirection = true;
		return true;
	}

	if (!committed)
	{
		NK_LocalBypasses.erase(actor);
	}
	return false;
}

static int NK_GroundPathRecoveryDirection(AActor *actor, int desiredDirection)
{
	if (!actor || actor->nkSmartPathIndex >= actor->nkSmartPath.Size())
	{
		return DI_NODIR;
	}

	unsigned index = actor->nkSmartPathIndex;
	if (index + 1 < actor->nkSmartPath.Size())
	{
		DVector2 outgoing = actor->nkSmartPath[index + 1].XY() -
			actor->nkSmartPath[index].XY();
		int direction = NK_DirectionTo(outgoing);
		if (NK_DirectionStepDistance(desiredDirection, direction) == 2)
		{
			return direction;
		}
	}

	if (index > 0)
	{
		DVector2 incoming = actor->nkSmartPath[index].XY() -
			actor->nkSmartPath[index - 1].XY();
		int direction = NK_DirectionTo(incoming);
		if (NK_DirectionStepDistance(desiredDirection, direction) == 2)
		{
			return direction;
		}
	}

	return DI_NODIR;
}

static bool NK_TryPlannedGroundDirection(AActor *actor, int direction,
	bool &softBlocked, bool &dynamicBlocked)
{
	DVector2 before = actor->Pos().XY();
	actor->movedir = direction;
	int moveResult = P_SmartMove(actor);
	bool actuallyMoved = actor->Pos().XY() != before;
	actor->movecount = 0;

	if (actuallyMoved)
	{
		return true;
	}
	if (moveResult != 0)
	{
		// P_Move may report success after activating a usable line even when
		// the actor has not changed XY yet. Treat that as a soft wait rather
		// than declaring the A* route physically invalid.
		softBlocked = true;
	}
	if (actor->BlockingMobj != nullptr)
	{
		// The ground planner uses PCM_NOACTORS, so an actor collision is not
		// evidence that the cached map connection is bad. Remember it separately
		// and let the movement layer solve the temporary crowding locally. Repeated
		// failed movement against actors also feeds the dynamic crowd-detour trigger.
		NK_RecordCrowdBlock(actor, actor->BlockingMobj);
		dynamicBlocked = true;
		softBlocked = true;
	}
	return false;
}

static bool NK_TryPlannedGroundMove(AActor *actor, const DVector2 &delta,
	int desiredDirection, bool &actuallyMoved, bool &usedFallbackDirection,
	bool &usedRecoveryDirection, bool &softBlocked, bool &dynamicBlocked)
{
	actuallyMoved = false;
	usedFallbackDirection = false;
	usedRecoveryDirection = false;
	softBlocked = false;
	dynamicBlocked = false;

	// A one-tic sidestep immediately bends back into the same blocker on the
	// following tic. If a previous actor/corner collision committed a local
	// bypass, consume that short bypass first and rejoin the A* route afterward.
	if (NK_TryActiveLocalBypass(actor, desiredDirection, actuallyMoved,
		usedFallbackDirection, usedRecoveryDirection, softBlocked, dynamicBlocked))
	{
		return true;
	}

	if (NK_TryPlannedGroundDirection(actor, desiredDirection, softBlocked, dynamicBlocked))
	{
		actuallyMoved = true;
		return true;
	}

	AActor *crowdBlocker = NK_GetCrowdBlocker(actor);

	int firstFallback = (desiredDirection + 1) & 7;
	int secondFallback = (desiredDirection + 7) & 7;
	double firstAlignment = delta.X * xspeed[firstFallback] + delta.Y * yspeed[firstFallback];
	double secondAlignment = delta.X * xspeed[secondFallback] + delta.Y * yspeed[secondFallback];
	if (secondAlignment > firstAlignment)
	{
		int temp = firstFallback;
		firstFallback = secondFallback;
		secondFallback = temp;
	}

	const int fallbackDirections[2] = { firstFallback, secondFallback };
	for (int direction : fallbackDirections)
	{
		if (NK_TryPlannedGroundDirection(actor, direction, softBlocked, dynamicBlocked))
		{
			actuallyMoved = true;
			usedFallbackDirection = true;
			if (dynamicBlocked && crowdBlocker)
			{
				NK_BeginLocalBypass(actor, desiredDirection, direction, true, crowdBlocker);
				usedRecoveryDirection = true;
			}
			else if (!softBlocked)
			{
				// The route direction itself hit static geometry but an adjacent 45-degree
				// lane moved. Commit that successful lane briefly instead of asking the
				// blocked waypoint to choose left/right again on the next tic.
				NK_BeginWallEscape(actor, desiredDirection, direction);
				usedRecoveryDirection = true;
			}
			return true;
		}
		AActor *blocker = NK_GetCrowdBlocker(actor);
		if (blocker)
		{
			crowdBlocker = blocker;
		}
	}

	// If the three forward-facing choices are blocked, allow exactly one
	// route-informed 90-degree recovery step before broader local recovery.
	int recoveryDirection = NK_GroundPathRecoveryDirection(actor, desiredDirection);
	if (recoveryDirection != DI_NODIR)
	{
		if (NK_TryPlannedGroundDirection(actor, recoveryDirection, softBlocked, dynamicBlocked))
		{
			actuallyMoved = true;
			usedFallbackDirection = true;
			usedRecoveryDirection = true;
			if (dynamicBlocked && crowdBlocker)
			{
				NK_BeginLocalBypass(actor, desiredDirection, recoveryDirection, true, crowdBlocker);
			}
			else if (!softBlocked)
			{
				NK_BeginWallEscape(actor, desiredDirection, recoveryDirection);
			}
			return true;
		}
		AActor *blocker = NK_GetCrowdBlocker(actor);
		if (blocker)
		{
			crowdBlocker = blocker;
		}
	}

	if (dynamicBlocked)
	{
		// The A* graph intentionally ignores actors. Pick a side around the crowd
		// blocker and remember that side for several tics, instead of taking one
		// sidestep and immediately steering back into the same actor next tic.
		int firstSide = (desiredDirection + 2) & 7;
		int secondSide = (desiredDirection + 6) & 7;
		if ((uint32_t(actor->nkSmartRouteSeed) & 1u) == 0)
		{
			int temp = firstSide;
			firstSide = secondSide;
			secondSide = temp;
		}

		const int sideDirections[2] = { firstSide, secondSide };
		for (int direction : sideDirections)
		{
			if (NK_TryPlannedGroundDirection(actor, direction, softBlocked, dynamicBlocked))
			{
				actuallyMoved = true;
				usedFallbackDirection = true;
				usedRecoveryDirection = true;
				if (crowdBlocker)
				{
					NK_BeginLocalBypass(actor, desiredDirection, direction, true, crowdBlocker);
				}
				return true;
			}
			AActor *blocker = NK_GetCrowdBlocker(actor);
			if (blocker)
			{
				crowdBlocker = blocker;
			}
		}

		// The local side escapes all failed. This is where repeated left/right
		// dithering used to begin. Treat rearward space as phase one of the crowd
		// detour: back out first, then force a fresh global detour from that position.
		if (NK_TryBeginCrowdRetreat(actor, desiredDirection, crowdBlocker,
			actuallyMoved, usedFallbackDirection, usedRecoveryDirection,
			softBlocked, dynamicBlocked))
		{
			return true;
		}

		// No rear-half direction was physically available. Keep vanilla Chase only
		// as an emergency anti-freeze fallback; it is no longer the normal crowd
		// escape path.
		DVector2 before = actor->Pos().XY();
		P_DoNewChaseDir(actor, delta.X, delta.Y);
		if (actor->Pos().XY() != before)
		{
			actuallyMoved = true;
			usedFallbackDirection = true;
			usedRecoveryDirection = true;
			return true;
		}
	}
	else if (!softBlocked)
	{
		// A compressed or handed-off route can occasionally leave a waypoint on
		// the wrong side of a tight corner from the actor's *current* offset. If a
		// later committed waypoint is already reachable, skip the stale corner
		// point instead of invalidating the entire route and standing still.
		if (NK_TryAdvanceBlockedGroundPath(actor))
		{
			softBlocked = true;
			return false;
		}

		// No later waypoint can be reached directly. Try a short geometry escape
		// around the corner before declaring the route unusable. The side is kept
		// briefly so repeated calls do not alternate left/right and visibly dither.
		int firstSideSign = (uint32_t(actor->nkSmartRouteSeed) & 1u) != 0 ? 1 : -1;
		const int sideSigns[2] = { firstSideSign, -firstSideSign };
		for (int sideSign : sideSigns)
		{
			const int escapeDirections[2] = {
				(desiredDirection + sideSign * 2 + 8) & 7,
				(desiredDirection + sideSign * 3 + 8) & 7
			};
			for (int direction : escapeDirections)
			{
				if (NK_TryPlannedGroundDirection(actor, direction,
					softBlocked, dynamicBlocked))
				{
					actuallyMoved = true;
					usedFallbackDirection = true;
					usedRecoveryDirection = true;
					if (dynamicBlocked && crowdBlocker != nullptr)
					{
						NK_BeginLocalBypass(actor, desiredDirection, direction,
							true, crowdBlocker);
					}
					else
					{
						NK_BeginWallEscape(actor, desiredDirection, direction);
					}
					return true;
				}
				AActor *blocker = NK_GetCrowdBlocker(actor);
				if (blocker)
				{
					crowdBlocker = blocker;
				}
			}
		}

		// Last local corner escape. This is reached only after the deterministic
		// route directions and geometric side escapes all failed, so it cannot
		// replace normal A* navigation; it only nudges the actor out of a pathological
		// corner contact and lets the committed route take over again next tic.
		DVector2 before = actor->Pos().XY();
		P_DoNewChaseDir(actor, delta.X, delta.Y);
		if (actor->Pos().XY() != before)
		{
			actuallyMoved = true;
			usedFallbackDirection = true;
			usedRecoveryDirection = true;
			AActor *blocker = NK_GetCrowdBlocker(actor);
			if (blocker)
			{
				crowdBlocker = blocker;
				NK_BeginLocalBypass(actor, desiredDirection, actor->movedir,
					true, crowdBlocker);
			}
			else
			{
				NK_BeginRandomRecovery(actor, actor->movedir);
			}
			return true;
		}
	}

	actor->movedir = DI_NODIR;
	actor->movecount = 0;
	return false;
}

static bool NK_DirectPursuitMove(AActor *actor, const DVector3 &goal, bool flying)
{
	DVector2 delta = goal.XY() - actor->Pos().XY();
	if (flying)
	{
		double verticalSpeed = std::max(actor->FloatSpeed, std::max(1.0, actor->Speed * 0.5));
		actor->Vel.Z = std::clamp(goal.Z - actor->Z(), -verticalSpeed, verticalSpeed);
		actor->flags |= MF_INFLOAT;
	}

	int desiredDirection = NK_DirectionTo(delta);
	if (desiredDirection == DI_NODIR)
	{
		return flying && std::abs(actor->Vel.Z) > 0.0001;
	}

	DVector2 before = actor->Pos().XY();
	bool usedNewChaseDir = false;
	bool moved;
	if (!flying && actor->nkSmartNavigationMode == 2)
	{
		// Near-range SmartChase must not let P_DoNewChaseDir hide a crowd block by
		// finding a one-tic sidestep and reporting success. Try the actual player-
		// facing direction first; if another actor blocks it, BlockingMobj remains
		// available to the Actor Bypass -> Retreat -> Detour pipeline. If the first
		// failure was geometry-only, retain vanilla local turning as a fallback so
		// eight-direction quantization does not make a clear diagonal lane stutter.
		actor->movedir = desiredDirection;
		actor->movecount = 0;
		P_SmartMove(actor);
		moved = actor->Pos().XY() != before;
		if (!moved && actor->BlockingMobj == nullptr)
		{
			usedNewChaseDir = true;
			P_DoNewChaseDir(actor, delta.X, delta.Y);
			moved = actor->Pos().XY() != before;
		}
	}
	else if (actor->movedir != desiredDirection || actor->movecount <= 0)
	{
		usedNewChaseDir = true;
		P_DoNewChaseDir(actor, delta.X, delta.Y);
		moved = actor->Pos().XY() != before;
	}
	else
	{
		actor->movecount--;
		moved = P_SmartMove(actor) != 0;
	}
	NK_DebugDrawMoveDecision(actor, before, goal, desiredDirection, usedNewChaseDir, moved);

	if (flying)
	{
		actor->flags |= MF_INFLOAT;
	}
	return moved;
}

static bool NK_TryKitsuneSpiritPlanningMove(AActor *actor, const DVector3 &goal)
{
	if (!actor) return false;

	// Spirit A* is sliced and globally budgeted, so a pack may wait several tics
	// before its next six-node search slice. During that interval keep moving
	// toward the committed landing anchor with the same deterministic local
	// steering used by a finished flying route. This is intentionally not
	// P_DoNewChaseDir: no random Doom chase turn is introduced while planning.
	DVector3 delta3 = goal - actor->Pos();
	// Reuse HypnotizeChase's damped vertical controller while the sliced Spirit
	// route is still being planned. The old hard clamp could flip Z speed abruptly
	// near ledges and 3D geometry, making the spirit look caught on an edge even
	// though the local XY steering had already found a viable escape.
	if (!NK_ApplyKitsuneSpiritClearanceVelocity(actor, delta3.Z))
	{
		NK_UpdateHypnotizeVerticalVelocity(actor, delta3.Z);
	}
	actor->flags |= MF_INFLOAT;

	DVector2 delta = delta3.XY();
	int desiredDirection = NK_DirectionTo(delta);
	desiredDirection = NK_StabilizeHypnotizeSteeringDirection(
		actor, delta, desiredDirection);
	if (desiredDirection == DI_NODIR)
	{
		return std::abs(actor->Vel.Z) > 0.0001;
	}

	bool actuallyMoved = false;
	bool usedFallbackDirection = false;
	bool usedBypass = false;
	bool moved = NK_TryHypnotizeDeterministicMove(actor, delta3,
		desiredDirection, true, actuallyMoved, usedFallbackDirection, usedBypass);
	if (actuallyMoved)
	{
		NK_RecordHypnotizeSteeringDirection(actor, actor->movedir);
		actor->nkSmartLastProgressPos = actor->Pos();
		actor->nkSmartLastProgressTime = actor->Level->maptime;
		actor->nkSmartProgressValid = true;
	}
	actor->flags |= MF_INFLOAT;
	return moved || std::abs(actor->Vel.Z) > 0.0001;
}

static bool NK_TryClearDirectPursuit(AActor *actor, const DVector3 &goal, bool flying)
{
	DVector3 accepted;
	if (!NK_CanTraverse(actor, actor->Pos(), goal, flying, accepted))
	{
		// A finished route must not fall back to the vanilla local chase search
		// through a wall or railing while the replacement A* route is pending.
		// Make the next planning opportunity immediate instead.
		actor->movedir = DI_NODIR;
		actor->movecount = 0;
		actor->nkSmartNextRepath = 0;
		if (flying)
		{
			actor->Vel.Z = 0;
			actor->flags |= MF_INFLOAT;
		}
		return false;
	}

	return NK_DirectPursuitMove(actor, accepted, flying);
}

static bool NK_ShouldPrefetchTacticalHandoff(AActor *actor, bool flying)
{
	if (!actor || flying || actor->nkSmartNavigationMode != 2 ||
		!actor->nkSmartTacticalValid || !actor->nkSmartTargetValid ||
		actor->nkSmartPathIndex >= actor->nkSmartPath.Size())
	{
		return false;
	}

	// Start building the next tactical route while the final waypoint of the
	// committed route is still being consumed. This avoids a path=N/N gap in
	// which the actor would otherwise have no navigation route to follow.
	if (actor->nkSmartPathIndex + 1 != actor->nkSmartPath.Size())
	{
		return false;
	}

	if (NK_PendingGroundSearches.find(actor) != NK_PendingGroundSearches.end())
	{
		return false;
	}

	double handoffDistance = std::max(NK_PATH_HANDOFF_DISTANCE, actor->radius * 3.0);
	DVector2 toFinal = actor->nkSmartPath[actor->nkSmartPathIndex].XY() - actor->Pos().XY();
	return toFinal.Length() <= handoffDistance;
}

}

void P_NKInvalidateSmartPath(AActor *actor)
{
	if (!actor)
	{
		return;
	}
	actor->nkSmartPath.Clear();
	actor->nkSmartPathIndex = 0;
	actor->nkSmartTargetValid = false;
	actor->nkSmartNextRepath = 0;
	actor->nkSmartRoamNextTarget = 0;
	actor->nkSmartDisconnectedRoam = false;
	NK_PendingGroundSearches.erase(actor);
	NK_PendingFlyingSearches.erase(actor);
	NK_KitsuneSpiritRoams.erase(actor);
	NK_HypnotizeBypasses.erase(actor);
	NK_HypnotizeFlightSteeringStates.erase(actor);
	NK_LocalBypasses.erase(actor);
	NK_GroundSteeringStates.erase(actor);
	NK_HardBlockStallDebug.erase(actor);
}

static bool NK_FollowSmartPath(AActor *actor, const DVector3 &targetPos, bool flying,
	uint32_t routeSeed, bool weighted, bool limitSearch, bool allowPlanning)
{
	if (!actor)
	{
		return false;
	}

	DVector3 effectiveTarget = targetPos;
	double cellSize = NK_CellSize(actor, flying);
	int now = actor->Level->maptime;

	if (actor->nkSmartPathFlying != flying)
	{
		P_NKInvalidateSmartPath(actor);
		actor->nkSmartPathFlying = flying;
	}

	if (!actor->nkSmartProgressValid ||
		(actor->Pos() - actor->nkSmartLastProgressPos).Length() > std::max(2.0, actor->Speed * 0.35))
	{
		actor->nkSmartLastProgressPos = actor->Pos();
		actor->nkSmartLastProgressTime = now;
		actor->nkSmartProgressValid = true;
	}
	else if (now - actor->nkSmartLastProgressTime >= NK_STUCK_TICS)
	{
		bool wasRoaming = actor->nkSmartDisconnectedRoam;
		bool smartGround = !flying && actor->nkSmartNavigationMode == 2;
		bool pendingGroundSearch = smartGround &&
			NK_PendingGroundSearches.find(actor) != NK_PendingGroundSearches.end();
		bool pendingFlyingSearch = flying &&
			(actor->nkSmartNavigationMode == 1 || actor->nkSmartNavigationMode == 3) &&
			NK_PendingFlyingSearches.find(actor) != NK_PendingFlyingSearches.end();
		bool hardBlockedPath = smartGround && !actor->nkSmartTargetValid &&
			actor->nkSmartPathIndex < actor->nkSmartPath.Size();
		// Ground A* deliberately ignores actors. If the committed route is only
		// being held up by another actor, do not poison the static ground cache or
		// discard a geometrically valid path. The local movement fallback below
		// gets another chance to sidestep the dynamic blocker on the next call.
		bool dynamicallyBlocked = smartGround && actor->BlockingMobj != nullptr;
		if (wasRoaming && flying && actor->nkSmartNavigationMode == 3)
		{
			// A disconnected spirit is intentionally reusing a known local route.
			// A temporary crowd/wall stall must not invalidate that route and trigger
			// a new cross-TeleportGroup search. Let the outer crowd separation keep
			// working and retain the roam state.
			actor->nkSmartLastProgressTime = now;
			actor->movedir = DI_NODIR;
			actor->movecount = 0;
			return false;
		}

		if (!pendingGroundSearch && !pendingFlyingSearch &&
			!hardBlockedPath && !dynamicallyBlocked)
		{
			if (!flying && actor->nkSmartNavigationMode == 2)
			{
				NK_InvalidateGroundConnectionsNear(actor);
			}
			P_NKInvalidateSmartPath(actor);
			if (wasRoaming)
			{
				actor->nkSmartDisconnectedRoam = true;
				actor->nkSmartTacticalValid = false;
				actor->nkSmartRoamNextTarget = now;
				actor->nkSmartLastProgressTime = now;
				return false;
			}
		}
		actor->nkSmartLastProgressTime = now;
	}

	bool waitingForHardBlockReplacement = !flying && actor->nkSmartNavigationMode == 2 &&
		!actor->nkSmartTargetValid && actor->nkSmartPathIndex < actor->nkSmartPath.Size();
	bool targetMoved = !actor->nkSmartTargetValid ||
		(effectiveTarget - actor->nkSmartPathTarget).Length() > cellSize * 1.5;
	bool pathFinished = actor->nkSmartPathIndex >= actor->nkSmartPath.Size();
	bool crowdDetourNeedsPath = !flying && actor->nkSmartNavigationMode == 2 &&
		NK_CrowdDetourNeedsPath(actor);
	if (allowPlanning && (targetMoved || pathFinished || crowdDetourNeedsPath) &&
		now >= actor->nkSmartNextRepath)
	{
		if (limitSearch && flying && !NK_ClaimSearchBudget(actor))
		{
			actor->nkSmartNextRepath = now + 1;
		}
		else
		{
			// Build into a temporary route. A deferred or failed search must not erase
			// the route that is already moving the actor toward its previous tactical
			// goal. Only commit when a complete replacement route is available.
			TArray<DVector3> replacementPath;
			bool deferred = false;
			bool found = NK_BuildPath(actor, effectiveTarget, flying, replacementPath,
				routeSeed, weighted, limitSearch && !flying, deferred);
			if (found)
			{
				actor->nkSmartPath = std::move(replacementPath);
				actor->nkSmartPathIndex = 0;
				// Record the actual committed endpoint. For sliced ground searches this
				// may be an older tactical snapshot than effectiveTarget; keeping that
				// distinction lets the next tic start a fresh search toward the moved
				// tactical anchor without interrupting the route we just committed.
				actor->nkSmartPathTarget = actor->nkSmartPath[actor->nkSmartPath.Size() - 1];
				actor->nkSmartTargetValid = true;
				NK_MarkCrowdDetourPathApplied(actor);
				actor->nkSmartNextRepath = now + NK_REPATH_TICS;
			}
			else if (deferred)
			{
				actor->nkSmartNextRepath = now + 1;
			}
			else
			{
				actor->nkSmartNextRepath = now +
					(waitingForHardBlockReplacement ? 4 : NK_FAILED_REPATH_TICS);
				if (limitSearch)
				{
					actor->nkSmartNextTacticalUpdate =
						std::min(actor->nkSmartNextTacticalUpdate, now + 7);
				}
			}
		}
	}

	// In open space a nearly vertical target should be approached as a true
	// vertical flight, not as a sequence of tiny 8-way XY corrections. SpiritMove
	// can safely share this with HypnotizeChase because its effectiveTarget is the
	// already committed dry landing anchor, not the live player. The short column
	// probe still rejects floors, ceilings and 3D floors before taking the shortcut.
	if (flying && (actor->nkSmartNavigationMode == 1 ||
		actor->nkSmartNavigationMode == 3) &&
		NK_TryHypnotizeDirectVerticalApproach(actor, effectiveTarget))
	{
		return true;
	}

	// A route that has hard-blocked is kept only as a placeholder while a
	// replacement search runs. Do not keep consuming the known-bad waypoints;
	// doing so repeatedly aborts progress and recreates the stationary loop.
	if (!flying && actor->nkSmartNavigationMode == 2 &&
		!actor->nkSmartTargetValid &&
		actor->nkSmartPathIndex < actor->nkSmartPath.Size())
	{
		// While a replacement A* route is being built, a hard-block placeholder
		// used to force a complete standstill. A tight corner could then keep the
		// actor frozen until direct-pursuit range was reached. After a short delay,
		// permit one normal collision-aware local chase step; the sliced search is
		// still kept and will reconnect from the actor's new position when ready.
		if (allowPlanning &&
			now - actor->nkSmartLastProgressTime >= NK_HARD_BLOCK_ESCAPE_DELAY_TICS)
		{
			DVector2 escapeDelta = effectiveTarget.XY() - actor->Pos().XY();
			int escapeDesiredDirection = NK_DirectionTo(escapeDelta);
			DVector2 before = actor->Pos().XY();

			// A wall escape or random recovery that was already successful remains
			// useful while the replacement A* is being built. Consume it first so
			// hard-block planning does not turn a committed escape into a standstill.
			if (escapeDesiredDirection != DI_NODIR)
			{
				bool actuallyMoved = false;
				bool usedFallbackDirection = false;
				bool usedRecoveryDirection = false;
				bool localSoftBlocked = false;
				bool localDynamicBlocked = false;
				if (NK_TryActiveLocalBypass(actor, escapeDesiredDirection, actuallyMoved,
					usedFallbackDirection, usedRecoveryDirection, localSoftBlocked,
					localDynamicBlocked) && actuallyMoved)
				{
					NK_HardBlockStallDebug.erase(actor);
					NK_RecordGroundSteeringDirection(actor, actor->movedir);
					actor->nkSmartLastProgressPos = actor->Pos();
					actor->nkSmartLastProgressTime = now;
					actor->nkSmartProgressValid = true;
					return true;
				}
			}

			if (actor->Pos().XY() == before &&
				actor->movedir >= DI_EAST && actor->movedir < DI_NODIR &&
				actor->movecount > 0)
			{
				actor->movecount--;
				P_SmartMove(actor);
			}

			if (actor->Pos().XY() == before)
			{
				actor->movecount = 0;
				P_DoNewChaseDir(actor, escapeDelta.X, escapeDelta.Y);
				if (actor->Pos().XY() != before &&
					actor->movedir >= DI_EAST && actor->movedir < DI_NODIR)
				{
					NK_BeginRandomRecovery(actor, actor->movedir);
				}
			}

			if (actor->Pos().XY() != before)
			{
				NK_HardBlockStallDebug.erase(actor);
				NK_RecordGroundSteeringDirection(actor, actor->movedir);
				actor->nkSmartLastProgressPos = actor->Pos();
				actor->nkSmartLastProgressTime = now;
				actor->nkSmartProgressValid = true;
				return true;
			}
		}
		NK_DebugRecordHardBlockStall(actor);
		actor->movedir = DI_NODIR;
		actor->movecount = 0;
		return false;
	}

	if (actor->nkSmartPathIndex >= actor->nkSmartPath.Size())
	{
		// Both flight modes use sliced A*. Hypnotize follows its live target while
		// planning; Kitsune Spirit follows its already committed dry landing anchor.
		// In v7 mode 3 fell through to DI_NODIR here, which made every spirit visibly
		// stop whenever its search slice had not finished yet (and the one-search-per-
		// tic global budget made that pause longer for a pack).
		if (flying && actor->nkSmartNavigationMode == 3)
		{
			return NK_TryKitsuneSpiritPlanningMove(actor, effectiveTarget);
		}

		if (flying && actor->nkSmartNavigationMode == 1)
		{
			DVector3 liveDelta3 = effectiveTarget - actor->Pos();
			NK_UpdateHypnotizeVerticalVelocity(actor, liveDelta3.Z);

			DVector2 liveDelta = liveDelta3.XY();
			int liveDirection = NK_DirectionTo(liveDelta);
			liveDirection = NK_StabilizeHypnotizeSteeringDirection(
				actor, liveDelta, liveDirection);
			if (liveDirection == DI_NODIR)
			{
				actor->movedir = DI_NODIR;
				return std::abs(actor->Vel.Z) > 0.0001;
			}

			bool actuallyMoved = false;
			bool usedFallbackDirection = false;
			bool usedBypass = false;
			bool moved = NK_TryHypnotizeDeterministicMove(actor, liveDelta3,
				liveDirection, true, actuallyMoved, usedFallbackDirection, usedBypass);
			if (actuallyMoved)
			{
				NK_RecordHypnotizeSteeringDirection(actor, actor->movedir);
			}
			actor->flags |= MF_INFLOAT;
			return moved || std::abs(actor->Vel.Z) > 0.0001;
		}

		actor->movedir = DI_NODIR;
		if (flying)
		{
			actor->Vel.Z = 0;
			actor->flags |= MF_INFLOAT;
		}
		return false;
	}

	double reachDistance = std::max(actor->radius * 1.25, cellSize * 0.45);
	while (actor->nkSmartPathIndex < actor->nkSmartPath.Size())
	{
		DVector3 delta = actor->nkSmartPath[actor->nkSmartPathIndex] - actor->Pos();
		double distance = flying ? delta.Length() : delta.XY().Length();
		if (distance > reachDistance)
		{
			break;
		}
		actor->nkSmartPathIndex++;
	}

	// The bounded route compressor intentionally stops after a small fixed
	// amount of work. On long steep flights that can leave raw grid steps such
	// as forward/down/forward/down. Smooth those remaining steps incrementally
	// while moving so XY and Z keep changing together whenever the full 3D
	// diagonal is actually clear.
	if (flying && (actor->nkSmartNavigationMode == 1 || actor->nkSmartNavigationMode == 3) &&
		actor->nkSmartPathIndex < actor->nkSmartPath.Size())
	{
		NK_TryAdvanceHypnotizeFlyingPath(actor);
	}

	if (actor->nkSmartPathIndex >= actor->nkSmartPath.Size())
	{
		if (flying)
		{
			if (actor->nkSmartNavigationMode == 1 ||
				actor->nkSmartNavigationMode == 3)
			{
				// The last waypoint can be consumed while a small vertical difference
				// remains inside the reach radius. Ease toward the final target height
				// instead of snapping Vel.Z to zero for one tic.
				NK_UpdateHypnotizeVerticalVelocity(actor,
					effectiveTarget.Z - actor->Z());
			}
			else
			{
				actor->Vel.Z = 0;
			}
			actor->flags |= MF_INFLOAT;
		}
		return flying &&
			(actor->nkSmartNavigationMode == 1 || actor->nkSmartNavigationMode == 3) &&
			std::abs(actor->Vel.Z) > 0.0001;
	}

	DVector3 waypoint = actor->nkSmartPath[actor->nkSmartPathIndex];
	DVector2 delta = waypoint.XY() - actor->Pos().XY();

	if (flying)
	{
		double zdelta = waypoint.Z - actor->Z();
		if (actor->nkSmartNavigationMode == 1 ||
			actor->nkSmartNavigationMode == 3)
		{
			// SpiritMove shares HypnotizeChase's vertical damping while following
			// 3D waypoints. A recent engine-approved MF_FLOAT clearance step gets
			// temporary priority so the waypoint controller cannot immediately push
			// the actor back into the same slope/step edge.
			if (actor->nkSmartNavigationMode != 3 ||
				!NK_ApplyKitsuneSpiritClearanceVelocity(actor, zdelta))
			{
				NK_UpdateHypnotizeVerticalVelocity(actor, zdelta);
			}
		}
		else
		{
			double verticalSpeed = std::max(actor->FloatSpeed,
				std::max(1.0, actor->Speed * 0.5));
			actor->Vel.Z = std::clamp(zdelta, -verticalSpeed, verticalSpeed);
		}
		// Suppress the vanilla FLOAT target-height correction for this tic. It
		// steers directly toward the target and can fight a vertical waypoint
		// that deliberately goes above or below an obstacle.
		actor->flags |= MF_INFLOAT;
	}

	int desiredDirection = NK_DirectionTo(delta);
	if (desiredDirection == DI_NODIR)
	{
		return flying && std::abs(actor->Vel.Z) > 0.0001;
	}
	if (!flying && actor->nkSmartNavigationMode == 2)
	{
		// Track route progress independently from raw XY movement. A monster can
		// keep sidestepping around its neighbors while making almost no progress
		// toward the committed waypoint; that is the signal to reassess the whole
		// passage as crowded rather than repeatedly choosing the same corridor.
		DVector2 progressProbeGoal = waypoint.XY();
		unsigned progressProbeIndex = std::min<unsigned>(
			unsigned(actor->nkSmartPath.Size() - 1), actor->nkSmartPathIndex + 2);
		if (progressProbeIndex > actor->nkSmartPathIndex)
		{
			progressProbeGoal = actor->nkSmartPath[progressProbeIndex].XY();
		}
		NK_UpdateCrowdRouteProgress(actor, progressProbeGoal);

		// First retain the existing crowd-specific short hold, then apply a
		// geometry-agnostic angular dead-band to all SmartChase ground routes.
		// The latter is what suppresses the visible E/NE/E/NE staircase when the
		// ideal heading sits close to an eight-direction boundary. True corners
		// still turn immediately because the old direction falls outside 30 deg.
		desiredDirection = NK_StabilizeCrowdSteeringDirection(actor, delta, desiredDirection);
		desiredDirection = NK_StabilizeGroundSteeringDirection(actor, delta, desiredDirection);
	}
	else if (actor->nkSmartNavigationMode == 1 || actor->nkSmartNavigationMode == 3)
	{
		desiredDirection = NK_StabilizeHypnotizeSteeringDirection(
			actor, delta, desiredDirection);
	}

	DVector2 before = actor->Pos().XY();
	bool usedNewChaseDir = false;
	bool usedPathRecovery = false;
	bool softBlocked = false;
	bool dynamicBlocked = false;
	bool actuallyMoved = false;
	bool moved;
	if (!flying && actor->nkSmartNavigationMode == 2)
	{
		// SmartChase ground routes already supply the navigation decision.
		// Follow it deterministically: desired direction first, then only
		// the two adjacent 45-degree directions as local collision escapes.
		// If those all fail, one path-informed 90-degree recovery step is
		// allowed before the route is declared hard-blocked.
		moved = NK_TryPlannedGroundMove(actor, delta, desiredDirection,
			actuallyMoved, usedNewChaseDir, usedPathRecovery, softBlocked, dynamicBlocked);
	}
	else if (actor->nkSmartNavigationMode == 1 || actor->nkSmartNavigationMode == 3)
	{
		// HypnotizeChase and Kitsune SpiritMove follow their committed A*
		// waypoint deterministically. Never hand the
		// route back to P_DoNewChaseDir: that routine may perform Doom's random
		// direction scan and can ultimately choose the 180-degree turnaround.
		// Try the waypoint direction first, use the committed actor bypass only for
		// real actor collisions, then allow only the two forward 45-degree static-
		// geometry escapes. If all of those fail, the path layer replans instead.
		bool usedHypnotizeFallback = false;
		bool usedHypnotizeBypass = false;
		DVector3 hypnotizeDelta(delta.X, delta.Y, waypoint.Z - actor->Z());
		moved = NK_TryHypnotizeDeterministicMove(actor, hypnotizeDelta, desiredDirection,
			flying, actuallyMoved, usedHypnotizeFallback, usedHypnotizeBypass);
		usedNewChaseDir = usedHypnotizeFallback;
		usedPathRecovery = usedHypnotizeBypass;
	}
	else if (actor->movedir != desiredDirection || actor->movecount <= 0)
	{
		usedNewChaseDir = true;
		P_DoNewChaseDir(actor, delta.X, delta.Y);
		actuallyMoved = actor->Pos().XY() != before;
		moved = actuallyMoved;
	}
	else
	{
		actor->movecount--;
		moved = P_SmartMove(actor) != 0;
		actuallyMoved = actor->Pos().XY() != before;
	}
	NK_DebugDrawMoveDecision(actor, before, waypoint, desiredDirection, usedNewChaseDir,
		actuallyMoved, usedPathRecovery);
	if (!flying && actuallyMoved)
	{
		NK_RecordGroundSteeringDirection(actor, actor->movedir);
	}
	else if ((actor->nkSmartNavigationMode == 1 || actor->nkSmartNavigationMode == 3) && actuallyMoved)
	{
		NK_RecordHypnotizeSteeringDirection(actor, actor->movedir);
	}

	if (!moved)
	{
		if (!flying && actor->nkSmartNavigationMode == 2 && !softBlocked)
		{
			// The desired direction, both adjacent directions and the only
			// route-informed recovery direction all failed to produce XY motion.
			// This committed route is physically unusable from the actor's current
			// position. Keep it only as a placeholder so the outer chase code does
			// not fall back to P_DoNewChaseDir while a fresh A* search is running.
			NK_InvalidateGroundConnectionsNear(actor);
			NK_PendingGroundSearches.erase(actor);
			actor->nkSmartTargetValid = false;
			// This tactical anchor led the committed route into a physical hard
			// block. Reject the anchor immediately instead of retrying it until
			// the normal 35-70 tic tactical timer expires.
			actor->nkSmartTacticalValid = false;
			actor->nkSmartNextTacticalUpdate = now;
			actor->nkSmartNextRepath = 0;
			actor->movedir = DI_NODIR;
			actor->movecount = 0;
			actor->nkSmartLastProgressPos = actor->Pos();
			actor->nkSmartLastProgressTime = now;
			actor->nkSmartProgressValid = true;
		}
		else if (!flying && actor->nkSmartNavigationMode == 2)
		{
			// A usable special line may have been activated without changing XY.
			// Preserve the route and let the normal stuck timer handle a door that
			// never actually opens instead of immediately invalidating the path.
			actor->nkSmartNextRepath = std::min(actor->nkSmartNextRepath, now + 2);
		}
		else if (flying && (actor->nkSmartNavigationMode == 1 || actor->nkSmartNavigationMode == 3))
		{
			// A committed flying waypoint that cannot be entered from the actor's
			// current position is a hard geometry failure. V6 only shortened
			// nkSmartNextRepath here, but targetMoved/pathFinished stayed false, so
			// the same stale waypoint could be retried forever. Drop the unusable
			// route and restart the sliced search from the actual current position.
			NK_HypnotizeBypasses.erase(actor);
			NK_ClearHypnotizeVerticalSteer(actor);
			NK_PendingFlyingSearches.erase(actor);
			actor->nkSmartPath.Clear();
			actor->nkSmartPathIndex = 0;
			actor->nkSmartTargetValid = false;
			actor->nkSmartNextRepath = 0;
			actor->movedir = DI_NODIR;
			actor->movecount = 0;
			actor->nkSmartLastProgressPos = actor->Pos();
			actor->nkSmartLastProgressTime = now;
			actor->nkSmartProgressValid = true;
		}
		else
		{
			actor->nkSmartNextRepath = std::min(actor->nkSmartNextRepath, now + 2);
		}
	}
	if (flying)
	{
		// P_SmartMove clears MF_INFLOAT after a successful XY move, so restore
		// it before normal Z movement is processed.
		actor->flags |= MF_INFLOAT;
	}
	return moved;
}

static bool NK_KitsuneLandingDry(AActor *actor, const DVector2 &xy, DVector3 &out)
{
	if (!actor || !actor->Level) return false;
	sector_t *sec = actor->Level->PointInSector(xy);
	if (!sec) return false;

	double floorz = sec->floorplane.ZatPoint(xy);
	double ceilingz = sec->ceilingplane.ZatPoint(xy);
	if (ceilingz - floorz < actor->Height + 4.0) return false;

	int terrain = sec->GetTerrain(sector_t::floor);
	sector_t *hsec = sec->GetHeightSec();
	if (hsec != nullptr && (hsec->MoreFlags & SECMF_CLIPFAKEPLANES))
	{
		terrain = hsec->GetTerrain(sector_t::floor);
	}
	if (terrain >= 0 && Terrains[terrain].IsLiquid) return false;

	// Reject a landing point that is already occupied. Spirit flight itself may
	// overlap actors briefly, but the final human-form anchor should not.
	double crowdRadius = std::max(40.0, actor->radius * 2.5);
	FBoundingBox box(xy.X, xy.Y, crowdRadius);
	FBlockThingsIterator it(actor->Level, box);
	while (AActor *other = it.Next())
	{
		if (!other || other == actor || (other->ObjectFlags & OF_EuthanizeMe)) continue;
		if (!(other->flags & MF_SOLID)) continue;
		DVector2 d = other->Pos().XY() - xy;
		double minDist = actor->radius + other->radius + 10.0;
		if (d.LengthSquared() < minDist * minDist)
		{
			return false;
		}
	}

	DVector3 landing(xy.X, xy.Y, floorz + 1.0);
	DVector3 geometryAccepted;
	if (!NK_CheckMoveAt(actor, actor->Pos(), landing, true, geometryAccepted))
	{
		return false;
	}

	out = landing;
	return true;
}

static DVector2 NK_KitsuneForward2D(AActor *target)
{
	DVector2 v = target ? target->Vel.XY() : DVector2(0, 0);
	if (v.LengthSquared() > 1.0)
	{
		v.MakeUnit();
		return v;
	}
	if (target)
	{
		return target->Angles.Yaw.ToVector();
	}
	return DVector2(1, 0);
}

static void NK_GatherKitsuneSpiritLandingReservations(AActor *actor, AActor *player,
	std::vector<DVector2> &reservations)
{
	reservations.clear();
	if (!actor || !actor->Level || !player) return;

	// Scan the thinker list once per landing selection, not once per candidate.
	// A large map can contain hundreds of thinkers, so repeating that scan for
	// every point in the rear fan would turn crowd spreading into a new spike.
	auto iterator = actor->Level->GetThinkerIterator<AActor>();
	AActor *other;
	while ((other = iterator.Next()) != nullptr)
	{
		if (other == actor || (other->ObjectFlags & OF_EuthanizeMe)) continue;
		if (other->nkSmartNavigationMode != 3 || !other->nkSmartTacticalValid) continue;
		if (!NK_IsFlying(other)) continue;
		if (other->target != player) continue;
		reservations.push_back(other->nkSmartTacticalGoal.XY());
	}
}

static bool NK_KitsuneSpiritLandingReserved(const std::vector<DVector2> &reservations,
	const DVector2 &xy)
{
	constexpr double reserveDistance = 72.0;
	constexpr double reserveDistanceSq = reserveDistance * reserveDistance;
	for (const DVector2 &reserved : reservations)
	{
		if ((reserved - xy).LengthSquared() < reserveDistanceSq) return true;
	}
	return false;
}

static bool NK_SelectKitsuneSpiritLanding(AActor *actor, int role, bool hypnotizeConverge, DVector3 &goal)
{
	if (!actor || !actor->target) return false;
	AActor *player = actor->target;
	DVector2 forward = NK_KitsuneForward2D(player);
	DVector2 right(-forward.Y, forward.X);
	const bool elite = role != 0;
	std::vector<DVector2> reservations;
	reservations.reserve(8);
	NK_GatherKitsuneSpiritLandingReservations(actor, player, reservations);

	if (!elite && hypnotizeConverge)
	{
		// While the player is hypnotized and actively following this Kitsune, a
		// player-facing rear fan becomes a moving goal: the player turns toward the
		// spirit, the rear fan rotates, and both actors can orbit forever. In this
		// special return-to-waiting mode, commit a world-space convergence point on
		// the side of the player where the spirit currently is. The player and spirit
		// therefore close the same gap instead of chasing a rotating "behind" slot.
		DVector2 approach = actor->Pos().XY() - player->Pos().XY();
		if (approach.LengthSquared() < 64.0)
		{
			approach = DVector2(-forward.X, -forward.Y);
		}
		if (approach.LengthSquared() > 0.0001) approach.MakeUnit();

		static const double convergeOffsetsDeg[] = {
			0.0, -20.0, 20.0, -40.0, 40.0, -60.0, 60.0, -80.0, 80.0, -100.0, 100.0
		};
		static const double convergeRadii[] = {112.0, 128.0, 96.0, 144.0, 160.0};
		double baseAngle = std::atan2(approach.Y, approach.X);
		DVector3 reservedFallback;
		bool haveReservedFallback = false;

		for (double radius : convergeRadii)
		{
			for (double offsetDeg : convergeOffsetsDeg)
			{
				double a = baseAngle + offsetDeg * (3.14159265358979323846 / 180.0);
				DVector2 xy = player->Pos().XY() +
					DVector2(std::cos(a), std::sin(a)) * radius;
				DVector3 accepted;
				if (!NK_KitsuneLandingDry(actor, xy, accepted)) continue;

				if (!NK_KitsuneSpiritLandingReserved(reservations, xy))
				{
					goal = accepted;
					return true;
				}
				if (!haveReservedFallback)
				{
					reservedFallback = accepted;
					haveReservedFallback = true;
				}
			}
		}
		if (haveReservedFallback)
		{
			goal = reservedFallback;
			return true;
		}
		return false;
	}

	if (!elite)
	{
		// Normal Kitsunes still favor the player's rear hemisphere, but the candidate
		// lanes are spread farther toward the sides. Five seed-rotated slots cover the
		// rear half-plane in 45-degree steps: left side, left-rear, rear, right-rear
		// and right side. This keeps the stalking-from-behind identity without making
		// every spirit look glued to the player's exact rear arc.
		static const double rearOffsetsDeg[] = {-90.0, -45.0, 0.0, 45.0, 90.0};
		static const double rearRadii[] = {128.0, 154.0, 180.0};
		const int slotCount = int(countof(rearOffsetsDeg));
		const int radiusCount = int(countof(rearRadii));
		uint32_t seed = uint32_t(actor->nkSmartRouteSeed);
		if (seed == 0) seed = 1u;

		// Keep one rear-side lane preference for the whole Spirit session. The old
		// circular slot walk could wrap straight from -90 to +90, so a refreshed
		// anchor sometimes looked like the Kitsune suddenly abandoned its route and
		// crossed to the opposite side of the player. Search by angular distance from
		// the preferred lane instead: preferred, adjacent lanes, then the far side.
		int preferredSlot = int(seed % uint32_t(slotCount));
		bool positiveFirst = ((seed >> 3) & 1u) != 0;
		int slotOrder[5];
		int slotOrderCount = 0;
		slotOrder[slotOrderCount++] = preferredSlot;
		for (int distance = 1; distance < slotCount; ++distance)
		{
			int first = preferredSlot + (positiveFirst ? distance : -distance);
			int second = preferredSlot - (positiveFirst ? distance : -distance);
			if (first >= 0 && first < slotCount)
			{
				slotOrder[slotOrderCount++] = first;
			}
			if (second >= 0 && second < slotCount)
			{
				slotOrder[slotOrderCount++] = second;
			}
		}

		int radiusStart = int((seed >> 8) % uint32_t(radiusCount));
		double rearAngle = std::atan2(forward.Y, forward.X) + 3.14159265358979323846;

		DVector3 reservedFallback;
		bool haveReservedFallback = false;
		for (int radiusPass = 0; radiusPass < radiusCount; ++radiusPass)
		{
			double radius = rearRadii[(radiusStart + radiusPass) % radiusCount];
			for (int sample = 0; sample < slotOrderCount; ++sample)
			{
				int slot = slotOrder[sample];
				double offset = rearOffsetsDeg[slot] *
					(3.14159265358979323846 / 180.0);
				double a = rearAngle + offset;
				DVector2 xy = player->Pos().XY() +
					DVector2(std::cos(a), std::sin(a)) * radius;
				DVector3 accepted;
				if (!NK_KitsuneLandingDry(actor, xy, accepted)) continue;

				if (!NK_KitsuneSpiritLandingReserved(reservations, xy))
				{
					goal = accepted;
					return true;
				}
				if (!haveReservedFallback)
				{
					reservedFallback = accepted;
					haveReservedFallback = true;
				}
			}
		}
		if (haveReservedFallback)
		{
			goal = reservedFallback;
			return true;
		}
		return false;
	}

	// Elite keeps its existing forward-lead identity. Reservations are only a
	// tie-breaker so two Elites do not deliberately commit the exact same point.
	static const double eliteForward[] = {384, 352, 352, 320, 320, 416, 416, 288, 288, 224};
	static const double eliteSide[]    = {  0,  64, -64, 112,-112,  96, -96, 160,-160,   0};
	DVector3 reservedFallback;
	bool haveReservedFallback = false;
	for (int i = 0; i < int(countof(eliteForward)); ++i)
	{
		DVector2 xy = player->Pos().XY() + forward * eliteForward[i] + right * eliteSide[i];
		DVector3 accepted;
		if (!NK_KitsuneLandingDry(actor, xy, accepted)) continue;
		if (!NK_KitsuneSpiritLandingReserved(reservations, xy))
		{
			goal = accepted;
			return true;
		}
		if (!haveReservedFallback)
		{
			reservedFallback = accepted;
			haveReservedFallback = true;
		}
	}

	// Preserve the old Elite emergency ring rather than changing its behavior in
	// this patch. Normal Kitsunes intentionally do not use this full ring: their
	// valid landing domain is the requested rear half-plane only.
	double radius = 320.0;
	double baseAngle = std::atan2(forward.Y, forward.X);
	for (int step = 0; step < 12; ++step)
	{
		int signedStep = (step == 0) ? 0 : ((step + 1) / 2) * ((step & 1) ? 1 : -1);
		double a = baseAngle + signedStep * (3.14159265358979323846 / 12.0);
		DVector2 xy = player->Pos().XY() + DVector2(std::cos(a), std::sin(a)) * radius;
		DVector3 accepted;
		if (!NK_KitsuneLandingDry(actor, xy, accepted)) continue;
		if (!NK_KitsuneSpiritLandingReserved(reservations, xy))
		{
			goal = accepted;
			return true;
		}
		if (!haveReservedFallback)
		{
			reservedFallback = accepted;
			haveReservedFallback = true;
		}
	}
	if (haveReservedFallback)
	{
		goal = reservedFallback;
		return true;
	}
	return false;
}

static bool NK_KitsuneSpiritCrowdStep(AActor *actor, const DVector3 &goal)
{
	if (!actor || !actor->Level) return false;
	double scan = std::max(56.0, actor->radius * 3.0);
	FBoundingBox box(actor->X(), actor->Y(), scan);
	FBlockThingsIterator it(actor->Level, box);
	DVector2 separation(0, 0);
	int contributors = 0;
	while (AActor *other = it.Next())
	{
		if (!other || other == actor || other == actor->target || (other->ObjectFlags & OF_EuthanizeMe)) continue;
		if (!(other->flags & MF_SOLID)) continue;
		DVector2 away = actor->Pos().XY() - other->Pos().XY();
		double dist = away.Length();
		double desired = actor->radius + other->radius + 18.0;
		if (dist <= 0.001 || dist >= desired) continue;
		away /= dist;
		separation += away * ((desired - dist) / desired);
		contributors++;
	}
	if (!contributors || separation.LengthSquared() < 0.0025) return false;

	separation.MakeUnit();
	DVector2 toward = goal.XY() - actor->Pos().XY();
	if (toward.LengthSquared() > 1.0) toward.MakeUnit();
	DVector2 steer = toward * 0.65 + separation * 1.15;
	if (steer.LengthSquared() < 0.01) steer = separation;
	steer.MakeUnit();
	DVector3 localGoal = actor->Pos() + DVector3(steer.X * 48.0, steer.Y * 48.0,
		std::clamp(goal.Z - actor->Z(), -24.0, 24.0));
	return NK_DirectPursuitMove(actor, localGoal, true);
}

static void NK_UpdateKitsuneSpiritYaw(AActor *actor, const DVector2 &before)
{
	if (!actor) return;
	DVector2 moved = actor->Pos().XY() - before;
	if (moved.LengthSquared() > 0.0001)
	{
		actor->Angles.Yaw = moved.Angle();
		return;
	}

	// A pure vertical step has no new horizontal facing. If horizontal velocity
	// exists, however, use it so the sprite still follows a direct steering move.
	DVector2 velocity = actor->Vel.XY();
	if (velocity.LengthSquared() > 0.0001)
	{
		actor->Angles.Yaw = velocity.Angle();
	}
}


bool P_NKHypnotizeChaseMove(AActor *actor)
{
	if (!actor || !actor->target || (actor->target->ObjectFlags & OF_EuthanizeMe))
	{
		P_NKInvalidateSmartPath(actor);
		return false;
	}

	if (actor->nkSmartNavigationMode != 1)
	{
		P_NKInvalidateSmartPath(actor);
		actor->nkSmartNavigationMode = 1;
		actor->nkSmartProgressValid = false;
		actor->nkSmartTacticalValid = false;
	}

	bool flying = NK_IsFlying(actor);
	return NK_FollowSmartPath(actor, NK_TargetPosition(actor, flying), flying,
		0, false, false, true);
}


bool P_NKKitsuneSpiritMove(AActor *actor, int role, double moveSpeed, bool hypnotizeConverge)
{
	if (!actor || !actor->target || (actor->target->ObjectFlags & OF_EuthanizeMe))
	{
		P_NKInvalidateSmartPath(actor);
		if (actor)
		{
			actor->nkSmartTacticalValid = false;
			actor->nkSmartLiveTargetValid = false;
			actor->nkSmartLastChaseCallTime = -1;
		}
		return false;
	}

	// SpiritMove owns its movement speed while this state is active. The ZScript
	// caller can tune it independently from the human-form A_Chase speed.
	if (moveSpeed > 0.0)
	{
		actor->Speed = moveSpeed;
	}

	int now = actor->Level->maptime;
	bool continuousSpiritSession = actor->nkSmartNavigationMode == 3 &&
		actor->nkSmartLastChaseCallTime >= 0 &&
		(actor->nkSmartLastChaseCallTime == now ||
		 actor->nkSmartLastChaseCallTime == now - 1);

	// Human-form A_Chase does not call this routine. A gap of more than one tic
	// therefore marks a fresh Spirit session even if navigationMode was left at 3
	// by the previous transformation. Reset the old live-target sample here so a
	// player who merely walked around while the Kitsune was human cannot be
	// mistaken for a TeleportGroup warp on the next transformation.
	if (!continuousSpiritSession)
	{
		P_NKInvalidateSmartPath(actor);
		actor->nkSmartNavigationMode = 3;
		actor->nkSmartProgressValid = false;
		actor->nkSmartTacticalValid = false;
		actor->nkSmartLiveTargetValid = false;
		actor->nkSmartRouteSeed = int(NK_NewRouteSeed());
		if (nk_smartchase_debug >= 2)
		{
			Printf("KitsuneSpiritMove: new spirit session; reset stale target history\n");
		}
	}
	actor->nkSmartLastChaseCallTime = now;

	DVector3 liveTarget = actor->target->Pos();
	bool hadLiveTarget = actor->nkSmartLiveTargetValid;
	double liveShiftDistance = hadLiveTarget
		? (liveTarget - actor->nkSmartLastLiveTarget).Length()
		: 0.0;
	bool targetWarped = hadLiveTarget &&
		liveShiftDistance > NK_KITSUNE_SPIRIT_RETRY_WARP_DISTANCE;
	actor->nkSmartLastLiveTarget = liveTarget;
	actor->nkSmartLiveTargetValid = true;
	const DVector2 spiritMoveStart = actor->Pos().XY();

	// A TeleportGroup jump is the only live-target movement that cancels a
	// committed Spirit route. Ordinary player motion never restarts an in-flight
	// sliced search or discards an already completed route.
	if (targetWarped && !actor->nkSmartDisconnectedRoam)
	{
		if (nk_smartchase_debug >= 1)
		{
			Printf("KitsuneSpiritMove: player warp; cancel committed route and repath\n");
		}
		P_NKInvalidateSmartPath(actor);
		actor->nkSmartNavigationMode = 3;
		actor->nkSmartProgressValid = false;
		actor->nkSmartTacticalValid = false;
		actor->nkSmartLiveTargetValid = true;
		actor->nkSmartLastLiveTarget = liveTarget;
		actor->nkSmartRouteSeed = int(NK_NewRouteSeed());
	}

	// A completed failed search means the selected player landing point belongs to
	// another disconnected TeleportGroup. Do not keep rebuilding the same 3D A*.
	// The spirit simply oscillates on the known reachable route. A large player
	// warp is the cheap signal that TeleportGroup may have changed; otherwise only
	// make a low-frequency fallback retry.
	if (actor->nkSmartDisconnectedRoam)
	{
		auto roamFound = NK_KitsuneSpiritRoams.find(actor);
		if (roamFound != NK_KitsuneSpiritRoams.end())
		{
			roamFound->second.LastTouched = now;
		}

		bool retryPlayer = targetWarped || now >= actor->nkSmartRoamNextTarget;
		if (!retryPlayer)
		{
			if (actor->nkSmartPathIndex >= actor->nkSmartPath.Size() &&
				roamFound != NK_KitsuneSpiritRoams.end())
			{
				NK_SetKitsuneSpiritRoamLeg(actor, !roamFound->second.Reverse);
			}

			if (actor->nkSmartTacticalValid)
			{
				if (!NK_KitsuneSpiritCrowdStep(actor, actor->nkSmartTacticalGoal))
				{
					NK_FollowSmartPath(actor, actor->nkSmartTacticalGoal, true,
						uint32_t(actor->nkSmartRouteSeed), false, true, false);
				}
			}
			else
			{
				actor->Vel.X = actor->Vel.Y = actor->Vel.Z = 0;
				actor->flags |= MF_INFLOAT;
			}
			NK_UpdateKitsuneSpiritYaw(actor, spiritMoveStart);
			NK_DebugDrawSmartChase(actor, liveTarget,
				actor->nkSmartTacticalValid ? actor->nkSmartTacticalGoal : actor->Pos());
			return false;
		}

		if (nk_smartchase_debug >= 1)
		{
			Printf("KitsuneSpiritMove: disconnected roam retry (%s)\n",
				targetWarped ? "player warped" : "fallback timer");
		}
		P_NKInvalidateSmartPath(actor);
		actor->nkSmartNavigationMode = 3;
		actor->nkSmartProgressValid = false;
		actor->nkSmartTacticalValid = false;
		actor->nkSmartLiveTargetValid = true;
		actor->nkSmartLastLiveTarget = liveTarget;
		if (targetWarped)
		{
			actor->nkSmartRouteSeed = int(NK_NewRouteSeed());
		}
	}

	// Select one dry player-relative landing anchor and commit to it. Once a
	// sliced search starts, or a route has been committed, this anchor remains
	// frozen until arrival, a hard route failure/replan to the same anchor, or an
	// actual player warp. This prevents a moving player from resetting A* every
	// seven tics before the search can finish.
	if (!actor->nkSmartTacticalValid)
	{
		DVector3 selected;
		if (NK_SelectKitsuneSpiritLanding(actor, role, hypnotizeConverge, selected))
		{
			actor->nkSmartTacticalGoal = selected;
			actor->nkSmartTacticalValid = true;
			actor->nkSmartTargetValid = false;
			actor->nkSmartNextRepath = 0;
			actor->nkSmartNextTacticalUpdate = 0x3fffffff;
			if (nk_smartchase_debug >= 1)
			{
				Printf(hypnotizeConverge && role == 0 ? "KitsuneSpiritMove: committed hypnotize convergence anchor\n" : "KitsuneSpiritMove: committed dry landing anchor\n");
			}
		}
		else
		{
			// No dry landing exists around the current player position. This is not
			// a reason to launch an unbounded search. Hold locally and wait for a
			// player warp / low-frequency retry.
			NK_BeginKitsuneSpiritIdleRoam(actor);
			if (nk_smartchase_debug >= 1)
			{
				Printf("KitsuneSpiritMove: no dry landing candidate; holding local area\n");
			}
			NK_UpdateKitsuneSpiritYaw(actor, spiritMoveStart);
			NK_DebugDrawSmartChase(actor, liveTarget, actor->Pos());
			return false;
		}
	}

	DVector3 goal = actor->nkSmartTacticalGoal;
	double reach = std::max(24.0, actor->radius * 1.5);
	DVector3 delta = goal - actor->Pos();
	if (delta.XY().Length() <= reach &&
		std::abs(delta.Z) <= std::max(24.0, actor->Height * 0.5))
	{
		// Hypnotize convergence deliberately ignores the player's facing direction.
		// If the normal Kitsune has reached its committed convergence anchor and is
		// already close enough for the frozen/chasing player to finish closing the
		// gap, transform immediately. Rebuilding another player-relative anchor here
		// would reintroduce the circular chase this mode exists to prevent.
		if (role == 0 && hypnotizeConverge &&
			(actor->target->Pos().XY() - actor->Pos().XY()).Length() <= 176.0)
		{
			actor->Vel.X = actor->Vel.Y = actor->Vel.Z = 0;
			actor->flags |= MF_INFLOAT;
			NK_UpdateKitsuneSpiritYaw(actor, spiritMoveStart);
			NK_DebugDrawSmartChase(actor, liveTarget, goal);
			if (nk_smartchase_debug >= 1)
			{
				Printf("KitsuneSpiritMove: hypnotize converge reached; return to waiting\n");
			}
			return true;
		}

		// We reached the committed anchor. Only now sample the player's current
		// position again. If the preferred dry landing has moved substantially,
		// commit one new route instead of transforming far away from the player.
		DVector3 refreshed;
		if (NK_SelectKitsuneSpiritLanding(actor, role, hypnotizeConverge, refreshed) &&
			(refreshed - goal).Length() > 96.0)
		{
			actor->nkSmartTacticalGoal = refreshed;
			actor->nkSmartTacticalValid = true;
			actor->nkSmartPath.Clear();
			actor->nkSmartPathIndex = 0;
			actor->nkSmartTargetValid = false;
			actor->nkSmartNextRepath = 0;
			NK_PendingFlyingSearches.erase(actor);
			if (nk_smartchase_debug >= 1)
			{
				Printf("KitsuneSpiritMove: landing reached; player moved, commit next anchor\n");
			}
			goal = refreshed;
		}
		else
		{
			actor->Vel.X = actor->Vel.Y = actor->Vel.Z = 0;
			actor->flags |= MF_INFLOAT;
			NK_UpdateKitsuneSpiritYaw(actor, spiritMoveStart);
			NK_DebugDrawSmartChase(actor, liveTarget, goal);
			return true;
		}
	}

	// Only the short-range separation part of SmartChase is retained here. It can
	// bend the current step around another monster but never changes the committed
	// player-relative landing anchor or starts a new A* by itself.
	if (NK_KitsuneSpiritCrowdStep(actor, goal))
	{
		actor->flags |= MF_INFLOAT;
		NK_UpdateKitsuneSpiritYaw(actor, spiritMoveStart);
		NK_DebugDrawSmartChase(actor, liveTarget, goal);
		return false;
	}

	// Navigation mode 3 uses a bounded sliced flight search. The tactical anchor
	// above stays fixed while this runs, so six-node slices can actually finish.
	NK_FollowSmartPath(actor, goal, true, uint32_t(actor->nkSmartRouteSeed),
		false, true, true);
	actor->flags |= MF_INFLOAT;
	NK_UpdateKitsuneSpiritYaw(actor, spiritMoveStart);
	NK_DebugDrawSmartChase(actor, liveTarget, goal);
	return false;
}

bool P_NKSmartChaseMove(AActor *actor)
{
	if (!actor || !actor->target || (actor->target->ObjectFlags & OF_EuthanizeMe))
	{
		P_NKInvalidateSmartPath(actor);
		if (actor)
		{
			actor->nkSmartTacticalValid = false;
			actor->nkSmartLiveTargetValid = false;
			actor->nkSmartLastChaseCallTime = -1;
		}
		return false;
	}

	if (actor->nkSmartNavigationMode != 2)
	{
		P_NKInvalidateSmartPath(actor);
		actor->nkSmartNavigationMode = 2;
		actor->nkSmartProgressValid = false;
		actor->nkSmartTacticalValid = false;
		actor->nkSmartRouteSeed = 0;
		actor->nkSmartLiveTargetValid = false;
		actor->nkSmartLastChaseCallTime = -1;
	}

	bool flying = NK_IsFlying(actor);
	int now = actor->Level->maptime;
	bool repeatedSameTic = actor->nkSmartLastChaseCallTime == now;
	actor->nkSmartLastChaseCallTime = now;
	if (!flying)
	{
		NK_PrepareGroundCache(actor);
	}
	DVector3 liveTarget = NK_SmartTargetPosition(actor, flying);
	if (actor->nkSmartLiveTargetValid)
	{
		DVector3 liveShift = liveTarget - actor->nkSmartLastLiveTarget;
		double liveShiftDistance = flying ? liveShift.Length() : liveShift.XY().Length();
		if (liveShiftDistance > NK_TARGET_WARP_DISTANCE)
		{
			P_NKInvalidateSmartPath(actor);
			actor->nkSmartTacticalValid = false;
			actor->nkSmartNextTacticalUpdate = 0;
		}
		else if (actor->nkSmartTacticalValid && !actor->nkSmartDisconnectedRoam &&
			liveShiftDistance > 0.0001)
		{
			// Keep the selected flank/lead offset attached to the moving target.
			// Previously the tactical goal was a fixed world-space point for 35-70
			// tics, so SmartChase could spend one or two seconds pursuing where the
			// player used to be. Translating the anchor preserves the encirclement
			// shape while making path refreshes react to real target movement just
			// like HypnotizeChase does.
			DVector3 shiftedGoal = actor->nkSmartTacticalGoal + liveShift;
			DVector3 accepted;
			if (NK_ValidateGoalPosition(actor, shiftedGoal, flying, accepted) &&
				NK_IsPursuitAnchoredGoal(actor, accepted, liveTarget))
			{
				actor->nkSmartTacticalGoal = accepted;
			}
			else
			{
				// If the translated offset slid into new geometry, immediately choose
				// another valid tactical anchor around the current target instead of
				// following the stale point or waiting for the normal sector timer.
				actor->nkSmartTacticalValid = false;
				actor->nkSmartNextTacticalUpdate = now;
			}
		}
	}
	actor->nkSmartLastLiveTarget = liveTarget;
	actor->nkSmartLiveTargetValid = true;
	DVector3 predictedTarget = NK_PredictTargetPosition(actor, flying);

	// Zero-tic state chains may invoke A_SmartChase several times during one
	// game tic. Only the first invocation is allowed to select or build a new
	// route; later invocations still consume the existing waypoints so burst
	// movement keeps its intended collision-aware fast approach behavior.
	if (repeatedSameTic)
	{
		// Preserve burst-state behavior near the player without forcing a later
		// invocation in the same tic back onto the saved A* route. We keep that
		// route as a fallback, but every same-tic call still prefers direct pursuit
		// while the lane is physically usable. If another actor blocks the move,
		// fall through to the saved route/local bypass instead.
		double repeatedTargetDistance = flying
			? (liveTarget - actor->Pos()).Length()
			: (liveTarget.XY() - actor->Pos().XY()).Length();
		DVector3 repeatedDirectTarget;
		if (repeatedTargetDistance <= NK_DIRECT_PURSUIT_RANGE &&
			NK_CanTraverse(actor, actor->Pos(), predictedTarget, flying, repeatedDirectTarget) &&
			(flying || !NK_ShouldDeferDirectPursuitForCrowd(actor, repeatedDirectTarget.XY())))
		{
			if (NK_DirectPursuitMove(actor, repeatedDirectTarget, flying))
			{
				return true;
			}
			if (flying)
			{
				return false;
			}
			AActor *repeatedBlocker = NK_GetCrowdBlocker(actor);
			if (!repeatedBlocker)
			{
				return false;
			}
			NK_RecordCrowdBlock(actor, repeatedBlocker);
		}

		if (actor->nkSmartDisconnectedRoam)
		{
			if (actor->nkSmartTacticalValid &&
				actor->nkSmartPathIndex < actor->nkSmartPath.Size())
			{
				return NK_FollowSmartPath(actor, actor->nkSmartTacticalGoal, false,
					uint32_t(actor->nkSmartRouteSeed), true, true, false);
			}
			return false;
		}

		if (actor->nkSmartTacticalValid &&
			actor->nkSmartPathIndex < actor->nkSmartPath.Size())
		{
			bool moved = NK_FollowSmartPath(actor, actor->nkSmartTacticalGoal, flying,
				uint32_t(actor->nkSmartRouteSeed), true, true, false);
			if (moved)
			{
				return true;
			}
			if (actor->nkSmartPathIndex < actor->nkSmartPath.Size())
			{
				return false;
			}
		}
		return NK_TryClearDirectPursuit(actor, predictedTarget, flying);
	}

	double targetDistance = flying
		? (liveTarget - actor->Pos()).Length()
		: (liveTarget.XY() - actor->Pos().XY()).Length();

	DVector3 directTarget;
	if (targetDistance <= NK_DIRECT_PURSUIT_RANGE &&
		NK_CanTraverse(actor, actor->Pos(), predictedTarget, flying, directTarget) &&
		(flying || !NK_ShouldDeferDirectPursuitForCrowd(actor, directTarget.XY())))
	{
		// Near-range direct pursuit remains the preferred aggressive behavior, but
		// keep the current tactical/A* route until real movement proves the lane is
		// usable. NK_CanTraverse ignores actors, so a pair of monsters standing in
		// front of the player can pass this static test while physically blocking us.
		bool moved = NK_DirectPursuitMove(actor, directTarget, flying);
		if (moved)
		{
			NK_DebugDrawSmartChase(actor, liveTarget, predictedTarget);
			return true;
		}

		if (flying)
		{
			NK_DebugDrawSmartChase(actor, liveTarget, predictedTarget);
			return false;
		}

		AActor *nearBlocker = NK_GetCrowdBlocker(actor);
		if (!nearBlocker)
		{
			// The direct move failed for a non-crowd reason. Keep the backup route,
			// but do not convert a door/special/temporary geometry wait into a crowd
			// reroute. The normal chase call will try direct pursuit again next tic.
			NK_DebugDrawSmartChase(actor, liveTarget, predictedTarget);
			return false;
		}

		// A non-target actor really blocked near-range movement. Feed the same
		// local actor-bypass -> repeated block -> crowd-cost A* pipeline used while
		// following routes, then continue into tactical path handling this tic.
		NK_RecordCrowdBlock(actor, nearBlocker);
		DVector2 nearDelta = directTarget.XY() - actor->Pos().XY();
		int nearDesiredDirection = NK_DirectionTo(nearDelta);
		if (nearDesiredDirection != DI_NODIR)
		{
			int nearActualDirection = actor->movedir;
			if (nearActualDirection < DI_EAST || nearActualDirection >= DI_NODIR)
			{
				nearActualDirection = nearDesiredDirection;
			}
			NK_BeginLocalBypass(actor, nearDesiredDirection, nearActualDirection,
				true, nearBlocker);
		}
		actor->nkSmartNextRepath = 0;
	}

	if (!flying && actor->nkSmartDisconnectedRoam)
	{
		bool pendingGroundSearch =
			NK_PendingGroundSearches.find(actor) != NK_PendingGroundSearches.end();
		bool pathFinished = actor->nkSmartPathIndex >= actor->nkSmartPath.Size();
		bool needsGoal = !actor->nkSmartTacticalValid || pathFinished ||
			(!pendingGroundSearch && now >= actor->nkSmartRoamNextTarget);
		if (needsGoal && !pendingGroundSearch)
		{
			actor->nkSmartPath.Clear();
			actor->nkSmartPathIndex = 0;
			actor->nkSmartTargetValid = false;
			actor->nkSmartNextRepath = 0;
			DVector3 roamGoal;
			if (!NK_SelectRandomRoamGoal(actor, roamGoal))
			{
				actor->nkSmartDisconnectedRoam = false;
				actor->nkSmartTacticalValid = false;
			}
		}

		if (actor->nkSmartDisconnectedRoam && actor->nkSmartTacticalValid)
		{
			bool moved = NK_FollowSmartPath(actor, actor->nkSmartTacticalGoal, false,
				uint32_t(actor->nkSmartRouteSeed), true, true, true);
			NK_DebugDrawSmartChase(actor, liveTarget, predictedTarget);
			return moved;
		}
	}

	if (NK_ShouldPrefetchTacticalHandoff(actor, flying))
	{
		// Selecting a new tactical anchor does not discard the committed path.
		// NK_FollowSmartPath will build the replacement transactionally while
		// the actor keeps moving toward the old final waypoint.
		actor->nkSmartTacticalValid = false;
	}

	DVector3 tacticalGoal = NK_SelectTacticalGoal(actor, flying, now);
	bool moved = NK_FollowSmartPath(actor, tacticalGoal, flying,
		uint32_t(actor->nkSmartRouteSeed), true, true, true);
	if (!moved && !actor->nkSmartDisconnectedRoam &&
		actor->nkSmartPathIndex >= actor->nkSmartPath.Size())
	{
		// A finished route may use direct pursuit only when the predicted target
		// is genuinely reachable in a straight line. If an obstacle separates the
		// actor from the player, wait for/continue the replacement A* route instead
		// of handing control back to P_DoNewChaseDir.
		moved = NK_TryClearDirectPursuit(actor, predictedTarget, flying);
	}
	NK_DebugDrawSmartChase(actor, liveTarget, predictedTarget);
	return moved;
}
