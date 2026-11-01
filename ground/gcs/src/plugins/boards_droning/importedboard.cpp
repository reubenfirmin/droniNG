/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "importedboard.h"
#include "hwimported.h"
#include "hwspracingf3.h"
#include "board_usb_ids.h"
#include <extensionsystem/pluginmanager.h>
#include <uavobjects/uavobjectmanager.h>
#include <uavobjectwidgetutils/configtaskwidget.h>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QMap>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

namespace {
const QMap<Core::IBoardType::InputType, quint8> receiverModes{
    {Core::IBoardType::INPUT_TYPE_DISABLED, HwImported::RECEIVER_DISABLED},
    {Core::IBoardType::INPUT_TYPE_SBUS, HwImported::RECEIVER_SBUS},
    {Core::IBoardType::INPUT_TYPE_SBUSNONINVERTED, HwImported::RECEIVER_SBUSNONINVERTED},
    {Core::IBoardType::INPUT_TYPE_DSM, HwImported::RECEIVER_DSM},
    {Core::IBoardType::INPUT_TYPE_HOTTSUMD, HwImported::RECEIVER_HOTTSUMD},
    {Core::IBoardType::INPUT_TYPE_HOTTSUMH, HwImported::RECEIVER_HOTTSUMH},
    {Core::IBoardType::INPUT_TYPE_SRXL, HwImported::RECEIVER_SRXL},
    {Core::IBoardType::INPUT_TYPE_IBUS, HwImported::RECEIVER_IBUS},
    {Core::IBoardType::INPUT_TYPE_TBSCROSSFIRE, HwImported::RECEIVER_TBSCROSSFIRE},
};
HwImported *settings()
{
    auto *manager = ExtensionSystem::PluginManager::instance()->getObject<UAVObjectManager>();
    return manager ? HwImported::GetInstance(manager) : nullptr;
}
HwSpracingF3 *f3Settings()
{
    auto *manager = ExtensionSystem::PluginManager::instance()->getObject<UAVObjectManager>();
    return manager ? HwSpracingF3::GetInstance(manager) : nullptr;
}
class ImportedConfiguration final : public ConfigTaskWidget {
public:
    ImportedConfiguration(const Core::FirmwareBoardProfile &profile, QWidget *parent, bool connected)
        : ConfigTaskWidget(parent)
    {
        setObjectName("importedBoardConfiguration");
        auto *layout = new QVBoxLayout(this);
        auto *title = new QLabel(profile.name, this); title->setProperty("role", "heading");
        layout->addWidget(title);
        auto *notice = new QLabel(tr("Save and reboot after changing ports."), this);
        notice->setWordWrap(true); layout->addWidget(notice);
        auto *form = new QFormLayout;
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        const bool radix = profile.id == "radix";
        const bool f3 = profile.id == "spracingf3";
        QList<QPair<QString, QString>> fields{
            {"Receiver", radix ? tr("Receiver · UART3") : tr("Receiver · UART6")}, {"DSMxMode", tr("DSM mode")},
            {"Telemetry", tr("External telemetry · UART1")}, {"GPS", radix ? tr("GPS · UART4") : tr("GPS · UART3")},
            {"USB_VCPPort", tr("USB serial")},
        };
        if (f3) {
            notice->setText(tr("UART1 is reserved for the USB bridge and GCS. Use UART2 for GPS, UART3 for a serial receiver or external MSP OSD, and RC1 for PPM. GPS plus external OSD requires a PPM receiver. Use an external inverter for an inverted S.Bus signal. UART2 shares the SWD clock pin. Save and reboot after changing ports."));
            fields = {{"Uart2", tr("UART2 · TX2/RX2")}, {"Uart3", tr("UART3 · TX3/RX3")},
                {"PPM", tr("PPM receiver · RC1")}, {"DSMxMode", tr("DSM mode")}};
        } else if (radix) {
            fields.append({"ReceiverTelemetry", tr("Receiver telemetry · UART6")});
            fields.append({"BuzzerType", tr("Buzzer")});
        } else fields.prepend({"ReceiverPort", tr("Receiver connector")});
        for (const auto &field : fields) {
            auto *combo = new QComboBox(this); combo->setObjectName("imported" + field.first);
            form->addRow(field.second, combo);
            addUAVObjectToWidgetRelation(f3 ? "HwSpracingF3" : "HwImported", field.first, combo);
        }
        if (radix) {
            auto *threshold = new QSpinBox(this); threshold->setRange(0, 255);
            form->addRow(tr("Video sync threshold"), threshold);
            addUAVObjectToWidgetRelation("HwImported", "VideoSyncThreshold", threshold);
        }
        layout->addLayout(form); layout->addStretch();
        auto *buttons = new QHBoxLayout;
        auto *apply = new QPushButton(tr("Apply"), this);
        auto *save = new QPushButton(tr("Save"), this); save->setProperty("primary", true);
        apply->setObjectName("settingsApply"); save->setObjectName("settingsSave");
        auto *reboot = new QPushButton(tr("Reboot"), this);
        buttons->addStretch(); buttons->addWidget(reboot); buttons->addWidget(apply); buttons->addWidget(save);
        layout->addLayout(buttons);
        apply->setProperty("objrelation", QStringList{"button:apply"});
        save->setProperty("objrelation", QStringList{"button:save"});
        reboot->setProperty("objrelation", QStringList{"button:reboot"});
        autoLoadWidgets();
        populateWidgets(); refreshWidgetsValues(); disableMouseWheelEvents();
        if (f3) {
            auto *warning = new QLabel(this); warning->setWordWrap(true); layout->insertWidget(2, warning);
            auto *uart2 = findChild<QComboBox *>("importedUart2");
            auto *uart3 = findChild<QComboBox *>("importedUart3");
            auto *ppm = findChild<QComboBox *>("importedPPM");
            const auto validate = [warning, uart2, uart3, ppm, apply, save, connected] {
                const auto a = uart2->currentText(), b = uart3->currentText();
                const bool conflict = ((a == "GPS" || a == "MSP") && a == b)
                    || (ppm->currentText() == "Enabled" && b != "Disabled" && b != "GPS" && b != "MSP");
                warning->setText(conflict ? tr("Choose each GPS/MSP role once, and select either PPM or a serial receiver before saving.") : QString());
                warning->setVisible(conflict); apply->setEnabled(connected && !conflict); save->setEnabled(connected && !conflict);
            };
            for (auto *combo : {uart2, uart3, ppm}) connect(combo, &QComboBox::currentIndexChanged, this, validate);
            QTimer::singleShot(0, this, validate);
        } else if (!radix) {
            auto *port = findChild<QComboBox *>("importedReceiverPort");
            const auto labels = [this, form, port] {
                const bool receiver3 = port->currentText() == "UART3";
                qobject_cast<QLabel *>(form->labelForField(findChild<QComboBox *>("importedReceiver")))
                    ->setText(receiver3 ? tr("Receiver · UART3") : tr("Receiver · UART6"));
                qobject_cast<QLabel *>(form->labelForField(findChild<QComboBox *>("importedGPS")))
                    ->setText(receiver3 ? tr("GPS · UART6") : tr("GPS · UART3"));
            };
            connect(port, &QComboBox::currentIndexChanged, this, labels);
            labels();
        }
        enableControls(connected);
        if (connected) forceConnectedState();
    }
};
}

ImportedBoard::ImportedBoard(const Core::FirmwareBoardProfile &profile) : m_profile(profile)
{
    boardType = profile.boardType;
    addFirmwareUSBInfo(USBInfo(DRONING_VID_DRONING_FIRMWARE, DRONING_PID_DRONING_FIRMWARE, BCD_DEVICE_FIRMWARE));
}
bool ImportedBoard::queryCapabilities(BoardCapabilities capability)
{
    if (m_profile.id == "spracingf3" && capability == BOARD_CAPABILITIES_OSD) return false;
    return capability == BOARD_CAPABILITIES_GYROS || capability == BOARD_CAPABILITIES_ACCELS
        || capability == BOARD_CAPABILITIES_BAROS || capability == BOARD_CAPABILITIES_OSD;
    // Updates use the firmware page's ROM DFU installer, not the legacy HID loader.
}
bool ImportedBoard::isInputConfigurationSupported(InputType type)
{ return m_profile.id == "spracingf3" ? type == INPUT_TYPE_ANY || type == INPUT_TYPE_PPM
        || (type != INPUT_TYPE_SBUS && receiverModes.contains(type)) : type == INPUT_TYPE_ANY || receiverModes.contains(type); }
bool ImportedBoard::setInputType(InputType type)
{
    if (m_profile.id == "spracingf3") {
        auto *object = f3Settings();
        if (!object || !isInputConfigurationSupported(type) || type == INPUT_TYPE_ANY) return false;
        auto data = object->getData();
        data.PPM = type == INPUT_TYPE_PPM ? HwSpracingF3::PPM_ENABLED : HwSpracingF3::PPM_DISABLED;
        if (type != INPUT_TYPE_PPM) data.Uart3 = receiverModes.value(type);
        else if (data.Uart3 != HwSpracingF3::UART3_GPS && data.Uart3 != HwSpracingF3::UART3_MSP) data.Uart3 = HwSpracingF3::UART3_DISABLED;
        object->setData(data); return true;
    }
    auto *object = settings();
    if (!object || !receiverModes.contains(type)) return false;
    auto data = object->getData(); data.Receiver = receiverModes.value(type); object->setData(data);
    return true;
}
Core::IBoardType::InputType ImportedBoard::getInputType()
{
    if (m_profile.id == "spracingf3") {
        auto *object = f3Settings();
        if (!object) return INPUT_TYPE_UNKNOWN;
        const auto data = object->getData();
        return data.PPM == HwSpracingF3::PPM_ENABLED ? INPUT_TYPE_PPM
            : data.Uart3 == HwSpracingF3::UART3_GPS || data.Uart3 == HwSpracingF3::UART3_MSP ? INPUT_TYPE_DISABLED
            : receiverModes.key(data.Uart3, INPUT_TYPE_UNKNOWN);
    }
    auto *object = settings();
    return object ? receiverModes.key(object->getData().Receiver, INPUT_TYPE_UNKNOWN) : INPUT_TYPE_UNKNOWN;
}
bool ImportedBoard::hasAnnunciator(AnnunciatorType type)
{ return type == ANNUNCIATOR_HEARTBEAT || type == ANNUNCIATOR_ALARM || type == ANNUNCIATOR_BUZZER; }
QWidget *ImportedBoard::getBoardConfiguration(QWidget *parent, bool connected)
{ return new ImportedConfiguration(m_profile, parent, connected); }
