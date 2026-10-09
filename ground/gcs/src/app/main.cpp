/**
 ******************************************************************************
 *
 * @file       main.cpp
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 *             Parts by Nokia Corporation (qt-info@nokia.com) Copyright (C) 2009.
 * @author     Tau Labs, http://taulabs.org, Copyright (C) 2013-2015
 * @author     dRonin, http://dronin.org Copyright (C) 2015
 * @brief      Main() file
 * @see        The GNU Public License (GPL) Version 3
 * @defgroup   app GCS main application group
 * @{
 *
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

#define COMMAND_LINE_OVERRIDE "override"
#define COMMAND_LINE_CLEAN_CONFIG "reset-config"
#define COMMAND_LINE_NO_LOAD "no-load"
#define COMMAND_LINE_TEST "test"
#define COMMAND_LINE_MULTIPLE_OK "multiple-ok"
#define COMMAND_LINE_PLUGIN_OPTION "plugin-option"

#include "utils/xmlconfig.h"
#include "utils/pathutils.h"
#include "utils/darktheme.h"
#include <runguard/runguard.h>

#include <extensionsystem/pluginmanager.h>
#include <extensionsystem/pluginspec.h>
#include <extensionsystem/iplugin.h>

#include "libcrashreporter-qt/libcrashreporter-handler/Handler.h"

#include <QtCore/QDir>
#include <QtCore/QTextStream>
#include <QtCore/QFileInfo>
#include <QtCore/QDebug>
#include <QtCore/QTimer>
#include <QtCore/QLibraryInfo>
#include <QtCore/QTranslator>
#include <QtCore/QSettings>
#include <QtCore/QRegularExpression>
#include <QtCore/QVariant>

#include <QMessageBox>
#include <QApplication>
#include <QMainWindow>
#include <QPixmap>
#include "customsplash.h"
#include <QBitmap>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QLoggingCategory>

#include <iostream>

#include GCS_VERSION_INFO_FILE

#define USE_CRASHREPORTING

enum { OptionIndent = 4, DescriptionIndent = 24 };

static const char *corePluginNameC = "Core";

typedef QList<ExtensionSystem::PluginSpec *> PluginSpecSet;

static void displayHelpText(const QString &t)
{
    qWarning("%s", qPrintable(t));
}

static void displayError(const QString &t)
{
    qCritical("%s", qPrintable(t));
}

static void printVersion(const ExtensionSystem::PluginSpec *coreplugin,
                         const ExtensionSystem::PluginManager &pm)
{
    QString version;
    QTextStream str(&version);
    str << '\n'
        << GCS_PROJECT_BRANDING_PRETTY " GCS " << coreplugin->version() << " based on Qt "
        << qVersion() << "\n\n";
    pm.formatPluginVersions(str);
    str << '\n' << coreplugin->copyright() << '\n';
    displayHelpText(version);
}

static void printHelp(const QString &a0, const ExtensionSystem::PluginManager &pm)
{
    QString help;
    QTextStream str(&help);
    str << a0 << '\n' << "Plugin Options:";
    pm.formatPluginOptions(str, OptionIndent, DescriptionIndent);
    displayHelpText(help);
}

static inline QString msgCoreLoadFailure(const QString &why)
{
    return QCoreApplication::translate("Application", "Failed to load core: %1").arg(why);
}

static inline QStringList getPluginPaths()
{
    QStringList rc;
    // Figure out root:  Up one from 'bin'
    QDir rootDir = QApplication::applicationDirPath();
    rootDir.cdUp();
    const QString rootDirPath = rootDir.canonicalPath();
    // 1) "plugins" in an installed Linux tree
    QString pluginPath = rootDirPath;
    pluginPath += QLatin1Char('/');
    pluginPath += QLatin1String(GCS_LIBRARY_BASENAME);
    pluginPath += QLatin1Char('/');
    pluginPath += QLatin1String("plugins");
    rc.push_back(pluginPath);
    // 2) "plugins" in the Linux build tree
    pluginPath = rootDirPath;
    pluginPath += QLatin1Char('/');
    pluginPath += QLatin1String(GCS_LIBRARY_BASENAME);
    pluginPath += QLatin1Char('/');
    pluginPath += QLatin1String(GCS_PROJECT_BRANDING);
    pluginPath += QLatin1Char('/');
    pluginPath += QLatin1String("plugins");
    rc.push_back(pluginPath);
    return rc;
}

#define SHARE_PATH "/../share"

static void overrideSettings(QCommandLineParser &parser, QSettings *settings)
{
    QMap<QString, QString> settingOptions;
    // Options like -DMy/setting=test
    const QRegularExpression rx("([^=]+)=(.*)");

    foreach (QString option, parser.values(COMMAND_LINE_OVERRIDE)) {
        const auto match = rx.match(option);
        if (match.hasMatch()) {
            settingOptions.insert(match.captured(1), match.captured(2));
        }
    }
    if (parser.isSet(COMMAND_LINE_CLEAN_CONFIG)) {
        settings->clear();
    }

    QList<QString> keys = settingOptions.keys();
    foreach (QString key, keys) {
        settings->setValue(key, settingOptions.value(key));
    }
    settings->sync();
}

#if defined(_MSC_VER)
#pragma optimize ("", off)
#endif

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    Utils::applyDarkTheme();

    QCommandLineParser parser;
    parser.setApplicationDescription(GCS_PROJECT_BRANDING_PRETTY " GCS");
    const QCommandLineOption helpOption = parser.addHelpOption();
    const QCommandLineOption versionOption = parser.addVersionOption();

    QCommandLineOption overrideOption(
        QStringList() << "o" << COMMAND_LINE_OVERRIDE,
        QCoreApplication::translate("main", "Overrides a configuration setting."),
        QCoreApplication::translate("main", "setting=value"), "");
    parser.addOption(overrideOption);
    QCommandLineOption cleanConfig(
        QStringList() << "r" << COMMAND_LINE_CLEAN_CONFIG,
        QCoreApplication::translate("main", "Resets the configuration file to default values."));
    parser.addOption(cleanConfig);
    QCommandLineOption noLoadOption(
        QStringList() << "n" << COMMAND_LINE_NO_LOAD,
        QCoreApplication::translate("main", "Skips loading of the plugin."),
        QCoreApplication::translate("main", "plugin name"), "");
    parser.addOption(noLoadOption);
    QCommandLineOption doTestsOption(
        QStringList() << "t" << COMMAND_LINE_TEST,
        QCoreApplication::translate("main", "Runs tests on the plugin. (Requires compiling with -D "
                                            "WITH_TESTS, look in the uploader plugin for example "
                                            "usage). Special argument 'all' will run all tests."),
        QCoreApplication::translate("main", "plugin name"), "");
    parser.addOption(doTestsOption);
    QCommandLineOption checkPluginsOption(QStringLiteral("check-plugins"),
        QCoreApplication::translate("main", "Loads the enabled plugins, reports errors, and exits."));
    parser.addOption(checkPluginsOption);
    QCommandLineOption pluginOption(
        QStringList() << "p" << COMMAND_LINE_PLUGIN_OPTION,
        QCoreApplication::translate("main", "Passes an option to a plugin."),
        QCoreApplication::translate("main", "option_name=option_value"), "");
    QCommandLineOption multipleInstancesOption(
        QStringList() << "m" << COMMAND_LINE_MULTIPLE_OK,
        QCoreApplication::translate("main", "Allows multiple instances to run at once"));
    parser.addOption(multipleInstancesOption);

    // The options are passed to the plugin init as a QStringList i.e. >> iplugin::initialize(const
    // QStringList &arguments, QString *errorString)
    // These options need to be set in the pluginspec file (look in the Core.pluginspec file for an
    // example)
    parser.addOption(pluginOption);
    parser.addPositionalArgument(
        "config", QCoreApplication::translate("main", "Use the specified configuration file."),
        QCoreApplication::translate("main", "config file"));

    if (!parser.parse(QCoreApplication::arguments())) {
        displayError(parser.errorText());
        displayHelpText(parser.helpText());
        return 1;
    }

#ifdef USE_CRASHREPORTING
    // Disable crash reporter if we're doing unit tests
    // This means we won't hang CI, and QtTest will give us a nice stack trace
    bool enableReporter = parser.values(doTestsOption).isEmpty() && !parser.isSet(checkPluginsOption);

    if (enableReporter) {
        QString dirName(GCS_REVISION_PRETTY);
        dirName = dirName.replace("%@%", "_");
        // Limit to alphanumerics plus dots, because this will be a filename
        // component.
        dirName = dirName.replace(QRegularExpression("[^A-Za-z0-9.]+"), "_");
        dirName = QDir::tempPath() + QDir::separator() + GCS_PROJECT_BRANDING + "_" + dirName;
        QDir().mkdir(dirName);
        new CrashReporter::Handler(dirName, true, "crashreporterapp");
    }
#endif

    RunGuard &guard = RunGuard::instance(QStringLiteral(GCS_PROJECT_BRANDING)
                                         + QStringLiteral("_gcs_run_guard"));

    if (!guard.tryToRun()) {
        std::cerr << "GCS already running!" << std::endl;

        if (!parser.isSet(multipleInstancesOption)) {
            return 1;
        }
    }

    // suppress SSL warnings generated during startup (handy if you want to use QT_FATAL_WARNINGS)
    QLoggingCategory::setFilterRules("qt.network.ssl.warning=false");

    // disable QML disk caching, this caches paths in qmlc files which break when the GCS
    // installation is moved. TODO: see if we can work around that
    qputenv("QML_DISABLE_DISK_CACHE", "true");

    QString locale = QLocale::system().name();

    // Must be done before any QSettings class is created
    QSettings::setPath(XmlConfig::XmlSettingsFormat, QSettings::SystemScope,
                       QCoreApplication::applicationDirPath() + QLatin1String(SHARE_PATH));

    QString settingsFilename;
    QStringList positionalArguments = parser.positionalArguments();
    {
        QSettings *settings;
        if (positionalArguments.length() > 0) {
            if (QFile::exists(positionalArguments.at(0)))
                settings = new QSettings(positionalArguments.at(0), XmlConfig::XmlSettingsFormat);
            else {
                displayError(
                    QString("Error loading configuration file:%0").arg(positionalArguments.at(0)));
                return 1;
            }
        } else
            settings = new QSettings(XmlConfig::XmlSettingsFormat, QSettings::UserScope,
                                     QLatin1String(GCS_PROJECT_BRANDING),
                                     QLatin1String(GCS_PROJECT_BRANDING "_config"));
        if (settings->status() != QSettings::NoError) {
            displayError(
                QString("Error parsing the configuration file:%0").arg(settings->fileName()));
            return 1;
        }
        overrideSettings(parser, settings);
        locale = settings->value("General/OverrideLanguage", locale).toString();
        settingsFilename = settings->fileName();
        delete settings;
    }
    QTranslator translator;
    QTranslator qtTranslator;

    QPixmap pixmap(":/images/gcs_splashscreen.png");
    CustomSplash splash(pixmap);
    if (!parser.isSet(versionOption) && !parser.isSet(helpOption)) {
        splash.show();
        splash.showMessage("Loading translations");
        qApp->processEvents();
    }
    const QString &gcsTranslationsPath =
        QCoreApplication::applicationDirPath() + QLatin1String(SHARE_PATH "/translations");
    if (translator.load(QLatin1String("droning_") + locale, gcsTranslationsPath)) {
        const QString &qtTrPath = QLibraryInfo::location(QLibraryInfo::TranslationsPath);
        const QString &qtTrFile = QLatin1String("qt_") + locale;
        // Binary installer puts Qt tr files into creatorTrPath
        if (qtTranslator.load(qtTrFile, qtTrPath)
            || qtTranslator.load(qtTrFile, gcsTranslationsPath)) {
            QCoreApplication::installTranslator(&translator);
            QCoreApplication::installTranslator(&qtTranslator);
        } else {
            translator.load(QString()); // unload()
        }
    }
    app.setProperty("qtc_locale", locale); // Do we need this?

    // Load
    ExtensionSystem::PluginManager pluginManager;
    Utils::PathUtils().setSettingsFilename(settingsFilename);
    pluginManager.setFileExtension(QLatin1String("pluginspec"));

    const QStringList pluginPaths = getPluginPaths();
    pluginManager.setPluginPaths(pluginPaths);

    splash.showMessage("Parsing command line options");
    qApp->processEvents();
    QStringList parsingErrors = pluginManager.parseOptions(
        parser.values(pluginOption), parser.values(doTestsOption), parser.values(noLoadOption));
    if (!parsingErrors.isEmpty()) {
        displayError("Plugin options parsing failed with the following errors:");
        foreach (QString str, parsingErrors) {
            displayError(str);
        }
        return 1;
    }
    const PluginSpecSet plugins = pluginManager.plugins();
    ExtensionSystem::PluginSpec *coreplugin = nullptr;
    foreach (ExtensionSystem::PluginSpec *spec, plugins) {
        if (spec->name() == QLatin1String(corePluginNameC)) {
            coreplugin = spec;
            break;
        }
    }
    if (!parser.isSet(versionOption) && !parser.isSet(helpOption)) {
        splash.showMessage(QLatin1String("Checking core plugin"));
        qApp->processEvents();
    }
    if (!coreplugin) {
        const QString reason =
            QCoreApplication::translate("Application", "Could not find 'Core.pluginspec' in %1")
                .arg(pluginPaths.join(QLatin1String(",")));
        displayError(msgCoreLoadFailure(reason));
        return 1;
    }
    if (coreplugin->hasError()) {
        displayError(msgCoreLoadFailure(coreplugin->errorString()));
        return 1;
    }
    if (parser.isSet(versionOption)) {
        printVersion(coreplugin, pluginManager);
        return 0;
    }

    if (parser.isSet(helpOption)) {
        printHelp(parser.helpText(), pluginManager);
        return 0;
    }

    QObject::connect(&pluginManager, &ExtensionSystem::PluginManager::splashMessages, &splash,
                     &CustomSplash::showMessage);
    QObject::connect(&pluginManager, &ExtensionSystem::PluginManager::hideSplash, &splash,
                     &CustomSplash::hide);
    QObject::connect(&pluginManager, &ExtensionSystem::PluginManager::showSplash, &splash,
                     &CustomSplash::show);

    pluginManager.loadPlugins();
    {
        QStringList errors, plugins;
        for (const auto p : pluginManager.plugins()) {
            if (p->hasError()) {
                errors.append(QStringLiteral("%0:\n%1").arg(p->name(), p->errorString()));
                plugins.append(p->name());
            }
        }

        if (!errors.isEmpty()) {
            QString msg(QCoreApplication::tr(
                        "Some problems were encountered whilst loading the following plugins: "));
            msg.append(plugins.join(QStringLiteral(", ")));

            if (parser.values(doTestsOption).isEmpty() && !parser.isSet(checkPluginsOption)) {
                QMessageBox msgBox(QMessageBox::Warning,
                                   QCoreApplication::tr("Plugin loader messages"),
                                   msg, QMessageBox::Ok);
                msgBox.setDetailedText(errors.join(QStringLiteral("\n\n")));
                msgBox.exec();
            } else {
                qCritical().noquote() << "Some plugins failed to load:\n"
                                      << errors.join(QStringLiteral("\n\n"));
                return 1;
            }
        }
    }

    if (parser.isSet(checkPluginsOption)) {
        int running = 0;
        for (const auto *plugin : pluginManager.plugins())
            running += plugin->state() == ExtensionSystem::PluginSpec::Running;
        std::cout << "Plugin loading check passed: " << running
                  << " plugins running." << std::endl;
        QTimer::singleShot(100, &app, &QCoreApplication::quit);
    } else {
        QTimer::singleShot(100, &pluginManager, SLOT(startTests()));
    }
    splash.close();
    return app.exec();
}
