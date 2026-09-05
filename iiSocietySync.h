#pragma once

#include <QString>
#include <QtGlobal>

#if defined(IISOCIETYSYNC_BUILDING_LIBRARY)
#  define IISOCIETYSYNC_EXPORT Q_DECL_EXPORT
#else
#  define IISOCIETYSYNC_EXPORT Q_DECL_IMPORT
#endif

namespace iiSocietySync {

[[nodiscard]] IISOCIETYSYNC_EXPORT QString helloWorld();

} // namespace iiSocietySync
