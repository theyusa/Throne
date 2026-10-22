#include "include/engine/services/HotspotGatewayManager.h"

#include <QTimer>

#include "include/configs/outbounds/socks.h"
#include "include/database/DatabaseManager.h"
#include "include/database/ProfilesRepo.h"
#include "include/engine/EngineHost.h"
#include "include/engine/ThreadCheck.h"
#include "include/engine/connection/ConnectionManager.h"
#include "include/global/Logger.hpp"
#include "include/global/RunningProfiles.hpp"
#include "include/global/Utils.hpp"
#include "include/sys/HotspotGateway.hpp"

namespace {
    constexpr int kHotspotDebounceMs = 500;

    void hotspotLog(const QString &line) {
        MW_show_log(QStringLiteral("[HotspotGateway] ") + line);
    }
}

namespace Engine {
    HotspotGatewayManager *HotspotGatewayManager::instance() {
        static HotspotGatewayManager *manager = new HotspotGatewayManager;
        return manager;
    }

    HotspotGatewayManager::HotspotGatewayManager(QObject *parent) : QObject(parent) {
        m_debounce = new QTimer(this);
        m_debounce->setSingleShot(true);
        m_debounce->setInterval(kHotspotDebounceMs);
        connect(m_debounce, &QTimer::timeout, this, [this] { observe(); });
    }

    QString HotspotGatewayManager::profileName() {
        // Not translated on purpose: the name doubles as the identity that keeps this to exactly one managed profile.
        return QStringLiteral("Hotspot Gateway");
    }

    QString HotspotGatewayManager::profileHost() {
        // Not translated either: the hosts file maps this fixed name to the current gateway, so the profile - and the
        // running core - never see a gateway change at all.
        return QStringLiteral("telefon.lan");
    }

    void HotspotGatewayManager::init() {
        ENGINE_CHECK_MAIN_THREAD();
        if (!Configs::dataManager->settingsRepo->hotspot_gateway) return;
        ensureWatching();
        observe();
    }

    void HotspotGatewayManager::ensureWatching() {
        if (m_watching) return;
        m_watching = Sys::HotspotGateway::StartWatching([this] {
            // The notification arrives on a foreign thread (a Windows thread-pool thread): queue onto the Qt main
            // thread, where the debounce timer and the repositories live.
            QMetaObject::invokeMethod(this, [this] { scheduleCheck(); }, Qt::QueuedConnection);
        });
        if (m_watching) {
            LOG_INFO("[HotspotGateway] watching route changes");
        } else {
            LOG_WARN("[HotspotGateway] the platform cannot watch route changes; the gateway is only re-read at start");
        }
    }

    void HotspotGatewayManager::setEnabled(bool enabled) {
        ENGINE_CHECK_MAIN_THREAD();
        if (!enabled) {
            m_debounce->stop();
            if (m_watching) {
                Sys::HotspotGateway::StopWatching();
                m_watching = false;
                LOG_INFO("[HotspotGateway] route watching stopped");
            }
            // The profile and the hosts entry stay as they are; the profile keeps working for whoever uses it.
            return;
        }
        ensureWatching();
        saveProfile();
        observe();
    }

    void HotspotGatewayManager::applySettings() {
        ENGINE_CHECK_MAIN_THREAD();
        saveProfile();
    }

    void HotspotGatewayManager::beforeCoreStart() {
        ENGINE_CHECK_MAIN_THREAD();
        if (!Configs::dataManager->settingsRepo->hotspot_gateway) return;
        // If the registration failed earlier (a transient OS condition), try again; this is the last moment to catch up.
        ensureWatching();
        // Refreshes the hosts mapping and the gateway the Tun exclusion is built from.
        observe();
    }

    void HotspotGatewayManager::shutdown() {
        ENGINE_CHECK_MAIN_THREAD();
        m_debounce->stop();
        if (m_watching) {
            Sys::HotspotGateway::StopWatching();
            m_watching = false;
        }
    }

    void HotspotGatewayManager::scheduleCheck() {
        // Bursts (a DHCP renewal moves several routes) collapse into one read; the timer is only alive while they last.
        m_debounce->start();
    }

    void HotspotGatewayManager::observe() {
        ENGINE_CHECK_MAIN_THREAD();
        auto &settings = *Configs::dataManager->settingsRepo;
        if (!settings.hotspot_gateway) return;

        // The one read per event; nothing else ever queries the routing table.
        const QString gateway = Sys::HotspotGateway::CurrentGateway();
        if (gateway != m_gateway) {
            hotspotLog(gateway.isEmpty() ? tr("No Wi-Fi gateway: the default route is gone")
                                         : tr("Gateway is %1").arg(gateway));
            m_gateway = gateway;
            emit gatewayChanged();
        }
        // No Wi-Fi gateway (not on Wi-Fi): keep the last mapping, do not delete anything.
        if (gateway.isEmpty()) return;
        if (gateway == settings.hotspot_gateway_last) return;

        // The whole change handling: one hosts-file write. The profile keeps its fixed name and the running core
        // resolves it again on its next dial, so nothing is restarted and the profile is never touched.
        if (const QString error = Sys::HotspotGateway::SetHostsEntry(profileHost(), gateway); !error.isEmpty()) {
            hotspotLog(tr("Could not update the hosts file (%1); is Throne running as administrator?").arg(error));
            return;
        }
        hotspotLog(tr("%1 -> %2").arg(settings.hotspot_gateway_last.isEmpty() ? QStringLiteral("-") : settings.hotspot_gateway_last, gateway));
        settings.hotspot_gateway_last = gateway;
        settings.Save();
    }

    void HotspotGatewayManager::saveProfile() {
        ENGINE_CHECK_MAIN_THREAD();
        auto &settings = *Configs::dataManager->settingsRepo;

        auto &profiles = *Configs::dataManager->profilesRepo;
        std::shared_ptr<Configs::Profile> ent;
        if (settings.hotspot_gateway_profile_id != Configs::NoProfileId)
            ent = profiles.GetProfile(settings.hotspot_gateway_profile_id);
        if (ent == nullptr) {
            if (const auto found = profiles.GetProfileByName(profileName()); found != nullptr && found->Socks() != nullptr)
                ent = found;
        }
        if (ent == nullptr) {
            ent = Configs::ProfilesRepo::NewProfile(QStringLiteral("socks"));
            ent->outbound->name = profileName();
            if (!profiles.AddProfile(ent)) {
                LOG_WARN("[HotspotGateway] could not create the managed profile");
                return;
            }
            settings.hotspot_gateway_profile_id = ent->id;
            hotspotLog(tr("Managed profile created (#%1)").arg(ent->id));
        }
        // Never touch anything else: only a SOCKS profile is ours to write.
        auto *socks = ent->Socks();
        if (socks == nullptr) {
            LOG_WARN("[HotspotGateway] the remembered profile is not a SOCKS profile; leaving it alone");
            return;
        }
        socks->name = profileName();
        // The fixed name; only the hosts file follows the gateway.
        socks->server = profileHost();
        socks->server_port = settings.hotspot_gateway_port;
        socks->username = settings.hotspot_gateway_user;
        // Stored exactly as typed, so the profile carries it exactly as typed.
        socks->password = settings.hotspot_gateway_pass;
        socks->version = 5;
        socks->socks4a = false;
        socks->uot = settings.hotspot_gateway_uot;
        ent->name = profileName();
        profiles.Save(ent);
        // A profile adopted by name carries its id home, so the next update goes straight to it.
        if (settings.hotspot_gateway_profile_id != ent->id) settings.hotspot_gateway_profile_id = ent->id;
        settings.Save();

        // A user edit of port/user/password/uot must reach the running core too.
        if (settings.started_id == ent->id || Configs::RunningUsesProfile(ent->id)) {
            hotspotLog(tr("Restarting the running profile to apply the edited settings"));
            if (auto *host = Engine::host(); host != nullptr && host->connection() != nullptr)
                host->connection()->restartStartedProfile(false);
        }
    }
} // namespace Engine
