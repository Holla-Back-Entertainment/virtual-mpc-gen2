// Offline self-check for VirtualMPC.c: drives the plug-in through its factory and driver interface
// with a fake host (no coreaudiod), for both device pairs and both directions.
// Build/run: driver/build.sh test
#include "VirtualMPC.c"
#include <assert.h>
#include <math.h>

static AudioServerPlugInDriverRef gD;
#define DRV (*gD)

static int gRequests = 0;
static OSStatus FakePropertiesChanged(AudioServerPlugInHostRef h, AudioObjectID o, UInt32 n, const AudioObjectPropertyAddress* a) {
    (void)h; (void)o; (void)n; (void)a; return 0;
}
static OSStatus FakeCopy(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef* d) { (void)h; (void)k; *d = NULL; return 0; }
static OSStatus FakeWrite(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef d) { (void)h; (void)k; (void)d; return 0; }
static OSStatus FakeDelete(AudioServerPlugInHostRef h, CFStringRef k) { (void)h; (void)k; return 0; }
static OSStatus FakeRequest(AudioServerPlugInHostRef h, AudioObjectID dev, UInt64 action, void* info) {
    (void)h; gRequests++;
    return DRV->PerformDeviceConfigurationChange(gD, dev, action, info);   // host would defer this
}
static AudioServerPlugInHostInterface gFakeHost = { FakePropertiesChanged, FakeCopy, FakeWrite, FakeDelete, FakeRequest };

static int gFails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); gFails++; } } while (0)

enum { XL = 0, XLE = 1, L3 = 2, L3E = 3 };   // device indices

static Boolean StrIs(CFStringRef s, const char* want) {
    char buf[96] = {0};
    if (s) { CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8); CFRelease(s); }
    if (strcmp(buf, want) != 0) { printf("  got '%s' want '%s'\n", buf, want); return false; }
    return true;
}

static CFStringRef GetStr(AudioObjectID o, AudioObjectPropertySelector sel, AudioObjectPropertyScope sc, UInt32 el) {
    AudioObjectPropertyAddress a = { sel, sc, el };
    CFStringRef s = NULL; UInt32 sz = sizeof(s);
    if (DRV->GetPropertyData(gD, o, 0, &a, 0, NULL, sz, &sz, &s) != 0) return NULL;
    return s;
}

static void CheckName(UInt32 dev, AudioObjectPropertyScope scope, UInt32 el, const char* want) {
    AudioObjectPropertyAddress a = { kAudioObjectPropertyElementName, scope, el };
    CHECK(DRV->HasProperty(gD, DevObj(dev), 0, &a));
    if (!StrIs(GetStr(DevObj(dev), kAudioObjectPropertyElementName, scope, el), want)) {
        printf("FAIL name dev=%u scope=%c el=%u\n", dev, (char)(scope >> 24), el); gFails++;
    }
}

static UInt32 GetU32(AudioObjectID o, AudioObjectPropertySelector sel, AudioObjectPropertyScope sc) {
    AudioObjectPropertyAddress a = { sel, sc, kAudioObjectPropertyElementMain };
    UInt32 v = 0xFFFFFFFF, sz = sizeof(v);
    CHECK(DRV->GetPropertyData(gD, o, 0, &a, 0, NULL, sz, &sz, &v) == 0);
    return v;
}

static Float64 GetRate(UInt32 dev) {
    AudioObjectPropertyAddress a = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    Float64 r = 0; UInt32 sz = sizeof(r);
    CHECK(DRV->GetPropertyData(gD, DevObj(dev), 0, &a, 0, NULL, sz, &sz, &r) == 0);
    return r;
}

static UInt32 StreamChannels(AudioObjectID stream) {
    AudioObjectPropertyAddress a = { kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioStreamBasicDescription f; UInt32 sz = sizeof(f);
    CHECK(DRV->GetPropertyData(gD, stream, 0, &a, 0, NULL, sz, &sz, &f) == 0);
    CHECK(f.mBytesPerFrame == f.mChannelsPerFrame * 4 && (f.mFormatFlags & kAudioFormatFlagIsFloat));
    return f.mChannelsPerFrame;
}

static AudioObjectID UIDToDevice(const char* uid) {
    AudioObjectPropertyAddress a = { kAudioPlugInPropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef u = CFStringCreateWithCString(NULL, uid, kCFStringEncodingUTF8);
    AudioObjectID dev = 12345; UInt32 sz = sizeof(dev);
    CHECK(DRV->GetPropertyData(gD, kObjectID_PlugIn, 0, &a, sizeof(u), &u, sz, &sz, &dev) == 0);
    CFRelease(u);
    return dev;
}

#define N 512
static Float32 bufA[N * kMaxChannels], bufB[N * kMaxChannels], in[N * kMaxChannels];

static void Fill(Float32* b, UInt32 ch, Float32 base) {   // value encodes source channel and frame
    for (UInt32 f = 0; f < N; f++) for (UInt32 c = 0; c < ch; c++) b[f * ch + c] = base + (Float32)c + (Float32)f * 0.001f;
}

static void IO(UInt32 dev, UInt32 op, Float64 t, void* buf) {
    AudioServerPlugInIOCycleInfo cyc; memset(&cyc, 0, sizeof(cyc));
    cyc.mOutputTime.mSampleTime = t;
    cyc.mInputTime.mSampleTime = t;
    AudioObjectID stream = op == kAudioServerPlugInIOOperationWriteMix ? OutStreamObj(dev) : InStreamObj(dev);
    CHECK(DRV->DoIOOperation(gD, DevObj(dev), stream, 1, op, N, &cyc, buf, NULL) == 0);
}
#define WRITE(dev, t, buf) IO(dev, kAudioServerPlugInIOOperationWriteMix, t, buf)
#define READ(dev, t, buf)  IO(dev, kAudioServerPlugInIOOperationReadInput, t, buf)

// writer's output channel c must land on reader's input channel c, summed over the given bases.
static int CheckPath(UInt32 ch, Float32 sumBase, int writers) {
    int bad = 0;
    for (UInt32 f = 0; f < N; f++) for (UInt32 c = 0; c < ch; c++) {
        Float32 want = sumBase + (Float32)writers * ((Float32)c + (Float32)f * 0.001f);
        if (fabsf(in[f * ch + c] - want) > 2e-3f) bad++;
    }
    return bad;
}

static Float32 Peak(UInt32 ch) { Float32 p = 0; for (UInt32 i = 0; i < N * ch; i++) p = fmaxf(p, fabsf(in[i])); return p; }

int main(void) {
    // Factory + QueryInterface, as CFPlugIn would do.
    CHECK(VirtualMPC_Create(NULL, kAudioServerPlugInDriverInterfaceUUID) == NULL);
    void* unk = VirtualMPC_Create(NULL, kAudioServerPlugInTypeUUID);
    CHECK(unk != NULL);
    void* iface = NULL;
    CHECK((*(AudioServerPlugInDriverRef)unk)->QueryInterface(unk, CFUUIDGetUUIDBytes(kAudioServerPlugInDriverInterfaceUUID), &iface) == S_OK);
    gD = (AudioServerPlugInDriverRef)iface;
    CHECK(DRV->Initialize(gD, &gFakeHost) == 0);

    // Device list + UIDs. The XL keeps its old UID so Live keeps its audio setting.
    {
        AudioObjectPropertyAddress a = { kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        AudioObjectID ids[8]; UInt32 sz = sizeof(ids);
        CHECK(DRV->GetPropertyData(gD, kObjectID_PlugIn, 0, &a, 0, NULL, sz, &sz, ids) == 0 && sz == 4 * sizeof(AudioObjectID));
    }
    CHECK(UIDToDevice("VirtualMPCXL_UID") == DevObj(XL));
    CHECK(UIDToDevice("VirtualMPCXL_Engine_UID") == DevObj(XLE));
    CHECK(UIDToDevice("VirtualMPCLive3_UID") == DevObj(L3));
    CHECK(UIDToDevice("VirtualMPCLive3_Engine_UID") == DevObj(L3E));
    CHECK(UIDToDevice("nope") == kAudioObjectUnknown);
    CHECK(StrIs(GetStr(DevObj(XL), kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, 0), "Virtual MPC XL"));
    CHECK(StrIs(GetStr(DevObj(XLE), kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, 0), "Virtual MPC XL Engine"));
    CHECK(StrIs(GetStr(DevObj(L3), kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, 0), "Virtual MPC Live III"));
    CHECK(StrIs(GetStr(DevObj(L3E), kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, 0), "VirtualMPCLive3_Engine_UID"));

    // Channel counts: XL 32 in / 30 out, Live III 30 in / 28 out; engines mirrored.
    CHECK(StreamChannels(InStreamObj(XL)) == 32 && StreamChannels(OutStreamObj(XL)) == 30);
    CHECK(StreamChannels(InStreamObj(XLE)) == 30 && StreamChannels(OutStreamObj(XLE)) == 32);
    CHECK(StreamChannels(InStreamObj(L3)) == 30 && StreamChannels(OutStreamObj(L3)) == 28);
    CHECK(StreamChannels(InStreamObj(L3E)) == 28 && StreamChannels(OutStreamObj(L3E)) == 30);
    {
        AudioObjectPropertyAddress a = { kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain };
        AudioObjectID ids[4]; UInt32 sz = sizeof(ids);
        CHECK(DRV->GetPropertyData(gD, DevObj(L3), 0, &a, 0, NULL, sz, &sz, ids) == 0 && sz == 4 && ids[0] == InStreamObj(L3));
        a.mScope = kAudioObjectPropertyScopeGlobal; sz = sizeof(ids);
        CHECK(DRV->GetPropertyData(gD, DevObj(XLE), 0, &a, 0, NULL, sz, &sz, ids) == 0 && sz == 8 && ids[1] == OutStreamObj(XLE));
    }

    // Visibility: user devices visible + can be default, engines hidden and never default.
    for (UInt32 i = 0; i < kNumDevices; i++) {
        Boolean eng = i & 1;
        CHECK(GetU32(DevObj(i), kAudioDevicePropertyIsHidden, kAudioObjectPropertyScopeGlobal) == (eng ? 1u : 0u));
        CHECK(GetU32(DevObj(i), kAudioDevicePropertyDeviceCanBeDefaultDevice, kAudioObjectPropertyScopeOutput) == (eng ? 0u : 1u));
        CHECK(GetU32(DevObj(i), kAudioDevicePropertyDeviceCanBeDefaultSystemDevice, kAudioObjectPropertyScopeOutput) == 0);
        CHECK(GetU32(DevObj(i), kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal) == kAudioDeviceTransportTypeVirtual);
        CHECK(GetU32(InStreamObj(i), kAudioStreamPropertyDirection, kAudioObjectPropertyScopeGlobal) == 1);
        CHECK(GetU32(OutStreamObj(i), kAudioStreamPropertyDirection, kAudioObjectPropertyScopeGlobal) == 0);
        AudioObjectPropertyAddress a = { kAudioObjectPropertyOwner, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        AudioObjectID owner = 0; UInt32 sz = sizeof(owner);
        CHECK(DRV->GetPropertyData(gD, OutStreamObj(i), 0, &a, 0, NULL, sz, &sz, &owner) == 0 && owner == DevObj(i));
    }
    CHECK(GetU32(DevObj(XL), kAudioDevicePropertyClockDomain, 0) == GetU32(DevObj(XLE), kAudioDevicePropertyClockDomain, 0));
    CHECK(GetU32(DevObj(XL), kAudioDevicePropertyClockDomain, 0) != GetU32(DevObj(L3), kAudioDevicePropertyClockDomain, 0));

    // Channel names (what Live shows).
    CheckName(XL, kAudioObjectPropertyScopeInput, 1, "MPC Out 1");
    CheckName(XL, kAudioObjectPropertyScopeInput, 8, "MPC Out 8");
    CheckName(XL, kAudioObjectPropertyScopeInput, 9, "MPC USB Out 1");
    CheckName(XL, kAudioObjectPropertyScopeInput, 32, "MPC USB Out 24");
    CheckName(XL, kAudioObjectPropertyScopeOutput, 1, "Monitor 1");
    CheckName(XL, kAudioObjectPropertyScopeOutput, 2, "Monitor 2");
    CheckName(XL, kAudioObjectPropertyScopeOutput, 3, "MPC In 1");
    CheckName(XL, kAudioObjectPropertyScopeOutput, 6, "MPC In 4");
    CheckName(XL, kAudioObjectPropertyScopeOutput, 7, "MPC USB In 1");
    CheckName(XL, kAudioObjectPropertyScopeOutput, 30, "MPC USB In 24");
    CheckName(L3, kAudioObjectPropertyScopeInput, 6, "MPC Out 6");
    CheckName(L3, kAudioObjectPropertyScopeInput, 7, "MPC USB Out 1");
    CheckName(L3, kAudioObjectPropertyScopeInput, 30, "MPC USB Out 24");
    CheckName(L3, kAudioObjectPropertyScopeOutput, 4, "MPC In 2");
    CheckName(L3, kAudioObjectPropertyScopeOutput, 5, "MPC USB In 1");
    CheckName(L3, kAudioObjectPropertyScopeOutput, 28, "MPC USB In 24");
    CheckName(XLE, kAudioObjectPropertyScopeOutput, 9, "MPC USB Out 1");   // engine out n = user in n
    CheckName(L3E, kAudioObjectPropertyScopeInput, 1, "Monitor 1");        // engine in n = user out n
    {
        AudioObjectPropertyAddress a = { kAudioObjectPropertyElementName, kAudioObjectPropertyScopeInput, 33 };
        CHECK(!DRV->HasProperty(gD, DevObj(XL), 0, &a));
        a.mElement = 31; CHECK(!DRV->HasProperty(gD, DevObj(L3), 0, &a));
        a.mScope = kAudioObjectPropertyScopeOutput; a.mElement = 29; CHECK(!DRV->HasProperty(gD, DevObj(L3), 0, &a));
        a.mElement = 0; CHECK(!DRV->HasProperty(gD, DevObj(XL), 0, &a));
    }

    // IO, both pairs running.
    for (UInt32 i = 0; i < kNumDevices; i++) CHECK(DRV->StartIO(gD, DevObj(i), 1) == 0);
    {
        Boolean will = false, inPlace = false;
        CHECK(DRV->WillDoIOOperation(gD, DevObj(XLE), 1, kAudioServerPlugInIOOperationWriteMix, &will, &inPlace) == 0 && will);
        CHECK(DRV->WillDoIOOperation(gD, DevObj(XL), 1, kAudioServerPlugInIOOperationProcessOutput, &will, &inPlace) == 0 && !will);
        CHECK(GetU32(DevObj(L3E), kAudioDevicePropertyDeviceIsRunning, 0) == 1);
    }
    Float64 T = 32768.0 * 3 - 100;   // straddles a ring wrap

    // Engine -> user (the MPC's outs): 32 ch on the XL, two writers in one cycle are summed.
    Fill(bufA, 32, 100.0f); Fill(bufB, 32, 1000.0f);
    WRITE(XLE, T, bufA); WRITE(XLE, T, bufB);
    READ(XL, T, in);
    CHECK(CheckPath(32, 1100.0f, 2) == 0);
    printf("XL engine->user: in1=%.0f in32=%.0f (100+1000 + 2*ch)\n", in[0], in[31]);

    // User -> engine (Live's outs): 30 ch on the XL.
    Fill(bufA, 30, 200.0f);
    WRITE(XL, T, bufA);
    READ(XLE, T, in);
    CHECK(CheckPath(30, 200.0f, 1) == 0);
    // ...and it never loops back onto the user's own input: XL input still carries only the engine's audio.
    READ(XL, T, in);
    CHECK(CheckPath(32, 1100.0f, 2) == 0);

    // Live III pair: 30 ch engine->user, 28 ch user->engine, independent of the XL pair.
    Fill(bufA, 30, 300.0f);
    WRITE(L3E, T, bufA);
    READ(L3, T, in);
    CHECK(CheckPath(30, 300.0f, 1) == 0);
    Fill(bufA, 28, 400.0f);
    WRITE(L3, T, bufA);
    READ(L3E, T, in);
    CHECK(CheckPath(28, 400.0f, 1) == 0);
    READ(XL, T + N, in);                                    // nothing written to the XL there
    CHECK(Peak(32) == 0.0f);

    // Next lap of the ring, single writer: first write overwrites (no accumulation onto the old lap).
    Fill(bufA, 32, 100.0f);
    WRITE(XLE, T + kRingFrames, bufA);
    READ(XL, T + kRingFrames, in);
    CHECK(CheckPath(32, 100.0f, 1) == 0);

    // A read of a time nobody wrote is silence (no replay after a wrap), both directions.
    READ(XL, T + 2 * kRingFrames, in);  CHECK(Peak(32) == 0.0f);
    READ(XLE, T + 2 * kRingFrames, in); CHECK(Peak(30) == 0.0f);
    READ(L3E, T + kRingFrames, in);     CHECK(Peak(28) == 0.0f);

    // Zero time stamps: period-aligned, and partners report the same time line.
    {
        Float64 s1, s2; UInt64 h1, h2, seed1, seed2;
        CHECK(DRV->GetZeroTimeStamp(gD, DevObj(XL), 1, &s1, &h1, &seed1) == 0);
        CHECK(DRV->GetZeroTimeStamp(gD, DevObj(XLE), 1, &s2, &h2, &seed2) == 0);
        CHECK(fmod(s1, kZeroTSPeriod) == 0.0 && s1 == s2 && h1 == h2 && seed1 == seed2);
        CHECK(h1 >= gPair[0].anchorHostTime);
    }

    // Stopping one side leaves the pair's time line running for the other.
    CHECK(DRV->StopIO(gD, DevObj(XLE), 1) == 0);
    CHECK(GetU32(DevObj(XLE), kAudioDevicePropertyDeviceIsRunning, 0) == 0);
    CHECK(GetU32(DevObj(XL), kAudioDevicePropertyDeviceIsRunning, 0) == 1);
    UInt64 anchor = gPair[0].anchorHostTime;
    CHECK(DRV->StartIO(gD, DevObj(XLE), 1) == 0);
    CHECK(gPair[0].anchorHostTime == anchor);
    for (UInt32 i = 0; i < kNumDevices; i++) CHECK(DRV->StopIO(gD, DevObj(i), 1) == 0);

    // Sample rate: set on the user device (Live) -> the engine follows; the other pair does not.
    {
        AudioObjectPropertyAddress a = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        Float64 r = 48000.0; UInt32 sz = sizeof(r);
        UInt64 seed0 = gPair[0].seed;
        CHECK(DRV->SetPropertyData(gD, DevObj(XL), 0, &a, 0, NULL, sz, &r) == 0);
        CHECK(GetRate(XL) == 48000.0 && GetRate(XLE) == 48000.0 && GetRate(L3) == 44100.0 && GetRate(L3E) == 44100.0);
        CHECK(gRequests == 2 && gPair[0].seed > seed0);
        // ...and set on the engine (the app) -> the user device follows.
        r = 44100.0;
        CHECK(DRV->SetPropertyData(gD, DevObj(XLE), 0, &a, 0, NULL, sz, &r) == 0);
        CHECK(GetRate(XL) == 44100.0 && GetRate(XLE) == 44100.0);
        r = 48000.0;
        CHECK(DRV->SetPropertyData(gD, DevObj(L3E), 0, &a, 0, NULL, sz, &r) == 0);
        CHECK(GetRate(L3) == 48000.0 && GetRate(L3E) == 48000.0 && GetRate(XL) == 44100.0);
        int before = gRequests;
        CHECK(DRV->SetPropertyData(gD, DevObj(L3), 0, &a, 0, NULL, sz, &r) == 0 && gRequests == before);   // no-op
        r = 96000.0; CHECK(DRV->SetPropertyData(gD, DevObj(XL), 0, &a, 0, NULL, sz, &r) != 0);
        // stream format: right channel count only
        AudioStreamBasicDescription f; FillASBD(&f, 44100.0, 28);
        a.mSelector = kAudioStreamPropertyVirtualFormat; sz = sizeof(f);
        CHECK(DRV->SetPropertyData(gD, OutStreamObj(L3), 0, &a, 0, NULL, sz, &f) == 0 && GetRate(L3E) == 44100.0);
        FillASBD(&f, 44100.0, 16);
        CHECK(DRV->SetPropertyData(gD, OutStreamObj(XL), 0, &a, 0, NULL, sz, &f) == kAudioDeviceUnsupportedFormatError);
    }

    // Rate changed while running: both ends still meet at the same sample time.
    for (UInt32 i = 0; i < kNumDevices; i++) CHECK(DRV->StartIO(gD, DevObj(i), 1) == 0);
    Fill(bufA, 28, 500.0f);
    WRITE(L3, 1000.0, bufA);
    READ(L3E, 1000.0, in);
    CHECK(CheckPath(28, 500.0f, 1) == 0);
    for (UInt32 i = 0; i < kNumDevices; i++) CHECK(DRV->StopIO(gD, DevObj(i), 1) == 0);

    // Bad objects are refused.
    CHECK(DRV->StartIO(gD, InStreamObj(XL), 1) == kAudioHardwareBadDeviceError);
    CHECK(DRV->StartIO(gD, 99, 1) == kAudioHardwareBadDeviceError);

    DRV->Release(gD);
    printf(gFails ? "selftest: %d FAILED\n" : "selftest: all checks passed\n", gFails);
    return gFails ? 1 : 0;
}
