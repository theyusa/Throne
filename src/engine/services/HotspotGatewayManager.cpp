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
    // A new gateway must survive this before anything is rewritten: route flaps (a Wi-Fi adapter briefly dropping, a
    // second default route flickering in) are common while Tun is up and cost a core restart each if acted on at once.
    constexpr int kHotspotSettleMs = 2000;
    // At most one watcher-driven core restart per this window; a burst of real changes is applied on the next start.
    constexpr int kHotspotRestartCooldownMs = 10000;

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
        connect(m_debounce, &QTimer::timeout, this, [this] { observe(false); });

        m_settle = new QTimer(this);
        m_settle->setSingleShot(true);
        m_settle->setInterval(kHotspotSettleMs);
        connect(m_settle, &QTimer::timeout, this, [this] { observe(true); });

        m_restartDefer = new QTimer(this);
        m_restartDefer->setSingleShot(true);
        connect(m_restartDefer, &QTimer::timeout, this, [this] {
            if (!m_restartPending) return;
            m_restartPending = false;
            const int managedId = Configs::dataManager->settingsRepo->hotspot_gateway_profile_id;
            if (managedId != Configs::NoProfileId) restartRunning(managedId, true);
        });
    }

    QString HotspotGatewayManager::profileName() {
        // Not translated on purpose: the name doubles as the identity that keeps this to exactly one managed profile.
        return QStringLiteral("Hotspot Gateway");
    }

    void HotspotGatewayManager::init() {
        ENGINE_CHECK_MAIN_THREAD();
        if (!Configs::dataManager->settingsRepo->hotspot_gateway) return;
        ensureWatching();
        observe(false);
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
            m_settle->stop();
            m_restartDefer->stop();
            m_restartPending = false;
            m_candidate.clear();
            if (m_watching) {
                Sys::HotspotGateway::StopWatching();
                m_watching = false;
                LOG_INFO("[HotspotGateway] route watching stopped");
            }
            return;
        }
        ensureWatching();
        // Forced: the just-entered port/user/password/uot must reach the profile even when the gateway did not move.
        apply(true, true);
    }

    void HotspotGatewayManager::applySettings() {
        ENGINE_CHECK_MAIN_THREAD();
        apply(true, true);
    }

    void HotspotGatewayManager::beforeCoreStart() {
        ENGINE_CHECK_MAIN_THREAD();
        if (!Configs::dataManager->settingsRepo->hotspot_gateway) return;
        // If the registration failed earlier (a transient OS condition), try again; this is the last moment to catch up.
        ensureWatching();
        // No reload from here: the start this runs inside is about to build its config with the answer.
        apply(false);
    }

    void HotspotGatewayManager::shutdown() {
        ENGINE_CHECK_MAIN_THREAD();
        m_debounce->stop();
        m_settle->stop();
        m_restartDefer->stop();
        m_restartPending = false;
        if (m_watching) {
            Sys::HotspotGateway::StopWatching();
            m_watching = false;
        }
    }

    void HotspotGatewayManager::scheduleCheck() {
        // Bursts (a DHCP renewal moves several routes) collapse into one check; the timer is only alive while they last.
        m_debounce->start();
    }

    void HotspotGatewayManager::observe(bool confirmed) {
        ENGINE_CHECK_MAIN_THREAD();
        auto &settings = *Configs::dataManager->settingsRepo;
        if (!settings.hotspot_gateway) return;

        const QString gateway = Sys::HotspotGateway::CurrentGateway();
        if (gateway != m_gateway) {
            m_gateway = gateway;
            emit gatewayChanged();
        }
        // No Wi-Fi gateway (not on Wi-Fi): keep the last profile unchanged, do not delete anything.
        if (gateway.isEmpty()) {
            m_candidate.clear();
            return;
        }
        // Back to the stored value (a flap that recovered): nothing to do, and the pending settle dies with it.
        if (gateway == settings.hotspot_gateway_last) {
            m_candidate.clear();
            return;
        }
        if (!confirmed || gateway != m_candidate) {
            // First sighting, or it moved again while settling: settle on the newest value and wait.
            m_candidate = gateway;
            m_settle->start();
            return;
        }
        m_candidate.clear();
        hotspotLog(tr("Gateway changed: %1 -> %2").arg(settings.hotspot_gateway_last.isEmpty() ? QStringLiteral("-") : settings.hotspot_gateway_last, gateway));
        apply(true);
    }

    void HotspotGatewayManager::apply(bool allowReload, bool immediateRestart) {
        ENGINE_CHECK_MAIN_THREAD();
        auto &settings = *Configs::dataManager->settingsRepo;
        if (!settings.hotspot_gateway) return;

        const QString gateway = Sys::HotspotGateway::CurrentGateway();
        if (gateway != m_gateway) {
            m_gateway = gateway;
            emit gatewayChanged();
        }
        // Nothing to dial at: keep the last profile untouched until a gateway is back.
        if (gateway.isEmpty()) return;

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
            hotspotLog(tr("Managed profile created (#%1)").arg(ent->id));
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

        if (allowReload) restartRunning(ent->id, immediateRestart);
    }

    void HotspotGatewayManager::restartRunning(int managedId, bool immediate) {
        ENGINE_CHECK_MAIN_THREAD();
        auto &settings = *Configs::dataManager->settingsRepo;
        // The running config dials this gateway; with nothing running, saving above is all it takes.
        if (settings.started_id != managedId && !Configs::RunningUsesProfile(managedId)) return;

        if (!immediate && m_lastRestart.isValid()) {
            if (const qint64 wait = kHotspotRestartCooldownMs - m_lastRestart.elapsed(); wait > 0) {
                // Rate-limited: the profile already carries the new gateway and the next start picks it up; a burst of
                // flaps must not become a burst of restarts.
                m_restartPending = true;
                m_restartDefer->start(static_cast<int>(wait));
                hotspotLog(tr("Restart deferred by %1 s (rate limit)").arg(wait / 1000));
                return;
            }
        }
        m_lastRestart.start();
        hotspotLog(tr("Restarting the running profile to follow the gateway"));
        if (auto *host = Engine::host(); host != nullptr && host->connection() != nullptr)
            host->connection()->restartStartedProfile(false);
    }
} // namespace Engine
