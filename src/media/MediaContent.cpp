#include "MediaContent.h"
#include <QtCore/QJsonDocument>
#include <QtCore/QCryptographicHash>
#include <cmath>

namespace iiLocalLLM {
namespace {
void require(bool ok, const char* message) { if (!ok) throw Error(ErrorCode::InvalidArgument, message); }
QJsonObject media(const QJsonObject& block) {
    const auto type = block["type"].toString(), mime = block["mimeType"].toString();
    require((type == "image" && (mime == "image/png" || mime == "image/jpeg" || mime == "image/bmp" || mime == "image/gif"))
        || (type == "audio" && (mime == "audio/wav" || mime == "audio/x-wav" || mime == "audio/mpeg" || mime == "audio/flac")),
        "Unsupported native media type; use PNG/JPEG/BMP/GIF images or WAV/MP3/FLAC audio");
    (void)mediaBytes(block);
    return {{"type", type}, {"mimeType", mime}, {"data", block["data"]}};
}
}
QByteArray mediaBytes(const QJsonObject& block) {
    require(block["data"].isString() && !block["data"].toString().isEmpty(), "Media data must be nonempty base64");
    const auto encoded = block["data"].toString().toLatin1();
    const auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
    require(bool(decoded) && !decoded.decoded.isEmpty() && decoded.decoded.toBase64() == encoded, "Invalid canonical base64 media data");
    return decoded.decoded;
}
QJsonArray nativeContent(const QJsonArray& content, int maxCharacters) {
    require(content.size() <= 64 && maxCharacters > 0, "Native content exceeds 64 blocks");
    require(QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Compact)).size() <= maxCharacters, "Native content exceeds input limit");
    QJsonArray result;
    int mediaCount = 0;
    for (const auto& value : content) {
        require(value.isObject(), "Content block must be an object");
        auto block = value.toObject(); const auto type = block["type"].toString();
        if(type=="image_url") {
            const auto url=block["image_url"].toObject()["url"].toString();const auto split=url.indexOf(";base64,");
            require(url.startsWith("data:")&&split>5,"Native image_url requires an inline base64 data URL");
            result.append(media({{"type","image"},{"mimeType",url.mid(5,split-5)},{"data",url.mid(split+8)}}));++mediaCount;continue;
        }
        if(type=="input_audio") {
            const auto audio=block["input_audio"].toObject();const auto format=audio["format"].toString();
            require(format=="wav"||format=="mp3"||format=="flac","Unsupported input_audio format");
            result.append(media({{"type","audio"},{"mimeType",format=="mp3"?"audio/mpeg":"audio/"+format},{"data",audio["data"]}}));++mediaCount;continue;
        }
        if (type == "text") {
            require(block["text"].isString(), "Content text must be a string");
            result.append(QJsonObject{{"type", "text"}, {"text", block["text"]}});
        } else if (type == "image" || type == "audio") {
            result.append(media(block)); ++mediaCount;
        } else if (type == "video") {
            require(block["frames"].isArray() && !block["frames"].toArray().isEmpty() && block["frames"].toArray().size() <= 64,
                "Video requires 1 to 64 timestamped image frames");
            QJsonArray frames; double previous = -1;
            for (const auto& item : block["frames"].toArray()) {
                require(item.isObject(), "Video frame must be an object");
                auto frame = item.toObject(); const auto stamp = frame["timestamp_ms"];
                require(stamp.isDouble() && std::isfinite(stamp.toDouble()) && stamp.toDouble() >= 0
                    && stamp.toDouble() <= 9007199254740991.0 && stamp.toDouble() > previous, "Video frame timestamps must strictly increase");
                previous = stamp.toDouble(); frame["type"] = "image";
                auto normalized = media(frame); normalized["timestamp_ms"] = stamp; frames.append(normalized); ++mediaCount;
            }
            result.append(QJsonObject{{"type", "video"}, {"frames", frames}});
        } else if (type == "resource") {
            require(block["resource"].isObject(), "Resource must be an object");
            const auto resource = block["resource"].toObject();
            if (resource.contains("blob")) {
                const auto mime = resource["mimeType"].toString();
                result.append(media({{"type", mime.startsWith("image/") ? "image" : "audio"}, {"mimeType", mime}, {"data", resource["blob"]}})); ++mediaCount;
            } else {
                require(resource["text"].isString(), "Resource requires text or supported media bytes");
                result.append(QJsonObject{{"type", "text"}, {"text", resource["text"]}});
            }
        } else if (type == "resource_link") {
            require(block["uri"].isString(), "Resource link requires a URI");
            result.append(QJsonObject{{"type", "text"}, {"text", QString::fromUtf8(QJsonDocument(block).toJson(QJsonDocument::Compact))}});
        } else throw Error(ErrorCode::RuntimeUnavailable, "Unsupported native content block: " + type);
    }
    require(mediaCount <= 64, "Native content exceeds 64 media items");
    return result;
}
Modalities contentModalities(const QJsonArray& content) {
    Modalities result = modality(Modality::Text);
    for (const auto& value : nativeContent(content)) {
        const auto type = value.toObject()["type"].toString();
        if (type == "image") result |= modality(Modality::Image);
        else if (type == "audio") result |= modality(Modality::Audio);
        else if (type == "video") result |= modality(Modality::Image) | modality(Modality::Video);
    }
    return result;
}
QJsonArray mediaEvidence(const QJsonArray& content) {
    QJsonArray result;
    auto append=[&](const QJsonObject& block) {
        const auto bytes=mediaBytes(block);
        QJsonObject entry{{"type",block["type"]},{"mimeType",block["mimeType"]},{"bytes",bytes.size()},
            {"sha256",QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())}};
        if(block.contains("timestamp_ms"))entry["timestamp_ms"]=block["timestamp_ms"];
        result.append(entry);
    };
    for(const auto& value:nativeContent(content)) {
        const auto block=value.toObject();const auto type=block["type"].toString();
        if(type=="video")for(const auto& frame:block["frames"].toArray())append(frame.toObject());
        else if(type=="image"||type=="audio")append(block);
    }
    return result;
}
}
