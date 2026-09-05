#include <iiLocalLLM.h>

#include <QtCore/QString>
#include <QtCore/QtGlobal>
#include <iostream>

static_assert(__cplusplus >= 202002L, "C++20 or newer is required");
static_assert(QT_VERSION == QT_VERSION_CHECK(6, 8, 3), "Qt 6.8.3 is required");

int main()
{
    if (QString::fromLatin1(qVersion()) != QStringLiteral("6.8.3")) {
        std::cerr << "Unexpected Qt runtime: " << qVersion() << '\n';
        return 1;
    }
    const QString message = iiLocalLLM::helloWorld();
    std::cout << message.toStdString() << '\n';
    return message == QStringLiteral("Hello world!") ? 0 : 1;
}
