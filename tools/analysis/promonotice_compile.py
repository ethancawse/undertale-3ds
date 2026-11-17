"""Change only screenshot-notice operands in the frozen UNDERTALE frame body.

The original compile pass is unchanged. Its two C dirty assignments compile to
three ARM stores (success/failure/expiry). NOPs retain their instruction slots.
The existing game-over guard remains first; only its terminal delegation goes
to the new notice dispatcher. No other dirty writer or configure caller changes.
"""
import re


def route(text):
    name = 'CtrRenderer_beginFrame'
    scopes = list(re.finditer(r'(?ms)^' + name + r':\r?\n.*?^\s*\.size\s+' + name +
                             r',\s*\.-' + name + r'\s*$', text))
    assert len(scopes) == 1, ('promonotice frame scope', len(scopes))
    scope = scopes[0]
    body = scope[0]
    # offsetof(displayDirty) == 12288 + 540, asserted by the helper pass.
    # Check the field base, read, three stores and source lines independently.
    assert len(re.findall(r'(?m)^\s*add\s+r7, r5, #12288\s*$', body)) >= 1
    assert len(re.findall(r'(?m)^\s*ldrb\s+r3, \[r7, #540\]', body)) == 1
    stores = list(re.finditer(r'(?m)^([ \t]+)strb\s+(r9|r3), \[r7, #540\]([^\n]*)$', body))
    assert len(stores) == 3, ('promonotice dirty stores', len(stores))
    lines = []
    for store in stores:
        locations = re.findall(r'(?m)^\s*\.loc\s+(\d+)\s+(\d+)\s+\d+[^\n]*$', body[:store.start()])
        assert locations, 'promonotice store has no source location'
        file, line = map(int, locations[-1])
        assert file == 2 and line in (18228, 18232), ('promonotice non-notice dirty store', file, line)
        lines.append(line)
    assert sorted(lines) == [18228, 18228, 18232], ('promonotice source sites', lines)
    for store in reversed(stores):
        body = body[:store.start()] + store[1] + 'nop\t@ promonotice: keep real displayDirty' + body[store.end():]
    calls = re.findall(r'(?m)^\s*bl\s+CtrGameover_configure(?:\(PLT\))?\s*$', body)
    assert len(calls) == 1, ('promonotice existing game-over guard', len(calls))
    return text[:scope.start()] + body + text[scope.end():], dict(stores=3, warm_calls_changed=0,
                                                               gameover_guard_calls=1, source_lines=lines)


def route_configure(text):
    name = 'CtrGameover_configure'
    scopes = list(re.finditer(r'(?ms)^' + name + r':\r?\n.*?^\s*\.size\s+' + name +
                             r',\s*\.-' + name + r'\s*$', text))
    assert len(scopes) == 1, ('promonotice guarded delegation scope', len(scopes))
    scope = scopes[0]
    body, calls = re.subn(r'(?m)^([ \t]+b(?:l)?[ \t]+)configureScreens(\(PLT\))?([ \t]*(?:@[^\n]*)?)$',
                         lambda m: m[1]+'CtrPromonotice_configure'+(m[2] or '')+m[3], scope[0])
    assert calls == 1, ('promonotice guarded delegation', calls)
    return text[:scope.start()] + body + text[scope.end():]
