/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef FIRMWAREINSTALLWIDGET_H
#define FIRMWAREINSTALLWIDGET_H
#include "firmwarecatalog.h"
#include <QWidget>
#include <QPointer>
namespace Core {
class ConnectionManager;
class DfuInstaller;
class DfuCommandRunner;
class CORE_EXPORT FirmwareInstallWidget : public QWidget {
    Q_OBJECT
public:
    FirmwareInstallWidget(ConnectionManager *manager, const FirmwareBoardProfile &board,
                          const QString &imagePath, QWidget *parent = nullptr, DfuCommandRunner *runner = nullptr);
    void cancelInstallation();
    bool verifyingConnection() const;
    bool complete() const;
    ~FirmwareInstallWidget() override;
signals:
    void closed();
    void workflowChanged();
private:
    QPointer<ConnectionManager> m_manager;
    DfuInstaller *m_installer;
    bool m_autoStartAttempted = false;
};
}
#endif
