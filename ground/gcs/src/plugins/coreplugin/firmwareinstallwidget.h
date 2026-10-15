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
class BetaflightBootController;
class IConnection;
class CORE_EXPORT FirmwareInstallWidget : public QWidget {
    Q_OBJECT
public:
    FirmwareInstallWidget(ConnectionManager *manager, const FirmwareBoardProfile &board,
                          const QString &imagePath, QWidget *parent = nullptr, DfuCommandRunner *runner = nullptr,
                          bool embedded = false);
    void cancelInstallation();
    bool verifyingConnection() const;
    bool complete() const;
    QString title() const;
    ~FirmwareInstallWidget() override;
signals:
    void closed();
    void workflowChanged();
private:
    QPointer<ConnectionManager> m_manager;
    DfuInstaller *m_installer;
    bool m_autoStartAttempted = false;
    bool m_softwareBootSent = false;
    BetaflightBootController *m_betaflightBoot = nullptr;
    QPointer<IConnection> m_bootConnection;
    QString m_bootDeviceName;
};
}
#endif
