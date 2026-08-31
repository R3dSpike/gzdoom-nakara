#ifndef __P_SMARTCHASE_H__
#define __P_SMARTCHASE_H__

class AActor;

// Runs the navigation-aware movement portion of A_HypnotizeChase.
// Target selection and attack decisions remain owned by A_DoChase.
bool P_NKHypnotizeChaseMove(AActor *actor);

// Runs predictive, route-diverse movement for A_SmartChase while A_DoChase
// continues to own normal combat target selection and attack decisions.
bool P_NKSmartChaseMove(AActor *actor);

// Lightweight, Kitsune-only spirit navigation. role=0 normally uses a rear fan;
// role=1 leads ahead. hypnotizeConverge makes role=0 approach from its current
// side of the player so hypnotize return cannot orbit around a rotating rear slot.
// Returns true when a dry landing anchor has been reached.
bool P_NKKitsuneSpiritMove(AActor *actor, int role, double moveSpeed, bool hypnotizeConverge);

// Drops cached navigation data so the next movement call builds a new route.
void P_NKInvalidateSmartPath(AActor *actor);

#endif
