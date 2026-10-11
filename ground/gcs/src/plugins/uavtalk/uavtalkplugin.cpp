/**
 ******************************************************************************
 *
 * @file       uavtalkplugin.cpp
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @addtogroup GCSPlugins GCS Plugins
 * @{
 * @addtogroup UAVTalkPlugin UAVTalk Plugin
 * @{
 * @brief The UAVTalk protocol plugin
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>
 */
#include "uavtalkplugin.h"
#include "boardsettingsinstaller.h"

#include <coreplugin/icore.h>
#include <coreplugin/connectionmanager.h>

UAVTalkPlugin::UAVTalkPlugin()
{
}

UAVTalkPlugin::~UAVTalkPlugin()
{
}
/**
  * Called once all the plugins which depend on us have been loaded
  */
void UAVTalkPlugin::extensionsInitialized()
{
}

/**
  * Called at startup, before any plugin which depends on us is initialized
  */
bool UAVTalkPlugin::initialize(const QStringList &arguments, QString *errorString)
{
    // Done
    Q_UNUSED(arguments);
    Q_UNUSED(errorString);
    // Get UAVObjectManager instance
    ExtensionSystem::PluginManager *pm = ExtensionSystem::PluginManager::instance();
    objMngr = pm->getObject<UAVObjectManager>();

    // Create TelemetryManager
    telMngr = new TelemetryManager();
    addAutoReleasedObject(telMngr);

    // Connect to connection manager so we get notified when the user connect to his device
    Core::ConnectionManager *cm = Core::ICore::instance()->connectionManager();
    auto *settings = new BoardSettingsInstaller(objMngr, this);
    connect(cm, &Core::ConnectionManager::boardSettingsRequested, settings, &BoardSettingsInstaller::start);
    connect(cm, &Core::ConnectionManager::deviceDisconnected, settings, &BoardSettingsInstaller::disconnected);
    connect(cm, &Core::ConnectionManager::boardSettingsCancelled, settings, &BoardSettingsInstaller::cancel);
    connect(settings, &BoardSettingsInstaller::finished, cm, &Core::ConnectionManager::boardSettingsFinished);
    QObject::connect(cm, &Core::ConnectionManager::deviceConnected, this,
                     &UAVTalkPlugin::onDeviceConnect);
    QObject::connect(cm, &Core::ConnectionManager::deviceAboutToDisconnect, this,
                     &UAVTalkPlugin::onDeviceDisconnect);
    return true;
}

void UAVTalkPlugin::shutdown()
{
}

void UAVTalkPlugin::onDeviceConnect(QIODevice *dev)
{
    telMngr->start(dev);
}

void UAVTalkPlugin::onDeviceDisconnect()
{
    telMngr->stop();
}
