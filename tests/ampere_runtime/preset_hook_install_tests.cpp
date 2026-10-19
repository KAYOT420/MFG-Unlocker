// Exercises the real entry registry/MinHook installer on a provider mapped
// without DllMain/import resolution. No NVIDIA code, Init/Create/Evaluate or GPU
// work runs. All patched pages are private to this short-lived test process.
#include "dlssg_preset.cpp"
#include <iostream>

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) { std::cerr << "usage: PresetHookInstallTests provider.dll\n"; return 2; }
    HMODULE module = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!module || !dlssg_provider_policy::IsSupportedRetainedProvider(module))
    { std::cerr << "provider mapping/identity failed\n"; return 2; }
    dlssg_preset::InstallCachedReader(module, 7);
    unsigned checks = 0, failures = 0;
    const auto check = [&](bool ok, const char* message) {
        ++checks; failures += !ok;
        std::cout << (ok ? "PASS " : "FAIL ") << message << '\n';
    };
    const auto snapshot = dlssg_preset::ReadSnapshot(module);
    check(snapshot.cachedReaderInstalled, "cached reader detour is current");
    check(snapshot.configurationResetAvailable && snapshot.configurationSlotRva,
        "configuration discovery and both endpoint detours are current");
    check(dlssg_preset::ProviderSupportsLiveSwitch(module), "the verified installed route admits live switching");
    for (const auto kind : {entry_detour::Kind::eDlssgCachedPresetRead,
        entry_detour::Kind::eDlssgEndpointCreate, entry_detour::Kind::eDlssgEndpointEvaluate})
    {
        const auto hook = entry_detour::ReadSnapshot(kind, module);
        std::cout << "kind=" << static_cast<unsigned>(kind) << " rva=0x" << std::hex << hook.targetRva
            << std::dec << " method=" << entry_detour::MethodName(hook.method)
            << " failure=" << entry_detour::FailureName(hook.failure) << '\n';
        check(hook.current && hook.original && hook.generation == 7 && hook.owner == module,
            "entry ownership, generation, trampoline and readback match");
    }
    dlssg_preset::InstallCachedReader(module, 7);
    check(dlssg_preset::ReadSnapshot(module).configurationResetAvailable, "repeated discovery preserves the installed route");
    check(!snapshot.observedPresetValid && !snapshot.configurationReads && !snapshot.reportReads,
        "installing hooks alone supplies no configuration evidence");
    std::cout << "PRESET_HOOK_INSTALL checks=" << checks << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}
