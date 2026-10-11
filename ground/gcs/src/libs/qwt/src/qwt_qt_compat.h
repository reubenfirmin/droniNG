#ifndef QWT_QT_COMPAT_H
#define QWT_QT_COMPAT_H

#include <QApplication>
#include <QWheelEvent>

inline QSize qwtGlobalStrut()
{
    return QSize(0, 0);
}

inline int qwtWheelDelta(const QWheelEvent *event)
{
    const QPoint delta = event->angleDelta();
    return delta.y() ? delta.y() : delta.x();
}

inline QPoint qwtWheelPosition(const QWheelEvent *event)
{
    return event->position().toPoint();
}

#endif
