#include "build_variant.h"
#include "unified_control_paths.h"
#include "status_transport.h"
#include "ampere_policy.h"
#include "nvidia_mfg_policy.h"
#include "ui_dynamic_mfg.h"
#include "universal_route_policy.h"
#include "reshade_frontend.h"

#include "backend_bridge.h"
#include "ampere_diagnostics.h"
#include "reflex_control.h"
#include "vsync_control.h"

#include <imgui.h>
#include "ui_host.h"
#include "standalone_ui.h"
#include "single_module.h"
#include "witcher_dots/witcher_dots.h"

#include <Psapi.h>
#include <d3d12.h>
#include <dxgi.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include "ui_status_json.h"
#include "xess_ui_state.h"
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <string>

namespace
{
constexpr const char* kConfigSection = MFG_PRODUCT;
constexpr const char* kOverlayTitle = MFG_OVERLAY_TITLE;
constexpr const char* kReShadeHomeWindow = "###home";

std::atomic<uint32_t> gGpuFamily{0};
bool UiAmpere() noexcept { return gGpuFamily.load(std::memory_order_acquire) == 2u; }

std::atomic<bool> gRegistered{false};
std::atomic<bool> gInitialized{false};
HMODULE gSelf = nullptr;
HMODULE gReShade = nullptr;
bool gDefaultDockInitialized = false;
bool gOverlayVisitedThisFrame = false;
bool gOverlayDockedThisFrame = false;
enum class BackendKind : uint32_t
{
    eNone = 0,
    eNativeFile = 2,
};

HMODULE gBackend = nullptr;
MfgUnlockBackendInterface gBackendInterface{};
std::atomic<bool> gBackendConnected{false};
std::atomic<BackendKind> gBackendKind{BackendKind::eNone};
SRWLOCK gBackendConnectionLock = SRWLOCK_INIT;
std::wstring gNativeConfigPath;
std::wstring gNativeStatusPath;
// Display-only state. Control writes always obtain their own current snapshot.
MfgUnlockReShadeSnapshot gLastNativeDisplay{};
std::wstring gLastNativeDisplayPath;
bool gHaveNativeDisplay = false;
using MfgUnlockSampleFrameTelemetryFn = BOOL (WINAPI*)();
MfgUnlockSampleFrameTelemetryFn gSampleFrameTelemetry = nullptr;
std::atomic<uintptr_t> gTelemetrySwapchain{0};
std::atomic<uint64_t> gTelemetrySwapchainTick{0};

bool ConnectBackend() noexcept;
SRWLOCK gPresentationQueueLock = SRWLOCK_INIT;
ID3D12CommandQueue* gPresentationQueue = nullptr;

int gMultiplier = 2;
bool gFollowGameMode = true;
bool gDynamicMode = false;
int gDynamicTargetFrameRate = 0;
bool gDynamicLockToRefreshRate = true;
int gDynamicCustomTargetFrameRate = 60;
int gDlssgPreset = 2;
int gVsyncMode = 0;
int gReflexFrameLimitFps = 0;
int gReflexCustomLimitFps = 120;
bool gPresentationSaveFailed = false;
bool gPresetSaveFailed = false;
bool gDynamicExperimental56 = false;
bool gGeneratedOnlyDebug = false;
bool gLastApplyAttempted = false;
bool gLastApplyAccepted = false;
bool gLastNativeConfigPersisted = true;


BOOL BackendGetSnapshot(MfgUnlockReShadeSnapshot* snapshot) noexcept
{
    if (!ConnectBackend() || !gBackendInterface.getSnapshot
        || !gBackendInterface.getSnapshot(snapshot))
    {
        return FALSE;
    }
    return TRUE;
}

BOOL BackendApplyControl(uint32_t multiplier, BOOL dynamicMode,
    uint32_t dynamicTargetFrameRate, BOOL dynamicExperimental56,
    BOOL generatedOnlyDebug) noexcept
{
    return ConnectBackend() && gBackendInterface.applyControl
        ? gBackendInterface.applyControl(multiplier, dynamicMode,
            dynamicTargetFrameRate, dynamicExperimental56,
            generatedOnlyDebug) : FALSE;
}

BOOL BackendRegisterDevice(ID3D12Device* device) noexcept
{
    return ConnectBackend() && gBackendInterface.registerD3D12Device
        ? gBackendInterface.registerD3D12Device(device) : FALSE;
}

BOOL BackendRegisterQueue(ID3D12CommandQueue* queue) noexcept
{
    return ConnectBackend() && gBackendInterface.registerD3D12Queue
        ? gBackendInterface.registerD3D12Queue(queue) : FALSE;
}

BOOL BackendRegisterSwapchain(IDXGISwapChain* swapchain,
    IUnknown* presentationQueue) noexcept
{
    return ConnectBackend() && gBackendInterface.registerD3D12Swapchain
        ? gBackendInterface.registerD3D12Swapchain(
            swapchain, presentationQueue) : FALSE;
}

BOOL BackendUnregisterSwapchain(IDXGISwapChain* swapchain) noexcept
{
    return ConnectBackend() && gBackendInterface.unregisterD3D12Swapchain
        ? gBackendInterface.unregisterD3D12Swapchain(swapchain) : FALSE;
}

bool BackendUsesNativeFile() noexcept
{
    return gBackendKind.load(std::memory_order_acquire)
        == BackendKind::eNativeFile;
}

bool FindJsonValue(const std::string& content, const char* name,
    size_t& offset) noexcept
{
    const std::string key = std::string("\"") + name + "\"";
    const size_t keyOffset = content.find(key);
    if (keyOffset == std::string::npos)
        return false;
    const size_t colon = content.find(':', keyOffset + key.size());
    if (colon == std::string::npos)
        return false;
    offset = content.find_first_not_of(" \t\r\n", colon + 1);
    return offset != std::string::npos;
}

bool ParseJsonBool(const std::string& content, const char* name,
    BOOL& value) noexcept
{
    size_t offset = 0;
    if (!FindJsonValue(content, name, offset))
        return false;
    if (content.compare(offset, 4, "true") == 0
        && ui_status_json::ValueEnd(content, offset+4))
    {
        value = TRUE;
        return true;
    }
    if (content.compare(offset, 5, "false") == 0
        && ui_status_json::ValueEnd(content, offset+5))
    {
        value = FALSE;
        return true;
    }
    return false;
}

template <typename T>
bool ParseJsonInteger(const std::string& content, const char* name,
    T& value) noexcept
{
    size_t offset = 0;
    if (!FindJsonValue(content, name, offset))
        return false;
    T parsed{};
    const auto result = std::from_chars(content.data()+offset,
        content.data()+content.size(), parsed);
    if (result.ec != std::errc{} || !ui_status_json::ValueEnd(content,
            static_cast<size_t>(result.ptr-content.data())))
        return false;
    value = parsed;
    return true;
}

bool ParseJsonFloat(const std::string& content, const char* name, float& value) noexcept
{
    size_t offset = 0;
    if (!FindJsonValue(content, name, offset)) return false;
    float parsed = 0.0f;
    const auto result = std::from_chars(content.data()+offset,
        content.data()+content.size(), parsed);
    if (result.ec != std::errc{} || !std::isfinite(parsed)
        || !ui_status_json::ValueEnd(content,
            static_cast<size_t>(result.ptr-content.data()))) return false;
    value = parsed;
    return true;
}

bool ParseJsonString(const std::string& content, const char* name,
    std::string& value) noexcept
{
    size_t offset = 0;
    if (!FindJsonValue(content, name, offset)
        || content[offset] != '"')
    {
        return false;
    }
    value.clear();
    for (size_t index = offset + 1; index < content.size(); ++index)
    {
        const char current = content[index];
        if (current == '"')
            return true;
        if (current != '\\')
        {
            value.push_back(current);
            continue;
        }
        if (++index >= content.size())
            return false;
        switch (content[index])
        {
        case '"': value.push_back('"'); break;
        case '\\': value.push_back('\\'); break;
        case '/': value.push_back('/'); break;
        case 'b': value.push_back('\b'); break;
        case 'f': value.push_back('\f'); break;
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        default: return false;
        }
    }
    return false;
}

bool ReadTextFile(const std::wstring& path, std::string& content) noexcept
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    const bool validSize = GetFileSizeEx(file, &size)
        && size.QuadPart > 0 && size.QuadPart <= 1024 * 1024;
    if (!validSize)
    {
        CloseHandle(file);
        return false;
    }
    content.resize(static_cast<size_t>(size.QuadPart));
    DWORD bytesRead = 0;
    const BOOL read = ReadFile(file, content.data(),
        static_cast<DWORD>(content.size()), &bytesRead, nullptr);
    CloseHandle(file);
    if (!read || bytesRead != content.size())
    {
        content.clear();
        return false;
    }
    return true;
}

bool ResolveNativeFilePaths() noexcept
{
    std::wstring executablePath(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executablePath.data(),
        static_cast<DWORD>(executablePath.size()));
    if (length == 0 || length >= executablePath.size())
        return false;
    executablePath.resize(length);
    const std::filesystem::path directory =
        std::filesystem::path(executablePath).parent_path();
    gNativeConfigPath = unified_control_paths::Config(directory.wstring());
    gNativeStatusPath = unified_control_paths::Status(gNativeConfigPath, directory.wstring());
    return true;
}

BOOL ParseNativeControlSnapshot(const std::string& control,
    MfgUnlockReShadeSnapshot* snapshot)
{
    if (!snapshot || snapshot->structSize < sizeof(*snapshot))
        return FALSE;
    MfgUnlockReShadeSnapshot output{};
    output.followGameMode = FALSE;
    output.intervalLoggingEnabled = TRUE;
    output.frontendClients = 1;
    strcpy_s(output.patchRoute, "pending");

    if (!ui_status_json::CompleteObject(control)
        || !ParseJsonInteger(control, "multiplier", output.desiredMultiplier)
        || output.desiredMultiplier < 1 || output.desiredMultiplier > 6) return FALSE;
    {
        size_t offset = 0;
        if (FindJsonValue(control, "followGame", offset)
            && !ParseJsonBool(control, "followGame", output.followGameMode)) return FALSE;
        if (FindJsonValue(control, "dynamicTargetFrameRate", offset)
            && (!ParseJsonInteger(control, "dynamicTargetFrameRate", output.dynamicTargetFrameRate)
                || output.dynamicTargetFrameRate > 1000)) return FALSE;
        if (FindJsonValue(control, "dlssgPreset", offset)
            && (!ParseJsonInteger(control, "dlssgPreset", output.dlssgPresetRequested)
                || output.dlssgPresetRequested > 3)) return FALSE;
        if (FindJsonValue(control, "vsyncMode", offset)
            && (!ParseJsonInteger(control, "vsyncMode", output.vsyncMode)
                || output.vsyncMode > 2)) return FALSE;
        if (FindJsonValue(control, "reflexFrameLimitFps", offset)
            && (!ParseJsonInteger(control, "reflexFrameLimitFps", output.reflexFrameLimitFps)
                || output.reflexFrameLimitFps > 1000)) return FALSE;
        // Legacy Dynamic 5X/6X preferences no longer control capacity.
        output.dynamicExperimental56 = FALSE;
        if (FindJsonValue(control, "generatedOnlyDebug", offset)
            && !ParseJsonBool(control, "generatedOnlyDebug", output.generatedOnlyDebug)) return FALSE;
        BOOL legacyValue = FALSE;
        for (const char* name : {"dynamicExperimental56", "selectiveOtaDlssgWrapper"})
            if (FindJsonValue(control, name, offset)
                && !ParseJsonBool(control, name, legacyValue)) return FALSE;
        if (FindJsonValue(control, "intervalLogging", offset)
            && !ParseJsonBool(control, "intervalLogging", output.intervalLoggingEnabled)) return FALSE;
        std::string controlMode;
        if (FindJsonValue(control, "mode", offset))
        {
            if (!ParseJsonString(control, "mode", controlMode)
                || (controlMode != "fixed" && controlMode != "dynamic"
                    && !(controlMode == "follow" && output.followGameMode))) return FALSE;
            output.dynamicMode = controlMode == "dynamic" ? TRUE : FALSE;
        }
        if (output.followGameMode) output.dynamicMode = FALSE;
    }

    *snapshot = output;
    return TRUE;
}

BOOL ReadNativeControlSnapshot(MfgUnlockReShadeSnapshot* snapshot)
{
    std::string control;
    return ReadTextFile(gNativeConfigPath, control)
        && ParseNativeControlSnapshot(control, snapshot);
}

uint64_t NativeStatusUnixSeconds() noexcept
{
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    const uint64_t ticks = (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    return ticks/10000000ull - 11644473600ull;
}

bool ValidNativeStatus(const std::string& status)
{
    if (!ui_status_json::CompleteObject(status)) return false;
    uint32_t version = 0, pid = 0, maximum = 0;
    uint64_t heartbeat = 0, processBirth = 0;
    const uint64_t now = NativeStatusUnixSeconds();
    if (!ParseJsonInteger(status, "version", version)
        || version != MFG_STATUS_VERSION_NUMBER
        || !ParseJsonInteger(status, "pid", pid) || pid != GetCurrentProcessId()
        || !ParseJsonInteger(status, "processBirth", processBirth)
        || !processBirth || processBirth != diagnostic_paths::ProcessBirth()
        || !ParseJsonInteger(status, "heartbeat", heartbeat)
        || heartbeat > now || now-heartbeat > 5
        || !ParseJsonInteger(status, "safeMaximumMultiplier", maximum)
        || maximum < 2 || maximum > 6u) return false;
    // These fields define availability and the menu's shape. Never silently
    // substitute defaults for a missing or ill-typed capability field.
    BOOL value = FALSE;
    for (const char* key : {"bridgeReady", "gameFrameGenerationOn",
            "appliedFrameGenerationOn",
            "activeWrapperObserved", "dynamicMfgSupportKnown", "dynamicMfgSupported"})
        if (!ParseJsonBool(status, key, value)) return false;
    return true;
}

// XeSS-FG state lives beside the shared ReShade snapshot ABI: only the
// integrated menu draws it, so the bridge contract is unchanged.
using XessUiState = xess_ui::State;
XessUiState gXessUi;
// The frame generation the game last turned on; the menu keeps its settings
// while the game's frame generation is off.
std::string gLastGameFrameGeneration = "none";

void ParseXessUiState(const std::string& status) noexcept
{
    try
    {
        gXessUi = xess_ui::Read(status, &ParseJsonBool,
            [](const std::string& text, const char* key, auto& value) {
                return ParseJsonInteger(text, key, value);
            }, &ParseJsonString);
        if (gXessUi.activeFrameGeneration == "xess" || gXessUi.activeFrameGeneration == "dlss")
            gLastGameFrameGeneration = gXessUi.activeFrameGeneration;
    }
    catch (...) {}
}

BOOL WINAPI NativeFileGetSnapshot(MfgUnlockReShadeSnapshot* snapshot)
{
    if (!snapshot || snapshot->structSize < sizeof(*snapshot)) return FALSE;
    MfgUnlockReShadeSnapshot output{};
    const bool haveSavedControl = ReadNativeControlSnapshot(&output) != FALSE;
    std::string status;
    const auto statusSource=status_transport::Read(gNativeStatusPath,status,&ReadTextFile,&ValidNativeStatus);
    if(statusSource==status_transport::Source::None)return FALSE;
    static auto previousSource=status_transport::Source::None;
    if(statusSource!=previousSource){
        single_module::Log(statusSource==status_transport::Source::Fallback
            ?L"MFG_STATUS_UI fresh snapshot source=process-memory"
            :L"MFG_STATUS_UI fresh snapshot source=config-directory");
        previousSource=statusSource;
    }
    {
        // A status heartbeat can precede the latest atomic config save. Saved
        // intent remains authoritative; applied state comes only from status.
        if (!haveSavedControl) ParseJsonBool(status, "followGame", output.followGameMode);
        ParseJsonInteger(status, "version",
            output.statusProtocolVersion);
        if (!ParseJsonInteger(status, "gpuFamily", output.gpuFamily) || output.gpuFamily > 3u)
            return FALSE;
        ParseJsonInteger(status, "gpuAdapterLuid", output.gpuAdapterLuid);
        ParseJsonInteger(status, "gpuSelectionFailure", output.gpuSelectionFailure);
        gGpuFamily.store(output.gpuFamily, std::memory_order_release);
        ParseXessUiState(status);
        if (UiAmpere())
        {
            if (output.statusProtocolVersion != MFG_STATUS_VERSION_NUMBER) return FALSE;
            ParseJsonBool(status, "ampereProgramReadyMfg", output.ampereProgramReadyMfg);
            ParseJsonInteger(status, "ampereKernelImage", output.ampereKernelImage);
            ParseJsonInteger(status, "ampereNativeCacheStatus", output.ampereNativeCacheStatus);
            ParseJsonInteger(status, "ampereCertifiedMaximum", output.ampereCertifiedMaximum);
            ParseJsonInteger(status, "ampereFailure", output.ampereFailure);
            ParseJsonInteger(status, "presetQueries", output.presetQueries);
            ParseJsonInteger(status, "ampereFirstFailure", output.ampereFirstFailure);
            ParseJsonInteger(status, "ampereFirstNgxResult", output.ampereFirstNgxResult);
            ParseJsonInteger(status, "amperePrimaryFailure", output.amperePrimaryFailure);
            ParseJsonInteger(status, "amperePrimaryNgxResult", output.amperePrimaryNgxResult);
            ParseJsonInteger(status, "ampereLastFailure", output.ampereLastFailure);
            ParseJsonInteger(status, "ampereLastNgxResult", output.ampereLastNgxResult);
            ParseJsonInteger(status, "ampereCreateAttempts", output.ampereCreateAttempts);
            ParseJsonInteger(status, "ampereCreateBlockedBeforeProvider", output.ampereCreateBlockedBeforeProvider);
            ParseJsonInteger(status, "ampereEvaluateAttempts", output.ampereEvaluateAttempts);
            ParseJsonInteger(status, "amperePreparationStage", output.amperePreparationStage);
            ParseJsonInteger(status, "ampereStartupFailureMask", output.ampereStartupFailureMask);
            ParseJsonInteger(status, "ampereCandidateVersionMajor", output.ampereCandidateVersionMajor);
            ParseJsonInteger(status, "ampereCandidateVersionMinor", output.ampereCandidateVersionMinor);
            ParseJsonInteger(status, "ampereCandidateVersionBuild", output.ampereCandidateVersionBuild);
            ParseJsonInteger(status, "ampereCreatedFeatures", output.ampereCreatedFeatures);
            ParseJsonInteger(status, "ampereEvaluations", output.ampereEvaluations);
            if (!ParseJsonBool(status, "ampereLegacySinglePreset", output.ampereLegacySinglePreset))
                return FALSE;
        }
        ParseJsonBool(status, "bridgeReady", output.bridgeReady);
        ParseJsonBool(status, "loaderCoreImported",
            output.loaderCoreImported);
        ParseJsonBool(status, "mainResolverDiscoveryInstalled",
            output.mainResolverDiscoveryInstalled);
        ParseJsonBool(status, "slInitEntryDetourCurrent",
            output.slInitEntryDetourCurrent);
        ParseJsonBool(status, "slInitIatFallbackInstalled",
            output.slInitIatFallbackInstalled);
        ParseJsonBool(status, "slInitResolverFallbackActive",
            output.slInitResolverFallbackActive);
        ParseJsonBool(status, "slInitControlPathReady",
            output.slInitControlPathReady);
        ParseJsonInteger(status, "slInitCalls", output.slInitCalls);
        ParseJsonInteger(status, "slInitFlagsBefore",
            output.slInitFlagsBefore);
        ParseJsonInteger(status, "slInitFlagsAfter",
            output.slInitFlagsAfter);
        ParseJsonBool(status, "otaPreferencesForced",
            output.otaPreferencesForced);
        ParseJsonBool(status, "otaPreferencesEnabledAtInit",
            output.otaPreferencesEnabledAtInit);
        ParseJsonBool(status, "downloadedStreamlinePluginsEnabledAtInit",
            output.downloadedStreamlinePluginsEnabledAtInit);
        ParseJsonBool(status, "otaProviderPreflightSupported",
            output.otaProviderPreflightSupported);
        ParseJsonBool(status, "otaForceSuppressed",
            output.otaForceSuppressed);
        ParseJsonBool(status, "nvidiaCompatibilityResolved",
            output.nvidiaCompatibilityResolved);
        ParseJsonInteger(status, "nvidiaProfileStatus",
            output.nvidiaProfileStatus);
        std::string nvidiaProfileName;
        if (ParseJsonString(status, "nvidiaProfileName",
                nvidiaProfileName))
        {
            strncpy_s(output.nvidiaProfileName,
                nvidiaProfileName.c_str(), _TRUNCATE);
        }
        ParseJsonInteger(status, "nvidiaCompatibilityTier",
            output.nvidiaCompatibilityTier);
        ParseJsonInteger(status, "nvidiaCompatibilityManifestEntries",
            output.nvidiaCompatibilityManifestEntries);
        ParseJsonInteger(status, "nvidiaPolicyCeilingMultiplier",
            output.nvidiaPolicyCeilingMultiplier);
        ParseJsonInteger(status, "wrapperNativeMaximumMultiplier",
            output.wrapperNativeMaximumMultiplier);
        ParseJsonBool(status, "compatibilityFallback",
            output.compatibilityFallback);
        ParseJsonInteger(status, "compatibilityReason",
            output.compatibilityReason);
        ParseJsonBool(status, "fullStreamlineOtaRequested",
            output.fullStreamlineOtaRequested);
        ParseJsonBool(status, "fullStreamlineOtaEligible",
            output.fullStreamlineOtaEligible);
        ParseJsonBool(status, "downloadedStreamlinePluginsForced",
            output.downloadedStreamlinePluginsForced);
        ParseJsonInteger(status, "streamlineHostVersionMajor",
            output.streamlineHostVersionMajor);
        ParseJsonInteger(status, "streamlineHostVersionMinor",
            output.streamlineHostVersionMinor);
        ParseJsonInteger(status, "streamlineHostVersionBuild",
            output.streamlineHostVersionBuild);
        ParseJsonInteger(status, "streamlineHostVersionPrivate",
            output.streamlineHostVersionPrivate);
        ParseJsonBool(status, "streamlineLoaderDiscoveryInstalled",
            output.streamlineLoaderDiscoveryInstalled);
        ParseJsonInteger(status, "streamlineLoaderDiscoveryCalls",
            output.streamlineLoaderDiscoveryCalls);
        ParseJsonBool(status, "setOptionsEntryDetourCurrent",
            output.setOptionsEntryDetourCurrent);
        ParseJsonBool(status, "setOptionsResolverFallbackActive",
            output.setOptionsResolverFallbackActive);
        ParseJsonInteger(status, "setOptionsResolverFallbackCalls",
            output.setOptionsResolverFallbackCalls);
        ParseJsonBool(status, "setOptionsControlPathReady",
            output.setOptionsControlPathReady);
        ParseJsonBool(status, "getStateEntryDetourCurrent",
            output.getStateEntryDetourCurrent);
        ParseJsonBool(status, "ngxCreateEntryDetourCurrent",
            output.ngxCreateEntryDetourCurrent);
        ParseJsonBool(status, "ngxEvaluateEntryDetourCurrent",
            output.ngxEvaluateEntryDetourCurrent);
        ParseJsonBool(status, "backportReadyAtCreate",
            output.backportReadyAtCreate);
        ParseJsonBool(status, "pipelineMayPredateDetour",
            output.pipelineMayPredateDetour);
        ParseJsonBool(status, "gameFrameGenerationOn",
            output.gameFrameGenerationOn);
        ParseJsonBool(status, "appliedFrameGenerationOn",
            output.appliedFrameGenerationOn);
        ParseJsonBool(status, "streamlineRebuildRequired",
            output.streamlineRebuildRequired);
        ParseJsonBool(status, "perSampleSynthesisReady",
            output.perSampleSynthesisReady);
        ParseJsonBool(status, "highCapabilityPublicationAllowed",
            output.highCapabilityPublicationAllowed);
        ParseJsonBool(status, "dllNotificationRegistered",
            output.dllNotificationRegistered);
        ParseJsonBool(status, "streamlinePluginLoaderHooksInstalled",
            output.pluginLoaderHooksInstalled);
        ParseJsonBool(status, "activeWrapperObserved",
            output.activeWrapperObserved);
        ParseJsonBool(status, "activeWrapperUsesNvidiaOta",
            output.activeWrapperUsesNvidiaOta);
        ParseJsonInteger(status, "activeWrapperVersionMajor",
            output.activeWrapperVersionMajor);
        ParseJsonInteger(status, "activeWrapperVersionMinor",
            output.activeWrapperVersionMinor);
        ParseJsonInteger(status, "activeWrapperVersionBuild",
            output.activeWrapperVersionBuild);
        ParseJsonInteger(status, "activeWrapperVersionPrivate",
            output.activeWrapperVersionPrivate);
        ParseJsonInteger(status,
            "wrapperCompiledMaximumGeneratedFrames",
            output.wrapperCompiledMaximumGeneratedFrames);
        ParseJsonInteger(status, "safeMaximumMultiplier",
            output.safeMaximumMultiplier);
        if (!haveSavedControl)
            ParseJsonInteger(status, "dlssgPresetRequested", output.dlssgPresetRequested);
        ParseJsonInteger(status, "dlssgPresetLatched", output.dlssgPresetLatched);
        ParseJsonBool(status, "dlssgPresetSelectionFrozen", output.dlssgPresetSelectionFrozen);
        ParseJsonBool(status, "dlssgPresetRestartRequired", output.dlssgPresetRestartRequired);
        ParseJsonInteger(status, "dlssgPresetObserved", output.dlssgPresetObserved);
        ParseJsonBool(status, "dlssgPresetObservedValid", output.dlssgPresetObservedValid);
        ParseJsonInteger(status, "dlssgPresetOverrideReadCount", output.dlssgPresetOverrideReadCount);
        ParseJsonBool(status, "dlssgPresetOverrideInstalled", output.dlssgPresetOverrideInstalled);
        ParseJsonInteger(status, "dlssgPresetOverrideFailure", output.dlssgPresetOverrideFailure);
        ParseJsonInteger(status, "dlssgPresetReadCount", output.dlssgPresetReadCount);
        ParseJsonBool(status, "dlssgPresetCAvailable", output.dlssgPresetCAvailable);
        ParseJsonBool(status, "requestedMultiplierLimited",
            output.requestedMultiplierLimited);
        if (!haveSavedControl) ParseJsonInteger(status, "multiplier", output.desiredMultiplier);
        ParseJsonInteger(status, "appliedMultiplier",
            output.appliedMultiplier);
        if (!haveSavedControl)
            ParseJsonInteger(status, "dynamicTargetFrameRate", output.dynamicTargetFrameRate);
        BOOL appliedTargetValid = FALSE;
        float appliedTarget = 0.0f;
        if (ParseJsonBool(status, "appliedDynamicTargetValid", appliedTargetValid)
            && appliedTargetValid
            && ParseJsonFloat(status, "appliedDynamicTargetFrameRate", appliedTarget)
            && appliedTarget >= 0.0f)
        {
            output.appliedDynamicTargetFrameRate = appliedTarget;
            output.appliedDynamicTargetValid = TRUE;
        }
        ParseJsonInteger(status, "requestRevision",
            output.desiredRevision);
        ParseJsonInteger(status, "appliedRevision",
            output.appliedRevision);
        ParseJsonInteger(status, "actualFramesPresented",
            output.actualFramesPresented);
        ParseJsonInteger(status, "numFramesToGenerateMax",
            output.numFramesToGenerateMax);
        ParseJsonInteger(status, "setOptionsResult",
            output.lastSetOptionsResult);
        ParseJsonInteger(status, "getStateResult",
            output.lastGetStateResult);
        ParseJsonBool(status, "setOptionsSeen",
            output.setOptionsSeen);
        ParseJsonBool(status, "setOptionsAccepted",
            output.setOptionsAccepted);
        ParseJsonBool(status, "getStateSeen",
            output.getStateSeen);
        ParseJsonBool(status, "dynamicMfgSupportKnown",
            output.dynamicMfgSupportKnown);
        ParseJsonBool(status, "dynamicMfgSupported",
            output.dynamicMfgSupported);
        ParseJsonBool(status, "fgVsyncSupportKnown", output.fgVsyncSupportKnown);
        ParseJsonBool(status, "fgVsyncSupported", output.fgVsyncSupported);
        if (!haveSavedControl)
        {
            ParseJsonInteger(status, "vsyncMode", output.vsyncMode);
            ParseJsonInteger(status, "reflexFrameLimitFps", output.reflexFrameLimitFps);
        }
        ParseJsonBool(status, "dynamicVsyncAvailable", output.dynamicVsyncAvailable);
        ParseJsonBool(status, "vsyncControlAvailable", output.vsyncControlAvailable);
        ParseJsonBool(status, "vsyncOverrideApplied", output.vsyncOverrideApplied);
        ParseJsonBool(status, "vsyncPresentationObserved", output.vsyncPresentationObserved);
        ParseJsonInteger(status, "vsyncOriginalInterval", output.vsyncOriginalInterval);
        ParseJsonInteger(status, "vsyncSubmittedInterval", output.vsyncSubmittedInterval);
        ParseJsonInteger(status, "vsyncFailure", output.vsyncFailure);
        ParseJsonBool(status, "reflexControlAvailable", output.reflexControlAvailable);
        ParseJsonBool(status, "reflexAppliedKnown", output.reflexAppliedKnown);
        ParseJsonInteger(status, "reflexAppliedFrameLimitUs", output.reflexAppliedFrameLimitUs);
        ParseJsonBool(status, "reflexLimitPending", output.reflexLimitPending);
        ParseJsonBool(status, "reflexRestorePending", output.reflexRestorePending);
        ParseJsonInteger(status, "reflexStatus", output.reflexStatus);
        ParseJsonInteger(status, "reflexLastResult", output.reflexLastResult);
        ParseJsonInteger(status, "reflexHookMask", output.reflexHookMask);
        ParseJsonInteger(status, "reflexModuleVersionMajor", output.reflexModuleVersionMajor);
        ParseJsonInteger(status, "reflexModuleVersionMinor", output.reflexModuleVersionMinor);
        ParseJsonInteger(status, "reflexModuleVersionPatch", output.reflexModuleVersionPatch);
        ParseJsonInteger(status, "reflexModuleGeneration", output.reflexModuleGeneration);
        ParseJsonBool(status, "intervalLoggingEnabled",
            output.intervalLoggingEnabled);
        ParseJsonBool(status, "intervalLogReady",
            output.intervalLogReady);
        ParseJsonInteger(status, "intervalValidSamples",
            output.intervalValidSamples);
        ParseJsonInteger(status, "intervalInvalidSamples",
            output.intervalInvalidSamples);
        ParseJsonInteger(status, "intervalDroppedSamples",
            output.intervalDroppedSamples);
        ParseJsonInteger(status, "intervalLastCount",
            output.intervalLastCount);
        ParseJsonInteger(status, "intervalLastIndex",
            output.intervalLastIndex);
        ParseJsonInteger(status, "intervalLastPositionNumerator",
            output.intervalLastPositionNumerator);
        ParseJsonInteger(status, "intervalLastPositionDenominator",
            output.intervalLastPositionDenominator);
        ParseJsonInteger(status, "realFpsMilli", output.realFpsMilli);
        ParseJsonInteger(status, "dlssFpsMilli", output.dlssFpsMilli);
        ParseJsonInteger(status, "fpsSampleAgeMs", output.fpsSampleAgeMs);
        ParseJsonBool(status, "uiInputsReady", output.uiInputsReady);
        ParseJsonBool(status, "uiRecompositionEnabled",
            output.uiRecompositionEnabled);
        ParseJsonBool(status, "uiRecompositionForced",
            output.uiRecompositionForced);
        ParseJsonBool(status, "gameUiRecompositionEnabled", output.gameUiRecompositionEnabled);
        if (!haveSavedControl)
            ParseJsonBool(status, "generatedOnlyDebug", output.generatedOnlyDebug);
        ParseJsonBool(status, "appliedGeneratedOnlyDebug",
            output.appliedGeneratedOnlyDebug);
        std::string intervalFile;
        if (ParseJsonString(status, "intervalLogFile", intervalFile))
            strncpy_s(output.intervalLogFile,
                intervalFile.c_str(), _TRUNCATE);
        std::string mode;
        if (!haveSavedControl && ParseJsonString(status, "mode", mode))
            output.dynamicMode = mode == "dynamic" ? TRUE : FALSE;
        std::string appliedMode;
        if (ParseJsonString(status, "appliedMode", appliedMode))
        {
            output.appliedDynamicMode =
                appliedMode == "dynamic" ? TRUE : FALSE;
        }
        std::string route;
        if (ParseJsonString(status, "route", route))
            strncpy_s(output.patchRoute, route.c_str(), _TRUNCATE);
        std::string activeWrapperPath;
        if (ParseJsonString(status, "activeWrapperPath", activeWrapperPath))
            strncpy_s(output.activeWrapperPath,
                activeWrapperPath.c_str(), _TRUNCATE);
        ParseJsonInteger(status, "activeWrapperGeneration",
            output.activeWrapperGeneration);
        std::string activeControlPath;
        if (ParseJsonString(status, "activeControlPath", activeControlPath))
            strncpy_s(output.activeControlPath,
                activeControlPath.c_str(), _TRUNCATE);
        std::string activeStatePath;
        if (ParseJsonString(status, "activeStatePath", activeStatePath))
            strncpy_s(output.activeStatePath,
                activeStatePath.c_str(), _TRUNCATE);
        std::string activeControlDetour;
        if (ParseJsonString(status, "activeControlDetour",
                activeControlDetour))
            strncpy_s(output.activeControlDetour,
                activeControlDetour.c_str(), _TRUNCATE);
        std::string activeStateDetour;
        if (ParseJsonString(status, "activeStateDetour", activeStateDetour))
            strncpy_s(output.activeStateDetour,
                activeStateDetour.c_str(), _TRUNCATE);
        std::string activeProviderPath;
        if (ParseJsonString(status, "activeProviderPath", activeProviderPath))
            strncpy_s(output.activeProviderPath,
                activeProviderPath.c_str(), _TRUNCATE);
        ParseJsonInteger(status, "activeProviderVersionMajor",
            output.activeProviderVersionMajor);
        ParseJsonInteger(status, "activeProviderVersionMinor",
            output.activeProviderVersionMinor);
        ParseJsonInteger(status, "activeProviderVersionBuild",
            output.activeProviderVersionBuild);
        ParseJsonInteger(status, "activeProviderVersionPrivate",
            output.activeProviderVersionPrivate);
        ParseJsonInteger(status, "activeProviderGeneration",
            output.activeProviderGeneration);
        std::string providerSelectionSource;
        if (ParseJsonString(status, "providerSelectionSource",
                providerSelectionSource))
            strncpy_s(output.providerSelectionSource,
                providerSelectionSource.c_str(), _TRUNCATE);
        std::string providerCreateDetour;
        if (ParseJsonString(status, "providerCreateDetour",
                providerCreateDetour))
            strncpy_s(output.providerCreateDetour,
                providerCreateDetour.c_str(), _TRUNCATE);
        std::string providerEvaluateDetour;
        if (ParseJsonString(status, "providerEvaluateDetour",
                providerEvaluateDetour))
            strncpy_s(output.providerEvaluateDetour,
                providerEvaluateDetour.c_str(), _TRUNCATE);
        ParseJsonBool(status, "midpointReadyAtFirstCreate",
            output.midpointReadyAtFirstCreate);
        ParseJsonInteger(status, "activeLastCallRevision",
            output.activeLastCallRevision);
        ParseJsonInteger(status, "activeLastAcceptedRevision",
            output.activeLastAcceptedRevision);
        ParseJsonInteger(status, "universalRouteFailure",
            output.universalRouteFailure);
        std::string routeFailureReason;
        if (ParseJsonString(status, "universalRouteFailureReason",
                routeFailureReason))
            strncpy_s(output.universalRouteFailureReason,
                routeFailureReason.c_str(), _TRUNCATE);
        ParseJsonBool(status, "releaseEntryCurrent",
            output.releaseEntryCurrent);
        ParseJsonBool(status, "frameGenerationOffAccepted",
            output.frameGenerationOffAccepted);
        ParseJsonBool(status, "releaseObserved", output.releaseObserved);
        output.perSampleSynthesisReady = output.midpointReadyAtFirstCreate;
        output.highCapabilityPublicationAllowed =
            output.bridgeReady && output.midpointReadyAtFirstCreate;
    }
    *snapshot = output;
    return TRUE;
}

std::string NativeControlJson(uint32_t multiplier, BOOL followGame, BOOL dynamicMode,
    uint32_t dynamicTargetFrameRate, BOOL generatedOnlyDebug, BOOL intervalLogging,
    uint32_t preset, uint32_t vsyncMode = 0, uint32_t reflexFrameLimitFps = 0)
{
    char content[384]{};
    const int length = std::snprintf(content, sizeof(content),
        "{\"followGame\":%s,\"mode\":\"%s\",\"multiplier\":%u,"
        "\"dynamicTargetFrameRate\":%u,\"dlssgPreset\":%u,"
        "\"vsyncMode\":%u,\"reflexFrameLimitFps\":%u,"
        "\"generatedOnlyDebug\":%s,\"intervalLogging\":%s,"
        "\"version\":13}\n",
        followGame ? "true" : "false",
        followGame ? "follow"
            : dynamicMode ? "dynamic" : "fixed",
        multiplier,
        dynamicTargetFrameRate,
        preset, vsyncMode, reflexFrameLimitFps,
        generatedOnlyDebug ? "true" : "false", intervalLogging ? "true" : "false");
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(content))
        return {};
    return std::string(content, static_cast<size_t>(length));
}

// Carries a raw JSON value of name from the saved control into content when the
// new content lacks it. Writers that rebuild the whole object (DLSS mode,
// preset) then keep the saved XeSS model and UI owner.
void CarryControlField(std::string& content, const std::string& saved, const char* name)
{
    size_t offset = 0, existing = 0;
    if (FindJsonValue(content, name, offset) || !FindJsonValue(saved, name, existing)) return;
    size_t end = existing;
    while (end < saved.size() && saved[end] != ',' && saved[end] != '}'
        && saved[end] != '\r' && saved[end] != '\n') ++end;
    const size_t close = content.find_last_not_of(" \t\r\n");
    if (end == existing || close == std::string::npos || content[close] != '}') return;
    content.insert(close, std::string(",\"") + name + "\":" + saved.substr(existing, end-existing));
}

BOOL WriteNativeControl(const std::string& input)
{
    std::string content = input;
    std::string saved;
    if (ReadTextFile(gNativeConfigPath, saved))
        for (const char* name : {"xessModel", "optiScalerUi"})
            CarryControlField(content, saved, name);
    if (content.empty() || content.size() > 4096) return FALSE;
    std::error_code error;
    std::filesystem::create_directories(
        std::filesystem::path(gNativeConfigPath).parent_path(), error);
    if (error)
        return FALSE;
    const std::wstring temporary = gNativeConfigPath + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return FALSE;
    DWORD written = 0;
    const BOOL write = WriteFile(file, content.data(), static_cast<DWORD>(content.size()),
        &written, nullptr);
    const BOOL flushed = write && written == content.size()
        && FlushFileBuffers(file);
    CloseHandle(file);
    if (!flushed || !MoveFileExW(temporary.c_str(), gNativeConfigPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(temporary.c_str());
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI NativeFileApplyControl(uint32_t multiplier, BOOL dynamicMode,
    uint32_t dynamicTargetFrameRate, BOOL dynamicExperimental56,
    BOOL generatedOnlyDebug)
{
    const BOOL followGame = multiplier == 0 && dynamicMode == FALSE;
    MfgUnlockReShadeSnapshot current{};
    if (!NativeFileGetSnapshot(&current)) return FALSE;
    if (current.gpuFamily != 1u && current.gpuFamily != 2u) return FALSE;
    if (UiAmpere())
    {
        if (!ampere_policy::ControlValid(followGame ? 2u : multiplier, dynamicMode != FALSE,
                dynamicExperimental56 != FALSE, generatedOnlyDebug != FALSE)) return FALSE;
    }
    const uint32_t safeMaximumMultiplier = current.safeMaximumMultiplier;
    if ((!followGame && multiplier > safeMaximumMultiplier)
        || (dynamicMode && (!current.dynamicMfgSupportKnown || !current.dynamicMfgSupported
            || !nvidia_mfg_policy::DynamicRangeFits(current.numFramesToGenerateMax, safeMaximumMultiplier)
            || (UiAmpere() && safeMaximumMultiplier < 6)))
        || current.dlssgPresetRequested > 3)
        return FALSE;
    multiplier = followGame
        ? 2u : std::clamp(multiplier, UiAmpere() ? 1u : 2u, safeMaximumMultiplier);
    dynamicTargetFrameRate = std::min(dynamicTargetFrameRate, 1000u);
    BOOL intervalLogging = UiAmpere() ? FALSE : TRUE;
    if (UiAmpere())
    {
        std::string existingControl;
        if (ReadTextFile(gNativeConfigPath, existingControl))
            ParseJsonBool(existingControl, "intervalLogging", intervalLogging);
    }
    return WriteNativeControl(NativeControlJson(multiplier, followGame, dynamicMode,
        dynamicTargetFrameRate, generatedOnlyDebug, intervalLogging,
        current.dlssgPresetRequested, current.vsyncMode, current.reflexFrameLimitFps));
}

bool SetUnsignedControlField(std::string& content, const char* name, uint32_t value)
{
    size_t offset = 0;
    if (FindJsonValue(content, name, offset))
    {
        uint32_t previous = 0;
        if (!ParseJsonInteger(content, name, previous)) return false;
        size_t end = offset;
        while (end < content.size() && content[end] >= '0' && content[end] <= '9') ++end;
        content.replace(offset, end-offset, std::to_string(value));
    }
    else
    {
        const size_t end = content.find_last_not_of(" \t\r\n");
        if (end == std::string::npos || content[end] != '}') return false;
        content.insert(end, std::string(",\"") + name + "\":" + std::to_string(value));
    }
    return true;
}

BOOL NativeFilePersistPreset(uint32_t preset)
{
    if (preset > 3) return FALSE;
    std::string content;
    if (ReadTextFile(gNativeConfigPath, content))
    {
        MfgUnlockReShadeSnapshot saved{};
        if (!ParseNativeControlSnapshot(content, &saved)
            || !SetUnsignedControlField(content, "dlssgPreset", preset)
            || !SetUnsignedControlField(content, "version", 13)) return FALSE;
    }
    else
    {
        // Never overwrite an unreadable existing file as though it were absent.
        const DWORD attributes = GetFileAttributesW(gNativeConfigPath.c_str());
        const DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
        if (attributes != INVALID_FILE_ATTRIBUTES
            || (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)) return FALSE;
        MfgUnlockReShadeSnapshot current{};
        if (!NativeFileGetSnapshot(&current)) return FALSE;
        content = NativeControlJson(current.desiredMultiplier, current.followGameMode,
            current.dynamicMode, current.dynamicTargetFrameRate, current.generatedOnlyDebug,
            current.intervalLoggingEnabled, preset, current.vsyncMode, current.reflexFrameLimitFps);
    }
    // Preset changes do not need Dynamic capability or an active FG feature.
    // Preserve the saved request and let the backend's immutable preset state
    // decide whether the choice belongs to this process or the next restart.
    return WriteNativeControl(content);
}

BOOL NativeFilePersistPresentation(uint32_t vsyncMode, uint32_t reflexFrameLimitFps)
{
    if (vsyncMode > 2 || reflexFrameLimitFps > 1000) return FALSE;
    std::string content;
    if (ReadTextFile(gNativeConfigPath, content))
    {
        MfgUnlockReShadeSnapshot saved{};
        if (!ParseNativeControlSnapshot(content, &saved)
            || !SetUnsignedControlField(content, "vsyncMode", vsyncMode)
            || !SetUnsignedControlField(content, "reflexFrameLimitFps", reflexFrameLimitFps)
            || !SetUnsignedControlField(content, "version", 13)) return FALSE;
    }
    else
    {
        const DWORD attributes = GetFileAttributesW(gNativeConfigPath.c_str());
        const DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
        if (attributes != INVALID_FILE_ATTRIBUTES
            || (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)) return FALSE;
        MfgUnlockReShadeSnapshot current{};
        if (!NativeFileGetSnapshot(&current)) return FALSE;
        content = NativeControlJson(current.desiredMultiplier, current.followGameMode,
            current.dynamicMode, current.dynamicTargetFrameRate, current.generatedOnlyDebug,
            current.intervalLoggingEnabled, current.dlssgPresetRequested,
            vsyncMode, reflexFrameLimitFps);
    }
    return WriteNativeControl(content);
}

void WINAPI NativeFileSetFrontendAttached(BOOL)
{
}

BOOL WINAPI NativeFileRegisterD3D12Device(ID3D12Device*)
{
    return TRUE;
}

BOOL WINAPI NativeFileRegisterD3D12Queue(ID3D12CommandQueue*)
{
    return TRUE;
}

BOOL WINAPI NativeFileRegisterD3D12Swapchain(
    IDXGISwapChain*, IUnknown*)
{
    return TRUE;
}

BOOL WINAPI NativeFileUnregisterD3D12Swapchain(IDXGISwapChain*)
{
    return TRUE;
}

bool ConnectBackend() noexcept
{
    if (gBackendConnected.load(std::memory_order_acquire))
        return true;

    AcquireSRWLockExclusive(&gBackendConnectionLock);
    if (gBackendConnected.load(std::memory_order_acquire))
    {
        ReleaseSRWLockExclusive(&gBackendConnectionLock);
        return true;
    }
    HMODULE core = gSelf;
    HMODULE shim = gSelf;
    if (!core || !shim || !GetProcAddress(core, "MfgUnlockCoreLoaded")
        || !ResolveNativeFilePaths())
    {
        ReleaseSRWLockExclusive(&gBackendConnectionLock);
        return false;
    }
    gBackend = core;
    gBackendInterface = {};
    gSampleFrameTelemetry = reinterpret_cast<
        MfgUnlockSampleFrameTelemetryFn>(GetProcAddress(
            core, "MfgUnlockSampleFrameTelemetry"));
    gBackendInterface.getSnapshot = &NativeFileGetSnapshot;
    gBackendInterface.applyControl = &NativeFileApplyControl;
    gBackendInterface.registerD3D12Device =
        &NativeFileRegisterD3D12Device;
    gBackendInterface.registerD3D12Queue =
        &NativeFileRegisterD3D12Queue;
    gBackendInterface.registerD3D12Swapchain =
        &NativeFileRegisterD3D12Swapchain;
    gBackendInterface.unregisterD3D12Swapchain =
        &NativeFileUnregisterD3D12Swapchain;
    gBackendInterface.setFrontendAttached =
        &NativeFileSetFrontendAttached;
    gBackendKind.store(BackendKind::eNativeFile,
        std::memory_order_release);
    gBackendConnected.store(true, std::memory_order_release);
    ReleaseSRWLockExclusive(&gBackendConnectionLock);
    return true;
}

void DisconnectBackend() noexcept
{
    gBackendConnected.store(false, std::memory_order_release);
    gBackendInterface = {};
    gBackend = nullptr;
    gSampleFrameTelemetry = nullptr;
    gTelemetrySwapchain.store(0, std::memory_order_release);
    gTelemetrySwapchainTick.store(0, std::memory_order_release);
    gBackendKind.store(BackendKind::eNone, std::memory_order_release);
}


void PersistSettings(reshade::api::effect_runtime* runtime)
{
    reshade::set_config_value(runtime, kConfigSection,
        "FollowGameMode", gFollowGameMode);
    reshade::set_config_value(runtime, kConfigSection,
        "Multiplier", gMultiplier);
    reshade::set_config_value(runtime, kConfigSection,
        "DynamicMode", gDynamicMode);
    reshade::set_config_value(runtime, kConfigSection,
        "DynamicTargetFrameRate", gDynamicTargetFrameRate);
    reshade::set_config_value(runtime, kConfigSection,
        "DynamicLockToRefreshRate", gDynamicLockToRefreshRate);
    reshade::set_config_value(runtime, kConfigSection,
        "DynamicCustomTargetFrameRate", gDynamicCustomTargetFrameRate);
    reshade::set_config_value(runtime, kConfigSection,
        "GeneratedOnlyDebug", gGeneratedOnlyDebug);
}

void ApplySettings(reshade::api::effect_runtime* runtime, bool persist)
{
    MfgUnlockReShadeSnapshot capacity{};
    if (!BackendGetSnapshot(&capacity))
    {
        if (persist)
        {
            gLastApplyAttempted = true;
            gLastApplyAccepted = false;
            gLastNativeConfigPersisted = false;
        }
        return;
    }
    if (UiAmpere())
    {
        gMultiplier = std::clamp(gMultiplier, 1, 6);
    }
    else
    {
        gMultiplier = std::clamp(gMultiplier, 2, 6);
    }
    gDynamicExperimental56 = false;
    gDynamicCustomTargetFrameRate = std::clamp(
        gDynamicCustomTargetFrameRate, 1, 1000);
    gDynamicTargetFrameRate = gDynamicLockToRefreshRate
        ? 0 : gDynamicCustomTargetFrameRate;
    const BOOL accepted = BackendApplyControl(
        gFollowGameMode ? 0u : static_cast<uint32_t>(gMultiplier),
        !gFollowGameMode && gDynamicMode ? TRUE : FALSE,
        static_cast<uint32_t>(gDynamicTargetFrameRate),
        gDynamicExperimental56 ? TRUE : FALSE,
        gGeneratedOnlyDebug ? TRUE : FALSE);
    if (persist)
    {
        const BOOL companionConfigPersisted = TRUE;
        const BOOL nativeConfigPersisted = accepted;
        gLastNativeConfigPersisted = companionConfigPersisted != FALSE
            && nativeConfigPersisted != FALSE;
        gLastApplyAttempted = true;
        gLastApplyAccepted = accepted != FALSE;
        if (accepted) PersistSettings(runtime);
    }
}

void LoadSettings()
{
    gPresetSaveFailed = false;
    gPresentationSaveFailed = false;
    gDefaultDockInitialized = false;
    reshade::get_config_value(nullptr, kConfigSection,
        "DefaultDockInitialized", gDefaultDockInitialized);

    MfgUnlockReShadeSnapshot snapshot{};
    bool snapshotLoaded = BackendGetSnapshot(&snapshot) != FALSE;
    // Saved intent is independent of a transient status-file read failure.
    if (BackendUsesNativeFile())
    {
        MfgUnlockReShadeSnapshot saved{};
        if (ReadNativeControlSnapshot(&saved)) { snapshot = saved; snapshotLoaded = true; }
    }
    if (snapshotLoaded)
    {
        gFollowGameMode = snapshot.followGameMode != FALSE;
        gMultiplier = static_cast<int>(snapshot.desiredMultiplier);
        gDynamicMode = snapshot.dynamicMode != FALSE;
        gDlssgPreset = static_cast<int>(snapshot.dlssgPresetRequested);
        gVsyncMode = static_cast<int>(snapshot.vsyncMode);
        gReflexFrameLimitFps = static_cast<int>(snapshot.reflexFrameLimitFps);
        gDynamicTargetFrameRate = static_cast<int>(
            snapshot.dynamicTargetFrameRate);
        if (gReflexFrameLimitFps > 0) gReflexCustomLimitFps = gReflexFrameLimitFps;
        gDynamicLockToRefreshRate = gDynamicTargetFrameRate == 0;
        if (!gDynamicLockToRefreshRate)
        {
            gDynamicCustomTargetFrameRate = gDynamicTargetFrameRate;
        }
        else
        {
            reshade::get_config_value(nullptr, kConfigSection,
                "DynamicCustomTargetFrameRate",
                gDynamicCustomTargetFrameRate);
        }
        gDynamicCustomTargetFrameRate = std::clamp(
            gDynamicCustomTargetFrameRate, 1, 1000);
        gDynamicExperimental56 = false;
        gGeneratedOnlyDebug = snapshot.generatedOnlyDebug != FALSE;
    }

    // The early-loaded core owns the universal control file. ReShade may
    // initialize much later, so stale ReShade.ini values must not overwrite
    // the request which was already consumed during Streamline startup.
    // No startup/migration write is allowed merely because status is absent.
}

const char* StreamlineResultLabel(int32_t result) noexcept
{
    switch (result)
    {
    case 0:
        return "ok";
    case 34:
        return "feature failed to load";
    case 38:
        return "invalid state/capacity";
    case 39:
        return "accepted; low VRAM warning";
    default:
        return "see Streamline result";
    }
}

const char* YesNo(BOOL value) noexcept
{
    return value ? "yes" : "no";
}

// Menu layout. Stock ImGui colours; only status values are tinted.
enum class Tone { Normal, Good, Pending, Bad, Dim };

ImVec4 ToneColor(Tone tone)
{
    switch (tone)
    {
    case Tone::Good: return ImVec4(0.50f, 0.79f, 0.50f, 1.0f);
    case Tone::Pending: return ImVec4(0.88f, 0.69f, 0.31f, 1.0f);
    case Tone::Bad: return ImVec4(0.94f, 0.45f, 0.42f, 1.0f);
    case Tone::Dim: return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    default: return ImGui::GetStyleColorVec4(ImGuiCol_Text);
    }
}

void HelpMarker(const char* text)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", text);
}

void Section(const char* title)
{
    ImGui::Spacing();
    ImGui::SeparatorText(title);
}

// Controls stay usable-looking while status is stale, so an open popup keeps
// its appearance across a missed read. Item-level disables inside still dim.
void BeginStaleDisabled(bool stale)
{
    ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0f);
    ImGui::BeginDisabled(stale);
    ImGui::PopStyleVar();
}

// Label | value grid shared by controls and status rows.
bool BeginGrid(const char* id)
{
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_NoSavedSettings)) return false;
    ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8.0f);
    ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

void RowLabel(const char* label, bool control)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    if (control) ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", label);
    ImGui::TableSetColumnIndex(1);
}

// Controls share one width, leaving room for a (?) marker.
void ControlRow(const char* label)
{
    RowLabel(label, true);
    const float font = ImGui::GetFontSize();
    ImGui::SetNextItemWidth(std::max(font * 4.0f,
        std::min(font * 13.0f, ImGui::GetContentRegionAvail().x - font * 1.8f)));
}

void StatusRow(const char* label, Tone tone, const char* format, ...)
{
    RowLabel(label, false);
    va_list args;
    va_start(args, format);
    ImGui::PushStyleColor(ImGuiCol_Text, ToneColor(tone));
    ImGui::TextWrappedV(format, args);
    ImGui::PopStyleColor();
    va_end(args);
}

// A short coloured value and a dim detail, e.g. "Active  tracing converted hair".
void StatusDetail(const char* label, Tone tone, const char* value, const char* detail)
{
    RowLabel(label, false);
    ImGui::TextColored(ToneColor(tone), "%s", value);
    if (detail && detail[0])
    {
        ImGui::SameLine();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", detail);
        ImGui::PopTextWrapPos();
    }
}

// Visible notes are for states the user acts on or waits for; explanations
// go into (?) markers.
void Note(Tone tone, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    ImGui::PushStyleColor(ImGuiCol_Text, ToneColor(tone));
    ImGui::TextWrappedV(format, args);
    ImGui::PopStyleColor();
    va_end(args);
}

std::string Grouped(uint64_t value)
{
    std::string digits = std::to_string(value);
    for (ptrdiff_t at = static_cast<ptrdiff_t>(digits.size()) - 3; at > 0; at -= 3)
        digits.insert(static_cast<size_t>(at), 1, ',');
    return digits;
}

// The overlay draws this beside the menu key, after the panel.
const char* gFooterStatus = "";
Tone gFooterTone = Tone::Dim;

void SetFooter(const char* text, Tone tone)
{
    gFooterStatus = text;
    gFooterTone = tone;
}

const char* DlssgPresetName(uint32_t preset) noexcept
{
    switch (preset)
    {
    case 0: return "Game / driver";
    case 1: return "A";
    case 2: return "B";
    case 3: return "C";
    default: return "Unknown";
    }
}

// Preset readback for the Status grid.
void PresetStatusRow(const MfgUnlockReShadeSnapshot& snapshot)
{
    if (UiAmpere() && snapshot.ampereLegacySinglePreset)
    {
        if (snapshot.dlssgPresetObservedValid && snapshot.dlssgPresetObserved == 1)
            StatusDetail("DLSS-G preset", Tone::Good, "Original", "observed");
        else
            StatusDetail("DLSS-G preset", Tone::Pending, "Original", "waiting for the provider");
        return;
    }
    const uint32_t activePreset = snapshot.dlssgPresetSelectionFrozen
        ? snapshot.dlssgPresetLatched : snapshot.dlssgPresetRequested;
    char value[32]{};
    snprintf(value, sizeof(value), "Preset %s", DlssgPresetName(activePreset));
    if (activePreset == 0)
        StatusDetail("DLSS-G preset", Tone::Normal, "Game / driver", "managed by the game or driver");
    else if (activePreset == 3 && snapshot.dlssgPresetReadCount > 0 && !snapshot.dlssgPresetCAvailable)
        StatusDetail("DLSS-G preset", Tone::Pending, value, "needs DLSS-G 310.9.16; game / driver in use");
    else if (snapshot.dlssgPresetObservedValid && snapshot.dlssgPresetObserved == activePreset
        && snapshot.dlssgPresetOverrideReadCount > 0)
        StatusDetail("DLSS-G preset", Tone::Good, value, "override supplied");
    else if (snapshot.dlssgPresetOverrideFailure != 0)
        StatusDetail("DLSS-G preset", Tone::Bad, value, "override unavailable");
    else
        StatusDetail("DLSS-G preset", Tone::Pending, value, "waiting for the provider");
}

// Preset messages the user acts on.
void PresetNotes(const MfgUnlockReShadeSnapshot& snapshot)
{
    if (UiAmpere() && snapshot.ampereLegacySinglePreset)
    {
        Note(Tone::Dim, "This DLSS-G provider supports its original preset only.%s",
            snapshot.dlssgPresetRequested == 2 ? " Preset B stays saved for providers that support it." : "");
    }
    else if (snapshot.dlssgPresetSelectionFrozen
        && snapshot.dlssgPresetRequested != snapshot.dlssgPresetLatched)
    {
        if (snapshot.dlssgPresetRequested == 0)
            Note(Tone::Pending, "The game / driver preset is saved. Restart the game to use it.");
        else
            Note(Tone::Pending, "Preset %s is saved. Restart the game to use it.",
                DlssgPresetName(snapshot.dlssgPresetRequested));
    }
    const uint32_t activePreset = snapshot.dlssgPresetSelectionFrozen
        ? snapshot.dlssgPresetLatched : snapshot.dlssgPresetRequested;
    if (!(UiAmpere() && snapshot.ampereLegacySinglePreset) && activePreset == 3
        && snapshot.dlssgPresetReadCount > 0 && !snapshot.dlssgPresetCAvailable)
        Note(Tone::Dim, "Preset C needs DLSS-G 310.9.16 or newer (NVIDIA app). This game's"
            " frame generation keeps the game / driver preset.");
    if (gPresetSaveFailed)
        Note(Tone::Bad, "The preset selection could not be saved.");
}

// Preset control row. Updates snapshot.dlssgPresetRequested to the selection
// for the status row.
void PresetControlRow(MfgUnlockReShadeSnapshot& snapshot, bool statusStale)
{
    // Preset C needs DLSS-G 310.9.16 or newer (RTX 40 and, since 931, the RTX 30 port).
    const int maximumPreset = 3;
    if (!statusStale && snapshot.dlssgPresetRequested <= 3)
        gDlssgPreset = static_cast<int>(snapshot.dlssgPresetRequested);
    const int previous = gDlssgPreset;
    bool changed = false;
    static const char* options[] = {"Game / driver default", "Preset A", "Preset B", "Preset C (performance)"};
    const bool legacyPreset = UiAmpere() && snapshot.ampereLegacySinglePreset;
    ControlRow("DLSS-G preset");
    ImGui::BeginDisabled(legacyPreset);
    if (ImGui::BeginCombo("##dlssgpreset", legacyPreset ? "Original preset" : options[std::clamp(gDlssgPreset, 0, 3)]))
    {
        for (int preset = 0; preset <= maximumPreset; ++preset)
        {
            if (ImGui::Selectable(options[preset], gDlssgPreset == preset))
            {
                gDlssgPreset = preset;
                changed = preset != previous;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    HelpMarker("The DLSS frame generation model preset.\n"
        "Preset C is NVIDIA's lighter Frame Generation mode. It needs DLSS-G 310.9.16 or newer,\n"
        "from the NVIDIA app's DLSS override; other providers keep the game / driver preset.\n"
        "A change made after frame generation has started applies after a restart.");
    if (changed)
    {
        const BOOL saved = NativeFilePersistPreset(static_cast<uint32_t>(gDlssgPreset));
        gLastApplyAttempted = true;
        gLastApplyAccepted = saved != FALSE;
        gLastNativeConfigPersisted = saved != FALSE;
        gPresetSaveFailed = !saved;
        if (!saved) gDlssgPreset = previous;
    }
    snapshot.dlssgPresetRequested = static_cast<uint32_t>(gDlssgPreset);
}

const char* ReflexPendingMessage(uint32_t status)
{
    switch (static_cast<reflex_control::Status>(status))
    {
    case reflex_control::Status::eWaitingForModule: return "Waiting for the game's Reflex plugin.";
    case reflex_control::Status::eUnsupportedModule: return "The loaded Reflex plugin version is unsupported.";
    case reflex_control::Status::eHookUnavailable: return "Reflex controls are unavailable for this plugin.";
    case reflex_control::Status::eIneligible: return "The Reflex limit is unavailable for the current FG runtime.";
    case reflex_control::Status::eWaitingForGameOptions: return "Waiting for game Reflex settings. Change the game's Reflex setting once to retry.";
    case reflex_control::Status::eWaitingForThread: return "Waiting for the game's Reflex settings thread.";
    case reflex_control::Status::eWaitingForAvailability: return "Waiting for Reflex availability.";
    case reflex_control::Status::eUnavailable: return "Reflex is unavailable.";
    case reflex_control::Status::eRestorePending: return "Restoring the game's Reflex limit...";
    case reflex_control::Status::eCallRejected: return "The game rejected the Reflex limit.";
    case reflex_control::Status::eUnknownShape: return "The game's Reflex settings cannot be safely replayed.";
    case reflex_control::Status::eStaleRoute: return "The Reflex plugin changed. Restart the game.";
    case reflex_control::Status::eBusy: return "Waiting for the next safe Reflex update.";
    default: return "Waiting for the game to accept the Reflex limit.";
    }
}

// Set by the control rows; read by the status rows in the same frame.
bool gPresentationChangedThisFrame = false;

// V-Sync and Reflex limit rows for the DLSS Latency and display grid.
void PresentationControlRows(const MfgUnlockReShadeSnapshot& snapshot, bool statusStale)
{
    gPresentationChangedThisFrame = false;
    if (!BackendUsesNativeFile()) return;
    if (!statusStale)
    {
        gVsyncMode = static_cast<int>(snapshot.vsyncMode);
        gReflexFrameLimitFps = static_cast<int>(snapshot.reflexFrameLimitFps);
    }
    const int previousLimit = gReflexFrameLimitFps;
    const int previousVsync = gVsyncMode;
    bool changed = false;
    // Off submits the game's frames without V-Sync. A saved On from older
    // builds is not offered and behaves as Game.
    const char* const vsyncModes[] = {"Game", "Off"};
    int vsyncChoice = gVsyncMode == 1 ? 1 : 0;
    ControlRow("V-Sync");
    if (ImGui::Combo("##vsync", &vsyncChoice, vsyncModes, 2))
    {
        gVsyncMode = vsyncChoice;
        changed = true;
    }
    HelpMarker("Off submits the game's frames without V-Sync.\n"
        "NVIDIA Control Panel V-Sync settings take priority.");
    const bool dynamic = snapshot.dynamicMode || snapshot.appliedDynamicMode;
    ControlRow("Reflex FPS limit");
    ImGui::BeginDisabled(dynamic);
    bool limitEnabled = gReflexFrameLimitFps > 0;
    if (ImGui::Checkbox("##reflexlimit", &limitEnabled) && !dynamic)
    {
        gReflexFrameLimitFps = limitEnabled ? std::clamp(gReflexCustomLimitFps, 1, 1000) : 0;
        changed = true;
    }
    ImGui::SameLine();
    if (limitEnabled)
    {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        ImGui::DragInt("##reflexlimitfps", &gReflexCustomLimitFps, 1.0f, 1, 1000, "%d FPS");
        if (ImGui::IsItemDeactivatedAfterEdit() && !dynamic)
        {
            gReflexCustomLimitFps = std::clamp(gReflexCustomLimitFps, 1, 1000);
            gReflexFrameLimitFps = gReflexCustomLimitFps;
            changed = true;
        }
    }
    else
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Off");
    }
    ImGui::EndDisabled();
    HelpMarker("Caps the game's frame rate through Reflex.\n"
        "Paused while Dynamic MFG is on; the saved limit is kept.");
    if (changed)
    {
        const BOOL saved = NativeFilePersistPresentation(static_cast<uint32_t>(gVsyncMode),
            static_cast<uint32_t>(gReflexFrameLimitFps));
        gLastApplyAttempted = true;
        gLastApplyAccepted = saved != FALSE;
        gLastNativeConfigPersisted = saved != FALSE;
        gPresentationSaveFailed = !saved;
        if (!saved)
        {
            gReflexFrameLimitFps = previousLimit;
            gVsyncMode = previousVsync;
        }
    }
    gPresentationChangedThisFrame = changed;
}

void PresentationNotes(const MfgUnlockReShadeSnapshot& snapshot)
{
    if (!BackendUsesNativeFile()) return;
    if ((snapshot.dynamicMode || snapshot.appliedDynamicMode) && gReflexFrameLimitFps > 0)
        Note(Tone::Dim, "The Reflex limit is paused while Dynamic MFG is on. The saved limit is kept.");
    if (gPresentationSaveFailed)
        Note(Tone::Bad, "The presentation settings could not be saved.");
}

// V-Sync and Reflex readback for the DLSS Status grid.
void PresentationStatusRows(const MfgUnlockReShadeSnapshot& snapshot, bool statusStale)
{
    if (!BackendUsesNativeFile()) return;
    const bool changed = gPresentationChangedThisFrame;
    char detail[96]{};
    if (gVsyncMode == 1)
    {
        const bool current = !statusStale && !changed && snapshot.vsyncMode == 1;
        if (current && snapshot.vsyncPresentationObserved && snapshot.vsyncOverrideApplied)
        {
            snprintf(detail, sizeof(detail), "game interval %u submitted as 0", snapshot.vsyncOriginalInterval);
            StatusDetail("V-Sync", Tone::Good, "Off", detail);
        }
        else if (current && snapshot.vsyncPresentationObserved)
            StatusDetail("V-Sync", Tone::Good, "Off", "the game already presents without V-Sync");
        else if (current && snapshot.vsyncFailure
                == static_cast<uint32_t>(vsync_control::Failure::ePresentFailed))
            StatusDetail("V-Sync", Tone::Bad, "Off not applied", "the game's last Present was not accepted");
        else if (current)
            StatusDetail("V-Sync", Tone::Pending, "Off requested", "waiting for the game's swapchain");
        else
            StatusRow("V-Sync", Tone::Pending, "Applying...");
    }
    const bool dynamic = snapshot.dynamicMode || snapshot.appliedDynamicMode;
    if (gReflexFrameLimitFps > 0)
    {
        const uint32_t requestedUs = (1000000u + gReflexFrameLimitFps - 1u) / gReflexFrameLimitFps;
        char value[48]{};
        if (!dynamic && !statusStale && !changed && snapshot.reflexAppliedKnown && !snapshot.reflexLimitPending
            && !snapshot.reflexRestorePending && snapshot.reflexAppliedFrameLimitUs == requestedUs
            && snapshot.reflexStatus == static_cast<uint32_t>(reflex_control::Status::eApplied))
        {
            snprintf(value, sizeof(value), "%.2f FPS", 1000000.0 / snapshot.reflexAppliedFrameLimitUs);
            StatusDetail("Reflex limit", Tone::Good, value, "accepted");
        }
        else
        {
            snprintf(value, sizeof(value), "%d FPS requested", gReflexFrameLimitFps);
            if (snapshot.reflexRestorePending)
                StatusDetail("Reflex limit", Tone::Pending, value, "restoring the game's Reflex limit");
            else if (dynamic)
                StatusDetail("Reflex limit", Tone::Dim, value, "paused for Dynamic MFG");
            else
                StatusDetail("Reflex limit", Tone::Pending, value, statusStale
                    ? "Reflex status is temporarily unavailable." : ReflexPendingMessage(snapshot.reflexStatus));
        }
    }
    else if (snapshot.reflexRestorePending)
        StatusRow("Reflex limit", Tone::Pending, "Restoring the game's Reflex limit...");
}

void DrawPresentationDebug(const MfgUnlockReShadeSnapshot& snapshot)
{
    ImGui::Text("V-Sync: requested=%u available=%u observed=%u applied=%u interval=%u->%u failure=%s",
        snapshot.vsyncMode, snapshot.vsyncControlAvailable, snapshot.vsyncPresentationObserved,
        snapshot.vsyncOverrideApplied, snapshot.vsyncOriginalInterval, snapshot.vsyncSubmittedInterval,
        vsync_control::FailureName(static_cast<vsync_control::Failure>(snapshot.vsyncFailure)));
    ImGui::Text("Reflex: available=%u accepted=%u limit=%u us pending=%u restore=%u status=%u result=%d",
        snapshot.reflexControlAvailable, snapshot.reflexAppliedKnown, snapshot.reflexAppliedFrameLimitUs,
        snapshot.reflexLimitPending, snapshot.reflexRestorePending, snapshot.reflexStatus,
        snapshot.reflexLastResult);
    ImGui::Text("Reflex module: %u.%u.%u generation=%llu hooks=0x%X",
        snapshot.reflexModuleVersionMajor, snapshot.reflexModuleVersionMinor,
        snapshot.reflexModuleVersionPatch, static_cast<unsigned long long>(snapshot.reflexModuleGeneration),
        snapshot.reflexHookMask);
}

// Witcher 3 only. The fallback is always prepared there; the game's own
// HairWorks / Path Traced Hair settings turn hair on and off.
void DrawWitcherHair()
{
    const auto status = witcher_dots::ReadSnapshot();
    if (!status.applicable) return;
    Section("Path traced hair");
    const bool active = status.stage == witcher_dots::Stage::Active;
    if (BeginGrid("##hair"))
    {
        if (status.environmentOverride)
            StatusDetail("State", Tone::Dim, "Off", "RTXMFG_WITCHER_DOTS=0");
        else
        {
            // "active (tracing converted hair)" -> "Active" + "tracing converted hair".
            std::string text = witcher_dots::ActivityText(status);
            std::string detail;
            if (const size_t open = text.find(" ("); open != std::string::npos && text.back() == ')')
            {
                detail = text.substr(open + 2, text.size() - open - 3);
                text.resize(open);
            }
            if (!text.empty()) text[0] = static_cast<char>(toupper(static_cast<unsigned char>(text[0])));
            Tone tone = Tone::Normal;
            switch (status.stage)
            {
            case witcher_dots::Stage::Active:
                tone = !status.gameHairTraced ? Tone::Dim : status.lastHairAgeMs < 2000 ? Tone::Good : Tone::Normal;
                break;
            case witcher_dots::Stage::Disabled: tone = Tone::Dim; break;
            case witcher_dots::Stage::Failed: tone = Tone::Bad; break;
            case witcher_dots::Stage::NativeLss: tone = Tone::Normal; break;
            default: tone = Tone::Pending; break;
            }
            StatusDetail("State", tone, text.c_str(), detail.c_str());
            HelpMarker("Turn it on or off with HairWorks and Path Traced Hair in the game's graphics settings.");
            if (active || status.builds)
            {
                StatusRow("Traced hair", Tone::Normal, "%u", status.lastHairAgeMs < 2000 ? status.hairInstances : 0u);
                const std::string updates = Grouped(status.updates) + " updates";
                StatusDetail("Builds", Tone::Normal, Grouped(status.builds).c_str(), updates.c_str());
            }
        }
        ImGui::EndTable();
    }
    if (!status.environmentOverride && status.reason[0])
        Note(active ? Tone::Dim : Tone::Pending, "%s", status.reason);
}

void DrawWitcherHairDiagnostics()
{
    const auto status = witcher_dots::ReadSnapshot();
    if (!status.applicable || status.environmentOverride) return;
    if (status.stage != witcher_dots::Stage::Active && !status.builds) return;
    if (!ImGui::CollapsingHeader("Hair diagnostics")) return;
    constexpr double mib = 1024.0 * 1024.0, gib = 1024.0 * 1024.0 * 1024.0;
    // DOTS time per second by hook, sampled once a second.
    static uint64_t sampleTick = 0, sampleBuild = 0, sampleCopy = 0, sampleCalls = 0, sampleInstances = 0,
        sampleWait = 0, sampleHeld = 0;
    static double buildMs = 0, copyMs = 0, callsPerSecond = 0, instancesPerSecond = 0, waitMs = 0, heldMs = 0;
    const uint64_t tick = GetTickCount64();
    if (!sampleTick || tick - sampleTick >= 1000)
    {
        if (sampleTick)
        {
            const double seconds = (tick - sampleTick) / 1000.0;
            buildMs = (status.buildMicroseconds - sampleBuild) / 1000.0 / seconds;
            copyMs = (status.copyMicroseconds - sampleCopy) / 1000.0 / seconds;
            callsPerSecond = (status.copyCalls - sampleCalls) / seconds;
            instancesPerSecond = (status.instancesScanned - sampleInstances) / seconds;
            waitMs = (status.buildLockWaitMicroseconds - sampleWait) / 1000.0 / seconds;
            heldMs = (status.buildLockHeldMicroseconds - sampleHeld) / 1000.0 / seconds;
        }
        sampleTick = tick; sampleBuild = status.buildMicroseconds; sampleCopy = status.copyMicroseconds;
        sampleWait = status.buildLockWaitMicroseconds; sampleHeld = status.buildLockHeldMicroseconds;
        sampleCalls = status.copyCalls; sampleInstances = status.instancesScanned;
    }
    char value[64]{}, detail[128]{};
    ImGui::TextDisabled("Memory");
    if (BeginGrid("##hairmemory"))
    {
        StatusRow("Vertex pool", Tone::Normal, "%.1f MiB", status.geometryBytes / mib);
        StatusRow("Hair BLAS", Tone::Normal, "%.0f MiB", status.hairBlasBytes / mib);
        StatusRow("Scratch", Tone::Normal, "%.0f MiB", status.hairScratchBytes / mib);
        if (status.memoryKnown)
        {
            snprintf(value, sizeof(value), "%.2f of %.2f GiB", status.vramUsage / gib, status.vramBudget / gib);
            snprintf(detail, sizeof(detail), "shared %.2f GiB", status.sharedUsage / gib);
            StatusDetail("Game VRAM", Tone::Normal, value, detail);
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("CPU per second");
    if (BeginGrid("##haircpu"))
    {
        snprintf(value, sizeof(value), "%.2f ms", buildMs);
        snprintf(detail, sizeof(detail), "lock wait %.2f, under lock %.2f, other %.2f ms",
            waitMs, heldMs, buildMs > waitMs + heldMs ? buildMs - waitMs - heldMs : 0.0);
        StatusDetail("Hair builds", Tone::Normal, value, detail);
        snprintf(value, sizeof(value), "%.2f ms", copyMs);
        snprintf(detail, sizeof(detail), "%.0f calls, %.0fk instances", callsPerSecond, instancesPerSecond / 1000.0);
        StatusDetail("Instance copies", Tone::Normal, value, detail);
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Counters");
    if (BeginGrid("##haircounters"))
    {
        StatusRow("Prebuilds", Tone::Normal, "%s", Grouped(status.prebuilds).c_str());
        StatusRow("Shader libraries", Tone::Normal, "%s", Grouped(status.shaderLibraries).c_str());
        StatusRow("Instance copies", Tone::Normal, "%s", Grouped(status.instanceCopies).c_str());
        snprintf(detail, sizeof(detail), "%s released", Grouped(status.evictions).c_str());
        snprintf(value, sizeof(value), "%u", status.liveOwners);
        StatusDetail("Live hair owners", Tone::Normal, value, detail);
        StatusRow("Pool", Tone::Normal, "%s allocated, %s released, %s returned",
            Grouped(status.poolAllocations).c_str(), Grouped(status.poolReleases).c_str(),
            Grouped(status.poolReturns).c_str());
        StatusRow("Full rebuilds", Tone::Normal, "%s", Grouped(status.fullRebuilds).c_str());
        StatusRow("Fallback checks", Tone::Normal, "%s", Grouped(status.rejected).c_str());
        ImGui::EndTable();
    }
    bool report = status.crashReportRequested;
    static bool reportSaveFailed = false;
    if (ImGui::Checkbox("GPU crash report, next launch", &report))
        reportSaveFailed = !witcher_dots::SetCrashReportRequested(report);
    HelpMarker("Enables D3D12 DRED breadcrumbs for the game's device. If the GPU crashes, the report is written to\n"
        "%TEMP%\\RTXMFG-witcher3-device-removed-<pid>.txt. Costs a little GPU time while enabled.");
    if (reportSaveFailed) Note(Tone::Bad, "Could not save the crash report setting.");
    if (status.crashReportArmed) Note(Tone::Good, "Crash report armed for this session.");
    else if (status.crashReportRequested) Note(Tone::Pending, "Crash report starts with the next launch.");
}

constexpr uint32_t kIntelVendor = 0x8086;
constexpr uint32_t kNvidiaVendor = 0x10de;

const char* VendorName(uint32_t vendor) noexcept
{
    switch (vendor)
    {
    case kNvidiaVendor: return "NVIDIA";
    case 0x1002: return "AMD";
    case kIntelVendor: return "Intel";
    default: return "Unknown";
    }
}

uint32_t GameGpuVendor() noexcept
{
    return xess_ui::Vendor(gXessUi);
}

// Which frame generation the game is using decides which settings are shown.
const char* ActiveFrameGenerationName() noexcept
{
    const auto& active = gXessUi.activeFrameGeneration;
    return active == "xess" ? "XeSS (Intel)" : active == "dlss" ? "DLSS (NVIDIA)" : "Off";
}

// Shown above the settings until the game turns frame generation on.
void FrameGenerationOffNote(const MfgUnlockReShadeSnapshot& snapshot)
{
    if (gXessUi.activeFrameGeneration != "none") return;
    const bool dlssOffered = snapshot.setOptionsSeen || snapshot.activeWrapperObserved;
    const bool xessOffered = gXessUi.hooksActive != FALSE;
    if (dlssOffered && xessOffered)
        Note(Tone::Pending, "Turn on frame generation in the game's settings (DLSS or XeSS).");
    else if (dlssOffered)
        Note(Tone::Pending, "Turn on frame generation in the game's settings (DLSS).");
    else if (xessOffered)
        Note(Tone::Pending, "Turn on frame generation in the game's settings (XeSS).");
    else
        Note(Tone::Pending, "Turn on DLSS or XeSS frame generation in the game's settings.");
}

// Game FG and GPU rows; familyDetail adds the DLSS GPU family.
void GameRows(const MfgUnlockReShadeSnapshot& snapshot, bool familyDetail)
{
    const bool off = gXessUi.activeFrameGeneration == "none";
    StatusRow("Game FG", off ? Tone::Dim : Tone::Normal, "%s", ActiveFrameGenerationName());
    const uint32_t vendor = GameGpuVendor();
    const std::string gpu = !vendor ? std::string() : gXessUi.adapter.empty()
        ? std::string(VendorName(vendor)) : gXessUi.adapter;
    const char* family = !familyDetail ? "" : snapshot.gpuFamily == 1u ? "Ada (RTX 40)"
        : snapshot.gpuFamily == 2u ? "Ampere (RTX 30)" : "";
    if (!gpu.empty())
        StatusDetail("GPU", Tone::Normal, gpu.c_str(), family);
    else if (familyDetail && family[0])
        StatusRow("GPU", Tone::Normal, "%s", family);
    else if (familyDetail)
        StatusRow("GPU", Tone::Pending, "Waiting for a supported game GPU");
}

// XeSS frame generation: status only. beta.8: XeSS MFG runs under OptiScaler, whose menu owns its
// controls (the frame generation ratio and the MFG model). Without OptiScaler the game's XeSS frame
// generation stays at Intel's 2x and RTXMFG leaves it untouched.
void DrawXessPanel(const MfgUnlockReShadeSnapshot& snapshot, bool statusStale)
{
    const auto& x = gXessUi;
    static const char* models[] = {"MFG Unlocker multi-frame port", "Intel 2x model"};

    FrameGenerationOffNote(snapshot);
    Section("Frame generation");
    if (!x.available)
        Note(Tone::Pending, "XeSS frame generation support is unavailable in this build.");
    else if (!x.optiScaler)
        Note(Tone::Pending, "XeSS multi-frame generation needs OptiScaler, with XeFG as its frame "
            "generation output. Without it the game's XeSS frame generation runs at Intel's standard 2x.");
    else
        Note(Tone::Normal, "XeSS multi-frame generation runs through OptiScaler. Set the frame "
            "generation ratio and the MFG model in OptiScaler's menu.");

    Section("Display");
    int vsync = snapshot.vsyncMode == 1 ? 1 : 0;
    bool vsyncChanged = false;
    BeginStaleDisabled(statusStale);
    if (BeginGrid("##xessdisplay"))
    {
        const char* vsyncModes[] = {"Game", "Off"};
        ControlRow("V-Sync");
        vsyncChanged = ImGui::Combo("##xessvsync", &vsync, vsyncModes, IM_ARRAYSIZE(vsyncModes));
        HelpMarker("Off submits the game's frames without V-Sync.");
        ImGui::EndTable();
    }
    if (vsyncChanged && !NativeFilePersistPresentation(static_cast<uint32_t>(vsync), snapshot.reflexFrameLimitFps))
        Note(Tone::Bad, "The V-Sync setting could not be saved.");
    ImGui::EndDisabled();

    DrawWitcherHair();

    Section("Status");
    char value[64]{}, detail[96]{};
    if (BeginGrid("##xessstatus"))
    {
        GameRows(snapshot, false);
        if (x.available && x.optiScaler)
        {
            if (xess_ui::AppliedKnown(x))
            {
                snprintf(value, sizeof(value), "%ux", x.applied);
                snprintf(detail, sizeof(detail), "%u generated frames capacity", x.capacity);
                StatusDetail("Applied", Tone::Good, value, detail);
            }
            else if (x.initialized)
                StatusRow("Applied", Tone::Pending, "Not verified");
            if (x.modelActive == 1 || x.modelActive == 2)
                StatusRow("Model", Tone::Normal, "%s", models[x.modelActive - 1]);
        }
        if (vsync == 1)
        {
            const auto result = !statusStale && !vsyncChanged
                ? xess_ui::VsyncResult(snapshot.vsyncControlAvailable != FALSE,
                    snapshot.vsyncPresentationObserved != FALSE, snapshot.vsyncOverrideApplied != FALSE,
                    snapshot.vsyncOriginalInterval, snapshot.vsyncSubmittedInterval)
                : xess_ui::VsyncReadback::Pending;
            switch (result)
            {
            case xess_ui::VsyncReadback::Overridden:
                snprintf(detail, sizeof(detail), "game interval %u submitted as 0", snapshot.vsyncOriginalInterval);
                StatusDetail("V-Sync", Tone::Good, "Off", detail);
                break;
            case xess_ui::VsyncReadback::AlreadyOff:
                StatusDetail("V-Sync", Tone::Good, "Off", "the game already presents without V-Sync");
                break;
            default:
                StatusDetail("V-Sync", Tone::Pending, "Off requested", "presentation override is not verified");
                break;
            }
        }
        ImGui::EndTable();
    }
    if (x.available && x.optiScaler && !x.status.empty()) Note(Tone::Dim, "%s", x.status.c_str());
    const uint32_t vendor = GameGpuVendor();
    const bool dlssOffered = snapshot.setOptionsSeen || snapshot.activeWrapperObserved;
    if (vendor && vendor != kNvidiaVendor)
        Note(Tone::Dim, "DLSS multi-frame generation needs an NVIDIA RTX GPU.");
    else if (dlssOffered && gXessUi.gpuVendorId == kNvidiaVendor && !xess_ui::DlssMfgFamily(snapshot.gpuFamily))
        Note(Tone::Dim, "DLSS multi-frame generation is unavailable on this GPU.");

    Section("Advanced");
    if (x.available && ImGui::CollapsingHeader("XeSS diagnostics"))
    {
        ImGui::Text("OptiScaler %s | hooks %s | initialized %s | enabled %s",
            x.optiScaler ? "detected" : "not detected", x.hooksActive ? "active" : "waiting",
            x.initialized ? "yes" : "no", x.enabled ? "yes" : "no");
        ImGui::Text("Provider frames %u | model %u", x.framesPresented, x.modelActive);
        if (!x.runtimeFailure.empty()) ImGui::TextWrapped("Runtime: %s", x.runtimeFailure.c_str());
    }
    DrawWitcherHairDiagnostics();
}

void DrawSettings(reshade::api::effect_runtime* runtime)
{
    gOverlayVisitedThisFrame = true;
    gOverlayDockedThisFrame = ImGui::IsWindowDocked();
    if (gOverlayDockedThisFrame && !gDefaultDockInitialized)
    {
        gDefaultDockInitialized = true;
        reshade::set_config_value(runtime, kConfigSection,
            "DefaultDockInitialized", true);
    }

    MfgUnlockReShadeSnapshot snapshot{};
    bool haveSnapshot = BackendGetSnapshot(&snapshot) != FALSE;
    bool statusStale = false;
    bool nativeFile = false;
    nativeFile = BackendUsesNativeFile();
    if (nativeFile)
    {
        if (gLastNativeDisplayPath != gNativeStatusPath)
        {
            gLastNativeDisplayPath = gNativeStatusPath;
            gHaveNativeDisplay = false;
        }
        if (haveSnapshot)
        {
            gLastNativeDisplay = snapshot;
            gHaveNativeDisplay = true;
        }
        else
        {
            statusStale = true;
            if (gHaveNativeDisplay) { snapshot = gLastNativeDisplay; haveSnapshot = true; }
        }
    }
    if (!haveSnapshot)
    {
        if (nativeFile)
        {
            SetFooter("Waiting for status", Tone::Pending);
            Note(Tone::Pending, "Waiting for current backend status. Saved settings are preserved.");
            return;
        }
        SetFooter("Not connected", Tone::Bad);
        Note(Tone::Bad, MFG_PRODUCT " backend is not loaded.");
        ImGui::TextWrapped("The integrated core is unavailable. Check the "
            "startup log and restart the game with one " MFG_PRODUCT " module.");
        return;
    }
    if (nativeFile)
        SetFooter(statusStale ? "Paused" : "Live", statusStale ? Tone::Pending : Tone::Good);
    else
        SetFooter("", Tone::Dim);

    {
        const bool dlssOffered = snapshot.setOptionsSeen || snapshot.activeWrapperObserved;
        if (xess_ui::SettingsSection(gXessUi, gLastGameFrameGeneration, dlssOffered, snapshot.gpuFamily)
            == xess_ui::Section::Xess)
        {
            DrawXessPanel(snapshot, statusStale);
            return;
        }
        FrameGenerationOffNote(snapshot);
        const uint32_t vendor = GameGpuVendor();
        if (vendor && vendor != kNvidiaVendor)
            Note(Tone::Dim, "DLSS multi-frame generation needs an NVIDIA RTX GPU.");
    }
    if (snapshot.gpuFamily == 3u)
    {
        Note(Tone::Pending, "The game changed graphics adapters. Restart the game to select a new GPU.");
        return;
    }

    MfgUnlockReShadeSnapshot presetView = snapshot;
    if (std::strcmp(snapshot.patchRoute, "remix") == 0)
    {
        Section("Frame generation");
        Note(Tone::Normal, "RTX Remix detected. Frame generation and its multiplier are managed "
            "in the RTX Remix menu.");
        BeginStaleDisabled(statusStale);
        if (BeginGrid("##remixfg"))
        {
            PresetControlRow(presetView, statusStale);
            ImGui::EndTable();
        }
        ImGui::EndDisabled();
        PresetNotes(presetView);
        Section("Status");
        if (BeginGrid("##remixstatus"))
        {
            StatusRow("Provider patch", snapshot.backportReadyAtCreate ? Tone::Good : Tone::Pending, "%s",
                snapshot.backportReadyAtCreate ? "Ready at feature creation" : "Waiting for frame generation");
            PresetStatusRow(presetView);
            ImGui::EndTable();
        }
        return;
    }
    const int safeMaximumMultiplier = std::clamp(
        static_cast<int>(snapshot.safeMaximumMultiplier), 2, 6);
    const auto dynamicAvailability = ui_dynamic_mfg::Classify(snapshot);
    const bool dynamicCapabilityKnown = dynamicAvailability != ui_dynamic_mfg::Availability::Checking;
    const bool dynamicSupported = dynamicAvailability == ui_dynamic_mfg::Availability::Supported;
    const bool dynamicUnsupported = dynamicAvailability == ui_dynamic_mfg::Availability::Unavailable;
    const bool wrapperControlUnavailable = snapshot.activeWrapperObserved
        && snapshot.universalRouteFailure
            == static_cast<uint32_t>(universal_route_policy::Failure::eActiveWrapperUnpatched);
    bool settingsChanged = false;
    static const char* fixedModes[] = {
        "2x", "3x", "4x", "5x", "6x"
    };

    Section("Frame generation");
    // Keep an already-open popup and its appearance stable across a missed
    // read. Disabled input prevents cached capabilities from authorizing writes.
    BeginStaleDisabled(statusStale);
    if (BeginGrid("##dlssfg"))
    {
        // Capability can disappear in menus and during map transitions. Rendering
        // that state must not overwrite the user's persisted Dynamic request.
        if (UiAmpere())
        {
            gDynamicExperimental56 = false;
            gGeneratedOnlyDebug = false;
            static const char* ampereModes[]{"Off", "2x FG", "3x MFG", "4x MFG",
                "5x MFG (experimental)", "6x MFG (experimental)"};
            const char* dynamicLabel = ui_dynamic_mfg::Label(dynamicAvailability);
            const char* selectedMode = gFollowGameMode ? "Follow game"
                : gDynamicMode ? dynamicLabel : ampereModes[std::clamp(gMultiplier, 1, 6) - 1];
            ControlRow("Mode");
            if (ImGui::BeginCombo("##framegeneration", selectedMode))
            {
                if (ImGui::Selectable("Follow game", gFollowGameMode))
                { gFollowGameMode = true; gDynamicMode = false; settingsChanged = true; }
                for (int value = 1; value <= 6; ++value)
                {
                    ImGui::BeginDisabled(value > safeMaximumMultiplier);
                    if (ImGui::Selectable(ampereModes[value - 1], !gFollowGameMode && !gDynamicMode && gMultiplier == value)
                        && value <= safeMaximumMultiplier)
                    { gFollowGameMode = false; gMultiplier = value; gDynamicMode = false; settingsChanged = true; }
                    ImGui::EndDisabled();
                }
                ImGui::BeginDisabled(!dynamicSupported);
                if (ImGui::Selectable(dynamicLabel, gDynamicMode) && dynamicSupported)
                { gFollowGameMode = false; gMultiplier = std::max(gMultiplier, 2); gDynamicMode = true; settingsChanged = true; }
                ImGui::EndDisabled();
                ImGui::EndCombo();
            }
        }
        else
        {
            const char* dynamicLabel = ui_dynamic_mfg::Label(dynamicAvailability);
            const char* currentMode = !snapshot.bridgeReady
                ? "Unavailable"
                : gFollowGameMode
                ? "Follow game"
                : gDynamicMode
                    ? dynamicLabel
                    : fixedModes[
                        std::clamp(gMultiplier, 2, 6) - 2];
            ControlRow("MFG mode");
            ImGui::BeginDisabled(!snapshot.bridgeReady);
            if (ImGui::BeginCombo("##mfgmode", currentMode))
            {
                if (ImGui::Selectable("Follow game", gFollowGameMode))
                {
                    gFollowGameMode = true;
                    gDynamicMode = false;
                    settingsChanged = true;
                }
                const int listedMaximum = snapshot.nvidiaPolicyCeilingMultiplier >= 2
                    && snapshot.nvidiaPolicyCeilingMultiplier <= 6
                    ? static_cast<int>(snapshot.nvidiaPolicyCeilingMultiplier) : safeMaximumMultiplier;
                for (int multiplier = 2; multiplier <= listedMaximum;
                     ++multiplier)
                {
                    const bool selected = !gFollowGameMode && !gDynamicMode
                        && gMultiplier == multiplier;
                    ImGui::BeginDisabled(multiplier > safeMaximumMultiplier);
                    if (ImGui::Selectable(fixedModes[multiplier - 2], selected)
                        && multiplier <= safeMaximumMultiplier)
                    {
                        gFollowGameMode = false;
                        gDynamicMode = false;
                        gMultiplier = multiplier;
                        settingsChanged = true;
                    }
                    ImGui::EndDisabled();
                }
                ImGui::BeginDisabled(!dynamicSupported);
                if (ImGui::Selectable(dynamicLabel, gDynamicMode)
                    && dynamicSupported)
                {
                    gFollowGameMode = false;
                    gDynamicMode = true;
                    settingsChanged = true;
                }
                ImGui::EndDisabled();
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
        }
        HelpMarker("Follow game uses the game's own frame generation setting.\n"
            "Dynamic adjusts frame generation toward a target frame rate; it does not set a hard FPS limit.");
        if (!gFollowGameMode && gDynamicMode)
        {
            ImGui::BeginDisabled(!dynamicSupported);
            ControlRow("Dynamic target");
            if (ImGui::Checkbox("Display refresh rate", &gDynamicLockToRefreshRate))
                settingsChanged = true;
            if (!gDynamicLockToRefreshRate)
            {
                ControlRow("Target FPS");
                ImGui::DragInt("##dynamictarget", &gDynamicCustomTargetFrameRate, 1.0f, 1, 1000, "%d FPS");
                if (ImGui::IsItemDeactivatedAfterEdit())
                    settingsChanged = true;
            }
            ImGui::EndDisabled();
        }
        PresetControlRow(presetView, statusStale);
        ImGui::EndTable();
    }
    if (!UiAmpere())
    {
        if (dynamicUnsupported)
            Note(Tone::Dim, "Dynamic MFG is unavailable with the current limit or runtime. "
                "Its full frame-generation range must fit the limit.");
        else if (!gFollowGameMode && gDynamicMode && !dynamicSupported)
            Note(Tone::Pending, "Dynamic selection is saved; waiting for runtime support.");
        if (!gFollowGameMode && !gDynamicMode
            && (!snapshot.bridgeReady || gMultiplier > safeMaximumMultiplier))
            Note(Tone::Pending, "Saved MFG request: %dx (unavailable in this session)", gMultiplier);
    }
    if (gLastApplyAttempted && !gPresetSaveFailed
        && (!gLastApplyAccepted || !gLastNativeConfigPersisted))
    {
        if (gLastApplyAccepted)
            Note(Tone::Bad, "Request accepted, but the configuration was not saved.");
        else
            Note(Tone::Bad, "The selection could not be applied or saved.");
    }
    PresetNotes(presetView);

    Section("Latency and display");
    if (BeginGrid("##dlsslatency"))
    {
        PresentationControlRows(snapshot, statusStale);
        ImGui::EndTable();
    }
    PresentationNotes(snapshot);
    ImGui::EndDisabled();

    DrawWitcherHair();

    Section("Status");
    char value[64]{}, detail[128]{};
    if (BeginGrid("##dlssstatus"))
    {
        GameRows(snapshot, true);
        const bool fgKnown = snapshot.setOptionsSeen && snapshot.setOptionsAccepted;
        snprintf(detail, sizeof(detail), "game intent: %s", !snapshot.setOptionsSeen ? "unknown"
            : snapshot.gameFrameGenerationOn ? "on" : "off");
        StatusDetail("FG state", !fgKnown ? Tone::Dim : snapshot.appliedFrameGenerationOn ? Tone::Good : Tone::Normal,
            !fgKnown ? "Unknown" : snapshot.appliedFrameGenerationOn ? "On" : "Off", detail);
        if (snapshot.followGameMode)
            StatusRow("Requested", Tone::Normal, "Follow game");
        else if (snapshot.appliedRevision == 0 || snapshot.dynamicMode != snapshot.appliedDynamicMode
            || (!snapshot.dynamicMode && snapshot.desiredMultiplier != snapshot.appliedMultiplier))
            StatusRow("Requested", Tone::Pending, snapshot.dynamicMode ? "Dynamic" : "%ux",
                snapshot.desiredMultiplier);
        if (snapshot.appliedRevision != 0)
        {
            snprintf(value, sizeof(value), snapshot.appliedDynamicMode ? "Dynamic, %ux" : "%ux",
                snapshot.appliedMultiplier);
            StatusRow("Applied", snapshot.appliedFrameGenerationOn ? Tone::Good : Tone::Normal, "%s", value);
        }
        const char* nvidiaList = !snapshot.nvidiaCompatibilityResolved ? "NVIDIA list unavailable"
            : snapshot.nvidiaCompatibilityTier == 2 ? "NVIDIA lists no MFG for this game"
            : snapshot.nvidiaCompatibilityTier != 4 && snapshot.nvidiaCompatibilityTier != 6
                ? "game not on NVIDIA's list" : nullptr;
        if (!nvidiaList)
        {
            snprintf(detail, sizeof(detail), "NVIDIA lists %ux", snapshot.nvidiaCompatibilityTier);
            nvidiaList = detail;
        }
        if (wrapperControlUnavailable)
            StatusDetail("Limit", Tone::Bad, "Unavailable", "the current wrapper has no verified MFG override");
        else if (snapshot.activeWrapperObserved)
        {
            snprintf(value, sizeof(value), "%dx%s", safeMaximumMultiplier,
                snapshot.compatibilityFallback ? " (safe fallback)" : "");
            StatusDetail("Limit", Tone::Normal, value, nvidiaList);
        }
        else
            StatusDetail("Limit", Tone::Pending, "Waiting", "for an active DLSS-G wrapper");
        char limitHelp[256]{};
        if (snapshot.nvidiaCompatibilityTier == 2)
            snprintf(limitHelp, sizeof(limitHelp),
                "NVIDIA lists ordinary Frame Generation only. The mod keeps its override limit at 2x.");
        else if (snapshot.nvidiaPolicyCeilingMultiplier > static_cast<uint32_t>(safeMaximumMultiplier))
            snprintf(limitHelp, sizeof(limitHelp),
                "The listed %ux ceiling requires a compatible runtime. The current limit is %dx.",
                snapshot.nvidiaPolicyCeilingMultiplier, safeMaximumMultiplier);
        else
            snprintf(limitHelp, sizeof(limitHelp),
                "The highest mode the current DLSS-G runtime supports, and the ceiling\n"
                "NVIDIA's override list gives this game.");
        HelpMarker(limitHelp);
        const bool fpsSampleCurrent = snapshot.realFpsMilli > 0
            && snapshot.fpsSampleAgeMs <= 2000;
        if (snapshot.appliedFrameGenerationOn && fpsSampleCurrent)
        {
            if (snapshot.dlssFpsMilli > 0)
                StatusRow("FPS", Tone::Normal, "%.1f real, %.1f DLSS",
                    static_cast<double>(snapshot.realFpsMilli) / 1000.0,
                    static_cast<double>(snapshot.dlssFpsMilli) / 1000.0);
            else
                StatusRow("FPS", Tone::Normal, "%.1f real, DLSS unavailable",
                    static_cast<double>(snapshot.realFpsMilli) / 1000.0);
        }
        else if (snapshot.appliedFrameGenerationOn && gSampleFrameTelemetry)
        {
            StatusRow("FPS", Tone::Dim, "Measuring...");
        }
        if (!gFollowGameMode && gDynamicMode)
        {
            if (gDynamicLockToRefreshRate) snprintf(value, sizeof(value), "Display refresh rate");
            else snprintf(value, sizeof(value), "%d FPS", gDynamicCustomTargetFrameRate);
            if (!snapshot.appliedDynamicTargetValid)
                StatusDetail("Dynamic target", Tone::Pending, value, "waiting for an accepted Dynamic request");
            else
            {
                if (snapshot.appliedDynamicTargetFrameRate == 0.0f)
                    snprintf(detail, sizeof(detail), "applied: display refresh rate");
                else
                    snprintf(detail, sizeof(detail), "applied: %.2f FPS", snapshot.appliedDynamicTargetFrameRate);
                StatusDetail("Dynamic target", Tone::Good, value, detail);
            }
        }
        PresetStatusRow(presetView);
        PresentationStatusRows(snapshot, statusStale);
        ImGui::EndTable();
    }
    if (snapshot.statusProtocolVersion != MFG_STATUS_VERSION_NUMBER)
    {
        Note(Tone::Bad, "Restart with the matching single-module build. "
            "The current status protocol is older than this UI expects.");
    }
    else if (!snapshot.bridgeReady)
    {
        if (wrapperControlUnavailable)
            Note(Tone::Pending, "Frame generation stays under the game's control. "
                "This wrapper has no verified MFG override support.");
        else if (snapshot.intervalValidSamples && !snapshot.setOptionsSeen)
            Note(Tone::Pending, "DLSS-G activity observed; waiting for the game's FG control calls.");
        else Note(Tone::Pending, "%s", snapshot.universalRouteFailure
            == static_cast<uint32_t>(universal_route_policy::Failure::eAwaitingOptionsRequest)
            ? "Waiting for the game to request frame generation."
            : "Waiting for an active DLSS-G pipeline.");
    }
    if (snapshot.streamlineRebuildRequired
        || snapshot.pipelineMayPredateDetour)
    {
        Note(Tone::Pending, "Toggle frame generation off then on, or restart "
            "the game, to recreate the pipeline with this selection.");
    }

    Section("Advanced");
    if (ImGui::CollapsingHeader("Diagnostics"))
    {
        if (!UiAmpere())
        {
            ImGui::BeginDisabled(statusStale);
            if (ImGui::Checkbox("Generated frames only",
                    &gGeneratedOnlyDebug))
            {
                settingsChanged = true;
            }
            ImGui::EndDisabled();
            HelpMarker("Debug: shows only generated frames.");
        }
        if (ImGui::TreeNode("Presentation"))
        {
            ImGui::Text("Runtime FG V-Sync capability: %s", !snapshot.fgVsyncSupportKnown
                ? "unknown" : snapshot.fgVsyncSupported ? "supported" : "unavailable");
            DrawPresentationDebug(snapshot);
            ImGui::TreePop();
        }
        if (UiAmpere() && ImGui::TreeNode("Ampere"))
        {
            ImGui::Text("Ampere SM86 | D3D12 | FG preset: %s",
                snapshot.ampereLegacySinglePreset ? "Original preset"
                    : DlssgPresetName(snapshot.dlssgPresetSelectionFrozen
                        ? snapshot.dlssgPresetLatched : snapshot.dlssgPresetRequested));
            ImGui::Text("Ampere provider: %s | Preset query: %s",
                snapshot.ampereProgramReadyMfg ? "MFG program prepared" : "pending",
                snapshot.presetQueries ? "observed" : "pending");
            ImGui::Text("Kernel program: %s | Cache status: %u", snapshot.ampereKernelImage == 2 ? "native SM86"
                : snapshot.ampereKernelImage == 1 ? "SM86 PTX" : "pending", snapshot.ampereNativeCacheStatus);
            if (snapshot.ampereFailure)
                ImGui::TextWrapped("Ampere failure: %u (%s)", snapshot.ampereFailure,
                    ampere_diagnostics::FailureName(snapshot.ampereFailure));
            ImGui::Text("Create attempts: %llu | blocked before NGX: %llu | created: %llu",
                static_cast<unsigned long long>(snapshot.ampereCreateAttempts),
                static_cast<unsigned long long>(snapshot.ampereCreateBlockedBeforeProvider),
                static_cast<unsigned long long>(snapshot.ampereCreatedFeatures));
            ImGui::Text("Evaluation attempts: %llu | accepted: %llu",
                static_cast<unsigned long long>(snapshot.ampereEvaluateAttempts),
                static_cast<unsigned long long>(snapshot.ampereEvaluations));
            if (snapshot.ampereCandidateVersionMajor)
                ImGui::Text("Observed provider candidate: %u.%u.%u",
                    snapshot.ampereCandidateVersionMajor, snapshot.ampereCandidateVersionMinor,
                    snapshot.ampereCandidateVersionBuild);
            ImGui::Text("Program preparation: %s (%u)",
                ampere_diagnostics::PreparationName(snapshot.amperePreparationStage), snapshot.amperePreparationStage);
            if (snapshot.ampereStartupFailureMask)
                ImGui::TextWrapped("Startup incomplete: %s%s%s%s%s%s%s%s",
                    (snapshot.ampereStartupFailureMask & 0x01u) ? "Streamline startup; " : "",
                    (snapshot.ampereStartupFailureMask & 0x02u) ? "NGX runtime route; " : "",
                    (snapshot.ampereStartupFailureMask & 0x04u) ? "provider route; " : "",
                    (snapshot.ampereStartupFailureMask & 0x08u) ? "startup capacity; " : "",
                    (snapshot.ampereStartupFailureMask & 0x10u) ? "D3D12 device; " : "",
                    (snapshot.ampereStartupFailureMask & 0x20u) ? "Ampere adapter; " : "",
                    (snapshot.ampereStartupFailureMask & 0x40u) ? "kernel program; " : "",
                    (snapshot.ampereStartupFailureMask & 0x80u) ? "fatal boundary; " : "");
            if (snapshot.amperePrimaryFailure)
                ImGui::TextWrapped("Current first failure: %u (%s) | NGX: 0x%08X",
                    snapshot.amperePrimaryFailure, ampere_diagnostics::FailureName(snapshot.amperePrimaryFailure),
                    snapshot.amperePrimaryNgxResult);
            if (snapshot.ampereFirstFailure)
                ImGui::TextWrapped("First failure this run: %u (%s) | NGX: 0x%08X",
                    snapshot.ampereFirstFailure, ampere_diagnostics::FailureName(snapshot.ampereFirstFailure),
                    snapshot.ampereFirstNgxResult);
            if (snapshot.ampereLastFailure && snapshot.ampereLastFailure != snapshot.amperePrimaryFailure)
                ImGui::TextWrapped("Latest failure: %u (%s) | NGX: 0x%08X",
                    snapshot.ampereLastFailure, ampere_diagnostics::FailureName(snapshot.ampereLastFailure),
                    snapshot.ampereLastNgxResult);
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Active route"))
        {
            ImGui::Text("Core protocol: %u | Bridge: %s",
                snapshot.statusProtocolVersion,
                snapshot.bridgeReady ? "ready" : "not ready");
            ImGui::Text("Fail-closed reason: %s (%u)",
                snapshot.universalRouteFailureReason[0]
                    ? snapshot.universalRouteFailureReason : "none",
                snapshot.universalRouteFailure);
            ImGui::TextWrapped("Wrapper: %s",
                snapshot.activeWrapperPath[0]
                    ? snapshot.activeWrapperPath : "waiting for a real call");
            ImGui::Text("Wrapper version: %u.%u.%u.%u | generation: %llu",
                snapshot.activeWrapperVersionMajor,
                snapshot.activeWrapperVersionMinor,
                snapshot.activeWrapperVersionBuild,
                snapshot.activeWrapperVersionPrivate,
                static_cast<unsigned long long>(
                    snapshot.activeWrapperGeneration));
            ImGui::Text("Control: %s (%s) | State: %s (%s)",
                snapshot.activeControlPath[0]
                    ? snapshot.activeControlPath : "none",
                snapshot.activeControlDetour[0]
                    ? snapshot.activeControlDetour : "none",
                snapshot.activeStatePath[0]
                    ? snapshot.activeStatePath : "none",
                snapshot.activeStateDetour[0]
                    ? snapshot.activeStateDetour : "none");
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Active provider"))
        {
            ImGui::TextWrapped("Provider: %s",
                snapshot.activeProviderPath[0]
                    ? snapshot.activeProviderPath : "waiting for FG Create");
            ImGui::Text("Provider version: %u.%u.%u.%u | generation: %llu",
                snapshot.activeProviderVersionMajor,
                snapshot.activeProviderVersionMinor,
                snapshot.activeProviderVersionBuild,
                snapshot.activeProviderVersionPrivate,
                static_cast<unsigned long long>(
                    snapshot.activeProviderGeneration));
            ImGui::Text("Selected by: %s | Create: %s | Evaluate: %s",
                snapshot.providerSelectionSource[0]
                    ? snapshot.providerSelectionSource : "none",
                snapshot.providerCreateDetour[0]
                    ? snapshot.providerCreateDetour : "none",
                snapshot.providerEvaluateDetour[0]
                    ? snapshot.providerEvaluateDetour : "none");
            ImGui::Text("Midpoint ready at first Create: %s",
                YesNo(snapshot.midpointReadyAtFirstCreate));
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Control and lifecycle"))
        {
            ImGui::Text("Requested/applied revision: %llu/%llu",
                static_cast<unsigned long long>(snapshot.desiredRevision),
                static_cast<unsigned long long>(snapshot.appliedRevision));
            ImGui::Text("Route last call/accepted revision: %llu/%llu",
                static_cast<unsigned long long>(
                    snapshot.activeLastCallRevision),
                static_cast<unsigned long long>(
                    snapshot.activeLastAcceptedRevision));
            ImGui::Text("FG applied/intent: %s/%s | Off accepted: %s | Release observed: %s",
                snapshot.appliedFrameGenerationOn ? "on" : "off",
                snapshot.gameFrameGenerationOn ? "on" : "off",
                YesNo(snapshot.frameGenerationOffAccepted),
                YesNo(snapshot.releaseObserved));
            ImGui::Text("Release entry: %s | Recreate required: %s",
                snapshot.releaseEntryCurrent ? "covered" : "unavailable",
                YesNo(snapshot.streamlineRebuildRequired));
            ImGui::Text("NVIDIA max: %s | Available max: %s",
                snapshot.nvidiaCompatibilityTier == 4
                        || snapshot.nvidiaCompatibilityTier == 6
                    ? (snapshot.nvidiaCompatibilityTier == 6 ? "6x" : "4x")
                    : snapshot.nvidiaCompatibilityTier == 2 ? "no MFG override listed" : "unknown",
                snapshot.activeWrapperObserved
                    ? (safeMaximumMultiplier == 6 ? "6x"
                        : safeMaximumMultiplier == 5 ? "5x"
                        : safeMaximumMultiplier == 4 ? "4x"
                        : safeMaximumMultiplier == 3 ? "3x" : "2x")
                    : "detecting");
            if (snapshot.getStateSeen && snapshot.lastGetStateResult == 0)
            {
                ImGui::Text("Runtime: %u presented | max %u generated",
                    snapshot.actualFramesPresented,
                    snapshot.numFramesToGenerateMax);
            }
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Temporal interval trace"))
        {
            ImGui::Text("Log: %s | Samples: %llu valid, %llu invalid, %llu dropped",
                snapshot.intervalLogReady ? "ready" : "opening",
                static_cast<unsigned long long>(snapshot.intervalValidSamples),
                static_cast<unsigned long long>(snapshot.intervalInvalidSamples),
                static_cast<unsigned long long>(snapshot.intervalDroppedSamples));
            if (snapshot.intervalValidSamples > 0)
            {
                ImGui::Text("Last interval: count %d | index %d | position %u/%u",
                    snapshot.intervalLastCount, snapshot.intervalLastIndex,
                    snapshot.intervalLastPositionNumerator,
                    snapshot.intervalLastPositionDenominator);
            }
            if (snapshot.intervalLogFile[0])
                ImGui::Text("Trace: %%TEMP%%\\%s", snapshot.intervalLogFile);
            ImGui::TreePop();
        }
        ImGui::Text("UI recomposition: %s", snapshot.gameUiRecompositionEnabled ? "on (the game's own)" : "off");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("DLSS-G UI recomposition is left to the game; RTXMFG never enables it.\n"
                "An NVIDIA frame generation preset can still turn it on in the driver.");
    }
    DrawWitcherHairDiagnostics();

    if (settingsChanged)
        ApplySettings(runtime, true);
}
}

namespace standalone_ui
{
void Initialize(HMODULE self)
{
    gSelf = self;
    if (!gInitialized.exchange(true, std::memory_order_acq_rel))
        LoadSettings();
}
void Draw()
{
    DrawSettings(nullptr);
}
void DrawFooterStatus()
{
    if (!gFooterStatus[0]) return;
    const float width = ImGui::CalcTextSize(gFooterStatus).x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - width));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ToneColor(gFooterTone), "%s", gFooterStatus);
    if (std::strcmp(gFooterStatus, "Paused") == 0 && ImGui::IsItemHovered())
        ImGui::SetTooltip("Status is temporarily unavailable; controls are paused.");
}
}
