// Shared arithmetic for the GMS 1.4 unrotated bbox. The two retained warm
// copies inline it; other callers use z_colexact.c's own-late entry point.
// Include after InstanceBBox and Collision_bboxRound are declared.
#ifndef _BS_COLEXACT_BBOX_H_
#define _BS_COLEXACT_BBOX_H_

// Keep attributes behind a macro for the raw-source AOT call audit.
#define COLEXACT_INLINE static inline __attribute__((always_inline))
COLEXACT_INLINE double colexactProduct(double a, double b) {
    double value = a * b;
#if defined(__arm__)
    __asm__ volatile("" : "+w"(value));
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("" : "+x"(value));
#else
    __asm__ volatile("" : "+m"(value));
#endif
    return value;
}

COLEXACT_INLINE InstanceBBox Colexact_inlineBBox(Sprite* spr, Instance* inst) {
    int32_t ml = spr->bboxMode == 1 ? 0 : spr->marginLeft;
    int32_t mt = spr->bboxMode == 1 ? 0 : spr->marginTop;
    int32_t w = spr->bboxMode == 1 ? (int32_t) spr->width : spr->marginRight + 1 - ml;
    int32_t h = spr->bboxMode == 1 ? (int32_t) spr->height : spr->marginBottom + 1 - mt;
    double xs = inst->imageXscale, ys = inst->imageYscale;
    int32_t l = (int32_t) Collision_bboxRound((double) inst->x + colexactProduct((float) (ml - spr->originX), xs));
    int32_t t = (int32_t) Collision_bboxRound((double) inst->y + colexactProduct((float) (mt - spr->originY), ys));
    int32_t r = (int32_t) Collision_bboxRound((double) (float) l + colexactProduct((float) w, xs));
    int32_t b = (int32_t) Collision_bboxRound((double) (float) t + colexactProduct((float) h, ys));
    if (l > r) { int32_t tmp = l; l = r; r = tmp; }
    if (t > b) { int32_t tmp = t; t = b; b = tmp; }
    return (InstanceBBox) {.left=l, .right=r, .top=t, .bottom=b, .valid=true};
}
#endif
