/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlError>
#include <QTest>
#include <memory>

class WelcomeJsonTests : public QObject
{
    Q_OBJECT
private slots:
    void malformedResponseAndRecovery()
    {
        QQmlEngine engine;
        int warnings = 0;
        connect(&engine, &QQmlEngine::warnings, this,
                [&](const QList<QQmlError> &errors) { warnings += errors.size(); });
        QQmlComponent component(&engine, QUrl::fromLocalFile(QString::fromUtf8(JSON_MODEL_FILE)));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> model(component.create());
        QVERIFY(model);
        model->setProperty("query", "$[*]");
        const QString valid = QString::fromUtf8("[{\"Board\":\"café 日本語\"},{\"Board\":\"CC3D\"}]");
        model->setProperty("json", valid);
        QCOMPARE(model->property("count").toInt(), 2);
        QVERIFY(model->property("errorString").toString().isEmpty());
        model->setProperty("json", "<html>Service unavailable</html>");
        QCOMPARE(model->property("count").toInt(), 0);
        QVERIFY(!model->property("errorString").toString().isEmpty());
        model->setProperty("json", valid);
        QCOMPARE(model->property("count").toInt(), 2);
        QVERIFY(model->property("errorString").toString().isEmpty());
        model->setProperty("json", "");
        QCOMPARE(model->property("count").toInt(), 0);
        QVERIFY(model->property("errorString").toString().isEmpty());
        QCOMPARE(warnings, 0);
    }
};

QTEST_MAIN(WelcomeJsonTests)
#include "welcome_json.moc"
