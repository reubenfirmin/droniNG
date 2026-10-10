/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <utils/qtlogging.h>
#include <QApplication>
#include <QInputMethodEvent>
#include <QLineEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QTest>
#include <QVBoxLayout>

static QStringList messages;
static void recordMessage(QtMsgType, const QMessageLogContext &, const QString &message)
{
    messages.append(message);
}

static const QString disableFunctionPrefix = QStringLiteral(
    "virtual void QtWaylandClient::QWaylandTextInputv3::disableSurface(wl_surface*) "
    "Trying to disable ");
static const QString disablePrefix = disableFunctionPrefix + "0x26e68f00 but ";

class QtLoggingTests : public QObject
{
    Q_OBJECT
private slots:
    void cleanup()
    {
        Utils::installQtMessageHandler();
        messages.clear();
    }

    void onlyAlreadyDisabledWarningIsFiltered_data()
    {
        QTest::addColumn<QString>("category");
        QTest::addColumn<QString>("message");
        QTest::addColumn<int>("severity");
        QTest::addColumn<bool>("benign");
        const QString category = QStringLiteral("qt.qpa.wayland.textinput");
        QTest::newRow("null-focused-spaced") << category << disablePrefix + "0x0 is focused" << int(QtWarningMsg) << true;
        QTest::newRow("null-focused-unspaced") << category << disablePrefix + "0x0is focused" << int(QtWarningMsg) << true;
        QTest::newRow("another-surface-focused") << category << disablePrefix + "0x1234 is focused" << int(QtWarningMsg) << false;
        QTest::newRow("another-surface-enabled") << category << disablePrefix + "0x1234 is enabled" << int(QtWarningMsg) << false;
        QTest::newRow("null-enabled") << category << disablePrefix + "0x0 is enabled" << int(QtWarningMsg) << false;
        QTest::newRow("another-category") << QString("app.test") << disablePrefix + "0x0 is focused" << int(QtWarningMsg) << false;
        QTest::newRow("critical") << category << disablePrefix + "0x0 is focused" << int(QtCriticalMsg) << false;
        QTest::newRow("other-warning") << category << QString("Input method failed") << int(QtWarningMsg) << false;
        QTest::newRow("other-function") << category << QString("Other::disableSurface Trying to disable 0x1 but 0x0 is focused") << int(QtWarningMsg) << false;
        QTest::newRow("qml-warning") << QString("default") << QString("SitesPanel.qml:48:13: Unable to assign [undefined] to QColor") << int(QtWarningMsg) << false;
    }

    void onlyAlreadyDisabledWarningIsFiltered()
    {
        QFETCH(QString, category);
        QFETCH(QString, message);
        QFETCH(int, severity);
        QFETCH(bool, benign);
        Utils::installQtMessageHandler(recordMessage);
        const QByteArray categoryBytes = category.toUtf8();
        QMessageLogger logger(__FILE__, __LINE__, Q_FUNC_INFO, categoryBytes.constData());
        if (severity == QtCriticalMsg)
            logger.critical().noquote().nospace() << message;
        else
            logger.warning().noquote().nospace() << message;
#if QT_VERSION == QT_VERSION_CHECK(6, 12, 0)
        QCOMPARE(messages, benign ? QStringList() : QStringList{message});
#else
        Q_UNUSED(benign);
        QCOMPARE(messages, QStringList{message});
#endif
    }

    void defaultLoggingSurvivesHandlerReplacement()
    {
        QProcess process;
        auto environment = QProcessEnvironment::systemEnvironment();
        // Fedora's Qt 5 defaults to journald; make the default sink observable.
        environment.insert("QT_FORCE_STDERR_LOGGING", "1");
        process.setProcessEnvironment(environment);
        process.start(QCoreApplication::applicationFilePath(), {"--probe-default-logging"});
        QVERIFY(process.waitForFinished(5000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 0);
        const QByteArray output = process.readAllStandardError();
        QVERIFY(output.contains("Startup warning remains visible"));
        QVERIFY(output.contains("Shutdown warning remains visible"));
        QVERIFY(!output.contains("Warning routed to the Debug gadget"));
    }

    void focusTransitionsKeepTextInputWorking()
    {
        QWidget window;
        QVBoxLayout layout(&window);
        QLineEdit edit;
        QPushButton button(QStringLiteral("Non-editable control"));
        QPushButton otherButton(QStringLiteral("Another non-editable control"));
        layout.addWidget(&edit);
        layout.addWidget(&button);
        layout.addWidget(&otherButton);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));

        // Observe the native Qt warning first, then verify the workaround on
        // the same focus transitions. No platform or input-method override.
        qInstallMessageHandler(recordMessage);
        for (int i = 0; i < 3; ++i) {
            button.setFocus();
            QTest::qWait(20);
            otherButton.setFocus();
            QTest::qWait(20);
            edit.setFocus();
            QTest::qWait(20);
        }
        button.setFocus();
        QTest::qWait(20);
#if QT_VERSION == QT_VERSION_CHECK(6, 12, 0)
        if (QGuiApplication::platformName().startsWith("wayland")) {
            QVERIFY2(messages.filter(disableFunctionPrefix).size() > 0,
                     qPrintable(QString("Native Wayland baseline did not reproduce the warning:\n")
                                + messages.join('\n')));
        }
#endif
        messages.clear();
        Utils::installQtMessageHandler(recordMessage);
        for (int i = 0; i < 10; ++i) {
            edit.setFocus();
            QTRY_VERIFY(edit.hasFocus());
            QTest::keyClicks(&edit, "Qt ");
            QInputMethodEvent commit;
            commit.setCommitString(QString::fromUtf8("café 日本語"));
            QApplication::sendEvent(&edit, &commit);
            QCOMPARE(edit.text(), QString::fromUtf8("Qt café 日本語"));
            edit.clear();
            button.setFocus();
            QTRY_VERIFY(button.hasFocus());
            QTest::qWait(20);
            otherButton.setFocus();
            QTRY_VERIFY(otherButton.hasFocus());
            QTest::qWait(20);
        }
        QCOMPARE(messages.filter(disableFunctionPrefix).size(), 0);
    }
};

int main(int argc, char **argv)
{
    Utils::installQtMessageHandler();
    if (argc == 2 && QString::fromLocal8Bit(argv[1]) == "--probe-default-logging") {
        qWarning("Startup warning remains visible");
        Utils::installQtMessageHandler(recordMessage);
        qWarning("Warning routed to the Debug gadget");
        Utils::installQtMessageHandler();
        qWarning("Shutdown warning remains visible");
        return messages == QStringList{"Warning routed to the Debug gadget"} ? 0 : 1;
    }
    QApplication application(argc, argv);
    QtLoggingTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "qt_logging.moc"
