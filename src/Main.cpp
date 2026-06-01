#define NOMINMAX

#include <RED4ext/RED4ext.hpp>
#include <RedLib.hpp>
#include "wooting-analog-wrapper.h"
#include <Windows.h>
#include <Psapi.h>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <cmath>
#include <cfloat>

// ============================================================================
// Ring buffer
// ============================================================================

struct RingEntry {
    int64_t offset;
    float   value;
    int32_t _pad;
};

static constexpr int RING_SIZE = 4096;
static constexpr int RING_MASK = RING_SIZE - 1;

static RingEntry             g_ring[RING_SIZE] = {};
static volatile int32_t      g_ringWriteIdx = 0;

// Results
static volatile float        g_steeringValue    = 0.0f;
static volatile int64_t      g_hitCount         = 0;
static volatile int64_t      g_discoveredOffset = 0;
static volatile bool         g_offsetFound      = false;
static volatile int32_t      g_discoveryCount   = 0;  // how many times we've rediscovered

// Hook state
static uint8_t   g_originalBytes[14] = {};
static void*     g_codeCave   = nullptr;
static uintptr_t g_patchAddr  = 0;
static bool      g_installed  = false;

static std::atomic<bool>     g_readerRunning{ false };
static std::thread           g_readerThread;

// ============================================================================
// AOB
// ============================================================================

static const uint8_t AOB_PATTERN[] = {
    0x49, 0x8B, 0x06,
    0x8B, 0x8E, 0x80, 0x00, 0x00, 0x00,
    0xF3, 0x0F, 0x11, 0x04, 0x01
};
static constexpr size_t AOB_LEN = sizeof(AOB_PATTERN);

static uintptr_t AOBScan(uintptr_t base, size_t size,
                          const uint8_t* pattern, size_t patLen)
{
    const uint8_t* mem = reinterpret_cast<const uint8_t*>(base);
    for (size_t i = 0; i <= size - patLen; ++i)
    {
        if (memcmp(mem + i, pattern, patLen) == 0)
            return base + i;
    }
    return 0;
}

// ============================================================================
// Install — ring buffer cave
// ============================================================================
static bool InstallSteeringHook(RED4ext::PluginHandle aHandle, const RED4ext::Sdk* aSdk)
{
    if (g_installed) return true;

    HMODULE hMod = GetModuleHandleW(L"Cyberpunk2077.exe");
    if (!hMod) {
        aSdk->logger->Error(aHandle, "SteeringHook: GetModuleHandle failed");
        return false;
    }

    MODULEINFO modInfo = {};
    if (!GetModuleInformation(GetCurrentProcess(), hMod, &modInfo, sizeof(modInfo))) {
        aSdk->logger->Error(aHandle, "SteeringHook: GetModuleInformation failed");
        return false;
    }

    uintptr_t moduleBase = reinterpret_cast<uintptr_t>(hMod);
    size_t    moduleSize = modInfo.SizeOfImage;

    g_patchAddr = AOBScan(moduleBase, moduleSize, AOB_PATTERN, AOB_LEN);
    if (g_patchAddr == 0) {
        aSdk->logger->Error(aHandle, "SteeringHook: AOB not found");
        return false;
    }

    aSdk->logger->InfoF(aHandle, "SteeringHook: AOB at base+0x%X",
                         (uint32_t)(g_patchAddr - moduleBase));

    uintptr_t returnAddr = g_patchAddr + 14;

    g_codeCave = VirtualAlloc(nullptr, 128, MEM_COMMIT | MEM_RESERVE,
                              PAGE_EXECUTE_READWRITE);
    if (!g_codeCave) {
        aSdk->logger->ErrorF(aHandle, "SteeringHook: VirtualAlloc failed (%lu)",
                              GetLastError());
        return false;
    }

    uint8_t cave[128] = {};
    size_t p = 0;

    // mov rax, [r14] — ORIGINAL
    cave[p++] = 0x49; cave[p++] = 0x8B; cave[p++] = 0x06;
    // push rdx
    cave[p++] = 0x52;
    // mov rcx, &g_ringWriteIdx
    cave[p++] = 0x48; cave[p++] = 0xB9;
    { uintptr_t a = reinterpret_cast<uintptr_t>(&g_ringWriteIdx); memcpy(&cave[p], &a, 8); p += 8; }
    // mov edx, [rcx]
    cave[p++] = 0x8B; cave[p++] = 0x11;
    // inc dword ptr [rcx]
    cave[p++] = 0xFF; cave[p++] = 0x01;
    // and edx, RING_MASK
    cave[p++] = 0x81; cave[p++] = 0xE2;
    { uint32_t mask = RING_MASK; memcpy(&cave[p], &mask, 4); p += 4; }
    // shl edx, 4
    cave[p++] = 0xC1; cave[p++] = 0xE2; cave[p++] = 0x04;
    // mov rcx, &g_ring
    cave[p++] = 0x48; cave[p++] = 0xB9;
    { uintptr_t a = reinterpret_cast<uintptr_t>(&g_ring[0]); memcpy(&cave[p], &a, 8); p += 8; }
    // add rcx, rdx
    cave[p++] = 0x48; cave[p++] = 0x01; cave[p++] = 0xD1;
    // mov [rcx], rax
    cave[p++] = 0x48; cave[p++] = 0x89; cave[p++] = 0x01;
    // movss [rcx+8], xmm0
    cave[p++] = 0xF3; cave[p++] = 0x0F; cave[p++] = 0x11; cave[p++] = 0x41; cave[p++] = 0x08;
    // pop rdx
    cave[p++] = 0x5A;
    // ORIGINAL: mov ecx, [rsi+0x80]
    cave[p++] = 0x8B; cave[p++] = 0x8E;
    cave[p++] = 0x80; cave[p++] = 0x00; cave[p++] = 0x00; cave[p++] = 0x00;
    // ORIGINAL: movss [rcx+rax], xmm0
    cave[p++] = 0xF3; cave[p++] = 0x0F; cave[p++] = 0x11; cave[p++] = 0x04; cave[p++] = 0x01;
    // jmp [rip+0]
    cave[p++] = 0xFF; cave[p++] = 0x25;
    cave[p++] = 0x00; cave[p++] = 0x00; cave[p++] = 0x00; cave[p++] = 0x00;
    memcpy(&cave[p], &returnAddr, 8); p += 8;

    memcpy(g_codeCave, cave, p);

    memcpy(g_originalBytes, reinterpret_cast<void*>(g_patchAddr), 14);

    uint8_t patch[14] = {};
    patch[0] = 0xFF;
    patch[1] = 0x25;
    { uintptr_t ca = reinterpret_cast<uintptr_t>(g_codeCave); memcpy(&patch[6], &ca, 8); }

    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(g_patchAddr), 14,
                        PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        aSdk->logger->ErrorF(aHandle, "SteeringHook: VirtualProtect failed (%lu)",
                              GetLastError());
        VirtualFree(g_codeCave, 0, MEM_RELEASE);
        g_codeCave = nullptr;
        return false;
    }

    memcpy(reinterpret_cast<void*>(g_patchAddr), patch, 14);
    FlushInstructionCache(GetCurrentProcess(),
                          reinterpret_cast<void*>(g_patchAddr), 14);

    g_installed = true;
    aSdk->logger->Info(aHandle, "SteeringHook: INSTALLED");
    return true;
}

static void UninstallSteeringHook()
{
    if (!g_installed) return;

    DWORD oldProtect = 0;
    VirtualProtect(reinterpret_cast<void*>(g_patchAddr), 14,
                   PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy(reinterpret_cast<void*>(g_patchAddr), g_originalBytes, 14);
    FlushInstructionCache(GetCurrentProcess(),
                          reinterpret_cast<void*>(g_patchAddr), 14);
    VirtualProtect(reinterpret_cast<void*>(g_patchAddr), 14, oldProtect, &oldProtect);

    Sleep(50);

    if (g_codeCave) {
        VirtualFree(g_codeCave, 0, MEM_RELEASE);
        g_codeCave = nullptr;
    }
    g_installed = false;
}

// ============================================================================
// Discovery helpers
// ============================================================================

struct OffsetStats {
    int64_t count         = 0;
    int64_t fractionalCnt = 0;
    float   minVal        = FLT_MAX;
    float   maxVal        = -FLT_MAX;

    void add(float v) {
        count++;
        if (v < minVal) minVal = v;
        if (v > maxVal) maxVal = v;
        float av = fabsf(v);
        if (av > 0.01f && fabsf(av - 1.0f) > 0.01f)
            fractionalCnt++;
    }

    bool isAnalog() const { return fractionalCnt > 0; }
};

// Run a discovery pass over the ring buffer for `durationMs` milliseconds.
// Returns the best analog steering offset, or 0 if none found.
static int64_t RunDiscovery(int32_t& readIdx, ULONGLONG durationMs,
                             RED4ext::PluginHandle aHandle, const RED4ext::Sdk* aSdk)
{
    std::unordered_map<int64_t, OffsetStats> stats;
    auto startTime = GetTickCount64();

    while (g_readerRunning.load() &&
           (GetTickCount64() - startTime) < durationMs)
    {
        int32_t writeIdx = g_ringWriteIdx;
        while (readIdx != writeIdx)
        {
            int slot = readIdx & RING_MASK;
            stats[g_ring[slot].offset].add(g_ring[slot].value);
            readIdx++;
        }
        Sleep(1);
    }

    if (stats.empty()) return 0;

    // Pass 1: analog with both negative and positive
    int64_t best = 0;
    int64_t bestFrac = 0;

    for (auto& [offset, st] : stats)
    {
        if (!st.isAnalog()) continue;
        if (st.minVal < -1.1f || st.maxVal > 1.1f) continue;
        if (st.minVal > -0.05f || st.maxVal < 0.05f) continue;
        if (st.count < 30) continue;

        if (st.fractionalCnt > bestFrac) {
            bestFrac = st.fractionalCnt;
            best = offset;
        }
    }

    if (best != 0) {
        aSdk->logger->InfoF(aHandle,
            "SteeringBridge: discovered ANALOG offset=0x%llX frac=%lld",
            (unsigned long long)best, (long long)bestFrac);
        return best;
    }

    // Pass 2: analog with any variance
    for (auto& [offset, st] : stats)
    {
        if (!st.isAnalog()) continue;
        if (st.minVal < -1.1f || st.maxVal > 1.1f) continue;
        if (st.count < 30) continue;

        if (st.fractionalCnt > bestFrac) {
            bestFrac = st.fractionalCnt;
            best = offset;
        }
    }

    if (best != 0) {
        aSdk->logger->InfoF(aHandle,
            "SteeringBridge: discovered ANALOG (relaxed) offset=0x%llX frac=%lld",
            (unsigned long long)best, (long long)bestFrac);
    }

    return best;
}

// ============================================================================
// Reader thread — continuous discovery + tracking with auto-rediscovery
// ============================================================================

static void ReaderThreadFunc(RED4ext::PluginHandle aHandle, const RED4ext::Sdk* aSdk)
{
    int32_t readIdx = g_ringWriteIdx;

    while (g_readerRunning.load())
    {
        // ---- DISCOVERY PHASE ----
        g_offsetFound = false;
        g_steeringValue = 0.0f;
        g_hitCount = 0;

        aSdk->logger->Info(aHandle,
            "SteeringBridge: Searching for steering offset (steer left+right)...");

        int64_t offset = 0;

        // Keep trying 5-second discovery windows until we find it
        while (g_readerRunning.load() && offset == 0)
        {
            offset = RunDiscovery(readIdx, 5000, aHandle, aSdk);

            if (offset == 0) {
                // Drain the ring buffer so we don't re-analyze stale data
                readIdx = g_ringWriteIdx;
            }
        }

        if (!g_readerRunning.load()) break;

        g_discoveredOffset = offset;
        g_offsetFound = true;
        g_discoveryCount++;

        aSdk->logger->InfoF(aHandle,
            "SteeringBridge: LOCKED offset=0x%llX (discovery #%d)",
            (unsigned long long)offset, (int)g_discoveryCount);

        // ---- TRACKING PHASE ----
        // Read ring buffer and update g_steeringValue for matched offset.
        // If we go 2 seconds with no hits while ring is active, assume
        // vehicle changed → break back to discovery.

        ULONGLONG lastHitTime = GetTickCount64();
        int64_t   prevHitCount = g_hitCount;
        constexpr ULONGLONG STALE_MS = 2000;

        while (g_readerRunning.load())
        {
            int32_t writeIdx = g_ringWriteIdx;
            bool gotRingData = (readIdx != writeIdx);

            while (readIdx != writeIdx)
            {
                int slot = readIdx & RING_MASK;
                if (g_ring[slot].offset == offset)
                {
                    g_steeringValue = g_ring[slot].value;
                    g_hitCount++;
                }
                readIdx++;
            }

            // Check for stale offset
            if (g_hitCount > prevHitCount) {
                lastHitTime = GetTickCount64();
                prevHitCount = g_hitCount;
            }
            else if (gotRingData &&
                     (GetTickCount64() - lastHitTime) > STALE_MS)
            {
                // Ring is getting data but our offset isn't showing up
                aSdk->logger->InfoF(aHandle,
                    "SteeringBridge: offset 0x%llX went stale — rediscovering...",
                    (unsigned long long)offset);
                g_steeringValue = 0.0f;
                break;  // back to discovery
            }

            Sleep(1);
        }
    }
}

// ============================================================================
// RedLib RTTI
// ============================================================================

class WootingBridge : public Red::IScriptable {
public:
    static float GetAnalog(int32_t keyCode) {
        return wooting_analog_read_analog((uint16_t)keyCode);
    }
    static float GetSteeringPosition() {
        return g_steeringValue;
    }
    static bool IsSteeringCaptured() {
        return g_installed && g_offsetFound && g_hitCount > 0;
    }
    static int64_t GetHookHitCount() {
        return g_hitCount;
    }
    static int64_t GetDiscoveredOffset() {
        return g_discoveredOffset;
    }
    static bool IsOffsetFound() {
        return g_offsetFound;
    }
    static int32_t GetDiscoveryCount() {
        return g_discoveryCount;
    }
    RTTI_IMPL_TYPEINFO(WootingBridge);
    RTTI_IMPL_ALLOCATOR();
};

RTTI_DEFINE_CLASS(WootingBridge, {
    RTTI_METHOD(GetAnalog);
    RTTI_METHOD(GetSteeringPosition);
    RTTI_METHOD(IsSteeringCaptured);
    RTTI_METHOD(GetHookHitCount);
    RTTI_METHOD(GetDiscoveredOffset);
    RTTI_METHOD(IsOffsetFound);
    RTTI_METHOD(GetDiscoveryCount);
});

// ============================================================================
// RED4ext entry
// ============================================================================

RED4EXT_C_EXPORT void RED4EXT_CALL Query(RED4ext::PluginInfo* aInfo)
{
    aInfo->name    = L"WootingAnalogBridge";
    aInfo->author  = L"Modder";
    aInfo->version = RED4EXT_SEMVER(1, 0, 0);
    aInfo->runtime = RED4EXT_RUNTIME_LATEST;
    aInfo->sdk     = RED4EXT_SDK_LATEST;
}

RED4EXT_C_EXPORT uint32_t RED4EXT_CALL Supports() { return 0; }

RED4EXT_C_EXPORT bool RED4EXT_CALL Main(RED4ext::PluginHandle aHandle,
                                         RED4ext::EMainReason aReason,
                                         const RED4ext::Sdk*  aSdk)
{
    switch (aReason)
    {
    case RED4ext::EMainReason::Load:
    {
        wooting_analog_initialise();
        Red::TypeInfoRegistrar::RegisterDiscovered();

        std::thread([aHandle, aSdk]()
        {
            Sleep(5000);
            if (!InstallSteeringHook(aHandle, aSdk)) {
                aSdk->logger->Error(aHandle, "SteeringBridge: hook install FAILED");
                return;
            }

            g_readerRunning.store(true);
            g_readerThread = std::thread(ReaderThreadFunc, aHandle, aSdk);

        }).detach();

        break;
    }
    case RED4ext::EMainReason::Unload:
    {
        g_readerRunning.store(false);
        if (g_readerThread.joinable())
            g_readerThread.join();

        UninstallSteeringHook();
        wooting_analog_uninitialise();
        break;
    }
    }
    return true;
}