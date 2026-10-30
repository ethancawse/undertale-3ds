// UNDERTALE's late copies of generated functions (utflowey; gmlaot.py's LATE_COPIES).
//
// A few small code entries run on hundreds of instances every frame in the heaviest bullet patterns (Photoshop
// Flowey's spinning eyes: some 250 at once). Each writes builtins on self (its angle and scale),
// and a builtin write in generated code goes the slow way, as the interpreter's Pop.v.v does: a site function that
// reads the instruction, the variable's definition, then VMBuiltins_setVariable's checks and switch. gmlaot.py gives
// such an entry, picked by its bytecode's digest, a second copy of its generated function (aotl_<k>, in aot_late.c,
// which platform/3ds/late_reserve.ld links into late text whole) where those writes are the setter's own work,
// inline; the table points at the copy, and aotf_<k> stays where it was, unchanged. Nothing else in the copy differs,
// and where self isn't an instance the copy runs aotf_<k> instead.
//
// Each AOT_LATE_SET_* below is VMBuiltins_setVariable's case for its builtin on self with a real value (no
// coercion: coerceIntStoreToReal leaves a real alone, and RValue_toReal of a real is its value), and the site (slow)
// for everything else: no instance, a struct, any other type. A tracing build always takes the site, which logs.
#ifndef AOT_LATE_COPY_H
#define AOT_LATE_COPY_H

#include "aot/vm_aot.h"
#include "runner.h"
#include "spatial_grid.h"

// self is an instance (not a struct, not none): the copy's case.
static inline bool aotLateSelfIsInstance(VMContext* ctx) {
    Instance* inst = (Instance*) ctx->currentInstance;
    return inst != nullptr && inst->objectIndex != STRUCT_OBJECT_INDEX;
}

#ifdef ENABLE_VM_TRACING
#define AOT_LATE_SELF_REAL(d, slow, inst_, v_, body) do { slow; } while (0)
#else
#define AOT_LATE_SELF_REAL(d, slow, inst_, v_, body) \
    do { \
        Instance* inst_ = (Instance*) ctx->currentInstance; \
        const RValue* v_ = &S[(d) - 1]; \
        if (__builtin_expect(inst_ != nullptr && inst_->objectIndex != STRUCT_OBJECT_INDEX && v_->type == RVALUE_REAL, 1)) { \
            body \
        } else { \
            slow; \
        } \
    } while (0)
#endif

// x, y, image_xscale, image_yscale: a float, and the spatial grid told when it changes.
#define AOT_LATE_SET_FLOAT_DIRTY(d, field, slow) \
    AOT_LATE_SELF_REAL(d, slow, inst_, v_, { \
        float value_ = (float) v_->real; \
        if (value_ != inst_->field) { \
            inst_->field = value_; \
            SpatialGrid_markInstanceAsDirty(((Runner*) ctx->runner)->spatialGrid, inst_); \
        } \
    })

// image_angle: a GMLReal, likewise.
#define AOT_LATE_SET_REAL_DIRTY(d, field, slow) \
    AOT_LATE_SELF_REAL(d, slow, inst_, v_, { \
        GMLReal value_ = v_->real; \
        if (value_ != inst_->field) { \
            inst_->field = value_; \
            SpatialGrid_markInstanceAsDirty(((Runner*) ctx->runner)->spatialGrid, inst_); \
        } \
    })

// image_index, image_speed, image_alpha: a float.
#define AOT_LATE_SET_FLOAT(d, field, slow) \
    AOT_LATE_SELF_REAL(d, slow, inst_, v_, { inst_->field = (float) v_->real; })

#endif // AOT_LATE_COPY_H
