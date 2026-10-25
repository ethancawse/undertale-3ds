/* Included at the END of runner.c: Drawable, drawableKey and the original
 * legacyTileVisible predicate remain private to that translation unit.
 * Modern tilemaps, tracing and dedicated tile backends use the original path.
 */
#include "utold_tile_plan.h"
#include "utold_tile_assets.h"

#if defined(__3DS__)
#define UTP_TEXT __attribute__((noinline,section(".text.late.utold")))
#define UTP_STATE __attribute__((section(".data.late.utold")))
#else
#define UTP_TEXT
#define UTP_STATE
#endif
#define UTP_FN static UTP_TEXT
#include "utold_tile_command.h"
#undef UTP_FN

/* The current room is the only owner. No pointer into a tile/layer/drawable
 * array survives a call. source/visible/runs are separately owned allocations.
 * Each run ends at a non-tile drawable, so no script can execute inside it.
 */
typedef struct {
    int32_t end;
    uint32_t generation;
    uint32_t selectedCount;
    uint32_t* selected;
    UtpCommand* commands;
    Matrix4f matrix;
} UtpRun;
static struct {
    Runner* owner;
    uint32_t generation;
    UtpRun* runs;
    uint32_t runSlots;
    UtpCommand* source;
    uint32_t sourceCount;
    Matrix4f viewMatrix;
    bool viewKnown;
} utp UTP_STATE;

static UTP_TEXT void utpDropRuns(void) {
    for (uint32_t i=0; i<utp.runSlots; ++i) {
        free(utp.runs[i].commands);
        free(utp.runs[i].selected);
    }
    free(utp.runs);
    utp.runs=NULL;
    utp.runSlots=0;
}

UTP_TEXT void Utp_release(Runner* runner) {
    if (utp.owner!=runner) return;
    utpDropRuns();
    free(utp.source);
    memset(&utp,0,sizeof(utp));
}

UTP_TEXT void Utp_drawablesChanged(Runner* runner) {
    if (utp.owner!=runner) return;
    /* Release before wrap: an old generation can never match a new lifetime. */
    if (++utp.generation==0) {
        utpDropRuns();
        utp.generation=1;
    }
}

UTP_TEXT void Utp_invalidate(Runner* runner) {
    if (utp.owner!=runner) return;
    Utp_drawablesChanged(runner);
    free(utp.source);
    utp.source=NULL;
    utp.sourceCount=0;
}

/* Source construction is cold. Parsed BGNDs are immutable; sprite-backed
 * sources (including surface sprites and reused dynamic TPAGs) are ineligible.
 * Invalid/unresolved sources retain the original helper's no-output result.
 */
static UTP_TEXT bool utpSource(Runner* runner,const RoomTile* tile,
                                float ox,float oy,UtpCommand* command) {
    if (tile->useSpriteDefinition) return false;
    /* Read the immutable legacy BGND directly. Do not add another call-site
     * specialization of the shared sprite/background renderer resolver. */
    int32_t background=tile->backgroundDefinition;
    int32_t tpag=-1;
    if (background>=0 && (uint32_t)background<runner->dataWin->bgnd.count)
        tpag=runner->dataWin->bgnd.backgrounds[background].tpagIndex;
    if (tpag<0) { memset(command,0,sizeof(*command)); return true; }
    if ((uint32_t)tpag>=runner->dataWin->tpag.count) return false;
    *command=utpBuild(tile,&runner->dataWin->tpag.items[tpag],tpag,ox,oy);
    return true;
}

UTP_TEXT void Utp_roomLoaded(Runner* runner) {
    if (utp.owner!=NULL) Utp_release(utp.owner);
    utp.owner=runner;
    utp.generation=1;
    Room* room=runner->currentRoom;
    if (room==NULL || DataWin_isVersionAtLeast(runner->dataWin,2,0,0,0)
        || room->tileCount==0 || room->tileCount>65536) return;
    UtpCommand* commands=calloc(room->tileCount,sizeof(*commands));
    if (commands==NULL) return;
    for (uint32_t i=0;i<room->tileCount;++i) commands[i].valid=-1;
#if defined(__3DS__)
    UtpAssets_bind(runner,commands,room->tileCount);
#endif
    for (uint32_t i=0; i<room->tileCount; ++i) {
        if (commands[i].valid<0 && !utpSource(runner,&room->tiles[i],0,0,&commands[i])) {
            free(commands);
            return;
        }
    }
    utp.source=commands;
    utp.sourceCount=room->tileCount;
}

/* Called once per room draw, including separate views. A changed camera
 * takes the original traversal for the entire draw, with no run allocation
 * or rebuild. After one identical draw the stationary view may build once.
 * Alternating views remain on the original path. Mid-draw matrix changes
 * are checked again at each run and also fall back.
 */
static UTP_TEXT bool utpPrepareDraw(Runner* runner) {
    Renderer* renderer=runner->renderer;
    if (renderer==NULL || renderer->vtable->drawTile!=NULL ||
        DataWin_isVersionAtLeast(runner->dataWin,2,0,0,0)) return false;
    if (utp.owner!=runner) Utp_roomLoaded(runner);
    const Matrix4f* matrix=&renderer->gmlMatrices[MATRIX_WORLD_VIEW_PROJECTION];
    if (!utp.viewKnown || memcmp(&utp.viewMatrix,matrix,sizeof(*matrix))) {
        utp.viewMatrix=*matrix;
        utp.viewKnown=true;
        return false;
    }
    return true;
}

/* This guard is per ordered run, never per tile. The matrix bits are the
 * exact key used by main's culling helper. Renderer target/clip, Smart,
 * palette, residency, recording, and batching remain live below.
 */
static UTP_TEXT int32_t utpDrawRun(Runner* runner,int32_t start) {
    Renderer* renderer=runner->renderer;
    if (renderer==NULL || renderer->vtable->drawTile!=NULL
        || DataWin_isVersionAtLeast(runner->dataWin,2,0,0,0)) return start;
#ifdef ENABLE_VM_TRACING
    if (shlen(runner->vmContext->tilesToBeTraced)>0) return start;
#endif
    if (utp.owner!=runner) Utp_roomLoaded(runner);
    const Matrix4f* matrix=&renderer->gmlMatrices[MATRIX_WORLD_VIEW_PROJECTION];
    if (!utp.viewKnown || memcmp(&utp.viewMatrix,matrix,sizeof(*matrix))) return start;
    uint32_t count=(uint32_t)arrlen(runner->cachedDrawables);
    if (count>131072 || start<0 || (uint32_t)start>=count) return start;
    if (utp.runSlots!=count) {
        utpDropRuns();
        utp.runs=calloc(count,sizeof(*utp.runs));
        if (utp.runs==NULL) return start;
        utp.runSlots=count;
    }
    UtpRun* run=&utp.runs[start];
    if (run->generation!=utp.generation || memcmp(&run->matrix,matrix,sizeof(*matrix))) {
        uint32_t end=(uint32_t)start;
        while (end<count && runner->cachedDrawables[end].type==DRAWABLE_TILE) ++end;
        uint32_t capacity=end-(uint32_t)start;
        UtpCommand* commands=calloc(capacity,sizeof(*commands));
        uint32_t* selected=malloc((size_t)capacity*sizeof(*selected));
        if (commands==NULL || selected==NULL) { free(commands); free(selected); return start; }
        uint32_t n=0;
        bool eligible=true;
        for (uint32_t j=(uint32_t)start;j<end;++j) {
            int32_t index=runner->cachedDrawables[j].tileIndex;
            if (index<0 || (uint32_t)index>=runner->currentRoom->tileCount) { eligible=false; break; }
            RoomTile* tile=&runner->currentRoom->tiles[index];
            ptrdiff_t layer=hmgeti(runner->tileLayerMap,tile->tileDepth);
            if (layer>=0 && !runner->tileLayerMap[layer].value.visible) continue;
            float ox=0,oy=0;
            if (layer>=0) { ox=runner->tileLayerMap[layer].value.offsetX; oy=runner->tileLayerMap[layer].value.offsetY; }
            if (!legacyTileVisible(runner,tile,ox,oy)) continue;
            UtpCommand command;
            if (ox==0 && oy==0 && utp.sourceCount>=(uint32_t)index+1)
                command=utp.source[index];
            else if (!utpSource(runner,tile,ox,oy,&command)) { eligible=false; break; }
            /* Preserve helper no-output behavior without invoking the backend. */
            if (command.valid) { commands[n]=command; selected[n++]=j; }
        }
        if (!eligible) { free(commands); free(selected); return start; }
        free(run->commands); free(run->selected);
        run->commands=commands; run->selected=selected; run->selectedCount=n;
        run->end=(int32_t)end; run->matrix=*matrix; run->generation=utp.generation;
    }
    for (uint32_t j=0;j<run->selectedCount;++j) {
        /* drawDepth is observable by the live backend. It must reflect the
         * actual drawable, and after the run even the final culled tile. */
        runner->drawDepth=runner->cachedDrawables[run->selected[j]].depth;
        UtpCommand* c=&run->commands[j];
        renderer->vtable->drawSpritePart(renderer,c->tpag,c->left,c->top,c->width,c->height,
            c->x,c->y,c->sx,c->sy,0,0,0,c->color,c->alpha);
    }
    return run->end;
}
