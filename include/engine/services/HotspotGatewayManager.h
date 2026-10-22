#pragma once

#include <QObject>
#include <QString>

class QTimer;

namespace Engine {
    // The "Hotspot Gateway" feature: exactly one managed SOCKS5 profile, named like the Android app's, that dials the
    // hotspot host through a fixed name ("telefon.lan"). The gateway address lives only in the system hosts file, which
    // Sys::HotspotGateway rewrites when the gateway moves (an OS route notification, no polling): the running core picks
    // the new address up on its next dial, so a gateway change never touches the profile and never restarts anything.
    // Main thread only; nothing runs while the feature is off.
    class HotspotGatewayManager : public QObject {
        Q_OBJECT

    public:
        static HotspotGatewayManager *instance();

        // The managed profile's name; the profile is found by this name or by the remembered id, and never anything else.
        static QString profileName();

        // The fixed address the managed profile dials; the hosts file maps it to the current gateway.
        static QString profileHost();

        // Once at app start, after the database exists: with the feature on, reads the gateway and registers the OS
        // change notification. Nothing at all is registered while the feature is off.
        void init();

        // After the settings dialog changed the enabled flag: on writes the hosts mapping and the profile at once, off
        // cancels the notification. The profile and the hosts entry are left in place on disable.
        void setEnabled(bool enabled);

        // After the settings dialog saved port/user/password/uot: rewrites the managed profile and restarts it when it
        // runs, so the edited fields apply.
        void applySettings();

        // Right before the core starts: refreshes the hosts mapping so the config that is about to be built sees a
        // current gateway exclusion.
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

        // One gateway read per (debounced) event: writes the hosts mapping when the value moved, and does nothing at
        // all when it did not.
        void observe();

        // Creates or updates the managed profile from the stored settings; only user actions call this.
        void saveProfile();

        QString m_gateway;
        QTimer *m_debounce = nullptr;
        bool m_watching = false;
    };
} // namespace Engine
