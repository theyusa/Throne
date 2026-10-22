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
}

namespace Engine {
    HotspotGatewayManager *HotspotGatewayManager::instance() {
        static HotspotGatewayManager manager;
        return &manager;
    }

    HotspotGatewayManager::HotspotGatewayManager(QObject *parent) : QObject(parent) {
        m_debounce = new QTimer(this);
        m_debounce->setSingleShot(true);
        m_debounce->setInterval(kHotspotDebounceMs);
        connect(m_debounce, &QTimer::timeout, this, [this] { checkAndUpdate(false, true); });
    }

    QString HotspotGatewayManager::profileName() {
        // Not translated on purpose: the name doubles as the identity that keeps this to exactly one managed profile.
        return QStringLiteral("Hotspot Gateway");
    }

    void HotspotGatewayManager::init() {
        ENGINE_CHECK_MAIN_THREAD();
        if (!Configs::dataManager->settingsRepo->hotspot_gateway) return;
        ensureWatching();
        checkAndUpdate(false, true);
    }

    void HotspotGatewayManager::ensureWatching() {
        if (m_watching) return;
        m_watching = Sys::HotspotGateway::StartWatching([this] {
            // The notification arrives on a foreign thread (a Windows thread-pool thread): queue onto the Qt main
            // thread, where the debounce timer and the repositories live.
            QMetaObject::invokeMethod(this, [this] { scheduleCheck(); }, Qt::QueuedConnection);
        });
        if (!m_watching) LOG_WARN("[HotspotGateway] the platform cannot watch route changes");
    }

    void HotspotGatewayManager::setEnabled(bool enabled) {
        ENGINE_CHECK_MAIN_THREAD();
        if (!enabled) {
            m_debounce->stop();
            if (m_watching) {
                Sys::HotspotGateway::StopWatching();
                m_watching = false;
            }
            return;
        }
        ensureWatching();
        // Forced: the just-entered port/user/password/uot must reach the profile even when the gateway did not move.
        checkAndUpdate(true, true);
    }

    void HotspotGatewayManager::applySettings() {
        ENGINE_CHECK_MAIN_THREAD();
        checkAndUpdate(true, true);
    }

    void HotspotGatewayManager::beforeCoreStart() {
        ENGINE_CHECK_MAIN_THREAD();
        // No reload from here: the start this runs inside is about to build its config with the answer.
        checkAndUpdate(false, false);
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
        // Bursts (a DHCP renewal moves several routes) collapse into one check; the timer is only alive while they last.
        m_debounce->start();
    }

    void HotspotGatewayManager::checkAndUpdate(bool forceReload, bool allowReload) {
        ENGINE_CHECK_MAIN_THREAD();
        auto &settings = *Configs::dataManager->settingsRepo;
        if (!settings.hotspot_gateway) return;

        const QString gateway = Sys::HotspotGateway::CurrentGateway();
        if (gateway != m_gateway) {
            m_gateway = gateway;
            emit gatewayChanged();
        }
        // No Wi-Fi gateway (not on Wi-Fi): keep the last profile unchanged, do not delete anything.
        if (gateway.isEmpty()) return;
        // Only act when the gateway moved; forceReload rewrites the profile for an edited port/user/password too.
        if (gateway == settings.hotspot_gateway_last && !forceReload) return;

        settings.hotspot_gateway_last = gateway;

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
        }
        // Never touch anything else: only a SOCKS profile is ours to write.
        auto *socks = ent->Socks();
        if (socks == nullptr) {
            LOG_WARN("[HotspotGateway] the remembered profile is not a SOCKS profile; leaving it alone");
            return;
        }
        socks->name = profileName();
        socks->server = gateway;
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

        if (allowReload && (settings.started_id == ent->id || Configs::RunningUsesProfile(ent->id))) {
            // The running profile dials this gateway, so it restarts through the regular mechanism; with nothing
            // running, saving above is all it takes.
            if (auto *host = Engine::host(); host != nullptr && host->connection() != nullptr)
                host->connection()->restartStartedProfile(false);
        }
    }
} // namespace Engine
