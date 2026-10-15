/**
 ******************************************************************************
 * @file       configradiowidget.cpp
 * @brief      Configure the integrated radio
 * @author     dRonin, http://dRonin.org/, Copyright (C) 2018
 * @addtogroup GCSPlugins GCS Plugins
 * @{
 * @addtogroup ConfigPlugin Config Plugin
 * @{
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

#include "configradiowidget.h"

#include <extensionsystem/pluginmanager.h>
#include <coreplugin/generalsettings.h>

#include "openlrs.h"

#include "ui_integratedradio.h"

ConfigRadioWidget::ConfigRadioWidget(QWidget *parent)
    : ConfigTaskWidget(parent)
{
    ui = new Ui::radio();
    ui->setupUi(this);
    // Keep role and telemetry choices together instead of separating them with
    // a flexible spacer across the whole page.
    while (auto *item = ui->gridLayout_3->takeAt(0))
        if (!item->layout()) delete item;
    ui->gridLayout_3->addWidget(ui->label_6, 0, 0);
    ui->gridLayout_3->addLayout(ui->horizontalLayout_3, 0, 1, 1, 3);
    ui->gridLayout_3->addWidget(ui->label_11, 1, 0);
    ui->gridLayout_3->addWidget(ui->cb_tel_source, 1, 1);
    ui->gridLayout_3->addWidget(ui->label_10, 2, 0);
    ui->gridLayout_3->addWidget(ui->cb_rssi_source, 2, 1);
    ui->gridLayout_3->setColumnMinimumWidth(0, 208);
    ui->gridLayout_3->setProperty("compactForm", true);

    for (auto *grid : {ui->gridLayout_9, ui->gridLayout_4, ui->gridLayout}) {
        struct Cell { QWidget *widget; int row, column, rows, columns; };
        QList<Cell> cells;
        for (int index = grid->count() - 1; index >= 0; --index) {
            auto *item = grid->itemAt(index);
            if (item->spacerItem()) { delete grid->takeAt(index); continue; }
            if (!item->widget()) continue;
            int row, column, rows, columns;
            grid->getItemPosition(index, &row, &column, &rows, &columns);
            cells.append({item->widget(), row, column, rows, columns});
            delete grid->takeAt(index);
        }
        for (const auto &cell : cells) {
            const int column = grid == ui->gridLayout ? (cell.column < 4 ? cell.column - 2 : cell.column - 3)
                : cell.column < 2 ? cell.column : cell.column - 1;
            grid->addWidget(cell.widget, cell.row, column, cell.rows, cell.columns);
            if (auto *label = qobject_cast<QLabel *>(cell.widget))
                label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        }
        grid->setColumnMinimumWidth(0, 208);
        grid->setProperty("compactForm", true);
    }
    for (auto *row : {ui->horizontalLayout_2, ui->horizontalLayout_5}) {
        for (int index = 0; index < row->count(); ++index)
            if (auto *label = row->itemAt(index)->widget()) label->setFixedWidth(36);
        row->addStretch();
    }

    // Load UAVObjects to widget relations from UI file
    // using objrelation dynamic property
    autoLoadWidgets();

    // Refresh widget contents
    ConfigTaskWidget::refreshWidgetsValues();

    // Prevent mouse wheel from changing values
    disableMouseWheelEvents();

    connect(ui->rb_tx, &QRadioButton::toggled, this,
            &ConfigRadioWidget::roleChanged);
    connect(ui->rb_rx, &QRadioButton::toggled, this,
            &ConfigRadioWidget::roleChanged);
    connect(ui->rb_disabled, &QRadioButton::toggled, this,
            &ConfigRadioWidget::roleChanged);
}

ConfigRadioWidget::~ConfigRadioWidget()
{
}

void ConfigRadioWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
}

void ConfigRadioWidget::enableControls(bool enable)
{
    Q_UNUSED(enable);
}

void ConfigRadioWidget::roleChanged(bool ignored)
{
    (void) ignored;

    if (ui->rb_tx->isChecked()) {
        ui->groupTxSettings->setEnabled(true);
        ui->groupRxSettings->setEnabled(false);
    } else if (ui->rb_rx->isChecked()) {
        ui->groupTxSettings->setEnabled(false);
        ui->groupRxSettings->setEnabled(true);
    } else {
        ui->groupTxSettings->setEnabled(false);
        ui->groupRxSettings->setEnabled(false);
    }
}

void ConfigRadioWidget::refreshWidgetsValues(UAVObject *obj)
{
    (void) obj;

    OpenLRS *olrsObj = OpenLRS::GetInstance(getObjectManager());

    OpenLRS::DataFields openlrs = olrsObj->getData();

    switch (openlrs.role) {
    case OpenLRS::ROLE_DISABLED:
    default:
        ui->rb_disabled->setChecked(true);
        break;
    case OpenLRS::ROLE_RX:
        ui->rb_rx->setChecked(true);
        break;
    case OpenLRS::ROLE_TX:
        ui->rb_tx->setChecked(true);
        break;
    }

    ConfigTaskWidget::refreshWidgetsValues();
}

void ConfigRadioWidget::updateObjectsFromWidgets()
{
    OpenLRS *olrsObj = OpenLRS::GetInstance(getObjectManager());

    if (ui->rb_tx->isChecked()) {
        olrsObj->setrole(OpenLRS::ROLE_TX);
    } else if (ui->rb_rx->isChecked()) {
        olrsObj->setrole(OpenLRS::ROLE_RX);
    } else {
        olrsObj->setrole(OpenLRS::ROLE_DISABLED);
    }

    ConfigTaskWidget::updateObjectsFromWidgets();
}

/**
 * @}
 * @}
 */
