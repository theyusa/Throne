#pragma once

#include <QString>

#include <functional>

// Per-platform. The hotspot host is this machine's IPv4 default gateway, and it moves between sessions (DHCP), so it is
// read from the routing table and its changes are watched through the OS's own route notifications: no timers, no polling
// threads, and nothing at all running while the feature is off. IPv4 only.
namespace Sys::HotspotGateway {
    // The next hop of the IPv4 default route (0.0.0.0/0) on a physical interface, Wi-Fi preferred; empty when there is
    // none. Loopback, our own Tun (HostTun::kTunName) and virtual/tunnel adapters (other VPNs, Docker, WSL, Hyper-V,
    // VMware, ...) are ignored, because while Tun is up the system default route points at the Tun itself. Any thread.
    QString CurrentGateway();

    // Event-driven change notification. onChange runs on any thread (a Windows thread-pool thread, or the Qt thread for
    // the netlink notifier) and must marshal itself to where it needs to be. False when this platform cannot watch.
    bool StartWatching(std::function<void()> onChange);

    // Cancels the notification and releases everything StartWatching acquired; safe to call when not watching.
    void StopWatching();
} // namespace Sys::HotspotGateway
