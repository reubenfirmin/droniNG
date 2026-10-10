/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "qtlogging.h"
#include <QRegularExpression>
#include <QString>
#include <atomic>
#include <cstring>

namespace {
std::atomic<QtMessageHandler> outputHandler{nullptr};

void filteredMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
#if QT_VERSION == QT_VERSION_CHECK(6, 12, 0)
    // Qt 6.12.0's QWaylandInputContext::setFocusObject calls disableSurface even
    // when a non-editable control gains focus and text input is already disabled.
    // QWaylandTextInputv3 warns and immediately returns in this case. Only hide
    // that no-op: other surfaces, enabled input and all other diagnostics remain
    // visible. Revisit this workaround when upgrading the SDK.
    // https://github.com/qt/qtbase/blob/v6.12.0/src/plugins/platforms/wayland/qwaylandinputcontext.cpp
    if (type == QtWarningMsg && context.category
        && std::strcmp(context.category, "qt.qpa.wayland.textinput") == 0) {
        static const QRegularExpression alreadyDisabled(QStringLiteral(
            "^virtual void QtWaylandClient::QWaylandTextInputv3::disableSurface\\(wl_surface\\*\\) "
            "Trying to disable 0x[0-9a-fA-F]+ but 0x0 ?is focused$"));
        if (alreadyDisabled.match(message).hasMatch())
            return;
    }
#endif
    if (const QtMessageHandler handler = outputHandler.load())
        handler(type, context, message);
}
}

void Utils::installQtMessageHandler(QtMessageHandler handler)
{
    // Install before QApplication construction, so this is Qt's default handler.
    // A null handler restores normal Qt output while retaining the narrow filter.
    static const QtMessageHandler defaultHandler = qInstallMessageHandler(nullptr);
    outputHandler.store(handler ? handler : defaultHandler);
    qInstallMessageHandler(filteredMessageHandler);
}
