/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef USBACCESSSETUP_H
#define USBACCESSSETUP_H
#include "core_global.h"
#include <QObject>
#include <QStringList>
namespace Core {
class CORE_EXPORT UsbAccessSetup : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool required READ required NOTIFY statusChanged)
    Q_PROPERTY(QString title READ title NOTIFY statusChanged)
    Q_PROPERTY(QString message READ message NOTIFY statusChanged)
    Q_PROPERTY(QString command READ command NOTIFY statusChanged)
    Q_PROPERTY(bool copied READ copied NOTIFY copiedChanged)
public:
    enum class State { Ready, InstallRules, Reconnect, MissingRules };
    Q_ENUM(State)
    struct Paths {
        QString bundledRules;
        // In increasing precedence, matching udev's rule-directory overrides.
        QStringList ruleDirectories;
        QString usbDevices;
        QString usbNodes;
    };
    explicit UsbAccessSetup(QObject *parent = nullptr);
    UsbAccessSetup(const Paths &paths, QObject *parent = nullptr);
    static State inspect(const Paths &paths);
    State state() const { return m_state; }
    bool required() const { return m_state != State::Ready; }
    QString title() const;
    QString message() const;
    QString command() const;
    bool copied() const { return m_copied; }
    Q_INVOKABLE bool copyCommand();
public slots:
    void refresh();
signals:
    void statusChanged();
    void copiedChanged();
private:
    Paths m_paths;
    State m_state = State::Ready;
    bool m_copied = false;
};
}
#endif
