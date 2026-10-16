/*
 * The QR encoder behind "Show QR code".
 *
 * The app hands the share link to Project Nayuki's QR generator
 * (src/qrcodegen.*, MIT); this pins the vendored copy. A pinned 25x25 matrix —
 * byte mode, version 2, ECC M, mask 3 — was produced by an independent
 * implementation (Python's `qrcode`) with those parameters fixed, so a mismatch
 * here means the encoding is wrong, not merely drawn differently.
 *
 * It also walks the path the app actually uses (byte mode, ECC M, automatic
 * version) on a link shaped like a real one, and checks the result is a valid QR
 * rather than trusting the library blindly.
 */

#include <stdio.h>

#include <cstdint>
#include <exception>
#include <string>
#include <vector>

#include <QCoreApplication>

#include "qrcodegen.hpp"

namespace {

int g_checks = 0;
int g_failures = 0;
const char *g_section = "";

void section(const char *name)
{
    g_section = name;
    printf("\n== %s\n", name);
}

template <typename A, typename B>
void checkEqual(const A &actual, const B &expected, const char *expression, int line)
{
    ++g_checks;
    if (actual == expected)
        return;
    ++g_failures;
    printf("  FAIL [%s] line %d: %s (actual %s, expected %s)\n", g_section, line,
           expression, std::to_string(actual).c_str(), std::to_string(expected).c_str());
}

void check(bool condition, const char *expression, int line)
{
    ++g_checks;
    if (condition)
        return;
    ++g_failures;
    printf("  FAIL [%s] line %d: %s\n", g_section, line, expression);
}

#define CHECK(cond) check((cond), #cond, __LINE__)
#define CHECK_EQ(actual, expected) checkEqual((actual), (expected), #actual " == " #expected, __LINE__)

// Byte mode, version 2, ECC M, mask 3 — pinned against Python's `qrcode`.
// Payload: "kelp://abc/12345678".
const char *kReference[25] = {
    "1111111011010010001111111",
    "1000001011100100001000001",
    "1011101000011111001011101",
    "1011101010101101101011101",
    "1011101001010101001011101",
    "1000001001011100001000001",
    "1111111010101010101111111",
    "0000000011011010100000000",
    "1011011101111100001001011",
    "0001000111100100000100100",
    "1111101100111100010000000",
    "0001000100100000011101110",
    "0000011010011011101010111",
    "0110110110011011011110011",
    "0111101010010100011000110",
    "1011100101010010010001010",
    "0000001100001010111111111",
    "0000000010000011100010001",
    "1111111010000110101011111",
    "1000001010010110100010001",
    "1011101001000110111110000",
    "1011101011100001101010111",
    "1011101010100010001111110",
    "1000001000011111000010100",
    "1111111010010001011111111",
};

std::vector<std::uint8_t> toBytes(const std::string &text)
{
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

const char *kFinder[7] = {
    "1111111",
    "1000001",
    "1011101",
    "1011101",
    "1011101",
    "1000001",
    "1111111",
};

bool finderAt(const qrcodegen::QrCode &qr, int originX, int originY)
{
    for (int y = 0; y < 7; ++y)
        for (int x = 0; x < 7; ++x)
            if (qr.getModule(originX + x, originY + y) != (kFinder[y][x] == '1'))
                return false;
    return true;
}

void testPinnedVector()
{
    section("pinned vector vs an independent encoder");
    std::vector<qrcodegen::QrSegment> segments{
        qrcodegen::QrSegment::makeBytes(toBytes("kelp://abc/12345678"))};
    const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeSegments(
        segments, qrcodegen::QrCode::Ecc::MEDIUM, 2, 2, 3, false);

    CHECK_EQ(qr.getSize(), 25);

    int mismatches = 0;
    for (int y = 0; y < qr.getSize(); ++y)
        for (int x = 0; x < qr.getSize(); ++x)
            if (qr.getModule(x, y) != (kReference[y][x] == '1'))
                ++mismatches;
    CHECK_EQ(mismatches, 0);
}

void testRealisticLink()
{
    section("a real-shaped link encodes to a valid QR");
    // Roughly the shape of a real link: 52-char bundle id, the address and label
    // sealed into one `q=` block, and a 43-char key in the fragment.
    const std::string link =
        "kelp://zsjvdjr4fgaswxdjc5hmyqckjhikf2vv6dmecxnba2tl7pm34qcq"
        "?q=OXGac2N-lR1tf2s_grQL4ZslqV-ekhbdrQLRowaZ9n7cdPagsgrEv4KixSY0Q6bJfL-Y2Bsf6WLum36poUg"
        "#k=jx4k5x43hYtVsCIiP8RakSDdVe8RzE6u8zqRZ72CTHc";

    const qrcodegen::QrCode qr =
        qrcodegen::QrCode::encodeBinary(toBytes(link), qrcodegen::QrCode::Ecc::MEDIUM);
    const int size = qr.getSize();

    CHECK(size >= 21);
    CHECK((size - 21) % 4 == 0);          // QR versions are 21 + 4*(v-1) modules
    CHECK(size <= 177);
    // The three finder patterns say this is a QR a scanner can locate.
    CHECK(finderAt(qr, 0, 0));
    CHECK(finderAt(qr, size - 7, 0));
    CHECK(finderAt(qr, 0, size - 7));
}

void testTooLong()
{
    section("a payload too long to encode is refused");
    bool threw = false;
    try {
        const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeBinary(
            std::vector<std::uint8_t>(3000, 'x'), qrcodegen::QrCode::Ecc::MEDIUM);
        (void)qr;
    } catch (const std::exception &) {
        threw = true;
    }
    CHECK(threw);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    printf("kelpqr unit test (QR encoding for share links)");

    testPinnedVector();
    testRealisticLink();
    testTooLong();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
