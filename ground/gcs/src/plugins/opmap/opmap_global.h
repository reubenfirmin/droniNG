/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <QtCore/qglobal.h>
#if defined(OPMAP_LIBRARY)
#define OPMAP_EXPORT Q_DECL_EXPORT
#else
#define OPMAP_EXPORT Q_DECL_IMPORT
#endif
