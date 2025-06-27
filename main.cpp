#include "chatwindow.h"
#include <QApplication>
#include <QMediaDevices>
#include <QDebug>
#include <QDir>
#include <QMessageBox>

int main(int argc, char *argv[])
{
    qputenv("QT_MEDIA_BACKEND", "windows");

    QApplication a(argc, argv);
    QString appDir = QApplication::applicationDirPath();
    QCoreApplication::addLibraryPath(appDir + "/plugins");
    QCoreApplication::addLibraryPath(appDir + "/multimedia");
    QCoreApplication::addLibraryPath(appDir + "/networkinformation");
    QCoreApplication::addLibraryPath(appDir + "/iconengines");
    QCoreApplication::addLibraryPath(appDir + "/imageformats");
    QCoreApplication::addLibraryPath(appDir + "/platforms");
    QCoreApplication::addLibraryPath(appDir + "/generic");
    QCoreApplication::addLibraryPath(appDir + "/mediaservice");



    ChatWindow w;
    w.show();

    return a.exec();
}
