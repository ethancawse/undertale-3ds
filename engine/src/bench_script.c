#include "bench_script.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gml_array.h"
#include "instance.h"
#include "int_rvalue_hashmap.h"
#include "log.h"
#include "runner_keyboard.h"
#include "stb_ds.h"
#include "utils.h"
#include "vm.h"
#include "vm_builtins.h"

// How long a wait may take before the scene counts as failed, in frames (60 s at 30 fps).
#define BENCH_WAIT_LIMIT 1800

typedef enum {
    BOP_RESTART,
    BOP_WAIT,
    BOP_WAITROOM,
    BOP_WAITOBJ,
    BOP_WAITVAR,
    BOP_GOTO,
    BOP_SET,
    BOP_CALL,
    BOP_CALLAS,
    BOP_CREATE,
    BOP_DESTROY,
    BOP_FILE,
    BOP_KEYS,
    BOP_KEEP,
    BOP_MEASURE,
    BOP_LOG,
    BOP_SHOT,
} OpKind;

static const char* const opNames[] = {
    "restart", "wait", "waitroom", "waitobj", "waitvar", "goto", "set", "call", "callas", "create", "destroy", "file", "keys", "keep",
    "measure", "log", "shot",
};

typedef enum {
    VALUE_NUMBER,
    VALUE_STRING,
    VALUE_NAME, // an asset name, or a function name for call; resolved when first used
    VALUE_REF, // global.name[i] or object.name[i]
} ValueKind;

typedef struct {
    ValueKind kind;
    double number;
    char* text; // VALUE_STRING contents, VALUE_NAME name, VALUE_REF variable name
    // VALUE_REF
    bool global;
    char* objectName;
    int32_t objectIndex; // resolved, -1 until then
    int32_t arrayIndex; // -1 = none
    // VALUE_NAME, resolved
    bool resolved;
    int32_t asset;
} Value;

typedef struct {
    int32_t key;
    int32_t period; // 0 = held
    int32_t hold;
} KeyPattern;

typedef struct Op Op;
struct Op {
    OpKind kind;
    Value* args; // stb_ds array
    KeyPattern* keys; // BOP_KEYS
    Op* inner; // BOP_KEEP
    int32_t line;
};

struct BenchScript {
    Op* ops; // stb_ds array
    int32_t pc;
    Op** keeps; // stb_ds array of pointers into ops
    KeyPattern* keys; // stb_ds array, current pattern
    int32_t* heldKeys; // stb_ds array of keys the script holds down
    int32_t keyFrame;
    int32_t waitFrames; // frames spent in the current op
    bool restartIssued;
    bool gotoIssued;
    bool measuring;
    bool shot; // a "shot" ran: BenchScript_takeShot
    int32_t framesMeasured;
    char* error;
};

// ===[ Parsing ]===

static char* formatString(const char* fmt, ...) {
    char buffer[512];
    va_list va;
    va_start(va, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, va);
    va_end(va);
    return safeStrdup(buffer);
}

static void freeValue(Value* v) {
    free(v->text);
    free(v->objectName);
}

static void freeOp(Op* op) {
    for (ptrdiff_t i = 0; arrlen(op->args) > i; i++) freeValue(&op->args[i]);
    arrfree(op->args);
    arrfree(op->keys);
    if (op->inner != nullptr) {
        freeOp(op->inner);
        free(op->inner);
    }
}

// Reads one token at *p: a quoted string or a run of non-space characters. Returns false at the end of the line.
static bool nextToken(const char** p, char** outText, bool* outQuoted) {
    const char* s = *p;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '\0' || *s == '#') {
        *p = s;
        return false;
    }
    char buffer[1024];
    size_t n = 0;
    if (*s == '"') {
        s++;
        while (*s != '\0' && *s != '"') {
            char c = *s++;
            if (c == '\\' && *s != '\0') {
                c = *s++;
                if (c == 'n') c = '\n';
                else if (c == 'r') c = '\r';
                else if (c == 't') c = '\t';
            }
            if (sizeof(buffer) - 1 > n) buffer[n++] = c;
        }
        if (*s == '"') s++;
        *outQuoted = true;
    } else {
        while (*s != '\0' && *s != ' ' && *s != '\t') {
            if (sizeof(buffer) - 1 > n) buffer[n++] = *s;
            s++;
        }
        *outQuoted = false;
    }
    buffer[n] = '\0';
    *outText = safeStrdup(buffer);
    *p = s;
    return true;
}

static bool parseNumber(const char* text, double* out) {
    char* end = nullptr;
    double v = strtod(text, &end);
    if (end == text || *end != '\0') return false;
    *out = v;
    return true;
}

static Value parseValue(char* text, bool quoted) {
    Value v = {0};
    v.objectIndex = -1;
    v.arrayIndex = -1;
    v.asset = -1;
    if (quoted) {
        v.kind = VALUE_STRING;
        v.text = text;
        return v;
    }
    if (parseNumber(text, &v.number)) {
        v.kind = VALUE_NUMBER;
        free(text);
        return v;
    }
    if (strcmp(text, "true") == 0 || strcmp(text, "false") == 0) {
        v.kind = VALUE_NUMBER;
        v.number = text[0] == 't' ? 1.0 : 0.0;
        free(text);
        return v;
    }
    char* dot = strchr(text, '.');
    if (dot != nullptr && dot != text && dot[1] != '\0') {
        v.kind = VALUE_REF;
        *dot = '\0';
        v.global = strcmp(text, "global") == 0;
        if (!v.global) v.objectName = safeStrdup(text);
        char* name = dot + 1;
        char* bracket = strchr(name, '[');
        if (bracket != nullptr) {
            v.arrayIndex = atoi(bracket + 1);
            *bracket = '\0';
        }
        v.text = safeStrdup(name);
        free(text);
        return v;
    }
    v.kind = VALUE_NAME;
    v.text = text;
    return v;
}

static int32_t keyCodeByName(const char* name) {
    static const struct { const char* name; int32_t code; } keys[] = {
        { "Z", 'Z' }, { "X", 'X' }, { "C", 'C' }, { "ENTER", VK_ENTER }, { "SHIFT", VK_SHIFT }, { "CTRL", VK_CONTROL },
        { "UP", VK_UP }, { "DOWN", VK_DOWN }, { "LEFT", VK_LEFT }, { "RIGHT", VK_RIGHT }, { "ESC", VK_ESCAPE },
    };
    for (size_t i = 0; sizeof(keys) / sizeof(keys[0]) > i; i++) {
        if (strcmp(keys[i].name, name) == 0) return keys[i].code;
    }
    return -1;
}

static bool parseKeyPattern(const char* text, KeyPattern* out) {
    char name[32];
    size_t n = 0;
    while (text[n] != '\0' && text[n] != '/' && sizeof(name) - 1 > n) {
        name[n] = text[n];
        n++;
    }
    name[n] = '\0';
    out->key = keyCodeByName(name);
    out->period = 0;
    out->hold = 2;
    if (text[n] == '/') {
        out->period = atoi(text + n + 1);
        const char* colon = strchr(text + n, ':');
        if (colon != nullptr) out->hold = atoi(colon + 1);
        if (out->hold >= out->period) out->hold = out->period > 1 ? out->period - 1 : 1;
        if (out->hold < 1) out->hold = 1;
        if (out->period < 0) return false;
    }
    return out->key >= 0;
}

static int32_t minArgs(OpKind kind) {
    switch (kind) {
        case BOP_WAIT: case BOP_WAITROOM: case BOP_WAITOBJ: case BOP_GOTO: case BOP_CALL: case BOP_DESTROY: return 1;
        case BOP_WAITVAR: case BOP_SET: case BOP_CALLAS: case BOP_FILE: return 2;
        case BOP_CREATE: return 3;
        default: return 0;
    }
}

// Parses the operation in line (after any "keep"). Returns false and sets *error on a syntax error.
static bool parseOp(const char* line, int32_t lineNumber, Op* out, char** error) {
    memset(out, 0, sizeof(*out));
    out->line = lineNumber;
    const char* p = line;
    char* word = nullptr;
    bool quoted = false;
    if (!nextToken(&p, &word, &quoted)) return false;
    // The battle sweep's own lines (ctr_benchmark.c reads them): not operations.
    if (strcmp(word, "sweep") == 0) {
        free(word);
        return false;
    }
    int32_t kind = -1;
    for (int32_t i = 0; (int32_t) (sizeof(opNames) / sizeof(opNames[0])) > i; i++) {
        if (strcmp(opNames[i], word) == 0) kind = i;
    }
    if (kind < 0) {
        *error = formatString("line %d: unknown operation '%s'", lineNumber, word);
        free(word);
        return false;
    }
    free(word);
    out->kind = (OpKind) kind;
    if (out->kind == BOP_KEEP) {
        out->inner = safeCalloc(1, sizeof(Op));
        if (!parseOp(p, lineNumber, out->inner, error)) {
            if (*error == nullptr) *error = formatString("line %d: keep needs an operation", lineNumber);
            return false;
        }
        if (out->inner->kind != BOP_SET && out->inner->kind != BOP_CALL && out->inner->kind != BOP_CALLAS) {
            *error = formatString("line %d: keep works with set, call and callas", lineNumber);
            return false;
        }
        return true;
    }
    if (out->kind == BOP_LOG) {
        while (*p == ' ' || *p == '\t') p++;
        arrput(out->args, parseValue(safeStrdup(p), true));
        return true;
    }
    if (out->kind == BOP_FILE) {
        // file <name> <text>: the rest of the line after the name is the text, quoted or not.
        char* name = nullptr;
        if (!nextToken(&p, &name, &quoted)) {
            *error = formatString("line %d: file needs a name and contents", lineNumber);
            return false;
        }
        arrput(out->args, parseValue(name, true));
        char* text = nullptr;
        if (!nextToken(&p, &text, &quoted)) text = safeStrdup("");
        if (!quoted && strcmp(text, "lines") == 0) {
            // file <name> lines <value...>: one line per value, "N*value" repeating it N times.
            free(text);
            char* contents = nullptr; // stb_ds char array
            char* item = nullptr;
            while (nextToken(&p, &item, &quoted)) {
                int32_t repeat = 1;
                const char* lineText = item;
                char* star = quoted ? nullptr : strchr(item, '*');
                if (star != nullptr && star != item) {
                    repeat = atoi(item);
                    lineText = star + 1;
                }
                for (int32_t r = 0; repeat > r; r++) {
                    for (const char* c = lineText; *c != '\0'; c++) arrput(contents, *c);
                    arrput(contents, '\r');
                    arrput(contents, '\n');
                }
                free(item);
            }
            arrput(contents, '\0');
            text = safeStrdup(contents);
            arrfree(contents);
        }
        arrput(out->args, parseValue(text, true));
        return true;
    }
    while (nextToken(&p, &word, &quoted)) {
        if (out->kind == BOP_KEYS) {
            KeyPattern k;
            if (!parseKeyPattern(word, &k)) {
                *error = formatString("line %d: bad key '%s'", lineNumber, word);
                free(word);
                return false;
            }
            arrput(out->keys, k);
            free(word);
            continue;
        }
        arrput(out->args, parseValue(word, quoted));
    }
    if (minArgs(out->kind) > arrlen(out->args)) {
        *error = formatString("line %d: %s needs %d argument(s)", lineNumber, opNames[out->kind], minArgs(out->kind));
        return false;
    }
    if (out->kind == BOP_SET && out->args[0].kind != VALUE_REF) {
        *error = formatString("line %d: set needs global.name or object.name", lineNumber);
        return false;
    }
    return true;
}

BenchScript* BenchScript_parse(const char* text, char** error) {
    *error = nullptr;
    BenchScript* script = safeCalloc(1, sizeof(BenchScript));
    const char* s = text;
    int32_t lineNumber = 0;
    while (*s != '\0') {
        size_t length = 0;
        while (s[length] != '\0' && s[length] != '\n') length++;
        const char* end = s + length;
        lineNumber++;
        char* line = safeMalloc(length + 1);
        for (size_t i = 0; length > i; i++) line[i] = s[i];
        line[length] = '\0';
        if (length > 0 && line[length - 1] == '\r') line[length - 1] = '\0';
        Op op;
        if (parseOp(line, lineNumber, &op, error)) {
            arrput(script->ops, op);
        } else {
            freeOp(&op);
            if (*error != nullptr) {
                free(line);
                BenchScript_free(script);
                return nullptr;
            }
        }
        free(line);
        s = *end == '\n' ? end + 1 : end;
    }
    return script;
}

void BenchScript_free(BenchScript* script) {
    if (script == nullptr) return;
    for (ptrdiff_t i = 0; arrlen(script->ops) > i; i++) freeOp(&script->ops[i]);
    arrfree(script->ops);
    arrfree(script->keeps);
    arrfree(script->keys);
    arrfree(script->heldKeys);
    free(script->error);
    free(script);
}

const char* BenchScript_error(const BenchScript* script) {
    return script->error;
}

int32_t BenchScript_framesMeasured(const BenchScript* script) {
    return script->framesMeasured;
}

// ===[ Resolving names ]===

#define FIND_BY_NAME(array, count)                                       \
    for (uint32_t i = 0; (count) > i; i++) {                           \
        if ((array)[i].name != nullptr && strcmp((array)[i].name, name) == 0) return (int32_t) i; \
    }

static int32_t findObject(DataWin* dw, const char* name) {
    FIND_BY_NAME(dw->objt.objects, dw->objt.count);
    return -1;
}

static int32_t findRoom(DataWin* dw, const char* name) {
    FIND_BY_NAME(dw->room.rooms, dw->room.count);
    return -1;
}

static int32_t findAsset(DataWin* dw, const char* name) {
    int32_t i;
    if ((i = findRoom(dw, name)) >= 0) return i;
    if ((i = findObject(dw, name)) >= 0) return i;
    FIND_BY_NAME(dw->sprt.sprites, dw->sprt.count);
    FIND_BY_NAME(dw->sond.sounds, dw->sond.count);
    FIND_BY_NAME(dw->font.fonts, dw->font.count);
    FIND_BY_NAME(dw->bgnd.backgrounds, dw->bgnd.count);
    FIND_BY_NAME(dw->path.paths, dw->path.count);
    FIND_BY_NAME(dw->scpt.scripts, dw->scpt.count);
    return -1;
}

static bool fail(BenchScript* script, const Op* op, const char* fmt, ...) {
    char buffer[384];
    va_list va;
    va_start(va, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, va);
    va_end(va);
    free(script->error);
    script->error = formatString("line %d (%s): %s", op->line, opNames[op->kind], buffer);
    return false;
}

static Instance* firstInstanceOf(Runner* runner, int32_t objectIndex) {
    int32_t base = Runner_pushInstancesOfObject(runner, objectIndex);
    Instance* found = nullptr;
    for (ptrdiff_t i = base; arrlen(runner->instanceSnapshots) > i; i++) {
        Instance* inst = runner->instanceSnapshots[i];
        if (!inst->destroyed) {
            found = inst;
            break;
        }
    }
    Runner_popInstanceSnapshot(runner, base);
    return found;
}

static Instance* anyInstance(Runner* runner) {
    for (ptrdiff_t i = 0; arrlen(runner->instances) > i; i++) {
        Instance* inst = runner->instances[i];
        if (!inst->destroyed && inst->active) return inst;
    }
    return nullptr;
}

static bool resolveRefObject(BenchScript* script, const Op* op, Runner* runner, Value* v) {
    if (v->global || v->objectIndex >= 0) return true;
    v->objectIndex = findObject(runner->dataWin, v->objectName);
    if (v->objectIndex < 0) return fail(script, op, "no object '%s'", v->objectName);
    return true;
}

// ===[ Variables ]===

static RValue readVariable(VMContext* ctx, Instance* inst, const char* name, int32_t arrayIndex) {
    if (inst->objectIndex == STRUCT_OBJECT_INDEX) return RValue_makeIndependent(VM_structGetVariableByVarName(ctx, inst, name, arrayIndex));
    int16_t builtinId = VMBuiltins_resolveBuiltinVarId(name);
    if (builtinId != BUILTIN_VAR_UNKNOWN) return VMBuiltins_getVariable(ctx, inst, builtinId, name, arrayIndex);
    ptrdiff_t slot = shgeti(ctx->varNameMap, (char*) name);
    if (0 > slot) return RValue_makeUndefined();
    RValue* value = IntRValueHashMap_findSlot(&inst->selfVars, ctx->varNameMap[slot].value);
    if (value == nullptr) return RValue_makeUndefined();
    if (arrayIndex >= 0) return RValue_makeIndependent(GMLArray_getOnArrayRef(value, arrayIndex));
    RValue weak = *value;
    weak.ownsReference = false;
    return RValue_makeIndependent(weak);
}

static void writeVariable(VMContext* ctx, Instance* inst, const char* name, int32_t arrayIndex, RValue val) {
    if (inst->objectIndex == STRUCT_OBJECT_INDEX) {
        VM_structSet(ctx, inst, name, val, arrayIndex);
        return;
    }
    int16_t builtinId = VMBuiltins_resolveBuiltinVarId(name);
    if (builtinId != BUILTIN_VAR_UNKNOWN) {
        VMBuiltins_setVariable(ctx, inst, builtinId, name, val, arrayIndex);
        return;
    }
    int32_t varId = VM_getOrAllocateVarID(ctx, name);
    if (arrayIndex >= 0) {
        RValue* slot = IntRValueHashMap_getOrInsertUndefined(&inst->selfVars, varId);
        VM_setArrayElementInSlot(ctx, slot, arrayIndex, val);
    } else {
        Instance_setSelfVar(inst, varId, val);
    }
}

// Evaluates v to an owned RValue. Returns false (with the script's error set) when it can't.
static bool evaluate(BenchScript* script, const Op* op, Runner* runner, Value* v, RValue* out) {
    switch (v->kind) {
        case VALUE_NUMBER: *out = RValue_makeReal((GMLReal) v->number); return true;
        case VALUE_STRING: *out = RValue_makeOwnedString(safeStrdup(v->text)); return true;
        case VALUE_NAME:
            if (!v->resolved) {
                v->asset = findAsset(runner->dataWin, v->text);
                v->resolved = true;
            }
            if (v->asset < 0) return fail(script, op, "no asset named '%s'", v->text);
            *out = RValue_makeReal((GMLReal) v->asset);
            return true;
        case VALUE_REF: {
            VMContext* ctx = runner->vmContext;
            if (v->global) {
                *out = readVariable(ctx, ctx->globalScopeInstance, v->text, v->arrayIndex);
                return true;
            }
            if (!resolveRefObject(script, op, runner, v)) return false;
            Instance* inst = firstInstanceOf(runner, v->objectIndex);
            if (inst == nullptr) return fail(script, op, "no instance of %s", v->objectName);
            *out = readVariable(ctx, inst, v->text, v->arrayIndex);
            return true;
        }
    }
    return false;
}

static bool valuesEqual(RValue a, RValue b) {
    bool aString = a.type == RVALUE_STRING, bString = b.type == RVALUE_STRING;
    if (aString || bString) return aString && bString && strcmp(a.string, b.string) == 0;
    return RValue_toReal(a) == RValue_toReal(b);
}

// ===[ Operations ]===

static bool doSet(BenchScript* script, const Op* op, Runner* runner) {
    VMContext* ctx = runner->vmContext;
    Value* ref = &op->args[0];
    RValue val;
    if (!evaluate(script, op, runner, &op->args[1], &val)) return false;
    if (ref->global) {
        writeVariable(ctx, ctx->globalScopeInstance, ref->text, ref->arrayIndex, val);
    } else {
        if (!resolveRefObject(script, op, runner, ref)) {
            RValue_free(&val);
            return false;
        }
        int32_t base = Runner_pushInstancesOfObject(runner, ref->objectIndex);
        int32_t count = 0;
        for (ptrdiff_t i = base; arrlen(runner->instanceSnapshots) > i; i++) {
            Instance* inst = runner->instanceSnapshots[i];
            if (inst->destroyed) continue;
            writeVariable(ctx, inst, ref->text, ref->arrayIndex, val);
            count++;
        }
        Runner_popInstanceSnapshot(runner, base);
        if (count == 0) {
            RValue_free(&val);
            return fail(script, op, "no instance of %s", ref->objectName);
        }
    }
    RValue_free(&val);
    return true;
}

// Calls a script or builtin with args[first..] as instance self.
static bool doCall(BenchScript* script, const Op* op, Runner* runner, Instance* self, int32_t first) {
    VMContext* ctx = runner->vmContext;
    const char* name = op->args[first].text;
    if (op->args[first].kind != VALUE_NAME) return fail(script, op, "expected a function name");
    RValue args[16];
    int32_t argCount = 0;
    for (ptrdiff_t i = first + 1; arrlen(op->args) > i && 16 > argCount; i++) {
        if (!evaluate(script, op, runner, &op->args[i], &args[argCount])) {
            for (int32_t j = 0; argCount > j; j++) RValue_free(&args[j]);
            return false;
        }
        argCount++;
    }
    int32_t codeIndex = -1;
    char prefixed[256];
    snprintf(prefixed, sizeof(prefixed), "gml_Script_%s", name);
    ptrdiff_t idx = shgeti(ctx->codeIndexByName, prefixed);
    if (0 > idx) idx = shgeti(ctx->codeIndexByName, (char*) name);
    if (idx >= 0) codeIndex = ctx->codeIndexByName[idx].value;
    BuiltinFunc builtin = codeIndex < 0 ? VM_findBuiltin(ctx, name) : nullptr;
    bool ok = true;
    if (codeIndex < 0 && builtin == nullptr) {
        ok = fail(script, op, "no script or builtin named '%s'", name);
    } else {
        struct Instance* savedSelf = ctx->currentInstance;
        struct Instance* savedOther = ctx->otherInstance;
        ctx->currentInstance = self;
        ctx->otherInstance = self;
        RValue result = codeIndex >= 0 ? VM_callCodeIndex(ctx, codeIndex, args, argCount) : builtin(ctx, args, argCount);
        RValue_free(&result);
        ctx->currentInstance = savedSelf;
        ctx->otherInstance = savedOther;
    }
    for (int32_t j = 0; argCount > j; j++) RValue_free(&args[j]);
    return ok;
}

typedef enum { STEP_DONE, STEP_BLOCKED, STEP_FAILED } StepResult;

static StepResult runOp(BenchScript* script, Op* op, Runner* runner) {
    DataWin* dw = runner->dataWin;
    switch (op->kind) {
        case BOP_RESTART:
            if (!script->restartIssued) {
                BenchScript_releaseKeys(script, runner);
                runner->pendingRoom = ROOM_RESTARTGAME;
                script->restartIssued = true;
                return STEP_BLOCKED;
            }
            if (runner->pendingRoom == ROOM_RESTARTGAME) return STEP_BLOCKED;
            script->restartIssued = false;
            return STEP_DONE;
        case BOP_WAIT:
            return script->waitFrames >= (int32_t) op->args[0].number ? STEP_DONE : STEP_BLOCKED;
        case BOP_GOTO:
        case BOP_WAITROOM: {
            Value* v = &op->args[0];
            if (!v->resolved) {
                v->asset = v->kind == VALUE_NUMBER ? (int32_t) v->number : findRoom(dw, v->text);
                v->resolved = true;
            }
            if (v->asset < 0 || (uint32_t) v->asset >= dw->room.count) return fail(script, op, "no room '%s'", v->text) ? STEP_DONE : STEP_FAILED;
            if (op->kind == BOP_GOTO && !script->gotoIssued) {
                runner->pendingRoom = v->asset;
                script->gotoIssued = true;
                return STEP_BLOCKED;
            }
            if (runner->currentRoomIndex == v->asset && runner->pendingRoom < 0) {
                script->gotoIssued = false;
                return STEP_DONE;
            }
            return STEP_BLOCKED;
        }
        case BOP_WAITOBJ: {
            Value* v = &op->args[0];
            if (!v->resolved) {
                v->asset = findObject(dw, v->text);
                v->resolved = true;
            }
            if (v->asset < 0) return fail(script, op, "no object '%s'", v->text) ? STEP_DONE : STEP_FAILED;
            return firstInstanceOf(runner, v->asset) != nullptr ? STEP_DONE : STEP_BLOCKED;
        }
        case BOP_WAITVAR: {
            RValue a, b;
            Value* ref = &op->args[0];
            // A missing instance just means "not yet".
            if (ref->kind == VALUE_REF && !ref->global) {
                if (!resolveRefObject(script, op, runner, ref)) return STEP_FAILED;
                if (firstInstanceOf(runner, ref->objectIndex) == nullptr) return STEP_BLOCKED;
            }
            if (!evaluate(script, op, runner, ref, &a)) return STEP_FAILED;
            if (!evaluate(script, op, runner, &op->args[1], &b)) {
                RValue_free(&a);
                return STEP_FAILED;
            }
            bool equal = valuesEqual(a, b);
            RValue_free(&a);
            RValue_free(&b);
            return equal ? STEP_DONE : STEP_BLOCKED;
        }
        case BOP_SET: return doSet(script, op, runner) ? STEP_DONE : STEP_FAILED;
        case BOP_CALL: {
            Instance* self = anyInstance(runner);
            if (self == nullptr) self = runner->vmContext->globalScopeInstance;
            return doCall(script, op, runner, self, 0) ? STEP_DONE : STEP_FAILED;
        }
        case BOP_CALLAS: {
            Value* v = &op->args[0];
            if (!v->resolved) {
                v->asset = findObject(dw, v->text);
                v->resolved = true;
            }
            if (v->asset < 0) return fail(script, op, "no object '%s'", v->text) ? STEP_DONE : STEP_FAILED;
            Instance* self = firstInstanceOf(runner, v->asset);
            if (self == nullptr) return fail(script, op, "no instance of %s", v->text) ? STEP_DONE : STEP_FAILED;
            return doCall(script, op, runner, self, 1) ? STEP_DONE : STEP_FAILED;
        }
        case BOP_CREATE: {
            Value* v = &op->args[0];
            if (!v->resolved) {
                v->asset = findObject(dw, v->text);
                v->resolved = true;
            }
            if (v->asset < 0) return fail(script, op, "no object '%s'", v->text) ? STEP_DONE : STEP_FAILED;
            RValue x, y;
            if (!evaluate(script, op, runner, &op->args[1], &x)) return STEP_FAILED;
            if (!evaluate(script, op, runner, &op->args[2], &y)) {
                RValue_free(&x);
                return STEP_FAILED;
            }
            Runner_createInstance(runner, RValue_toReal(x), RValue_toReal(y), v->asset);
            RValue_free(&x);
            RValue_free(&y);
            return STEP_DONE;
        }
        case BOP_DESTROY: {
            Value* v = &op->args[0];
            if (!v->resolved) {
                v->asset = findObject(dw, v->text);
                v->resolved = true;
            }
            if (v->asset < 0) return fail(script, op, "no object '%s'", v->text) ? STEP_DONE : STEP_FAILED;
            int32_t base = Runner_pushInstancesOfObject(runner, v->asset);
            for (ptrdiff_t i = base; arrlen(runner->instanceSnapshots) > i; i++) {
                Instance* inst = runner->instanceSnapshots[i];
                if (!inst->destroyed) Runner_destroyInstance(runner, inst, true);
            }
            Runner_popInstanceSnapshot(runner, base);
            return STEP_DONE;
        }
        case BOP_FILE:
            if (!runner->fileSystem->vtable->writeFileText(runner->fileSystem, op->args[0].text, op->args[1].text)) {
                return fail(script, op, "could not write '%s'", op->args[0].text) ? STEP_DONE : STEP_FAILED;
            }
            return STEP_DONE;
        case BOP_KEYS:
            BenchScript_releaseKeys(script, runner);
            arrsetlen(script->keys, 0);
            for (ptrdiff_t i = 0; arrlen(op->keys) > i; i++) arrput(script->keys, op->keys[i]);
            script->keyFrame = 0;
            return STEP_DONE;
        case BOP_KEEP:
            arrput(script->keeps, op->inner);
            return STEP_DONE;
        case BOP_MEASURE:
            script->measuring = true;
            return STEP_DONE;
        case BOP_LOG:
            logInfo("Bench: %s\n", op->args[0].text);
            return STEP_DONE;
        case BOP_SHOT:
            script->shot = true;
            return STEP_DONE;
    }
    return STEP_DONE;
}

static void setKey(BenchScript* script, Runner* runner, int32_t key, bool down) {
    ptrdiff_t at = -1;
    for (ptrdiff_t i = 0; arrlen(script->heldKeys) > i; i++) {
        if (script->heldKeys[i] == key) at = i;
    }
    if (down && at < 0) {
        RunnerKeyboard_onKeyDown(runner->keyboard, key);
        arrput(script->heldKeys, key);
    } else if (!down && at >= 0) {
        RunnerKeyboard_onKeyUp(runner->keyboard, key);
        arrdelswap(script->heldKeys, at);
    }
}

void BenchScript_releaseKeys(BenchScript* script, Runner* runner) {
    while (arrlen(script->heldKeys) > 0) setKey(script, runner, script->heldKeys[0], false);
}

static void applyKeys(BenchScript* script, Runner* runner) {
    int32_t f = script->keyFrame++;
    for (ptrdiff_t i = 0; arrlen(script->keys) > i; i++) {
        KeyPattern* k = &script->keys[i];
        bool down = k->period == 0 || f % k->period < k->hold;
        // Another pattern for the same key may hold it.
        for (ptrdiff_t j = 0; arrlen(script->keys) > j && !down; j++) {
            if (j != i && script->keys[j].key == k->key) down = script->keys[j].period == 0 || f % script->keys[j].period < script->keys[j].hold;
        }
        setKey(script, runner, k->key, down);
    }
}

bool BenchScript_setKeys(BenchScript* script, Runner* runner, const char* keys) {
    Op op;
    char* error = nullptr;
    char line[256];
    snprintf(line, sizeof(line), "keys %s", keys != nullptr ? keys : "");
    if (!parseOp(line, 0, &op, &error)) {
        free(error);
        freeOp(&op);
        return false;
    }
    runOp(script, &op, runner);
    freeOp(&op);
    return true;
}

bool BenchScript_runNow(BenchScript* script, Runner* runner, const char* line) {
    Op op;
    char* error = nullptr;
    if (!parseOp(line, 0, &op, &error)) {
        if (error != nullptr) logWarn("Bench: '%s': %s\n", line, error);
        free(error);
        freeOp(&op);
        return false;
    }
    bool ok = false;
    if (op.kind == BOP_KEEP || op.kind == BOP_WAIT || op.kind == BOP_WAITROOM || op.kind == BOP_WAITOBJ || op.kind == BOP_WAITVAR ||
        op.kind == BOP_GOTO || op.kind == BOP_RESTART || op.kind == BOP_MEASURE) {
        logWarn("Bench: '%s' can't run at once\n", line);
    } else {
        ok = runOp(script, &op, runner) == STEP_DONE && script->error == nullptr;
        if (script->error != nullptr) {
            logWarn("Bench: '%s': %s\n", line, script->error);
            free(script->error);
            script->error = nullptr;
        }
    }
    freeOp(&op);
    return ok;
}

BenchScriptState BenchScript_tick(BenchScript* script, Runner* runner) {
    if (script->error != nullptr) return BENCH_SCRIPT_FAILED;
    for (ptrdiff_t i = 0; arrlen(script->keeps) > i; i++) {
        // A keep that can't run this frame (its instance is gone between phases, say) is skipped, not fatal.
        runOp(script, script->keeps[i], runner);
        free(script->error);
        script->error = nullptr;
    }
    while (arrlen(script->ops) > script->pc) {
        Op* op = &script->ops[script->pc];
        StepResult r = runOp(script, op, runner);
        if (r == STEP_FAILED || script->error != nullptr) {
            if (script->error == nullptr) fail(script, op, "failed");
            logWarn("Bench: %s\n", script->error);
            BenchScript_releaseKeys(script, runner);
            return BENCH_SCRIPT_FAILED;
        }
        if (r == STEP_BLOCKED) {
            if (++script->waitFrames > BENCH_WAIT_LIMIT && op->kind != BOP_WAIT) {
                fail(script, op, "still waiting after %d frames", BENCH_WAIT_LIMIT);
                logWarn("Bench: %s\n", script->error);
                BenchScript_releaseKeys(script, runner);
                return BENCH_SCRIPT_FAILED;
            }
            break;
        }
        script->waitFrames = 0;
        script->pc++;
    }
    if (script->pc >= arrlen(script->ops)) script->measuring = true;
    applyKeys(script, runner);
    if (script->measuring) script->framesMeasured++;
    return script->measuring ? BENCH_SCRIPT_MEASURING : BENCH_SCRIPT_SETTING_UP;
}

// ===[ Encounters ]===

#define ENCOUNTER_SLOTS 3

// What scr_encountersetup left in the enemy slots: global.monstertype[0..2], then global.monsterinstancetype[0..2].
static void readEncounterSlots(VMContext* ctx, GMLReal* out) {
    for (int32_t i = 0; ENCOUNTER_SLOTS > i; i++) {
        RValue type = readVariable(ctx, ctx->globalScopeInstance, "monstertype", i);
        RValue object = readVariable(ctx, ctx->globalScopeInstance, "monsterinstancetype", i);
        out[i] = RValue_toReal(type);
        // Old cases can name an object the chapter no longer has, which reads as an unset variable.
        out[ENCOUNTER_SLOTS + i] = object.type == RVALUE_UNDEFINED || object.type == RVALUE_STRING ? -1 : RValue_toReal(object);
        RValue_free(&type);
        RValue_free(&object);
    }
}

static void runEncounterSetup(VMContext* ctx, int32_t codeIndex, int32_t encounter) {
    // As the global scope, so that its working variables (xx, yy, i) land on no instance of the game's.
    struct Instance* savedSelf = ctx->currentInstance;
    struct Instance* savedOther = ctx->otherInstance;
    ctx->currentInstance = ctx->globalScopeInstance;
    ctx->otherInstance = ctx->globalScopeInstance;
    RValue arg = RValue_makeReal((GMLReal) encounter);
    RValue result = VM_callCodeIndex(ctx, codeIndex, &arg, 1);
    RValue_free(&result);
    RValue_free(&arg);
    ctx->currentInstance = savedSelf;
    ctx->otherInstance = savedOther;
}

int32_t BenchScript_findEncounter(Runner* runner, int32_t from, int32_t last, char* names, size_t size) {
    if (size > 0) names[0] = '\0';
    VMContext* ctx = runner->vmContext;
    ptrdiff_t idx = shgeti(ctx->codeIndexByName, "gml_Script_scr_encountersetup");
    if (0 > idx) return -1;
    int32_t codeIndex = ctx->codeIndexByName[idx].value;
    // Every case starts from the same defaults (all slots type 1, one object); an id no case takes leaves them.
    GMLReal defaults[2 * ENCOUNTER_SLOTS], slots[2 * ENCOUNTER_SLOTS];
    runEncounterSetup(ctx, codeIndex, INT32_MIN);
    readEncounterSlots(ctx, defaults);
    for (int32_t id = from; last >= id; id++) {
        runEncounterSetup(ctx, codeIndex, id);
        readEncounterSlots(ctx, slots);
        if (memcmp(slots, defaults, sizeof(slots)) == 0) continue;
        DataWin* dw = runner->dataWin;
        bool objectsExist = true;
        for (int32_t i = 0; ENCOUNTER_SLOTS > i; i++) {
            GMLReal object = slots[ENCOUNTER_SLOTS + i];
            if (slots[i] != 0 && !(0 <= object && (GMLReal) dw->objt.count > object)) objectsExist = false;
        }
        if (!objectsExist) {
            // Left over from an earlier chapter: the game can't start it either.
            logInfo("Bench: encounter %ld names an object this chapter doesn't have; left out\n", (long) id);
            continue;
        }
        size_t used = 0;
        for (int32_t i = 0; ENCOUNTER_SLOTS > i && size > used; i++) {
            if (slots[i] == 0) continue; // an empty slot
            int32_t object = (int32_t) slots[ENCOUNTER_SLOTS + i];
            const char* name = dw->objt.objects[object].name != nullptr ? dw->objt.objects[object].name : "?";
            int n = snprintf(names + used, size - used, "%s%s", used > 0 ? "+" : "", name);
            if (0 > n) break;
            used += (size_t) n;
        }
        return id;
    }
    return -1;
}

// ===[ Variable references ]===

struct BenchRef {
    Value value;
};

BenchRef* BenchRef_parse(const char* text) {
    Value v = parseValue(safeStrdup(text), false);
    if (v.kind != VALUE_REF) {
        freeValue(&v);
        return nullptr;
    }
    BenchRef* ref = safeCalloc(1, sizeof(BenchRef));
    ref->value = v;
    return ref;
}

void BenchRef_free(BenchRef* ref) {
    if (ref == nullptr) return;
    freeValue(&ref->value);
    free(ref);
}

// The instance a reference reads, or nullptr (no instance, or no such object).
static Instance* refInstance(BenchRef* ref, Runner* runner) {
    Value* v = &ref->value;
    if (v->global) return runner->vmContext->globalScopeInstance;
    if (v->objectIndex < 0) v->objectIndex = findObject(runner->dataWin, v->objectName);
    return v->objectIndex >= 0 ? firstInstanceOf(runner, v->objectIndex) : nullptr;
}

bool BenchRef_get(BenchRef* ref, Runner* runner, RValue* out) {
    Instance* inst = refInstance(ref, runner);
    if (inst == nullptr) return false;
    *out = readVariable(runner->vmContext, inst, ref->value.text, ref->value.arrayIndex);
    return true;
}

bool BenchRef_getReal(BenchRef* ref, Runner* runner, double* out) {
    RValue v;
    if (!BenchRef_get(ref, runner, &v)) return false;
    bool number = v.type != RVALUE_UNDEFINED && v.type != RVALUE_STRING;
    if (number) *out = (double) RValue_toReal(v);
    RValue_free(&v);
    return number;
}

void BenchRef_format(BenchRef* ref, Runner* runner, char* out, size_t size) {
    RValue v;
    if (!BenchRef_get(ref, runner, &v)) {
        snprintf(out, size, "-");
        return;
    }
    if (v.type == RVALUE_STRING) snprintf(out, size, "%s", v.string != nullptr ? v.string : "");
    else if (v.type == RVALUE_UNDEFINED) snprintf(out, size, "undefined");
    else snprintf(out, size, "%g", (double) RValue_toReal(v));
    RValue_free(&v);
}

bool BenchRef_setReal(BenchRef* ref, Runner* runner, double value) {
    Value* v = &ref->value;
    VMContext* ctx = runner->vmContext;
    if (v->global) {
        writeVariable(ctx, ctx->globalScopeInstance, v->text, v->arrayIndex, RValue_makeReal((GMLReal) value));
        return true;
    }
    if (v->objectIndex < 0) v->objectIndex = findObject(runner->dataWin, v->objectName);
    if (v->objectIndex < 0) return false;
    int32_t base = Runner_pushInstancesOfObject(runner, v->objectIndex);
    int32_t count = 0;
    for (ptrdiff_t i = base; arrlen(runner->instanceSnapshots) > i; i++) {
        Instance* inst = runner->instanceSnapshots[i];
        if (inst->destroyed) continue;
        writeVariable(ctx, inst, v->text, v->arrayIndex, RValue_makeReal((GMLReal) value));
        count++;
    }
    Runner_popInstanceSnapshot(runner, base);
    return count > 0;
}

// ===[ Scene lists ]===

static char* dupRange(const char* s, size_t length) {
    char* copy = safeMalloc(length + 1);
    memcpy(copy, s, length);
    copy[length] = '\0';
    return copy;
}

BenchScene* BenchScene_parseList(const char* text) {
    BenchScene* scenes = nullptr;
    char* body = nullptr; // stb_ds char array of the current scene's script
    const char* s = text;
    while (*s != '\0') {
        const char* end = s;
        while (*end != '\0' && *end != '\n') end++;
        const char* p = s;
        while (*p == ' ' || *p == '\t') p++;
        bool battle = strncmp(p, "battle ", 7) == 0;
        if (strncmp(p, "scene ", 6) == 0 || battle) {
            if (arrlen(scenes) > 0) {
                arrput(body, '\0');
                arrlast(scenes).script = safeStrdup(body);
            }
            arrsetlen(body, 0);
            p += battle ? 7 : 6;
            while (*p == ' ') p++;
            size_t idLength = 0;
            while (p + idLength < end && p[idLength] != ' ' && p[idLength] != '\r') idLength++;
            BenchScene scene = {0};
            scene.battle = battle;
            scene.id = dupRange(p, idLength);
            const char* t = p + idLength;
            while (t < end && *t == ' ') t++;
            size_t titleLength = t < end ? (size_t) (end - t) : 0;
            while (titleLength > 0 && (t[titleLength - 1] == '\r' || t[titleLength - 1] == ' ')) titleLength--;
            scene.title = dupRange(t, titleLength);
            arrput(scenes, scene);
        } else if (arrlen(scenes) > 0 && strncmp(p, "chapter ", 8) == 0) {
            arrlast(scenes).chapter = atoi(p + 8);
        } else if (arrlen(scenes) > 0 && strncmp(p, "include ", 8) == 0) {
            p += 8;
            while (*p == ' ') p++;
            size_t idLength = 0;
            while (p + idLength < end && p[idLength] != ' ' && p[idLength] != '\r') idLength++;
            for (ptrdiff_t i = 0; arrlen(scenes) - 1 > i; i++) {
                if (strlen(scenes[i].id) == idLength && strncmp(scenes[i].id, p, idLength) == 0) {
                    for (const char* c = scenes[i].script; *c != '\0'; c++) arrput(body, *c);
                }
            }
        } else if (arrlen(scenes) > 0) {
            for (const char* c = s; c < end; c++) arrput(body, *c);
            arrput(body, '\n');
        }
        s = *end == '\n' ? end + 1 : end;
    }
    if (arrlen(scenes) > 0) {
        arrput(body, '\0');
        arrlast(scenes).script = safeStrdup(body);
    }
    arrfree(body);
    return scenes;
}

void BenchScene_freeList(BenchScene* scenes) {
    for (ptrdiff_t i = 0; arrlen(scenes) > i; i++) {
        free(scenes[i].id);
        free(scenes[i].title);
        free(scenes[i].script);
    }
    arrfree(scenes);
}

// The old interpreter and its per-frame entry keep their bodies and slots. Observation runs alongside it,
// never performs an operation itself, and needs no extra fields in the existing script object.
// GCC can turn even this if-chain into a jump table in ordinary .rodata. Keep its dispatch in the late body.
static unsigned __attribute__((noinline, optimize("no-jump-tables", "no-tree-switch-conversion"),
        section(".text.late.benchromfs.prewindow"))) benchActivityOp(const Op* op, bool keep, bool seeking) {
    if (op->kind == BOP_RESTART || op->kind == BOP_GOTO) return BENCH_ACTIVITY_WARP;
    if (op->kind == BOP_KEYS) return BENCH_ACTIVITY_INPUT;
    if (op->kind == BOP_SET && keep) return BENCH_ACTIVITY_GUARD;
    if (op->kind == BOP_SET || op->kind == BOP_CALL || op->kind == BOP_CALLAS || op->kind == BOP_CREATE ||
        op->kind == BOP_DESTROY || op->kind == BOP_FILE) return BENCH_ACTIVITY_WRITE;
    if (seeking && (op->kind == BOP_WAIT || op->kind == BOP_WAITROOM || op->kind == BOP_WAITOBJ || op->kind == BOP_WAITVAR))
        return BENCH_ACTIVITY_SEEK;
    return 0;
}

static struct {
    int32_t first;
    bool seeking;
    unsigned flags;
} benchObserved __attribute__((section(".bss.late.benchromfs.prewindow")));

void __attribute__((noinline, section(".text.late.benchromfs.prewindow"))) BenchScript_observeBefore(const BenchScript* script) {
    benchObserved.first = script->pc;
    benchObserved.seeking = !script->measuring;
    benchObserved.flags = arrlen(script->heldKeys) > 0 ? BENCH_ACTIVITY_INPUT : 0;
    // These keeps are the ones actually run by this tick (not new keeps installed later in the tick).
    for (ptrdiff_t i = 0; i < arrlen(script->keeps); i++)
        benchObserved.flags |= benchActivityOp(script->keeps[i], true, benchObserved.seeking);
}

unsigned __attribute__((noinline, section(".text.late.benchromfs.prewindow"))) BenchScript_observeAfter(const BenchScript* script) {
    unsigned flags = benchObserved.flags;
    if (arrlen(script->heldKeys) > 0) flags |= BENCH_ACTIVITY_INPUT;
    // pc ends just past the last completed op, or on the op that blocked. Reading the blocked wait/goto
    // too is intentional: its game frame is still part of the seek/warp. Operations are never re-executed.
    for (int32_t i = benchObserved.first; i <= script->pc && i < arrlen(script->ops); i++) {
        flags |= benchActivityOp(&script->ops[i], false, benchObserved.seeking);
    }
    return flags;
}

bool BenchScript_takeShot(BenchScript* script) {
    bool shot = script->shot;
    script->shot = false;
    return shot;
}
