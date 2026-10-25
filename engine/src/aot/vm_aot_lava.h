#ifndef _BS_VM_AOT_LAVA_H_
#define _BS_VM_AOT_LAVA_H_

/* Only the generator's audited lava Draw call site uses this entry. Keep the
 * same ABI and macro operations as AOT_CALL_BUILTIN: a layout gate verifies
 * the caller's instructions, data and extent against the ordinary generator. */
#if defined(__3DS__) && defined(ENABLE_WAD16) && defined(ENABLE_UTOLD_LAVA_DISPATCH) && !defined(UTOLD_LAVA_REFERENCE)
void Utold_lavaPartCall(VMContext* ctx, uint32_t instr,
                      const uint8_t* extraData, uint32_t funcIndex);
#define AOT_CALL_LAVA_PART(d, instr, addr, dAfter, funcIndex) \
    do { AOT_SYNC(d); ctx->ip = (addr) + 8; Utold_lavaPartCall(ctx, (instr), BC + (addr) + 4, (funcIndex)); AOT_CHECK(dAfter, (addr) + 8); } while (0)
#else
#define AOT_CALL_LAVA_PART AOT_CALL_BUILTIN
#endif

#endif
