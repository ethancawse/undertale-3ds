/* Source command, not a GPU command/residency cache.
 * Include after public RoomTile/TexturePageItem types. All outputs use the
 * original Renderer_drawTile float operation order, including clip branches.
 */
#ifndef UTOLD_TILE_PLAN_H
#define UTOLD_TILE_PLAN_H
#ifndef UTP_FN
#define UTP_FN static inline
#endif
typedef struct {
    int32_t valid, tpag;
    float left, top, width, height, x, y, sx, sy;
    uint32_t color;
    float alpha;
} UtpCommand;
_Static_assert(sizeof(UtpCommand)==48,"roomplan source command layout");

UTP_FN UtpCommand utpBuild(const RoomTile* tile,const TexturePageItem* page,
                          int32_t tpag,float offsetX,float offsetY) {
    UtpCommand out={0};
    if (tpag<0) return out;
    int32_t x=tile->sourceX,y=tile->sourceY;
    int32_t w=(int32_t)tile->width,h=(int32_t)tile->height;
    float dx=(float)tile->x+offsetX,dy=(float)tile->y+offsetY;
    if (page->targetX>x) {
        int32_t clip=page->targetX-x;
        dx+=(float)clip*tile->scaleX; w-=clip; x=page->targetX;
    }
    if (page->targetY>y) {
        int32_t clip=page->targetY-y;
        dy+=(float)clip*tile->scaleY; h-=clip; y=page->targetY;
    }
    int32_t right=page->targetX+page->sourceWidth;
    int32_t bottom=page->targetY+page->sourceHeight;
    if (x+w>right) w=right-x;
    if (y+h>bottom) h=bottom-y;
    if (w<=0 || h<=0) return out;
    out.valid=1; out.tpag=tpag;
    out.left=(float)(x-page->targetX); out.top=(float)(y-page->targetY);
    out.width=(float)w; out.height=(float)h;
    out.x=dx; out.y=dy; out.sx=tile->scaleX; out.sy=tile->scaleY;
    out.color=tile->color&0x00ffffff; out.alpha=tile->alpha;
    return out;
}
#endif
