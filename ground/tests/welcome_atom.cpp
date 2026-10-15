/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "atomfeedmodel.h"
#include <utils/darktheme.h>
#include <QApplication>
#include <QFile>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <memory>

static const QByteArray feed = QString::fromUtf8(
    "<feed xmlns='http://www.w3.org/2005/Atom' xmlns:media='http://search.yahoo.com/mrss/'>"
    "<entry><title>café 日本語 &amp; model</title>"
    "<author><name>Zoë</name><uri>https://example.invalid/zoe</uri></author>"
    "<link rel='self' href='ignored.atom'/><link rel='alternate' href='commit/123'/>"
    "<media:thumbnail url='https://example.invalid/avatar.png'/></entry>"
    "<entry><title>Merge pull request #12</title></entry>"
    "<entry><title>Camera update</title><link href='https://example.invalid/456'/></entry>"
    "</feed>").toUtf8();

class WelcomeAtomTests : public QObject
{
    Q_OBJECT
    static QUrl write(const QTemporaryDir &directory, const QString &name, const QByteArray &bytes)
    {
        QFile file(directory.filePath(name));
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
            return {};
        return QUrl::fromLocalFile(file.fileName());
    }
private slots:
    void initTestCase()
    {
        Utils::applyDarkTheme();
        qmlRegisterType<Welcome::AtomFeedModel>("DroniNG.Welcome", 1, 0, "AtomFeedModel");
    }
    void atomRolesAndFiltering_data()
    {
        QTest::addColumn<QByteArray>("xml");
        QTest::newRow("default-namespace") << feed;
        QTest::newRow("prefixed-namespace") << QByteArray(
            "<a:feed xmlns:a='http://www.w3.org/2005/Atom'>"
            "<a:entry><a:title>Camera update</a:title><a:link href='https://example.invalid/456'/></a:entry>"
            "</a:feed>");
    }
    void atomRolesAndFiltering()
    {
        QFETCH(QByteArray, xml);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QUrl source = write(directory, "feed.atom", xml);
        QVERIFY(!source.isEmpty());
        Welcome::AtomFeedModel model;
        model.setSource(source);
        QTRY_COMPARE(model.rowCount(), xml == feed ? 2 : 1);
        QVERIFY(model.errorString().isEmpty());
        QCOMPARE(model.roleNames().value(Welcome::AtomFeedModel::AuthorUriRole), QByteArray("authoruri"));
        if (xml == feed) {
            QCOMPARE(model.data(model.index(0), Welcome::AtomFeedModel::TitleRole).toString(), QString::fromUtf8("café 日本語 & model"));
            QCOMPARE(model.data(model.index(0), Welcome::AtomFeedModel::AuthorRole).toString(), QString::fromUtf8("Zoë"));
            QCOMPARE(model.data(model.index(0), Welcome::AtomFeedModel::AuthorUriRole).toString(), QString("https://example.invalid/zoe"));
            QCOMPARE(model.data(model.index(0), Welcome::AtomFeedModel::LinkRole).toString(), source.resolved(QUrl("commit/123")).toString());
            QCOMPARE(model.data(model.index(0), Welcome::AtomFeedModel::ThumbnailRole).toString(), QString("https://example.invalid/avatar.png"));
        }
        QCOMPARE(model.data(model.index(model.rowCount() - 1), Welcome::AtomFeedModel::TitleRole).toString(), QString("Camera update"));
        QVERIFY(!model.data(QModelIndex(), Welcome::AtomFeedModel::TitleRole).isValid());
        QCOMPARE(model.rowCount(model.index(0)), 0);
    }
    void malformedResponseAndRecovery()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Welcome::AtomFeedModel model;
        for (const QByteArray &invalid : {QByteArray("<html>Service unavailable</html>"), feed + "<broken", feed.left(feed.size() - 10)}) {
            const QUrl source = write(directory, QString::number(invalid.size()) + ".xml", invalid);
            model.setSource(source);
            QTRY_VERIFY(!model.errorString().isEmpty());
            QCOMPARE(model.rowCount(), 0);
        }
        model.setSource(write(directory, "valid.atom", feed));
        QTRY_COMPARE(model.rowCount(), 2);
        QVERIFY(model.errorString().isEmpty());
        model.setSource({});
        QCOMPARE(model.rowCount(), 0);
        QVERIFY(model.errorString().isEmpty());
    }
    void httpFailureAndSourceReplacement()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        int heldRequests = 0;
        connect(&server, &QTcpServer::newConnection, this, [&]() {
            QTcpSocket *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, this, [&, socket]() {
                const QByteArray request = socket->readAll();
                if (!request.contains("\r\n\r\n"))
                    return;
                if (request.startsWith("GET /held ")) {
                    ++heldRequests;
                    return;
                }
                const bool failed = request.startsWith("GET /failed ");
                // A valid Atom body under an error status must still fail.
                socket->write(QByteArray(failed ? "HTTP/1.1 503 Unavailable\r\n" : "HTTP/1.1 200 OK\r\n")
                              + "Content-Length: " + QByteArray::number(feed.size())
                              + "\r\nConnection: close\r\n\r\n" + feed);
                socket->disconnectFromHost();
            });
        });
        const QUrl base(QString("http://127.0.0.1:%1/").arg(server.serverPort()));
        Welcome::AtomFeedModel model;
        model.setSource(base.resolved(QUrl("failed")));
        QTRY_VERIFY(!model.errorString().isEmpty());
        QCOMPARE(model.rowCount(), 0);
        model.setSource(base.resolved(QUrl("held")));
        QTRY_COMPARE(heldRequests, 1);
        model.setSource(base.resolved(QUrl("valid")));
        QTRY_COMPARE(model.rowCount(), 2);
        QVERIFY(model.errorString().isEmpty());
        QCOMPARE(model.data(model.index(0), Welcome::AtomFeedModel::LinkRole).toString(), base.resolved(QUrl("commit/123")).toString());
        QTest::qWait(50);
        QCOMPARE(model.rowCount(), 2);
        QVERIFY(model.errorString().isEmpty());
    }
    void shippedNewsPanel()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QUrl source = write(directory, "feed.atom", feed);
        QQmlEngine engine;
        int warnings = 0;
        connect(&engine, &QQmlEngine::warnings, this,
                [&](const QList<QQmlError> &errors) { warnings += errors.size(); });
        QQmlComponent component(&engine, QUrl::fromLocalFile(QString::fromUtf8(NEWS_PANEL_FILE)));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> panel(component.createWithInitialProperties({{"feedSource", source}}));
        QVERIFY(panel);
        auto model = panel->findChild<Welcome::AtomFeedModel *>("newsFeed");
        QVERIFY(model);
        QTRY_COMPARE(model->rowCount(), 2);
        QCOMPARE(warnings, 0);
    }

    void panelHover_data()
    {
        QTest::addColumn<QString>("fileName");
        QTest::addColumn<int>("rows");
        QTest::addColumn<int>("textsPerRow");
        QTest::newRow("sites") << QString("SitesPanel.qml") << 4 << 1;
        QTest::newRow("news") << QString("NewsPanel.qml") << 1 << 2;
        QTest::newRow("autotune") << QString("AutoTunePanel.qml") << 1 << 3;
    }

    void panelHover()
    {
        QFETCH(QString, fileName);
        QFETCH(int, rows);
        QFETCH(int, textsPerRow);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QUrl atomSource = write(directory, "hover.atom", QByteArray(
            "<feed xmlns='http://www.w3.org/2005/Atom'><entry><title>Hover test</title>"
            "<author><name>Developer</name></author></entry></feed>"));
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        connect(&server, &QTcpServer::newConnection, this, [&]() {
            QTcpSocket *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, socket, [socket]() {
                if (!socket->readAll().contains("\r\n\r\n"))
                    return;
                const QByteArray json = "[{\"Board\":\"CC3D\",\"Key\":\"test\",\"Tau\":0.01,"
                    "\"Timestamp\":\"2026-01-01T00:00:00Z\",\"City\":\"london\",\"Country\":\"UK\"}]";
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                              + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n" + json);
                socket->disconnectFromHost();
            });
        });
        QQmlEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this, [&](const QList<QQmlError> &errors) {
            for (const QQmlError &error : errors)
                warnings.append(error.toString());
        });
        const QString panelDir = qEnvironmentVariable("GCS_WELCOME_QML_DIR", WELCOME_PANEL_DIR);
        QQmlComponent component(&engine, QUrl::fromLocalFile(panelDir + "/" + fileName));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QVariantMap properties;
        if (fileName == "NewsPanel.qml")
            properties.insert("feedSource", atomSource);
        else if (fileName == "AutoTunePanel.qml")
            properties.insert("feedSource", QUrl(QString("http://127.0.0.1:%1/").arg(server.serverPort())));
        QQuickWindow window;
        window.resize(500, 400);
        std::unique_ptr<QQuickItem> panel(qobject_cast<QQuickItem *>(component.createWithInitialProperties(properties)));
        QVERIFY(panel);
        panel->setParentItem(window.contentItem());
        panel->setSize(QSizeF(480, 380));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTest::mouseMove(&window, QPoint(490, 390));
        QTRY_COMPARE(itemsWithProperty(panel.get(), "hoverEnabled", true).size(), rows);
        const auto areas = itemsWithProperty(panel.get(), "hoverEnabled", true);
        for (QQuickItem *area : areas) {
            const auto texts = itemsWithProperty(area->parentItem(), "color");
            QCOMPARE(texts.size(), textsPerRow);
            const QPoint inside = area->mapToScene(QPointF(area->width() / 2, area->height() - 2)).toPoint();
            QTest::mouseMove(&window, inside);
            QTRY_VERIFY(area->property("containsMouse").toBool());
            QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
            for (QQuickItem *text : texts)
                QCOMPARE(text->property("color").value<QColor>(), qApp->palette().color(QPalette::Active, QPalette::Highlight));
            QTest::mouseMove(&window, QPoint(490, 390));
            QTRY_VERIFY(!area->property("containsMouse").toBool());
            for (QQuickItem *text : texts)
                QCOMPARE(text->property("color").value<QColor>(), qApp->palette().color(QPalette::Active, QPalette::Text));
        }
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
    }

private:
    static QList<QQuickItem *> itemsWithProperty(QQuickItem *root, const char *name,
                                               const QVariant &value = {})
    {
        QList<QQuickItem *> items;
        const QVariant property = root->property(name);
        if (property.isValid() && (!value.isValid() || property == value))
            items.append(root);
        for (QQuickItem *child : root->childItems())
            items.append(itemsWithProperty(child, name, value));
        return items;
    }
};

QTEST_MAIN(WelcomeAtomTests)
#include "welcome_atom.moc"
