/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QObject>
#include <QPointF>
#include <QSize>

class QNetworkAccessManager;
class QSettings;

class MapStartup final : public QObject
{
    Q_OBJECT
public:
    explicit MapStartup(QSettings *settings, QObject *parent = nullptr,
                        QNetworkAccessManager *network = nullptr);
    void start(const QPointF &savedLocation);
    void setControllerLocation(const QPointF &location);
    void clearControllerLocation() { m_hasControllerLocation = false; }
    void setUserInteracted() { m_userInteracted = true; }
    bool automaticView() const { return !m_userInteracted; }
    static bool validLocation(const QPointF &location);
    static double regionZoom(double latitude, const QSize &viewport);

signals:
    void centerRequested(const QPointF &location);
    void statusChanged(const QString &status);

private:
    QSettings *m_settings;
    QNetworkAccessManager *m_network;
    QPointF m_controllerLocation;
    bool m_hasControllerLocation = false;
    bool m_userInteracted = false;
    bool m_requestPending = false;
};
