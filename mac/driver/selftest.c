// Offline self-check for VirtualMPCXL.c: drives the plug-in through its factory and driver
// interface with a fake host (no coreaudiod). Build/run: driver/build.sh test
#include "VirtualMPCXL.c"
#include <assert.h>
#include <math.h>

static AudioServerPlugInDriverRef gD;
#define DRV (*gD)

static OSStatus FakePropertiesChanged(AudioServerPlugInHostRef h, AudioObjectID o, UInt32 n, const AudioObjectPropertyAddress* a) {
    (void)h; (void)o; (void)n; (void)a; return 0;
}
static OSStatus FakeCopy(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef* d) { (void)h; (void)k; *d = NULL; return 0; }
static OSStatus FakeWrite(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef d) { (void)h; (void)k; (void)d; return 0; }
static OSStatus FakeDelete(AudioServerPlugInHostRef h, CFStringRef k) { (void)h; (void)k; return 0; }
static OSStatus FakeRequest(AudioServerPlugInHostRef h, AudioObjectID dev, UInt64 action, void* info) {
    (void)h; return DRV->PerformDeviceConfigurationChange(gD, dev, action, info);   // host would defer this
}
static AudioServerPlugInHostInterface gFakeHost = { FakePropertiesChanged, FakeCopy, FakeWrite, FakeDelete, FakeRequest };

static int gFails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); gFails++; } } while (0)

static void CheckName(AudioObjectPropertyScope scope, UInt32 el, const char* want) {
    AudioObjectPropertyAddress a = { kAudioObjectPropertyElementName, scope, el };
    CFStringRef s = NULL; UInt32 sz = sizeof(s);
    CHECK(DRV->HasProperty(gD, kObjectID_Device, 0, &a));
    CHECK(DRV->GetPropertyData(gD, kObjectID_Device, 0, &a, 0, NULL, sz, &sz, &s) == 0);
    char buf[64] = {0};
    if (s) { CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8); CFRelease(s); }
    if (strcmp(buf, want) != 0) { printf("FAIL name scope=%c el=%u got '%s' want '%s'\n", (char)(scope >> 24), el, buf, want); gFails++; }
}

static UInt32 GetU32(AudioObjectID o, AudioObjectPropertySelector sel, AudioObjectPropertyScope sc) {
    AudioObjectPropertyAddress a = { sel, sc, kAudioObjectPropertyElementMain };
    UInt32 v = 0xFFFFFFFF, sz = sizeof(v);
    CHECK(DRV->GetPropertyData(gD, o, 0, &a, 0, NULL, sz, &sz, &v) == 0);
    return v;
}

#define N 512
static Float32 bufA[N * kChannels], bufB[N * kChannels], in[N * kChannels];

static void Fill(Float32* b, Float32 base) {   // value encodes source channel and frame
    for (UInt32 f = 0; f < N; f++) for (UInt32 c = 0; c < kChannels; c++) b[f * kChannels + c] = base + (Float32)c + (Float32)f * 0.001f;
}

int main(void) {
    // Factory + QueryInterface, as CFPlugIn would do.
    CHECK(VirtualMPCXL_Create(NULL, kAudioServerPlugInDriverInterfaceUUID) == NULL);
    void* unk = VirtualMPCXL_Create(NULL, kAudioServerPlugInTypeUUID);
    CHECK(unk != NULL);
    void* iface = NULL;
    CHECK((*(AudioServerPlugInDriverRef)unk)->QueryInterface(unk, CFUUIDGetUUIDBytes(kAudioServerPlugInDriverInterfaceUUID), &iface) == S_OK);
    gD = (AudioServerPlugInDriverRef)iface;
    CHECK(DRV->Initialize(gD, &gFakeHost) == 0);

    // Identity + topology.
    {
        AudioObjectPropertyAddress a = { kAudioPlugInPropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        CFStringRef uid = CFSTR("VirtualMPCXL_UID"); AudioObjectID dev = 0; UInt32 sz = sizeof(dev);
        CHECK(DRV->GetPropertyData(gD, kObjectID_PlugIn, 0, &a, sizeof(uid), &uid, sz, &sz, &dev) == 0 && dev == kObjectID_Device);
        a.mSelector = kAudioObjectPropertyName; CFStringRef name = NULL; sz = sizeof(name);
        CHECK(DRV->GetPropertyData(gD, kObjectID_Device, 0, &a, 0, NULL, sz, &sz, &name) == 0);
        CHECK(name && CFStringCompare(name, CFSTR("Virtual MPC XL"), 0) == kCFCompareEqualTo);
        a.mSelector = kAudioDevicePropertyStreams; a.mScope = kAudioObjectPropertyScopeInput;
        AudioObjectID ids[4]; sz = sizeof(ids);
        CHECK(DRV->GetPropertyData(gD, kObjectID_Device, 0, &a, 0, NULL, sz, &sz, ids) == 0 && sz == 4 && ids[0] == kObjectID_InputStream);
        a.mSelector = kAudioStreamPropertyVirtualFormat; a.mScope = kAudioObjectPropertyScopeGlobal;
        AudioStreamBasicDescription f; sz = sizeof(f);
        CHECK(DRV->GetPropertyData(gD, kObjectID_OutputStream, 0, &a, 0, NULL, sz, &sz, &f) == 0);
        CHECK(f.mChannelsPerFrame == 16 && f.mSampleRate == 44100.0 && (f.mFormatFlags & kAudioFormatFlagIsFloat));
    }
    CHECK(GetU32(kObjectID_Device, kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal) == kAudioDeviceTransportTypeVirtual);
    CHECK(GetU32(kObjectID_Device, kAudioDevicePropertyDeviceCanBeDefaultDevice, kAudioObjectPropertyScopeOutput) == 1);
    CHECK(GetU32(kObjectID_Device, kAudioDevicePropertyDeviceCanBeDefaultSystemDevice, kAudioObjectPropertyScopeOutput) == 0);
    CHECK(GetU32(kObjectID_Device, kAudioDevicePropertyIsHidden, kAudioObjectPropertyScopeGlobal) == 0);
    CHECK(GetU32(kObjectID_InputStream, kAudioStreamPropertyDirection, kAudioObjectPropertyScopeGlobal) == 1);
    CHECK(GetU32(kObjectID_OutputStream, kAudioStreamPropertyDirection, kAudioObjectPropertyScopeGlobal) == 0);

    CheckName(kAudioObjectPropertyScopeInput, 1, "MPC Out 1");
    CheckName(kAudioObjectPropertyScopeInput, 8, "MPC Out 8");
    CheckName(kAudioObjectPropertyScopeInput, 9, "Computer 1");
    CheckName(kAudioObjectPropertyScopeInput, 16, "Computer 8");
    CheckName(kAudioObjectPropertyScopeOutput, 1, "To MPC 1");
    CheckName(kAudioObjectPropertyScopeOutput, 9, "MPC Engine 1");
    CheckName(kAudioObjectPropertyScopeOutput, 16, "MPC Engine 8");
    {
        AudioObjectPropertyAddress a = { kAudioObjectPropertyElementName, kAudioObjectPropertyScopeInput, 17 };
        CHECK(!DRV->HasProperty(gD, kObjectID_Device, 0, &a));
    }

    // IO: two writers in the same cycle are summed; output c lands on input (c+8)%16.
    CHECK(DRV->StartIO(gD, kObjectID_Device, 1) == 0);
    Boolean will = false, inPlace = false;
    CHECK(DRV->WillDoIOOperation(gD, kObjectID_Device, 1, kAudioServerPlugInIOOperationWriteMix, &will, &inPlace) == 0 && will);
    CHECK(DRV->WillDoIOOperation(gD, kObjectID_Device, 1, kAudioServerPlugInIOOperationProcessOutput, &will, &inPlace) == 0 && !will);

    AudioServerPlugInIOCycleInfo cyc; memset(&cyc, 0, sizeof(cyc));
    Float64 T = 65536.0 * 3 - 100;   // straddles a ring wrap
    cyc.mOutputTime.mSampleTime = T;
    cyc.mInputTime.mSampleTime = T;
    Fill(bufA, 100.0f);
    Fill(bufB, 1000.0f);
    CHECK(DRV->DoIOOperation(gD, kObjectID_Device, kObjectID_OutputStream, 1, kAudioServerPlugInIOOperationWriteMix, N, &cyc, bufA, NULL) == 0);
    CHECK(DRV->DoIOOperation(gD, kObjectID_Device, kObjectID_OutputStream, 2, kAudioServerPlugInIOOperationWriteMix, N, &cyc, bufB, NULL) == 0);
    CHECK(DRV->DoIOOperation(gD, kObjectID_Device, kObjectID_InputStream, 1, kAudioServerPlugInIOOperationReadInput, N, &cyc, in, NULL) == 0);
    int bad = 0;
    for (UInt32 f = 0; f < N; f++) for (UInt32 c = 0; c < kChannels; c++) {
        UInt32 src = (c + kHalf) % kChannels;        // input c came from output src
        Float32 want = (100.0f + src + f * 0.001f) + (1000.0f + src + f * 0.001f);
        if (fabsf(in[f * kChannels + c] - want) > 1e-3f) bad++;
    }
    CHECK(bad == 0);
    printf("swap+sum: in1=%.0f (out9 108 + 1008), in9=%.0f (out1 100 + 1000)\n", in[0], in[8]);

    // Next cycle, single writer: first write must overwrite (not accumulate onto the old cycle).
    cyc.mOutputTime.mSampleTime = cyc.mInputTime.mSampleTime = T + 65536.0;   // same ring slots, new time
    CHECK(DRV->DoIOOperation(gD, kObjectID_Device, kObjectID_OutputStream, 1, kAudioServerPlugInIOOperationWriteMix, N, &cyc, bufA, NULL) == 0);
    CHECK(DRV->DoIOOperation(gD, kObjectID_Device, kObjectID_InputStream, 1, kAudioServerPlugInIOOperationReadInput, N, &cyc, in, NULL) == 0);
    CHECK(fabsf(in[0] - 108.0f) < 1e-3f && fabsf(in[8] - 100.0f) < 1e-3f);

    // A read of a time nobody wrote is silence (no replay after a wrap).
    cyc.mInputTime.mSampleTime = T + 2 * 65536.0;
    CHECK(DRV->DoIOOperation(gD, kObjectID_Device, kObjectID_InputStream, 1, kAudioServerPlugInIOOperationReadInput, N, &cyc, in, NULL) == 0);
    Float32 peak = 0; for (UInt32 i = 0; i < N * kChannels; i++) peak = fmaxf(peak, fabsf(in[i]));
    CHECK(peak == 0.0f);

    // Zero time stamps: monotonic, period-aligned, seed constant.
    Float64 st; UInt64 ht, seed;
    CHECK(DRV->GetZeroTimeStamp(gD, kObjectID_Device, 1, &st, &ht, &seed) == 0);
    CHECK(fmod(st, kZeroTSPeriod) == 0.0 && seed == 1 && ht >= gAnchorHostTime);

    // Sample rate change goes through the host's configuration-change request.
    CHECK(DRV->StopIO(gD, kObjectID_Device, 1) == 0);
    {
        AudioObjectPropertyAddress a = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        Float64 r = 48000.0; UInt32 sz = sizeof(r);
        CHECK(DRV->SetPropertyData(gD, kObjectID_Device, 0, &a, 0, NULL, sz, &r) == 0);
        r = 0; CHECK(DRV->GetPropertyData(gD, kObjectID_Device, 0, &a, 0, NULL, sz, &sz, &r) == 0 && r == 48000.0);
        r = 96000.0; CHECK(DRV->SetPropertyData(gD, kObjectID_Device, 0, &a, 0, NULL, sz, &r) != 0);
    }

    DRV->Release(gD);
    printf(gFails ? "selftest: %d FAILED\n" : "selftest: all checks passed\n", gFails);
    return gFails ? 1 : 0;
}
