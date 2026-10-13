#include "internal.hpp"

namespace weflow {

namespace {

std::string TrimWhitespaceAndNulls(const std::string& input) {
    std::string cleaned;
    cleaned.reserve(input.size());
    for (char c : input) {
        if (c != '\0') cleaned.push_back(c);
    }
    size_t start = 0;
    while (start < cleaned.size() && std::isspace(static_cast<unsigned char>(cleaned[start]))) {
        ++start;
    }
    size_t end = cleaned.size();
    while (end > start && std::isspace(static_cast<unsigned char>(cleaned[end - 1]))) {
        --end;
    }
    return cleaned.substr(start, end - start);
}

void ParseContactExtraBuffer(
    const uint8_t* data,
    size_t len,
    std::string& outSignature,
    std::string& outLabelIdsCsv,
    bool& outIsMuted) {
    outSignature.clear();
    outLabelIdsCsv.clear();
    outIsMuted = false;
    if (!data || len == 0) return;

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
            if (fieldNum == 12 && val != 0) {
                outIsMuted = true;
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
            std::string raw(reinterpret_cast<const char*>(data + pos), static_cast<size_t>(subLen));
            pos += static_cast<size_t>(subLen);
            if (fieldNum == 4) {
                outSignature = TrimWhitespaceAndNulls(raw);
            } else if (fieldNum == 30) {
                outLabelIdsCsv = TrimWhitespaceAndNulls(raw);
            }
        } else {
            return;
        }
    }
}

std::unordered_map<std::string, std::string> LoadContactLabelMap(WCDB::Handle& dbHandle) {
    std::unordered_map<std::string, std::string> labelMap;
    std::string idCol, nameCol;
    WCDB::Statement pragmaStmt = MakeRawStatement("PRAGMA table_info(contact_label)");
    if (dbHandle.prepare(pragmaStmt)) {
        while (dbHandle.step()) {
            WCDB::Value cv = dbHandle.getValue(1);
            if (cv.isNull()) continue;
            std::string col = static_cast<std::string>(cv.textValue());
            std::string lower = ToLower(col);
            if (lower == "label_id_" || lower == "label_id") idCol = col;
            else if (lower == "label_name_" || lower == "label_name") nameCol = col;
        }
        dbHandle.finalize();
    }
    if (idCol.empty() || nameCol.empty()) return labelMap;

    std::string sql = "SELECT " + idCol + ", " + nameCol + " FROM contact_label";
    WCDB::Statement stmt = MakeRawStatement(sql);
    if (dbHandle.prepare(stmt)) {
        while (dbHandle.step()) {
            WCDB::Value idVal = dbHandle.getValue(0);
            WCDB::Value nameVal = dbHandle.getValue(1);
            if (idVal.isNull() || nameVal.isNull()) continue;
            std::string idStr = (idVal.getType() == WCDB::Value::Type::Integer)
                ? std::to_string(idVal.intValue())
                : TrimWhitespaceAndNulls(static_cast<std::string>(idVal.textValue()));
            std::string nameStr = TrimWhitespaceAndNulls(static_cast<std::string>(nameVal.textValue()));
            if (!idStr.empty() && !nameStr.empty()) {
                labelMap[idStr] = nameStr;
            }
        }
        dbHandle.finalize();
    }
    return labelMap;
}

std::string ResolveLabelListNames(
    const std::string& labelIdsCsv,
    const std::unordered_map<std::string, std::string>& labelMap) {
    if (labelIdsCsv.empty()) return "";
    std::ostringstream oss;
    bool first = true;
    size_t start = 0;
    while (start <= labelIdsCsv.size()) {
        size_t comma = labelIdsCsv.find(',', start);
        std::string token = (comma == std::string::npos)
            ? labelIdsCsv.substr(start)
            : labelIdsCsv.substr(start, comma - start);
        token = TrimWhitespaceAndNulls(token);
        if (!token.empty()) {
            auto it = labelMap.find(token);
            std::string resolved = (it != labelMap.end()) ? it->second : token;
            if (!resolved.empty()) {
                if (!first) oss << ",";
                first = false;
                oss << resolved;
            }
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return oss.str();
}

std::string SerializeDynamicContactRow(
    WCDB::StatementOperation& stmt,
    const std::unordered_map<std::string, std::string>& labelMap,
    bool includeNickNameAlias = false) {
    std::string username;
    int64_t local_type = 0;
    int64_t type = 0;
    std::string alias;
    std::string encrypt_username;
    int64_t flag = 0;
    int64_t delete_flag = 0;
    int64_t verify_flag = 0;
    std::string remark;
    std::string remark_quan_pin;
    std::string remark_pin_yin_initial;
    std::string nick_name;
    std::string pin_yin_initial;
    std::string quan_pin;
    std::string big_head_url;
    std::string small_head_url;
    std::string head_img_md5;
    int64_t chat_room_type = 0;
    std::string description;
    std::string signature;
    std::string label_list;
    std::vector<uint8_t> extraBuffer;

    int numCols = stmt.getNumberOfColumns();
    for (int i = 0; i < numCols; ++i) {
        std::string col = ToLower(static_cast<std::string>(stmt.getColumnName(i)));
        WCDB::Value v = stmt.getValue(i);
        if (v.isNull()) continue;

        auto asStr = [&v]() -> std::string {
            return static_cast<std::string>(v.textValue());
        };
        auto asInt = [&v]() -> int64_t {
            return v.intValue();
        };

        if (col == "username" || col == "user_name") {
            if (username.empty()) username = asStr();
        } else if (col == "local_type") {
            local_type = asInt();
        } else if (col == "type") {
            type = asInt();
        } else if (col == "alias") {
            alias = asStr();
        } else if (col == "encrypt_username" || col == "encrypt_user_name") {
            if (encrypt_username.empty()) encrypt_username = asStr();
        } else if (col == "flag") {
            flag = asInt();
        } else if (col == "delete_flag") {
            delete_flag = asInt();
        } else if (col == "verify_flag") {
            verify_flag = asInt();
        } else if (col == "remark") {
            remark = asStr();
        } else if (col == "remark_quan_pin") {
            remark_quan_pin = asStr();
        } else if (col == "remark_pin_yin_initial") {
            remark_pin_yin_initial = asStr();
        } else if (col == "nick_name" || col == "nickname") {
            if (nick_name.empty()) nick_name = asStr();
        } else if (col == "pin_yin_initial") {
            pin_yin_initial = asStr();
        } else if (col == "quan_pin") {
            quan_pin = asStr();
        } else if (col == "big_head_url") {
            big_head_url = asStr();
        } else if (col == "small_head_url") {
            small_head_url = asStr();
        } else if (col == "head_img_md5") {
            head_img_md5 = asStr();
        } else if (col == "chat_room_type") {
            chat_room_type = asInt();
        } else if (col == "description" || col == "desc") {
            if (description.empty()) description = asStr();
        } else if (col == "signature") {
            signature = asStr();
        } else if (col == "label_list") {
            label_list = asStr();
        } else if (col == "extra_buffer") {
            if (v.getType() == WCDB::Value::Type::BLOB) {
                WCDB::Data d = v.blobValue();
                extraBuffer.assign(d.buffer(), d.buffer() + d.size());
            } else {
                std::string s = asStr();
                extraBuffer.assign(reinterpret_cast<const uint8_t*>(s.data()),
                                   reinterpret_cast<const uint8_t*>(s.data()) + s.size());
            }
        }
    }

    if (!extraBuffer.empty()) {
        std::string protoSig, protoLabels;
        bool protoMuted = false;
        ParseContactExtraBuffer(extraBuffer.data(), extraBuffer.size(), protoSig, protoLabels, protoMuted);
        if (signature.empty() && !protoSig.empty()) {
            signature = protoSig;
        }
        if (label_list.empty() && !protoLabels.empty()) {
            label_list = ResolveLabelListNames(protoLabels, labelMap);
        }
    } else if (!label_list.empty()) {
        label_list = ResolveLabelListNames(label_list, labelMap);
    }
    if (description.empty() && !signature.empty()) {
        description = signature;
    }

    std::ostringstream oss;
    oss << "{"
        << "\"username\":\"" << EscapeJson(username) << "\","
        << "\"user_name\":\"" << EscapeJson(username) << "\","
        << "\"local_type\":" << local_type << ","
        << "\"type\":" << type << ","
        << "\"alias\":\"" << EscapeJson(alias) << "\","
        << "\"encrypt_username\":\"" << EscapeJson(encrypt_username) << "\","
        << "\"encrypt_user_name\":\"" << EscapeJson(encrypt_username) << "\","
        << "\"flag\":" << flag << ","
        << "\"delete_flag\":" << delete_flag << ","
        << "\"verify_flag\":" << verify_flag << ","
        << "\"remark\":\"" << EscapeJson(remark) << "\","
        << "\"remark_quan_pin\":\"" << EscapeJson(remark_quan_pin) << "\","
        << "\"remark_pin_yin_initial\":\"" << EscapeJson(remark_pin_yin_initial) << "\","
        << "\"nick_name\":\"" << EscapeJson(nick_name) << "\","
        << "\"pin_yin_initial\":\"" << EscapeJson(pin_yin_initial) << "\","
        << "\"quan_pin\":\"" << EscapeJson(quan_pin) << "\","
        << "\"big_head_url\":\"" << EscapeJson(big_head_url) << "\","
        << "\"small_head_url\":\"" << EscapeJson(small_head_url) << "\","
        << "\"head_img_md5\":\"" << EscapeJson(head_img_md5) << "\","
        << "\"chat_room_type\":" << chat_room_type << ","
        << "\"description\":\"" << EscapeJson(description) << "\","
        << "\"signature\":\"" << EscapeJson(signature) << "\","
        << "\"label_list\":\"" << EscapeJson(label_list) << "\"";
    if (includeNickNameAlias) {
        oss << ",\"nickName\":\"" << EscapeJson(nick_name) << "\"";
    }
    oss << "}";
    return oss.str();
}

void CollectGroupNicknamesFromExtBuffer(
    const uint8_t* data,
    size_t len,
    std::unordered_map<std::string, std::string>& outMap,
    int depth) {
    if (!data || len == 0 || depth > 3) return;
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
            while (pos < len && (data[pos++] & 0x80) != 0) {}
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

            std::string subUname, subNick;
            size_t subPos = 0;
            while (subPos < subLen) {
                uint64_t subTag = 0;
                int subShift = 0;
                while (subPos < subLen) {
                    uint8_t b = subPtr[subPos++];
                    subTag |= static_cast<uint64_t>(b & 0x7F) << subShift;
                    if ((b & 0x80) == 0) break;
                    subShift += 7;
                    if (subShift > 63) break;
                }
                uint32_t fn = static_cast<uint32_t>(subTag >> 3);
                uint32_t wt = static_cast<uint32_t>(subTag & 0x07);
                if (fn == 0) break;
                if (wt == 2) {
                    uint64_t sLen = 0;
                    int sShift = 0;
                    while (subPos < subLen) {
                        uint8_t b = subPtr[subPos++];
                        sLen |= static_cast<uint64_t>(b & 0x7F) << sShift;
                        if ((b & 0x80) == 0) break;
                        sShift += 7;
                        if (sShift > 63) break;
                    }
                    if (subPos + sLen > subLen) break;
                    std::string s(reinterpret_cast<const char*>(subPtr + subPos), static_cast<size_t>(sLen));
                    subPos += static_cast<size_t>(sLen);
                    if (fn == 1) {
                        subUname = TrimWhitespaceAndNulls(s);
                    } else if (fn == 2) {
                        subNick = TrimWhitespaceAndNulls(s);
                    }
                } else if (wt == 0) {
                    while (subPos < subLen && (subPtr[subPos++] & 0x80) != 0) {}
                } else if (wt == 1) {
                    if (subPos + 8 > subLen) break;
                    subPos += 8;
                } else if (wt == 5) {
                    if (subPos + 4 > subLen) break;
                    subPos += 4;
                } else {
                    break;
                }
            }
            if (!subUname.empty() && !subNick.empty()) {
                outMap[subUname] = subNick;
            } else {
                CollectGroupNicknamesFromExtBuffer(subPtr, static_cast<size_t>(subLen), outMap, depth + 1);
            }
        } else {
            return;
        }
    }
}

} // namespace

} // namespace weflow

extern "C" {

WCDB_API int32_t wcdb_get_sessions(int64_t handle, char** outJson) {
    if (!outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx || !ctx->session_db) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        WCDB::Handle dbHandle = ctx->session_db->getHandle();
        WCDB::Statement stmt = weflow::MakeRawStatement(
            "SELECT * FROM SessionTable ORDER BY sort_timestamp DESC");

        std::ostringstream oss;
        oss << "[";
        bool first = true;
        if (dbHandle.prepare(stmt)) {
            int numCols = dbHandle.getNumberOfColumns();
            while (dbHandle.step()) {
                std::string username;
                std::ostringstream rowOss;
                rowOss << "{";
                bool firstCol = true;
                for (int i = 0; i < numCols; ++i) {
                    std::string colName = static_cast<std::string>(dbHandle.getColumnName(i));
                    std::string colLower = weflow::ToLower(colName);
                    WCDB::Value v = dbHandle.getValue(i);

                    if (!firstCol) rowOss << ",";
                    firstCol = false;
                    rowOss << "\"" << weflow::EscapeJson(colName) << "\":";

                    if (colLower == "summary" || colLower == "draft") {
                        std::string text;
                        if (v.getType() == WCDB::Value::Type::BLOB) {
                            WCDB::Data blob = v.blobValue();
                            if (!weflow::TryDecompressZstd(blob.buffer(), blob.size(), text)) {
                                text.assign(reinterpret_cast<const char*>(blob.buffer()), blob.size());
                            }
                        } else if (!v.isNull()) {
                            text = static_cast<std::string>(v.textValue());
                            std::string dec;
                            if (weflow::TryDecompressZstd(text.data(), text.size(), dec)) {
                                text = dec;
                            }
                        }
                        if (colLower == "summary") {
                            size_t sep = text.find(":\n");
                            if (sep != std::string::npos && sep > 0 && sep < 64) {
                                bool validPrefix = true;
                                for (size_t k = 0; k < sep; ++k) {
                                    unsigned char ch = static_cast<unsigned char>(text[k]);
                                    if (std::isspace(ch)) {
                                        validPrefix = false;
                                        break;
                                    }
                                }
                                if (validPrefix) {
                                    text = text.substr(sep + 2);
                                }
                            }
                        }
                        rowOss << "\"" << weflow::EscapeJson(text) << "\"";
                    } else if (v.isNull()) {
                        if (colLower == "username" || colLower == "last_msg_sender" || colLower == "last_sender_display_name") {
                            rowOss << "\"\"";
                        } else {
                            rowOss << "0";
                        }
                    } else if (v.getType() == WCDB::Value::Type::Integer) {
                        rowOss << v.intValue();
                    } else if (v.getType() == WCDB::Value::Type::Float) {
                        rowOss << v.floatValue();
                    } else if (v.getType() == WCDB::Value::Type::BLOB) {
                        WCDB::Data d = v.blobValue();
                        rowOss << "\"" << weflow::BytesToHex(d.buffer(), d.size()) << "\"";
                    } else {
                        std::string s = static_cast<std::string>(v.textValue());
                        if (colLower == "username") username = s;
                        rowOss << "\"" << weflow::EscapeJson(s) << "\"";
                    }
                }
                rowOss << "}";
                if (username.empty()) continue;
                if (!first) oss << ",";
                first = false;
                oss << rowOss.str();
            }
            dbHandle.finalize();
        }
        oss << "]";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_sessions", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_contact(int64_t handle, const char* username, char** outJson) {
    if (!username || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto contactDb = weflow::ResolveDbByKindOrPath(*ctx, "contact");
        if (!contactDb) {
            *outJson = weflow::AllocCString("{}");
            return 0;
        }
        WCDB::Handle dbHandle = contactDb->getHandle();
        auto labelMap = weflow::LoadContactLabelMap(dbHandle);
        std::string json = "{}";
        for (const char* tbl : {"contact", "stranger"}) {
            std::string sql = std::string("SELECT * FROM ") + tbl +
                              " WHERE username = '" + weflow::EscapeSqlLiteral(username) + "' LIMIT 1";
            WCDB::Statement stmt = weflow::MakeRawStatement(sql);
            if (dbHandle.prepare(stmt)) {
                if (dbHandle.step()) {
                    json = weflow::SerializeDynamicContactRow(dbHandle, labelMap, true);
                }
                dbHandle.finalize();
            }
            if (json != "{}") break;
        }
        *outJson = weflow::AllocCString(json);
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_contact", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_contacts_compact(int64_t handle, const char* usernamesJson, char** outJson) {
    if (!outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto contactDb = weflow::ResolveDbByKindOrPath(*ctx, "contact");
        if (!contactDb) {
            *outJson = weflow::AllocCString("[]");
            return 0;
        }
        WCDB::Handle dbHandle = contactDb->getHandle();
        auto labelMap = weflow::LoadContactLabelMap(dbHandle);

        std::vector<std::string> usernames;
        if (usernamesJson && usernamesJson[0] != '\0') {
            usernames = weflow::ParseJsonStringArray(usernamesJson);
        }

        std::ostringstream sql;
        sql << "SELECT * FROM contact WHERE username IS NOT NULL AND username != ''";
        if (!usernames.empty()) {
            sql << " AND username IN (";
            for (size_t i = 0; i < usernames.size(); ++i) {
                if (i > 0) sql << ",";
                sql << "'" << weflow::EscapeSqlLiteral(usernames[i]) << "'";
            }
            sql << ")";
        }

        WCDB::Statement stmt = weflow::MakeRawStatement(sql.str());
        std::ostringstream oss;
        oss << "[";
        bool first = true;
        if (dbHandle.prepare(stmt)) {
            while (dbHandle.step()) {
                if (!first) oss << ",";
                first = false;
                oss << weflow::SerializeDynamicContactRow(dbHandle, labelMap, true);
            }
            dbHandle.finalize();
        }
        oss << "]";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_contacts_compact", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_contacts(int64_t handle, char** outJson) {
    return wcdb_get_contacts_compact(handle, nullptr, outJson);
}

WCDB_API int32_t wcdb_get_contacts_Info(int64_t handle, char** outJson) {
    return wcdb_get_contacts_compact(handle, nullptr, outJson);
}

WCDB_API int32_t wcdb_get_contact_status(int64_t handle, const char* usernamesJson, char** outJson) {
    if (!usernamesJson || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto usernames = weflow::ParseJsonStringArray(usernamesJson);
        auto contactDb = weflow::ResolveDbByKindOrPath(*ctx, "contact");
        if (!contactDb || usernames.empty()) {
            *outJson = weflow::AllocCString("{}");
            return 0;
        }
        WCDB::Handle dbHandle = contactDb->getHandle();
        std::ostringstream inClause;
        for (size_t i = 0; i < usernames.size(); ++i) {
            if (i > 0) inClause << ",";
            inClause << "'" << weflow::EscapeSqlLiteral(usernames[i]) << "'";
        }
        std::string sql = "SELECT username, flag, extra_buffer FROM contact WHERE username IN (" + inClause.str() + ")";
        WCDB::Statement stmt = weflow::MakeRawStatement(sql);
        std::ostringstream oss;
        oss << "{";
        bool first = true;
        if (dbHandle.prepare(stmt)) {
            while (dbHandle.step()) {
                std::string u = dbHandle.getValue(0).isNull() ? "" : static_cast<std::string>(dbHandle.getValue(0).textValue());
                int64_t flag = dbHandle.getValue(1).isNull() ? 0 : dbHandle.getValue(1).intValue();
                bool isFolded = ((flag >> 28) & 1) != 0;
                bool isMuted = false;
                WCDB::Value ebv = dbHandle.getValue(2);
                if (ebv.getType() == WCDB::Value::Type::BLOB) {
                    WCDB::Data d = ebv.blobValue();
                    std::string sig, labels;
                    weflow::ParseContactExtraBuffer(d.buffer(), d.size(), sig, labels, isMuted);
                }
                if (!u.empty()) {
                    if (!first) oss << ",";
                    first = false;
                    oss << "\"" << weflow::EscapeJson(u) << "\":{\"isFolded\":"
                        << (isFolded ? "true" : "false") << ",\"isMuted\":"
                        << (isMuted ? "true" : "false") << "}";
                }
            }
            dbHandle.finalize();
        }
        oss << "}";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_contact_status", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_contact_type_counts(int64_t handle, char** outJson) {
    if (!outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto contactDb = weflow::ResolveDbByKindOrPath(*ctx, "contact");
        if (!contactDb) {
            *outJson = weflow::AllocCString("{\"private\":0,\"group\":0,\"official\":0,\"former_friend\":0,\"blocked\":0}");
            return 0;
        }
        WCDB::Handle dbHandle = contactDb->getHandle();
        const std::string sql =
            "SELECT "
            "SUM(CASE WHEN username LIKE '%@chatroom' THEN 1 ELSE 0 END) AS group_count, "
            "SUM(CASE WHEN username LIKE 'gh_%' AND local_type = 1 AND username != 'gh_f0a92aa7146c' THEN 1 ELSE 0 END) AS official_count, "
            "SUM(CASE WHEN username NOT LIKE '%@chatroom' AND username NOT LIKE 'gh_%' AND local_type = 1 AND ((COALESCE(flag, 0) & 8) = 0) AND username NOT IN ('medianote','floatbottle','qmessage','qqmail','fmessage') THEN 1 ELSE 0 END) AS private_count, "
            "SUM(CASE WHEN username NOT LIKE '%@chatroom' AND username NOT LIKE 'gh_%' AND ((COALESCE(flag, 0) & 8) = 0) AND ((local_type = 0 AND COALESCE(quan_pin, '') != '') OR (local_type = 3 AND COALESCE(flag, 0) != 4 AND (COALESCE(quan_pin, '') != '' OR COALESCE(alias, '') != '' OR COALESCE(remark, '') != ''))) THEN 1 ELSE 0 END) AS former_friend_count, "
            "SUM(CASE WHEN username NOT LIKE '%@chatroom' AND username NOT LIKE 'gh_%' AND ((COALESCE(flag, 0) & 8) = 8) THEN 1 ELSE 0 END) AS blocked_count "
            "FROM contact WHERE username IS NOT NULL AND username != ''";

        WCDB::Statement stmt = weflow::MakeRawStatement(sql);
        int64_t grp = 0, official = 0, priv = 0, former = 0, blocked = 0;
        if (dbHandle.prepare(stmt)) {
            if (dbHandle.step()) {
                grp = dbHandle.getValue(0).intValue();
                official = dbHandle.getValue(1).intValue();
                priv = dbHandle.getValue(2).intValue();
                former = dbHandle.getValue(3).intValue();
                blocked = dbHandle.getValue(4).intValue();
            }
            dbHandle.finalize();
        }
        std::ostringstream oss;
        oss << "{\"private\":" << priv
            << ",\"group\":" << grp
            << ",\"official\":" << official
            << ",\"former_friend\":" << former
            << ",\"blocked\":" << blocked << "}";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_contact_type_counts", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_search_contacts(int64_t handle, const char* keyword, int32_t limit, char** outJson) {
    if (!keyword || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto contactDb = weflow::ResolveDbByKindOrPath(*ctx, "contact");
        if (!contactDb) {
            *outJson = weflow::AllocCString("[]");
            return 0;
        }
        int32_t lim = (limit > 0) ? limit : 100;
        std::string kw = weflow::EscapeSqlLiteral(keyword);
        std::ostringstream sql;
        sql << "SELECT * FROM contact WHERE "
            << "username LIKE '%" << kw << "%' OR "
            << "alias LIKE '%" << kw << "%' OR "
            << "remark LIKE '%" << kw << "%' OR "
            << "nick_name LIKE '%" << kw << "%' OR "
            << "quan_pin LIKE '%" << kw << "%' OR "
            << "remark_quan_pin LIKE '%" << kw << "%' "
            << "LIMIT " << lim;

        WCDB::Handle dbHandle = contactDb->getHandle();
        auto labelMap = weflow::LoadContactLabelMap(dbHandle);
        WCDB::Statement stmt = weflow::MakeRawStatement(sql.str());
        std::ostringstream oss;
        oss << "[";
        bool first = true;
        if (dbHandle.prepare(stmt)) {
            while (dbHandle.step()) {
                if (!first) oss << ",";
                first = false;
                oss << weflow::SerializeDynamicContactRow(dbHandle, labelMap, true);
            }
            dbHandle.finalize();
        }
        oss << "]";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("search_contacts", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_display_names(int64_t handle, const char* usernamesJson, char** outJson) {
    if (!usernamesJson || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto usernames = weflow::ParseJsonStringArray(usernamesJson);
        auto contactDb = weflow::ResolveDbByKindOrPath(*ctx, "contact");
        if (!contactDb || usernames.empty()) {
            *outJson = weflow::AllocCString("{}");
            return 0;
        }
        WCDB::Handle dbHandle = contactDb->getHandle();
        std::ostringstream oss;
        oss << "{";
        bool first = true;
        for (const auto& u : usernames) {
            if (u.empty()) continue;
            std::string display;
            for (const char* tbl : {"contact", "stranger"}) {
                std::string sql = std::string("SELECT remark, nick_name, alias, username FROM ") + tbl +
                                  " WHERE username = '" + weflow::EscapeSqlLiteral(u) + "' LIMIT 1";
                WCDB::Statement stmt = weflow::MakeRawStatement(sql);
                if (dbHandle.prepare(stmt)) {
                    if (dbHandle.step()) {
                        for (int col = 0; col < 4; ++col) {
                            WCDB::Value v = dbHandle.getValue(col);
                            if (!v.isNull()) {
                                std::string s = static_cast<std::string>(v.textValue());
                                if (!s.empty()) {
                                    display = s;
                                    break;
                                }
                            }
                        }
                    }
                    dbHandle.finalize();
                }
                if (!display.empty()) break;
            }
            if (!display.empty()) {
                if (!first) oss << ",";
                first = false;
                oss << "\"" << weflow::EscapeJson(u) << "\":\"" << weflow::EscapeJson(display) << "\"";
            }
        }
        oss << "}";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_display_names", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_avatars(int64_t handle, const char* usernamesJson, char** outJson) {
    if (!usernamesJson || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto usernames = weflow::ParseJsonStringArray(usernamesJson);
        auto contactDb = weflow::ResolveDbByKindOrPath(*ctx, "contact");
        if (!contactDb || usernames.empty()) {
            *outJson = weflow::AllocCString("{}");
            return 0;
        }
        WCDB::Handle dbHandle = contactDb->getHandle();
        std::ostringstream oss;
        oss << "{";
        bool first = true;
        for (const auto& u : usernames) {
            if (u.empty()) continue;
            std::string smallUrl, bigUrl;
            for (const char* tbl : {"contact", "stranger"}) {
                std::string sql = std::string("SELECT small_head_url, big_head_url FROM ") + tbl +
                                  " WHERE username = '" + weflow::EscapeSqlLiteral(u) + "' LIMIT 1";
                WCDB::Statement stmt = weflow::MakeRawStatement(sql);
                if (dbHandle.prepare(stmt)) {
                    if (dbHandle.step()) {
                        WCDB::Value sv = dbHandle.getValue(0);
                        WCDB::Value bv = dbHandle.getValue(1);
                        if (!sv.isNull()) smallUrl = static_cast<std::string>(sv.textValue());
                        if (!bv.isNull()) bigUrl = static_cast<std::string>(bv.textValue());
                    }
                    dbHandle.finalize();
                }
                if (!smallUrl.empty() || !bigUrl.empty()) break;
            }
            std::string chosen = !smallUrl.empty() ? smallUrl : bigUrl;
            if (!chosen.empty()) {
                if (!first) oss << ",";
                first = false;
                oss << "\"" << weflow::EscapeJson(u) << "\":\"" << weflow::EscapeJson(chosen) << "\"";
            }
        }
        oss << "}";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_avatars", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_contact_alias_map(int64_t handle, const char* usernamesJson, char** outJson) {
    if (!usernamesJson || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto usernames = weflow::ParseJsonStringArray(usernamesJson);
        auto contactDb = weflow::ResolveDbByKindOrPath(*ctx, "contact");
        if (!contactDb || usernames.empty()) {
            *outJson = weflow::AllocCString("{}");
            return 0;
        }
        WCDB::Handle dbHandle = contactDb->getHandle();
        std::ostringstream oss;
        oss << "{";
        bool first = true;
        for (const auto& u : usernames) {
            if (u.empty()) continue;
            std::string sql = "SELECT alias FROM contact WHERE username = '" + weflow::EscapeSqlLiteral(u) + "' LIMIT 1";
            WCDB::Statement stmt = weflow::MakeRawStatement(sql);
            if (dbHandle.prepare(stmt)) {
                if (dbHandle.step()) {
                    WCDB::Value v = dbHandle.getValue(0);
                    if (!v.isNull()) {
                        std::string alias = static_cast<std::string>(v.textValue());
                        if (!alias.empty()) {
                            if (!first) oss << ",";
                            first = false;
                            oss << "\"" << weflow::EscapeJson(u) << "\":\"" << weflow::EscapeJson(alias) << "\"";
                        }
                    }
                }
                dbHandle.finalize();
            }
        }
        oss << "}";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_contact_alias_map", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_contact_friend_flags(int64_t handle, const char* usernamesJson, char** outJson) {
    if (!usernamesJson || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto usernames = weflow::ParseJsonStringArray(usernamesJson);
        auto contactDb = weflow::ResolveDbByKindOrPath(*ctx, "contact");
        if (!contactDb || usernames.empty()) {
            *outJson = weflow::AllocCString("{}");
            return 0;
        }
        WCDB::Handle dbHandle = contactDb->getHandle();
        std::ostringstream oss;
        oss << "{";
        bool first = true;
        for (const auto& u : usernames) {
            if (u.empty()) continue;
            bool isFriend = false;
            std::string sql = "SELECT local_type FROM contact WHERE username = '" + weflow::EscapeSqlLiteral(u) + "' LIMIT 1";
            WCDB::Statement stmt = weflow::MakeRawStatement(sql);
            if (dbHandle.prepare(stmt)) {
                if (dbHandle.step()) {
                    WCDB::Value v = dbHandle.getValue(0);
                    if (!v.isNull()) {
                        int64_t lt = v.intValue();
                        isFriend = (lt != 0 && (lt & 1) != 0);
                    }
                }
                dbHandle.finalize();
            }
            if (!first) oss << ",";
            first = false;
            oss << "\"" << weflow::EscapeJson(u) << "\":" << (isFriend ? "true" : "false");
        }
        oss << "}";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_contact_friend_flags", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_chatroom_info(int64_t handle, const char* chatroomId, char** outJson) {
    if (!chatroomId || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto contactDb = weflow::ResolveDbByKindOrPath(*ctx, "contact");
        if (!contactDb) {
            *outJson = weflow::AllocCString("{}");
            return 0;
        }
        WCDB::Handle dbHandle = contactDb->getHandle();
        std::string sql =
            "SELECT id, username, owner, ext_buffer FROM chat_room WHERE username = '" +
            weflow::EscapeSqlLiteral(chatroomId) + "' LIMIT 1";
        WCDB::Statement stmt = weflow::MakeRawStatement(sql);
        std::ostringstream oss;
        if (dbHandle.prepare(stmt)) {
            if (dbHandle.step()) {
                int64_t id = dbHandle.getValue(0).intValue();
                std::string uname = dbHandle.getValue(1).isNull() ? "" : static_cast<std::string>(dbHandle.getValue(1).textValue());
                std::string owner = dbHandle.getValue(2).isNull() ? "" : static_cast<std::string>(dbHandle.getValue(2).textValue());
                std::string extHex;
                WCDB::Value ev = dbHandle.getValue(3);
                if (ev.getType() == WCDB::Value::Type::BLOB) {
                    WCDB::Data d = ev.blobValue();
                    extHex = weflow::BytesToHex(d.buffer(), d.size());
                }
                oss << "{\"id\":" << id
                    << ",\"username\":\"" << weflow::EscapeJson(uname) << "\""
                    << ",\"owner\":\"" << weflow::EscapeJson(owner) << "\""
                    << ",\"ext_buffer\":\"" << extHex << "\"}";
            }
            dbHandle.finalize();
        }
        std::string res = oss.str();
        *outJson = weflow::AllocCString(res.empty() ? "{}" : res);
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_chatroom_info", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_chatroom_members(int64_t handle, const char* chatroomId, char** outJson) {
    if (!chatroomId || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto contactDb = weflow::ResolveDbByKindOrPath(*ctx, "contact");
        if (!contactDb) {
            *outJson = weflow::AllocCString("[]");
            return 0;
        }
        WCDB::Handle dbHandle = contactDb->getHandle();
        int64_t roomId = 0;
        bool hasRoomId = false;
        {
            std::string rsql = "SELECT rowid FROM name2id WHERE username = '" +
                               weflow::EscapeSqlLiteral(chatroomId) + "' LIMIT 1";
            WCDB::Statement rstmt = weflow::MakeRawStatement(rsql);
            if (dbHandle.prepare(rstmt)) {
                if (dbHandle.step() && !dbHandle.getValue(0).isNull()) {
                    roomId = dbHandle.getValue(0).intValue();
                    hasRoomId = true;
                }
                dbHandle.finalize();
            }
        }

        std::ostringstream oss;
        oss << "[";
        bool first = true;
        if (hasRoomId) {
            std::string sql =
                "SELECT n.username, c.small_head_url, c.big_head_url, c.nick_name, c.remark "
                "FROM chatroom_member m "
                "JOIN name2id n ON m.member_id = n.rowid "
                "LEFT JOIN contact c ON c.username = n.username "
                "WHERE m.room_id = " + std::to_string(roomId);
            WCDB::Statement stmt = weflow::MakeRawStatement(sql);
            if (dbHandle.prepare(stmt)) {
                while (dbHandle.step()) {
                    std::string uname = dbHandle.getValue(0).isNull() ? "" : static_cast<std::string>(dbHandle.getValue(0).textValue());
                    std::string smallUrl = dbHandle.getValue(1).isNull() ? "" : static_cast<std::string>(dbHandle.getValue(1).textValue());
                    std::string bigUrl = dbHandle.getValue(2).isNull() ? "" : static_cast<std::string>(dbHandle.getValue(2).textValue());
                    std::string nick = dbHandle.getValue(3).isNull() ? "" : static_cast<std::string>(dbHandle.getValue(3).textValue());
                    std::string remark = dbHandle.getValue(4).isNull() ? "" : static_cast<std::string>(dbHandle.getValue(4).textValue());
                    if (uname.empty()) continue;
                    std::string avatarUrl = !smallUrl.empty() ? smallUrl : bigUrl;
                    if (!first) oss << ",";
                    first = false;
                    oss << "{\"username\":\"" << weflow::EscapeJson(uname) << "\","
                        << "\"avatarUrl\":\"" << weflow::EscapeJson(avatarUrl) << "\","
                        << "\"nick_name\":\"" << weflow::EscapeJson(nick) << "\","
                        << "\"remark\":\"" << weflow::EscapeJson(remark) << "\","
                        << "\"big_head_url\":\"" << weflow::EscapeJson(bigUrl) << "\","
                        << "\"small_head_url\":\"" << weflow::EscapeJson(smallUrl) << "\"}";
                }
                dbHandle.finalize();
            }
        }
        oss << "]";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_chatroom_members", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_group_members(int64_t handle, const char* chatroomId, char** outJson) {
    return wcdb_get_chatroom_members(handle, chatroomId, outJson);
}

WCDB_API int32_t wcdb_get_group_member_count(int64_t handle, const char* chatroomId, int32_t* outCount) {
    if (!chatroomId || !outCount) return -1;
    *outCount = 0;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto contactDb = weflow::ResolveDbByKindOrPath(*ctx, "contact");
        if (!contactDb) return 0;
        WCDB::Handle dbHandle = contactDb->getHandle();
        int64_t roomId = 0;
        std::string rsql = "SELECT rowid FROM name2id WHERE username = '" +
                           weflow::EscapeSqlLiteral(chatroomId) + "' LIMIT 1";
        WCDB::Statement rstmt = weflow::MakeRawStatement(rsql);
        if (dbHandle.prepare(rstmt)) {
            if (dbHandle.step() && !dbHandle.getValue(0).isNull()) {
                roomId = dbHandle.getValue(0).intValue();
            }
            dbHandle.finalize();
        }
        if (roomId > 0) {
            std::string sql = "SELECT COUNT(*) FROM chatroom_member WHERE room_id = " + std::to_string(roomId);
            WCDB::Statement stmt = weflow::MakeRawStatement(sql);
            if (dbHandle.prepare(stmt)) {
                if (dbHandle.step()) {
                    *outCount = static_cast<int32_t>(dbHandle.getValue(0).intValue());
                }
                dbHandle.finalize();
            }
        }
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_group_member_count", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_group_member_counts(int64_t handle, const char* chatroomIdsJson, char** outJson) {
    if (!chatroomIdsJson || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto ids = weflow::ParseJsonStringArray(chatroomIdsJson);
        auto contactDb = weflow::ResolveDbByKindOrPath(*ctx, "contact");
        if (!contactDb || ids.empty()) {
            *outJson = weflow::AllocCString("{}");
            return 0;
        }
        WCDB::Handle dbHandle = contactDb->getHandle();
        std::ostringstream inClause;
        for (size_t i = 0; i < ids.size(); ++i) {
            if (i > 0) inClause << ",";
            inClause << "'" << weflow::EscapeSqlLiteral(ids[i]) << "'";
        }
        std::string sql =
            "SELECT n.username, COUNT(c.member_id) "
            "FROM name2id n "
            "LEFT JOIN chatroom_member c ON n.rowid = c.room_id "
            "WHERE n.username IN (" + inClause.str() + ") "
            "GROUP BY n.username";
        WCDB::Statement stmt = weflow::MakeRawStatement(sql);
        std::ostringstream oss;
        oss << "{";
        bool first = true;
        if (dbHandle.prepare(stmt)) {
            while (dbHandle.step()) {
                std::string u = dbHandle.getValue(0).isNull() ? "" : static_cast<std::string>(dbHandle.getValue(0).textValue());
                int64_t cnt = dbHandle.getValue(1).intValue();
                if (!u.empty()) {
                    if (!first) oss << ",";
                    first = false;
                    oss << "\"" << weflow::EscapeJson(u) << "\":" << cnt;
                }
            }
            dbHandle.finalize();
        }
        oss << "}";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_group_member_counts", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_group_nicknames(int64_t handle, const char* chatroomId, char** outJson) {
    if (!chatroomId || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto contactDb = weflow::ResolveDbByKindOrPath(*ctx, "contact");
        if (!contactDb) {
            *outJson = weflow::AllocCString("{}");
            return 0;
        }
        WCDB::Handle dbHandle = contactDb->getHandle();
        std::unordered_map<std::string, std::string> nicknames;
        std::string sql = "SELECT ext_buffer FROM chat_room WHERE username = '" +
                          weflow::EscapeSqlLiteral(chatroomId) + "' LIMIT 1";
        WCDB::Statement stmt = weflow::MakeRawStatement(sql);
        if (dbHandle.prepare(stmt)) {
            if (dbHandle.step()) {
                WCDB::Value v = dbHandle.getValue(0);
                if (v.getType() == WCDB::Value::Type::BLOB) {
                    WCDB::Data d = v.blobValue();
                    weflow::CollectGroupNicknamesFromExtBuffer(d.buffer(), d.size(), nicknames, 0);
                }
            }
            dbHandle.finalize();
        }

        std::ostringstream oss;
        oss << "{";
        bool first = true;
        for (const auto& [uname, nick] : nicknames) {
            if (uname.empty() || nick.empty()) continue;
            if (!first) oss << ",";
            first = false;
            oss << "\"" << weflow::EscapeJson(uname) << "\":\"" << weflow::EscapeJson(nick) << "\"";
        }
        oss << "}";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_group_nicknames", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_chat_room_ext_buffer(int64_t handle, const char* chatroomId, char** outJson) {
    if (!chatroomId || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        auto contactDb = weflow::ResolveDbByKindOrPath(*ctx, "contact");
        if (!contactDb) {
            *outJson = weflow::AllocCString("{}");
            return 0;
        }
        WCDB::Handle dbHandle = contactDb->getHandle();
        std::string sql = "SELECT ext_buffer FROM chat_room WHERE username = '" +
                          weflow::EscapeSqlLiteral(chatroomId) + "' LIMIT 1";
        WCDB::Statement stmt = weflow::MakeRawStatement(sql);
        std::string hex;
        if (dbHandle.prepare(stmt)) {
            if (dbHandle.step()) {
                WCDB::Value v = dbHandle.getValue(0);
                if (v.getType() == WCDB::Value::Type::BLOB) {
                    WCDB::Data d = v.blobValue();
                    hex = weflow::BytesToHex(d.buffer(), d.size());
                } else if (!v.isNull()) {
                    std::string s = static_cast<std::string>(v.textValue());
                    hex = weflow::BytesToHex(reinterpret_cast<const uint8_t*>(s.data()), s.size());
                }
            }
            dbHandle.finalize();
        }
        std::ostringstream oss;
        oss << "{\"username\":\"" << weflow::EscapeJson(chatroomId)
            << "\",\"ext_buffer\":\"" << hex << "\"}";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_chat_room_ext_buffer", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_head_image_buffers(int64_t handle, const char* usernamesJson, char** outJson) {
    if (!usernamesJson || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        auto usernames = weflow::ParseJsonStringArray(usernamesJson);
        auto headDb = weflow::ResolveDbByKindOrPath(*ctx, "head_image");
        if (!headDb || usernames.empty()) {
            *outJson = weflow::AllocCString("{}");
            return 0;
        }
        std::ostringstream inClause;
        bool firstIn = true;
        for (const auto& u : usernames) {
            if (u.empty()) continue;
            if (!firstIn) inClause << ",";
            firstIn = false;
            inClause << "'" << weflow::EscapeSqlLiteral(u) << "'";
        }
        std::string sql = "SELECT username, image_buffer FROM head_image WHERE username IN (" + inClause.str() + ")";
        WCDB::Handle dbHandle = headDb->getHandle();
        WCDB::Statement stmt = weflow::MakeRawStatement(sql);
        std::ostringstream oss;
        oss << "{";
        bool firstOut = true;
        if (dbHandle.prepare(stmt)) {
            while (dbHandle.step()) {
                std::string u = dbHandle.getValue(0).isNull() ? "" : static_cast<std::string>(dbHandle.getValue(0).textValue());
                WCDB::Value bv = dbHandle.getValue(1);
                std::string hex;
                if (bv.getType() == WCDB::Value::Type::BLOB) {
                    WCDB::Data d = bv.blobValue();
                    hex = weflow::BytesToHex(d.buffer(), d.size());
                }
                if (!u.empty() && !hex.empty()) {
                    if (!firstOut) oss << ",";
                    firstOut = false;
                    oss << "\"" << weflow::EscapeJson(u) << "\":\"" << hex << "\"";
                }
            }
            dbHandle.finalize();
        }
        oss << "}";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_head_image_buffers", -2, e.what());
        return -2;
    }
}

} // extern "C"
