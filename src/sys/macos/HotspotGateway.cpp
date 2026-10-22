#include "include/sys/HotspotGateway.hpp"

// macOS is not implemented yet; the seam exists so the platform can be added without touching the callers. A real
// implementation would read the default route of the primary service from the dynamic store (SCDynamicStoreCopyIPv6 /
// SCDynamicStoreCopyValue, "State:/Network/Global/IPv4" -> Router) and watch "State:/Network/Global/IPv4" changes.
namespace Sys::HotspotGateway {
    QString CurrentGateway() {
        return {};
    }

    bool StartWatching(std::function<void()>) {
        return false;
    }

    void StopWatching() {
    }
} // namespace Sys::HotspotGateway
