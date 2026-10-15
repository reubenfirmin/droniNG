/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mapstartup.h"
#include <QDateTime>
#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <cmath>

class LocationReply final : public QNetworkReply
{
public:
    LocationReply(const QNetworkRequest &request, QObject *parent) : QNetworkReply(parent)
    {
        setRequest(request); setUrl(request.url()); open(QIODevice::ReadOnly);
    }
    void finish(const QByteArray &body, bool failed = false)
    {
        m_body = body;
        if (failed) setError(QNetworkReply::TimeoutError, "Location request timed out");
        setFinished(true); emit readyRead(); emit finished();
    }
    void abort() override {}
    qint64 bytesAvailable() const override { return m_body.size() + QIODevice::bytesAvailable(); }
protected:
    qint64 readData(char *buffer, qint64 maximum) override
    {
        const auto size = qMin(maximum, qint64(m_body.size()));
        if (!size) return -1;
        memcpy(buffer, m_body.constData(), size); m_body.remove(0, size); return size;
    }
private:
    QByteArray m_body;
};

class LocationNetwork final : public QNetworkAccessManager
{
public:
    LocationReply *reply = nullptr;
    int requests = 0;
protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override
    {
        ++requests; reply = new LocationReply(request, this); return reply;
    }
};

class MapStartupTests final : public QObject
{
    Q_OBJECT
private slots:
    void fitsThirtyKilometresAtDifferentSizesAndLatitudes()
    {
        for (const auto &size : {QSize(1000, 600), QSize(600, 1000), QSize(400, 280), QSize(1800, 1000)}) {
            for (const double latitude : {0.0, 9.5, 65.0, -80.0}) {
                const double zoom = MapStartup::regionZoom(latitude, size);
                const double integral = std::floor(zoom);
                const double resolution = 40075016.686 * std::cos(latitude * std::acos(-1.0) / 180)
                    / (256 * std::pow(2.0, integral) * (1 + zoom - integral));
                const double coverage = qMin(size.width(), size.height()) * resolution;
                QVERIFY(std::abs(coverage - 30000) < 0.1);
            }
        }
    }
    void cachedLocationStartsImmediatelyWithoutNetwork()
    {
        QTemporaryDir directory;
        QSettings settings(directory.filePath("map.ini"), QSettings::IniFormat);
        settings.setValue("Map/ApproximateLongitude", -83.4); settings.setValue("Map/ApproximateLatitude", 9.2);
        settings.setValue("Map/ApproximateLocationUpdated", QDateTime::currentSecsSinceEpoch());
        LocationNetwork network;
        MapStartup startup(&settings, nullptr, &network);
        QSignalSpy centers(&startup, &MapStartup::centerRequested);
        startup.start({0, 0});
        QCOMPARE(centers.size(), 1); QCOMPARE(centers.first().first().toPointF(), QPointF(-83.4, 9.2));
        QCOMPARE(network.requests, 0);
    }
    void slowLocationRequestKeepsTheEventLoopResponsive()
    {
        QTemporaryDir directory;
        QSettings settings(directory.filePath("map.ini"), QSettings::IniFormat);
        LocationNetwork network;
        MapStartup startup(&settings, nullptr, &network);
        QSignalSpy centers(&startup, &MapStartup::centerRequested);
        QElapsedTimer time; time.start(); startup.start({-83, 9});
        QVERIFY(time.elapsed() < 100);
        bool ticked = false;
        QTimer::singleShot(0, &startup, [&ticked] { ticked = true; });
        QTRY_VERIFY(ticked);
        QCOMPARE(centers.size(), 1);
        network.reply->finish(R"({"latitude":9.2,"longitude":-83.4})");
        QCOMPARE(centers.size(), 2);
        QCOMPARE(centers.last().first().toPointF(), QPointF(-83.4, 9.2));
    }
    void gpsWinsOverALateApproximateLocation()
    {
        QTemporaryDir directory;
        QSettings settings(directory.filePath("map.ini"), QSettings::IniFormat);
        LocationNetwork network;
        MapStartup startup(&settings, nullptr, &network);
        QSignalSpy centers(&startup, &MapStartup::centerRequested);
        startup.start({0, 0}); startup.setControllerLocation({-83.7, 9.5});
        network.reply->finish(R"({"latitude":9.2,"longitude":-83.4})");
        QCOMPARE(centers.size(), 2); QCOMPARE(centers.last().first().toPointF(), QPointF(-83.7, 9.5));
        startup.start({0, 0}); QCOMPARE(network.requests, 1);
    }
    void lateLocationDoesNotInterruptManualNavigation()
    {
        QTemporaryDir directory;
        QSettings settings(directory.filePath("map.ini"), QSettings::IniFormat);
        LocationNetwork network;
        MapStartup startup(&settings, nullptr, &network);
        QSignalSpy centers(&startup, &MapStartup::centerRequested);
        startup.start({0, 0}); startup.setUserInteracted();
        network.reply->finish(R"({"latitude":9.2,"longitude":-83.4})");
        startup.setControllerLocation({-83.7, 9.5});
        QCOMPARE(centers.size(), 1); QVERIFY(!startup.automaticView());
        startup.start({0, 0}); QVERIFY(startup.automaticView());
        QCOMPARE(centers.last().first().toPointF(), QPointF(-83.7, 9.5));
    }
    void offlineAndMalformedLocationKeepTheSavedMapUsable()
    {
        for (const auto &response : {QByteArray(), QByteArray("{}"), QByteArray(R"({"latitude":91,"longitude":0})")}) {
            QTemporaryDir directory;
            QSettings settings(directory.filePath("map.ini"), QSettings::IniFormat);
            LocationNetwork network;
            MapStartup startup(&settings, nullptr, &network);
            QSignalSpy centers(&startup, &MapStartup::centerRequested);
            QSignalSpy statuses(&startup, &MapStartup::statusChanged);
            startup.start({-83, 9}); network.reply->finish(response, response.isEmpty());
            QCOMPARE(centers.size(), 1); QCOMPARE(centers.first().first().toPointF(), QPointF(-83, 9));
            QVERIFY(statuses.last().first().toString().contains("Location unavailable"));
            QVERIFY(!settings.contains("Map/ApproximateLocationUpdated"));
        }
    }
};

QTEST_GUILESS_MAIN(MapStartupTests)
#include "map_startup.moc"
