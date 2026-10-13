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

#ifndef PFDQMLGADGET_H_
#define PFDQMLGADGET_H_

#include <coreplugin/iuavgadget.h>
#include "pfdqmlgadgetwidget.h"

class IUAVGadget;
class QWidget;
class QString;
class PfdQmlGadgetWidget;

using namespace Core;

class PfdQmlGadget : public Core::IUAVGadget
{
    Q_OBJECT
public:
    PfdQmlGadget(QString classId, PfdQmlGadgetWidget *widget, QWidget *parent = nullptr);
    ~PfdQmlGadget();

    QWidget *widget()
    {
        return m_widget;
    }

    void loadConfiguration(IUAVGadgetConfiguration *config);

private:
    PfdQmlGadgetWidget *m_widget;
};

#endif // PFDQMLQMLGADGET_H_
