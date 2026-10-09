// SPDX-License-Identifier: GPL-3.0-or-later
#include "darktheme.h"

#include <QApplication>
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
    palette.setColor(QPalette::Highlight, QColor(61, 174, 233));
    palette.setColor(QPalette::HighlightedText, QColor(20, 22, 24));
    palette.setColor(QPalette::Link, QColor(61, 174, 233));
    palette.setColor(QPalette::LinkVisited, QColor(188, 132, 214));
    palette.setColor(QPalette::PlaceholderText, QColor(161, 169, 177));
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    palette.setColor(QPalette::Accent, QColor(61, 174, 233));
#endif
    for (QPalette::ColorRole role : {QPalette::WindowText, QPalette::Text,
                                    QPalette::ButtonText, QPalette::PlaceholderText})
        palette.setColor(QPalette::Disabled, role, QColor(127, 134, 142));
    palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor(49, 54, 59));
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, QColor(127, 134, 142));
    QApplication::setPalette(palette);
}
