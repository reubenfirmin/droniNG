/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "atomfeedmodel.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QXmlStreamReader>

namespace Welcome {

AtomFeedModel::AtomFeedModel(QObject *parent)
    : QAbstractListModel(parent)
{}

AtomFeedModel::~AtomFeedModel()
{
    if (m_reply) {
        disconnect(m_reply, nullptr, this, nullptr);
        m_reply->abort();
    }
}

int AtomFeedModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_entries.size();
}

QVariant AtomFeedModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.column() != 0 || index.row() < 0 || index.row() >= m_entries.size())
        return {};
    const Entry &entry = m_entries.at(index.row());
    switch (role) {
    case TitleRole: return entry.title;
    case AuthorRole: return entry.author;
    case AuthorUriRole: return entry.authorUri;
    case LinkRole: return entry.link;
    case ThumbnailRole: return entry.thumbnail;
    default: return {};
    }
}

QHash<int, QByteArray> AtomFeedModel::roleNames() const
{
    return {{TitleRole, "title"}, {AuthorRole, "author"}, {AuthorUriRole, "authoruri"},
            {LinkRole, "link"}, {ThumbnailRole, "thumbnail"}};
}

void AtomFeedModel::replaceEntries(const QList<Entry> &entries, const QString &error)
{
    beginResetModel();
    m_entries = entries;
    endResetModel();
    if (m_errorString != error) {
        m_errorString = error;
        emit errorStringChanged();
    }
}

void AtomFeedModel::setSource(const QUrl &source)
{
    if (source == m_source)
        return;
    if (m_reply) {
        disconnect(m_reply, nullptr, this, nullptr);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    m_source = source;
    replaceEntries({}, {});
    emit sourceChanged();
    if (source.isEmpty())
        return;

    QNetworkRequest request(source);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(15000);
    QNetworkReply *reply = m_network.get(request);
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (m_reply != reply) {
            reply->deleteLater();
            return;
        }
        m_reply = nullptr;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool success = reply->error() == QNetworkReply::NoError
            && (m_source.isLocalFile() || (status >= 200 && status < 300));
        QList<Entry> entries;
        const bool valid = success && parse(reply->readAll(), entries);
        reply->deleteLater();
        replaceEntries(valid ? entries : QList<Entry>(), valid ? QString() : tr("Results unavailable"));
    });
}

bool AtomFeedModel::parse(const QByteArray &xml, QList<Entry> &entries) const
{
    QXmlStreamReader reader(xml);
    const QString atom = QStringLiteral("http://www.w3.org/2005/Atom");
    const QString media = QStringLiteral("http://search.yahoo.com/mrss/");
    if (!reader.readNextStartElement() || reader.name() != QLatin1String("feed")
        || reader.namespaceUri() != atom)
        return false;

    auto resolve = [this](const QString &value) {
        return value.isEmpty() ? QString() : m_source.resolved(QUrl(value)).toString();
    };
    while (reader.readNextStartElement()) {
        if (reader.name() != QLatin1String("entry") || reader.namespaceUri() != atom) {
            reader.skipCurrentElement();
            continue;
        }
        Entry entry;
        while (reader.readNextStartElement()) {
            if (reader.namespaceUri() == atom && reader.name() == QLatin1String("title")) {
                entry.title = reader.readElementText(QXmlStreamReader::SkipChildElements);
            } else if (reader.namespaceUri() == atom && reader.name() == QLatin1String("author")) {
                while (reader.readNextStartElement()) {
                    if (reader.namespaceUri() == atom && reader.name() == QLatin1String("name"))
                        entry.author = reader.readElementText(QXmlStreamReader::SkipChildElements);
                    else if (reader.namespaceUri() == atom && reader.name() == QLatin1String("uri"))
                        entry.authorUri = resolve(reader.readElementText());
                    else
                        reader.skipCurrentElement();
                }
            } else if (reader.namespaceUri() == atom && reader.name() == QLatin1String("link")) {
                const auto attributes = reader.attributes();
                const QString rel = attributes.value(QLatin1String("rel")).toString();
                if (entry.link.isEmpty() && (rel.isEmpty() || rel == QLatin1String("alternate")))
                    entry.link = resolve(attributes.value(QLatin1String("href")).toString());
                reader.skipCurrentElement();
            } else if (reader.namespaceUri() == media && reader.name() == QLatin1String("thumbnail")) {
                entry.thumbnail = resolve(reader.attributes().value(QLatin1String("url")).toString());
                reader.skipCurrentElement();
            } else {
                reader.skipCurrentElement();
            }
        }
        if (!entry.title.contains(QLatin1String("Merge pull request")))
            entries.append(entry);
    }
    // Consume the complete document so malformed tails cannot produce partial rows.
    while (!reader.atEnd())
        reader.readNext();
    return !reader.hasError();
}

} // namespace Welcome
