#ifndef KELPQR_H
#define KELPQR_H

#include <QImage>
#include <QQuickPaintedItem>
#include <QString>
#include <QVector>

/*
 * The share link as a QR code (Bundle page).
 *
 * A kelp:// link is the whole share, and its fragment is the key, so a QR code is
 * the shortest path from the device that sealed a bundle to the device that opens
 * it: no clipboard, no retyping. The encoding is Project Nayuki's QR generator
 * (src/qrcodegen.*, MIT — see its header); this class is only the Qt Quick item
 * that renders it, one module per square block, black on white, with the standard
 * four-module quiet zone.
 *
 * It is purely a view: `text` is the link, held only for as long as the page
 * shows it, never logged and never stored.
 */
class KelpQrCode : public QQuickPaintedItem
{
    Q_OBJECT

    Q_PROPERTY(QString text READ text WRITE setText NOTIFY textChanged)
    // False when `text` is empty or too long to be a QR code, so the page can
    // show a message instead of a blank square.
    Q_PROPERTY(bool valid READ valid NOTIFY textChanged)

public:
    explicit KelpQrCode(QQuickItem *parent = nullptr);

    QString text() const { return m_text; }
    void setText(const QString &text);

    bool valid() const { return m_valid; }

    void paint(QPainter *painter) override;

signals:
    void textChanged();

private:
    // Encodes m_text into m_cells; called only when the text changes.
    void encode();

    // How many modules the quiet zone is, per the QR standard.
    static const int kQuietZone = 4;

    QString m_text;
    bool m_valid = false;
    int m_modules = 0;
    QVector<bool> m_cells;   // row-major, m_modules * m_modules
    QImage m_image;          // last rendered pixels, reused while the size holds
};

#endif // KELPQR_H
