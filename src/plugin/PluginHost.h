// PluginHost.h
// Owns explicitly supplied in-process plugins and validates their descriptors.
#pragma once

#include "PluginApi.h"

#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace rivan::plugin {

class PluginHost final {
public:
    PluginHost() = default;
    ~PluginHost();
    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    [[nodiscard]] static bool IsCompatible(const PluginDescriptor& descriptor) noexcept;
    [[nodiscard]] bool Register(std::unique_ptr<IPlugin> plugin);
    [[nodiscard]] bool Unregister(std::wstring_view id) noexcept;
    [[nodiscard]] IPlugin* Find(std::wstring_view id) const noexcept;
    [[nodiscard]] std::span<const std::unique_ptr<IPlugin>> Plugins() const noexcept;
    void StopAll() noexcept;

private:
    std::vector<std::unique_ptr<IPlugin>> plugins_;
};

} // namespace rivan::plugin
