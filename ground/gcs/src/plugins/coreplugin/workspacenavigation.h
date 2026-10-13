/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QWidget>
#include <QMap>

class QPushButton;
class QTabBar;
class QLabel;
class QMenu;
class QMenuBar;

namespace Core {
class IMode;
class ModeManager;
namespace Internal {

class WorkspaceNavigation final : public QWidget
{
    Q_OBJECT
public:
    explicit WorkspaceNavigation(ModeManager *manager, QWidget *parent = nullptr);
    ~WorkspaceNavigation() override;
    QTabBar *systemTools() const { return m_systemTools; }
    void setAdvancedMenuBar(QMenuBar *menuBar);

signals:
    void exitRequested();

private:
    void setExpanded(bool expanded);
    QPushButton *addButton(const QString &label, const QString &icon);
    void refreshModes();
    void selectMode(IMode *mode);
    ModeManager *m_manager;
    QMap<QString, QPushButton *> m_buttons;
    QTabBar *m_systemTools;
    QLabel *m_brand;
    QPushButton *m_toggle, *m_advanced, *m_exit;
    QMenu *m_advancedMenu;
};
}
}
