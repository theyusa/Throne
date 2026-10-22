#pragma once

#include <QObject>
#include <QString>

class QTimer;

namespace Engine {
    // The "Hotspot Gateway" feature: exactly one managed SOCKS5 profile, named like the Android app's, that always points
    // at the current Wi-Fi gateway of the network this machine joined. The gateway is the hotspot host's address and it
    // moves between sessions (DHCP), so an OS route notification (Sys::HotspotGateway) re-reads it and the profile is
    // rewritten in place; nothing runs while the feature is off. Main thread only.
    class HotspotGatewayManager : public QObject {
        Q_OBJECT

    public:
        static HotspotGatewayManager *instance();

        // The managed profile's name; the profile is found by this name or by the remembered id, and never anything else.
        static QString profileName();

        // Once at app start, after the database exists: with the feature on, reads the gateway and registers the OS
        // change notification. Nothing at all is registered while the feature is off.
        void init();

        // After the settings dialog changed the enabled flag: on starts the watcher and updates the profile at once,
        // off cancels the notification and the pending debounce.
        void setEnabled(bool enabled);

        // After the settings dialog saved port/user/password/uot: rewrites the managed profile and reloads the core when
        // that profile is part of the running config. With nothing to write it still does, so an edit never needs a
        // second step.
        void applySettings();

        // Right before the core starts: re-reads the gateway so the config that is about to be built carries it. It never
        // restarts anything: the start in flight already covers the change.
        void beforeCoreStart();

        // On app exit: cancels the OS notification.
        void shutdown();

        // What the settings page shows: the gateway last read, empty for "no Wi-Fi gateway".
        QString currentGateway() const { return m_gateway; }

    signals:
        // The gateway value changed (including to empty); the settings page redraws its description.
        void gatewayChanged();

    private:
        explicit HotspotGatewayManager(QObject *parent = nullptr);

        // Registers the OS route notification once; nothing is registered while the feature is off.
        void ensureWatching();

        // Marshals a watcher event to this thread and debounces bursts.
        void scheduleCheck();

        void checkAndUpdate(bool forceReload, bool allowReload);

        QString m_gateway;
        QTimer *m_debounce = nullptr;
        bool m_watching = false;
    };
} // namespace Engine
