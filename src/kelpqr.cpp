#include "kelpqr.h"

#include <QPainter>
#include <QPoint>
#include <QRectF>

#include <exception>
#include <vector>

#include "qrcodegen.hpp"

KelpQrCode::KelpQrCode(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    // Rendered into an image (raster) and painted opaque: it is a black-on-white
    // square, and a scanner wants neither transparency nor an FBO's smoothing.
    setRenderTarget(QQuickPaintedItem::Image);
    setOpaquePainting(true);
    setAntialiasing(false);
}

void KelpQrCode::setText(const QString &text)
{
    if (m_text == text)
        return;
    m_text = text;
    encode();
    emit textChanged();
}

void KelpQrCode::encode()
{
    m_cells.clear();
    m_modules = 0;
    m_valid = false;
    m_image = QImage();

    if (m_text.isEmpty()) {
        update();
        return;
    }

    // Byte mode so any link is encodable whatever characters it holds; medium
    // error correction is the usual screen-to-camera compromise. encodeBinary
    // picks the smallest version that fits.
    const QByteArray utf8 = m_text.toUtf8();
    const std::vector<std::uint8_t> data(utf8.constData(),
                                         utf8.constData() + utf8.size());
    try {
        const qrcodegen::QrCode qr =
            qrcodegen::QrCode::encodeBinary(data, qrcodegen::QrCode::Ecc::MEDIUM);
        m_modules = qr.getSize();
        m_cells.resize(m_modules * m_modules);
        for (int y = 0; y < m_modules; ++y)
            for (int x = 0; x < m_modules; ++x)
                m_cells[y * m_modules + x] = qr.getModule(x, y);
        m_valid = true;
    } catch (const std::exception &) {
        // The only way this throws is a payload longer than a QR code can hold
        // (2953 bytes). The page shows a message rather than an empty square.
        m_valid = false;
    }
    update();
}

void KelpQrCode::paint(QPainter *painter)
{
    // A white backdrop even if the code cannot be drawn, so the box never shows
    // through as transparent.
    painter->fillRect(QRectF(0, 0, width(), height()), Qt::white);

    if (!m_valid || m_modules <= 0)
        return;

    // Whole-pixel modules: a scanner reads squares, and a fractional scale would
    // blur their edges.
    const int span = m_modules + 2 * kQuietZone;
    const int available = qMin(int(width()), int(height()));
    int scale = available / span;
    if (scale < 1)
        scale = 1;
    const int side = span * scale;

    if (m_image.width() != side) {
        m_image = QImage(side, side, QImage::Format_RGB32);
        m_image.fill(Qt::white);
        QPainter modules(&m_image);
        modules.setPen(Qt::NoPen);
        modules.setBrush(Qt::black);
        for (int y = 0; y < m_modules; ++y) {
            for (int x = 0; x < m_modules; ++x) {
                if (!m_cells.at(y * m_modules + x))
                    continue;
                modules.drawRect((x + kQuietZone) * scale, (y + kQuietZone) * scale,
                                 scale, scale);
            }
        }
    }

    painter->setRenderHint(QPainter::Antialiasing, false);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter->drawImage(QPoint((int(width()) - side) / 2, (int(height()) - side) / 2),
                       m_image);
}
