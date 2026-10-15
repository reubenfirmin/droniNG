/**
 ******************************************************************************
 * @file       droningplugin.cpp
 * @author     dRonin, http://dRonin.org/, Copyright (C) 2016
 * @addtogroup GCSPlugins GCS Plugins
 * @{
 * @addtogroup Boards_droniNG droniNG board support plugin
 * @{
 * @brief Supports droniNG board configuration
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
 *
 * Additional note on redistribution: The copyright and license notices above
 * must be maintained in each individual source file that is a derivative work
 * of this source file; otherwise redistribution is prohibited.
 */

#include "droningplugin.h"
#include "simulation.h"
#include "omnibusf3.h"
#include "pikoblx.h"
#include "playuavosd.h"
#include "seppuku.h"
#include "sprf3e.h"
#include "importedboard.h"
#include <QtPlugin>

DroningPlugin::DroningPlugin()
{
}

DroningPlugin::~DroningPlugin()
{
}

bool DroningPlugin::initialize(const QStringList &arguments, QString *errorString)
{
    Q_UNUSED(arguments);
    Q_UNUSED(errorString);
    return true;
}

void DroningPlugin::extensionsInitialized()
{
    for (const auto &id : {"radix", "omnibusf4sd"}) {
        const auto profile = Core::FirmwareCatalog::instance().board(id);
        if (profile.isValid()) addAutoReleasedObject(new ImportedBoard(profile));
    }
    // Init boards
    Simulation *sim = new Simulation();
    addAutoReleasedObject(sim);

    OmnibusF3 *omnif3 = new OmnibusF3();
    addAutoReleasedObject(omnif3);

    PikoBLX *pikoblx = new PikoBLX();
    addAutoReleasedObject(pikoblx);

    PlayUavOsd *playuav = new PlayUavOsd();
    addAutoReleasedObject(playuav);

    Seppuku *seppuku = new Seppuku();
    addAutoReleasedObject(seppuku);

    Sprf3e *sprf3e = new Sprf3e();
    addAutoReleasedObject(sprf3e);
}

void DroningPlugin::shutdown()
{
}

/**
 * @}
 * @}
 */
