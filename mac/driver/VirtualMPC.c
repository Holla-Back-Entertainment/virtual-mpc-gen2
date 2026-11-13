// VirtualMPC.c - "Virtual MPC" AudioServerPlugIn (HAL driver). MIT licence.
//
// Four devices in two pairs, one pair per MPC model. Each pair is a user-facing device (what
// Ableton Live opens) and a hidden "Engine" partner (what Virtual MPC.app opens):
//
//   pair    device                       UID                         in   out
//   XL      Virtual MPC XL               VirtualMPCXL_UID            32   30
//           Virtual MPC XL Engine        VirtualMPCXL_Engine_UID     30   32   (hidden)
//   Live3   Virtual MPC Live III         VirtualMPCLive3_UID         30   28
//           Virtual MPC Live III Engine  VirtualMPCLive3_Engine_UID  28   30   (hidden)
//
// Two rings per pair, indexed by sample time on one shared clock:
//   ringToUser:   Engine output n -> user input n    (the MPC's outs, written by the app)
//   ringToEngine: user output n   -> Engine input n  (Live's outs, read by the app)
// Each ring has exactly one writer device and one reader device, so nothing loops back onto
// itself and the channel counts can differ per direction. The two devices of a pair share one
// time line (anchor host time + rate) and one sample rate: a rate change on either is requested
// on the other too. Hidden devices are not in kAudioHardwarePropertyDevices and cannot be default;
// the app finds the Engine by UID (kAudioHardwarePropertyTranslateUIDToDevice).
//
// Written from scratch against <CoreAudio/AudioServerPlugIn.h>. Object model and timing follow the
// documented AudioServerPlugIn contract (zero time stamps from mach_absolute_time on a fixed period).

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------------------------
// Constants and layout tables

#define kManufacturer       "Henry Skillz"
#define kRingFrames         32768u          // per ring, power of two (> write-ahead + read-behind)
#define kZeroTSPeriod       16384u          // frames between zero time stamps
#define kDefaultRate        44100.0
#define kMaxChannels        32u
#define kNumDevices         4u
#define kNumPairs           2u

static const Float64 kRates[] = { 44100.0, 48000.0 };
#define kNumRates (sizeof(kRates) / sizeof(kRates[0]))

// A run of channels named "<prefix> <first..>".
typedef struct { UInt32 count; const char* prefix; UInt32 first; } Segment;
typedef struct { UInt32 nseg; Segment seg[3]; } Layout;

// User inputs = the MPC's outs. User outputs = Monitor 1/2 then the MPC's ins.
static const Layout kXLOuts    = { 2, { { 8, "MPC Out", 1 }, { 24, "MPC USB Out", 1 } } };
static const Layout kXLIns     = { 3, { { 2, "Monitor", 1 }, { 4, "MPC In", 1 }, { 24, "MPC USB In", 1 } } };
static const Layout kLive3Outs = { 2, { { 6, "MPC Out", 1 }, { 24, "MPC USB Out", 1 } } };
static const Layout kLive3Ins  = { 3, { { 2, "Monitor", 1 }, { 2, "MPC In", 1 }, { 24, "MPC USB In", 1 } } };

static UInt32 LayoutChannels(const Layout* l) {
    UInt32 n = 0;
    for (UInt32 i = 0; i < l->nseg; i++) n += l->seg[i].count;
    return n;
}

// Object IDs: plug-in 1, then per device i: device 2+3i, input stream 3+3i, output stream 4+3i.
enum { kObjectID_PlugIn = kAudioObjectPlugInObject };
#define DevObj(i)       ((AudioObjectID)(2 + 3 * (i)))
#define InStreamObj(i)  ((AudioObjectID)(3 + 3 * (i)))
#define OutStreamObj(i) ((AudioObjectID)(4 + 3 * (i)))
#define kLastObject     OutStreamObj(kNumDevices - 1)

typedef struct {
    Float32* data;              // kRingFrames * ch, interleaved
    Float64* stamp;             // sample time last written into each frame slot
    UInt32   ch;
} Ring;

typedef struct {
    Ring    toUser, toEngine;
    UInt64  anchorHostTime;     // guarded by gIOMutex
    UInt32  ioCount;            // devices of the pair with IO running (sum of their counts)
    UInt64  seed;
} Pair;

typedef struct {
    const char*   name;
    const char*   uid;
    const char*   modelUID;
    const char*   streamIn;
    const char*   streamOut;
    UInt32        pair;
    Boolean       engine;       // hidden partner (app side)
    const Layout* inLayout;     // input channel names
    const Layout* outLayout;    // output channel names
    // state
    Float64       rate;         // guarded by gStateMutex
    Boolean       inActive, outActive;
    UInt32        ioCount;      // guarded by gIOMutex
    Float64       ticksPerFrame;
} Device;

static Device gDev[kNumDevices] = {
    { "Virtual MPC XL",              "VirtualMPCXL_UID",           "VirtualMPCXL_ModelUID",    "MPC XL In",       "MPC XL Out",
      0, false, &kXLOuts,    &kXLIns,    kDefaultRate, true, true, 0, 0 },
    { "Virtual MPC XL Engine",       "VirtualMPCXL_Engine_UID",    "VirtualMPCXL_ModelUID",    "MPC XL Engine In", "MPC XL Engine Out",
      0, true,  &kXLIns,     &kXLOuts,   kDefaultRate, true, true, 0, 0 },
    { "Virtual MPC Live III",        "VirtualMPCLive3_UID",        "VirtualMPCLive3_ModelUID", "MPC Live III In", "MPC Live III Out",
      1, false, &kLive3Outs, &kLive3Ins, kDefaultRate, true, true, 0, 0 },
    { "Virtual MPC Live III Engine", "VirtualMPCLive3_Engine_UID", "VirtualMPCLive3_ModelUID", "MPC Live III Engine In", "MPC Live III Engine Out",
      1, true,  &kLive3Ins,  &kLive3Outs, kDefaultRate, true, true, 0, 0 },
};

static Pair gPair[kNumPairs];

static UInt32 InChannels(const Device* d)  { return LayoutChannels(d->inLayout); }
static UInt32 OutChannels(const Device* d) { return LayoutChannels(d->outLayout); }
static UInt32 Partner(UInt32 i)            { return i ^ 1u; }   // user/engine are adjacent

// The ring a device writes its output into, and the one it reads its input from.
static Ring* WriteRing(UInt32 i) { Pair* p = &gPair[gDev[i].pair]; return gDev[i].engine ? &p->toUser : &p->toEngine; }
static Ring* ReadRing(UInt32 i)  { Pair* p = &gPair[gDev[i].pair]; return gDev[i].engine ? &p->toEngine : &p->toUser; }

// Which device / stream an object ID is. Returns false for the plug-in and unknown IDs.
static Boolean DeviceIndex(AudioObjectID obj, UInt32* outIndex) {
    if (obj < DevObj(0) || obj > kLastObject) return false;
    *outIndex = (obj - 2) / 3;
    return true;
}
static Boolean IsDevice(AudioObjectID obj)    { return obj >= DevObj(0) && obj <= kLastObject && (obj - 2) % 3 == 0; }
static Boolean IsInStream(AudioObjectID obj)  { return obj >= DevObj(0) && obj <= kLastObject && (obj - 2) % 3 == 1; }
static Boolean IsOutStream(AudioObjectID obj) { return obj >= DevObj(0) && obj <= kLastObject && (obj - 2) % 3 == 2; }

// ---------------------------------------------------------------------------------------------
// State

static pthread_mutex_t gStateMutex = PTHREAD_MUTEX_INITIALIZER;   // properties
static pthread_mutex_t gIOMutex    = PTHREAD_MUTEX_INITIALIZER;   // timing + IO state

static AudioServerPlugInHostRef gHost = NULL;
static _Atomic UInt32 gRefCount = 0;

static Float64 HostClockFrequency(void) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return 1.0e9 * (Float64)tb.denom / (Float64)tb.numer;
}

// Only the stamps: a slot whose stamp does not match the read time reads as silence, so the
// audio itself never needs clearing (and its pages are only touched once used).
static void ResetRing(Ring* r) {
    for (UInt32 i = 0; i < kRingFrames; i++) r->stamp[i] = -1.0;
}

static Boolean AllocRing(Ring* r, UInt32 ch) {
    r->ch = ch;
    if (r->data == NULL) r->data = calloc((size_t)kRingFrames * ch, sizeof(Float32));
    if (r->stamp == NULL) r->stamp = malloc(kRingFrames * sizeof(Float64));
    if (r->data == NULL || r->stamp == NULL) return false;
    ResetRing(r);
    return true;
}

static Boolean IsSupportedRate(Float64 r) {
    for (UInt32 i = 0; i < kNumRates; i++) if (r == kRates[i]) return true;
    return false;
}

static void FillASBD(AudioStreamBasicDescription* d, Float64 rate, UInt32 ch) {
    memset(d, 0, sizeof(*d));
    d->mSampleRate       = rate;
    d->mFormatID         = kAudioFormatLinearPCM;
    d->mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    d->mBytesPerPacket   = ch * (UInt32)sizeof(Float32);
    d->mFramesPerPacket  = 1;
    d->mBytesPerFrame    = ch * (UInt32)sizeof(Float32);
    d->mChannelsPerFrame = ch;
    d->mBitsPerChannel   = 32;
}

static Boolean FormatIsValid(const AudioStreamBasicDescription* d, UInt32 ch) {
    return d->mFormatID == kAudioFormatLinearPCM
        && (d->mFormatFlags & kAudioFormatFlagIsFloat)
        && d->mBitsPerChannel == 32
        && d->mChannelsPerFrame == ch
        && d->mBytesPerFrame == ch * sizeof(Float32)
        && d->mFramesPerPacket == 1
        && IsSupportedRate(d->mSampleRate);
}

static CFStringRef CopyElementName(const Device* d, AudioObjectPropertyScope scope, UInt32 element) {
    const Layout* l = scope == kAudioObjectPropertyScopeInput ? d->inLayout
                    : scope == kAudioObjectPropertyScopeOutput ? d->outLayout : NULL;
    if (l == NULL || element < 1) return NULL;
    UInt32 e = element - 1;
    for (UInt32 i = 0; i < l->nseg; i++) {
        if (e < l->seg[i].count) {
            char buf[48];
            snprintf(buf, sizeof(buf), "%s %u", l->seg[i].prefix, (unsigned)(l->seg[i].first + e));
            return CFStringCreateWithCString(NULL, buf, kCFStringEncodingUTF8);
        }
        e -= l->seg[i].count;
    }
    return NULL;
}

// ---------------------------------------------------------------------------------------------
// Forward declarations of the interface

static HRESULT  VMP_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface);
static ULONG    VMP_AddRef(void* inDriver);
static ULONG    VMP_Release(void* inDriver);
static OSStatus VMP_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost);
static OSStatus VMP_CreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo*, AudioObjectID*);
static OSStatus VMP_DestroyDevice(AudioServerPlugInDriverRef, AudioObjectID);
static OSStatus VMP_AddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*);
static OSStatus VMP_RemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*);
static OSStatus VMP_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*);
static OSStatus VMP_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*);
static Boolean  VMP_HasProperty(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*);
static OSStatus VMP_IsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, Boolean*);
static OSStatus VMP_GetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32*);
static OSStatus VMP_GetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, UInt32*, void*);
static OSStatus VMP_SetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, const void*);
static OSStatus VMP_StartIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus VMP_StopIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus VMP_GetZeroTimeStamp(AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64*, UInt64*, UInt64*);
static OSStatus VMP_WillDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, Boolean*, Boolean*);
static OSStatus VMP_BeginIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*);
static OSStatus VMP_DoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*, void*, void*);
static OSStatus VMP_EndIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*);

static AudioServerPlugInDriverInterface gInterface = {
    NULL,
    VMP_QueryInterface, VMP_AddRef, VMP_Release,
    VMP_Initialize, VMP_CreateDevice, VMP_DestroyDevice,
    VMP_AddDeviceClient, VMP_RemoveDeviceClient,
    VMP_PerformDeviceConfigurationChange, VMP_AbortDeviceConfigurationChange,
    VMP_HasProperty, VMP_IsPropertySettable, VMP_GetPropertyDataSize, VMP_GetPropertyData, VMP_SetPropertyData,
    VMP_StartIO, VMP_StopIO, VMP_GetZeroTimeStamp,
    VMP_WillDoIOOperation, VMP_BeginIOOperation, VMP_DoIOOperation, VMP_EndIOOperation,
};
static AudioServerPlugInDriverInterface* gInterfacePtr = &gInterface;
static AudioServerPlugInDriverRef gDriverRef = &gInterfacePtr;

// ---------------------------------------------------------------------------------------------
// Factory (named in Info.plist CFPlugInFactories)

__attribute__((visibility("default")))
void* VirtualMPC_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID);

void* VirtualMPC_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID) {
    (void)inAllocator;
    if (inRequestedTypeUUID == NULL || !CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID)) return NULL;
    return gDriverRef;
}

// ---------------------------------------------------------------------------------------------
// IUnknown

static HRESULT VMP_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface) {
    if (inDriver != gDriverRef || outInterface == NULL) return kAudioHardwareIllegalOperationError;
    CFUUIDRef req = CFUUIDCreateFromUUIDBytes(NULL, inUUID);
    if (req == NULL) return kAudioHardwareIllegalOperationError;
    HRESULT r = E_NOINTERFACE;
    if (CFEqual(req, IUnknownUUID) || CFEqual(req, kAudioServerPlugInDriverInterfaceUUID)) {
        atomic_fetch_add(&gRefCount, 1);
        *outInterface = gDriverRef;
        r = S_OK;
    } else {
        *outInterface = NULL;
    }
    CFRelease(req);
    return r;
}

static ULONG VMP_AddRef(void* inDriver) {
    if (inDriver != gDriverRef) return 0;
    return atomic_fetch_add(&gRefCount, 1) + 1;
}

static ULONG VMP_Release(void* inDriver) {
    if (inDriver != gDriverRef) return 0;
    UInt32 cur = atomic_load(&gRefCount);
    while (cur > 0 && !atomic_compare_exchange_weak(&gRefCount, &cur, cur - 1)) {}
    return cur > 0 ? cur - 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// Basic operations

static OSStatus VMP_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost) {
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    gHost = inHost;
    Float64 hz = HostClockFrequency();
    pthread_mutex_lock(&gIOMutex);
    for (UInt32 i = 0; i < kNumDevices; i++) gDev[i].ticksPerFrame = hz / gDev[i].rate;
    for (UInt32 p = 0; p < kNumPairs; p++) {
        UInt32 user = 2 * p;   // user device of pair p
        if (!AllocRing(&gPair[p].toUser, InChannels(&gDev[user])) ||
            !AllocRing(&gPair[p].toEngine, OutChannels(&gDev[user]))) {
            pthread_mutex_unlock(&gIOMutex);
            return kAudioHardwareUnspecifiedError;
        }
        gPair[p].seed = 1;
    }
    pthread_mutex_unlock(&gIOMutex);
    return kAudioHardwareNoError;
}

static OSStatus VMP_CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc, const AudioServerPlugInClientInfo* ci, AudioObjectID* out) {
    (void)d; (void)desc; (void)ci; (void)out;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus VMP_DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID dev) {
    (void)d; (void)dev;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus VMP_AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID dev, const AudioServerPlugInClientInfo* ci) {
    (void)ci;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    return IsDevice(dev) ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}

static OSStatus VMP_RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID dev, const AudioServerPlugInClientInfo* ci) {
    (void)ci;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    return IsDevice(dev) ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}

// The change action carries the requested sample rate as a whole number of Hz. The partner of the
// device is asked to follow, so both ends of a pair always run at one rate.
static OSStatus VMP_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt64 action, void* info) {
    (void)info;
    UInt32 i;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    if (!IsDevice(dev) || !DeviceIndex(dev, &i)) return kAudioHardwareBadDeviceError;
    Float64 rate = (Float64)action;
    if (!IsSupportedRate(rate)) return kAudioHardwareIllegalOperationError;
    UInt32 other = Partner(i);
    pthread_mutex_lock(&gStateMutex);
    gDev[i].rate = rate;
    Boolean follow = gDev[other].rate != rate;
    pthread_mutex_unlock(&gStateMutex);
    pthread_mutex_lock(&gIOMutex);
    Pair* p = &gPair[gDev[i].pair];
    gDev[i].ticksPerFrame = HostClockFrequency() / rate;
    p->seed++;
    ResetRing(&p->toUser);
    ResetRing(&p->toEngine);
    pthread_mutex_unlock(&gIOMutex);
    if (follow && gHost != NULL) gHost->RequestDeviceConfigurationChange(gHost, DevObj(other), action, NULL);
    return kAudioHardwareNoError;
}

static OSStatus VMP_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt64 action, void* info) {
    (void)action; (void)info;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    return IsDevice(dev) ? kAudioHardwareNoError : kAudioHardwareBadDeviceError;
}

// ---------------------------------------------------------------------------------------------
// Properties. One routine answers both size (outData == NULL) and data queries.

#define NEED(sz) do { if (outData != NULL && inDataSize < (UInt32)(sz)) return kAudioHardwareBadPropertySizeError; } while (0)
#define PUT(type, val) do { NEED(sizeof(type)); if (outData) *(type*)outData = (val); *outSize = (UInt32)sizeof(type); return kAudioHardwareNoError; } while (0)
#define PUTSTR(cstr) do { NEED(sizeof(CFStringRef)); \
    if (outData) *(CFStringRef*)outData = CFStringCreateWithCString(NULL, (cstr), kCFStringEncodingUTF8); \
    *outSize = (UInt32)sizeof(CFStringRef); return kAudioHardwareNoError; } while (0)

static OSStatus PutIDs(const AudioObjectID* ids, UInt32 n, UInt32 inDataSize, UInt32* outSize, void* outData) {
    if (outData != NULL) {
        UInt32 fit = inDataSize / (UInt32)sizeof(AudioObjectID);
        if (fit < n) n = fit;
        memcpy(outData, ids, n * sizeof(AudioObjectID));
    }
    *outSize = n * (UInt32)sizeof(AudioObjectID);
    return kAudioHardwareNoError;
}

static UInt32 DeviceStreams(UInt32 i, AudioObjectPropertyScope scope, AudioObjectID out[2]) {
    UInt32 n = 0;
    if (scope == kAudioObjectPropertyScopeGlobal || scope == kAudioObjectPropertyScopeInput) out[n++] = InStreamObj(i);
    if (scope == kAudioObjectPropertyScopeGlobal || scope == kAudioObjectPropertyScopeOutput) out[n++] = OutStreamObj(i);
    return n;
}

static OSStatus PlugInProp(const AudioObjectPropertyAddress* a, UInt32 qSize, const void* q,
                           UInt32 inDataSize, UInt32* outSize, void* outData) {
    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass:     PUT(AudioClassID, kAudioObjectClassID);
    case kAudioObjectPropertyClass:         PUT(AudioClassID, kAudioPlugInClassID);
    case kAudioObjectPropertyOwner:         PUT(AudioObjectID, kAudioObjectUnknown);
    case kAudioObjectPropertyManufacturer:  PUT(CFStringRef, CFSTR(kManufacturer));
    case kAudioObjectPropertyOwnedObjects:
    case kAudioPlugInPropertyDeviceList: {
        AudioObjectID ids[kNumDevices];
        for (UInt32 i = 0; i < kNumDevices; i++) ids[i] = DevObj(i);
        return PutIDs(ids, kNumDevices, inDataSize, outSize, outData);
    }
    case kAudioPlugInPropertyBoxList:
        *outSize = 0;
        return kAudioHardwareNoError;
    case kAudioPlugInPropertyTranslateUIDToDevice: {
        NEED(sizeof(AudioObjectID));
        if (outData != NULL) {
            if (qSize != sizeof(CFStringRef) || q == NULL) return kAudioHardwareBadPropertySizeError;
            CFStringRef uid = *(const CFStringRef*)q;
            AudioObjectID found = kAudioObjectUnknown;
            for (UInt32 i = 0; uid != NULL && i < kNumDevices; i++) {
                CFStringRef u = CFStringCreateWithCString(NULL, gDev[i].uid, kCFStringEncodingUTF8);
                if (u && CFStringCompare(uid, u, 0) == kCFCompareEqualTo) found = DevObj(i);
                if (u) CFRelease(u);
            }
            *(AudioObjectID*)outData = found;
        }
        *outSize = sizeof(AudioObjectID);
        return kAudioHardwareNoError;
    }
    case kAudioPlugInPropertyResourceBundle: PUT(CFStringRef, CFSTR(""));
    default: return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus DeviceProp(UInt32 i, const AudioObjectPropertyAddress* a, UInt32 inDataSize, UInt32* outSize, void* outData) {
    const Device* d = &gDev[i];
    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass:     PUT(AudioClassID, kAudioObjectClassID);
    case kAudioObjectPropertyClass:         PUT(AudioClassID, kAudioDeviceClassID);
    case kAudioObjectPropertyOwner:         PUT(AudioObjectID, kObjectID_PlugIn);
    case kAudioObjectPropertyName:          PUTSTR(d->name);
    case kAudioObjectPropertyManufacturer:  PUT(CFStringRef, CFSTR(kManufacturer));
    case kAudioDevicePropertyDeviceUID:     PUTSTR(d->uid);
    case kAudioDevicePropertyModelUID:      PUTSTR(d->modelUID);
    case kAudioObjectPropertyElementName: {
        NEED(sizeof(CFStringRef));
        CFStringRef s = CopyElementName(d, a->mScope, a->mElement);
        if (s == NULL) return kAudioHardwareUnknownPropertyError;
        if (outData) *(CFStringRef*)outData = s; else CFRelease(s);
        *outSize = sizeof(CFStringRef);
        return kAudioHardwareNoError;
    }
    case kAudioDevicePropertyTransportType:  PUT(UInt32, kAudioDeviceTransportTypeVirtual);
    case kAudioDevicePropertyRelatedDevices: {
        AudioObjectID ids[1] = { DevObj(i) };
        return PutIDs(ids, 1, inDataSize, outSize, outData);
    }
    case kAudioDevicePropertyClockDomain:    PUT(UInt32, 0x564D5000u + d->pair);   // 'VMP' + pair: partners share a clock
    case kAudioDevicePropertyDeviceIsAlive:  PUT(UInt32, 1);
    case kAudioDevicePropertyDeviceIsRunning: {
        pthread_mutex_lock(&gIOMutex);
        UInt32 running = d->ioCount > 0;
        pthread_mutex_unlock(&gIOMutex);
        PUT(UInt32, running);
    }
    case kAudioDevicePropertyDeviceCanBeDefaultDevice:       PUT(UInt32, d->engine ? 0 : 1);
    case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: PUT(UInt32, 0);
    case kAudioDevicePropertyLatency:        PUT(UInt32, 0);
    case kAudioDevicePropertySafetyOffset:   PUT(UInt32, 0);
    case kAudioObjectPropertyOwnedObjects:
    case kAudioDevicePropertyStreams: {
        AudioObjectID ids[2];
        UInt32 n = DeviceStreams(i, a->mScope, ids);
        return PutIDs(ids, n, inDataSize, outSize, outData);
    }
    case kAudioObjectPropertyControlList:
        *outSize = 0;
        return kAudioHardwareNoError;
    case kAudioDevicePropertyNominalSampleRate: {
        pthread_mutex_lock(&gStateMutex);
        Float64 r = d->rate;
        pthread_mutex_unlock(&gStateMutex);
        PUT(Float64, r);
    }
    case kAudioDevicePropertyAvailableNominalSampleRates: {
        UInt32 n = kNumRates;
        if (outData != NULL) {
            UInt32 fit = inDataSize / (UInt32)sizeof(AudioValueRange);
            if (fit < n) n = fit;
            AudioValueRange* r = (AudioValueRange*)outData;
            for (UInt32 k = 0; k < n; k++) { r[k].mMinimum = kRates[k]; r[k].mMaximum = kRates[k]; }
        }
        *outSize = n * (UInt32)sizeof(AudioValueRange);
        return kAudioHardwareNoError;
    }
    case kAudioDevicePropertyIsHidden:       PUT(UInt32, d->engine ? 1 : 0);
    case kAudioDevicePropertyPreferredChannelsForStereo: {
        NEED(2 * sizeof(UInt32));
        if (outData) { ((UInt32*)outData)[0] = 1; ((UInt32*)outData)[1] = 2; }
        *outSize = 2 * sizeof(UInt32);
        return kAudioHardwareNoError;
    }
    case kAudioDevicePropertyZeroTimeStampPeriod: PUT(UInt32, kZeroTSPeriod);
    case kAudioDevicePropertyClockIsStable:  PUT(UInt32, 1);
    default: return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus StreamProp(AudioObjectID obj, const AudioObjectPropertyAddress* a,
                           UInt32 inDataSize, UInt32* outSize, void* outData) {
    UInt32 i;
    DeviceIndex(obj, &i);
    const Device* d = &gDev[i];
    Boolean isInput = IsInStream(obj);
    UInt32 ch = isInput ? InChannels(d) : OutChannels(d);
    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass:     PUT(AudioClassID, kAudioObjectClassID);
    case kAudioObjectPropertyClass:         PUT(AudioClassID, kAudioStreamClassID);
    case kAudioObjectPropertyOwner:         PUT(AudioObjectID, DevObj(i));
    case kAudioObjectPropertyName:          PUTSTR(isInput ? d->streamIn : d->streamOut);
    case kAudioObjectPropertyOwnedObjects:
        *outSize = 0;
        return kAudioHardwareNoError;
    case kAudioStreamPropertyIsActive: {
        pthread_mutex_lock(&gStateMutex);
        UInt32 v = isInput ? d->inActive : d->outActive;
        pthread_mutex_unlock(&gStateMutex);
        PUT(UInt32, v);
    }
    case kAudioStreamPropertyDirection:     PUT(UInt32, isInput ? 1 : 0);
    case kAudioStreamPropertyTerminalType:  PUT(UInt32, kAudioStreamTerminalTypeLine);
    case kAudioStreamPropertyStartingChannel: PUT(UInt32, 1);
    case kAudioStreamPropertyLatency:       PUT(UInt32, 0);
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat: {
        NEED(sizeof(AudioStreamBasicDescription));
        if (outData) {
            pthread_mutex_lock(&gStateMutex);
            FillASBD((AudioStreamBasicDescription*)outData, d->rate, ch);
            pthread_mutex_unlock(&gStateMutex);
        }
        *outSize = sizeof(AudioStreamBasicDescription);
        return kAudioHardwareNoError;
    }
    case kAudioStreamPropertyAvailableVirtualFormats:
    case kAudioStreamPropertyAvailablePhysicalFormats: {
        UInt32 n = kNumRates;
        if (outData != NULL) {
            UInt32 fit = inDataSize / (UInt32)sizeof(AudioStreamRangedDescription);
            if (fit < n) n = fit;
            AudioStreamRangedDescription* r = (AudioStreamRangedDescription*)outData;
            for (UInt32 k = 0; k < n; k++) {
                FillASBD(&r[k].mFormat, kRates[k], ch);
                r[k].mSampleRateRange.mMinimum = kRates[k];
                r[k].mSampleRateRange.mMaximum = kRates[k];
            }
        }
        *outSize = n * (UInt32)sizeof(AudioStreamRangedDescription);
        return kAudioHardwareNoError;
    }
    default: return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus GetProp(AudioObjectID obj, const AudioObjectPropertyAddress* a, UInt32 qSize, const void* q,
                        UInt32 inDataSize, UInt32* outSize, void* outData) {
    if (a == NULL || outSize == NULL) return kAudioHardwareIllegalOperationError;
    UInt32 i;
    if (obj == kObjectID_PlugIn) return PlugInProp(a, qSize, q, inDataSize, outSize, outData);
    if (IsDevice(obj) && DeviceIndex(obj, &i)) return DeviceProp(i, a, inDataSize, outSize, outData);
    if (IsInStream(obj) || IsOutStream(obj)) return StreamProp(obj, a, inDataSize, outSize, outData);
    return kAudioHardwareBadObjectError;
}

static Boolean VMP_HasProperty(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress* a) {
    (void)pid;
    if (d != gDriverRef || a == NULL) return false;
    // A property exists iff a size query for it succeeds.
    UInt32 sz = 0;
    return GetProp(obj, a, 0, NULL, 0, &sz, NULL) == kAudioHardwareNoError;
}

static OSStatus VMP_IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                                       const AudioObjectPropertyAddress* a, Boolean* outSettable) {
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    if (a == NULL || outSettable == NULL) return kAudioHardwareIllegalOperationError;
    if (!VMP_HasProperty(d, obj, pid, a)) return kAudioHardwareUnknownPropertyError;
    *outSettable = false;
    if (IsDevice(obj) && a->mSelector == kAudioDevicePropertyNominalSampleRate) *outSettable = true;
    if ((IsInStream(obj) || IsOutStream(obj)) &&
        (a->mSelector == kAudioStreamPropertyIsActive || a->mSelector == kAudioStreamPropertyVirtualFormat ||
         a->mSelector == kAudioStreamPropertyPhysicalFormat)) *outSettable = true;
    return kAudioHardwareNoError;
}

static OSStatus VMP_GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                                        const AudioObjectPropertyAddress* a, UInt32 qSize, const void* q, UInt32* outSize) {
    (void)pid;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    return GetProp(obj, a, qSize, q, 0, outSize, NULL);
}

static OSStatus VMP_GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                                    const AudioObjectPropertyAddress* a, UInt32 qSize, const void* q,
                                    UInt32 inDataSize, UInt32* outSize, void* outData) {
    (void)pid;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    if (outData == NULL) return kAudioHardwareIllegalOperationError;
    return GetProp(obj, a, qSize, q, inDataSize, outSize, outData);
}

static OSStatus RequestRate(UInt32 i, Float64 rate) {
    if (!IsSupportedRate(rate)) return kAudioHardwareIllegalOperationError;
    pthread_mutex_lock(&gStateMutex);
    Float64 cur = gDev[i].rate;
    pthread_mutex_unlock(&gStateMutex);
    if (rate == cur) return kAudioHardwareNoError;
    if (gHost == NULL) return kAudioHardwareNotRunningError;
    // Never change the rate inline: the host calls PerformDeviceConfigurationChange when IO is safe.
    return gHost->RequestDeviceConfigurationChange(gHost, DevObj(i), (UInt64)rate, NULL);
}

static OSStatus VMP_SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                                    const AudioObjectPropertyAddress* a, UInt32 qSize, const void* q,
                                    UInt32 inDataSize, const void* inData) {
    (void)pid; (void)qSize; (void)q;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    if (a == NULL || inData == NULL) return kAudioHardwareIllegalOperationError;
    UInt32 i;
    if (IsDevice(obj) && DeviceIndex(obj, &i)) {
        if (a->mSelector != kAudioDevicePropertyNominalSampleRate) return kAudioHardwareUnknownPropertyError;
        if (inDataSize != sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
        return RequestRate(i, *(const Float64*)inData);
    }
    if ((IsInStream(obj) || IsOutStream(obj)) && DeviceIndex(obj, &i)) {
        Boolean isInput = IsInStream(obj);
        switch (a->mSelector) {
        case kAudioStreamPropertyIsActive: {
            if (inDataSize != sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            Boolean v = *(const UInt32*)inData != 0;
            Boolean changed;
            pthread_mutex_lock(&gStateMutex);
            Boolean* slot = isInput ? &gDev[i].inActive : &gDev[i].outActive;
            changed = *slot != v;
            *slot = v;
            pthread_mutex_unlock(&gStateMutex);
            if (changed && gHost) {
                AudioObjectPropertyAddress ch = { kAudioStreamPropertyIsActive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
                gHost->PropertiesChanged(gHost, obj, 1, &ch);
            }
            return kAudioHardwareNoError;
        }
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: {
            if (inDataSize != sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
            const AudioStreamBasicDescription* f = (const AudioStreamBasicDescription*)inData;
            if (!FormatIsValid(f, isInput ? InChannels(&gDev[i]) : OutChannels(&gDev[i]))) return kAudioDeviceUnsupportedFormatError;
            return RequestRate(i, f->mSampleRate);
        }
        default: return kAudioHardwareUnknownPropertyError;
        }
    }
    if (obj == kObjectID_PlugIn) return kAudioHardwareUnknownPropertyError;
    return kAudioHardwareBadObjectError;
}

// ---------------------------------------------------------------------------------------------
// IO

static Boolean DeviceArg(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32* i, OSStatus* err) {
    if (d != gDriverRef) { *err = kAudioHardwareBadObjectError; return false; }
    if (!IsDevice(dev) || !DeviceIndex(dev, i)) { *err = kAudioHardwareBadDeviceError; return false; }
    return true;
}

// The first device of a pair to start sets the pair's anchor and clears its rings; the second
// joins the same time line, so a sample time means the same instant on both devices.
static OSStatus VMP_StartIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client) {
    (void)client;
    UInt32 i; OSStatus err;
    if (!DeviceArg(d, dev, &i, &err)) return err;
    Pair* p = &gPair[gDev[i].pair];
    pthread_mutex_lock(&gIOMutex);
    if (gDev[i].ioCount == UINT32_MAX) { pthread_mutex_unlock(&gIOMutex); return kAudioHardwareIllegalOperationError; }
    gDev[i].ioCount++;
    if (p->ioCount++ == 0) {
        p->anchorHostTime = mach_absolute_time();
        ResetRing(&p->toUser);
        ResetRing(&p->toEngine);
    }
    pthread_mutex_unlock(&gIOMutex);
    return kAudioHardwareNoError;
}

static OSStatus VMP_StopIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client) {
    (void)client;
    UInt32 i; OSStatus err;
    if (!DeviceArg(d, dev, &i, &err)) return err;
    Pair* p = &gPair[gDev[i].pair];
    pthread_mutex_lock(&gIOMutex);
    if (gDev[i].ioCount > 0) { gDev[i].ioCount--; if (p->ioCount > 0) p->ioCount--; }
    pthread_mutex_unlock(&gIOMutex);
    return kAudioHardwareNoError;
}

// Zero time stamps on a fixed period from the pair's anchor: sample time n*period at host time
// anchor + n*period*ticksPerFrame, n = periods elapsed so far.
static OSStatus VMP_GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client,
                                     Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed) {
    (void)client;
    UInt32 i; OSStatus err;
    if (!DeviceArg(d, dev, &i, &err)) return err;
    Pair* p = &gPair[gDev[i].pair];
    pthread_mutex_lock(&gIOMutex);
    Float64 ticksPerPeriod = gDev[i].ticksPerFrame * (Float64)kZeroTSPeriod;
    UInt64 now = mach_absolute_time();
    UInt64 n = now > p->anchorHostTime ? (UInt64)((Float64)(now - p->anchorHostTime) / ticksPerPeriod) : 0;
    *outSampleTime = (Float64)(n * kZeroTSPeriod);
    *outHostTime   = p->anchorHostTime + (UInt64)((Float64)n * ticksPerPeriod);
    *outSeed       = p->seed;
    pthread_mutex_unlock(&gIOMutex);
    return kAudioHardwareNoError;
}

static OSStatus VMP_WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op,
                                      Boolean* outWillDo, Boolean* outInPlace) {
    (void)client;
    UInt32 i; OSStatus err;
    if (!DeviceArg(d, dev, &i, &err)) return err;
    Boolean will = op == kAudioServerPlugInIOOperationReadInput || op == kAudioServerPlugInIOOperationWriteMix;
    if (outWillDo) *outWillDo = will;
    if (outInPlace) *outInPlace = true;
    return kAudioHardwareNoError;
}

static OSStatus VMP_BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op,
                                     UInt32 frames, const AudioServerPlugInIOCycleInfo* info) {
    (void)client; (void)op; (void)frames; (void)info;
    UInt32 i; OSStatus err;
    return DeviceArg(d, dev, &i, &err) ? kAudioHardwareNoError : err;
}

static OSStatus VMP_EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op,
                                   UInt32 frames, const AudioServerPlugInIOCycleInfo* info) {
    (void)client; (void)op; (void)frames; (void)info;
    UInt32 i; OSStatus err;
    return DeviceArg(d, dev, &i, &err) ? kAudioHardwareNoError : err;
}

static inline UInt32 RingIndex(Float64 sampleTime) {
    // Sample times are whole numbers; reduce modulo the ring (handles negatives too).
    SInt64 t = (SInt64)sampleTime;
    return (UInt32)(t & (SInt64)(kRingFrames - 1));
}

// WriteMix: the first write to a frame for a given sample time overwrites; further writes for the
// same time (more than one mix) are summed. Audio is stored before its stamp, so the reader on the
// partner's IO thread never takes a stamped slot whose audio is not there yet.
static void WriteMix(Ring* r, const Float32* src, UInt32 frames, Float64 sampleTime) {
    UInt32 ch = r->ch, idx = RingIndex(sampleTime);
    for (UInt32 f = 0; f < frames; f++) {
        Float64 t = sampleTime + (Float64)f;
        Float32* dst = &r->data[(size_t)idx * ch];
        const Float32* in = &src[(size_t)f * ch];
        if (r->stamp[idx] != t) {
            memcpy(dst, in, ch * sizeof(Float32));
            atomic_thread_fence(memory_order_release);
            r->stamp[idx] = t;
        } else {
            for (UInt32 c = 0; c < ch; c++) dst[c] += in[c];
        }
        idx = (idx + 1) & (kRingFrames - 1);
    }
}

// ReadInput: frames nobody wrote for exactly this sample time read as silence.
static void ReadInput(Ring* r, Float32* dst, UInt32 frames, Float64 sampleTime) {
    UInt32 ch = r->ch, idx = RingIndex(sampleTime);
    for (UInt32 f = 0; f < frames; f++) {
        Float64 t = sampleTime + (Float64)f;
        Float32* out = &dst[(size_t)f * ch];
        if (r->stamp[idx] == t) {
            atomic_thread_fence(memory_order_acquire);
            memcpy(out, &r->data[(size_t)idx * ch], ch * sizeof(Float32));
        } else {
            memset(out, 0, ch * sizeof(Float32));
        }
        idx = (idx + 1) & (kRingFrames - 1);
    }
}

static OSStatus VMP_DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, AudioObjectID stream, UInt32 client,
                                  UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo* info,
                                  void* ioMain, void* ioSecondary) {
    (void)client; (void)ioSecondary;
    UInt32 i; OSStatus err;
    if (!DeviceArg(d, dev, &i, &err)) return err;
    if (info == NULL || ioMain == NULL) return kAudioHardwareIllegalOperationError;
    if (frames > kRingFrames / 2) return kAudioHardwareIllegalOperationError;
    if (op == kAudioServerPlugInIOOperationWriteMix && stream == OutStreamObj(i)) {
        WriteMix(WriteRing(i), (const Float32*)ioMain, frames, info->mOutputTime.mSampleTime);
    } else if (op == kAudioServerPlugInIOOperationReadInput && stream == InStreamObj(i)) {
        ReadInput(ReadRing(i), (Float32*)ioMain, frames, info->mInputTime.mSampleTime);
    }
    return kAudioHardwareNoError;
}
