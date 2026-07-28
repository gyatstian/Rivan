// PluginApi.h
// Versioned, in-process extension contract. This task intentionally provides no DLL loader.
#pragma once

#include <cstdint>

namespace rivan::plugin {

inline constexpr std::uint32_t kPluginApiVersion = 1;

enum class PluginKind : std::uint32_t { Visualization = 1, SkinProvider = 2, Feature = 3 };

struct PluginDescriptor {
    std::uint32_t structureSize{sizeof(PluginDescriptor)};
    std::uint32_t apiVersion{kPluginApiVersion};
    PluginKind kind{PluginKind::Feature};
    const wchar_t* id{};
    const wchar_t* displayName{};
    const wchar_t* author{};
};

class IPlugin {
public:
    virtual ~IPlugin() = default;
    [[nodiscard]] virtual const PluginDescriptor& Descriptor() const noexcept = 0;
    virtual bool Start() { return true; }
    virtual void Stop() noexcept {}
};

// Kept for a future loader boundary; no code in this task resolves or invokes it.
using GetPluginDescriptor = const PluginDescriptor* (*)() noexcept;

} // namespace rivan::plugin
