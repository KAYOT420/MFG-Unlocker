#pragma once
#include <windows.h>
#include <d3d12.h>
#include <cstdint>
#include <string>

struct IDXGISwapChain1;

namespace sdk_pacing
{
// Hooks the exact profiled provider's native Present, Pace and Deadline. Install before
// registering an owner. The published route and the two modules remain pinned for
// process lifetime; no hook is removed during play.
bool Install(HMODULE provider, std::string& failure) noexcept;
// Called under the backend's context lock after a successful SDK initialization;
// presentation callbacks never take that context lock. Plain owner: extra outputs go
// through the SDK's own pacing wait; deadlines follow the DLSS-G batch interval
// (sdk_pacing_dlssg.h). The native swap chain's buffer count is unchanged.
bool Register(void* handle, uint64_t generation, ID3D12Device* device, std::string& failure) noexcept;
// Clears the owner's DLSS-G batch-interval history on an accepted enable/count/resize transition.
void Reset(void* handle, uint64_t generation) noexcept;
// The SDK has joined its presenter threads before a successful Destroy returns.
void Unregister(void* handle, uint64_t generation) noexcept;
}
