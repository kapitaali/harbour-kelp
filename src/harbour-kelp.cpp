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
#include <QVariantMap>
#include <QDebug>

#include "kelpbundles.h"
#include "kelpbuild.h"
#include "kelpclient.h"
#include "kelpdevicestate.h"
#include "kelpfiles.h"
#include "kelphandoff.h"
#include "kelplog.h"
#include "kelppolicy.h"
#include "kelpqr.h"
#include "kelppreviewserver.h"
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

    // Scheme activation (META-INF/scheme-plan.md). The system starts this process
    // with the kelp:// URL when the scheme is opened in another app. If an
    // instance is already serving, hand the link over and exit without a window;
    // otherwise this process becomes the instance. The socket is bound before the
    // UI exists so a second activation during startup still finds it.
    KelpHandoff handoff;
    const QString schemeLink = KelpHandoff::linkFromArguments(QCoreApplication::arguments());
    bool mayReplaceStale = false;
    if (!schemeLink.isEmpty()) {
        QString handoffError;
        const KelpHandoff::Result sent = handoff.forward(schemeLink, &handoffError);
        if (sent == KelpHandoff::Sent) {
            qInfo() << "Kelp: handed the link to the running instance";
            return 0;
        }
        if (sent == KelpHandoff::Unresponsive) {
            // Never start beside a live instance: two of them rewrite shares.json.
            qWarning() << "Kelp: an instance is running but did not take the link:"
                       << handoffError;
            return 1;
        }
        mayReplaceStale = true;          // NoServer/Refused: nobody is serving
    } else if (handoff.instanceRunning()) {
        qInfo() << "Kelp: another instance is already running";
        return 0;
    } else {
        mayReplaceStale = true;          // no socket, or a stale one
    }

    QString handoffError;
    if (!handoff.listen(mayReplaceStale, &handoffError)) {
        // Lost the race to an activation that bound the socket first. Forward to
        // the winner instead of running beside it.
        if (!schemeLink.isEmpty()
                && handoff.forward(schemeLink) == KelpHandoff::Sent) {
            qInfo() << "Kelp: handed the link to the instance that started first";
            return 0;
        }
        qWarning() << "Kelp: cannot take the link socket:" << handoffError;
        return 1;
    }

    QScopedPointer<QQuickView> view(SailfishApp::createView());

    KelpFiles kelpFiles;
    KelpLog kelpLog;
    KelpSettings kelpSettings;

    // The serving policy (design §12.8): what the device's connection and
    // battery look like right now, and whether that means this device may serve
    // at all. It reads sysfs/procfs (a sandboxed app may not ask connman or
    // MCE), polls, and holds no state the UI does not act on itself.
    KelpDeviceState kelpDeviceState;
    KelpPolicy kelpPolicy(&kelpSettings, &kelpDeviceState);

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

    // The loopback preview (design §5.1): when a browser on *this* device is shown
    // a page from a bundle this device holds, the app decrypts and serves it on
    // 127.0.0.1. It binds nothing until a page is actually opened, so a launch that
    // never previews opens no port at all. One listener serves both kinds of page:
    // a share's own (served from the app store) and a *received* webpage link
    // (served from the memory store KelpClient fetches into — design §5.5).
    KelpPreviewServer previewServer;

    KelpBundles kelpBundles(&objectStore, &objectServer);
    // The publisher acts on the log-links preference; hand it the same object
    // the Settings page writes to.
    kelpBundles.setSettings(&kelpSettings);
    kelpBundles.setPreviewServer(&previewServer);

    // The receiving end shares the same object store: an object received for one
    // bundle is verified content like any other, and nothing here can be confused
    // with a sealed one (different key, different ids).
    KelpClient kelpClient(&objectStore);
    // A webpage link is viewed, never saved (design §5.5): its fetch goes into a
    // memory store of the client's own and is served through this same preview
    // listener. The settings are for the one thing it reads from them — whether a
    // capability URL may be written to the log.
    kelpClient.setPreviewServer(&previewServer);
    kelpClient.setSettings(&kelpSettings);
    // The chosen download folder is remembered; an empty setting means the
    // platform's Downloads folder, which KelpClient falls back to by itself.
    kelpClient.setDirectory(kelpSettings.downloadDirectory());

    // A link handed to the app on the command line, or in KELP_INITIAL_LINK, is
    // offered to the Open-a-link page at startup. It is how a link reaches a
    // device with no usable clipboard (the emulator over ssh), and it is the same
    // seam a kelp:// scheme activation uses: the validated argument above.
    QString initialLink = QString::fromLocal8Bit(qgetenv("KELP_INITIAL_LINK")).trimmed();
    if (initialLink.isEmpty())
        initialLink = schemeLink;
    if (!initialLink.isEmpty())
        qInfo() << "Kelp: opening a link given at startup";

    QQmlContext *context = view->rootContext();
    context->setContextProperty("initialLink", initialLink);

    // The dev-extras bundle (src/kelpbuild.h) as one object, so QML asks by name
    // and a feature added to the bundle needs no new context property. Also logged
    // once: which extras a build carries is otherwise invisible, and "is this the
    // build I think it is" is exactly what a test wants to know.
    QVariantMap features;
    features.insert(QStringLiteral("devExtras"), KelpBuild::devExtras());
    context->setContextProperty("kelpFeatures", features);
    qInfo() << "Kelp: build mode — dev extras" << (KelpBuild::devExtras() ? "on" : "off");

    context->setContextProperty("kelpFiles", &kelpFiles);
    context->setContextProperty("kelpLog", &kelpLog);
    context->setContextProperty("kelpSettings", &kelpSettings);
    context->setContextProperty("kelpPolicy", &kelpPolicy);
    context->setContextProperty("kelpShares", &kelpShares);
    context->setContextProperty("kelpBundles", &kelpBundles);
    context->setContextProperty("kelpClient", &kelpClient);
    context->setContextProperty("appVersion", QStringLiteral(APP_VERSION));
    context->setContextProperty("buildId", QStringLiteral(BUILD_ID));

    // The QR code for a share link is a painted item, so it is a QML type rather
    // than a context property (the page instantiates it).
    qmlRegisterType<KelpQrCode>("harbour.kelp", 1, 0, "KelpQrCode");

    // A link that arrives after startup is a tap in another app. It goes to the
    // same page the startup argument would have opened — prefilled, never opened
    // by itself (scheme-plan.md, rule 1). Queued so the page push happens on a
    // settled event loop, and buffered for the one moment before the root object
    // exists.
    QString pendingLink;
    QQuickView *const window = view.data();
    QObject::connect(&handoff, &KelpHandoff::linkArrived, window,
                     [window, &pendingLink](const QString &link) {
                         if (!window->rootObject()) {
                             pendingLink = link;
                             return;
                         }
                         window->requestActivate();
                         // Only the fact, never the link: its fragment is the key.
                         qInfo() << "Kelp: a link was handed over by another app";
                         QMetaObject::invokeMethod(window->rootObject(),
                                                   "openLink",
                                                   Q_ARG(QVariant, link));
                     },
                     Qt::QueuedConnection);

    view->setSource(SailfishApp::pathTo("qml/harbour-kelp.qml"));

    // QQuickView shows an empty window on load failure without reporting
    // errors itself — print them here or the failure is invisible.
    if (view->status() == QQuickView::Error) {
        const QList<QQmlError> errors = view->errors();
        for (int i = 0; i < errors.count(); ++i)
            qWarning() << errors.at(i).toString();
        qWarning() << "Failed to load" << view->source();
    }

    if (!pendingLink.isEmpty()) {
        QMetaObject::invokeMethod(view->rootObject(), "openLink",
                                  Q_ARG(QVariant, pendingLink));
        pendingLink.clear();
    }

    view->showFullScreen();

    const int result = app->exec();
    qInfo() << "Kelp exiting with code" << result;
    return result;
}
