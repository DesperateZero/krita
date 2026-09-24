#ifndef SIMPLETEST_H
#define SIMPLETEST_H

#include <QTest>
#include <QStandardPaths>
#include <QLocale>
// #include <KLocalizedString>
#include <KoTestConfig.h>
#include <KisSynchronizedConnection.h>

// Out-of-tree tests may use an installed dependency prefix but freshly built
// Krita plugins. Keep the configured default; never silently mix builds or
// overwrite an explicit test-only selection with the install directory.
inline void configureKritaTestPluginPath()
{
    const QByteArray testPlugins = qgetenv("KIS_TEST_PLUGIN_PATH");
    qputenv("KRITA_PLUGIN_PATH", testPlugins.isEmpty()
        ? QByteArray(KRITA_PLUGINS_DIR_FOR_TESTS) : testPlugins);
}

#define SIMPLE_MAIN_IMPL(TestObject) \
    qputenv("LANGUAGE", "en"); \
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates)); \
    QStandardPaths::setTestModeEnabled(true); \
    KisSynchronizedConnectionBase::setAutoModeForUnittestsEnabled(true); \
    qputenv("EXTRA_RESOURCE_DIRS", QByteArray(KRITA_RESOURCE_DIRS_FOR_TESTS)); \
    configureKritaTestPluginPath(); \
    QApplication app(argc, argv); \
    app.setAttribute(Qt::AA_Use96Dpi, true); \
    /*QLocale en_US(QLocale::English, QLocale::UnitedStates); \
    KLocalizedString::setLanguages(QStringList() << QStringLiteral("en_US"));*/ \
    QTEST_DISABLE_KEYPAD_NAVIGATION \
    TestObject tc; \
    QTEST_SET_MAIN_SOURCE_PATH \
    return QTest::qExec(&tc, argc, argv);

#define SIMPLE_TEST_MAIN(TestObject) \
int main(int argc, char *argv[]) \
{ \
    QStandardPaths::setTestModeEnabled(true); \
    SIMPLE_MAIN_IMPL(TestObject) \
}

#endif // SIMPLETEST_H
