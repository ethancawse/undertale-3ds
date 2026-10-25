// Public fountain inputs live outside the base sequential allocator. Neither
// query order nor adding a supported fountain can renumber another shader.
static __attribute__((noinline, section(".fountain_patch"))) int32_t fountainUniformSlot(const char* name) {
    if (name == nullptr) return -1;
    if (strcmp(name, "iTime") == 0) return 0;
    if (strcmp(name, "iPosOffset") == 0) return 1;
    if (strcmp(name, "uvs") == 0) return 2;
    // iResolution is unused in the shader, and desktop returns -1 for it.
    return -1;
}

static __attribute__((noinline, section(".fountain_patch"))) CtrFountainUniforms* fountainUniformFind(CtrRenderer* r, int32_t shader) {
    CtrFountainState* state = fountainFind(r);
    if (state != nullptr) {
        for (CtrFountainUniforms* u = state->uniforms; u != nullptr; u = u->next)
            if (u->shader == shader) return u;
    }
    return nullptr;
}

static __attribute__((noinline, section(".fountain_patch"))) int32_t fountainUniformHandle(CtrRenderer* r, int32_t shader, const char* name) {
    int32_t slot = fountainUniformSlot(name);
    if (slot < 0 || shader < 0 || shader >= FOUNTAIN_HANDLE_SHADERS) return -1;
    CtrFountainUniforms* u = fountainUniformFind(r, shader);
    if (u == nullptr) {
        CtrFountainState* state = fountainGet(r);
        u = safeCalloc(1, sizeof(*u));
        u->shader = shader;
        u->next = state->uniforms;
        state->uniforms = u;
    }
    u->issued |= 1u << slot;
    return FOUNTAIN_HANDLE_FIRST + shader * FOUNTAIN_HANDLE_SLOTS + slot;
}

static __attribute__((noinline, section(".fountain_patch"))) const float* fountainUniformValue(CtrRenderer* r, const char* name) {
    static const float zero[4] = {0};
    int32_t slot = fountainUniformSlot(name);
    if (slot < 0) return zero;
    CtrFountainUniforms* u = fountainUniformFind(r, r->fxShader);
    return u != nullptr && (u->issued & (1u << slot)) != 0 ? u->values[slot] : zero;
}

static __attribute__((noinline, section(".fountain_patch"))) void fountainUniformDispatch(
    CtrRenderer* r, int32_t handle, const float* values, uint32_t count) {
    uint32_t index = (uint32_t) handle - FOUNTAIN_HANDLE_FIRST;
    if (index >= FOUNTAIN_HANDLE_LIMIT - FOUNTAIN_HANDLE_FIRST) {
        setUniform(r, handle, values, count);
        return;
    }
    uint32_t slot = index % FOUNTAIN_HANDLE_SLOTS;
    if (slot >= 3) return;
    int32_t shader = (int32_t) (index / FOUNTAIN_HANDLE_SLOTS);
    CtrFountainUniforms* u = fountainUniformFind(r, shader);
    // An arbitrary integer in the range cannot create a new binding.
    if (u == nullptr || (u->issued & (1u << slot)) == 0) return;
    if (count > 4) count = 4;
    if (memcmp(u->values[slot], values, count * sizeof(float)) == 0) return;
    bool current = r->shaderFx == SHADER_FX_FOUNTAIN && r->fxShader == shader;
    if (current) flushBatch(r);
    memcpy(u->values[slot], values, count * sizeof(float));
    if (current && r->inFrame) applyTexEnv(r);
}
