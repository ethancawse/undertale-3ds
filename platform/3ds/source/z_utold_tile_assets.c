/* Roomplan version 2 source commands. Uses the boot check's existing digest;
 * it never hashes data.win again. Missing/bad/stale data means room-load plans.
 * All code/constants/statics of this TU are selected into utold late sections.
 */
#if defined(ENABLE_WAD16) && defined(CTRPORT_UTOLD_TILE_PLAN)
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "data_win.h"
#include "runner.h"
#include "ctr_data_version.h"
#include "ctr_benchromfs.h"
#include "roomplan_format.h"
#include "utold_tile_assets.h"

#define UTP_TEXT __attribute__((noinline,section(".text.late.utold")))
#define UTP_RODATA __attribute__((section(".rodata.late.utold")))
#define UTP_SOURCE_VERSION 2u
#define UTP_SOURCE_MAX (16u*1024u*1024u)
#define UTP_COMMAND_BYTES 48u
#define UTP_RECORD_BYTES 96u

/* Named arrays in a non-merge section are required here. Anonymous
 * SHF_MERGE literals let ld fold existing libctru strings into this TU,
 * removing bytes from earlier late inputs and moving their constants.
 */
static const char utpGame[] UTP_RODATA="undertale";
static const char utpMount[] UTP_RODATA="romfs";
static const char utpSourcePath[] UTP_RODATA="romfs:/roomplan/undertale-source.dtp";
static const char utpReadMode[] UTP_RODATA="rb";
typedef struct {
    int32_t x,y,background,sourceX,sourceY;
    uint32_t width,height;
    int32_t depth;
    uint32_t id;
    float scaleX,scaleY;
    uint32_t color;
} UtpAssetTile;
_Static_assert(sizeof(UtpAssetTile)==48,"source tile record layout");
typedef struct { uint32_t offset,count; } UtpAssetRoom;
static struct {
    char digest[65];
    bool validDigest, checked;
    UtpAssetRoom* rooms;
    uint32_t roomCount;
} assets __attribute__((section(".data.late.utold")));

UTP_TEXT CtrDataVersionResult Utp_dataCheck(const char* path,const char* game,const char* part,
    const char* cachePath,void (*progress)(void),bool* cached,char digest[65]) {
    /* This ordinary call still passes through main's nativemac wrapper. */
    CtrDataVersionResult result=CtrDataVersion_check(path,game,part,cachePath,progress,cached,digest);
    free(assets.rooms);
    memset(&assets,0,sizeof(assets));
    if (result!=CTR_DATA_UNREADABLE && strcmp(game,utpGame)==0 && strlen(digest)==64) {
        memcpy(assets.digest,digest,65);
        assets.validDigest=true;
    }
    return result;
}

static UTP_TEXT bool utpHeader(const RoomPlanHeader* h,DataWin* dw) {
    if (!assets.validDigest || h->magic!=ROOMPLAN_MAGIC || h->version!=UTP_SOURCE_VERSION
        || h->dataWinSize!=dw->fileSize || h->gameId!=dw->gen8.gameID
        || h->timestamp!=dw->gen8.timestamp || h->tileMax!=256 || h->gridVersion!=1
        || h->roomCount!=dw->room.count || h->sceneCount>h->roomCount
        || h->bodySize>UTP_SOURCE_MAX-sizeof(*h)) return false;
    for (uint32_t i=0;i<32;++i) {
        unsigned hi=(unsigned char)assets.digest[i*2],lo=(unsigned char)assets.digest[i*2+1];
        hi=hi>='a' && hi<='f' ? hi-'a'+10 : hi>='0' && hi<='9' ? hi-'0' : 256;
        lo=lo>='a' && lo<='f' ? lo-'a'+10 : lo>='0' && lo<='9' ? lo-'0' : 256;
        if (hi>15 || lo>15 || h->sha256[i]!=(hi*16+lo)) return false;
    }
    return true;
}

static UTP_TEXT void utpIndex(DataWin* dw) {
    assets.checked=true;
    if (!assets.validDigest || dw->room.count==0 || dw->room.count>65536) return;
    CtrRomfs_init(utpMount);
    FILE* f=fopen(utpSourcePath,utpReadMode);
    if (f==NULL) return;
    RoomPlanHeader h;
    UtpAssetRoom* rooms=NULL;
    if (fread(&h,1,sizeof(h),f)!=sizeof(h) || !utpHeader(&h,dw)) goto done;
    uint8_t buffer[512];
    uint32_t hash=2166136261u,remaining=h.bodySize;
    while (remaining>0) {
        uint32_t n=remaining>sizeof(buffer) ? sizeof(buffer) : remaining;
        if (fread(buffer,1,n,f)!=n) goto done;
        for (uint32_t i=0;i<n;++i) hash=(hash^buffer[i])*16777619u;
        remaining-=n;
    }
    /* Keep the EOF/error distinction using already-linked fread. fgetc
     * pulls newlib's fgetc + __srget_r into ordinary .text (320 B), which
     * shifts the original layout and crosses the following 4 KiB boundary.
     */
    if (hash!=h.checksum || fread(buffer,1,1,f)!=0 || ferror(f)) goto done;
    rooms=calloc(h.roomCount,sizeof(*rooms));
    if (rooms==NULL || fseek(f,sizeof(h),SEEK_SET)!=0) goto done;
    uint32_t at=sizeof(h);
    for (uint32_t i=0;i<h.sceneCount;++i) {
        uint32_t entry[2];
        if (at>sizeof(h)+h.bodySize || sizeof(entry)>sizeof(h)+h.bodySize-at
            || fread(entry,1,sizeof(entry),f)!=sizeof(entry)) goto done;
        at+=sizeof(entry);
        uint32_t room=entry[0],count=entry[1];
        if (room>=h.roomCount || count==0 || count>65536 || rooms[room].count!=0
            || count>(sizeof(h)+h.bodySize-at)/UTP_RECORD_BYTES) goto done;
        rooms[room]=(UtpAssetRoom){at,count};
        at+=count*UTP_RECORD_BYTES;
        if (fseek(f,at,SEEK_SET)!=0) goto done;
    }
    if (at!=sizeof(h)+h.bodySize) goto done;
    assets.rooms=rooms;
    assets.roomCount=h.roomCount;
    rooms=NULL;
done:
    free(rooms);
    fclose(f);
}

UTP_TEXT void UtpAssets_bind(Runner* runner,void* commands,uint32_t count) {
    DataWin* dw=runner->dataWin;
    int32_t room=runner->currentRoomIndex;
    if (!assets.checked) utpIndex(dw);
    if (room<0 || (uint32_t)room>=assets.roomCount || assets.rooms[room].count!=count) return;
    FILE* f=fopen(utpSourcePath,utpReadMode);
    if (f==NULL) return;
    if (fseek(f,assets.rooms[room].offset,SEEK_SET)==0) {
        uint8_t record[UTP_RECORD_BYTES];
        uint8_t* dst=commands;
        for (uint32_t i=0;i<count;++i) {
            if (fread(record,1,sizeof(record),f)!=sizeof(record)) break;
            RoomTile* t=&runner->currentRoom->tiles[i];
            UtpAssetTile live={t->x,t->y,t->backgroundDefinition,t->sourceX,t->sourceY,
                t->width,t->height,t->tileDepth,t->instanceID,t->scaleX,t->scaleY,t->color};
            int32_t valid;
            memcpy(&valid,record+sizeof(live),4);
            /* Match Create-event edits and persistent payloads exactly. The
             * builder emits only the integer/unit-scale, alpha=1 subset.
             * Other float operations are performed by the room-load helper.
             */
            if (!t->useSpriteDefinition && t->alpha==1.0f && valid>=0 && valid<=1
                && memcmp(&live,record,sizeof(live))==0)
                memcpy(dst,record+sizeof(live),UTP_COMMAND_BYTES);
            dst+=UTP_COMMAND_BYTES;
        }
    }
    fclose(f);
}
#endif
