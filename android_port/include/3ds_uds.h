/*
 * The 3DS's local wireless (libctru's UDS service) and the kernel events it
 * signals, as 3ds_port/src/3ds_local.c uses them, on Android
 * (android_port/src/android_uds.c): its sessions and message streams run
 * unchanged over Bluetooth LE (app/java/.../LocalBluetooth.java) instead.
 *
 * A UDS network is a host and the clients that joined it, each with a node
 * id (the host's 1) and everyone's in a bitmask; frames go to a node, and
 * the host's beacon carries the session's application data. Here the host
 * advertises over Bluetooth LE and listens on an L2CAP channel, the clients
 * connect to it, and the host relays between clients - what UDS did itself.
 * Only the calls 3ds_local.c makes, with libctru's names and types.
 */
#ifndef ANDROID_SHIM_3DS_UDS_H
#define ANDROID_SHIM_3DS_UDS_H

/* Included by 3ds.h, after its types. */

#define CPU_TICKS_PER_MSEC (SYSCLOCK_ARM11 / 1000.0)

/* ── Kernel events (svcCreateEvent...) ──────────────────────────────── */
Result svcCreateEvent(Handle *event, ResetType reset);
Result svcSignalEvent(Handle event);
Result svcClearEvent(Handle event);
Result svcCloseHandle(Handle handle);
/* *out: the index of the event that was signalled, -1 on a timeout. */
Result svcWaitSynchronizationN(s32 *out, const Handle *handles, s32 count, bool waitAll, s64 ns);

/* ── UDS ────────────────────────────────────────────────────────────── */
#define UDS_DATAFRAME_MAXSIZE 0x5C6
#define UDS_BROADCAST_NETWORKNODEID 0xFFFF
#define UDS_DEFAULT_RECVBUFSIZE 0x2E30
#define UDS_SENDFLAG_Default 0x01
#define UDS_MAXNODES 16

enum { UDSCONTYPE_Client = 1, UDSCONTYPE_Spectator = 2 };

/* A session: what a scan found, or what we host. */
typedef struct
{
    u8 total_nodes;
    u8 max_nodes;
    /* Where to reach its host: a Bluetooth address and its L2CAP channel. */
    char address[18];
    u16 psm;
    /* The beacon's application data, as udsSetApplicationData gave it (the
     * host's own) or as rebuilt from the advertisement (one scanned). */
    u8 appdata[64];
    u32 appdataSize;
} udsNetworkStruct;

typedef struct
{
    udsNetworkStruct network;
    s8 rssi;
} udsNetworkScanInfo;

typedef struct
{
    u32 status;              /* 3 idle, 6 host, 9 client */
    u16 cur_NetworkNodeID;   /* ours, 0 outside a network */
    u16 node_bitmask;        /* bit id - 1 for every node in it, us too */
    u8 total_nodes;
    u8 max_nodes;
} udsConnectionStatus;

typedef struct
{
    u32 BindNodeID;
    Handle event; /* signalled when a frame comes in */
    bool spectator;
} udsBindContext;

Result udsInit(size_t sharedmem_size, const char *username);
void udsExit(void);
void udsGenerateDefaultNetworkStruct(udsNetworkStruct *network, u32 wlancommID, u8 id8, u8 max_nodes);
/* *networks: malloc'd, the caller frees it. */
Result udsScanBeacons(void *outbuf, size_t maxsize, udsNetworkScanInfo **networks, size_t *total_networks,
                      u32 wlancommID, u8 id8, const u8 *host_macaddress, bool connected);
Result udsGetNetworkStructApplicationData(const udsNetworkStruct *network, void *buf, size_t size,
                                          size_t *actual_size);
Result udsSetApplicationData(const void *buf, size_t size);
Result udsCreateNetwork(const udsNetworkStruct *network, const void *passphrase, size_t passphrase_size,
                        udsBindContext *context, u8 data_channel, u32 recv_buffer_size);
Result udsConnectNetwork(const udsNetworkStruct *network, const void *passphrase, size_t passphrase_size,
                         udsBindContext *context, u16 recv_netnodeid, int connection_type, u8 data_channel,
                         u32 recv_buffer_size);
Result udsDestroyNetwork(void);
Result udsDisconnectNetwork(void);
Result udsUnbind(udsBindContext *context);
Result udsEjectClient(u16 NetworkNodeID);
Result udsSendTo(u16 dst_NetworkNodeID, u8 data_channel, u8 flags, const void *buf, size_t size);
/* Non-blocking: *actual_size 0 when nothing waits. */
Result udsPullPacket(const udsBindContext *context, void *buf, size_t size, size_t *actual_size,
                     u16 *src_NetworkNodeID);
Result udsGetConnectionStatus(udsConnectionStatus *output);
/* Whether the status changed since it was last read (nothing blocks here). */
bool udsWaitConnectionStatusEvent(bool nextEvent, bool wait);

#endif
