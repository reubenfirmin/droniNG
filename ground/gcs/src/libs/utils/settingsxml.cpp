#include "settingsxml.h"
#include <QDomDocument>
#include <QXmlStreamWriter>
#include <algorithm>

namespace {
void writeNode(QXmlStreamWriter &writer, const QDomNode &node)
{
    if (node.isElement()) {
        const QDomElement element = node.toElement();
        writer.writeStartElement(element.tagName());
        const QDomNamedNodeMap attributes = element.attributes();
        QStringList names;
        for (int i = 0; i < attributes.size(); ++i)
            names.append(attributes.item(i).nodeName());
        names.sort(Qt::CaseSensitive);
        for (const QString &name : names)
            writer.writeAttribute(name, element.attribute(name));

        QList<QDomNode> settingsObjects;
        if (element.tagName() == QStringLiteral("settings")) {
            for (QDomNode child = node.firstChild(); !child.isNull(); child = child.nextSibling()) {
                if (child.isElement() && child.nodeName() == QStringLiteral("object"))
                    settingsObjects.append(child);
            }
            std::stable_sort(settingsObjects.begin(), settingsObjects.end(),
                             [](const QDomNode &left, const QDomNode &right) {
                return left.toElement().attribute("name") < right.toElement().attribute("name");
            });
        }
        int objectIndex = 0;
        for (QDomNode child = node.firstChild(); !child.isNull(); child = child.nextSibling()) {
            if (!settingsObjects.isEmpty() && child.isElement()
                    && child.nodeName() == QStringLiteral("object"))
                writeNode(writer, settingsObjects.at(objectIndex++));
            else
                writeNode(writer, child);
        }
        writer.writeEndElement();
    } else if (node.isText() || node.isCDATASection()) {
        if (!node.nodeValue().trimmed().isEmpty())
            writer.writeCharacters(node.nodeValue());
    } else if (node.isComment()) {
        writer.writeComment(node.nodeValue());
    } else if (node.isProcessingInstruction() && node.nodeName() != QStringLiteral("xml")) {
        writer.writeProcessingInstruction(node.nodeName(), node.nodeValue());
    }
}
}

QString Utils::serializeUavSettings(const QDomDocument &document)
{
    QByteArray output;
    QXmlStreamWriter writer(&output);
    writer.setAutoFormatting(true);
    writer.setAutoFormattingIndent(4);
    writer.writeStartDocument();
    for (QDomNode node = document.firstChild(); !node.isNull(); node = node.nextSibling())
        writeNode(writer, node);
    writer.writeEndDocument();
    return QString::fromUtf8(output);
}
