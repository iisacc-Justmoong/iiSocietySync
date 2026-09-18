#pragma once
#include <QtGlobal>
#if defined(IISOCIETYSYNC_BUILDING_LIBRARY)
#  define IISOCIETYSYNC_EXPORT Q_DECL_EXPORT
#else
#  define IISOCIETYSYNC_EXPORT Q_DECL_IMPORT
#endif
