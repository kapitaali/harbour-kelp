// Writes a fixture for the gateway's JavaScript tests (design §5.2, C4b-plan.md).
//
// The browser half of the gateway re-implements, in JavaScript, three things
// libkelp already does: the object envelope, the two HKDF derivations, and the
// §5.3 page convention (`Kelp::Web`). Reading one format in two languages is
// exactly where a silent divergence costs a recipient a file, so the JavaScript
// is checked against *this* — real seals, minted by the C++ the device runs, with
// libkelp's own answers as the expected values:
//
//   * every object, so the JavaScript can decrypt what `Kelp::seal` sealed;
//   * relay_token, so the derivation can be compared byte for byte;
//   * the entry document, web root, MIME types and sizes, straight from `Kelp::Web`.
//
// Three bundles, because the convention has three answers: a page at the root, a
// page in a subfolder, and no page at all (the gateway's file list). The secret
// is synthetic — every byte 0..31 with the last byte varied — and opens nothing.
//
//   makefixture <output.json>
//
// Run inside the Sailfish build engine, from relay/gateway/test.
#include <cstdio>

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QStringList>
#include <QVector>

#include "kelp_bundle.h"
#include "kelp_crypto.h"
#include "kelp_link.h"
#include "kelp_object.h"
#include "kelp_store.h"
#include "kelp_web.h"

using namespace Kelp;

namespace {

// The plaintext of every file in one fixture bundle, keyed by the file's
// bundle-relative path — which is also the path the sealer is given.
class MemorySource : public ContentSource
{
public:
    QHash<QString, QByteArray> contents;

    bool read(const QString &path, qint64 offset, qint64 length,
              QByteArray *out, QString *error) override
    {
        if (!contents.contains(path)) {
            *error = QStringLiteral("no source named %1").arg(path);
            return false;
        }
        const QByteArray &data = contents.value(path);
        if (offset < 0 || length < 0 || offset > data.size()
            || length > data.size() - offset) {
            *error = QStringLiteral("a read past the end of %1").arg(path);
            return false;
        }
        *out = data.mid(int(offset), int(length));
        return true;
    }
};

SourceNode fileNode(const QString &name, const QString &sourcePath, const QByteArray &content)
{
    SourceNode node;
    node.name = name;
    node.path = sourcePath;         // the key the ContentSource is asked for
    node.size = content.size();
    return node;
}

SourceNode dirNode(const QString &name, const QVector<SourceNode> &children)
{
    SourceNode node;
    node.name = name;
    node.isDir = true;
    node.children = children;
    return node;
}

QByteArray b64url(const QByteArray &bytes)
{
    return Link::base64UrlEncode(bytes).toUtf8();
}

QByteArray jsonString(const QString &text)
{
    QByteArray out = text.toUtf8();
    out.replace('\\', "\\\\");
    out.replace('"', "\\\"");
    return '"' + out + '"';
}

void collectPaths(const SourceNode &node, const QString &prefix, QStringList *out)
{
    for (int i = 0; i < node.children.size(); ++i) {
        const SourceNode &child = node.children.at(i);
        const QString path = prefix.isEmpty() ? child.name : prefix + '/' + child.name;
        if (child.isDir)
            collectPaths(child, path, out);
        else
            out->append(path);
    }
}

// One sealed bundle, with everything the JavaScript side must agree about.
struct Fixture {
    QString name;
    QByteArray secret;
    SealedBundle sealed;
    QVector<QByteArray> objectIds;
    QVector<QByteArray> objects;
    Web::Page page;
    QStringList paths;                 // every file, sorted
    QVector<QString> mimes;            // Kelp::Web::mimeType's answer for each
    QVector<quint64> sizes;            // the manifest's own size
    QVector<QByteArray> digests;       // SHA-256 of each file's plaintext
};

// `source` must hold one entry per file, keyed by the file's bundle-relative
// path; the paths the tree points at are those same keys.
bool buildFixture(const QString &name, const QByteArray &secret, const SourceNode &root,
                  MemorySource *source, Fixture *out, QString *error)
{
    MemoryStore store;
    out->name = name;
    out->secret = secret;
    // The caller may hand the same Fixture back for the next bundle; nothing here
    // may depend on what is already in it.
    out->objectIds.clear();
    out->objects.clear();
    out->paths.clear();
    out->mimes.clear();
    out->sizes.clear();
    out->digests.clear();

    const QByteArray key = contentKey(secret);
    if (!seal(&store, key, root, source, &out->sealed, error))
        return false;

    if (!bundleObjects(&store, key, out->sealed.rootId, &out->objectIds, error))
        return false;
    for (int i = 0; i < out->objectIds.size(); ++i) {
        QByteArray bytes;
        if (!store.get(out->objectIds.at(i), &bytes)) {
            *error = QStringLiteral("an object the bundle names is missing");
            return false;
        }
        out->objects.append(bytes);
    }

    // The page, by libkelp's own rule: the answer the JavaScript mirror must give.
    if (!Web::page(&store, key, out->sealed.rootId, &out->page, error))
        return false;

    collectPaths(root, QString(), &out->paths);
    out->paths.sort();
    for (int i = 0; i < out->paths.size(); ++i) {
        const QString path = out->paths.at(i);
        bool found = false;
        bool isDir = false;
        QByteArray hash;
        Manifest::File file;
        if (!Web::resolve(&store, key, out->sealed.rootId, path, &found, &isDir,
                          &hash, &file, error))
            return false;
        if (!found || isDir) {
            *error = QStringLiteral("%1 is not a file in the sealed bundle").arg(path);
            return false;
        }
        out->mimes.append(Web::mimeType(file, path));
        out->sizes.append(file.size);
        out->digests.append(Crypto::sha256(source->contents.value(path)));
    }
    return true;
}

// The three files that make up the bodies below, and the shapes that use them.
struct Bodies {
    QByteArray page;
    QByteArray style;
    QByteArray note;
    QByteArray inner;
    QByteArray deep;
    QByteArray readme;
    QByteArray listing;
    QByteArray big;
};

void addRootPage(MemorySource *source, const Bodies &b)
{
    source->contents.insert(QStringLiteral("index.html"), b.page);
    source->contents.insert(QStringLiteral("style.css"), b.style);
    source->contents.insert(QStringLiteral("sub/note.txt"), b.note);
    source->contents.insert(QStringLiteral("sub/inner/index.html"), b.inner);
}

void addNestedPage(MemorySource *source, const Bodies &b)
{
    source->contents.insert(QStringLiteral("readme.txt"), b.readme);
    source->contents.insert(QStringLiteral("sub/index.html"), b.inner);
    source->contents.insert(QStringLiteral("deep/deeper/index.html"), b.deep);
}

void addPlainFolder(MemorySource *source, const Bodies &b)
{
    source->contents.insert(QStringLiteral("listing.txt"), b.listing);
    source->contents.insert(QStringLiteral("big.bin"), b.big);
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 2) {
        fprintf(stderr, "usage: %s <output.json>\n", qPrintable(app.arguments().value(0)));
        return 2;
    }

    Bodies bodies;
    bodies.page = QByteArrayLiteral("<!doctype html>\n<meta charset=\"utf-8\">\n"
                                    "<title>Kelp gateway fixture</title>\n"
                                    "<link rel=\"stylesheet\" href=\"style.css\">\n"
                                    "<h1 id=\"hello\">Kelp</h1>\n"
                                    "<img src=\"sub/note.txt\">\n");
    bodies.style = QByteArrayLiteral("body { color: #4fd1c5; }\n");
    bodies.note = QByteArrayLiteral("a note in a subfolder\n");
    bodies.inner = QByteArrayLiteral("<!doctype html>\n<h1>inner</h1>\n");
    bodies.deep = QByteArrayLiteral("<!doctype html>\n<h1>deep</h1>\n");
    bodies.readme = QByteArrayLiteral("no page at the root\n");
    bodies.listing = QByteArrayLiteral("plain text, no HTML anywhere\n");
    // More than one 256 KiB chunk (the fixed chunking of §1.2): the JavaScript
    // proves it fetches and concatenates a multi-chunk file in the right order.
    bodies.big = QByteArrayLiteral("kelp!").repeated(53000);        // 265 000 bytes

    Fixture fixture;
    QVector<Fixture> fixtures;
    QString error;

    const QByteArray base = QByteArray(32, char(0));

    {   // a page at the bundle root
        MemorySource source;
        addRootPage(&source, bodies);
        QByteArray secret = base;
        secret[31] = char(1);
        const SourceNode root = dirNode(QStringLiteral("page"), QVector<SourceNode>()
            << fileNode(QStringLiteral("index.html"), QStringLiteral("index.html"), bodies.page)
            << fileNode(QStringLiteral("style.css"), QStringLiteral("style.css"), bodies.style)
            << dirNode(QStringLiteral("sub"), QVector<SourceNode>()
                << fileNode(QStringLiteral("note.txt"), QStringLiteral("sub/note.txt"), bodies.note)
                << dirNode(QStringLiteral("inner"), QVector<SourceNode>()
                    << fileNode(QStringLiteral("index.html"),
                                QStringLiteral("sub/inner/index.html"), bodies.inner))));
        if (!buildFixture(QStringLiteral("page"), secret, root, &source, &fixture, &error)) {
            fprintf(stderr, "makefixture: %s\n", qPrintable(error));
            return 1;
        }
        fixtures.append(fixture);
    }

    {   // a page in a subfolder, and one deeper still
        MemorySource source;
        addNestedPage(&source, bodies);
        QByteArray secret = base;
        secret[31] = char(2);
        const SourceNode root = dirNode(QStringLiteral("nested"), QVector<SourceNode>()
            << fileNode(QStringLiteral("readme.txt"), QStringLiteral("readme.txt"), bodies.readme)
            << dirNode(QStringLiteral("sub"), QVector<SourceNode>()
                << fileNode(QStringLiteral("index.html"),
                            QStringLiteral("sub/index.html"), bodies.inner))
            << dirNode(QStringLiteral("deep"), QVector<SourceNode>()
                << dirNode(QStringLiteral("deeper"), QVector<SourceNode>()
                    << fileNode(QStringLiteral("index.html"),
                                QStringLiteral("deep/deeper/index.html"), bodies.deep))));
        if (!buildFixture(QStringLiteral("nested"), secret, root, &source, &fixture, &error)) {
            fprintf(stderr, "makefixture: %s\n", qPrintable(error));
            return 1;
        }
        fixtures.append(fixture);
    }

    {   // no page at all: the file list, and a file of more than one chunk
        MemorySource source;
        addPlainFolder(&source, bodies);
        QByteArray secret = base;
        secret[31] = char(3);
        const SourceNode root = dirNode(QStringLiteral("folder"), QVector<SourceNode>()
            << fileNode(QStringLiteral("listing.txt"),
                        QStringLiteral("listing.txt"), bodies.listing)
            << fileNode(QStringLiteral("big.bin"), QStringLiteral("big.bin"), bodies.big));
        if (!buildFixture(QStringLiteral("folder"), secret, root, &source, &fixture, &error)) {
            fprintf(stderr, "makefixture: %s\n", qPrintable(error));
            return 1;
        }
        fixtures.append(fixture);
    }

    QByteArray json = QByteArrayLiteral("{\n  \"bundles\": [\n");
    for (int i = 0; i < fixtures.size(); ++i) {
        const Fixture &f = fixtures.at(i);
        json += QByteArrayLiteral("    {\n");
        json += QByteArrayLiteral("      \"name\": ") + jsonString(f.name) + QByteArrayLiteral(",\n");
        json += QByteArrayLiteral("      \"secret\": ") + jsonString(QString::fromLatin1(b64url(f.secret))) + QByteArrayLiteral(",\n");
        json += QByteArrayLiteral("      \"token\": ") + jsonString(QString::fromLatin1(b64url(relayToken(f.secret)))) + QByteArrayLiteral(",\n");
        json += QByteArrayLiteral("      \"bundle\": ") + jsonString(QString::fromLatin1(Link::base32Encode(f.sealed.rootId))) + QByteArrayLiteral(",\n");
        // The real link libkelp would hand out for this share, so the browser
        // side can open its `q=` block and must reach libkelp's own label and
        // page hint (§12.12: the archive is named after the folder).
        Link::Bundle link;
        link.bundleId = f.sealed.rootId;
        link.secret = f.secret;
        link.name = f.name;
        link.page = !f.page.path.isEmpty();
        json += QByteArrayLiteral("      \"link\": ") + jsonString(Link::encode(link)) + QByteArrayLiteral(",\n");
        json += QByteArrayLiteral("      \"entry\": ") + jsonString(f.page.path) + QByteArrayLiteral(",\n");
        json += QByteArrayLiteral("      \"root\": ") + jsonString(f.page.root) + QByteArrayLiteral(",\n");
        json += QByteArrayLiteral("      \"candidates\": [");
        for (int j = 0; j < f.page.candidates.size(); ++j) {
            if (j)
                json += QByteArrayLiteral(", ");
            json += jsonString(f.page.candidates.at(j));
        }
        json += QByteArrayLiteral("],\n");

        json += QByteArrayLiteral("      \"files\": [\n");
        for (int j = 0; j < f.paths.size(); ++j) {
            json += QByteArrayLiteral("        {\"path\": ") + jsonString(f.paths.at(j))
                + QByteArrayLiteral(", \"mime\": ") + jsonString(f.mimes.at(j))
                + QByteArrayLiteral(", \"size\": ") + QByteArray::number(f.sizes.at(j))
                + QByteArrayLiteral(", \"sha256\": ")
                + jsonString(QString::fromLatin1(b64url(f.digests.at(j)))) + QByteArrayLiteral("}");
            json += (j + 1 < f.paths.size()) ? QByteArrayLiteral(",\n") : QByteArrayLiteral("\n");
        }
        json += QByteArrayLiteral("      ],\n");

        json += QByteArrayLiteral("      \"objects\": [\n");
        for (int j = 0; j < f.objects.size(); ++j) {
            json += QByteArrayLiteral("        {\"id\": ")
                + jsonString(QString::fromLatin1(Link::base32Encode(f.objectIds.at(j))))
                + QByteArrayLiteral(", \"bytes\": ")
                + jsonString(QString::fromLatin1(b64url(f.objects.at(j)))) + QByteArrayLiteral("}");
            json += (j + 1 < f.objects.size()) ? QByteArrayLiteral(",\n") : QByteArrayLiteral("\n");
        }
        json += QByteArrayLiteral("      ]\n");
        json += QByteArrayLiteral("    }");
        json += (i + 1 < fixtures.size()) ? QByteArrayLiteral(",\n") : QByteArrayLiteral("\n");
    }
    json += QByteArrayLiteral("  ]\n}\n");

    QFile out(app.arguments().at(1));
    if (!out.open(QIODevice::WriteOnly) || out.write(json) != json.size()) {
        fprintf(stderr, "makefixture: cannot write %s\n", qPrintable(app.arguments().at(1)));
        return 1;
    }
    printf("makefixture: wrote %s (%d bytes, %d bundles)\n",
           qPrintable(app.arguments().at(1)), int(json.size()), fixtures.size());
    return 0;
}

// Temporary: the shape of what was sealed, when something does not resolve.
