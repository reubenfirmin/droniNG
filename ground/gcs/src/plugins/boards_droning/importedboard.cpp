/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "importedboard.h"
#include "hwimported.h"
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
        QList<QPair<QString, QString>> fields{
            {"Receiver", radix ? tr("Receiver · UART3") : tr("Receiver · UART6")}, {"DSMxMode", tr("DSM mode")},
            {"Telemetry", tr("External telemetry · UART1")}, {"GPS", radix ? tr("GPS · UART4") : tr("GPS · UART3")},
            {"USB_VCPPort", tr("USB serial")},
        };
        if (radix) {
            fields.append({"ReceiverTelemetry", tr("Receiver telemetry · UART6")});
            fields.append({"BuzzerType", tr("Buzzer")});
        } else fields.prepend({"ReceiverPort", tr("Receiver connector")});
        for (const auto &field : fields) {
            auto *combo = new QComboBox(this); combo->setObjectName("imported" + field.first);
            form->addRow(field.second, combo);
            addUAVObjectToWidgetRelation("HwImported", field.first, combo);
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
        auto *reboot = new QPushButton(tr("Reboot"), this);
        buttons->addStretch(); buttons->addWidget(reboot); buttons->addWidget(apply); buttons->addWidget(save);
        layout->addLayout(buttons);
        apply->setProperty("objrelation", QStringList{"button:apply"});
        save->setProperty("objrelation", QStringList{"button:save"});
        reboot->setProperty("objrelation", QStringList{"button:reboot"});
        autoLoadWidgets();
        populateWidgets(); refreshWidgetsValues(); disableMouseWheelEvents();
        if (!radix) {
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
    return capability == BOARD_CAPABILITIES_GYROS || capability == BOARD_CAPABILITIES_ACCELS
        || capability == BOARD_CAPABILITIES_BAROS || capability == BOARD_CAPABILITIES_OSD;
    // Updates use the firmware page's ROM DFU installer, not the legacy HID loader.
}
bool ImportedBoard::isInputConfigurationSupported(InputType type)
{ return type == INPUT_TYPE_ANY || receiverModes.contains(type); }
bool ImportedBoard::setInputType(InputType type)
{
    auto *object = settings();
    if (!object || !receiverModes.contains(type)) return false;
    auto data = object->getData(); data.Receiver = receiverModes.value(type); object->setData(data);
    return true;
}
Core::IBoardType::InputType ImportedBoard::getInputType()
{
    auto *object = settings();
    return object ? receiverModes.key(object->getData().Receiver, INPUT_TYPE_UNKNOWN) : INPUT_TYPE_UNKNOWN;
}
bool ImportedBoard::hasAnnunciator(AnnunciatorType type)
{ return type == ANNUNCIATOR_HEARTBEAT || type == ANNUNCIATOR_ALARM || type == ANNUNCIATOR_BUZZER; }
QWidget *ImportedBoard::getBoardConfiguration(QWidget *parent, bool connected)
{ return new ImportedConfiguration(m_profile, parent, connected); }
