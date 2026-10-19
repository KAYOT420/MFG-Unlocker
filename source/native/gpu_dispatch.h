#pragma once
#include "midpoint_fix.h"

namespace gpu_dispatch
{
enum class Family : uint32_t { eUnknown = 0, eAda = 1, eAmpere = 2, eConflict = 3, eTuring = 4 };
Family Selected() noexcept;
bool IsAda() noexcept;
bool IsAmpere() noexcept;
bool IsTuring() noexcept;
// RTX 20/30 share the lifetime/capacity backend, with separate GPU programs.
bool UsesTranslatedProgram() noexcept;
uint64_t AdapterLuid() noexcept;
// PCI vendor of the last observed game device (0 until a device is seen).
// Non-NVIDIA devices never reach the CUDA/NVAPI family probes.
uint32_t AdapterVendor() noexcept;
void SetLogCallback(midpoint_fix::LogCallback) noexcept;
bool ObserveD3D12Device(void*) noexcept;
// Selects Ada only (a verified Ada D3D12 adapter); never selects RTX 30 and
// publishes nothing for other adapters. For devices seen before the game's own
// selection boundaries, such as OptiScaler's private Streamline.
bool ObserveAdaD3D12Device(void*) noexcept;
bool ObserveVulkanPhysicalDevice(void*) noexcept;
bool PatchProvider(HMODULE, const wchar_t*) noexcept;
bool AdapterVerified() noexcept;
bool Ready() noexcept;
uint32_t FailureCode() noexcept;
}
