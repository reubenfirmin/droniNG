/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "workspacenavigation.h"
#include "modemanager.h"
#include "imode.h"
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSvgRenderer>
#include <QTabBar>
#include <QStyle>
#include <QVBoxLayout>

using namespace Core;
using namespace Core::Internal;

static void populateAdvancedMenu(QMenu *target, const QList<QAction *> &actions)
{
    qDeleteAll(target->findChildren<QMenu *>(QString(), Qt::FindDirectChildrenOnly));
    target->clear();
    for (auto *source : actions) {
        if (!source->isVisible()) continue;
        if (source->isSeparator()) {
            target->addSeparator();
            continue;
        }
        if (auto *sourceMenu = source->menu()) {
            auto *submenu = target->addMenu(source->icon(), source->text());
            submenu->setEnabled(source->isEnabled());
            QObject::connect(submenu, &QMenu::aboutToShow, submenu, [submenu, sourceMenu] {
                QMetaObject::invokeMethod(sourceMenu, "aboutToShow", Qt::DirectConnection);
                populateAdvancedMenu(submenu, sourceMenu->actions());
            });
            continue;
        }
        auto *action = new QAction(target);
        const auto update = [action, source] {
            const QString shortcut = source->shortcut().toString(QKeySequence::NativeText);
            action->setText(source->text() + (shortcut.isEmpty() ? QString() : "\t" + shortcut));
            action->setIcon(source->icon());
            action->setEnabled(source->isEnabled());
            action->setVisible(source->isVisible());
            action->setCheckable(source->isCheckable());
            action->setChecked(source->isChecked());
            action->setToolTip(source->toolTip());
        };
        update();
        QObject::connect(source, &QAction::changed, action, update);
        QObject::connect(action, &QAction::triggered, source, &QAction::trigger);
        target->addAction(action);
    }
}

static QIcon navigationIcon(const QString &name, const QPalette &palette)
{
    QFile file(QString(":/core/images/navigation-%1.svg").arg(name));
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto source = file.readAll();
    QIcon icon;
    for (const auto state : {QIcon::Off, QIcon::On}) {
        const auto color = palette.color(state == QIcon::On ? QPalette::Highlight : QPalette::WindowText);
        QSvgRenderer renderer(QByteArray(source).replace("currentColor", color.name().toUtf8()));
        QPixmap pixmap(48, 48); pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap); renderer.render(&painter); painter.end();
        pixmap.setDevicePixelRatio(2);
        icon.addPixmap(pixmap, QIcon::Normal, state);
        icon.addPixmap(pixmap, QIcon::Active, state);
    }
    return icon;
}

WorkspaceNavigation::WorkspaceNavigation(ModeManager *manager, QWidget *parent)
    : QWidget(parent), m_manager(manager), m_systemTools(new QTabBar(parent))
{
    setObjectName("workspaceNavigation");
    setAttribute(Qt::WA_StyledBackground, true);
    setAccessibleName(tr("Main navigation"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 20, 10, 12); layout->setSpacing(6);
    m_brand = new QLabel(this);
    m_brand->setObjectName("navigationBrand");
    m_brand->setAccessibleName(tr("DroniNG"));
    m_brand->setFixedHeight(32);
    layout->addWidget(m_brand);
    m_toggle = addButton(tr("Expand sidebar"), "menu");
    connect(m_toggle, &QPushButton::clicked, this, [this] {
        setExpanded(!property("expanded").toBool());
    });
    layout->addSpacing(12);
    struct Entry { const char *mode; const char *label; const char *icon; };
    const Entry entries[] = {{"Welcome", QT_TR_NOOP("Welcome"), "home"},
        {"Map", QT_TR_NOOP("Map"), "map"},
        {"Configuration", QT_TR_NOOP("Configuration"), "configuration"},
        {"System", QT_TR_NOOP("System"), "system"},
        {"Firmware", QT_TR_NOOP("Firmware"), "firmware"}};
    for (const auto &entry : entries) {
        auto *button = addButton(tr(entry.label), entry.icon);
        button->setCheckable(true); button->setAutoExclusive(true);
        m_buttons.insert(entry.mode, button);
        connect(button, &QPushButton::clicked, this, [this, name = QString(entry.mode)] {
            m_manager->activateModeByWorkspaceName(name);
        });
    }
    layout->addStretch();
    m_advanced = addButton(tr("Advanced"), "advanced");
    m_advancedMenu = new QMenu;
    m_advancedMenu->setObjectName("advancedApplicationMenu");
    m_advanced->setMenu(m_advancedMenu);
    m_exit = addButton(tr("Exit"), "exit");
    connect(m_exit, &QPushButton::clicked, this, &WorkspaceNavigation::exitRequested);
    m_systemTools->setObjectName("systemWorkspaceNavigation");
    m_systemTools->setAccessibleName(tr("System tools"));
    m_systemTools->setExpanding(false); m_systemTools->setDrawBase(false);
    connect(m_systemTools, &QTabBar::currentChanged, this, [this](int index) {
        if (index >= 0) m_manager->activateMode(m_systemTools->tabData(index).toString());
    });
    connect(manager, &ModeManager::modesChanged, this, &WorkspaceNavigation::refreshModes);
    connect(manager, &ModeManager::currentModeChanged, this, &WorkspaceNavigation::selectMode);
    setExpanded(false);
    refreshModes();
}

WorkspaceNavigation::~WorkspaceNavigation()
{
    delete m_advancedMenu;
}

QPushButton *WorkspaceNavigation::addButton(const QString &label, const QString &icon)
{
    auto *button = new QPushButton(navigationIcon(icon, palette()), label, this);
    button->setObjectName(QString("navigation-%1").arg(icon));
    button->setAccessibleName(label);
    button->setToolTip(label);
    button->setProperty("navigationItem", true);
    button->setProperty("navigationLabel", label);
    button->setIconSize(QSize(20, 20));
    button->setFixedHeight(46);
    layout()->addWidget(button);
    return button;
}

void WorkspaceNavigation::setAdvancedMenuBar(QMenuBar *menuBar)
{
    menuBar->hide();
    connect(m_advancedMenu, &QMenu::aboutToShow, this, [this, menuBar] {
        // Give the popup its own accessible menu tree while retaining the registered commands.
        populateAdvancedMenu(m_advancedMenu, menuBar->actions());
    });
}

void WorkspaceNavigation::setExpanded(bool expanded)
{
    setProperty("expanded", expanded);
    setFixedWidth(expanded ? 208 : 68);
    m_brand->setText(expanded ? tr("DroniNG") : tr("D"));
    const QString toggleLabel = expanded ? tr("Collapse sidebar") : tr("Expand sidebar");
    m_toggle->setProperty("navigationLabel", toggleLabel);
    m_toggle->setAccessibleName(toggleLabel);
    m_toggle->setToolTip(toggleLabel);
    for (auto *button : findChildren<QPushButton *>(QString(), Qt::FindDirectChildrenOnly)) {
        button->setText(expanded ? button->property("navigationLabel").toString() : QString());
        button->setProperty("expanded", expanded);
        button->style()->unpolish(button); button->style()->polish(button);
    }
}

void WorkspaceNavigation::refreshModes()
{
    const QSignalBlocker blocker(m_systemTools);
    while (m_systemTools->count()) m_systemTools->removeTab(0);
    for (auto it = m_buttons.begin(); it != m_buttons.end(); ++it) {
        bool exists = false;
        for (auto *mode : m_manager->modes()) if (mode->name() == it.key()) exists = true;
        it.value()->setEnabled(exists);
    }
    for (auto *mode : m_manager->modes()) {
        if (mode->name() != "System" && m_buttons.contains(mode->name())) continue;
        const int index = m_systemTools->addTab(mode->name() == "System" ? tr("Overview") : mode->name());
        m_systemTools->setTabData(index, mode->uniqueModeName());
    }
    selectMode(m_manager->currentMode());
}

void WorkspaceNavigation::selectMode(IMode *mode)
{
    const QString name = mode ? mode->name() : QString();
    const QString section = m_buttons.contains(name) ? name : QString("System");
    for (auto it = m_buttons.begin(); it != m_buttons.end(); ++it) {
        it.value()->setAutoExclusive(false);
        it.value()->setChecked(mode && it.key() == section);
        it.value()->setAutoExclusive(true);
    }
    const QSignalBlocker blocker(m_systemTools);
    for (int i = 0; mode && i < m_systemTools->count(); ++i)
        if (m_systemTools->tabData(i).toString() == mode->uniqueModeName()) m_systemTools->setCurrentIndex(i);
    m_systemTools->setVisible(mode && section == "System" && m_systemTools->count() > 1);
}
