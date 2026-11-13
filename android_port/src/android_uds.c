/*
 * The 3DS's local wireless on Android: libctru's UDS calls and kernel events
 * that 3ds_port/src/3ds_local.c makes (include/3ds_uds.h), over Bluetooth LE
 * (app/java/com/cer0/emerald/LocalBluetooth.java, called through JNI from the
 * session's thread). 3ds_local.c - its sessions, merges, message streams and
 * acknowledgements - runs unchanged on top.
 *
 * A session's beacon. On the 3DS its application data is 3ds_local.c's
 * struct AppData: "EM3L", the protocol version, the counter's kind, the host's
 * token and the build's 32-byte id - 44 bytes, more than a Bluetooth LE
 * advertisement carries with the rest (31 in all). It goes on the air as 14:
 * the version, the kind, the token and the build id's first 8 bytes; a scan
 * rebuilds the 44 - with this build's whole id when those 8 bytes are its,
 * with them and zeros otherwise, which 3ds_local.c then finds is another build,
 * as it would the whole id.
 */
#include <3ds.h>
#include <jni.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "3ds_log.h"
#include "3ds_platform.h"

/* ── Kernel events ─────────────────────────────────────────────────────── */
#define EVENT_SLOTS 32

static struct
{
    bool used, signalled;
    ResetType mode;
} sEvents[EVENT_SLOTS];
static pthread_mutex_t sEventLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sEventCond = PTHREAD_COND_INITIALIZER;

/* Handles are slot + 1: 0 is no event. */
static int Slot(Handle h)
{
    return h >= 1 && h <= EVENT_SLOTS && sEvents[h - 1].used ? (int)h - 1 : -1;
}

Result svcCreateEvent(Handle *event, ResetType reset)
{
    pthread_mutex_lock(&sEventLock);
    for (int i = 0; i < EVENT_SLOTS; i++)
        if (!sEvents[i].used)
        {
            sEvents[i].used = true;
            sEvents[i].signalled = false;
            sEvents[i].mode = reset;
            *event = (Handle)(i + 1);
            pthread_mutex_unlock(&sEventLock);
            return 0;
        }
    pthread_mutex_unlock(&sEventLock);
    *event = 0;
    return -1;
}

Result svcSignalEvent(Handle event)
{
    int i;

    pthread_mutex_lock(&sEventLock);
    i = Slot(event);
    if (i >= 0)
    {
        sEvents[i].signalled = true;
        pthread_cond_broadcast(&sEventCond);
    }
    pthread_mutex_unlock(&sEventLock);
    return i >= 0 ? 0 : -1;
}

Result svcClearEvent(Handle event)
{
    int i;

    pthread_mutex_lock(&sEventLock);
    i = Slot(event);
    if (i >= 0)
        sEvents[i].signalled = false;
    pthread_mutex_unlock(&sEventLock);
    return i >= 0 ? 0 : -1;
}

Result svcCloseHandle(Handle handle)
{
    int i;

    pthread_mutex_lock(&sEventLock);
    i = Slot(handle);
    if (i >= 0)
        sEvents[i].used = false;
    pthread_mutex_unlock(&sEventLock);
    return 0;
}

/* Any one of them (waitAll is not used by 3ds_local.c). */
Result svcWaitSynchronizationN(s32 *out, const Handle *handles, s32 count, bool waitAll, s64 ns)
{
    struct timespec until;

    (void)waitAll;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += (time_t)(ns / 1000000000LL);
    until.tv_nsec += (long)(ns % 1000000000LL);
    if (until.tv_nsec >= 1000000000L)
    {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&sEventLock);
    for (;;)
    {
        for (s32 n = 0; n < count; n++)
        {
            int i = Slot(handles[n]);

            if (i >= 0 && sEvents[i].signalled)
            {
                if (sEvents[i].mode == RESET_ONESHOT)
                    sEvents[i].signalled = false;
                *out = n;
                pthread_mutex_unlock(&sEventLock);
                return 0;
            }
        }
        if (pthread_cond_timedwait(&sEventCond, &sEventLock, &until) != 0)
            break;
    }
    pthread_mutex_unlock(&sEventLock);
    *out = -1;
    return 0x09401BFE; /* timed out (libctru's code) */
}

/* ── JNI ───────────────────────────────────────────────────────────────── */
static JavaVM *sVm;
static jclass sClass;
static jmethodID sStart, sStop, sScan, sHost, sSetAppData, sConnect, sLeave, sStatus, sSend, sPull, sEject;
static Handle sBindEvent;
static bool sStatusPending;

static JNIEnv *Env(void)
{
    JNIEnv *env = NULL;

    if (sVm == NULL)
        return NULL;
    if ((*sVm)->GetEnv(sVm, (void **)&env, JNI_VERSION_1_6) == JNI_OK)
        return env;
    /* The session's thread, attached once and for good. */
    if ((*sVm)->AttachCurrentThread(sVm, &env, NULL) != JNI_OK)
        return NULL;
    return env;
}

static void JNICALL NativeWake(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    if (sBindEvent)
        svcSignalEvent(sBindEvent);
}

/* From AndroidMain_OnLoad, on the thread Java loaded the library on. */
jint AndroidUds_OnLoad(JavaVM *vm, JNIEnv *env)
{
    static const JNINativeMethod methods[] = {{"nativeWake", "()V", (void *)NativeWake}};
    jclass cls = (*env)->FindClass(env, "com/cer0/emerald/LocalBluetooth");

    if (cls == NULL)
        return JNI_ERR;
    sVm = vm;
    sClass = (*env)->NewGlobalRef(env, cls);
    (*env)->DeleteLocalRef(env, cls);
    if ((*env)->RegisterNatives(env, sClass, methods, 1) != 0)
        return JNI_ERR;
    sStart = (*env)->GetStaticMethodID(env, sClass, "start", "()Z");
    sStop = (*env)->GetStaticMethodID(env, sClass, "stop", "()V");
    sScan = (*env)->GetStaticMethodID(env, sClass, "scan", "(I)[[B");
    sHost = (*env)->GetStaticMethodID(env, sClass, "host", "(I[B)Z");
    sSetAppData = (*env)->GetStaticMethodID(env, sClass, "setAppData", "([B)V");
    sConnect = (*env)->GetStaticMethodID(env, sClass, "connect", "(Ljava/lang/String;I)Z");
    sLeave = (*env)->GetStaticMethodID(env, sClass, "leave", "()V");
    sStatus = (*env)->GetStaticMethodID(env, sClass, "status", "()[I");
    sSend = (*env)->GetStaticMethodID(env, sClass, "send", "(I[B)Z");
    sPull = (*env)->GetStaticMethodID(env, sClass, "pull", "()[B");
    sEject = (*env)->GetStaticMethodID(env, sClass, "eject", "(I)Z");
    if (!sStart || !sStop || !sScan || !sHost || !sSetAppData || !sConnect || !sLeave || !sStatus || !sSend
     || !sPull || !sEject)
        return JNI_ERR;
    return JNI_VERSION_1_6;
}

static void ClearException(JNIEnv *env, const char *what)
{
    if ((*env)->ExceptionCheck(env))
    {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        CtrLog_Write(CTR_LOG_ERROR, "local: %s threw", what);
    }
}

static bool CallBool(jmethodID m, const char *what)
{
    JNIEnv *env = Env();
    jboolean r;

    if (env == NULL)
        return false;
    r = (*env)->CallStaticBooleanMethod(env, sClass, m);
    ClearException(env, what);
    return r;
}

static void CallVoid(jmethodID m, const char *what)
{
    JNIEnv *env = Env();

    if (env == NULL)
        return;
    (*env)->CallStaticVoidMethod(env, sClass, m);
    ClearException(env, what);
}

/* ── The beacon ────────────────────────────────────────────────────────── */
/* 3ds_local.c's struct AppData, as it puts it in the beacon. */
struct AppData
{
    char magic[4];
    uint8_t version;
    uint8_t kind;
    uint8_t reserved[2];
    uint32_t token;
    uint8_t build[32];
};
_Static_assert(sizeof(struct AppData) == 44, "3ds_local.c's beacon data is 44 bytes");

#define COMPACT_SIZE 14 /* version, kind, token, build[0..7] */
#define BUILD_ID_PATH "engine/build_id.bin"

static uint8_t sBuildId[32];
static bool sBuildRead;

static const uint8_t *BuildId(void)
{
    if (!sBuildRead)
    {
        FILE *file = CtrFs_OpenAsset(BUILD_ID_PATH);

        if (file)
        {
            if (fread(sBuildId, 1, sizeof(sBuildId), file) != sizeof(sBuildId))
                memset(sBuildId, 0, sizeof(sBuildId));
            fclose(file);
        }
        sBuildRead = true;
    }
    return sBuildId;
}

static size_t Compact(const void *buf, size_t size, uint8_t out[COMPACT_SIZE])
{
    struct AppData app;

    if (size < sizeof(app))
        return 0;
    memcpy(&app, buf, sizeof(app));
    out[0] = app.version;
    out[1] = app.kind;
    memcpy(out + 2, &app.token, 4);
    memcpy(out + 6, app.build, 8);
    return COMPACT_SIZE;
}

static void Expand(const uint8_t *compact, udsNetworkStruct *net)
{
    struct AppData app;

    memset(&app, 0, sizeof(app));
    memcpy(app.magic, "EM3L", 4);
    app.version = compact[0];
    app.kind = compact[1];
    memcpy(&app.token, compact + 2, 4);
    if (memcmp(compact + 6, BuildId(), 8) == 0)
        memcpy(app.build, BuildId(), sizeof(app.build));
    else
        memcpy(app.build, compact + 6, 8);
    memcpy(net->appdata, &app, sizeof(app));
    net->appdataSize = sizeof(app);
}

/* ── UDS ───────────────────────────────────────────────────────────────── */
#define UDS_NOT_SENT ((Result)0xC86113F0) /* what 3ds_local.c lets pass */

Result udsInit(size_t sharedmem_size, const char *username)
{
    (void)sharedmem_size; (void)username;
    BuildId();
    return CallBool(sStart, "start") ? 0 : -1;
}

void udsExit(void)
{
    CallVoid(sStop, "stop");
}

void udsGenerateDefaultNetworkStruct(udsNetworkStruct *network, u32 wlancommID, u8 id8, u8 max_nodes)
{
    (void)wlancommID; (void)id8;
    memset(network, 0, sizeof(*network));
    network->max_nodes = max_nodes;
    network->total_nodes = 1;
}

Result udsScanBeacons(void *outbuf, size_t maxsize, udsNetworkScanInfo **networks, size_t *total_networks,
                      u32 wlancommID, u8 id8, const u8 *host_macaddress, bool connected)
{
    JNIEnv *env = Env();
    jobjectArray found;
    jsize count;
    udsNetworkScanInfo *list;
    size_t n = 0;

    (void)outbuf; (void)maxsize; (void)wlancommID; (void)id8; (void)host_macaddress; (void)connected;
    *networks = NULL;
    *total_networks = 0;
    if (env == NULL)
        return -1;
    found = (*env)->CallStaticObjectMethod(env, sClass, sScan, (jint)1500);
    ClearException(env, "scan");
    if (found == NULL)
        return -1;
    count = (*env)->GetArrayLength(env, found);
    list = calloc(count > 0 ? (size_t)count : 1, sizeof(*list));
    if (list == NULL)
        return -1;
    for (jsize i = 0; i < count; i++)
    {
        jbyteArray rec = (*env)->GetObjectArrayElement(env, found, i);
        jsize len = rec ? (*env)->GetArrayLength(env, rec) : 0;
        uint8_t data[64];

        /* address (17), "E3", psm (2), total, max, the compact data */
        if (len >= 17 + 6 + COMPACT_SIZE && len <= (jsize)sizeof(data))
        {
            udsNetworkStruct *net = &list[n].network;

            (*env)->GetByteArrayRegion(env, rec, 0, len, (jbyte *)data);
            memcpy(net->address, data, 17);
            net->address[17] = 0;
            net->psm = (u16)(data[19] | (data[20] << 8));
            net->total_nodes = data[21];
            net->max_nodes = data[22];
            Expand(data + 23, net);
            n++;
        }
        if (rec)
            (*env)->DeleteLocalRef(env, rec);
    }
    (*env)->DeleteLocalRef(env, found);
    *networks = list;
    *total_networks = n;
    return 0;
}

Result udsGetNetworkStructApplicationData(const udsNetworkStruct *network, void *buf, size_t size,
                                          size_t *actual_size)
{
    size_t n = network->appdataSize < size ? network->appdataSize : size;

    memcpy(buf, network->appdata, n);
    *actual_size = network->appdataSize;
    return network->appdataSize ? 0 : -1;
}

Result udsSetApplicationData(const void *buf, size_t size)
{
    JNIEnv *env = Env();
    uint8_t compact[COMPACT_SIZE];
    jbyteArray data;

    if (env == NULL || Compact(buf, size, compact) == 0)
        return -1;
    data = (*env)->NewByteArray(env, COMPACT_SIZE);
    (*env)->SetByteArrayRegion(env, data, 0, COMPACT_SIZE, (const jbyte *)compact);
    (*env)->CallStaticVoidMethod(env, sClass, sSetAppData, data);
    ClearException(env, "setAppData");
    (*env)->DeleteLocalRef(env, data);
    return 0;
}

static Result Bind(udsBindContext *context)
{
    memset(context, 0, sizeof(*context));
    if (R_FAILED(svcCreateEvent(&context->event, RESET_ONESHOT)))
        return -1;
    sBindEvent = context->event;
    sStatusPending = true;
    return 0;
}

Result udsCreateNetwork(const udsNetworkStruct *network, const void *passphrase, size_t passphrase_size,
                        udsBindContext *context, u8 data_channel, u32 recv_buffer_size)
{
    JNIEnv *env = Env();
    jbyteArray empty;
    jboolean ok;

    (void)passphrase; (void)passphrase_size; (void)data_channel; (void)recv_buffer_size;
    if (env == NULL)
        return -1;
    empty = (*env)->NewByteArray(env, 0);
    ok = (*env)->CallStaticBooleanMethod(env, sClass, sHost, (jint)network->max_nodes, empty);
    ClearException(env, "host");
    (*env)->DeleteLocalRef(env, empty);
    if (!ok)
        return -1;
    return Bind(context);
}

Result udsConnectNetwork(const udsNetworkStruct *network, const void *passphrase, size_t passphrase_size,
                         udsBindContext *context, u16 recv_netnodeid, int connection_type, u8 data_channel,
                         u32 recv_buffer_size)
{
    JNIEnv *env = Env();
    jstring address;
    jboolean ok;

    (void)passphrase; (void)passphrase_size; (void)recv_netnodeid; (void)connection_type; (void)data_channel;
    (void)recv_buffer_size;
    if (env == NULL)
        return -1;
    address = (*env)->NewStringUTF(env, network->address);
    ok = (*env)->CallStaticBooleanMethod(env, sClass, sConnect, address, (jint)network->psm);
    ClearException(env, "connect");
    (*env)->DeleteLocalRef(env, address);
    if (!ok)
        return -1;
    return Bind(context);
}

Result udsDestroyNetwork(void)
{
    CallVoid(sLeave, "leave");
    return 0;
}

Result udsDisconnectNetwork(void)
{
    CallVoid(sLeave, "leave");
    return 0;
}

Result udsUnbind(udsBindContext *context)
{
    if (context->event)
    {
        if (sBindEvent == context->event)
            sBindEvent = 0;
        svcCloseHandle(context->event);
        context->event = 0;
    }
    return 0;
}

Result udsEjectClient(u16 NetworkNodeID)
{
    JNIEnv *env = Env();
    jboolean ok;

    if (env == NULL)
        return -1;
    ok = (*env)->CallStaticBooleanMethod(env, sClass, sEject, (jint)NetworkNodeID);
    ClearException(env, "eject");
    return ok ? 0 : -1;
}

Result udsSendTo(u16 dst_NetworkNodeID, u8 data_channel, u8 flags, const void *buf, size_t size)
{
    JNIEnv *env = Env();
    jbyteArray data;
    jboolean ok;

    (void)data_channel; (void)flags;
    if (env == NULL)
        return UDS_NOT_SENT;
    data = (*env)->NewByteArray(env, (jsize)size);
    (*env)->SetByteArrayRegion(env, data, 0, (jsize)size, (const jbyte *)buf);
    ok = (*env)->CallStaticBooleanMethod(env, sClass, sSend, (jint)dst_NetworkNodeID, data);
    ClearException(env, "send");
    (*env)->DeleteLocalRef(env, data);
    return ok ? 0 : UDS_NOT_SENT;
}

Result udsPullPacket(const udsBindContext *context, void *buf, size_t size, size_t *actual_size,
                     u16 *src_NetworkNodeID)
{
    JNIEnv *env = Env();
    jbyteArray item;
    jsize len;

    (void)context;
    *actual_size = 0;
    if (env == NULL)
        return -1;
    item = (*env)->CallStaticObjectMethod(env, sClass, sPull);
    ClearException(env, "pull");
    if (item == NULL)
        return 0;
    len = (*env)->GetArrayLength(env, item);
    if (len >= 1)
    {
        uint8_t from;
        size_t frame = (size_t)len - 1;

        if (frame > size)
            frame = size;
        (*env)->GetByteArrayRegion(env, item, 0, 1, (jbyte *)&from);
        (*env)->GetByteArrayRegion(env, item, 1, (jsize)frame, (jbyte *)buf);
        *src_NetworkNodeID = from;
        *actual_size = frame;
    }
    (*env)->DeleteLocalRef(env, item);
    return 0;
}

Result udsGetConnectionStatus(udsConnectionStatus *output)
{
    JNIEnv *env = Env();
    jintArray st;
    jint v[5];

    memset(output, 0, sizeof(*output));
    if (env == NULL)
        return -1;
    st = (*env)->CallStaticObjectMethod(env, sClass, sStatus);
    ClearException(env, "status");
    if (st == NULL)
        return -1;
    (*env)->GetIntArrayRegion(env, st, 0, 5, v);
    (*env)->DeleteLocalRef(env, st);
    output->status = (u32)v[0];
    output->cur_NetworkNodeID = (u16)v[1];
    output->node_bitmask = (u16)v[2];
    output->total_nodes = (u8)v[3];
    output->max_nodes = UDS_MAXNODES;
    if (v[4])
        sStatusPending = true;
    return 0;
}

bool udsWaitConnectionStatusEvent(bool nextEvent, bool wait)
{
    udsConnectionStatus st;
    bool changed;

    (void)nextEvent; (void)wait;
    udsGetConnectionStatus(&st); /* takes Java's "changed" into sStatusPending */
    changed = sStatusPending;
    sStatusPending = false;
    return changed;
}
