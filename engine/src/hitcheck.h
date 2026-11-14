// Included by collision.h after the common bbox definition.
#ifndef BS_HITCHECK_H
#define BS_HITCHECK_H
#if defined(ENABLE_WAD16) && !defined(HITCHECK_IMPLEMENTATION)
InstanceBBox Hitcheck_collisionBBox(Runner*, Instance*);
bool Hitcheck_instancesOverlapPrecise(Runner*, Instance*, Instance*, InstanceBBox, InstanceBBox);
void Hitcheck_freeRunner(Runner*);



#if defined(__3DS__) && defined(CTRPORT_HITCHECK_ROUTED)
// The release compiler adapter redirects only qualified collision call sites;
// their original C graph stays unchanged; all reader operands are qualified.
#define Collision_hitBBox Collision_computeBBox
#define Collision_readBBox Collision_computeBBox
#define Collision_hitPair Collision_instancesOverlapPrecise


#else
#define Collision_hitBBox Hitcheck_collisionBBox
#define Collision_readBBox Hitcheck_collisionBBox
#define Collision_hitPair Hitcheck_instancesOverlapPrecise


#endif
#else
#define Collision_hitBBox Collision_computeBBox
#define Collision_readBBox Collision_computeBBox
#define Collision_hitPair Collision_instancesOverlapPrecise


#endif
#endif
