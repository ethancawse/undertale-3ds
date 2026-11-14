Late linker input order lives in `late-order.json`. Its four ordered lists mirror
the selectors in `.late.text`, `.late.rodata`, `.late.data` and `.late.bss` in
`platform/3ds/late_reserve.ld`, including object selectors and `KEEP(...)` wrappers.
The registry is for checking only; it never generates or changes product inputs.
Its `aac_text` and `aac_rodata` lists also record the ordered exact input names
in `platform/3ds/aacout.cmake`. These retired AAC slots are UNDERTALE-only;
DELTARUNE retains its decoder. Both model builds use the same ordered lists.

When appending a late section, add its exact selector to the appropriate JSON list
and to the linker fragment in merge order. Text appends go before the final repack
test pair. Keep existing selectors and their order intact. No test tail lists need
editing: they take slices of this registry through `late_order.inputs()`.
For an AAC-slot append, update the appropriate `aac_*` registry list and its
CMake list instead. The GOG test reads these from the same registry too.
AAC registry lists contain production inputs only. The emitter appends the
test-only `.aacout.<kind>.aacslot_test` fixture after every production input;
the fixture does not consume a registry entry or release capacity.

Run `py -3 -I tools/analysis/late_order.py` for the central check, or
`py -3 -I tools/analysis/test_late_order.py` for the check and negative controls.
The first mismatch in each region names the registry, its input number, and both
selectors. A missing entry should be fixed there, rather than in each consumer.

Feature-specific tests retain their own section attributes, anchors, retention
requirements, and native behavior checks. Only their growing order lists come
from the registry. The reader's filtered views preserve existing tests' handling
of object selectors and `KEEP(...)`. New tests should normally use the complete
selector view. The central check compares every selector without filtering.

Its `devonly` key is the dev-only contract (devdiet, 11-11). UNDERTALE builds
without the release source URL move labguard (`z_labguard.o`), the pen log's own
pairs and bsrdev out of late text into dev-only AAC inputs after every production
input: `aac_text` and `aac_rodata` are aacout.cmake's `aacout_devonly_*` lists,
`late_removed` the late selectors CMakeLists.txt drops from that build's copy of
the script (each exactly once in late_reserve.ld), `late_replaced` the one it puts
back narrowed, and `p51_pad` the placeholder for P5-1 (release-only), whose bytes
must equal P5-1's two inputs. Release scripts are unchanged. `late_order.py`
checks all four against CMake; `test_devonly_aac.py` covers the emitter, the link
check and the budgets, and `check_devonly_aac.py` proves a dev link against its
release and the previous dev build.
