/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "workspacelayout.h"
#include <QSettings>
#include <QVariantMap>
#include <QStringList>

namespace {
using Panels = QList<QVariantMap>;

Panels panelsIn(QSettings *settings, const QString &mode)
{
    Panels panels;
    settings->beginGroup("UAVGadgetManager/" + mode + "/splitter");
    const auto keys = settings->allKeys();
    for (const auto &key : keys) {
        if (key != "classId" && !key.endsWith("/classId")) continue;
        const QString prefix = key.left(key.size() - 7);
        QVariantMap panel;
        for (const auto &child : keys)
            if (child.startsWith(prefix)) panel.insert(child.mid(prefix.size()), settings->value(child));
        panels.append(panel);
    }
    settings->endGroup();
    return panels;
}

bool dashboardPanel(const QVariantMap &panel)
{
    const QString type = panel.value("classId").toString();
    const QString configuration = panel.value("gadget/activeConfiguration").toString();
    return (type == "LineardialGadget" && QStringList{"Flight Time", "Arm Status", "Flight mode"}.contains(configuration))
        || type == "PfdQmlGadget"
        || type == "ModelViewGadget" || type == "SystemHealthGadget";
}

void writePanels(QSettings *settings, const QString &path, const Panels &panels)
{
    if (panels.size() == 1) {
        for (auto it = panels.first().begin(); it != panels.first().end(); ++it)
            settings->setValue(path + "/" + it.key(), it.value());
        settings->setValue(path + "/type", "uavGadget");
        return;
    }
    settings->setValue(path + "/type", "splitter");
    settings->setValue(path + "/splitterOrientation", int(Qt::Horizontal));
    writePanels(settings, path + "/side0", {panels.first()});
    writePanels(settings, path + "/side1", panels.mid(1));
}

void replaceLayout(QSettings *settings, const QString &mode, const Panels &panels)
{
    const QString path = "UAVGadgetManager/" + mode;
    settings->remove(path + "/splitter");
    writePanels(settings, path + "/splitter", panels);
    settings->setValue(path + "/showToolbars", false);
    settings->setValue(path + "/version", "UAVGadgetManagerV1");
}
}

void Core::Internal::migrateFlightWorkspace(QSettings *settings)
{
    if (settings->value("Workspace/FlightDashboardVersion").toInt() >= 1) return;
    const int count = settings->value("Workspace/NumberOfWorkspaces").toInt();
    for (int i = 1; i <= count; ++i) {
        const QString workspace = "Workspace/Workspace" + QString::number(i);
        const QString name = settings->value(workspace).toString();
        const QString mode = "Mode" + QString::number(i);
        if (name.compare("Flight data", Qt::CaseInsensitive) == 0) {
            Panels remaining;
            bool hasMap = false;
            for (const auto &panel : panelsIn(settings, mode)) {
                const QString type = panel.value("classId").toString();
                const QString configuration = panel.value("gadget/activeConfiguration").toString();
                if (dashboardPanel(panel)) {
                    // Retain the user's instrument and model selections.
                    const QString key = type == "LineardialGadget" ? type + "/" + configuration : type;
                    settings->setValue("FlightDashboard/" + key + "/configuration", configuration);
                } else {
                    remaining.append(panel);
                    hasMap |= type == "OPMapGadget";
                }
            }
            if (!hasMap) remaining.prepend({{"classId", "OPMapGadget"}, {"type", "uavGadget"},
                {"gadget/activeConfiguration", "default"}});
            replaceLayout(settings, mode, remaining);
            settings->setValue(workspace, "Map");
            settings->setValue("Workspace/Icon" + QString::number(i), ":/core/images/navigation-map.svg");
        } else if (name == "Firmware") {
            // Older profiles also embedded duplicate flight panels beside the uploader.
            Panels remaining;
            bool hasUploader = false;
            for (const auto &panel : panelsIn(settings, mode)) {
                if (!dashboardPanel(panel)) remaining.append(panel);
                hasUploader |= panel.value("classId").toString() == "Uploader";
            }
            if (hasUploader && !remaining.isEmpty()) replaceLayout(settings, mode, remaining);
        }
    }
    settings->setValue("Workspace/FlightDashboardVersion", 1);
}
