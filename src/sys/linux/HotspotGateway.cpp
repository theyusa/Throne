#include "include/sys/HotspotGateway.hpp"

#include "include/sys/HostTun.hpp"

#include <QFileInfo>
#include <QScopeGuard>
#include <QSocketNotifier>
#include <QString>
#include <QStringList>

#include <arpa/inet.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <functional>
#include <memory>

namespace {
    int HgWatchSocket = -1;
    std::unique_ptr<QSocketNotifier> HgWatchNotifier;

    // Interface name prefixes and substrings that mark a non-physical interface: tunnels, container and virtual-machine
    // bridges, and other VPNs.
    bool HgVirtualInterface(const QString &name) {
        static const QStringList keywords = {
            QStringLiteral("tun"), QStringLiteral("tap"), QStringLiteral("wg"), QStringLiteral("wireguard"),
            QStringLiteral("tailscale"), QStringLiteral("zt"), QStringLiteral("docker"), QStringLiteral("br-"),
            QStringLiteral("veth"), QStringLiteral("virbr"), QStringLiteral("vmnet"), QStringLiteral("vboxnet"),
            QStringLiteral("hamachi"), QStringLiteral("dummy"), QStringLiteral("bond"), QStringLiteral("macvlan"),
            QStringLiteral("ipvlan"), QStringLiteral("sit"), QStringLiteral("gre"), QStringLiteral("ipip"),
            QStringLiteral("teql"), QStringLiteral("ifb"), QStringLiteral("vcan"), QStringLiteral("netns"),
        };
        const QString lower = name.toLower();
        return std::any_of(keywords.cbegin(), keywords.cend(), [&lower](const QString &keyword) {
            return lower.contains(keyword);
        });
    }

    QString HgInterfaceName(int ifIndex) {
        char name[IF_NAMESIZE] = {};
        if (if_indextoname(static_cast<unsigned>(ifIndex), name) == nullptr) return {};
        return QString::fromLatin1(name);
    }

    // The physical uplink of a hotspot client is its WLAN interface; sysfs says which one that is.
    bool HgIsWifiInterface(const QString &name) {
        return QFileInfo::exists(QStringLiteral("/sys/class/net/%1/wireless").arg(name)) ||
               QFileInfo::exists(QStringLiteral("/sys/class/net/%1/phy80211").arg(name));
    }

    QString HgUsableGateway(const void *raw, int length) {
        if (length != 4) return {};
        quint32 host = 0;
        std::memcpy(&host, raw, 4);
        host = ntohl(host);
        if (host == 0 || (host >> 24) == 127 || (host >> 16) == 0xA9FE) return {};
        char text[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, raw, text, sizeof(text)) == nullptr) return {};
        return QString::fromLatin1(text);
    }

    // One RTM_GETROUTE dump answering which default route this machine would take; a throwaway socket per read, since
    // reads only happen when something changed.
    QString HgReadDefaultGateway() {
        const int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
        if (fd < 0) return {};
        const auto closeFd = qScopeGuard([fd] { close(fd); });

        struct sockaddr_nl kernel {};
        kernel.nl_family = AF_NETLINK;
        struct {
            struct nlmsghdr nlh;
            struct rtmsg rtm;
        } request {};
        request.nlh.nlmsg_len = sizeof(request);
        request.nlh.nlmsg_type = RTM_GETROUTE;
        request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
        request.nlh.nlmsg_seq = 1;
        request.rtm.rtm_family = AF_INET;
        if (sendto(fd, &request, sizeof(request), 0, reinterpret_cast<struct sockaddr *>(&kernel), sizeof(kernel)) < 0) return {};

        QString best;
        unsigned bestMetric = 0;
        bool bestIsWifi = false;
        char buffer[32 * 1024];
        bool done = false;
        while (!done) {
            const ssize_t received = recv(fd, buffer, sizeof(buffer), 0);
            if (received <= 0) {
                if (received < 0 && errno == EINTR) continue;
                break;
            }
            int remaining = static_cast<int>(received);
            for (auto *nlh = reinterpret_cast<struct nlmsghdr *>(buffer); NLMSG_OK(nlh, remaining);
                 nlh = NLMSG_NEXT(nlh, remaining)) {
                if (nlh->nlmsg_type == NLMSG_DONE) {
                    done = true;
                    break;
                }
                if (nlh->nlmsg_type == NLMSG_ERROR || nlh->nlmsg_type != RTM_NEWROUTE) continue;
                const auto *rtm = reinterpret_cast<struct rtmsg *>(NLMSG_DATA(nlh));
                // The IPv4 default route only; netlink carries IPv6 rows in other dumps, never here (AF_INET request).
                if (rtm->rtm_family != AF_INET || rtm->rtm_dst_len != 0) continue;
                if (rtm->rtm_type != RTN_UNICAST) continue;
                QString gateway;
                int oif = -1;
                unsigned metric = 0;
                int payloadLength = RTM_PAYLOAD(nlh);
                for (auto *rta = RTM_RTA(rtm); RTA_OK(rta, payloadLength); rta = RTA_NEXT(rta, payloadLength)) {
                    switch (rta->rta_type) {
                        case RTA_GATEWAY: gateway = HgUsableGateway(RTA_DATA(rta), RTA_PAYLOAD(rta)); break;
                        case RTA_OIF: if (RTA_PAYLOAD(rta) == 4) oif = *reinterpret_cast<int *>(RTA_DATA(rta)); break;
                        case RTA_PRIORITY: if (RTA_PAYLOAD(rta) == 4) metric = *reinterpret_cast<unsigned *>(RTA_DATA(rta)); break;
                        default: break;
                    }
                }
                if (gateway.isEmpty() || oif < 0) continue;
                const QString name = HgInterfaceName(oif);
                if (name.isEmpty() || name == QStringLiteral("lo") || name == QLatin1String(Sys::HostTun::kTunName) ||
                    HgVirtualInterface(name)) continue;
                const bool wifi = HgIsWifiInterface(name);
                if (best.isEmpty() || (wifi && !bestIsWifi) || (wifi == bestIsWifi && metric < bestMetric)) {
                    best = gateway;
                    bestMetric = metric;
                    bestIsWifi = wifi;
                }
            }
        }
        return best;
    }
} // namespace

namespace Sys::HotspotGateway {
    QString CurrentGateway() {
        return HgReadDefaultGateway();
    }

    bool StartWatching(std::function<void()> onChange) {
        if (HgWatchSocket >= 0) return true;
        const int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
        if (fd < 0) return false;
        struct sockaddr_nl local {};
        local.nl_family = AF_NETLINK;
        // IPv4 route changes only: adding or removing 0.0.0.0/0 lands here, which is all the gateway cares about.
        local.nl_groups = RTMGRP_IPV4_ROUTE;
        if (bind(fd, reinterpret_cast<struct sockaddr *>(&local), sizeof(local)) < 0) {
            close(fd);
            return false;
        }
        HgWatchSocket = fd;
        // Idle cost is one epoll registration inside the Qt dispatcher: no timer, no thread, no polling.
        HgWatchNotifier = std::make_unique<QSocketNotifier>(fd, QSocketNotifier::Read);
        QObject::connect(HgWatchNotifier.get(), &QSocketNotifier::activated, HgWatchNotifier.get(),
                         [fd, sink = std::move(onChange)] {
                             // Drain the notifications (route churn arrives in bursts) and let the sink debounce.
                             char buffer[8 * 1024];
                             while (recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT) > 0) {}
                             if (sink) sink();
                         });
        return true;
    }

    void StopWatching() {
        HgWatchNotifier.reset();
        if (HgWatchSocket >= 0) {
            close(HgWatchSocket);
            HgWatchSocket = -1;
        }
    }
} // namespace Sys::HotspotGateway
