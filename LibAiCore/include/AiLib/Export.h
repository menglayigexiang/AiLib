#pragma once

#include <QtCore/qglobal.h>

#if defined(AILIB_CORE_BUILD)
#  define AILIB_EXPORT Q_DECL_EXPORT
#else
#  define AILIB_EXPORT Q_DECL_IMPORT
#endif
