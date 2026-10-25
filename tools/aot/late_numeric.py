"""Experimental WAD16 numeric entries, generated only by an explicit caller.

The ordinary generated function and its caches stay byte-for-byte intact.
Only the selected table's function pointer changes. The caller and numeric
regions are separate translation units: only the regions require strict math.
All output derives from player data and belongs in a private build directory.
Normal generation never invokes this experiment without --late-numeric.
"""
import re


def late_copy(original, candidate, regions, k):
    """Return the unchanged original and a separately named guarded copy.

    Refuse inline script bindings for now: those have their own invalidation
    data, which this experiment does not duplicate. A full linked-byte gate
    must still prove placement of every new symbol inside existing reserves.
    """
    if candidate[1:] != original[1:]:
        raise ValueError('Numeric generation changed the entry ABI or bindings')
    body = candidate[0]
    if not regions or re.search(r'\baoti[cd]_\d+\b', body):
        raise ValueError('Expected numeric regions without inline script bindings')
    head = 'RValue aotf_%d(VMContext* ctx) {' % k
    if body.count(head) != 1:
        raise ValueError('Expected exactly one generated entry')
    # New caches belong to the copy. The original declarations and physical
    # lines are never changed, including __LINE__ literals in later functions.
    names = {'aotf_%d' % k: 'aotul_%d' % k,
             'aotlv_%d' % k: 'aotulv_%d' % k,
             'aotls_%d' % k: 'aotuls_%d' % k,
             'aotlc_%d' % k: 'aotulc_%d' % k}
    rename = re.compile(r'\b(' + '|'.join(map(re.escape, names)) + r')\b')
    body = rename.sub(lambda m: names[m[0]], body)
    # Force zeroed cache storage into reserved initialized data. The original
    # function's ordinary BSS must stay the same size and at the same address.
    body = re.sub(r'^static (uint32_t aotuls_|AotLocalCache aotulc_)',
                  r'static __attribute__((section(".data.late.utoldloop"))) \1', body, flags=re.M)
    numeric = '\n\n'.join(t[2] for t in regions)
    # Region symbols are private to this generated entry family too.
    region_name = re.compile(r'\baot([ts])_' + str(k) + r'_([0-9A-Fa-f]+)\b')
    body = region_name.sub(r'aotul\1_' + str(k) + r'_\2', body)
    numeric = region_name.sub(r'aotul\1_' + str(k) + r'_\2', numeric)
    new_head = 'RValue aotul_%d(VMContext* ctx) {' % k
    # This reference retains the original body and all of its caches. No
    # effects or local resolution happen before the ordinary fallback.
    guard = ('\n    if (ctx->currentInstance == nullptr ||\n'
             '        ((Instance*)ctx->currentInstance)->objectIndex == STRUCT_OBJECT_INDEX)\n'
             '        return aotf_%d(ctx);' % k)
    body = ('RValue aotf_%d(VMContext* ctx) AOT_FN_DECL;\n' % k
            + body.replace(new_head, new_head + guard, 1))
    declaration = 'RValue aotf_%d(VMContext* ctx) AOT_FN_DECL;' % k
    if original[1].count(declaration) != 1:
        raise ValueError('Expected one table declaration')
    table = original[1].replace(declaration, declaration +
                               ' RValue aotul_%d(VMContext* ctx);' % k, 1)
    pointer = ' aotf_%d,' % k
    if original[2].count(pointer) != 1:
        raise ValueError('Expected one table function pointer')
    entry = original[2].replace(pointer, ' aotul_%d,' % k, 1)
    return dict(original=original[0], caller=body, regions=numeric,
                table=table, entry=entry, instruction_count=original[3])


def generate(G, dw, name, k, report):
    """Generate one late entry, restoring every experimental compiler switch.

    WAD17 is rejected before generation, rather than admitting UT code into
    chapter modules. Ordinary --typed configurations remain independent.
    """
    if dw.wad != 16:
        raise ValueError('Late numeric experiment requires WAD16')
    import typed as T
    T.bind(G)
    old_mod, old_selection, old_flag = G.typed_mod, G.TYPED, T.NUMERIC_SPRITE_LOOPS
    old_texts, old_bindings = list(G.TYPED_TEXTS), list(G.TYPED_BIND)
    keep_texts, keep_bindings = old_texts, old_bindings
    try:
        original = G.generate_function(dw, name, k, [])
        original_texts, original_bindings = list(G.TYPED_TEXTS), list(G.TYPED_BIND)
        G.typed_mod, G.TYPED = T, {('root', name)}
        G.TYPED_TEXTS[:], G.TYPED_BIND[:] = [], []
        T.NUMERIC_SPRITE_LOOPS = True
        candidate = G.generate_function(dw, name, k, report)
        regions = list(G.TYPED_TEXTS)
        if original is None or candidate is None:
            raise ValueError('Entry was not generated')
        if not all(t[0] == name and t[1] == k for t in regions):
            raise ValueError('Unexpected region family')
        result = late_copy(original, candidate, regions, k)
        keep_texts, keep_bindings = original_texts, original_bindings
        return result
    finally:
        G.typed_mod, G.TYPED = old_mod, old_selection
        T.NUMERIC_SPRITE_LOOPS = old_flag
        G.TYPED_TEXTS[:], G.TYPED_BIND[:] = keep_texts, keep_bindings
