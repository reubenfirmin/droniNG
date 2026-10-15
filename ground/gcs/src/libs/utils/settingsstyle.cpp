// SPDX-License-Identifier: GPL-3.0-or-later
#include "settingsstyle.h"
#include "darktheme.h"

#include <QApplication>
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QDomDocument>
#include <QEvent>
#include <QFile>
#include <QGraphicsView>
#include <QGridLayout>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPainter>
#include <QProgressBar>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSlider>
#include <QPushButton>
#include <QStyle>
#include <QSvgRenderer>
#include <QTableView>
#include <QTextEdit>
#include <QTextBlock>
#include <QTextCursor>
#include <functional>

QIcon Utils::settingsIcon(const QString &name)
{
    QSvgRenderer renderer(QStringLiteral(":/utils/images/settings-icons.svg"));
    QIcon icon;
    const QPalette palette = QApplication::palette();
    for (const auto mode : {QIcon::Normal, QIcon::Selected, QIcon::Active, QIcon::Disabled}) {
        for (const int scale : {1, 2, 3}) {
            QPixmap pixmap(24 * scale, 24 * scale);
            pixmap.fill(Qt::transparent);
            QPainter painter(&pixmap);
            const QRectF bounds = renderer.boundsOnElement(name);
            renderer.render(&painter, name, QRectF(bounds.topLeft() * scale, bounds.size() * scale));
            painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
            const QColor color = mode == QIcon::Disabled ? palette.color(QPalette::Disabled, QPalette::Text)
                : mode == QIcon::Selected || mode == QIcon::Active ? palette.color(QPalette::Highlight)
                : palette.color(QPalette::WindowText);
            painter.fillRect(pixmap.rect(), color);
            painter.end();
            pixmap.setDevicePixelRatio(scale);
            icon.addPixmap(pixmap, mode);
        }
    }
    return icon;
}

void Utils::styleSettingsPage(QWidget *page)
{
    const auto packRows = [](QLayout *layout) {
        if (auto *grid = qobject_cast<QGridLayout *>(layout)) {
            int tail = grid->property("settingsTailRow").toInt();
            if (!grid->property("settingsTailRow").isValid()) {
                tail = grid->rowCount();
                grid->addItem(new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Expanding), tail, 0);
                grid->setProperty("settingsTailRow", tail);
            }
            for (int row = 0; row < tail; ++row) grid->setRowStretch(row, 0);
            grid->setRowStretch(tail, 1);
        } else if (auto *column = qobject_cast<QVBoxLayout *>(layout)) {
            if (!column->property("settingsTrailingSpace").toBool()) {
                for (int index = 0; index < column->count(); ++index) column->setStretch(index, 0);
                column->addStretch(1);
                column->setProperty("settingsTrailingSpace", true);
            }
        }
    };
    page->setProperty("settingsPage", true);
    QFont font = QApplication::font();
    font.setPixelSize(14);
    page->setFont(font);
    const auto widgets = page->findChildren<QWidget *>();
    for (auto *widget : widgets) {
        // Embedded editors inherit their spin box's font and palette.
        if (qobject_cast<QLineEdit *>(widget)
            && qobject_cast<QAbstractSpinBox *>(widget->parentWidget())) continue;
        // Designer forms predate the shared theme. Remove decorative overrides,
        // retaining functional colours such as channel inversion and LED swatches.
        const QString localStyle = widget->styleSheet();
        if (localStyle.contains("qlineargradient")) {
            widget->setStyleSheet({});
            widget->setProperty("columnHeading", true);
        }
        if (auto *label = qobject_cast<QLabel *>(widget)) {
            if (localStyle.contains("color: red") || localStyle.contains("color:red")) {
                label->setStyleSheet({});
                label->setProperty("role", "warning");
            }
            if (label->text().contains("font-family") || label->text().contains("font-size")) {
                QString text = label->text();
                text.remove(QRegularExpression("font-family\\s*:\\s*(?:'[^']*'|[^;\"<]*);?"));
                text.remove(QRegularExpression("font-size\\s*:[^;\"<]*;?"));
                label->setText(text);
            }
            if (label->text().contains("color:#ff0000") || label->text().contains("color:#ff6b74")) {
                QString text = label->text();
                text.remove(QRegularExpression("color\\s*:#[0-9a-fA-F]{6};?"));
                label->setText(text);
                label->setProperty("role", "warning");
            }
            if (label->wordWrap()) label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        }
        const bool heading = widget->property("columnHeading").toBool() || qobject_cast<QGroupBox *>(widget);
        QFont widgetFont = font;
        if (heading) widgetFont.setWeight(QFont::DemiBold);
        widget->setFont(widgetFont);
        if (widget->testAttribute(Qt::WA_SetPalette)) widget->setPalette(QApplication::palette());
        if (auto *spin = qobject_cast<QAbstractSpinBox *>(widget)) {
            spin->setProperty("spinButtons", spin->buttonSymbols() != QAbstractSpinBox::NoButtons);
            spin->setAlignment(Qt::AlignRight);
            // Legacy forms fix many tuning fields at 55 x 22 pixels.
            // Let their contents and the shared control height determine sizing.
            if (spin->maximumHeight() > 0 && spin->maximumHeight() <= 32)
                spin->setMaximumHeight(QWIDGETSIZE_MAX);
            if (spin->maximumHeight() > 0) spin->setMinimumHeight(32);
            if (spin->maximumWidth() > 0 && spin->maximumWidth() < 120)
                spin->setMaximumWidth(QWIDGETSIZE_MAX);
        }
        if (auto *combo = qobject_cast<QComboBox *>(widget)) {
            if (combo->maximumWidth() > 0 && combo->maximumWidth() < 120)
                combo->setMaximumWidth(QWIDGETSIZE_MAX);
        }
        if (auto *label = qobject_cast<QLabel *>(widget)) {
            if (label->maximumHeight() > 0 && label->maximumHeight() < 28)
                label->setMaximumHeight(QWIDGETSIZE_MAX);
            if (!label->wordWrap() && !label->text().isEmpty() && label->pixmap().isNull())
                label->setMinimumWidth(label->sizeHint().width());
        }
        if (auto *scroll = qobject_cast<QScrollArea *>(widget)) {
            scroll->setFrameShape(QFrame::NoFrame);
            scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
            if (scroll->widget() && scroll->widget()->layout()) {
                // QScrollArea calculates height-for-width itself. An explicit
                // minimum height from a wrapped label's narrow size hint can
                // otherwise create an unnecessary scrollbar on a wide page.
                scroll->widget()->layout()->setSizeConstraint(QLayout::SetNoConstraint);
                scroll->widget()->setMinimumSize(0, 0);
                scroll->widget()->layout()->setAlignment(Qt::AlignTop);
                packRows(scroll->widget()->layout());
            }
        }
        if (auto *graphics = qobject_cast<QGraphicsView *>(widget)) {
            graphics->setFrameShape(QFrame::NoFrame);
            graphics->setBackgroundBrush(Qt::NoBrush);
            graphics->setForegroundBrush(Qt::NoBrush);
        }
        if (auto *group = qobject_cast<QGroupBox *>(widget)) {
            group->setProperty("sectionTitle", !group->title().isEmpty());
            if (group->layout()) {
                group->layout()->setAlignment(Qt::AlignTop);
                packRows(group->layout());
            }
        }
        if (auto *progress = qobject_cast<QProgressBar *>(widget))
            progress->setProperty("percentVisible", progress->isTextVisible());
        if (auto *table = qobject_cast<QTableView *>(widget)) {
            table->setShowGrid(false);
            table->setAlternatingRowColors(true);
            table->verticalHeader()->setMinimumSectionSize(40);
            table->verticalHeader()->setDefaultSectionSize(40);
            table->horizontalHeader()->setHighlightSections(false);
        }
        if (auto *text = qobject_cast<QTextEdit *>(widget)) {
            text->document()->setDefaultFont(font);
            QTextCursor cursor(text->document());
            cursor.select(QTextCursor::Document);
            QTextCharFormat format;
            format.setFontFamilies(font.families());
            format.setProperty(QTextFormat::FontPixelSize, 14);
            cursor.mergeCharFormat(format);
            // Retain emphasis and links while replacing the old bright-red warning.
            for (auto block = text->document()->begin(); block.isValid(); block = block.next()) {
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const auto fragment = it.fragment();
                    const auto color = fragment.charFormat().foreground().color();
                    if (fragment.isValid() && color.red() > 180 && color.green() < 80 && color.blue() < 80) {
                        cursor.setPosition(fragment.position());
                        cursor.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
                        format = {};
                        format.setForeground(warningColor());
                        cursor.mergeCharFormat(format);
                    }
                }
            }
        }
        if (auto *button = qobject_cast<QAbstractButton *>(widget)) {
            if (button->text() == "Save") button->setProperty("primary", true);
            if (button->property("objrelation").toStringList().contains("button:help")) {
                button->setIcon(settingsIcon("help"));
                button->setIconSize(QSize(20, 20));
                button->setFixedSize(34, 34);
                button->setProperty("iconOnly", true);
            }
            if (button->objectName() == "btnReverse" || button->objectName() == "pb_reverseActuator") {
                button->setIcon(settingsIcon("reverse"));
                button->setIconSize(QSize(18, 18));
                button->setMinimumWidth(36);
                button->setMaximumWidth(48);
                button->setProperty("iconOnly", true);
            }
        }
        widget->style()->unpolish(widget);
        widget->style()->polish(widget);
        QEvent styleChange(QEvent::StyleChange);
        QApplication::sendEvent(widget, &styleChange);
        // Let layouts size containers from the styled contents instead of the
        // fixed heights and minimum sizes in the original Designer forms.
        if (widget->layout() && !qobject_cast<QAbstractItemView *>(widget)) {
            widget->setMinimumSize(0, 0);
            widget->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        }
        if (qobject_cast<QAbstractSpinBox *>(widget) && widget->maximumWidth() > 0)
            widget->setMinimumWidth(widget->sizeHint().width());
        // Polishing restores Designer's original size constraints. Apply the
        // control height afterwards so layouts reserve the full styled bounds.
        if (qobject_cast<QAbstractSpinBox *>(widget) || qobject_cast<QComboBox *>(widget)) {
            if (widget->maximumHeight() > 0 && widget->maximumHeight() <= 32)
                widget->setMaximumHeight(QWIDGETSIZE_MAX);
            if (widget->maximumHeight() > 0)
                widget->setMinimumHeight(qMax(36, widget->sizeHint().height()));
        }
        // Size fields for their contents, leaving excess space outside the form.
        if (auto *spin = qobject_cast<QAbstractSpinBox *>(widget)) {
            if (auto *editor = spin->findChild<QLineEdit *>())
                editor->setTextMargins(8, 0, 8, 0);
            if (spin->maximumWidth() > 0) {
                spin->setFixedWidth(qMax(124, spin->sizeHint().width() + 16));
                spin->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            }
        } else if (auto *combo = qobject_cast<QComboBox *>(widget)) {
            int textWidth = 0;
            for (int index = 0; index < combo->count(); ++index)
                textWidth = qMax(textWidth, combo->fontMetrics().horizontalAdvance(combo->itemText(index)));
            combo->setFixedWidth(qBound(124, textWidth + 52, 300));
            combo->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        } else if (auto *editor = qobject_cast<QLineEdit *>(widget)) {
            if (!editor->property("flexibleWidth").toBool()) {
                editor->setFixedWidth(editor->isReadOnly() ? 160 : 280);
                editor->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            }
        } else if (auto *button = qobject_cast<QPushButton *>(widget)) {
            if (!button->property("iconOnly").toBool())
                button->setFixedWidth(qMax(80, button->sizeHint().width()));
        } else if (auto *slider = qobject_cast<QSlider *>(widget)) {
            if (slider->orientation() == Qt::Horizontal) {
                slider->setMinimumWidth(160);
                slider->setMaximumWidth(240);
            } else {
                slider->setFixedHeight(180);
                slider->setMinimumWidth(32);
            }
        }
        if (auto *label = qobject_cast<QLabel *>(widget)) {
            if (!label->wordWrap() && !label->text().isEmpty() && label->pixmap().isNull()) {
                if (label->maximumWidth() > 0 && label->maximumWidth() < label->sizeHint().width())
                    label->setMaximumWidth(QWIDGETSIZE_MAX);
                const int height = label->sizeHint().height();
                if (label->maximumHeight() > 0 && label->maximumHeight() < height)
                    label->setMaximumHeight(QWIDGETSIZE_MAX);
                if (label->maximumHeight() > 0) label->setMinimumHeight(height);
            }
        }
        widget->updateGeometry();
    }
    // Layouts retain the original rows, columns and control order.
    for (auto *layout : page->findChildren<QLayout *>()) {
        if (auto *grid = qobject_cast<QGridLayout *>(layout)) {
            grid->setHorizontalSpacing(12);
            grid->setVerticalSpacing(8);
            QMap<int, QList<QWidget *>> columns;
            bool simpleForm = true;
            for (int index = 0; index < grid->count(); ++index) {
                auto *item = grid->itemAt(index);
                auto *widget = item->widget();
                if (item->layout()) simpleForm = false;
                if (!widget) continue;
                const bool field = qobject_cast<QAbstractSpinBox *>(widget)
                    || qobject_cast<QComboBox *>(widget) || qobject_cast<QLineEdit *>(widget);
                if (field && !widget->property("flexibleWidth").toBool()) {
                    int row, column, rows, span;
                    grid->getItemPosition(index, &row, &column, &rows, &span);
                    grid->setAlignment(widget, Qt::AlignLeft | Qt::AlignVCenter);
                    if (span == 1 && !widget->isHidden()) columns[column].append(widget);
                } else if (!qobject_cast<QLabel *>(widget) && !qobject_cast<QAbstractButton *>(widget)
                           && !qobject_cast<QSlider *>(widget)) {
                    simpleForm = false;
                }
            }
            int sharedWidth = grid->property("fieldMinimumWidth").toInt();
            const bool compactForm = grid->property("compactForm").toBool()
                || (simpleForm && !columns.isEmpty());
            if (compactForm) {
                grid->setAlignment(Qt::AlignLeft | Qt::AlignTop);
                packRows(grid);
            }
            if (grid->property("uniformFieldWidth").toBool())
                for (const auto &fields : columns)
                    for (auto *field : fields) sharedWidth = qMax(sharedWidth, field->maximumWidth());
            for (auto it = columns.cbegin(); it != columns.cend(); ++it) {
                int numericWidth = sharedWidth, selectionWidth = sharedWidth, textWidth = sharedWidth;
                for (auto *field : it.value()) {
                    int &width = qobject_cast<QAbstractSpinBox *>(field) ? numericWidth
                        : qobject_cast<QComboBox *>(field) ? selectionWidth : textWidth;
                    width = qMax(width, field->maximumWidth());
                }
                for (auto *field : it.value())
                    field->setFixedWidth(qobject_cast<QAbstractSpinBox *>(field) ? numericWidth
                        : qobject_cast<QComboBox *>(field) ? selectionWidth : textWidth);
                for (int index = 0; index < grid->count(); ++index) {
                    auto *label = qobject_cast<QLabel *>(grid->itemAt(index)->widget());
                    if (!label || !label->property("columnHeading").toBool()) continue;
                    int row, column, rows, span;
                    grid->getItemPosition(index, &row, &column, &rows, &span);
                    if (column == it.key() && span == 1) {
                        label->setFixedWidth(qMax(label->fontMetrics().horizontalAdvance(label->text()) + 8,
                            qMax(numericWidth, qMax(selectionWidth, textWidth))));
                        grid->setAlignment(label, Qt::AlignLeft | Qt::AlignVCenter);
                    }
                }
            }
            if (compactForm && !columns.isEmpty()) {
                int tail = grid->property("settingsTailColumn").toInt();
                if (!grid->property("settingsTailColumn").isValid()) {
                    tail = grid->columnCount();
                    grid->addItem(new QSpacerItem(0, 0, QSizePolicy::Expanding, QSizePolicy::Minimum), 0, tail);
                    grid->setProperty("settingsTailColumn", tail);
                }
                for (int column = 0; column < tail; ++column) grid->setColumnStretch(column, 0);
                grid->setColumnStretch(tail, 1);
            }
        } else if (auto *form = qobject_cast<QFormLayout *>(layout)) {
            form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
            form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
            form->setHorizontalSpacing(12);
            form->setVerticalSpacing(8);
        } else if (auto *row = qobject_cast<QHBoxLayout *>(layout)) {
            bool fields = false, simpleForm = true;
            for (int index = 0; index < row->count(); ++index) {
                auto *widget = row->itemAt(index)->widget();
                if (!widget) {
                    if (!row->itemAt(index)->spacerItem()) simpleForm = false;
                    continue;
                }
                fields |= qobject_cast<QAbstractSpinBox *>(widget) || qobject_cast<QComboBox *>(widget);
                simpleForm &= qobject_cast<QLabel *>(widget) || qobject_cast<QAbstractSpinBox *>(widget)
                    || qobject_cast<QComboBox *>(widget) || qobject_cast<QAbstractButton *>(widget);
            }
            if (fields && simpleForm && !row->property("settingsTrailingSpace").toBool()) {
                for (int index = row->count() - 1; index >= 0; --index)
                    if (row->itemAt(index)->spacerItem()) delete row->takeAt(index);
                for (int index = 0; index < row->count(); ++index) row->setStretch(index, 0);
                row->addStretch(1);
                row->setProperty("settingsTrailingSpace", true);
            }
            row->setSpacing(qMax(8, row->spacing()));
        } else if (layout->spacing() < 8) {
            layout->setSpacing(8);
        }
    }
    page->style()->unpolish(page);
    page->style()->polish(page);
    page->setMinimumSize(0, 0);
    const auto layouts = page->findChildren<QLayout *>();
    for (auto it = layouts.crbegin(); it != layouts.crend(); ++it) {
        (*it)->invalidate();
        (*it)->activate();
    }
    page->updateGeometry();
}

QByteArray Utils::flatSvgDiagram(const QString &resource)
{
    return flatSvgDiagram(resource, false);
}

QByteArray Utils::flatSvgDiagram(const QString &resource, bool emphasizeRotation)
{
    QFile file(resource);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QDomDocument document;
    if (!document.setContent(file.readAll())) return {};
    const QPalette palette = QApplication::palette();
    auto themedPaint = [&](const QColor &color) {
        if (!color.isValid()) return color.name();
        if (color.saturationF() < 0.18)
            return palette.color(color.lightnessF() < 0.35 ? QPalette::WindowText
                : color.lightnessF() > 0.85 ? QPalette::PlaceholderText : QPalette::Mid).name();
        return color.hueF() < 0.18 || color.hueF() > 0.9 ? warningColor().name()
            : palette.color(QPalette::Highlight).name();
    };
    QHash<QString, QString> gradients;
    QHash<QString, QString> inheritedGradients;
    for (const auto &tag : {QString("linearGradient"), QString("radialGradient")}) {
        const auto elements = document.elementsByTagName(tag);
        for (int i = 0; i < elements.size(); ++i) {
            const auto gradient = elements.at(i).toElement();
            const auto stops = gradient.elementsByTagName("stop");
            if (stops.isEmpty()) {
                inheritedGradients.insert(gradient.attribute("id"), gradient.attribute("xlink:href").mid(1));
                continue;
            }
            const auto stop = stops.at(stops.size() - 1).toElement();
            auto match = QRegularExpression("stop-color:([^;]+)").match(stop.attribute("style"));
            gradients.insert(gradient.attribute("id"), themedPaint(QColor(match.hasMatch()
                ? match.captured(1) : stop.attribute("stop-color"))));
        }
    }
    // Illustrator/Inkscape diagrams commonly reuse gradient definitions by href.
    for (int pass = 0; pass < inheritedGradients.size(); ++pass) {
        bool resolved = false;
        for (auto it = inheritedGradients.cbegin(); it != inheritedGradients.cend(); ++it) {
            if (!gradients.contains(it.key()) && gradients.contains(it.value())) {
                gradients.insert(it.key(), gradients.value(it.value()));
                resolved = true;
            }
        }
        if (!resolved) break;
    }
    QList<QDomElement> elements;
    std::function<void(QDomElement)> collect = [&](QDomElement element) {
        elements.append(element);
        for (auto child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement())
            collect(child);
    };
    collect(document.documentElement());
    const QRegularExpression colorExpression("#[0-9a-fA-F]{6}\\b");
    const QRegularExpression gradientExpression("url\\(#([^)]+)\\)");
    for (int i = 0; i < elements.size(); ++i) {
        auto element = elements.at(i);
        const QString originalStyle = element.attribute("style");
        for (const auto &attribute : {QString("style"), QString("fill"), QString("stroke")}) {
            QString paint = element.attribute(attribute);
            auto matches = colorExpression.globalMatch(paint);
            QList<QRegularExpressionMatch> colors;
            while (matches.hasNext()) {
                colors.append(matches.next());
            }
            for (auto it = colors.crbegin(); it != colors.crend(); ++it)
                paint.replace(it->capturedStart(), it->capturedLength(), themedPaint(QColor(it->captured())));
            matches = gradientExpression.globalMatch(paint);
            colors.clear();
            while (matches.hasNext()) colors.append(matches.next());
            for (auto it = colors.crbegin(); it != colors.crend(); ++it) {
                if (gradients.contains(it->captured(1)))
                    paint.replace(it->capturedStart(), it->capturedLength(), gradients.value(it->captured(1)));
            }
            if (attribute == "style")
                paint.replace(QRegularExpression("font-family:[^;]+"), "font-family:" + QApplication::font().family());
            if (!paint.isEmpty()) element.setAttribute(attribute, paint);
        }
        if (!emphasizeRotation) continue;
        QString style = element.attribute("style");
        // Keep the original arrow geometry and rotation direction, with a quiet
        // rotor disk behind fully opaque, outlined arrows.
        style.replace(QRegularExpression("(?<![-\\w])opacity:0\\.5\\b"), "opacity:0.16");
        style.replace(QRegularExpression("(?<![-\\w])opacity:0\\.69999701\\b"), "opacity:1");
        if (element.tagName() == "path") {
            if (originalStyle.contains("fill:url("))
                style += QString(";stroke:%1;stroke-width:1.5;stroke-linejoin:round")
                    .arg(palette.color(QPalette::Base).name());
            if (originalStyle.contains("#48484a") || originalStyle.contains("#100f0d"))
                style = QString("fill:%1;stroke:%2;stroke-width:1.5")
                    .arg(palette.color(QPalette::Base).name(), palette.color(QPalette::WindowText).name());
            else if (originalStyle.contains("#2e2d2d") || originalStyle.contains("#2f2d2e"))
                style.replace(palette.color(QPalette::WindowText).name(), palette.color(QPalette::Mid).name());
            else if (originalStyle.contains("fill:#ffffff"))
                style = QString("fill:%1;stroke:%1;stroke-width:0.5;stroke-linejoin:round")
                    .arg(palette.color(QPalette::WindowText).name());
        }
        element.setAttribute("style", style);
        if (element.tagName() == "text") {
            const QString label = element.text().trimmed();
            const auto span = element.firstChildElement("tspan");
            if (span.isNull()) continue;
            QFont originalFont = QApplication::font();
            originalFont.setPixelSize(20);
            originalFont.setBold(true);
            const qreal center = span.attribute("x").toDouble()
                + QFontMetricsF(originalFont).horizontalAdvance(label) / 2;
            element.setAttribute("x", QString::number(center));
            element.setAttribute("y", span.attribute("y"));
            element.setAttribute("style", QString("font-family:%1;font-size:24px;font-weight:bold;"
                "text-anchor:middle;fill:%2;stroke:none")
                .arg(QApplication::font().family(), palette.color(QPalette::WindowText).name()));
            // Qt's SVG renderer offsets nested text spans in these legacy files.
            // A single text node puts each label back in the center of its hub.
            while (!element.firstChild().isNull()) element.removeChild(element.firstChild());
            element.appendChild(document.createTextNode(label));
        }
    }
    return document.toByteArray();
}
