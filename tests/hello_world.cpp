#include "iiSocietySync.h"

#include <QTextStream>
#include <QtGlobal>

#include <cstdlib>
#include <cstring>

static_assert(__cplusplus >= 202002L, "This SDK requires C++20.");
static_assert(QT_VERSION == QT_VERSION_CHECK(6, 8, 3), "This SDK requires Qt 6.8.3.");

int main()
{
    if (std::strcmp(qVersion(), "6.8.3") != 0) {
        QTextStream(stderr) << "Unexpected Qt runtime: " << qVersion() << Qt::endl;
        return EXIT_FAILURE;
    }
    const QString actual = iiSocietySync::helloWorld();
    QTextStream(stdout) << actual << Qt::endl;
    if (actual != QStringLiteral("Hello world!")) {
        QTextStream(stderr) << "Expected Hello world!" << Qt::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
