#include <QDomDocument>
#include <QFile>
#include <QPainter>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <atomic>
#include <qwt_color_map.h>
#include <qwt_legend.h>
#include <qwt_plot.h>
#include <qwt_plot_curve.h>
#include <qwt_plot_magnifier.h>
#include <qwt_plot_picker.h>
#include <qwt_plot_renderer.h>
#include <qwt_plot_spectrogram.h>
#include <qwt_scale_map.h>
#include <qwt_wheel.h>

class TrackerProbe : public QwtPlotPicker
{
public:
    using QwtPlotPicker::QwtPlotPicker;
    using QwtPlotPicker::trackerTextF;
};

class RasterFixture : public QwtRasterData
{
public:
    RasterFixture() : guiThread(QThread::currentThread())
    {
        setInterval(Qt::XAxis, QwtInterval(0, 1));
        setInterval(Qt::YAxis, QwtInterval(0, 1));
        setInterval(Qt::ZAxis, QwtInterval(0, 2));
    }
    double value(double x, double y) const override
    {
        if (QThread::currentThread() != guiThread)
            workerSamples.fetch_add(1, std::memory_order_relaxed);
        return x + y;
    }
    mutable std::atomic<int> workerSamples{0};
private:
    QThread *const guiThread;
};

class SpectrogramProbe : public QwtPlotSpectrogram
{
public:
    using QwtPlotSpectrogram::renderImage;
};

class PlotCompatibilityTests : public QObject
{
    Q_OBJECT
private slots:
    void threadedSpectrogram()
    {
        SpectrogramProbe spectrogram;
        auto *data = new RasterFixture;
        spectrogram.setData(data);
        spectrogram.setColorMap(new QwtLinearColorMap(Qt::blue, Qt::red));
        QwtScaleMap xMap, yMap;
        xMap.setScaleInterval(0, 1);
        xMap.setPaintInterval(0, 127);
        yMap.setScaleInterval(0, 1);
        yMap.setPaintInterval(127, 0);
        const QRectF area(0, 0, 1, 1);
        spectrogram.setRenderThreadCount(1);
        const QImage single = spectrogram.renderImage(xMap, yMap, area, QSize(128, 128));
        QVERIFY(!single.isNull());
        QCOMPARE(data->workerSamples.load(), 0);
        spectrogram.setRenderThreadCount(4);
        const QImage threaded = spectrogram.renderImage(xMap, yMap, area, QSize(128, 128));
        QVERIFY(data->workerSamples.load() > 0);
        QCOMPARE(threaded, single);
        QVERIFY(threaded.pixelColor(0, 127).blue() > 240);
        QVERIFY(threaded.pixelColor(127, 0).red() > 240);
    }

    void curvesAxesLegendAndExport()
    {
        QwtPlot plot;
        plot.setTitle(QString::fromUtf8("Actitud – café"));
        plot.setAxisTitle(QwtPlot::xBottom, "Time (s)");
        plot.setAxisTitle(QwtPlot::yLeft, "Angle (degrees)");
        plot.setAxisScale(QwtPlot::xBottom, 0, 10);
        plot.setAxisScale(QwtPlot::yLeft, -5, 5);
        plot.insertLegend(new QwtLegend);
        auto *curve = new QwtPlotCurve("Yaw");
        curve->setPen(Qt::red, 2);
        curve->setRenderHint(QwtPlotItem::RenderAntialiased);
        const QVector<QPointF> samples{{0, -2}, {2, 2}, {4, -2}, {6, 2}, {8, -2}, {10, 2}};
        curve->setSamples(samples);
        curve->attach(&plot);
        plot.resize(640, 400);
        plot.show();
        QTest::qWait(50);
        plot.replot();
        QCOMPARE(curve->dataSize(), size_t(samples.size()));
        QCOMPARE(curve->boundingRect(), QRectF(0, -2, 10, 4));
        const QwtScaleMap map = plot.canvasMap(QwtPlot::xBottom);
        for (const QPointF &sample : samples)
            QVERIFY(qAbs(map.invTransform(map.transform(sample.x())) - sample.x()) < 1e-9);
        QCOMPARE(plot.infoToItem(plot.itemToInfo(curve)), static_cast<QwtPlotItem *>(curve));
        const QList<QwtLegendData> legend = curve->legendData();
        QCOMPARE(legend.size(), 1);
        QCOMPARE(legend.first().title().text(), QStringLiteral("Yaw"));
        QVERIFY(!legend.first().icon().isNull());

        QwtPlotRenderer renderer;
        QImage image(640, 400, QImage::Format_ARGB32);
        image.fill(Qt::white);
        QPainter painter(&image);
        renderer.render(&plot, &painter, QRectF(0, 0, 640, 400));
        painter.end();
        int curvePixels = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                const QColor color(image.pixel(x, y));
                curvePixels += color.red() > 180 && color.green() < 100 && color.blue() < 100;
            }
        QVERIFY(curvePixels > 100);
        const QString artifactDirectory = qEnvironmentVariable("GCS_PLOT_ARTIFACT_DIR");
        QVERIFY(!artifactDirectory.isEmpty());
        QVERIFY(image.save(artifactDirectory + "/scope.png"));
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString svgName = temporary.filePath("scope.svg");
        renderer.renderDocument(&plot, svgName, "svg", QSizeF(160, 100), 96);
        QFile svg(svgName);
        QVERIFY(svg.open(QIODevice::ReadOnly));
        QDomDocument document;
        QVERIFY(document.setContent(svg.readAll()));
        QCOMPARE(document.documentElement().tagName(), QStringLiteral("svg"));
        QVERIFY(document.elementsByTagName("polyline").size()
                + document.elementsByTagName("path").size() > 0);
        const QString pdfName = temporary.filePath("scope.pdf");
        renderer.renderDocument(&plot, pdfName, "pdf", QSizeF(160, 100), 96);
        QFile pdf(pdfName);
        QVERIFY(pdf.open(QIODevice::ReadOnly));
        QVERIFY(pdf.size() > 1000);
        QVERIFY(pdf.read(5) == "%PDF-");
        plot.resize(400, 300);
        plot.replot();
        QVERIFY(plot.canvasMap(QwtPlot::xBottom).pDist() < map.pDist());
    }

    void wheelAndMagnifier()
    {
        QwtWheel wheel;
        wheel.resize(200, 50);
        wheel.setRange(0, 10);
        wheel.setSingleStep(1);
        wheel.setValue(5);
        const QPointF position(wheel.rect().center());
        QWheelEvent forward(position, position, QPoint(), QPoint(0, 120),
                            Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(&wheel, &forward);
        QCOMPARE(wheel.value(), 6.0);
        QWheelEvent backward(position, position, QPoint(), QPoint(-120, 0),
                             Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(&wheel, &backward);
        QCOMPARE(wheel.value(), 5.0);
        QwtPlot plot;
        plot.setAxisScale(QwtPlot::xBottom, 0, 10);
        plot.resize(640, 400);
        plot.show();
        plot.replot();
        QwtPlotMagnifier magnifier(plot.canvas());
        magnifier.setWheelFactor(0.8);
        QApplication::sendEvent(plot.canvas(), &forward);
        // Keep Qwt's existing convention: a positive wheel delta zooms out.
        QVERIFY(plot.axisScaleDiv(QwtPlot::xBottom).range() > 10);
        const double expandedRange = plot.axisScaleDiv(QwtPlot::xBottom).range();
        QApplication::sendEvent(plot.canvas(), &backward);
        QVERIFY(plot.axisScaleDiv(QwtPlot::xBottom).range() < expandedRange);
        TrackerProbe picker(plot.canvas());
        QCOMPARE(picker.trackerTextF(QPointF(1.25, -2.5)).text(), QStringLiteral("1.2500, -2.5000"));
    }
};

QTEST_MAIN(PlotCompatibilityTests)
#include "plot_compat.moc"
