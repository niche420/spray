#pragma once

#include "Types.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace spray::graphics {

class IDevice;

struct AdapterInfo {
    uint32_t index = 0;
    std::string name;
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    size_t dedicatedVideoMemoryBytes = 0;
    bool isIntegrated = false;
};

class IContext {
public:
    virtual ~IContext() = default;

    virtual BackendType GetBackendType() const = 0;
    virtual std::vector<AdapterInfo> EnumerateAdapters() = 0;
    virtual std::unique_ptr<IDevice> CreateDevice(uint32_t adapterIndex) = 0;

	static std::unique_ptr<IContext> Create(BackendType type);
};

struct GpuRequirements {
    size_t minDedicatedVideoMemoryBytes = 0;
    bool requireDiscrete = false;
    std::optional<uint32_t> preferredVendorId;
};

std::optional<uint32_t> SelectAdapter(const std::vector<AdapterInfo>& adapters, const GpuRequirements& reqs);

} // namespace spray::graphics