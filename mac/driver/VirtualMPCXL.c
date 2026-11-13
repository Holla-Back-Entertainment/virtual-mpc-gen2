// VirtualMPCXL.c - "Virtual MPC XL" AudioServerPlugIn (HAL driver). MIT licence.
//
// One device, one 16-ch input stream, one 16-ch output stream, 32-bit float, 44.1/48 kHz.
// Half-swapped loopback through one ring indexed by sample time:
//   OUTPUT 1-8  ("To MPC")       -> INPUT 9-16 ("Computer")
//   OUTPUT 9-16 ("MPC Engine")   -> INPUT 1-8  ("MPC Out")
// The companion app writes the MPC's outs to output 9-16 and reads Live's outs on input 9-16;
// Live reads the MPC on input 1-8 and writes back on output 1-8. No path loops a bank onto itself.
//
// Written from scratch against <CoreAudio/AudioServerPlugIn.h>. Object model and timing follow the
// documented AudioServerPlugIn contract (zero time stamps from mach_absolute_time on a fixed period).

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------------------------
// Constants

#define kDeviceName         "Virtual MPC XL"
#define kManufacturer       "Henry Skillz"
#define kDeviceUID          "VirtualMPCXL_UID"
#define kModelUID           "VirtualMPCXL_ModelUID"

enum {
    kObjectID_PlugIn       = kAudioObjectPlugInObject, // 1
    kObjectID_Device       = 2,
    kObjectID_InputStream  = 3,
    kObjectID_OutputStream = 4,
};

#define kChannels           16u
#define kHalf               8u
#define kBytesPerFrame      (kChannels * (UInt32)sizeof(Float32))
#define kRingFrames         65536u          // loopback ring, power of two
#define kZeroTSPeriod       16384u          // frames between zero time stamps
#define kDefaultRate        44100.0

static const Float64 kRates[] = { 44100.0, 48000.0 };
#define kNumRates (sizeof(kRates) / sizeof(kRates[0]))

// ---------------------------------------------------------------------------------------------
// State

static pthread_mutex_t gStateMutex = PTHREAD_MUTEX_INITIALIZER;   // properties
static pthread_mutex_t gIOMutex    = PTHREAD_MUTEX_INITIALIZER;   // timing + IO state

static AudioServerPlugInHostRef gHost = NULL;
static _Atomic UInt32 gRefCount = 0;

static Float64 gSampleRate        = kDefaultRate;   // guarded by gStateMutex
static Boolean gInputActive       = true;
static Boolean gOutputActive      = true;

static UInt32  gIOCount           = 0;              // guarded by gIOMutex
static Float64 gHostTicksPerFrame = 0;
static UInt64  gAnchorHostTime    = 0;
static UInt64  gNumZeroTimeStamps = 0;

// Ring stored in INPUT channel order; gStamp[i] = sample time last written into slot i, so a read
// of a slot nobody wrote this cycle returns silence (no replay of stale audio after a wrap).
static Float32 gRing[kRingFrames * kChannels];
static Float64 gStamp[kRingFrames];

static Float64 HostClockFrequency(void) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return 1.0e9 * (Float64)tb.denom / (Float64)tb.numer;
}

static void ResetRing(void) {
    memset(gRing, 0, sizeof(gRing));
    for (UInt32 i = 0; i < kRingFrames; i++) gStamp[i] = -1.0;
}

static Boolean IsSupportedRate(Float64 r) {
    for (UInt32 i = 0; i < kNumRates; i++) if (r == kRates[i]) return true;
    return false;
}

static void FillASBD(AudioStreamBasicDescription* d, Float64 rate) {
    memset(d, 0, sizeof(*d));
    d->mSampleRate       = rate;
    d->mFormatID         = kAudioFormatLinearPCM;
    d->mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    d->mBytesPerPacket   = kBytesPerFrame;
    d->mFramesPerPacket  = 1;
    d->mBytesPerFrame    = kBytesPerFrame;
    d->mChannelsPerFrame = kChannels;
    d->mBitsPerChannel   = 32;
}

static Boolean FormatIsValid(const AudioStreamBasicDescription* d) {
    return d->mFormatID == kAudioFormatLinearPCM
        && (d->mFormatFlags & kAudioFormatFlagIsFloat)
        && d->mBitsPerChannel == 32
        && d->mChannelsPerFrame == kChannels
        && d->mBytesPerFrame == kBytesPerFrame
        && d->mFramesPerPacket == 1
        && IsSupportedRate(d->mSampleRate);
}

static CFStringRef CopyElementName(AudioObjectPropertyScope scope, UInt32 element) {
    if (element < 1 || element > kChannels) return NULL;
    Boolean low = element <= kHalf;
    UInt32  n   = low ? element : element - kHalf;
    const char* fmt;
    if (scope == kAudioObjectPropertyScopeInput) fmt = low ? "MPC Out %u" : "Computer %u";
    else if (scope == kAudioObjectPropertyScopeOutput) fmt = low ? "To MPC %u" : "MPC Engine %u";
    else return NULL;
    char buf[32];
    snprintf(buf, sizeof(buf), fmt, (unsigned)n);
    return CFStringCreateWithCString(NULL, buf, kCFStringEncodingUTF8);
}

// ---------------------------------------------------------------------------------------------
// Forward declarations of the interface

static HRESULT  VMX_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface);
static ULONG    VMX_AddRef(void* inDriver);
static ULONG    VMX_Release(void* inDriver);
static OSStatus VMX_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost);
static OSStatus VMX_CreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo*, AudioObjectID*);
static OSStatus VMX_DestroyDevice(AudioServerPlugInDriverRef, AudioObjectID);
static OSStatus VMX_AddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*);
static OSStatus VMX_RemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*);
static OSStatus VMX_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*);
static OSStatus VMX_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*);
static Boolean  VMX_HasProperty(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*);
static OSStatus VMX_IsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, Boolean*);
static OSStatus VMX_GetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32*);
static OSStatus VMX_GetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, UInt32*, void*);
static OSStatus VMX_SetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, const void*);
static OSStatus VMX_StartIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus VMX_StopIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus VMX_GetZeroTimeStamp(AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64*, UInt64*, UInt64*);
static OSStatus VMX_WillDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, Boolean*, Boolean*);
static OSStatus VMX_BeginIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*);
static OSStatus VMX_DoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*, void*, void*);
static OSStatus VMX_EndIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*);

static AudioServerPlugInDriverInterface gInterface = {
    NULL,
    VMX_QueryInterface, VMX_AddRef, VMX_Release,
    VMX_Initialize, VMX_CreateDevice, VMX_DestroyDevice,
    VMX_AddDeviceClient, VMX_RemoveDeviceClient,
    VMX_PerformDeviceConfigurationChange, VMX_AbortDeviceConfigurationChange,
    VMX_HasProperty, VMX_IsPropertySettable, VMX_GetPropertyDataSize, VMX_GetPropertyData, VMX_SetPropertyData,
    VMX_StartIO, VMX_StopIO, VMX_GetZeroTimeStamp,
    VMX_WillDoIOOperation, VMX_BeginIOOperation, VMX_DoIOOperation, VMX_EndIOOperation,
};
static AudioServerPlugInDriverInterface* gInterfacePtr = &gInterface;
static AudioServerPlugInDriverRef gDriverRef = &gInterfacePtr;

// ---------------------------------------------------------------------------------------------
// Factory (named in Info.plist CFPlugInFactories)

__attribute__((visibility("default")))
void* VirtualMPCXL_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID);

void* VirtualMPCXL_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID) {
    (void)inAllocator;
    if (inRequestedTypeUUID == NULL || !CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID)) return NULL;
    return gDriverRef;
}

// ---------------------------------------------------------------------------------------------
// IUnknown

static HRESULT VMX_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface) {
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

static ULONG VMX_AddRef(void* inDriver) {
    if (inDriver != gDriverRef) return 0;
    return atomic_fetch_add(&gRefCount, 1) + 1;
}

static ULONG VMX_Release(void* inDriver) {
    if (inDriver != gDriverRef) return 0;
    UInt32 cur = atomic_load(&gRefCount);
    while (cur > 0 && !atomic_compare_exchange_weak(&gRefCount, &cur, cur - 1)) {}
    return cur > 0 ? cur - 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// Basic operations

static OSStatus VMX_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost) {
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    gHost = inHost;
    pthread_mutex_lock(&gIOMutex);
    gHostTicksPerFrame = HostClockFrequency() / gSampleRate;
    ResetRing();
    pthread_mutex_unlock(&gIOMutex);
    return kAudioHardwareNoError;
}

static OSStatus VMX_CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc, const AudioServerPlugInClientInfo* ci, AudioObjectID* out) {
    (void)d; (void)desc; (void)ci; (void)out;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus VMX_DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID dev) {
    (void)d; (void)dev;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus VMX_AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID dev, const AudioServerPlugInClientInfo* ci) {
    (void)ci;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    return dev == kObjectID_Device ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}

static OSStatus VMX_RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID dev, const AudioServerPlugInClientInfo* ci) {
    (void)ci;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    return dev == kObjectID_Device ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}

// The change action carries the requested sample rate as a whole number of Hz.
static OSStatus VMX_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt64 action, void* info) {
    (void)info;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    if (dev != kObjectID_Device) return kAudioHardwareBadDeviceError;
    Float64 rate = (Float64)action;
    if (!IsSupportedRate(rate)) return kAudioHardwareIllegalOperationError;
    pthread_mutex_lock(&gStateMutex);
    gSampleRate = rate;
    pthread_mutex_unlock(&gStateMutex);
    pthread_mutex_lock(&gIOMutex);
    gHostTicksPerFrame = HostClockFrequency() / rate;
    ResetRing();
    pthread_mutex_unlock(&gIOMutex);
    return kAudioHardwareNoError;
}

static OSStatus VMX_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt64 action, void* info) {
    (void)action; (void)info;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    return dev == kObjectID_Device ? kAudioHardwareNoError : kAudioHardwareBadDeviceError;
}

// ---------------------------------------------------------------------------------------------
// Properties. One routine answers both size (outData == NULL) and data queries.

#define NEED(sz) do { if (outData != NULL && inDataSize < (UInt32)(sz)) return kAudioHardwareBadPropertySizeError; } while (0)
#define PUT(type, val) do { NEED(sizeof(type)); if (outData) *(type*)outData = (val); *outSize = (UInt32)sizeof(type); return kAudioHardwareNoError; } while (0)

static OSStatus PutIDs(const AudioObjectID* ids, UInt32 n, UInt32 inDataSize, UInt32* outSize, void* outData) {
    if (outData != NULL) {
        UInt32 fit = inDataSize / (UInt32)sizeof(AudioObjectID);
        if (fit < n) n = fit;
        memcpy(outData, ids, n * sizeof(AudioObjectID));
    }
    *outSize = n * (UInt32)sizeof(AudioObjectID);
    return kAudioHardwareNoError;
}

static UInt32 DeviceStreams(AudioObjectPropertyScope scope, AudioObjectID out[2]) {
    UInt32 n = 0;
    if (scope == kAudioObjectPropertyScopeGlobal || scope == kAudioObjectPropertyScopeInput) out[n++] = kObjectID_InputStream;
    if (scope == kAudioObjectPropertyScopeGlobal || scope == kAudioObjectPropertyScopeOutput) out[n++] = kObjectID_OutputStream;
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
        AudioObjectID ids[1] = { kObjectID_Device };
        return PutIDs(ids, 1, inDataSize, outSize, outData);
    }
    case kAudioPlugInPropertyBoxList:
        *outSize = 0;
        return kAudioHardwareNoError;
    case kAudioPlugInPropertyTranslateUIDToDevice: {
        NEED(sizeof(AudioObjectID));
        if (outData != NULL) {
            if (qSize != sizeof(CFStringRef) || q == NULL) return kAudioHardwareBadPropertySizeError;
            CFStringRef uid = *(const CFStringRef*)q;
            *(AudioObjectID*)outData = (uid && CFStringCompare(uid, CFSTR(kDeviceUID), 0) == kCFCompareEqualTo)
                                       ? kObjectID_Device : kAudioObjectUnknown;
        }
        *outSize = sizeof(AudioObjectID);
        return kAudioHardwareNoError;
    }
    case kAudioPlugInPropertyResourceBundle: PUT(CFStringRef, CFSTR(""));
    default: return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus DeviceProp(const AudioObjectPropertyAddress* a, UInt32 inDataSize, UInt32* outSize, void* outData) {
    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass:     PUT(AudioClassID, kAudioObjectClassID);
    case kAudioObjectPropertyClass:         PUT(AudioClassID, kAudioDeviceClassID);
    case kAudioObjectPropertyOwner:         PUT(AudioObjectID, kObjectID_PlugIn);
    case kAudioObjectPropertyName:          PUT(CFStringRef, CFSTR(kDeviceName));
    case kAudioObjectPropertyManufacturer:  PUT(CFStringRef, CFSTR(kManufacturer));
    case kAudioDevicePropertyDeviceUID:     PUT(CFStringRef, CFSTR(kDeviceUID));
    case kAudioDevicePropertyModelUID:      PUT(CFStringRef, CFSTR(kModelUID));
    case kAudioObjectPropertyElementName: {
        NEED(sizeof(CFStringRef));
        CFStringRef s = CopyElementName(a->mScope, a->mElement);
        if (s == NULL) return kAudioHardwareUnknownPropertyError;
        if (outData) *(CFStringRef*)outData = s; else CFRelease(s);
        *outSize = sizeof(CFStringRef);
        return kAudioHardwareNoError;
    }
    case kAudioDevicePropertyTransportType:  PUT(UInt32, kAudioDeviceTransportTypeVirtual);
    case kAudioDevicePropertyRelatedDevices: {
        AudioObjectID ids[1] = { kObjectID_Device };
        return PutIDs(ids, 1, inDataSize, outSize, outData);
    }
    case kAudioDevicePropertyClockDomain:    PUT(UInt32, 0);
    case kAudioDevicePropertyDeviceIsAlive:  PUT(UInt32, 1);
    case kAudioDevicePropertyDeviceIsRunning: {
        pthread_mutex_lock(&gIOMutex);
        UInt32 running = gIOCount > 0;
        pthread_mutex_unlock(&gIOMutex);
        PUT(UInt32, running);
    }
    case kAudioDevicePropertyDeviceCanBeDefaultDevice:       PUT(UInt32, 1);
    case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: PUT(UInt32, 0);
    case kAudioDevicePropertyLatency:        PUT(UInt32, 0);
    case kAudioDevicePropertySafetyOffset:   PUT(UInt32, 0);
    case kAudioObjectPropertyOwnedObjects:
    case kAudioDevicePropertyStreams: {
        AudioObjectID ids[2];
        UInt32 n = DeviceStreams(a->mScope, ids);
        return PutIDs(ids, n, inDataSize, outSize, outData);
    }
    case kAudioObjectPropertyControlList:
        *outSize = 0;
        return kAudioHardwareNoError;
    case kAudioDevicePropertyNominalSampleRate: {
        pthread_mutex_lock(&gStateMutex);
        Float64 r = gSampleRate;
        pthread_mutex_unlock(&gStateMutex);
        PUT(Float64, r);
    }
    case kAudioDevicePropertyAvailableNominalSampleRates: {
        UInt32 n = kNumRates;
        if (outData != NULL) {
            UInt32 fit = inDataSize / (UInt32)sizeof(AudioValueRange);
            if (fit < n) n = fit;
            AudioValueRange* r = (AudioValueRange*)outData;
            for (UInt32 i = 0; i < n; i++) { r[i].mMinimum = kRates[i]; r[i].mMaximum = kRates[i]; }
        }
        *outSize = n * (UInt32)sizeof(AudioValueRange);
        return kAudioHardwareNoError;
    }
    case kAudioDevicePropertyIsHidden:       PUT(UInt32, 0);
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
    Boolean isInput = obj == kObjectID_InputStream;
    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass:     PUT(AudioClassID, kAudioObjectClassID);
    case kAudioObjectPropertyClass:         PUT(AudioClassID, kAudioStreamClassID);
    case kAudioObjectPropertyOwner:         PUT(AudioObjectID, kObjectID_Device);
    case kAudioObjectPropertyName:          PUT(CFStringRef, isInput ? CFSTR("MPC XL In") : CFSTR("MPC XL Out"));
    case kAudioObjectPropertyOwnedObjects:
        *outSize = 0;
        return kAudioHardwareNoError;
    case kAudioStreamPropertyIsActive: {
        pthread_mutex_lock(&gStateMutex);
        UInt32 v = isInput ? gInputActive : gOutputActive;
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
            FillASBD((AudioStreamBasicDescription*)outData, gSampleRate);
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
            for (UInt32 i = 0; i < n; i++) {
                FillASBD(&r[i].mFormat, kRates[i]);
                r[i].mSampleRateRange.mMinimum = kRates[i];
                r[i].mSampleRateRange.mMaximum = kRates[i];
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
    switch (obj) {
    case kObjectID_PlugIn:       return PlugInProp(a, qSize, q, inDataSize, outSize, outData);
    case kObjectID_Device:       return DeviceProp(a, inDataSize, outSize, outData);
    case kObjectID_InputStream:
    case kObjectID_OutputStream: return StreamProp(obj, a, inDataSize, outSize, outData);
    default:                     return kAudioHardwareBadObjectError;
    }
}

static Boolean VMX_HasProperty(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress* a) {
    (void)pid;
    if (d != gDriverRef || a == NULL) return false;
    // A property exists iff a size query for it succeeds.
    UInt32 sz = 0;
    return GetProp(obj, a, 0, NULL, 0, &sz, NULL) == kAudioHardwareNoError;
}

static OSStatus VMX_IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                                       const AudioObjectPropertyAddress* a, Boolean* outSettable) {
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    if (a == NULL || outSettable == NULL) return kAudioHardwareIllegalOperationError;
    if (!VMX_HasProperty(d, obj, pid, a)) return kAudioHardwareUnknownPropertyError;
    *outSettable = false;
    if (obj == kObjectID_Device && a->mSelector == kAudioDevicePropertyNominalSampleRate) *outSettable = true;
    if ((obj == kObjectID_InputStream || obj == kObjectID_OutputStream) &&
        (a->mSelector == kAudioStreamPropertyIsActive || a->mSelector == kAudioStreamPropertyVirtualFormat ||
         a->mSelector == kAudioStreamPropertyPhysicalFormat)) *outSettable = true;
    return kAudioHardwareNoError;
}

static OSStatus VMX_GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                                        const AudioObjectPropertyAddress* a, UInt32 qSize, const void* q, UInt32* outSize) {
    (void)pid;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    return GetProp(obj, a, qSize, q, 0, outSize, NULL);
}

static OSStatus VMX_GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                                    const AudioObjectPropertyAddress* a, UInt32 qSize, const void* q,
                                    UInt32 inDataSize, UInt32* outSize, void* outData) {
    (void)pid;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    if (outData == NULL) return kAudioHardwareIllegalOperationError;
    return GetProp(obj, a, qSize, q, inDataSize, outSize, outData);
}

static OSStatus RequestRate(Float64 rate) {
    if (!IsSupportedRate(rate)) return kAudioHardwareIllegalOperationError;
    pthread_mutex_lock(&gStateMutex);
    Float64 cur = gSampleRate;
    pthread_mutex_unlock(&gStateMutex);
    if (rate == cur) return kAudioHardwareNoError;
    if (gHost == NULL) return kAudioHardwareNotRunningError;
    // Never change the rate inline: the host calls PerformDeviceConfigurationChange when IO is safe.
    return gHost->RequestDeviceConfigurationChange(gHost, kObjectID_Device, (UInt64)rate, NULL);
}

static OSStatus VMX_SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                                    const AudioObjectPropertyAddress* a, UInt32 qSize, const void* q,
                                    UInt32 inDataSize, const void* inData) {
    (void)pid; (void)qSize; (void)q;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    if (a == NULL || inData == NULL) return kAudioHardwareIllegalOperationError;
    if (obj == kObjectID_Device) {
        if (a->mSelector != kAudioDevicePropertyNominalSampleRate) return kAudioHardwareUnknownPropertyError;
        if (inDataSize != sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
        return RequestRate(*(const Float64*)inData);
    }
    if (obj == kObjectID_InputStream || obj == kObjectID_OutputStream) {
        switch (a->mSelector) {
        case kAudioStreamPropertyIsActive: {
            if (inDataSize != sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            Boolean v = *(const UInt32*)inData != 0;
            Boolean changed;
            pthread_mutex_lock(&gStateMutex);
            Boolean* slot = obj == kObjectID_InputStream ? &gInputActive : &gOutputActive;
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
            if (!FormatIsValid(f)) return kAudioDeviceUnsupportedFormatError;
            return RequestRate(f->mSampleRate);
        }
        default: return kAudioHardwareUnknownPropertyError;
        }
    }
    if (obj == kObjectID_PlugIn) return kAudioHardwareUnknownPropertyError;
    return kAudioHardwareBadObjectError;
}

// ---------------------------------------------------------------------------------------------
// IO

static OSStatus VMX_StartIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client) {
    (void)client;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    if (dev != kObjectID_Device) return kAudioHardwareBadDeviceError;
    pthread_mutex_lock(&gIOMutex);
    if (gIOCount == UINT32_MAX) { pthread_mutex_unlock(&gIOMutex); return kAudioHardwareIllegalOperationError; }
    if (gIOCount++ == 0) {
        gNumZeroTimeStamps = 0;
        gAnchorHostTime = mach_absolute_time();
        ResetRing();
    }
    pthread_mutex_unlock(&gIOMutex);
    return kAudioHardwareNoError;
}

static OSStatus VMX_StopIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client) {
    (void)client;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    if (dev != kObjectID_Device) return kAudioHardwareBadDeviceError;
    pthread_mutex_lock(&gIOMutex);
    if (gIOCount > 0) gIOCount--;
    pthread_mutex_unlock(&gIOMutex);
    return kAudioHardwareNoError;
}

static OSStatus VMX_GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client,
                                     Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed) {
    (void)client;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    if (dev != kObjectID_Device) return kAudioHardwareBadDeviceError;
    pthread_mutex_lock(&gIOMutex);
    Float64 ticksPerPeriod = gHostTicksPerFrame * (Float64)kZeroTSPeriod;
    UInt64 now = mach_absolute_time();
    UInt64 next = gAnchorHostTime + (UInt64)((Float64)(gNumZeroTimeStamps + 1) * ticksPerPeriod);
    if (next <= now) gNumZeroTimeStamps++;
    *outSampleTime = (Float64)(gNumZeroTimeStamps * kZeroTSPeriod);
    *outHostTime   = gAnchorHostTime + (UInt64)((Float64)gNumZeroTimeStamps * ticksPerPeriod);
    *outSeed       = 1;
    pthread_mutex_unlock(&gIOMutex);
    return kAudioHardwareNoError;
}

static OSStatus VMX_WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op,
                                      Boolean* outWillDo, Boolean* outInPlace) {
    (void)client;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    if (dev != kObjectID_Device) return kAudioHardwareBadDeviceError;
    Boolean will = op == kAudioServerPlugInIOOperationReadInput || op == kAudioServerPlugInIOOperationWriteMix;
    if (outWillDo) *outWillDo = will;
    if (outInPlace) *outInPlace = true;
    return kAudioHardwareNoError;
}

static OSStatus VMX_BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op,
                                     UInt32 frames, const AudioServerPlugInIOCycleInfo* info) {
    (void)client; (void)op; (void)frames; (void)info;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    return dev == kObjectID_Device ? kAudioHardwareNoError : kAudioHardwareBadDeviceError;
}

static OSStatus VMX_EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op,
                                   UInt32 frames, const AudioServerPlugInIOCycleInfo* info) {
    (void)client; (void)op; (void)frames; (void)info;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    return dev == kObjectID_Device ? kAudioHardwareNoError : kAudioHardwareBadDeviceError;
}

static inline UInt32 RingIndex(Float64 sampleTime) {
    // Sample times are whole numbers; reduce modulo the ring (handles negatives too).
    SInt64 t = (SInt64)sampleTime;
    return (UInt32)(t & (SInt64)(kRingFrames - 1));
}

// WriteMix: OUTPUT channel c lands in INPUT channel (c + 8) % 16. The first write to a frame for
// a given sample time overwrites; further writes in the same cycle (more than one mix) are summed.
static void WriteMix(const Float32* src, UInt32 frames, Float64 sampleTime) {
    UInt32 idx = RingIndex(sampleTime);
    for (UInt32 f = 0; f < frames; f++) {
        Float64 t = sampleTime + (Float64)f;
        Float32* dst = &gRing[idx * kChannels];
        const Float32* in = &src[f * kChannels];
        if (gStamp[idx] != t) {
            for (UInt32 c = 0; c < kHalf; c++) { dst[c + kHalf] = in[c]; dst[c] = in[c + kHalf]; }
            gStamp[idx] = t;
        } else {
            for (UInt32 c = 0; c < kHalf; c++) { dst[c + kHalf] += in[c]; dst[c] += in[c + kHalf]; }
        }
        idx = (idx + 1) & (kRingFrames - 1);
    }
}

// ReadInput: frames nobody wrote for exactly this sample time read as silence.
static void ReadInput(Float32* dst, UInt32 frames, Float64 sampleTime) {
    UInt32 idx = RingIndex(sampleTime);
    for (UInt32 f = 0; f < frames; f++) {
        Float64 t = sampleTime + (Float64)f;
        Float32* out = &dst[f * kChannels];
        if (gStamp[idx] == t) memcpy(out, &gRing[idx * kChannels], kBytesPerFrame);
        else memset(out, 0, kBytesPerFrame);
        idx = (idx + 1) & (kRingFrames - 1);
    }
}

static OSStatus VMX_DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, AudioObjectID stream, UInt32 client,
                                  UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo* info,
                                  void* ioMain, void* ioSecondary) {
    (void)client; (void)ioSecondary;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    if (dev != kObjectID_Device) return kAudioHardwareBadDeviceError;
    if (info == NULL || ioMain == NULL) return kAudioHardwareIllegalOperationError;
    if (frames > kRingFrames) return kAudioHardwareIllegalOperationError;
    if (op == kAudioServerPlugInIOOperationWriteMix && stream == kObjectID_OutputStream) {
        WriteMix((const Float32*)ioMain, frames, info->mOutputTime.mSampleTime);
    } else if (op == kAudioServerPlugInIOOperationReadInput && stream == kObjectID_InputStream) {
        ReadInput((Float32*)ioMain, frames, info->mInputTime.mSampleTime);
    }
    return kAudioHardwareNoError;
}
