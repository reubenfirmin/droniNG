/**
 ******************************************************************************
 *
 * @file       mixercurvepoint.h
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 *
 * @addtogroup GCSPlugins GCS Plugins
 * @{
 * @addtogroup UAVObjectWidgetUtils Plugin
 * @{
 * @brief Utility plugin for UAVObject to Widget relation management
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

#include <QGraphicsScene>
#include <QApplication>
#include <QGraphicsSceneMouseEvent>
#include <QPainter>
#include <QStyleOption>
#include <QColor>
#include <QDebug>
#include <utils/darktheme.h>

#include "mixercurveline.h"
#include "mixercurvepoint.h"
#include "mixercurvewidget.h"

MixerNode::MixerNode(MixerCurveWidget *graphWidget)
    : graph(graphWidget)
{
    setFlag(ItemIsMovable);
    setFlag(ItemSendsGeometryChanges);
    setCacheMode(DeviceCoordinateCache);
    setZValue(1);
    cmdActive = false;
    vertical = false;
    cmdNode = false;
    cmdToggle = true;
    drawNode = true;
    drawText = true;

    posColor0 = "#1c870b"; // greenish?
    posColor1 = "#116703"; // greenish?
    negColor0 = "#aa0000"; // red
    negColor1 = "#aa0000"; // red
}

void MixerNode::addEdge(Edge *edge)
{
    edgeList << edge;
    edge->adjust();
}

QList<Edge *> MixerNode::edges() const
{
    return edgeList;
}

QRectF MixerNode::boundingRect() const
{
    return cmdNode ? QRectF(-4, -4, 15, 10) : QRectF(-21, -13, 42, 26);
}

QPainterPath MixerNode::shape() const
{
    QPainterPath path;
    path.addRoundedRect(boundingRect(), cmdNode ? 2 : 4, cmdNode ? 2 : 4);
    return path;
}

void MixerNode::paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *)
{
    QString text = cmdNode ? cmdText : QString::asprintf("%.2f", value());
    QFont font = graph->font();
    font.setPixelSize(cmdNode ? 9 : 12);
    painter->setFont(font);
    const QPalette palette = QApplication::palette();
    QColor color = cmdNode ? (cmdActive ? posColor0 : negColor0) : (value() < 0 ? negColor0 : posColor0);
    if (color.saturationF() < 0.18)
        color = palette.color(QPalette::Mid);
    else if (color.hueF() < 0.07 || color.hueF() > 0.9)
        color = Utils::errorColor();
    else if (color.hueF() < 0.2)
        color = Utils::warningColor();
    else
        color = palette.color(QPalette::Highlight);
    const bool active = (option->state & QStyle::State_Sunken) || (cmdNode && cmdActive);
    if (drawNode) {
        painter->setBrush(active ? color : palette.color(QPalette::Base));
        QPen border(color, 1);
        border.setCosmetic(true);
        painter->setPen(border);
        painter->drawRoundedRect(boundingRect(), cmdNode ? 2 : 4, cmdNode ? 2 : 4);

        if (!image.isNull())
            painter->drawImage(boundingRect().adjusted(1, 1, -1, -1), image);
    }

    if (drawText) {
        painter->setPen(active && drawNode ? palette.color(QPalette::HighlightedText) : palette.color(QPalette::Text));
        if (cmdNode) {
            painter->drawText(0, 4, text);
        } else {
            painter->drawText(boundingRect(), Qt::AlignCenter, text);
        }
    }
}

void MixerNode::verticalMove(bool flag)
{
    vertical = flag;
}

void MixerNode::commandNode(bool enable)
{
    cmdNode = enable;
}
void MixerNode::commandText(QString text)
{
    cmdText = text;
}

double MixerNode::value()
{
    double h = graph->sceneRect().height();
    double ratio = (h - pos().y()) / h;
    return ((graph->getMax() - graph->getMin()) * ratio) + graph->getMin();
}

QVariant MixerNode::itemChange(GraphicsItemChange change, const QVariant &val)
{
    QPointF newPos = val.toPointF();
    double h = graph->sceneRect().height();

    switch (change) {
    case ItemPositionChange: {

        if (!vertical)
            break;

        // Force node to move vertically
        newPos.setX(pos().x());

        // Stay inside graph
        if (newPos.y() < 0)
            newPos.setY(0);
        // qDebug() << h << " - " << newPos.y();
        if (newPos.y() > h)
            newPos.setY(h);

        return newPos;
    }
    case ItemPositionHasChanged: {
        foreach (Edge *edge, edgeList)
            edge->adjust();

        update();

        graph->itemMoved(value());
        break;
    }
    default:
        break;
    };

    return QGraphicsItem::itemChange(change, val);
}

void MixerNode::mousePressEvent(QGraphicsSceneMouseEvent *event)
{
    if (cmdNode) {
        graph->cmdActivated(this);
        // return;
    }
    update();
    QGraphicsItem::mousePressEvent(event);
}

void MixerNode::mouseReleaseEvent(QGraphicsSceneMouseEvent *event)
{
    update();
    QGraphicsItem::mouseReleaseEvent(event);
}
