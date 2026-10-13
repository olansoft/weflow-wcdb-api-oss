#include "internal.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/event.h>
#elif defined(__linux__)
#include <sys/inotify.h>
#include <poll.h>
#endif
#endif

namespace weflow {

namespace {

std::mutex g_state_mutex;
bool g_initialized = false;
int64_t g_next_handle = 1;
std::unordered_map<int64_t, std::shared_ptr<AccountContext>> g_accounts;

std::mutex g_error_mutex;
std::string g_last_error_op;
int g_last_error_code = 0;
std::string g_last_error_msg;

} // namespace

void SetLastError(const std::string& operation, int code, const std::string& message) {
    std::lock_guard<std::mutex> lock(g_error_mutex);
    g_last_error_op = operation;
    g_last_error_code = code;
    g_last_error_msg = message;
}

char* AllocCString(const std::string& str) {
    char* out = static_cast<char*>(std::malloc(str.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, str.data(), str.size());
    out[str.size()] = '\0';
    return out;
}

std::shared_ptr<AccountContext> GetAccount(int64_t handle) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    auto it = g_accounts.find(handle);
    if (it != g_accounts.end()) return it->second;
    if (handle <= 0 && !g_accounts.empty()) return g_accounts.begin()->second;
    return nullptr;
}

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::vector<uint8_t> HexToBytes(const std::string& hex) {
    std::string clean;
    clean.reserve(hex.size());
    for (char c : hex) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            clean.push_back(c);
        }
    }
    if (clean.size() >= 2 && clean[0] == '0' && (clean[1] == 'x' || clean[1] == 'X')) {
        clean.erase(0, 2);
    }
    std::vector<uint8_t> bytes;
    if (clean.empty() || (clean.size() % 2 != 0)) {
        return bytes;
    }
    bytes.reserve(clean.size() / 2);
    for (size_t i = 0; i < clean.size(); i += 2) {
        char buf[3] = {clean[i], clean[i + 1], '\0'};
        char* endp = nullptr;
        long val = std::strtol(buf, &endp, 16);
        if (!endp || *endp != '\0') {
            return {};
        }
        bytes.push_back(static_cast<uint8_t>(val & 0xFF));
    }
    return bytes;
}

std::string BytesToHex(const uint8_t* data, size_t len) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        uint8_t b = data[i];
        out.push_back(kHex[(b >> 4) & 0x0F]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

std::string Base64Encode(const uint8_t* data, size_t len) {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<uint32_t>(data[i + 2]);
        out.push_back(kTable[(n >> 18) & 0x3F]);
        out.push_back(kTable[(n >> 12) & 0x3F]);
        out.push_back((i + 1 < len) ? kTable[(n >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < len) ? kTable[n & 0x3F] : '=');
    }
    return out;
}

std::vector<uint8_t> Base64Decode(const std::string& input) {
    static const auto buildDecodeTable = []() {
        std::array<int, 256> t{};
        t.fill(-1);
        const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) {
            t[static_cast<unsigned char>(chars[i])] = i;
        }
        return t;
    };
    static const auto kDec = buildDecodeTable();
    std::vector<uint8_t> out;
    out.reserve((input.size() * 3) / 4);
    uint32_t val = 0;
    int valb = -8;
    for (unsigned char c : input) {
        if (std::isspace(c)) continue;
        if (c == '=') break;
        int d = kDec[c];
        if (d == -1) break;
        val = (val << 6) + static_cast<uint32_t>(d);
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

std::string Md5HexLower(const std::string& input) {
    return ComputeMD5Hex(input);
}

std::string EscapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '\"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return out;
}

std::string EscapeSqlLiteral(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\'') {
            out += "''";
        } else if (c != '\0') {
            out.push_back(c);
        }
    }
    return out;
}

std::string UnescapeXmlEntities(const std::string& s) {
    std::string out = s;
    auto replaceAll = [&out](const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = out.find(from, pos)) != std::string::npos) {
            out.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replaceAll("&lt;", "<");
    replaceAll("&gt;", ">");
    replaceAll("&quot;", "\"");
    replaceAll("&apos;", "'");
    replaceAll("&amp;", "&");
    return out;
}

std::vector<std::string> ParseJsonStringArray(const char* jsonStr) {
    std::vector<std::string> result;
    if (!jsonStr) return result;
    std::string s(jsonStr);
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '"') {
            ++i;
            std::string item;
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    ++i;
                    char esc = s[i];
                    switch (esc) {
                        case 'n': item.push_back('\n'); break;
                        case 'r': item.push_back('\r'); break;
                        case 't': item.push_back('\t'); break;
                        default: item.push_back(esc); break;
                    }
                } else {
                    item.push_back(s[i]);
                }
                ++i;
            }
            result.push_back(std::move(item));
        } else if (std::isdigit(static_cast<unsigned char>(s[i]))) {
            size_t start = i;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
                ++i;
            }
            result.push_back(s.substr(start, i - start));
            continue;
        }
        ++i;
    }
    return result;
}

std::vector<std::string> ParseJsonObjectArray(const char* jsonStr) {
    std::vector<std::string> result;
    if (!jsonStr) return result;
    std::string s(jsonStr);
    size_t i = 0;
    int depth = 0;
    size_t objStart = 0;
    bool inString = false;
    while (i < s.size()) {
        char c = s[i];
        if (inString) {
            if (c == '\\' && i + 1 < s.size()) {
                i += 2;
                continue;
            }
            if (c == '"') {
                inString = false;
            }
        } else {
            if (c == '"') {
                inString = true;
            } else if (c == '{') {
                if (depth == 0) objStart = i;
                ++depth;
            } else if (c == '}') {
                if (depth > 0) {
                    --depth;
                    if (depth == 0) {
                        result.push_back(s.substr(objStart, i - objStart + 1));
                    }
                }
            }
        }
        ++i;
    }
    return result;
}

std::vector<std::string> ParseJsonStringArrayField(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    pos = json.find('[', pos + 1);
    if (pos == std::string::npos) return {};
    size_t i = pos;
    int depth = 0;
    bool inString = false;
    while (i < json.size()) {
        char c = json[i];
        if (inString) {
            if (c == '\\' && i + 1 < json.size()) {
                i += 2;
                continue;
            }
            if (c == '"') inString = false;
        } else {
            if (c == '"') inString = true;
            else if (c == '[') ++depth;
            else if (c == ']') {
                --depth;
                if (depth == 0) {
                    std::string sub = json.substr(pos, i - pos + 1);
                    return ParseJsonStringArray(sub.c_str());
                }
            }
        }
        ++i;
    }
    return {};
}

std::string ParseJsonStringField(const std::string& json, const std::string& key, const std::string& defaultVal) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return defaultVal;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return defaultVal;
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    if (pos >= json.size() || json[pos] != '"') return defaultVal;
    ++pos;
    std::string out;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            ++pos;
            switch (json[pos]) {
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                default: out.push_back(json[pos]); break;
            }
        } else {
            out.push_back(json[pos]);
        }
        ++pos;
    }
    return out;
}

int64_t ParseJsonIntField(const std::string& json, const std::string& key, int64_t defaultVal) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return defaultVal;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return defaultVal;
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    if (pos >= json.size()) return defaultVal;
    if (json[pos] == '"') ++pos;
    char* endp = nullptr;
    long long v = std::strtoll(json.c_str() + pos, &endp, 10);
    if (endp == json.c_str() + pos) return defaultVal;
    return static_cast<int64_t>(v);
}

bool ParseJsonBoolField(const std::string& json, const std::string& key, bool defaultVal) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return defaultVal;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return defaultVal;
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    if (pos + 4 <= json.size() && json.compare(pos, 4, "true") == 0) return true;
    if (pos + 5 <= json.size() && json.compare(pos, 5, "false") == 0) return false;
    if (pos < json.size() && json[pos] == '1') return true;
    if (pos < json.size() && json[pos] == '0') return false;
    return defaultVal;
}

bool TryDecompressZstd(const void* src, size_t srcLen, std::string& outDecompressed) {
    if (!src || srcLen < 4) return false;
    const uint8_t* bytes = static_cast<const uint8_t*>(src);
    // Zstd frame magic: 0xFD2FB528 (little-endian: 28 B5 2F FD)
    bool hasMagic = (bytes[0] == 0x28 && bytes[1] == 0xB5 && bytes[2] == 0x2F && bytes[3] == 0xFD);
    unsigned long long frameSize = ZSTD_getFrameContentSize(src, srcLen);
    if (frameSize != ZSTD_CONTENTSIZE_UNKNOWN && frameSize != ZSTD_CONTENTSIZE_ERROR && frameSize <= (64ULL << 20)) {
        outDecompressed.resize(static_cast<size_t>(frameSize));
        size_t ret = ZSTD_decompress(outDecompressed.data(), outDecompressed.size(), src, srcLen);
        if (!ZSTD_isError(ret)) {
            outDecompressed.resize(ret);
            return true;
        }
    }
    if (!hasMagic && frameSize == ZSTD_CONTENTSIZE_ERROR) {
        return false;
    }
    // Streaming decompression fallback for unknown frame size
    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    if (!dctx) return false;
    outDecompressed.clear();
    std::vector<char> outBuf(32768);
    ZSTD_inBuffer in = {src, srcLen, 0};
    bool ok = true;
    while (in.pos < in.size) {
        ZSTD_outBuffer out = {outBuf.data(), outBuf.size(), 0};
        size_t ret = ZSTD_decompressStream(dctx, &out, &in);
        if (ZSTD_isError(ret)) {
            ok = false;
            break;
        }
        if (out.pos > 0) {
            outDecompressed.append(outBuf.data(), out.pos);
        }
        if (ret == 0 && out.pos == 0) break;
    }
    ZSTD_freeDCtx(dctx);
    return ok && !outDecompressed.empty();
}

namespace {

bool IsLikelyAsciiHex32(const std::string& s) {
    if (s.size() != 32) return false;
    for (unsigned char c : s) {
        if (!std::isxdigit(c)) return false;
    }
    return true;
}

bool IsPrintableUtf8(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s) {
        if (c == 0) return false;
        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') return false;
    }
    return true;
}

void ScanProtoWire(const uint8_t* data, size_t len, ParsedPackedInfo& info, int depth) {
    if (!data || len == 0 || depth > 4) return;
    size_t pos = 0;
    while (pos < len) {
        uint64_t tag = 0;
        int shift = 0;
        while (pos < len) {
            uint8_t b = data[pos++];
            tag |= static_cast<uint64_t>(b & 0x7F) << shift;
            if ((b & 0x80) == 0) break;
            shift += 7;
            if (shift > 63) return;
        }
        uint32_t fieldNum = static_cast<uint32_t>(tag >> 3);
        uint32_t wireType = static_cast<uint32_t>(tag & 0x07);
        if (fieldNum == 0) return;

        if (wireType == 0) {
            uint64_t val = 0;
            shift = 0;
            while (pos < len) {
                uint8_t b = data[pos++];
                val |= static_cast<uint64_t>(b & 0x7F) << shift;
                if ((b & 0x80) == 0) break;
                shift += 7;
                if (shift > 63) return;
            }
            if (info.file_size == 0 && val > 0 && val < (10ULL << 30) && (fieldNum == 3 || fieldNum == 4)) {
                info.file_size = static_cast<int64_t>(val);
            }
        } else if (wireType == 1) {
            if (pos + 8 > len) return;
            pos += 8;
        } else if (wireType == 5) {
            if (pos + 4 > len) return;
            pos += 4;
        } else if (wireType == 2) {
            uint64_t subLen = 0;
            shift = 0;
            while (pos < len) {
                uint8_t b = data[pos++];
                subLen |= static_cast<uint64_t>(b & 0x7F) << shift;
                if ((b & 0x80) == 0) break;
                shift += 7;
                if (shift > 63) return;
            }
            if (pos + subLen > len) return;
            const uint8_t* subPtr = data + pos;
            pos += static_cast<size_t>(subLen);

            std::string str(reinterpret_cast<const char*>(subPtr), static_cast<size_t>(subLen));
            std::string decompressed;
            if (TryDecompressZstd(subPtr, static_cast<size_t>(subLen), decompressed)) {
                str = decompressed;
                ScanProtoWire(reinterpret_cast<const uint8_t*>(str.data()), str.size(), info, depth + 1);
            }

            if (IsPrintableUtf8(str)) {
                std::string lower = ToLower(str);
                if (IsLikelyAsciiHex32(str)) {
                    if (info.image_md5.empty()) info.image_md5 = lower;
                    if (info.video_md5.empty()) info.video_md5 = lower;
                    if (info.emoji_md5.empty()) info.emoji_md5 = lower;
                } else if (lower.find("http://") == 0 || lower.find("https://") == 0) {
                    if (info.emoji_cdn_url.empty()) info.emoji_cdn_url = str;
                } else if (lower.find(".dat") != std::string::npos || lower.find("img/") != std::string::npos ||
                           lower.find("attach/") != std::string::npos) {
                    if (info.image_path.empty()) info.image_path = str;
                    if (info.image_dat_name.empty()) {
                        info.image_dat_name = fs::path(str).filename().string();
                    }
                } else if (lower.find(".mp4") != std::string::npos || lower.find("video/") != std::string::npos) {
                    if (info.video_file_name.empty()) info.video_file_name = str;
                } else if (info.file_name.empty() && str.size() < 260 && str.find('/') == std::string::npos) {
                    info.file_name = str;
                }
            } else {
                ScanProtoWire(subPtr, static_cast<size_t>(subLen), info, depth + 1);
            }
        } else {
            return;
        }
    }
}

} // namespace

ParsedPackedInfo ParsePackedInfoProto(const uint8_t* data, size_t len) {
    ParsedPackedInfo info;
    if (!data || len == 0) return info;
    std::string decompressed;
    if (TryDecompressZstd(data, len, decompressed)) {
        ScanProtoWire(reinterpret_cast<const uint8_t*>(decompressed.data()), decompressed.size(), info, 0);
    } else {
        ScanProtoWire(data, len, info, 0);
    }
    return info;
}

// ============================================================================
// ApplyReadOptimizations (`ZN12_GLOBAL__N_122ApplyReadOptimizationsERN4WCDB8DatabaseEE3$_0`)
// Reconstructed from 0xa22c and 0x1291a4..0x1292ec:
// Registers config "weflow_read_opt" executing:
//   PRAGMA cache_size = -4096;
//   PRAGMA cache_spill = 1;
//   PRAGMA busy_timeout = 5000;
// ============================================================================
void ApplyReadOptimizations(WCDB::Database& db) {
    WCDB::UnsafeStringView cfgName("weflow_read_opt");
    db.setConfig(
        cfgName,
        [](WCDB::Handle& handle) -> bool {
            auto* op = handle.asHandleOperation();
            if (!op->execute(MakeRawStatement("PRAGMA cache_size = -4096"))) return false;
            if (!op->execute(MakeRawStatement("PRAGMA cache_spill = 1"))) return false;
            if (!op->execute(MakeRawStatement("PRAGMA busy_timeout = 5000"))) return false;
            return true;
        },
        nullptr,
        WCDB::Database::Priority::Default);
}

std::shared_ptr<WCDB::Database> OpenEncryptedDb(AccountContext& ctx, const std::string& dbPath, bool readonly) {
    std::lock_guard<std::recursive_mutex> lock(ctx.mutex);
    auto it = ctx.opened_dbs.find(dbPath);
    if (it != ctx.opened_dbs.end() && it->second) {
        return it->second;
    }
    std::error_code ec;
    if (!fs::exists(dbPath, ec) || !fs::is_regular_file(dbPath, ec)) {
        return nullptr;
    }
    WCDB::UnsafeStringView pathView(dbPath.c_str(), dbPath.size());
    auto db = std::make_shared<WCDB::Database>(pathView, readonly);
    if (!ctx.raw_key.empty()) {
        WCDB::UnsafeData keyData(const_cast<uint8_t*>(ctx.raw_key.data()), ctx.raw_key.size());
        db->setCipherKey(keyData, ctx.cipher_page_size, WCDB::Database::CipherVersion::DefaultVersion);
    }
    ApplyReadOptimizations(*db);
    if (!db->canOpen()) {
        // Fallback to alternative page size (4096 <-> 1024)
        int altPageSize = (ctx.cipher_page_size == 4096) ? 1024 : 4096;
        if (!ctx.raw_key.empty()) {
            WCDB::UnsafeData keyData(const_cast<uint8_t*>(ctx.raw_key.data()), ctx.raw_key.size());
            db->setCipherKey(keyData, altPageSize, WCDB::Database::CipherVersion::DefaultVersion);
        }
        if (!db->canOpen()) {
            return nullptr;
        }
    }
    ctx.opened_dbs[dbPath] = db;
    return db;
}

std::shared_ptr<WCDB::Database> ResolveDbByKindOrPath(AccountContext& ctx, const std::string& kindOrPath) {
    std::lock_guard<std::recursive_mutex> lock(ctx.mutex);
    if (kindOrPath.empty() || kindOrPath == "session" || kindOrPath == "session.db") {
        return ctx.session_db;
    }
    std::string lower = ToLower(kindOrPath);
    fs::path base(ctx.db_storage_dir);
    std::vector<fs::path> candidates;
    if (lower == "contact" || lower == "contact.db") {
        candidates.push_back(base / "contact" / "contact.db");
        candidates.push_back(base / "Contact" / "contact.db");
        candidates.push_back(base / "Contact" / "Contact.db");
    } else if (lower == "head_image" || lower == "head_image.db") {
        candidates.push_back(base / "head_image" / "head_image.db");
        candidates.push_back(base / "HeadImage" / "head_image.db");
    } else if (lower == "hardlink" || lower == "hardlink.db") {
        candidates.push_back(base / "hardlink" / "hardlink.db");
        candidates.push_back(base / "Hardlink" / "hardlink.db");
    } else if (lower == "emoticon" || lower == "emoticon.db") {
        candidates.push_back(base / "emoticon" / "emoticon.db");
        candidates.push_back(base / "Emoticon" / "emoticon.db");
    } else if (lower == "sns" || lower == "sns.db") {
        candidates.push_back(base / "sns" / "sns.db");
        candidates.push_back(base / "Sns" / "sns.db");
    } else if (lower == "fts" || lower == "message_fts" || lower == "message_fts.db") {
        candidates.push_back(base / "message" / "message_fts.db");
        candidates.push_back(base / "Message" / "message_fts.db");
        candidates.push_back(base / "fts" / "message_fts.db");
    } else if (lower == "message" || lower == "message_0.db") {
        candidates.push_back(base / "message" / "message_0.db");
        candidates.push_back(base / "Message" / "message_0.db");
    } else {
        fs::path p(kindOrPath);
        if (p.is_absolute()) {
            candidates.push_back(p);
        } else {
            candidates.push_back(base / p);
            candidates.push_back(base / "message" / p);
            candidates.push_back(base / "Message" / p);
            candidates.push_back(base / "contact" / p);
            candidates.push_back(base / "Contact" / p);
            candidates.push_back(base / "session" / p);
            candidates.push_back(base / "Session" / p);
        }
    }
    for (const auto& c : candidates) {
        std::error_code ec;
        if (fs::exists(c, ec) && fs::is_regular_file(c, ec)) {
            if (auto db = OpenEncryptedDb(ctx, c.string(), false)) {
                return db;
            }
        }
    }
    return nullptr;
}

namespace {

bool IsTargetMessageDbFilename(const std::string& filename) {
    std::string lower = ToLower(filename);
    if (lower.size() < 4 || lower.substr(lower.size() - 3) != ".db") return false;
    if (lower.find("-wal") != std::string::npos || lower.find("-shm") != std::string::npos) return false;
    if (lower.find("fts") != std::string::npos) return false;
    // Match message_<N>.db or biz_message_<N>.db
    auto checkNumericMid = [](const std::string& s, const std::string& prefix) -> bool {
        if (s.rfind(prefix, 0) != 0) return false;
        std::string mid = s.substr(prefix.size(), s.size() - prefix.size() - 3);
        if (mid.empty()) return false;
        return std::all_of(mid.begin(), mid.end(), [](unsigned char c) { return std::isdigit(c); });
    };
    return checkNumericMid(lower, "message_") || checkNumericMid(lower, "biz_message_");
}

void PopulateMessageDbMeta(MessageDbMeta& meta) {
    if (!meta.db) return;
    WCDB::Handle handle = meta.db->getHandle();
    WCDB::Statement stmt = MakeRawStatement(
        "SELECT name FROM sqlite_master WHERE type='table' AND (name LIKE 'Msg_%' OR name = 'Name2Id')");
    if (handle.prepare(stmt)) {
        while (handle.step()) {
            WCDB::Value v = handle.getValue(0);
            if (!v.isNull()) {
                std::string tbl = static_cast<std::string>(v.textValue());
                std::string lower = ToLower(tbl);
                meta.tables.insert(lower);
                meta.canonical_table_names[lower] = tbl;
            }
        }
        handle.finalize();
    }
    if (meta.tables.count("name2id") && !meta.name2id_loaded) {
        WCDB::Statement nstmt = MakeRawStatement("SELECT rowid, user_name FROM Name2Id");
        if (handle.prepare(nstmt)) {
            while (handle.step()) {
                int64_t rowid = handle.getValue(0).intValue();
                WCDB::Value uv = handle.getValue(1);
                if (!uv.isNull()) {
                    std::string uname = static_cast<std::string>(uv.textValue());
                    meta.id_to_username[rowid] = uname;
                    meta.username_to_id[uname] = rowid;
                    meta.table_to_username["msg_" + Md5HexLower(uname)] = uname;
                }
            }
            handle.finalize();
            meta.name2id_loaded = true;
        }
    }
}

} // namespace

std::shared_ptr<MessageDbCatalog> EnsureMessageCatalog(AccountContext& ctx, bool forceRefresh) {
    std::lock_guard<std::recursive_mutex> lock(ctx.mutex);
    if (ctx.msg_catalog && ctx.msg_catalog->initialized && !forceRefresh) {
        return ctx.msg_catalog;
    }
    auto catalog = std::make_shared<MessageDbCatalog>();
    std::error_code ec;
    std::vector<fs::path> dbFiles;
    for (const char* sub : {"message", "Message"}) {
        fs::path msgDir = fs::path(ctx.db_storage_dir) / sub;
        if (fs::exists(msgDir, ec) && fs::is_directory(msgDir, ec)) {
            for (auto it = fs::recursive_directory_iterator(msgDir, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (!it->is_regular_file(ec)) continue;
                std::string fname = it->path().filename().string();
                if (IsTargetMessageDbFilename(fname)) {
                    dbFiles.push_back(it->path());
                }
            }
            if (!dbFiles.empty()) break;
        }
    }
    std::sort(dbFiles.begin(), dbFiles.end());
    for (const auto& p : dbFiles) {
        auto db = OpenEncryptedDb(ctx, p.string(), false);
        if (!db) continue;
        auto meta = std::make_shared<MessageDbMeta>();
        meta->db_path = p.string();
        meta->db = db;
        PopulateMessageDbMeta(*meta);
        catalog->dbs.push_back(std::move(meta));
    }
    catalog->built_at = std::chrono::steady_clock::now();
    catalog->initialized = true;
    ctx.msg_catalog = catalog;
    return catalog;
}

std::vector<std::pair<std::shared_ptr<MessageDbMeta>, std::string>> ResolveSessionMessageTables(
    AccountContext& ctx, const std::string& sessionId) {
    std::vector<std::pair<std::shared_ptr<MessageDbMeta>, std::string>> matches;
    if (sessionId.empty()) return matches;
    std::string md5 = Md5HexLower(sessionId);
    std::string targetLower = "msg_" + md5;
    std::string sidLower = ToLower(sessionId);
    bool isNumeric = std::all_of(sessionId.begin(), sessionId.end(), [](unsigned char c) { return std::isdigit(c); });
    int64_t numericId = isNumeric ? std::strtoll(sessionId.c_str(), nullptr, 10) : 0;

    auto catalog = EnsureMessageCatalog(ctx, false);
    if (!catalog) return matches;
    for (const auto& meta : catalog->dbs) {
        auto it = meta->canonical_table_names.find(targetLower);
        if (it != meta->canonical_table_names.end()) {
            matches.emplace_back(meta, it->second);
            continue;
        }
        if (sidLower.rfind("msg_", 0) == 0) {
            auto itDirect = meta->canonical_table_names.find(sidLower);
            if (itDirect != meta->canonical_table_names.end()) {
                matches.emplace_back(meta, itDirect->second);
                continue;
            }
        }
        if (isNumeric && numericId > 0) {
            auto itName = meta->id_to_username.find(numericId);
            if (itName != meta->id_to_username.end()) {
                std::string numTargetLower = "msg_" + Md5HexLower(itName->second);
                auto itTbl = meta->canonical_table_names.find(numTargetLower);
                if (itTbl != meta->canonical_table_names.end()) {
                    matches.emplace_back(meta, itTbl->second);
                }
            }
        }
    }
    return matches;
}

} // namespace weflow

// ============================================================================
// Exported C-ABI Functions: Lifecycle & Account Management
// ============================================================================

extern "C" {

WCDB_API int32_t InitProtection(const char* /*resourcePath*/) {
    return 0;
}

WCDB_API int32_t wcdb_runtime_policy_version(void) {
    // Observed in disassembly @ 0x31fc: mov w0, #0x4641; movk w0, #0x5745, lsl #16 -> 0x57454641 (1464206081)
    return 1464206081;
}

WCDB_API int32_t wcdb_authenticate(const char* /*token*/, int32_t /*port*/, int64_t /*issuedAtMs*/) {
    return 0;
}

WCDB_API void VerifyUser(int64_t /*hwnd*/, const char* /*message*/, char* outResult, int maxLen) {
    if (outResult && maxLen > 0) {
        std::snprintf(outResult, static_cast<size_t>(maxLen), "{\"success\":true}");
    }
}

WCDB_API int32_t wcdb_init(void) {
    std::lock_guard<std::mutex> lock(weflow::g_state_mutex);
    weflow::g_initialized = true;
    return 0;
}

WCDB_API int32_t wcdb_shutdown(void) {
    std::lock_guard<std::mutex> lock(weflow::g_state_mutex);
    for (auto& kv : weflow::g_accounts) {
        if (kv.second) {
            kv.second->monitor_enabled.store(false);
            if (kv.second->monitor_listen_fd >= 0) {
                ::close(kv.second->monitor_listen_fd);
                kv.second->monitor_listen_fd = -1;
            }
            if (kv.second->monitor_thread.joinable()) {
                kv.second->monitor_thread.join();
            }
        }
    }
    weflow::g_accounts.clear();
    WCDB::Database::purgeAll();
    weflow::g_initialized = false;
    return 0;
}

WCDB_API int32_t wcdb_purge_memory(void) {
    WCDB::Database::purgeAll();
    return 0;
}

WCDB_API int32_t wcdb_open_account(const char* sessionDbPath, const char* hexKey, int64_t* outHandle) {
    if (!sessionDbPath || !hexKey || !outHandle) {
        weflow::SetLastError("open_account", -1, "Invalid null arguments");
        return -1;
    }
    *outHandle = 0;
    std::string pathStr(sessionDbPath);
    std::string keyStr(hexKey);
    if (keyStr.empty()) {
        weflow::SetLastError("open_account", -2, "open_account fail: empty key");
        return -2;
    }
    std::error_code ec;
    if (!std::filesystem::exists(pathStr, ec)) {
        weflow::SetLastError("open_account", -2, "open_account fail: session db not found " + pathStr);
        return -2;
    }
    if (!std::filesystem::is_regular_file(pathStr, ec)) {
        weflow::SetLastError("open_account", -3, "open_account fail: session db path is not a readable file " + pathStr);
        return -3;
    }

    std::vector<uint8_t> rawKey = weflow::HexToBytes(keyStr);
    if (rawKey.empty()) {
        rawKey.assign(reinterpret_cast<const uint8_t*>(keyStr.data()),
                      reinterpret_cast<const uint8_t*>(keyStr.data()) + keyStr.size());
    }

    auto ctx = std::make_shared<weflow::AccountContext>();
    ctx->session_db_path = pathStr;
    ctx->hex_key = keyStr;
    ctx->raw_key = std::move(rawKey);

    // Infer db_storage directory (e.g. .../db_storage/session/session.db -> .../db_storage)
    std::filesystem::path p(pathStr);
    std::filesystem::path parent = p.parent_path();
    if (weflow::ToLower(parent.filename().string()) == "session") {
        ctx->db_storage_dir = parent.parent_path().string();
    } else {
        ctx->db_storage_dir = parent.string();
    }
    ctx->account_root_dir = std::filesystem::path(ctx->db_storage_dir).parent_path().string();

    // Try cipher page sizes {4096 (ps4096), 1024 (ps1024)} as in 0x7600..0x766c
    WCDB::UnsafeStringView dbPathView(pathStr.c_str(), pathStr.size());
    auto db = std::make_shared<WCDB::Database>(dbPathView, false);
    weflow::ApplyReadOptimizations(*db);

    const int pageSizes[2] = {4096, 1024};
    bool opened = false;
    std::string lastErrMsg;
    for (int ps : pageSizes) {
        WCDB::UnsafeData keyData(ctx->raw_key.data(), ctx->raw_key.size());
        db->setCipherKey(keyData, ps, WCDB::Database::CipherVersion::DefaultVersion);
        if (db->canOpen()) {
            ctx->cipher_page_size = ps;
            opened = true;
            break;
        }
        lastErrMsg = static_cast<std::string>(db->getError().getMessage());
    }
    if (!opened) {
        weflow::SetLastError("open_account", -4, "open_account fail: cannot open db " + pathStr + " " + lastErrMsg);
        return -4;
    }

    ctx->session_db = db;
    ctx->opened_dbs[pathStr] = db;

    std::lock_guard<std::mutex> lock(weflow::g_state_mutex);
    int64_t h = weflow::g_next_handle++;
    ctx->handle = h;
    weflow::g_accounts[h] = ctx;
    *outHandle = h;
    return 0;
}

WCDB_API int32_t wcdb_close_account(int64_t handle) {
    std::shared_ptr<weflow::AccountContext> ctx;
    {
        std::lock_guard<std::mutex> lock(weflow::g_state_mutex);
        auto it = weflow::g_accounts.find(handle);
        if (it == weflow::g_accounts.end()) return -1;
        ctx = it->second;
        weflow::g_accounts.erase(it);
    }
    if (ctx) {
        ctx->monitor_enabled.store(false);
        if (ctx->monitor_listen_fd >= 0) {
            ::close(ctx->monitor_listen_fd);
            ctx->monitor_listen_fd = -1;
        }
        if (ctx->monitor_thread.joinable()) {
            ctx->monitor_thread.join();
        }
        if (!ctx->monitor_pipe_name.empty()) {
            ::unlink(ctx->monitor_pipe_name.c_str());
        }
    }
    WCDB::Database::purgeAll();
    return 0;
}

WCDB_API int32_t wcdb_set_my_wxid(int64_t handle, const char* wxid) {
    auto ctx = weflow::GetAccount(handle);
    if (!ctx || !wxid) return -1;
    std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
    ctx->my_wxid = wxid;
    return 0;
}

WCDB_API int32_t wcdb_set_my_account_id(int64_t handle, const char* accountId) {
    return wcdb_set_my_wxid(handle, accountId);
}

WCDB_API int32_t wcdb_set_monitor(int64_t handle, bool enable) {
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;
    std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
    if (!enable) {
        ctx->monitor_enabled.store(false);
#if defined(_WIN32)
        if (ctx->monitor_listen_handle) {
            ::CloseHandle(static_cast<HANDLE>(ctx->monitor_listen_handle));
            ctx->monitor_listen_handle = nullptr;
        }
#else
        if (ctx->monitor_listen_fd >= 0) {
            ::close(ctx->monitor_listen_fd);
            ctx->monitor_listen_fd = -1;
        }
#endif
        if (ctx->monitor_thread.joinable()) {
            ctx->monitor_thread.join();
        }
        return 0;
    }
    if (ctx->monitor_enabled.load()) return 0;

#if defined(_WIN32)
    if (ctx->monitor_pipe_name.empty()) {
        ctx->monitor_pipe_name = "\\\\.\\pipe\\weflow_monitor_" + std::to_string(::GetCurrentProcessId());
    }
    HANDLE hPipe = ::CreateNamedPipeA(
        ctx->monitor_pipe_name.c_str(),
        PIPE_ACCESS_OUTBOUND,
        PIPE_TYPE_BYTE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES,
        4096, 4096, 0, nullptr
    );
    if (hPipe == INVALID_HANDLE_VALUE) return -2;
    ctx->monitor_listen_handle = hPipe;
    ctx->monitor_enabled.store(true);

    std::string walPath = ctx->session_db_path + "-wal";
    ctx->monitor_thread = std::thread([ctx, walPath]() {
        HANDLE pipe = static_cast<HANDLE>(ctx->monitor_listen_handle);
        BOOL connected = ::ConnectNamedPipe(pipe, nullptr) ? TRUE : (::GetLastError() == ERROR_PIPE_CONNECTED);

        auto lastWriteTime = std::filesystem::file_time_type::min();
        uintmax_t lastSize = 0;
        try {
            if (std::filesystem::exists(walPath)) {
                lastWriteTime = std::filesystem::last_write_time(walPath);
                lastSize = std::filesystem::file_size(walPath);
            }
        } catch (...) {}

        while (ctx->monitor_enabled.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            bool changed = false;
            try {
                if (std::filesystem::exists(walPath)) {
                    auto curTime = std::filesystem::last_write_time(walPath);
                    auto curSize = std::filesystem::file_size(walPath);
                    if (curTime != lastWriteTime || curSize != lastSize) {
                        lastWriteTime = curTime;
                        lastSize = curSize;
                        changed = true;
                    }
                }
            } catch (...) {}

            if (changed && connected) {
                const std::string msg = "{\"db\":\"session.db\",\"table\":\"Session\",\"action\":\"update\"}\n";
                DWORD written = 0;
                if (!::WriteFile(pipe, msg.data(), static_cast<DWORD>(msg.size()), &written, nullptr)) {
                    ::DisconnectNamedPipe(pipe);
                    connected = ::ConnectNamedPipe(pipe, nullptr) ? TRUE : (::GetLastError() == ERROR_PIPE_CONNECTED);
                }
            }
        }
        if (pipe && pipe != INVALID_HANDLE_VALUE) {
            ::DisconnectNamedPipe(pipe);
            ::CloseHandle(pipe);
        }
    });
    return 0;

#else
    // POSIX (macOS / Linux)
    if (ctx->monitor_pipe_name.empty()) {
        ctx->monitor_pipe_name = "/tmp/weflow_monitor_" + std::to_string(::getpid()) + ".sock";
    }
    ::unlink(ctx->monitor_pipe_name.c_str());
    int listenFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenFd < 0) return -2;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", ctx->monitor_pipe_name.c_str());
    if (::bind(listenFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0 ||
        ::listen(listenFd, 4) < 0) {
        ::close(listenFd);
        return -3;
    }
    ::fcntl(listenFd, F_SETFL, O_NONBLOCK);
    ctx->monitor_listen_fd = listenFd;
    ctx->monitor_enabled.store(true);

    std::string walPath = ctx->session_db_path + "-wal";
    ctx->monitor_thread = std::thread([ctx, walPath]() {
#if defined(__APPLE__)
        int kq = ::kqueue();
        if (kq < 0) return;
        int walFd = ::open(walPath.c_str(), O_EVTONLY);
        if (walFd >= 0) {
            struct kevent change{};
            EV_SET(&change, walFd, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_CLEAR,
                   NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB, 0, nullptr);
            ::kevent(kq, &change, 1, nullptr, 0, nullptr);
        }
        while (ctx->monitor_enabled.load()) {
            if (ctx->monitor_listen_fd >= 0) {
                int clientFd = ::accept(ctx->monitor_listen_fd, nullptr, nullptr);
                if (clientFd >= 0) {
                    std::lock_guard<std::mutex> lk(ctx->monitor_clients_mutex);
                    ctx->monitor_client_fds.push_back(clientFd);
                }
            }
            struct timespec ts{0, 250000000}; // 250ms
            struct kevent event{};
            int nev = ::kevent(kq, nullptr, 0, &event, 1, &ts);
            if (nev > 0) {
                const std::string msg = "{\"db\":\"session.db\",\"table\":\"Session\",\"action\":\"update\"}\n";
                std::lock_guard<std::mutex> lk(ctx->monitor_clients_mutex);
                for (auto it = ctx->monitor_client_fds.begin(); it != ctx->monitor_client_fds.end();) {
                    ssize_t n = ::send(*it, msg.data(), msg.size(), 0);
                    if (n < 0) {
                        ::close(*it);
                        it = ctx->monitor_client_fds.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
        }
        if (walFd >= 0) ::close(walFd);
        ::close(kq);
#elif defined(__linux__)
        int ifd = ::inotify_init1(IN_NONBLOCK);
        int wd = -1;
        if (ifd >= 0) {
            wd = ::inotify_add_watch(ifd, walPath.c_str(), IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE);
        }
        while (ctx->monitor_enabled.load()) {
            if (ctx->monitor_listen_fd >= 0) {
                int clientFd = ::accept(ctx->monitor_listen_fd, nullptr, nullptr);
                if (clientFd >= 0) {
                    std::lock_guard<std::mutex> lk(ctx->monitor_clients_mutex);
                    ctx->monitor_client_fds.push_back(clientFd);
                }
            }
            bool triggered = false;
            if (ifd >= 0) {
                struct pollfd pfd{ifd, POLLIN, 0};
                int pr = ::poll(&pfd, 1, 250);
                if (pr > 0 && (pfd.revents & POLLIN)) {
                    char buf[512];
                    while (::read(ifd, buf, sizeof(buf)) > 0) {}
                    triggered = true;
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            if (triggered) {
                const std::string msg = "{\"db\":\"session.db\",\"table\":\"Session\",\"action\":\"update\"}\n";
                std::lock_guard<std::mutex> lk(ctx->monitor_clients_mutex);
                for (auto it = ctx->monitor_client_fds.begin(); it != ctx->monitor_client_fds.end();) {
                    ssize_t n = ::send(*it, msg.data(), msg.size(), 0);
                    if (n < 0) {
                        ::close(*it);
                        it = ctx->monitor_client_fds.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
        }
        if (wd >= 0) ::inotify_rm_watch(ifd, wd);
        if (ifd >= 0) ::close(ifd);
#else
        while (ctx->monitor_enabled.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
#endif
    });
    return 0;
#endif
}

WCDB_API int32_t wcdb_get_monitor_pipe_name(char** outPipeName) {
    if (!outPipeName) return -1;
#if defined(_WIN32)
    std::string pipeName = "\\\\.\\pipe\\weflow_monitor_" + std::to_string(::GetCurrentProcessId());
#else
    std::string pipeName = "/tmp/weflow_monitor_" + std::to_string(::getpid()) + ".sock";
#endif
    *outPipeName = weflow::AllocCString(pipeName);
    return *outPipeName ? 0 : -2;
}

WCDB_API void wcdb_free_string(char* str) {
    if (str) {
        std::free(str);
    }
}

WCDB_API void wcdb_free_binary(uint8_t* ptr) {
    if (ptr) {
        std::free(ptr);
    }
}

WCDB_API int32_t wcdb_get_last_error(char** outErrorJson) {
    if (!outErrorJson) return -1;
    std::lock_guard<std::mutex> lock(weflow::g_error_mutex);
    std::ostringstream oss;
    oss << "{\"operation\":\"" << weflow::EscapeJson(weflow::g_last_error_op) << "\","
        << "\"code\":" << weflow::g_last_error_code << ","
        << "\"message\":\"" << weflow::EscapeJson(weflow::g_last_error_msg) << "\"}";
    *outErrorJson = weflow::AllocCString(oss.str());
    return 0;
}

} // extern "C"
