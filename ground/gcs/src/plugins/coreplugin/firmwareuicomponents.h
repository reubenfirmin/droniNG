/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef FIRMWAREUICOMPONENTS_H
#define FIRMWAREUICOMPONENTS_H
#include <QFrame>
#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>

namespace Core::FirmwareUi {
inline QLabel *label(const QString &text, const char *role, QWidget *parent)
{
    auto *result = new QLabel(text, parent);
    result->setProperty("role", role);
    result->setTextFormat(Qt::PlainText);
    result->setWordWrap(true);
    return result;
}
inline QFrame *card(QWidget *parent)
{
    auto *result = new QFrame(parent);
    result->setProperty("uiCard", true);
    auto *layout = new QVBoxLayout(result);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);
    return result;
}
}
#endif
