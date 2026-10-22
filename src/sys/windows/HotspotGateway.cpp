// <winsock2.h> must come before any <windows.h>/<winsock.h> in this TU (the unity build never merges this file with
// other TUs; see the SKIP_UNITY_BUILD_INCLUSION note in CMakeLists.txt).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <netioapi.h>
#include <iphlpapi.h>

#include "include/sys/HotspotGateway.hpp"

#include "include/sys/HostTun.hpp"

#include <QFile>
#include <QScopeGuard>
#include <QString>
#include <QStringList>

#include <windns.h>

#ifdef __MINGW32__
// mingw-w64's windns.h omits this prototype although dnsapi exports it; the SDK declares it on MSVC.
extern "C" DNS_STATUS WINAPI DnsFlushResolverCache(VOID);
#endif

#include <algorithm>
#include <limits>
#include <mutex>
#include <vector>

namespace {
    std::mutex HgWatchMutex;
    HANDLE HgRouteNotifyHandle = nullptr;
    HANDLE HgIfcNotifyHandle = nullptr;
    std::function<void()> HgChangeSink;

    // Names that mark a non-physical interface: other VPNs and tunnels, virtual switches and container bridges.
    bool HgVirtualAdapter(const QString &text) {
        static const QStringList keywords = {
            QStringLiteral("throne-tun"), QStringLiteral("virtual"), QStringLiteral("vmware"), QStringLiteral("vbox"),
            QStringLiteral("virtualbox"), QStringLiteral("hyper-v"), QStringLiteral("vethernet"), QStringLiteral("wsl"),
            QStringLiteral("docker"), QStringLiteral("bridge"), QStringLiteral("tap"), QStringLiteral("tun"),
            QStringLiteral("wireguard"), QStringLiteral("wintun"), QStringLiteral("openvpn"), QStringLiteral("tailscale"),
            QStringLiteral("zerotier"), QStringLiteral("hamachi"), QStringLiteral("npcap"), QStringLiteral("loopback"),
            QStringLiteral("wan miniport"), QStringLiteral("anyconnect"), QStringLiteral("pangp"), QStringLiteral("juniper"),
            QStringLiteral("fortinet"), QStringLiteral("nordlynx"), QStringLiteral("mullvad"), QStringLiteral("protonvpn"),
            QStringLiteral("windscribe"), QStringLiteral("outline"), QStringLiteral("softether"), QStringLiteral("cisco"),
        };
        const QString lower = text.toLower();
        return std::any_of(keywords.cbegin(), keywords.cend(), [&lower](const QString &keyword) {
            return lower.contains(keyword);
        });
    }

    struct HgAdapterKey {
        NET_LUID luid{};
        NET_IFINDEX ifIndex = 0;
        bool wifi = false;
    };

    // The Up, physical adapters worth routing through. Only identities and the Wi-Fi flag survive the walk: the rows
    // point into the GetAdaptersAddresses buffer, which is gone when this returns.
    std::vector<HgAdapterKey> HgUpAdapters(bool *anyWifi) {
        std::vector<HgAdapterKey> result;
        if (anyWifi != nullptr) *anyWifi = false;
        ULONG size = 16 * 1024;
        std::vector<char> buffer(size);
        auto addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
        const auto flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
        DWORD ret = GetAdaptersAddresses(AF_INET, flags, nullptr, addresses, &size);
        // MSDN: a too-small buffer comes back as ERROR_BUFFER_OVERFLOW, with the required size in size.
        if (ret == ERROR_BUFFER_OVERFLOW) {
            buffer.resize(size);
            addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
            ret = GetAdaptersAddresses(AF_INET, flags, nullptr, addresses, &size);
        }
        if (ret != NO_ERROR) return result;
        for (auto adapter = addresses; adapter != nullptr; adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp) continue;
            if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK || adapter->IfType == IF_TYPE_TUNNEL) continue;
            const QString friendly = QString::fromWCharArray(adapter->FriendlyName);
            const QString description = QString::fromWCharArray(adapter->Description);
            if (HgVirtualAdapter(friendly) || HgVirtualAdapter(description)) continue;
            const bool wifi = adapter->IfType == IF_TYPE_IEEE80211;
            result.push_back({adapter->Luid, adapter->IfIndex, wifi});
            if (wifi && anyWifi != nullptr) *anyWifi = true;
        }
        return result;
    }

    // InterfaceLuid identifies the row's adapter; a zero Luid falls back to the interface index.
    const HgAdapterKey *HgFindAdapter(const std::vector<HgAdapterKey> &adapters, const MIB_IPFORWARD_ROW2 &row) {
        for (const auto &adapter : adapters) {
            const bool match = row.InterfaceLuid.Value != 0 ? adapter.luid.Value == row.InterfaceLuid.Value
                                                           : adapter.ifIndex == row.InterfaceIndex;
            if (match) return &adapter;
        }
        return nullptr;
    }

    // The next hop as text, empty for an unusable one (not IPv4, unspecified, loopback or link-local).
    QString HgUsableGateway(const MIB_IPFORWARD_ROW2 &row) {
        if (row.NextHop.si_family != AF_INET) return {};
        const IN_ADDR &addr = row.NextHop.Ipv4.sin_addr;
        const quint32 host = ntohl(addr.S_un.S_addr);
        if (host == 0 || (host >> 24) == 127 || (host >> 16) == 0xA9FE) return {};
        char text[INET_ADDRSTRLEN] = {};
        if (InetNtopA(AF_INET, const_cast<IN_ADDR *>(&addr), text, sizeof(text)) == nullptr) return {};
        return QString::fromLatin1(text);
    }

    // Delivered on a system thread-pool thread: hand the event over and return at once.
    void HgDeliverChange() {
        std::function<void()> sink;
        {
            std::lock_guard<std::mutex> lock(HgWatchMutex);
            sink = HgChangeSink;
        }
        if (sink) sink();
    }

    // The SDK types NotifyRouteChange2's callback as PIPFORWARD_CHANGE_CALLBACK: (context, row, notification type).
    // The row is not needed here.
    VOID NETIOAPI_API_ HgRouteChanged(PVOID, PMIB_IPFORWARD_ROW2, MIB_NOTIFICATION_TYPE) {
        HgDeliverChange();
    }

    VOID NETIOAPI_API_ HgIfcChanged(PVOID, PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE) {
        HgDeliverChange();
    }
} // namespace

namespace Sys::HotspotGateway {
    QString CurrentGateway() {
        PMIB_IPFORWARD_TABLE2 table = nullptr;
        if (GetIpForwardTable2(AF_INET, &table) != NO_ERROR || table == nullptr) return {};
        const auto freeTable = qScopeGuard([table] { FreeMibTable(table); });

        bool anyWifi = false;
        const auto adapters = HgUpAdapters(&anyWifi);

        QString best;
        ULONG bestMetric = (std::numeric_limits<ULONG>::max)();
        bool bestIsWifi = false;
        for (ULONG i = 0; i < table->NumEntries; ++i) {
            const MIB_IPFORWARD_ROW2 &row = table->Table[i];
            // The IPv4 default route only: 0.0.0.0/0. This table holds no IPv6 rows at all, so a link-local IPv6
            // gateway can never be picked.
            if (row.DestinationPrefix.Prefix.si_family != AF_INET || row.DestinationPrefix.PrefixLength != 0) continue;
            if (row.DestinationPrefix.Prefix.Ipv4.sin_addr.S_un.S_addr != 0) continue;
            const QString gateway = HgUsableGateway(row);
            if (gateway.isEmpty()) continue;
            const HgAdapterKey *adapter = HgFindAdapter(adapters, row);
            if (adapter == nullptr) continue;
            // A Wi-Fi default route wins; within one class the lowest route metric wins (the row carries no interface
            // metric of its own).
            const ULONG metric = row.Metric;
            if (best.isEmpty() || (adapter->wifi && !bestIsWifi) || (adapter->wifi == bestIsWifi && metric < bestMetric)) {
                best = gateway;
                bestMetric = metric;
                bestIsWifi = adapter->wifi;
            }
        }
        return best;
    }

    bool StartWatching(std::function<void()> onChange) {
        std::lock_guard<std::mutex> lock(HgWatchMutex);
        if (HgRouteNotifyHandle != nullptr || HgIfcNotifyHandle != nullptr) return true;
        HgChangeSink = std::move(onChange);
        // Both arrive on foreign threads; the sink marshals to the Qt main thread. No initial notification: the caller
        // reads the gateway itself when it wants one.
        if (NotifyRouteChange2(AF_UNSPEC, HgRouteChanged, nullptr, FALSE, &HgRouteNotifyHandle) != NO_ERROR) {
            HgRouteNotifyHandle = nullptr;
        }
        if (NotifyIpInterfaceChange(AF_INET, HgIfcChanged, nullptr, FALSE, &HgIfcNotifyHandle) != NO_ERROR) {
            HgIfcNotifyHandle = nullptr;
        }
        if (HgRouteNotifyHandle == nullptr && HgIfcNotifyHandle == nullptr) {
            HgChangeSink = nullptr;
            return false;
        }
        return true;
    }

    void StopWatching() {
        HANDLE routeHandle = nullptr;
        HANDLE ifcHandle = nullptr;
        {
            std::lock_guard<std::mutex> lock(HgWatchMutex);
            routeHandle = HgRouteNotifyHandle;
            ifcHandle = HgIfcNotifyHandle;
            HgRouteNotifyHandle = nullptr;
            HgIfcNotifyHandle = nullptr;
            HgChangeSink = nullptr;
        }
        // CancelMibChangeNotify2 waits for the callbacks that are still running, and the lock is not held here, so a
        // callback racing this teardown cannot deadlock against it.
        if (routeHandle != nullptr) CancelMibChangeNotify2(routeHandle);
        if (ifcHandle != nullptr) CancelMibChangeNotify2(ifcHandle);
    }

    QString SetHostsEntry(const QString &host, const QString &address) {
        wchar_t systemDir[MAX_PATH] = {};
        if (GetSystemDirectoryW(systemDir, MAX_PATH) == 0) return QStringLiteral("GetSystemDirectory failed (%1)").arg(GetLastError());
        const QString path = QString::fromWCharArray(systemDir) + QStringLiteral("\\drivers\\etc\\hosts");

        // Keep every line that does not map this host; the mapping is rebuilt, so an old gateway never lingers.
        QStringList lines;
        QFile file(path);
        if (file.exists()) {
            if (!file.open(QIODevice::ReadOnly)) return QStringLiteral("%1: %2").arg(path, file.errorString());
            while (!file.atEnd()) {
                const QString line = QString::fromLatin1(file.readLine()).trimmed();
                const qsizetype comment = line.indexOf(QLatin1Char('#'));
                const QString fields = (comment < 0 ? line : line.left(comment)).simplified();
                if (!fields.split(QLatin1Char(' '), Qt::SkipEmptyParts).contains(host)) lines << line;
            }
            file.close();
        }
        lines << address + QLatin1Char(' ') + host;
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return QStringLiteral("%1: %2").arg(path, file.errorString());
        file.write((lines.join(QStringLiteral("\r\n")) + QStringLiteral("\r\n")).toLatin1());
        file.close();

        DnsFlushResolverCache();
        return {};
    }
} // namespace Sys::HotspotGateway
