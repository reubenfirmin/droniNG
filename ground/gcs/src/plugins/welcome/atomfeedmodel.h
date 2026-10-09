/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QAbstractListModel>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QUrl>

class QNetworkReply;

namespace Welcome {

// The Welcome feed needs Atom namespaces and merge filtering, which are not
// supported by the Qt 6 replacement for the old XPath-based XmlListModel.
class AtomFeedModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)
public:
    enum Role { TitleRole = Qt::UserRole + 1, AuthorRole, AuthorUriRole, LinkRole, ThumbnailRole };
    explicit AtomFeedModel(QObject *parent = nullptr);
    ~AtomFeedModel() override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    QUrl source() const { return m_source; }
    void setSource(const QUrl &source);
    QString errorString() const { return m_errorString; }

signals:
    void sourceChanged();
    void errorStringChanged();

private:
    struct Entry {
        QString title, author, authorUri, link, thumbnail;
    };
    bool parse(const QByteArray &xml, QList<Entry> &entries) const;
    void replaceEntries(const QList<Entry> &entries, const QString &error);
    QUrl m_source;
    QString m_errorString;
    QList<Entry> m_entries;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
};

} // namespace Welcome
