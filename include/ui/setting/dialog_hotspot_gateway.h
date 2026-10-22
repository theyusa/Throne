#pragma once

#include <QDialog>

#include "ui_dialog_hotspot_gateway.h"

QT_BEGIN_NAMESPACE
namespace Ui {
    class DialogHotspotGateway;
}
QT_END_NAMESPACE

namespace Validation {
    class Form;
}

// Edits the managed "Hotspot Gateway" profile's fields: the SOCKS5 port of the host's inbound, optional credentials and
// the UDP-over-TCP switch. Nothing is written until the validation passes, so an invalid port keeps this open and saves
// nothing; there is no fallback to a default port. The password is stored exactly as typed.
class DialogHotspotGateway : public QDialog {
    Q_OBJECT

public:
    explicit DialogHotspotGateway(QWidget *parent = nullptr);

    ~DialogHotspotGateway() override;

public slots:

    void accept();

private:
    Ui::DialogHotspotGateway *ui;

    Validation::Form *form = nullptr;

    void bindValidation();
};
