#include "goim-codec.hpp"
#include <QtEndian>
#include <array>
#include <brotli/decode.h>
#include <zlib.h>
namespace livehime {
namespace {
bool unpack(const QByteArray &input, quint16 version, QByteArray &out, QString &error) {
    std::array<char, 32768> chunk{};
    if (version == 2) {
        z_stream stream{};
        stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.constData()));
        stream.avail_in = static_cast<uInt>(input.size());
        if (inflateInit(&stream) != Z_OK) {
            error = "zlib initialization failed";
            return false;
        }
        int result = Z_OK;
        while (result == Z_OK) {
            stream.next_out = reinterpret_cast<Bytef *>(chunk.data());
            stream.avail_out = static_cast<uInt>(chunk.size());
            result = inflate(&stream, Z_NO_FLUSH);
            const auto size = static_cast<qsizetype>(chunk.size() - stream.avail_out);
            if (out.size() + size > GoimCodec::MaxBody) {
                inflateEnd(&stream);
                error = "Expanded packet exceeds limit";
                return false;
            }
            out.append(chunk.data(), size);
        }
        const bool ok = result == Z_STREAM_END && stream.avail_in == 0;
        inflateEnd(&stream);
        if (!ok)
            error = "Invalid zlib packet";
        return ok;
    }
    auto *state = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
    if (!state) {
        error = "Brotli initialization failed";
        return false;
    }
    size_t available = static_cast<size_t>(input.size());
    const auto *next = reinterpret_cast<const uint8_t *>(input.constData());
    BrotliDecoderResult result;
    do {
        size_t output = chunk.size();
        auto *destination = reinterpret_cast<uint8_t *>(chunk.data());
        result = BrotliDecoderDecompressStream(state, &available, &next, &output, &destination, nullptr);
        const auto size = static_cast<qsizetype>(chunk.size() - output);
        if (out.size() + size > GoimCodec::MaxBody) {
            BrotliDecoderDestroyInstance(state);
            error = "Expanded packet exceeds limit";
            return false;
        }
        out.append(chunk.data(), size);
    } while (result == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT);
    const bool ok = result == BROTLI_DECODER_RESULT_SUCCESS && available == 0;
    BrotliDecoderDestroyInstance(state);
    if (!ok)
        error = "Invalid Brotli packet";
    return ok;
}
} // namespace
QByteArray GoimCodec::frame(quint32 operation, quint32 sequence, const QByteArray &body, quint16 version) {
    QByteArray result(16, '\0');
    auto *p = reinterpret_cast<uchar *>(result.data());
    qToBigEndian<quint32>(static_cast<quint32>(16 + body.size()), p);
    qToBigEndian<quint16>(16, p + 4);
    qToBigEndian(version, p + 6);
    qToBigEndian(operation, p + 8);
    qToBigEndian(sequence, p + 12);
    result += body;
    return result;
}
bool GoimCodec::decode(const QByteArray &bytes, int depth, qsizetype &budget, QVector<GoimPacket> &packets,
                       QString &error) {
    if (depth > 4) {
        error = "Compressed packet nesting exceeds limit";
        return false;
    }
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        if (bytes.size() - offset < 16) {
            error = "Truncated nested frame";
            return false;
        }
        const auto *p = reinterpret_cast<const uchar *>(bytes.constData() + offset);
        const auto size = qFromBigEndian<quint32>(p);
        const auto header = qFromBigEndian<quint16>(p + 4);
        const auto version = qFromBigEndian<quint16>(p + 6);
        if (header != 16 || size < 16 || size - 16 > MaxBody || size > bytes.size() - offset) {
            error = "Invalid frame length";
            return false;
        }
        const auto body = bytes.mid(offset + 16, size - 16);
        if (version == 2 || version == 3) {
            QByteArray expanded;
            if (!unpack(body, version, expanded, error))
                return false;
            budget -= expanded.size();
            if (budget < 0) {
                error = "Expanded batch exceeds limit";
                return false;
            }
            if (!decode(expanded, depth + 1, budget, packets, error))
                return false;
        } else if (version <= 1) {
            if (packets.size() >= 10000) {
                error = "Packet count exceeds limit";
                return false;
            }
            packets.append({version, qFromBigEndian<quint32>(p + 8), qFromBigEndian<quint32>(p + 12), body});
        } else {
            error = "Unsupported frame version";
            return false;
        }
        offset += size;
    }
    return true;
}
bool GoimCodec::feed(const QByteArray &bytes, QVector<GoimPacket> &packets, QString &error) {
    error.clear();
    // Socket reads are capped; this also bounds malicious incremental accumulation.
    if (buffer_.size() + bytes.size() > MaxBody + 16 + 65536) {
        error = "Receive buffer exceeds limit";
        reset();
        return false;
    }
    buffer_ += bytes;
    qsizetype budget = MaxBody * 4;
    while (buffer_.size() >= 16) {
        const auto *p = reinterpret_cast<const uchar *>(buffer_.constData());
        const auto size = qFromBigEndian<quint32>(p);
        if (qFromBigEndian<quint16>(p + 4) != 16 || size < 16 || size - 16 > MaxBody) {
            error = "Invalid outer frame length";
            reset();
            return false;
        }
        if (buffer_.size() < size)
            break;
        const auto packet = buffer_.first(size);
        buffer_.remove(0, size);
        if (!decode(packet, 0, budget, packets, error)) {
            packets.clear();
            reset();
            return false;
        }
    }
    return true;
}
} // namespace livehime
