// PluginHost.cpp
#include "PluginHost.h"

#include <algorithm>

namespace rivan::plugin {

PluginHost::~PluginHost() { StopAll(); }

bool PluginHost::IsCompatible(const PluginDescriptor& descriptor) noexcept {
    const auto kind = static_cast<std::uint32_t>(descriptor.kind);
    return descriptor.structureSize >= sizeof(PluginDescriptor) &&
           descriptor.apiVersion == kPluginApiVersion && descriptor.id != nullptr &&
           descriptor.id[0] != L'\0' && descriptor.displayName != nullptr &&
           descriptor.displayName[0] != L'\0' &&
           kind >= static_cast<std::uint32_t>(PluginKind::Visualization) &&
           kind <= static_cast<std::uint32_t>(PluginKind::Feature);
}

bool PluginHost::Register(std::unique_ptr<IPlugin> plugin) {
    if (!plugin || !IsCompatible(plugin->Descriptor()) || Find(plugin->Descriptor().id)) return false;
    // Reserve first so push_back cannot throw after Start(): a started plugin
    // must always be owned by plugins_.
    plugins_.reserve(plugins_.size() + 1);
    if (!plugin->Start()) return false;
    plugins_.push_back(std::move(plugin));
    return true;
}

bool PluginHost::Unregister(std::wstring_view id) noexcept {
    const auto found = std::find_if(plugins_.begin(), plugins_.end(), [id](const auto& plugin) {
        return std::wstring_view(plugin->Descriptor().id) == id;
    });
    if (found == plugins_.end()) return false;
    (*found)->Stop();
    plugins_.erase(found);
    return true;
}

IPlugin* PluginHost::Find(std::wstring_view id) const noexcept {
    const auto found = std::find_if(plugins_.begin(), plugins_.end(), [id](const auto& plugin) {
        return std::wstring_view(plugin->Descriptor().id) == id;
    });
    return found == plugins_.end() ? nullptr : found->get();
}

std::span<const std::unique_ptr<IPlugin>> PluginHost::Plugins() const noexcept { return plugins_; }

void PluginHost::StopAll() noexcept {
    for (auto it = plugins_.rbegin(); it != plugins_.rend(); ++it) (*it)->Stop();
    plugins_.clear();
}

} // namespace rivan::plugin
