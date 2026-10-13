/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mapstartup.h"
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <cmath>

MapStartup::MapStartup(QSettings *settings, QObject *parent, QNetworkAccessManager *network)
    : QObject(parent), m_settings(settings),
      m_network(network ? network : new QNetworkAccessManager(this))
{}

bool MapStartup::validLocation(const QPointF &location)
{
    return std::isfinite(location.x()) && std::isfinite(location.y())
        && std::abs(location.x()) <= 180 && std::abs(location.y()) <= 85.05112878;
}

double MapStartup::regionZoom(double latitude, const QSize &viewport)
{
    // Fit the entire 30 km square. TLMap's fractional zoom scales by 1 + fraction.
    const double pixels = qMax(1, qMin(viewport.width(), viewport.height()));
    const double equatorialResolution = 40075016.686 * std::cos(latitude * std::acos(-1.0) / 180.0) / 256;
    const double level = std::log2(equatorialResolution * pixels / 30000);
    const double integral = std::floor(level);
    const double scale = std::pow(2.0, level - integral);
    return qBound(2.0, integral + scale - 1, 17.0);
}

void MapStartup::setControllerLocation(const QPointF &location)
{
    if (!validLocation(location)) return;
    const bool firstFix = !m_hasControllerLocation;
    m_controllerLocation = location;
    m_hasControllerLocation = true;
    if (firstFix && !m_userInteracted) {
        emit centerRequested(location);
        emit statusChanged(tr("Controller GPS · 30 × 30 km"));
    }
}

void MapStartup::start(const QPointF &savedLocation)
{
    m_userInteracted = false;
    if (m_hasControllerLocation) {
        emit centerRequested(m_controllerLocation);
        emit statusChanged(tr("Controller GPS · 30 × 30 km"));
        return;
    }
    const QPointF cached(m_settings->value("Map/ApproximateLongitude").toDouble(),
                         m_settings->value("Map/ApproximateLatitude").toDouble());
    const qint64 updated = m_settings->value("Map/ApproximateLocationUpdated").toLongLong();
    const bool hasCache = updated > 0 && validLocation(cached);
    emit centerRequested(hasCache ? cached : validLocation(savedLocation) ? savedLocation : QPointF());
    emit statusChanged(hasCache ? tr("Approximate location · 30 × 30 km") : tr("Finding current location…"));
    if ((hasCache && QDateTime::currentSecsSinceEpoch() - updated < 3600) || m_requestPending) return;

    // Resolve only the computer's approximate location. Controller coordinates stay local.
    QNetworkRequest request(QUrl(QStringLiteral("https://ipapi.co/json/")));
    request.setRawHeader("User-Agent", "DroniNG-GCS/1.0");
    request.setTransferTimeout(4000);
    m_requestPending = true;
    auto *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, hasCache] {
        m_requestPending = false;
        const auto document = QJsonDocument::fromJson(reply->readAll()).object();
        const QPointF location(document.value("longitude").toDouble(), document.value("latitude").toDouble());
        const bool valid = reply->error() == QNetworkReply::NoError && !document.value("error").toBool()
            && document.value("latitude").isDouble() && document.value("longitude").isDouble() && validLocation(location);
        reply->deleteLater();
        if (valid) {
            m_settings->setValue("Map/ApproximateLongitude", location.x());
            m_settings->setValue("Map/ApproximateLatitude", location.y());
            m_settings->setValue("Map/ApproximateLocationUpdated", QDateTime::currentSecsSinceEpoch());
        }
        if (m_hasControllerLocation || m_userInteracted) return;
        if (valid) {
            emit centerRequested(location);
            emit statusChanged(tr("Approximate location · 30 × 30 km"));
        } else if (!hasCache) {
            emit statusChanged(tr("Location unavailable. Pan the map or connect controller GPS."));
        }
    });
}
