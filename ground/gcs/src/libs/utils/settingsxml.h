#ifndef UTILS_SETTINGSXML_H
#define UTILS_SETTINGSXML_H

#include "utils_global.h"
#include <QString>

class QDomDocument;

namespace Utils {
// Sort settings objects by name and attributes by name. Preserve data-object
// and field order, metadata, values, and descriptions.
QTCREATOR_UTILS_EXPORT QString serializeUavSettings(const QDomDocument &document);
}

#endif
