// Exact fountain mesh, DELTARUNE only. No CtrRenderer layout changes.
#define PILLAR_LATE __attribute__((noinline, section(".text.late.pillar")))
#define PILLAR_RO __attribute__((section(".rodata.pillar_in_text")))
#define PILLAR_FX ((CtrShaderFx) 254)
#define PILLAR_OPAQUE(fn) ({ __typeof__(&fn) p_ = fn; __asm__("" : "+r"(p_)); p_; })
// Match main's existing assembler-alias technique for screensCtrDestroy.
// The base ignores this function's return, so GCC emits this void clone.
// Referencing the C definition from the new recursive mesh call graph changes
// the emission order of existing unnamed constants. Keep that graph separate;
// the object/link gates must verify the clone still exists at its base slot.
extern void pillarWindowRecordedSurface(CtrRenderer*) __asm__("windowRecordedSurface.isra.0");
static PILLAR_LATE void pillarDrawPairBatch(CtrRenderer* r, uint32_t count);
static PILLAR_LATE void pillarEmitPageRegion(CtrRenderer*, CtrPage*, float, float, float, float, const float*, const float*, const CtrColor*);
static PILLAR_LATE void pillarEmitPageRegionDirect(CtrRenderer*, CtrPage*, bool, float, float, float, float, const float*, const float*, const CtrColor*);
static PILLAR_LATE void pillarSurfaceQuad(CtrRenderer*, CtrSurface*, C3D_Tex*, float, float, float, float, float, float, const float*, const float*, const CtrColor*);
static PILLAR_LATE bool pillarSurfaceFxDraw(const CtrRenderer*);
static PILLAR_LATE void pillarGpuSetShader(Renderer*, int32_t);
static PILLAR_LATE int32_t pillarShaderGetUniform(Renderer*, int32_t, char*);
static PILLAR_LATE void pillarShaderSetUniformF(Renderer*, int32_t, int32_t, float, float, float, float);
static PILLAR_LATE void pillarShaderSetUniformFArray(Renderer*, int32_t, float*, uint32_t);
static PILLAR_LATE void pillarShaderSetUniformI(Renderer*, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
static PILLAR_LATE void pillarDestroy(Renderer*);
