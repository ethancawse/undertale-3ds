// New state is allocated only when a fountain draw first needs it. Neither
// CtrRenderer nor CtrUniform acquires a field or changes its original layout.
#include "ctr_fountain_uniforms.h"
_Static_assert(MAX_UNIFORMS <= FOUNTAIN_HANDLE_FIRST, "Fountain handles overlap base uniforms");
_Static_assert(sizeof(((CtrRenderer*) 0)->shaderFxOf) <= FOUNTAIN_HANDLE_SHADERS, "Fountain shader handle range is too small");

typedef struct CtrFountainState {
    CtrRenderer* owner;
    struct CtrFountainState* next;
    bool drawing;
    CtrFountainUniforms* uniforms;
    CtrFountainB2m patch;
} CtrFountainState;

static int32_t fountainUniformHandle(CtrRenderer* r, int32_t shader, const char* name);
static const float* fountainUniformValue(CtrRenderer* r, const char* name);
static void fountainUniformDispatch(CtrRenderer* r, int32_t handle, const float* values, uint32_t count);

// A separate section prevents GCC's local BSS anchor grouping from changing
// the offsets/code generation for pre-existing renderer globals.
static CtrFountainState* fountainStates __attribute__((section(".bss.fountain_late")));

static __attribute__((noinline, section(".fountain_patch")))
CtrFountainState* fountainFind(CtrRenderer* r) {
    for (CtrFountainState* s = fountainStates; s != nullptr; s = s->next)
        if (s->owner == r) return s;
    return nullptr;
}

static __attribute__((noinline, section(".fountain_patch")))
CtrFountainState* fountainGet(CtrRenderer* r) {
    CtrFountainState* s = fountainFind(r);
    if (s == nullptr) {
        s = safeCalloc(1, sizeof(*s));
        s->owner = r;
        s->next = fountainStates;
        fountainStates = s;
    }
    return s;
}

static __attribute__((noinline, section(".fountain_patch")))
bool fountainDrawing(CtrRenderer* r) {
    const CtrFountainState* s = fountainFind(r);
    return s != nullptr && s->drawing;
}

static __attribute__((noinline, section(".fountain_patch")))
void fountainDestroy(CtrRenderer* r) {
    CtrFountainState** link = &fountainStates;
    while (*link != nullptr) {
        CtrFountainState* s = *link;
        if (s->owner != r) {
            link = &s->next;
            continue;
        }
        if (s->patch.patchReady) C3D_TexDelete(&s->patch.patch);
        if (s->patch.maskReady) C3D_TexDelete(&s->patch.mask);
        while (s->uniforms != nullptr) {
            CtrFountainUniforms* u = s->uniforms;
            s->uniforms = u->next;
            free(u);
        }
        *link = s->next;
        free(s);
        return;
    }
}
