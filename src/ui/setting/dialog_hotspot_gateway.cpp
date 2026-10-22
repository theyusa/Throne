#include "include/ui/setting/dialog_hotspot_gateway.h"

#include "include/database/DatabaseManager.h"
#include "include/engine/services/HotspotGatewayManager.h"
#include "include/global/Utils.hpp"
#include "include/global/Validation.hpp"
#include "include/ui/utils/ValidationForm.h"

#include <QDialogButtonBox>

DialogHotspotGateway::DialogHotspotGateway(QWidget *parent)
    : QDialog(parent), ui(new Ui::DialogHotspotGateway) {
    ui->setupUi(this);
    form = new Validation::Form(this);
    form->attachStatus(ui->buttonBox);

    const auto &settings = *Configs::dataManager->settingsRepo;
    ui->port->setText(Int2String(settings.hotspot_gateway_port));
    ui->username->setText(settings.hotspot_gateway_user);
    // Shown masked, read back exactly as typed.
    ui->password->setText(settings.hotspot_gateway_pass);
    ui->uot->setChecked(settings.hotspot_gateway_uot);
    const QString gateway = Engine::HotspotGatewayManager::instance()->currentGateway();
    ui->gateway_note->setText(gateway.isEmpty() ? tr("no Wi-Fi gateway") : tr("Current gateway: %1").arg(gateway));

    bindValidation();
    connect(ui->buttonBox, &QDialogButtonBox::accepted, this, &DialogHotspotGateway::accept);
    connect(ui->buttonBox, &QDialogButtonBox::rejected, this, &DialogHotspotGateway::reject);
}

DialogHotspotGateway::~DialogHotspotGateway() {
    delete ui;
}

void DialogHotspotGateway::bindValidation() {
    // An integer 1..65535; empty or anything else is an error, so nothing falls back to a default port silently.
    form->bind(ui->port, Validation::Rules::Port()).label(ui->port_l->text());
}

void DialogHotspotGateway::accept() {
    // Marks the invalid fields and shows the problem; this dialog stays open and nothing is saved.
    if (!form->validateAll()) return;

    auto &settings = *Configs::dataManager->settingsRepo;
    settings.hotspot_gateway_port = ui->port->text().trimmed().toInt();
    settings.hotspot_gateway_user = ui->username->text().trimmed();
    // Not trimmed: the password is stored exactly as it was typed.
    settings.hotspot_gateway_pass = ui->password->text();
    settings.hotspot_gateway_uot = ui->uot->isChecked();
    settings.Save();
    QDialog::accept();
}
