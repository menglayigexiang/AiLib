#pragma once

#include <QtCore/qglobal.h>

#if defined(LIBMCP_SHARED)
#  if defined(LIBMCP_LIBRARY)
#    define LIBMCP_EXPORT Q_DECL_EXPORT
#  else
#    define LIBMCP_EXPORT Q_DECL_IMPORT
#  endif
#else
#  define LIBMCP_EXPORT
#endif

/// 最新受支持的 MCP 协议版本。
#define LIBMCP_PROTOCOL_VERSION "2026-07-28"
