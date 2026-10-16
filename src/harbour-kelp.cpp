/*
 * Application entry point.
 *
 * Wires QML to the real things: the content layer (libkelp) with its on-disk
 * object store, the filesystem helper behind the Add page's sizes, and the file
 * logger. The bundle list still lives in qml/harbour-kelp.qml and is not
 * persisted yet, so restarting forgets the links (the objects stay).
 */

#ifdef QT_QML_DEBUG
#include <QtQuick>
#endif

#include <sailfishapp.h>
#include <QDir>
#include <QGuiApplication>
#include <QQmlContext>
#include <QQmlError>
#include <QQmlEngine>
#include <QQuickView>
#include <QScopedPointer>
#include <QStandardPaths>
#include <QDebug>

#include "kelpbundles.h"
#include "kelpclient.h"
#include "kelpfiles.h"
#include "kelplog.h"
#include "kelpqr.h"
#include "kelpserver.h"
#include "kelpsettings.h"
#include "kelpshares.h"
#include "kelpstore.h"

int main(int argc, char *argv[])
{
    QScopedPointer<QGuiApplication> app(SailfishApp::application(argc, argv));

    // The organization/application names decide where QStandardPaths puts our
    // data, and Sailjail provisions exactly that directory and hides the rest of
    // ~/.local/share. They must be set before the first path is asked for.
    QCoreApplication::setOrganizationName(QStringLiteral("harbour.kelp"));
    QCoreApplication::setApplicationName(QStringLiteral("harbour-kelp"));

    // Install first: this way any QML load error that follows is captured in
    // the log file, not only on stderr.
    KelpLog::install();

    QScopedPointer<QQuickView> view(SailfishApp::createView());

    KelpFiles kelpFiles;
    KelpLog kelpLog;
    KelpSettings kelpSettings;
    // The saved share list, restored by the UI at startup so a restart no longer
    // drops a share (the objects were already staying on disk).
    KelpShares kelpShares;

    // Objects live beside the log, in the app's own data directory — the one
    // Sailjail gives us, so this works from the app grid as well as a shell.
    const QString dataDirectory = KelpLog::dataDirectory();

    KelpObjectStore objectStore(dataDirectory + QStringLiteral("/objects"));
    QString storeError;
    if (!objectStore.initialise(&storeError))
        qWarning() << "Kelp: object store is unavailable:" << storeError;
    else
        qInfo() << "Kelp: object store at" << objectStore.rootPath()
                << "with" << objectStore.objectCount() << "objects";

    // The listener starts lazily, on the first bundle that is actually shared.
    KelpServer objectServer(&objectStore);

    KelpBundles kelpBundles(&objectStore, &objectServer);
    // The publisher acts on the log-links preference; hand it the same object
    // the Settings page writes to.
    kelpBundles.setSettings(&kelpSettings);

    // The receiving end shares the same object store: an object received for one
    // bundle is verified content like any other, and nothing here can be confused
    // with a sealed one (different key, different ids).
    KelpClient kelpClient(&objectStore);
    // The chosen download folder is remembered; an empty setting means the
    // platform's Downloads folder, which KelpClient falls back to by itself.
    kelpClient.setDirectory(kelpSettings.downloadDirectory());

    // A link handed to the app on the command line, or in KELP_INITIAL_LINK, is
    // offered to the Open-a-link page at startup. It is how a link reaches a
    // device with no usable clipboard (the emulator over ssh), and it is the
    // same seam a kelp:// scheme activation will use.
    QString initialLink = QString::fromLocal8Bit(qgetenv("KELP_INITIAL_LINK")).trimmed();
    for (int i = 1; i < argc && initialLink.isEmpty(); ++i) {
        const QString argument = QString::fromLocal8Bit(argv[i]);
        if (argument.startsWith(QStringLiteral("kelp://"), Qt::CaseInsensitive))
            initialLink = argument.trimmed();
    }
    if (!initialLink.isEmpty())
        qInfo() << "Kelp: opening a link given at startup";

    QQmlContext *context = view->rootContext();
    context->setContextProperty("initialLink", initialLink);
    context->setContextProperty("kelpFiles", &kelpFiles);
    context->setContextProperty("kelpLog", &kelpLog);
    context->setContextProperty("kelpSettings", &kelpSettings);
    context->setContextProperty("kelpShares", &kelpShares);
    context->setContextProperty("kelpBundles", &kelpBundles);
    context->setContextProperty("kelpClient", &kelpClient);
    context->setContextProperty("appVersion", QStringLiteral(APP_VERSION));
    context->setContextProperty("buildId", QStringLiteral(BUILD_ID));

    // The QR code for a share link is a painted item, so it is a QML type rather
    // than a context property (the page instantiates it).
    qmlRegisterType<KelpQrCode>("harbour.kelp", 1, 0, "KelpQrCode");

    view->setSource(SailfishApp::pathTo("qml/harbour-kelp.qml"));

    // QQuickView shows an empty window on load failure without reporting
    // errors itself — print them here or the failure is invisible.
    if (view->status() == QQuickView::Error) {
        const QList<QQmlError> errors = view->errors();
        for (int i = 0; i < errors.count(); ++i)
            qWarning() << errors.at(i).toString();
        qWarning() << "Failed to load" << view->source();
    }

    view->showFullScreen();

    const int result = app->exec();
    qInfo() << "Kelp exiting with code" << result;
    return result;
}
