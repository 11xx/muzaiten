#include "app/MuzaitenApplication.h"

#include "Version.h"
#include "ui/MainWindow.h"

#include <QCoreApplication>

MuzaitenApplication::MuzaitenApplication(int &argc, char **argv)
    : QApplication(argc, argv)
{
    QCoreApplication::setApplicationName(QStringLiteral(MUZAITEN_APP_NAME));
    QCoreApplication::setApplicationVersion(QStringLiteral(MUZAITEN_VERSION));
    QCoreApplication::setOrganizationName(QStringLiteral("muzaiten"));
    QCoreApplication::setOrganizationDomain(QStringLiteral(MUZAITEN_APP_ID));
}

int MuzaitenApplication::run()
{
    MainWindow window;
    window.show();
    return exec();
}

