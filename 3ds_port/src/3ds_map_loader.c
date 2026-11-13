/*
 * ARM11 map payload resolver.
 *
 * `gen_map_data.py` replaces every `.incbin "data/layouts/<map>/map.bin"` with a
 * 12-byte descriptor {offset, size, slot} and writes all payloads to a single
 * `maps/layouts.bin`. The game still sees a pointer to the descriptor, so the
 * resolver maps it to the real tile data.
 *
 * The whole payload file is 0.7 MiB, so it stays resident: a resolved map
 * pointer remains valid for the entire session and map changes free nothing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port_platform.h"
#include "port_log.h"
#include "3ds_assets.h"
#include "3ds_data.h"

#define MAP_PAYLOAD_PATH "maps/layouts.bin"

struct MapAsset
{
    u32 offset;
    u32 size;
    u32 slot;
};

extern const struct MapAsset gPortMapAssetsStart[], gPortMapAssetsEnd[];

static u8 *sPayload;
static u32 sPayloadSize;
static bool sTried;

bool CtrMaps_Init(void)
{
    FILE *file;
    long size;

    if (sTried)
        return sPayload != NULL;
    sTried = true;

    file = CtrData_Open(MAP_PAYLOAD_PATH);
    if (file == NULL)
    {
        PORT_LOG("[ERROR] map payloads missing: %s\n", MAP_PAYLOAD_PATH);
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) <= 0
     || fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        PORT_LOG("[ERROR] map payload file unreadable\n");
        return false;
    }
    sPayload = malloc((size_t)size);
    if (sPayload == NULL || fread(sPayload, 1, (size_t)size, file) != (size_t)size)
    {
        free(sPayload);
        sPayload = NULL;
        fclose(file);
        PORT_LOG("[ERROR] map payloads could not be loaded (%ld bytes)\n", size);
        return false;
    }
    fclose(file);
    sPayloadSize = (u32)size;
    PORT_LOG("[FS] maps: %lu KiB resident, %lu descriptors\n",
            (unsigned long)(sPayloadSize >> 10),
            (unsigned long)(gPortMapAssetsEnd - gPortMapAssetsStart));
    return true;
}

u32 CtrMaps_Bytes(void)
{
    return sPayloadSize;
}

const void *Port_ResolveMapAssetPointer(const void *ptr)
{
    const struct MapAsset *asset = ptr;
    uintptr_t addr = (uintptr_t)ptr;

    if (addr < (uintptr_t)gPortMapAssetsStart || addr >= (uintptr_t)gPortMapAssetsEnd)
        return ptr;
    if ((addr - (uintptr_t)gPortMapAssetsStart) % sizeof(*asset) != 0)
        goto fail;
    if (!CtrMaps_Init())
        goto fail;
    if (asset->size == 0 || asset->offset > sPayloadSize
     || asset->size > sPayloadSize - asset->offset)
        goto fail;
    return sPayload + asset->offset;

fail:
    /* A descriptor is not tile data: never continue with it. */
    PORT_LOG("[ERROR] map descriptor %08lX unresolved\n", (unsigned long)addr);
    CtrAssets_Fatal("map payload could not be resolved");
}

/*
 * The map headers, layouts, event tables and connection lists are assembled
 * data (data/maps, gen_map_data.py's layouts), their pointers 4-byte
 * addresses: on a 32-bit build they are the C structures as they are. A
 * 64-bit build's C structures have 8-byte pointers, so each one the game asks
 * for is copied once into a native structure, whose pointers are those
 * addresses widened (the game image is below 4 GiB). Everything the game
 * reads maps through comes from here (overworld.c's accessors), and the event
 * arrays themselves (struct ObjectEventTemplate...) keep their assembled
 * layout: their script members are u32 on every build.
 */
extern const u32 gMapLayouts[];
extern const u32 gMapGroups[];

#if UINTPTR_MAX > 0xFFFFFFFFu
#include <pthread.h>
#include <stddef.h>

struct AssembledMapLayout
{
    s32 width, height;
    u32 border, map, primaryTileset, secondaryTileset;
};

struct AssembledMapEvents
{
    u8 objectEventCount, warpCount, coordEventCount, bgEventCount;
    u32 objectEvents, warps, coordEvents, bgEvents;
};

struct AssembledMapConnections
{
    s32 count;
    u32 connections;
};

struct AssembledMapHeader
{
    u32 mapLayout, events, mapScripts, connections;
    u8 rest[12]; /* music .. battleType: the same 12 bytes in struct MapHeader */
};

_Static_assert(sizeof(struct AssembledMapHeader) == 28, "the assembled map header is 28 bytes");
_Static_assert(offsetof(struct MapHeader, battleType) - offsetof(struct MapHeader, music) == 11,
               "struct MapHeader's members after the pointers are 12 bytes, as assembled");

/* A few hundred of each: Emerald has 518 maps and 442 layouts. */
#define NATIVE_SLOTS 2048

struct NativeSlot
{
    u32 assembled; /* the address it was made from, 0 if free */
    void *native;
};

static pthread_mutex_t sNativeLock = PTHREAD_MUTEX_INITIALIZER;
static struct NativeSlot sNativeIndex[NATIVE_SLOTS];
static struct MapHeader sHeaders[NATIVE_SLOTS / 2];
static struct MapLayout sLayouts[NATIVE_SLOTS / 2];
static struct MapEvents sEvents[NATIVE_SLOTS / 2];
static struct MapConnections sConnections[NATIVE_SLOTS / 2];
static u16 sHeaderCount, sLayoutCount, sEventCount, sConnectionCount;

static void FatalMapData(const char *what)
{
    PORT_LOG("[ERROR] too many %s for the native map table\n", what);
    CtrAssets_Fatal("map data could not be converted");
}

/* The slot for an assembled address, holding its native copy once made. */
static struct NativeSlot *Slot(u32 assembled)
{
    u32 i = (assembled >> 2) * 2654435761u % NATIVE_SLOTS;

    while (sNativeIndex[i].assembled != 0 && sNativeIndex[i].assembled != assembled)
        i = (i + 1) % NATIVE_SLOTS;
    return &sNativeIndex[i];
}

static const struct MapLayout *NativeLayout(u32 assembled)
{
    struct NativeSlot *slot;
    const struct AssembledMapLayout *from = PTR32(assembled);
    struct MapLayout *to;

    if (assembled == 0)
        return NULL;
    slot = Slot(assembled);
    if (slot->native != NULL)
        return slot->native;
    if (sLayoutCount == ARRAY_COUNT(sLayouts))
        FatalMapData("layouts");
    to = &sLayouts[sLayoutCount++];
    to->width = from->width;
    to->height = from->height;
    to->border = PTR32(from->border);
    to->map = PTR32(from->map);
    to->primaryTileset = PTR32(from->primaryTileset);
    to->secondaryTileset = PTR32(from->secondaryTileset);
    slot->assembled = assembled;
    slot->native = to;
    return to;
}

static const struct MapEvents *NativeEvents(u32 assembled)
{
    struct NativeSlot *slot;
    const struct AssembledMapEvents *from = PTR32(assembled);
    struct MapEvents *to;

    if (assembled == 0)
        return NULL;
    slot = Slot(assembled);
    if (slot->native != NULL)
        return slot->native;
    if (sEventCount == ARRAY_COUNT(sEvents))
        FatalMapData("event tables");
    to = &sEvents[sEventCount++];
    to->objectEventCount = from->objectEventCount;
    to->warpCount = from->warpCount;
    to->coordEventCount = from->coordEventCount;
    to->bgEventCount = from->bgEventCount;
    to->objectEvents = PTR32(from->objectEvents);
    to->warps = PTR32(from->warps);
    to->coordEvents = PTR32(from->coordEvents);
    to->bgEvents = PTR32(from->bgEvents);
    slot->assembled = assembled;
    slot->native = to;
    return to;
}

static const struct MapConnections *NativeConnections(u32 assembled)
{
    struct NativeSlot *slot;
    const struct AssembledMapConnections *from = PTR32(assembled);
    struct MapConnections *to;

    if (assembled == 0)
        return NULL;
    slot = Slot(assembled);
    if (slot->native != NULL)
        return slot->native;
    if (sConnectionCount == ARRAY_COUNT(sConnections))
        FatalMapData("connection lists");
    to = &sConnections[sConnectionCount++];
    to->count = from->count;
    to->connections = PTR32(from->connections);
    slot->assembled = assembled;
    slot->native = to;
    return to;
}

static const struct MapHeader *NativeHeader(u32 assembled)
{
    struct NativeSlot *slot;
    const struct AssembledMapHeader *from = PTR32(assembled);
    struct MapHeader *to;

    if (assembled == 0)
        return NULL;
    slot = Slot(assembled);
    if (slot->native != NULL)
        return slot->native;
    if (sHeaderCount == ARRAY_COUNT(sHeaders))
        FatalMapData("headers");
    to = &sHeaders[sHeaderCount++];
    to->mapLayout = NativeLayout(from->mapLayout);
    to->events = NativeEvents(from->events);
    to->mapScripts = PTR32(from->mapScripts);
    to->connections = NativeConnections(from->connections);
    memcpy(&to->music, from->rest, sizeof(from->rest));
    slot->assembled = assembled;
    slot->native = to;
    return to;
}

const struct MapLayout *Port_GetMapLayoutById(u16 mapLayoutId)
{
    const struct MapLayout *layout;

    if (mapLayoutId == 0)
        return NULL;
    pthread_mutex_lock(&sNativeLock);
    layout = NativeLayout(gMapLayouts[mapLayoutId - 1]);
    pthread_mutex_unlock(&sNativeLock);
    return layout;
}

const struct MapHeader *Port_GetMapHeaderByGroupAndId(u16 mapGroup, u16 mapNum)
{
    const struct MapHeader *header;

    pthread_mutex_lock(&sNativeLock);
    header = NativeHeader(((const u32 *)PTR32(gMapGroups[mapGroup]))[mapNum]);
    pthread_mutex_unlock(&sNativeLock);
    return header;
}
#else
const struct MapLayout *Port_GetMapLayoutById(u16 mapLayoutId)
{
    if (mapLayoutId == 0)
        return NULL;
    return PTR32(gMapLayouts[mapLayoutId - 1]);
}

const struct MapHeader *Port_GetMapHeaderByGroupAndId(u16 mapGroup, u16 mapNum)
{
    return PTR32(((const u32 *)PTR32(gMapGroups[mapGroup]))[mapNum]);
}
#endif
