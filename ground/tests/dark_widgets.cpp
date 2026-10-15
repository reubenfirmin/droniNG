/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <config/expocurve.h>
#include <uavobjectbrowser/uavobjecttreemodel.h>
#include <uavobjectbrowser/fieldtreeitem.h>
#include <uavobjects/uavobjectsplugin.h>
#include <uavobjects/uavobjectmanager.h>
#include <extensionsystem/pluginmanager.h>
#include <utils/darktheme.h>
#include <attitudeactual.h>
#include <QDir>
#include <QPainter>
#include <QStyle>
#include <QTest>
#include <QTreeView>
#include <cmath>

namespace {
double luminance(const QColor &color)
{
    const auto channel = [](double component) {
        return component <= 0.04045 ? component / 12.92
                                   : std::pow((component + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF())
           + 0.0722 * channel(color.blueF());
}

double contrast(const QColor &foreground, const QColor &background)
{
    const double first = luminance(foreground), second = luminance(background);
    return (qMax(first, second) + 0.05) / (qMin(first, second) + 0.05);
}

QModelIndex namedChild(UAVObjectTreeModel &model, const QModelIndex &parent, const QString &name)
{
    for (int row = 0; row < model.rowCount(parent); ++row) {
        const QModelIndex index = model.index(row, 0, parent);
        if (index.data().toString() == name)
            return index;
    }
    return {};
}
}

class DarkWidgetTests : public QObject
{
    Q_OBJECT
private slots:
    void expoPlotLabelsAndCurves()
    {
        Utils::applyDarkTheme();
        QWidget parent;
        ExpoCurve plot(&parent);
        plot.resize(640, 400);
        plot.show();
        QTest::qWait(50);
        const auto axis = plot.axisWidget(QwtPlot::xBottom);
        QVERIFY(contrast(axis->palette().color(QPalette::WindowText),
                         axis->palette().color(QPalette::Window)) >= 4.5);
        const auto legend = plot.legend();
        QVERIFY(legend);
        QVERIFY(contrast(legend->palette().color(QPalette::Text),
                         legend->palette().color(legend->backgroundRole())) >= 4.5);
        const auto curves = plot.itemList(QwtPlotItem::Rtti_PlotCurve);
        QCOMPARE(curves.size(), 3);
        for (auto item : curves) {
            const auto curve = static_cast<QwtPlotCurve *>(item);
            QCOMPARE(curve->dataSize(), size_t(150));
            QVERIFY(curve->isVisible());
        }
        const QImage initial = plot.grab().toImage();
        QVERIFY(!initial.isNull());
        plot.plotDataRoll(80, 900, 50);
        plot.replot();
        QVERIFY(initial != plot.grab().toImage());
        const QString artifacts = qEnvironmentVariable("GCS_DARK_ARTIFACT_DIR");
        if (!artifacts.isEmpty()) {
            QVERIFY(QDir().mkpath(artifacts));
            QVERIFY(plot.grab().save(artifacts + "/expo.png"));
        }
    }

    void objectHighlights_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<int>("state");
        const QStringList states{"recent", "custom-dark", "pending", "updated",
                                 "status", "transparent", "custom-status"};
        for (const bool dark : {false, true}) {
            const QString prefix = dark ? "dark-" : "light-";
            for (int state = 0; state < states.size(); ++state)
                QTest::newRow(qPrintable(prefix + states.at(state))) << dark << state;
        }
    }

    void objectHighlights()
    {
        QFETCH(bool, dark);
        QFETCH(int, state);
        if (dark)
            Utils::applyDarkTheme();
        else
            QApplication::setPalette(QApplication::style()->standardPalette());
        ExtensionSystem::PluginManager plugins;
        UAVObjectsPlugin uavObjects;
        QString error;
        QVERIFY(uavObjects.initialize({}, &error));
        auto *objects = plugins.getObject<UAVObjectManager>();
        QVERIFY(objects);
        UAVObjectTreeModel model;
        model.setOnlyHighlightChangedValues(true);
        model.initializeModel();
        const QModelIndex dataObjects = namedChild(model, {}, "Data Objects");
        QVERIFY(dataObjects.isValid());
        auto *object = AttitudeActual::GetInstance(objects);
        QVERIFY(object);
        QModelIndex attitude;
        for (const auto &index : model.getDataObjectIndexes()) {
            auto *item = static_cast<DataObjectTreeItem *>(index.internalPointer());
            if (item->object() == object)
                attitude = index;
        }
        QVERIFY(attitude.isValid());
        const QModelIndex roll = namedChild(model, attitude, "Roll");
        QVERIFY(roll.isValid());
        const QModelIndex value = model.index(roll.row(), 1, attitude);
        auto *item = static_cast<FieldTreeItem *>(roll.internalPointer());
        item->removeHighlight();
        item->setChanged(false);
        item->setUpdatedOnly(false);
        const QColor statusColor(220, 170, 60);
        item->setIsPresentOnHardware(state != 6);
        if (state == 0 || state == 1 || state == 5 || state == 6) {
            if (state == 1 || state == 6)
                model.setRecentlyUpdatedColor(QColor(28, 46, 56));
            if (state == 5)
                model.setRecentlyUpdatedColor(QColor(255, 230, 230, 64));
            if (state == 6)
                model.setNotPresentOnHwColor(statusColor);
            auto data = object->getData();
            data.Roll = 25;
            object->setData(data);
            QVERIFY(item->highlighted());
        } else if (state == 2) {
            QVERIFY(model.setData(value, 25.0, Qt::EditRole));
            QVERIFY(item->changed());
        } else if (state == 3) {
            item->setChanged(true);
            item->setUpdatedOnly(true);
            item->setChanged(false);
        } else {
            const QColor statusColor(130, 160, 170);
            model.setNotPresentOnHwColor(statusColor);
            item->setIsPresentOnHardware(false);
            QCOMPARE(model.data(value, Qt::ForegroundRole).value<QColor>(), statusColor);
            QVERIFY(!model.data(value, Qt::BackgroundRole).isValid());
            return;
        }
        const QColor background = model.data(value, Qt::BackgroundRole).value<QColor>();
        const QColor foreground = model.data(value, Qt::ForegroundRole).value<QColor>();
        QVERIFY(background.isValid());
        QImage sample(1, 1, QImage::Format_ARGB32);
        sample.fill(QApplication::palette().color(QPalette::Base));
        QPainter painter(&sample);
        painter.fillRect(sample.rect(), background);
        painter.end();
        const QColor paintedBackground = sample.pixelColor(0, 0);
        QVERIFY2(contrast(foreground, paintedBackground) >= 4.5,
                 qPrintable(QString("Unreadable highlight: foreground %1, background %2, ratio %3")
                            .arg(foreground.name(), paintedBackground.name())
                            .arg(contrast(foreground, paintedBackground))));
        if (state == 6)
            QCOMPARE(foreground, statusColor);
        QTreeView tree;
        tree.setModel(&model);
        tree.resize(700, 400);
        tree.expand(dataObjects);
        tree.expand(attitude);
        tree.show();
        tree.scrollTo(value);
        QTest::qWait(50);
        QVERIFY(tree.visualRect(value).intersects(tree.viewport()->rect()));
        const QString artifacts = qEnvironmentVariable("GCS_DARK_ARTIFACT_DIR");
        if (!artifacts.isEmpty()) {
            QVERIFY(QDir().mkpath(artifacts));
            QVERIFY(tree.grab().save(artifacts + QString("/browser-%1-%2.png").arg(dark).arg(state)));
        }
    }
};

QTEST_MAIN(DarkWidgetTests)
#include "dark_widgets.moc"
