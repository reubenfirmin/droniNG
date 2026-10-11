// SPDX-License-Identifier: GPL-3.0-or-later
#include "darktheme.h"

#include <QApplication>
#include <QFile>
#include <QPalette>
#include <QStyleFactory>

void Utils::applyDarkTheme()
{
    // Fusion ships with Qt, so packaged builds need no desktop style plugin.
    // Colours follow KDE Breeze Dark's Window, View, Button and Selection roles:
    // https://github.com/KDE/breeze/blob/master/colors/BreezeDark.colors
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QPalette palette = QApplication::palette();
    palette.setColor(QPalette::Window, QColor(32, 35, 38));
    palette.setColor(QPalette::WindowText, QColor(252, 252, 252));
    palette.setColor(QPalette::Base, QColor(20, 22, 24));
    palette.setColor(QPalette::AlternateBase, QColor(29, 31, 34));
    palette.setColor(QPalette::Text, QColor(252, 252, 252));
    palette.setColor(QPalette::Button, QColor(41, 44, 48));
    palette.setColor(QPalette::ButtonText, QColor(252, 252, 252));
    palette.setColor(QPalette::ToolTipBase, QColor(41, 44, 48));
    palette.setColor(QPalette::ToolTipText, QColor(252, 252, 252));
    palette.setColor(QPalette::Light, QColor(90, 97, 105));
    palette.setColor(QPalette::Midlight, QColor(49, 54, 59));
    palette.setColor(QPalette::Mid, QColor(69, 75, 82));
    palette.setColor(QPalette::Dark, QColor(20, 22, 24));
    palette.setColor(QPalette::Shadow, QColor(12, 13, 14));
    palette.setColor(QPalette::BrightText, QColor(252, 252, 252));
    const QColor accent(120, 168, 255);
    palette.setColor(QPalette::Highlight, accent);
    palette.setColor(QPalette::HighlightedText, QColor(20, 22, 24));
    palette.setColor(QPalette::Link, accent);
    palette.setColor(QPalette::LinkVisited, accent);
    const QColor muted(161, 169, 177);
    palette.setColor(QPalette::PlaceholderText, muted);
    palette.setColor(QPalette::Accent, accent);
    for (QPalette::ColorRole role : {QPalette::WindowText, QPalette::Text,
                                    QPalette::ButtonText, QPalette::PlaceholderText})
        palette.setColor(QPalette::Disabled, role, muted);
    palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor(49, 54, 59));
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, QColor(127, 134, 142));
    QApplication::setPalette(palette);
    QFile stylesheet(QStringLiteral(":/utils/application.qss"));
    if (stylesheet.open(QIODevice::ReadOnly)) {
        QString style = QString::fromUtf8(stylesheet.readAll());
        style.replace(QStringLiteral("@muted@"), palette.color(QPalette::PlaceholderText).name());
        style.replace(QStringLiteral("@highlightedText@"), palette.color(QPalette::HighlightedText).name());
        qApp->setStyleSheet(style);
    } else {
        qFatal("The application stylesheet is missing.");
    }
}
