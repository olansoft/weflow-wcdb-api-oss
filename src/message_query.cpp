#include "internal.hpp"

namespace weflow {

namespace {

std::mutex g_cursor_mutex;
int64_t g_next_cursor_id = 1;
std::unordered_map<int64_t, std::shared_ptr<ExportCursorState>> g_cursors;

struct MsgRowData {
    int64_t local_id = 0;
    int64_t server_id = 0;
    int64_t local_type = 0;
    int64_t sort_seq = 0;
    int64_t real_sender_id = 0;
    int64_t create_time = 0;
    int64_t status = 0;
    int64_t upload_status = 0;
    int64_t download_status = 0;
    int64_t server_seq = 0;
    int64_t origin_source = 0;
    std::string source;
    std::string message_content;
    std::string compress_content;
    std::string packed_info_hex;
    ParsedPackedInfo packed;
    std::string sender_username;
    int computed_is_send = 0;
    std::string session_id;
    std::string table_name;
    std::string db_path;
};

MsgRowData ExtractMessageRow(
    WCDB::StatementOperation& stmt,
    const std::shared_ptr<MessageDbMeta>& meta,
    const std::string& myWxid,
    const std::string& sessionId,
    const std::string& tableName) {
    MsgRowData row;
    row.session_id = sessionId;
    row.table_name = tableName;
    if (meta) row.db_path = meta->db_path;

    int numCols = stmt.getNumberOfColumns();
    int64_t wcdbCtMsg = 0;
    int64_t wcdbCtSource = 0;
    std::vector<uint8_t> rawMsgBlob;
    std::vector<uint8_t> rawCompBlob;
    std::vector<uint8_t> rawSourceBlob;
    std::vector<uint8_t> rawPackedBlob;

    for (int i = 0; i < numCols; ++i) {
        std::string colName = ToLower(static_cast<std::string>(stmt.getColumnName(i)));
        WCDB::Value v = stmt.getValue(i);
        if (v.isNull()) continue;

        if (colName == "local_id") {
            row.local_id = v.intValue();
        } else if (colName == "server_id") {
            row.server_id = v.intValue();
        } else if (colName == "local_type") {
            row.local_type = v.intValue();
        } else if (colName == "sort_seq") {
            row.sort_seq = v.intValue();
        } else if (colName == "real_sender_id") {
            row.real_sender_id = v.intValue();
        } else if (colName == "create_time") {
            row.create_time = v.intValue();
        } else if (colName == "status") {
            row.status = v.intValue();
        } else if (colName == "upload_status") {
            row.upload_status = v.intValue();
        } else if (colName == "download_status") {
            row.download_status = v.intValue();
        } else if (colName == "server_seq") {
            row.server_seq = v.intValue();
        } else if (colName == "origin_source") {
            row.origin_source = v.intValue();
        } else if (colName == "wcdb_ct_message_content") {
            wcdbCtMsg = v.intValue();
        } else if (colName == "wcdb_ct_source") {
            wcdbCtSource = v.intValue();
        } else if (colName == "message_content") {
            if (v.getType() == WCDB::Value::Type::BLOB) {
                WCDB::Data d = v.blobValue();
                rawMsgBlob.assign(d.buffer(), d.buffer() + d.size());
            } else {
                row.message_content = static_cast<std::string>(v.textValue());
            }
        } else if (colName == "compress_content") {
            if (v.getType() == WCDB::Value::Type::BLOB) {
                WCDB::Data d = v.blobValue();
                rawCompBlob.assign(d.buffer(), d.buffer() + d.size());
            } else {
                row.compress_content = static_cast<std::string>(v.textValue());
            }
        } else if (colName == "source") {
            if (v.getType() == WCDB::Value::Type::BLOB) {
                WCDB::Data d = v.blobValue();
                rawSourceBlob.assign(d.buffer(), d.buffer() + d.size());
            } else {
                row.source = static_cast<std::string>(v.textValue());
            }
        } else if (colName == "packed_info_data" || colName == "bytesextra") {
            if (v.getType() == WCDB::Value::Type::BLOB) {
                WCDB::Data d = v.blobValue();
                rawPackedBlob.assign(d.buffer(), d.buffer() + d.size());
            }
        }
    }

    // Handle Zstd decompression on message_content / compress_content / source
    if (!rawMsgBlob.empty()) {
        std::string dec;
        if (wcdbCtMsg == 4 || TryDecompressZstd(rawMsgBlob.data(), rawMsgBlob.size(), dec)) {
            if (dec.empty()) TryDecompressZstd(rawMsgBlob.data(), rawMsgBlob.size(), dec);
            row.message_content = !dec.empty()
                ? dec
                : std::string(reinterpret_cast<const char*>(rawMsgBlob.data()), rawMsgBlob.size());
        } else {
            row.message_content.assign(reinterpret_cast<const char*>(rawMsgBlob.data()), rawMsgBlob.size());
        }
    } else if (!row.message_content.empty()) {
        std::string dec;
        if (TryDecompressZstd(row.message_content.data(), row.message_content.size(), dec)) {
            row.message_content = dec;
        }
    }

    if (!rawCompBlob.empty()) {
        std::string dec;
        if (TryDecompressZstd(rawCompBlob.data(), rawCompBlob.size(), dec)) {
            row.compress_content = dec;
            if (row.message_content.empty()) row.message_content = dec;
        } else {
            row.compress_content = BytesToHex(rawCompBlob.data(), rawCompBlob.size());
        }
    }

    if (!rawSourceBlob.empty()) {
        std::string dec;
        if (wcdbCtSource == 4 || TryDecompressZstd(rawSourceBlob.data(), rawSourceBlob.size(), dec)) {
            if (dec.empty()) TryDecompressZstd(rawSourceBlob.data(), rawSourceBlob.size(), dec);
            row.source = !dec.empty()
                ? dec
                : std::string(reinterpret_cast<const char*>(rawSourceBlob.data()), rawSourceBlob.size());
        } else {
            row.source.assign(reinterpret_cast<const char*>(rawSourceBlob.data()), rawSourceBlob.size());
        }
    }

    if (!rawPackedBlob.empty()) {
        row.packed_info_hex = BytesToHex(rawPackedBlob.data(), rawPackedBlob.size());
        row.packed = ParsePackedInfoProto(rawPackedBlob.data(), rawPackedBlob.size());
    }

    // Resolve sender_username and computed_is_send via Name2Id
    if (meta && row.real_sender_id > 0) {
        auto it = meta->id_to_username.find(row.real_sender_id);
        if (it != meta->id_to_username.end()) {
            row.sender_username = it->second;
        }
    }
    if (!myWxid.empty() && !row.sender_username.empty()) {
        row.computed_is_send = (row.sender_username == myWxid) ? 1 : 0;
    } else if (row.status == 2 || row.status == 3) {
        row.computed_is_send = (row.status == 2) ? 1 : 0;
    }

    // For chatroom messages where message_content starts with "wxid_xxx:\n"
    if (row.sender_username.empty() && sessionId.find("@chatroom") != std::string::npos) {
        size_t nl = row.message_content.find(":\n");
        if (nl != std::string::npos && nl > 0 && nl < 64) {
            row.sender_username = row.message_content.substr(0, nl);
        }
    }

    return row;
}

std::string SerializeMessageRowJson(const MsgRowData& r, bool journeyMode = false) {
    std::ostringstream oss;
    oss << "{"
        << "\"local_id\":" << r.local_id << ","
        << "\"server_id\":" << r.server_id << ","
        << "\"local_type\":" << r.local_type << ","
        << "\"sort_seq\":" << r.sort_seq << ","
        << "\"real_sender_id\":" << r.real_sender_id << ","
        << "\"create_time\":" << r.create_time << ","
        << "\"status\":" << r.status << ","
        << "\"upload_status\":" << r.upload_status << ","
        << "\"download_status\":" << r.download_status << ","
        << "\"server_seq\":" << r.server_seq << ","
        << "\"origin_source\":" << r.origin_source << ","
        << "\"source\":\"" << EscapeJson(r.source) << "\","
        << "\"message_content\":\"" << EscapeJson(r.message_content) << "\","
        << "\"compress_content\":\"" << EscapeJson(r.compress_content) << "\","
        << "\"sender_username\":\"" << EscapeJson(r.sender_username) << "\","
        << "\"computed_is_send\":" << r.computed_is_send;
    if (!journeyMode) {
        oss << ",\"packed_info_data\":\"" << r.packed_info_hex << "\""
            << ",\"table_name\":\"" << EscapeJson(r.table_name) << "\""
            << ",\"session_id\":\"" << EscapeJson(r.session_id) << "\"";
        if (!r.packed.image_md5.empty()) {
            oss << ",\"image_md5\":\"" << EscapeJson(r.packed.image_md5) << "\"";
        }
        if (!r.packed.image_path.empty()) {
            oss << ",\"image_path\":\"" << EscapeJson(r.packed.image_path) << "\"";
        }
        if (!r.packed.video_md5.empty()) {
            oss << ",\"video_md5\":\"" << EscapeJson(r.packed.video_md5) << "\"";
        }
        if (!r.packed.emoji_md5.empty()) {
            oss << ",\"emoji_md5\":\"" << EscapeJson(r.packed.emoji_md5) << "\"";
        }
        if (!r.packed.emoji_cdn_url.empty()) {
            oss << ",\"emoji_cdn_url\":\"" << EscapeJson(r.packed.emoji_cdn_url) << "\"";
        }
        if (!r.packed.file_name.empty()) {
            oss << ",\"file_name\":\"" << EscapeJson(r.packed.file_name) << "\"";
        }
        if (r.packed.file_size > 0) {
            oss << ",\"file_size\":" << r.packed.file_size;
        }
    }
    oss << "}";
    return oss.str();
}

void RefillCursorSource(AccountContext& ctx, ExportCursorState& cursor, CursorSource& src) {
    if (src.exhausted || !src.db_meta || !src.db_meta->db) return;

    std::string op = cursor.ascending ? ">" : "<";
    std::string orderDir = cursor.ascending ? "ASC" : "DESC";
    int32_t fetchLimit = std::max<int32_t>(cursor.batch_size, 100);

    std::ostringstream sql;
    sql << "SELECT * FROM " << src.table_name << " WHERE 1=1";
    if (cursor.start_timestamp > 0) {
        sql << " AND create_time >= " << cursor.start_timestamp;
    }
    if (cursor.end_timestamp > 0) {
        sql << " AND create_time <= " << cursor.end_timestamp;
    }
    if (src.has_last_key) {
        sql << " AND (sort_seq " << op << " " << src.last_sort_seq
            << " OR (sort_seq = " << src.last_sort_seq << " AND create_time " << op << " " << src.last_create_time << ")"
            << " OR (sort_seq = " << src.last_sort_seq << " AND create_time = " << src.last_create_time
            << " AND local_id " << op << " " << src.last_local_id << "))";
    }
    sql << " ORDER BY sort_seq " << orderDir
        << ", create_time " << orderDir
        << ", local_id " << orderDir
        << " LIMIT " << fetchLimit;

    WCDB::Handle dbHandle = src.db_meta->db->getHandle();
    WCDB::Statement stmt = MakeRawStatement(sql.str());
    int fetched = 0;
    bool journeyMode = (cursor.projection_mode == "journey");
    if (dbHandle.prepare(stmt)) {
        while (dbHandle.step()) {
            MsgRowData r = ExtractMessageRow(dbHandle, src.db_meta, ctx.my_wxid, cursor.session_id, src.table_name);
            CursorRowItem item;
            item.sort_seq = r.sort_seq;
            item.create_time = r.create_time;
            item.local_id = r.local_id;
            item.json_row = SerializeMessageRowJson(r, journeyMode);
            src.last_sort_seq = r.sort_seq;
            src.last_create_time = r.create_time;
            src.last_local_id = r.local_id;
            src.has_last_key = true;
            src.buffer.push_back(std::move(item));
            ++fetched;
        }
        dbHandle.finalize();
    }
    if (fetched < fetchLimit) {
        src.exhausted = true;
    }
}

} // namespace

} // namespace weflow

extern "C" {

WCDB_API int32_t wcdb_get_messages(
    int64_t handle, const char* sessionId, int32_t limit, int32_t offset, char** outJson) {
    if (!sessionId || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto tables = weflow::ResolveSessionMessageTables(*ctx, sessionId);
        int32_t lim = (limit > 0) ? limit : 100;
        int32_t off = (offset >= 0) ? offset : 0;
        int32_t perDbLimit = lim + off;

        std::vector<weflow::MsgRowData> allRows;
        for (const auto& [meta, tbl] : tables) {
            std::ostringstream sql;
            sql << "SELECT * FROM " << tbl
                << " ORDER BY sort_seq DESC, create_time DESC, local_id DESC LIMIT " << perDbLimit;
            WCDB::Handle dbHandle = meta->db->getHandle();
            WCDB::Statement stmt = weflow::MakeRawStatement(sql.str());
            if (dbHandle.prepare(stmt)) {
                while (dbHandle.step()) {
                    allRows.push_back(weflow::ExtractMessageRow(dbHandle, meta, ctx->my_wxid, sessionId, tbl));
                }
                dbHandle.finalize();
            }
        }
        std::sort(allRows.begin(), allRows.end(), [](const weflow::MsgRowData& a, const weflow::MsgRowData& b) {
            if (a.sort_seq != b.sort_seq) return a.sort_seq > b.sort_seq;
            if (a.create_time != b.create_time) return a.create_time > b.create_time;
            return a.local_id > b.local_id;
        });

        std::ostringstream oss;
        oss << "[";
        bool first = true;
        size_t startIdx = static_cast<size_t>(off);
        size_t endIdx = std::min(allRows.size(), startIdx + static_cast<size_t>(lim));
        for (size_t i = startIdx; i < endIdx; ++i) {
            if (!first) oss << ",";
            first = false;
            oss << weflow::SerializeMessageRowJson(allRows[i], false);
        }
        oss << "]";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_messages", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_message_count(int64_t handle, const char* sessionId, int32_t* outCount) {
    if (!sessionId || !outCount) return -1;
    *outCount = 0;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto tables = weflow::ResolveSessionMessageTables(*ctx, sessionId);
        int64_t total = 0;
        for (const auto& [meta, tbl] : tables) {
            WCDB::Handle dbHandle = meta->db->getHandle();
            WCDB::Statement stmt = weflow::MakeRawStatement("SELECT COUNT(*) FROM " + tbl);
            if (dbHandle.prepare(stmt)) {
                if (dbHandle.step()) {
                    total += dbHandle.getValue(0).intValue();
                }
                dbHandle.finalize();
            }
        }
        *outCount = static_cast<int32_t>(total);
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_message_count", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_session_message_TotalCount(int64_t handle, const char* sessionId, int32_t* outCount) {
    return wcdb_get_message_count(handle, sessionId, outCount);
}

WCDB_API int32_t wcdb_get_all_messages_count(int64_t handle, char** outJson) {
    if (!outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto catalog = weflow::EnsureMessageCatalog(*ctx, false);
        int64_t total = 0;
        std::ostringstream byDb;
        byDb << "{";
        bool first = true;
        if (catalog) {
            for (const auto& meta : catalog->dbs) {
                int64_t dbSum = 0;
                WCDB::Handle dbHandle = meta->db->getHandle();
                for (const auto& [lowerTbl, actualTbl] : meta->canonical_table_names) {
                    if (lowerTbl.rfind("msg_", 0) != 0) continue;
                    WCDB::Statement stmt = weflow::MakeRawStatement("SELECT COUNT(*) FROM " + actualTbl);
                    if (dbHandle.prepare(stmt)) {
                        if (dbHandle.step()) {
                            dbSum += dbHandle.getValue(0).intValue();
                        }
                        dbHandle.finalize();
                    }
                }
                total += dbSum;
                std::string fname = std::filesystem::path(meta->db_path).filename().string();
                if (!first) byDb << ",";
                first = false;
                byDb << "\"" << weflow::EscapeJson(fname) << "\":" << dbSum;
            }
        }
        byDb << "}";
        std::ostringstream oss;
        oss << "{\"total\":" << total << ",\"databases\":" << byDb.str() << "}";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_all_messages_count", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_messages_by_type(
    int64_t handle,
    const char* sessionId,
    int64_t localType,
    int32_t ascending,
    int32_t limit,
    int32_t offset,
    char** outJson) {
    if (!sessionId || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto tables = weflow::ResolveSessionMessageTables(*ctx, sessionId);
        bool asc = (ascending != 0);
        std::string dir = asc ? "ASC" : "DESC";
        int32_t lim = (limit > 0) ? limit : 100;
        int32_t off = (offset >= 0) ? offset : 0;

        std::vector<weflow::MsgRowData> rows;
        for (const auto& [meta, tbl] : tables) {
            std::ostringstream sql;
            sql << "SELECT * FROM " << tbl
                << " WHERE local_type = " << localType
                << " ORDER BY sort_seq " << dir << ", create_time " << dir << ", local_id " << dir
                << " LIMIT " << (lim + off);
            WCDB::Handle dbHandle = meta->db->getHandle();
            WCDB::Statement stmt = weflow::MakeRawStatement(sql.str());
            if (dbHandle.prepare(stmt)) {
                while (dbHandle.step()) {
                    rows.push_back(weflow::ExtractMessageRow(dbHandle, meta, ctx->my_wxid, sessionId, tbl));
                }
                dbHandle.finalize();
            }
        }
        std::sort(rows.begin(), rows.end(), [asc](const weflow::MsgRowData& a, const weflow::MsgRowData& b) {
            if (a.sort_seq != b.sort_seq) return asc ? (a.sort_seq < b.sort_seq) : (a.sort_seq > b.sort_seq);
            if (a.create_time != b.create_time) return asc ? (a.create_time < b.create_time) : (a.create_time > b.create_time);
            return asc ? (a.local_id < b.local_id) : (a.local_id > b.local_id);
        });

        std::ostringstream oss;
        oss << "[";
        bool first = true;
        size_t s = static_cast<size_t>(off);
        size_t e = std::min(rows.size(), s + static_cast<size_t>(lim));
        for (size_t i = s; i < e; ++i) {
            if (!first) oss << ",";
            first = false;
            oss << weflow::SerializeMessageRowJson(rows[i], false);
        }
        oss << "]";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_messages_by_type", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_message_meta(
    int64_t handle,
    const char* dbPath,
    const char* tableName,
    int32_t limit,
    int32_t offset,
    char** outJson) {
    if (!dbPath || !tableName || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto db = weflow::ResolveDbByKindOrPath(*ctx, dbPath);
        if (!db) {
            *outJson = weflow::AllocCString("[]");
            return 0;
        }
        int32_t lim = (limit > 0) ? limit : 100;
        int32_t off = (offset >= 0) ? offset : 0;
        std::ostringstream sql;
        sql << "SELECT local_id, create_time, local_type, status, real_sender_id, server_id FROM "
            << tableName << " ORDER BY sort_seq DESC, create_time DESC, local_id DESC LIMIT "
            << lim << " OFFSET " << off;
        WCDB::Handle dbHandle = db->getHandle();
        WCDB::Statement stmt = weflow::MakeRawStatement(sql.str());
        std::ostringstream oss;
        oss << "[";
        bool first = true;
        if (dbHandle.prepare(stmt)) {
            while (dbHandle.step()) {
                if (!first) oss << ",";
                first = false;
                oss << "{\"local_id\":" << dbHandle.getValue(0).intValue()
                    << ",\"create_time\":" << dbHandle.getValue(1).intValue()
                    << ",\"local_type\":" << dbHandle.getValue(2).intValue()
                    << ",\"status\":" << dbHandle.getValue(3).intValue()
                    << ",\"real_sender_id\":" << dbHandle.getValue(4).intValue()
                    << ",\"server_id\":" << dbHandle.getValue(5).intValue() << "}";
            }
            dbHandle.finalize();
        }
        oss << "]";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_message_meta", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_last_messages_batch(int64_t handle, const char* sessionIdsJson, char** outJson) {
    if (!sessionIdsJson || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto sessionIds = weflow::ParseJsonStringArray(sessionIdsJson);
        std::ostringstream oss;
        oss << "{";
        bool first = true;
        for (const auto& sid : sessionIds) {
            auto tables = weflow::ResolveSessionMessageTables(*ctx, sid);
            weflow::MsgRowData best;
            bool found = false;
            for (const auto& [meta, tbl] : tables) {
                std::string sql = "SELECT * FROM " + tbl + " ORDER BY sort_seq DESC, create_time DESC, local_id DESC LIMIT 1";
                WCDB::Handle dbHandle = meta->db->getHandle();
                WCDB::Statement stmt = weflow::MakeRawStatement(sql);
                if (dbHandle.prepare(stmt)) {
                    if (dbHandle.step()) {
                        auto r = weflow::ExtractMessageRow(dbHandle, meta, ctx->my_wxid, sid, tbl);
                        if (!found || r.create_time > best.create_time ||
                            (r.create_time == best.create_time && r.sort_seq > best.sort_seq)) {
                            best = std::move(r);
                            found = true;
                        }
                    }
                    dbHandle.finalize();
                }
            }
            if (found) {
                if (!first) oss << ",";
                first = false;
                oss << "\"" << weflow::EscapeJson(sid) << "\":" << weflow::SerializeMessageRowJson(best, false);
            }
        }
        oss << "}";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_last_messages_batch", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_scan_media_stream(
    int64_t handle,
    const char* sessionIdsJson,
    int32_t mediaType,
    int32_t beginTimestamp,
    int32_t endTimestamp,
    int32_t limit,
    int32_t offset,
    char** outJson,
    int32_t* outHasMore) {
    if (!outJson) return -1;
    *outJson = nullptr;
    if (outHasMore) *outHasMore = 0;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto sessionIds = weflow::ParseJsonStringArray(sessionIdsJson);
        std::string typeFilter = " AND local_type IN (3,43)";
        if (mediaType == 1) {
            typeFilter = " AND local_type = 3";
        } else if (mediaType == 2) {
            typeFilter = " AND local_type = 43";
        }

        int32_t lim = (limit > 0) ? limit : 100;
        int32_t off = (offset >= 0) ? offset : 0;

        std::vector<weflow::MsgRowData> mediaRows;
        for (const auto& sid : sessionIds) {
            auto tables = weflow::ResolveSessionMessageTables(*ctx, sid);
            for (const auto& [meta, tbl] : tables) {
                std::ostringstream sql;
                sql << "SELECT * FROM " << tbl << " WHERE 1=1" << typeFilter;
                if (beginTimestamp > 0) sql << " AND create_time >= " << beginTimestamp;
                if (endTimestamp > 0) sql << " AND create_time <= " << endTimestamp;
                sql << " ORDER BY sort_seq DESC, create_time DESC, local_id DESC LIMIT " << (lim + off + 1);
                WCDB::Handle dbHandle = meta->db->getHandle();
                WCDB::Statement stmt = weflow::MakeRawStatement(sql.str());
                if (dbHandle.prepare(stmt)) {
                    while (dbHandle.step()) {
                        mediaRows.push_back(weflow::ExtractMessageRow(dbHandle, meta, ctx->my_wxid, sid, tbl));
                    }
                    dbHandle.finalize();
                }
            }
        }
        std::sort(mediaRows.begin(), mediaRows.end(), [](const weflow::MsgRowData& a, const weflow::MsgRowData& b) {
            if (a.sort_seq != b.sort_seq) return a.sort_seq > b.sort_seq;
            if (a.create_time != b.create_time) return a.create_time > b.create_time;
            return a.local_id > b.local_id;
        });

        std::ostringstream oss;
        oss << "[";
        bool first = true;
        size_t s = static_cast<size_t>(off);
        size_t e = std::min(mediaRows.size(), s + static_cast<size_t>(lim));
        for (size_t i = s; i < e; ++i) {
            if (!first) oss << ",";
            first = false;
            oss << weflow::SerializeMessageRowJson(mediaRows[i], false);
        }
        oss << "]";
        if (outHasMore) {
            *outHasMore = (mediaRows.size() > s + static_cast<size_t>(lim)) ? 1 : 0;
        }
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("scan_media_stream", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_open_message_cursor_with_key(
    int64_t handle,
    const char* sessionId,
    int32_t batchSize,
    int32_t ascending,
    int64_t startTimestamp,
    int64_t endTimestamp,
    const char* projectionMode,
    int32_t hasKey,
    int64_t keySortSeq,
    int64_t keyCreateTime,
    int64_t keyLocalId,
    int64_t* outCursor) {
    if (!sessionId || !outCursor) return -1;
    *outCursor = 0;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::string proj = projectionMode ? weflow::ToLower(projectionMode) : "full";
        if (proj.empty()) proj = "full";
        if (proj != "full" && proj != "journey") {
            weflow::SetLastError("open_message_cursor", -3, "Invalid projection_mode (expected 'full' or 'journey')");
            return -3;
        }
        auto state = std::make_shared<weflow::ExportCursorState>();
        state->account_handle = handle;
        state->session_id = sessionId;
        state->batch_size = (batchSize > 0) ? batchSize : 200;
        state->ascending = (ascending != 0);
        state->start_timestamp = startTimestamp;
        state->end_timestamp = endTimestamp;
        state->projection_mode = proj;
        state->has_initial_key = (hasKey != 0);
        state->init_sort_seq = keySortSeq;
        state->init_create_time = keyCreateTime;
        state->init_local_id = keyLocalId;

        auto tables = weflow::ResolveSessionMessageTables(*ctx, sessionId);
        for (const auto& [meta, tbl] : tables) {
            weflow::CursorSource src;
            src.db_meta = meta;
            src.table_name = tbl;
            if (state->has_initial_key) {
                src.has_last_key = true;
                src.last_sort_seq = keySortSeq;
                src.last_create_time = keyCreateTime;
                src.last_local_id = keyLocalId;
            }
            weflow::RefillCursorSource(*ctx, *state, src);
            state->sources.push_back(std::move(src));
        }

        std::lock_guard<std::mutex> lk(weflow::g_cursor_mutex);
        int64_t cid = weflow::g_next_cursor_id++;
        state->cursor_id = cid;
        weflow::g_cursors[cid] = state;
        *outCursor = cid;
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("open_message_cursor", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_open_message_cursor(
    int64_t handle,
    const char* sessionId,
    int32_t batchSize,
    int32_t ascending,
    int32_t beginTimestamp,
    int32_t endTimestamp,
    int64_t* outCursor) {
    return wcdb_open_message_cursor_with_key(
        handle, sessionId, batchSize, ascending, beginTimestamp, endTimestamp, "full",
        0, 0, 0, 0, outCursor);
}

WCDB_API int32_t wcdb_fetch_message_cursor_batch(
    int64_t handle, int64_t cursor, char** outJson, int32_t* outHasMore) {
    if (!outJson || !outHasMore) return -1;
    *outJson = nullptr;
    *outHasMore = 0;

    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    std::shared_ptr<weflow::ExportCursorState> state;
    {
        std::lock_guard<std::mutex> lk(weflow::g_cursor_mutex);
        auto it = weflow::g_cursors.find(cursor);
        if (it == weflow::g_cursors.end() || it->second->account_handle != handle) {
            return -1;
        }
        state = it->second;
    }

    try {
        std::lock_guard<std::mutex> clk(state->mutex);
        std::ostringstream oss;
        oss << "[";
        bool first = true;
        int32_t count = 0;

        while (count < state->batch_size) {
            size_t bestIdx = state->sources.size();
            for (size_t i = 0; i < state->sources.size(); ++i) {
                auto& src = state->sources[i];
                if (src.buffer.empty() && !src.exhausted) {
                    weflow::RefillCursorSource(*ctx, *state, src);
                }
                if (src.buffer.empty()) continue;
                if (bestIdx == state->sources.size()) {
                    bestIdx = i;
                } else {
                    const auto& a = src.buffer.front();
                    const auto& b = state->sources[bestIdx].buffer.front();
                    bool better = false;
                    if (a.sort_seq != b.sort_seq) {
                        better = state->ascending ? (a.sort_seq < b.sort_seq) : (a.sort_seq > b.sort_seq);
                    } else if (a.create_time != b.create_time) {
                        better = state->ascending ? (a.create_time < b.create_time) : (a.create_time > b.create_time);
                    } else {
                        better = state->ascending ? (a.local_id < b.local_id) : (a.local_id > b.local_id);
                    }
                    if (better) bestIdx = i;
                }
            }
            if (bestIdx == state->sources.size()) break;

            auto item = std::move(state->sources[bestIdx].buffer.front());
            state->sources[bestIdx].buffer.pop_front();
            if (!first) oss << ",";
            first = false;
            oss << item.json_row;
            ++count;
        }
        oss << "]";

        bool anyMore = false;
        for (auto& src : state->sources) {
            if (src.buffer.empty() && !src.exhausted) {
                weflow::RefillCursorSource(*ctx, *state, src);
            }
            if (!src.buffer.empty()) {
                anyMore = true;
                break;
            }
        }
        *outHasMore = anyMore ? 1 : 0;
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("fetch_message_cursor_batch", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_close_message_cursor(int64_t /*handle*/, int64_t cursor) {
    std::lock_guard<std::mutex> lk(weflow::g_cursor_mutex);
    weflow::g_cursors.erase(cursor);
    return 0;
}

WCDB_API int32_t wcdb_get_session_timeline(int64_t handle, const char* sessionId, char** outJson) {
    if (!sessionId || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto tables = weflow::ResolveSessionMessageTables(*ctx, sessionId);
        struct BucketInfo {
            int64_t count = 0;
            int64_t first_ts = 0;
            int64_t last_ts = 0;
        };
        std::map<std::string, BucketInfo, std::greater<std::string>> buckets;

        for (const auto& [meta, tbl] : tables) {
            std::string sql =
                "SELECT strftime('%Y-%m', create_time, 'unixepoch', 'localtime') AS ym, "
                "COUNT(*) AS cnt, MIN(create_time) AS min_t, MAX(create_time) AS max_t "
                "FROM " + tbl + " WHERE create_time > 0 GROUP BY ym";
            WCDB::Handle dbHandle = meta->db->getHandle();
            WCDB::Statement stmt = weflow::MakeRawStatement(sql);
            if (dbHandle.prepare(stmt)) {
                while (dbHandle.step()) {
                    WCDB::Value ymv = dbHandle.getValue(0);
                    if (ymv.isNull()) continue;
                    std::string ym = static_cast<std::string>(ymv.textValue());
                    int64_t cnt = dbHandle.getValue(1).intValue();
                    int64_t minT = dbHandle.getValue(2).intValue();
                    int64_t maxT = dbHandle.getValue(3).intValue();
                    auto& b = buckets[ym];
                    b.count += cnt;
                    if (b.first_ts == 0 || (minT > 0 && minT < b.first_ts)) b.first_ts = minT;
                    if (maxT > b.last_ts) b.last_ts = maxT;
                }
                dbHandle.finalize();
            }
        }
        std::ostringstream oss;
        oss << "[";
        bool first = true;
        for (const auto& [ym, b] : buckets) {
            if (!first) oss << ",";
            first = false;
            oss << "{\"bucket\":\"" << weflow::EscapeJson(ym) << "\","
                << "\"count\":" << b.count << ","
                << "\"first_timestamp\":" << b.first_ts << ","
                << "\"last_timestamp\":" << b.last_ts << "}";
        }
        oss << "]";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_session_timeline", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_locate_message_anchor(
    int64_t handle, const char* sessionId, const char* anchorSpecJson, char** outJson) {
    if (!sessionId || !anchorSpecJson || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        std::string spec(anchorSpecJson);
        std::string type = weflow::ToLower(weflow::ParseJsonStringField(spec, "type", "latest"));
        int64_t targetTs = weflow::ParseJsonIntField(spec, "timestamp", 0);
        int64_t targetLocalId = weflow::ParseJsonIntField(spec, "local_id", 0);
        std::string bucket = weflow::ParseJsonStringField(spec, "bucket", "");

        auto tables = weflow::ResolveSessionMessageTables(*ctx, sessionId);
        weflow::MsgRowData best;
        bool found = false;

        for (const auto& [meta, tbl] : tables) {
            std::ostringstream sql;
            sql << "SELECT * FROM " << tbl << " WHERE 1=1";
            if (targetLocalId > 0) {
                sql << " AND local_id = " << targetLocalId;
            } else if (!bucket.empty()) {
                sql << " AND strftime('%Y-%m', create_time, 'unixepoch', 'localtime') = '"
                    << weflow::EscapeSqlLiteral(bucket) << "'";
            } else if (targetTs > 0) {
                sql << " AND create_time <= " << targetTs;
            }
            if (type == "earliest" || type == "first" || !bucket.empty()) {
                sql << " ORDER BY sort_seq ASC, create_time ASC, local_id ASC LIMIT 1";
            } else {
                sql << " ORDER BY sort_seq DESC, create_time DESC, local_id DESC LIMIT 1";
            }
            WCDB::Handle dbHandle = meta->db->getHandle();
            WCDB::Statement stmt = weflow::MakeRawStatement(sql.str());
            if (dbHandle.prepare(stmt)) {
                if (dbHandle.step()) {
                    auto r = weflow::ExtractMessageRow(dbHandle, meta, ctx->my_wxid, sessionId, tbl);
                    if (!found || r.create_time > best.create_time) {
                        best = std::move(r);
                        found = true;
                    }
                }
                dbHandle.finalize();
            }
        }
        if (!found) {
            *outJson = weflow::AllocCString("{\"found\":false}");
            return 0;
        }
        std::ostringstream oss;
        oss << "{\"found\":true,"
            << "\"sort_seq\":" << best.sort_seq << ","
            << "\"create_time\":" << best.create_time << ","
            << "\"local_id\":" << best.local_id << ","
            << "\"server_id\":" << best.server_id << ","
            << "\"message\":" << weflow::SerializeMessageRowJson(best, false) << "}";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("locate_message_anchor", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_messages_around_anchor(
    int64_t handle,
    const char* sessionId,
    int64_t anchorSortSeq,
    int64_t anchorCreateTime,
    int64_t anchorLocalId,
    int32_t beforeCount,
    int32_t afterCount,
    int32_t includeAnchor,
    char** outJson) {
    if (!sessionId || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto tables = weflow::ResolveSessionMessageTables(*ctx, sessionId);
        std::vector<weflow::MsgRowData> beforeRows;
        std::vector<weflow::MsgRowData> afterRows;

        for (const auto& [meta, tbl] : tables) {
            WCDB::Handle dbHandle = meta->db->getHandle();
            if (beforeCount > 0 || includeAnchor != 0) {
                std::string cmp = (includeAnchor != 0) ? "<=" : "<";
                std::ostringstream sql;
                sql << "SELECT * FROM " << tbl
                    << " WHERE (create_time < " << anchorCreateTime
                    << " OR (create_time = " << anchorCreateTime << " AND local_id " << cmp << " " << anchorLocalId << "))"
                    << " ORDER BY sort_seq DESC, create_time DESC, local_id DESC LIMIT "
                    << (beforeCount + (includeAnchor ? 1 : 0));
                WCDB::Statement stmt = weflow::MakeRawStatement(sql.str());
                if (dbHandle.prepare(stmt)) {
                    while (dbHandle.step()) {
                        beforeRows.push_back(weflow::ExtractMessageRow(dbHandle, meta, ctx->my_wxid, sessionId, tbl));
                    }
                    dbHandle.finalize();
                }
            }
            if (afterCount > 0) {
                std::ostringstream sql;
                sql << "SELECT * FROM " << tbl
                    << " WHERE (create_time > " << anchorCreateTime
                    << " OR (create_time = " << anchorCreateTime << " AND local_id > " << anchorLocalId << "))"
                    << " ORDER BY sort_seq ASC, create_time ASC, local_id ASC LIMIT " << afterCount;
                WCDB::Statement stmt = weflow::MakeRawStatement(sql.str());
                if (dbHandle.prepare(stmt)) {
                    while (dbHandle.step()) {
                        afterRows.push_back(weflow::ExtractMessageRow(dbHandle, meta, ctx->my_wxid, sessionId, tbl));
                    }
                    dbHandle.finalize();
                }
            }
        }

        auto ascCmp = [](const weflow::MsgRowData& a, const weflow::MsgRowData& b) {
            if (a.sort_seq != b.sort_seq) return a.sort_seq < b.sort_seq;
            if (a.create_time != b.create_time) return a.create_time < b.create_time;
            return a.local_id < b.local_id;
        };
        std::sort(beforeRows.begin(), beforeRows.end(), ascCmp);
        if (beforeRows.size() > static_cast<size_t>(beforeCount + (includeAnchor ? 1 : 0))) {
            beforeRows.erase(beforeRows.begin(),
                             beforeRows.end() - static_cast<ptrdiff_t>(beforeCount + (includeAnchor ? 1 : 0)));
        }
        std::sort(afterRows.begin(), afterRows.end(), ascCmp);
        if (afterRows.size() > static_cast<size_t>(afterCount)) {
            afterRows.resize(static_cast<size_t>(afterCount));
        }

        std::vector<weflow::MsgRowData> combined = std::move(beforeRows);
        for (auto& r : afterRows) combined.push_back(std::move(r));

        std::ostringstream oss;
        oss << "{\"anchor\":{\"sort_seq\":" << anchorSortSeq
            << ",\"create_time\":" << anchorCreateTime
            << ",\"local_id\":" << anchorLocalId << "},\"messages\":[";
        for (size_t i = 0; i < combined.size(); ++i) {
            if (i > 0) oss << ",";
            oss << weflow::SerializeMessageRowJson(combined[i], false);
        }
        oss << "]}";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_messages_around_anchor", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_probe_fts_schema(int64_t handle, char** outJson) {
    if (!outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        auto ftsDb = weflow::ResolveDbByKindOrPath(*ctx, "message_fts.db");
        if (!ftsDb) {
            *outJson = weflow::AllocCString("{\"available\":false,\"tables\":[]}");
            return 0;
        }
        WCDB::Handle dbHandle = ftsDb->getHandle();
        WCDB::Statement stmt = weflow::MakeRawStatement(
            "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name");
        std::ostringstream oss;
        oss << "{\"available\":true,\"tables\":[";
        bool first = true;
        if (dbHandle.prepare(stmt)) {
            while (dbHandle.step()) {
                WCDB::Value v = dbHandle.getValue(0);
                if (!v.isNull()) {
                    if (!first) oss << ",";
                    first = false;
                    oss << "\"" << weflow::EscapeJson(static_cast<std::string>(v.textValue())) << "\"";
                }
            }
            dbHandle.finalize();
        }
        oss << "]}";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("probe_fts_schema", -2, e.what());
        return -2;
    }
}

static int32_t SearchMessagesCore(
    int64_t handle,
    const char* sessionId,
    const char* keyword,
    int32_t limit,
    int32_t offset,
    int64_t startTime,
    int64_t endTime,
    char** outJson) {
    if (!outJson) return -1;
    *outJson = nullptr;
    if (!keyword) {
        *outJson = weflow::AllocCString("[]");
        return 0;
    }
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        std::string kw(keyword);
        int32_t lim = (limit > 0) ? limit : 50;
        int32_t off = (offset > 0) ? offset : 0;
        std::string sidStr = (sessionId ? std::string(sessionId) : "");

        std::vector<weflow::MsgRowData> matches;

        // 1. 尝试使用 FTS 索引
        auto ftsDb = weflow::ResolveDbByKindOrPath(*ctx, "message_fts.db");
        bool ftsUsed = false;
        if (ftsDb) {
            try {
                WCDB::Handle ftsHandle = ftsDb->getHandle();
                std::unordered_map<int64_t, std::string> id2Name;
                int64_t targetSessionRowId = -1;
                WCDB::Statement sNames = weflow::MakeRawStatement("SELECT rowid, username FROM name2id");
                if (ftsHandle.prepare(sNames)) {
                    while (ftsHandle.step()) {
                        int64_t rid = ftsHandle.getValue(0).intValue();
                        std::string uname = static_cast<std::string>(ftsHandle.getValue(1).textValue());
                        id2Name[rid] = uname;
                        if (!sidStr.empty() && uname == sidStr) {
                            targetSessionRowId = rid;
                        }
                    }
                    ftsHandle.finalize();
                }

                if (sidStr.empty() || targetSessionRowId >= 0) {
                    std::vector<std::pair<std::string, std::string>> ftsPairs;
                    WCDB::Statement sTables = weflow::MakeRawStatement(
                        "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'message_fts_%_content'");
                    if (ftsHandle.prepare(sTables)) {
                        while (ftsHandle.step()) {
                            std::string cTbl = static_cast<std::string>(ftsHandle.getValue(0).textValue());
                            std::string prefix = cTbl.substr(0, cTbl.rfind("_content"));
                            std::string aTbl = prefix + "_aux_0";
                            ftsPairs.emplace_back(cTbl, aTbl);
                        }
                        ftsHandle.finalize();
                    }

                    if (!ftsPairs.empty()) {
                        for (const auto& [cTbl, aTbl] : ftsPairs) {
                            std::ostringstream sql;
                            sql << "SELECT c.c0, a.sort_seq, a.session_id, a.message_local_id, c.c3, c.c5, c.c6 FROM \""
                                << cTbl << "\" c JOIN \"" << aTbl << "\" a ON c.id = a.rowid WHERE c.c0 LIKE '%"
                                << weflow::EscapeSqlLiteral(kw) << "%'";
                            if (targetSessionRowId >= 0) {
                                sql << " AND a.session_id = " << targetSessionRowId;
                            }
                            if (startTime > 0) {
                                sql << " AND a.sort_seq >= " << (startTime > 1e11 ? startTime : startTime * 1000LL);
                            }
                            if (endTime > 0) {
                                sql << " AND a.sort_seq <= " << (endTime > 1e11 ? endTime : endTime * 1000LL);
                            }
                            sql << " ORDER BY a.sort_seq DESC LIMIT " << (lim + off);
                            WCDB::Statement sQuery = weflow::MakeRawStatement(sql.str());
                            if (ftsHandle.prepare(sQuery)) {
                                while (ftsHandle.step()) {
                                    weflow::MsgRowData r;
                                    r.message_content = static_cast<std::string>(ftsHandle.getValue(0).textValue());
                                    r.sort_seq = ftsHandle.getValue(1).intValue();
                                    r.create_time = (r.sort_seq > 1e11) ? (r.sort_seq / 1000LL) : r.sort_seq;
                                    int64_t sessRid = ftsHandle.getValue(2).intValue();
                                    auto itU = id2Name.find(sessRid);
                                    if (itU != id2Name.end()) {
                                        r.session_id = itU->second;
                                    } else if (!sidStr.empty()) {
                                        r.session_id = sidStr;
                                    } else {
                                        r.session_id = std::to_string(sessRid);
                                    }
                                    r.local_id = ftsHandle.getValue(3).intValue();
                                    r.local_type = 1;
                                    r.real_sender_id = ftsHandle.getValue(4).intValue();
                                    if (!ftsHandle.getValue(5).isNull()) {
                                        r.sender_username = static_cast<std::string>(ftsHandle.getValue(5).textValue());
                                    }
                                    if (r.sender_username.empty()) {
                                        auto itSender = id2Name.find(r.real_sender_id);
                                        if (itSender != id2Name.end()) r.sender_username = itSender->second;
                                    }
                                    r.computed_is_send = (!ctx->my_wxid.empty() && r.sender_username == ctx->my_wxid);
                                    matches.push_back(std::move(r));
                                }
                                ftsHandle.finalize();
                            }
                        }
                        if (!matches.empty()) {
                            ftsUsed = true;
                        }
                    }
                }
            } catch (...) {
            }
        }

        // 2. 普通消息表回退查询
        if (!ftsUsed) {
            std::vector<std::pair<std::shared_ptr<weflow::MessageDbMeta>, std::string>> tables;
            std::unordered_map<std::string, std::string> tableToSession;

            if (!sidStr.empty()) {
                tables = weflow::ResolveSessionMessageTables(*ctx, sidStr);
                for (const auto& [meta, tbl] : tables) {
                    tableToSession[tbl] = sidStr;
                }
            } else {
                std::unordered_map<std::string, std::string> md5ToUsername;
                if (ctx->session_db) {
                    WCDB::Handle sessHandle = ctx->session_db->getHandle();
                    WCDB::Statement stmt = weflow::MakeRawStatement("SELECT username FROM SessionAbstract");
                    if (sessHandle.prepare(stmt)) {
                        while (sessHandle.step()) {
                            if (!sessHandle.getValue(0).isNull()) {
                                std::string u = static_cast<std::string>(sessHandle.getValue(0).textValue());
                                md5ToUsername[weflow::ToLower(weflow::Md5HexLower(u))] = u;
                            }
                        }
                        sessHandle.finalize();
                    }
                }
                auto catalog = weflow::EnsureMessageCatalog(*ctx, false);
                if (catalog) {
                    for (const auto& meta : catalog->dbs) {
                        for (const auto& [lowerTbl, actualTbl] : meta->canonical_table_names) {
                            if (lowerTbl.rfind("msg_", 0) == 0) {
                                tables.emplace_back(meta, actualTbl);
                                std::string hashPart = lowerTbl.substr(4);
                                auto itMd5 = md5ToUsername.find(hashPart);
                                if (itMd5 != md5ToUsername.end()) {
                                    tableToSession[actualTbl] = itMd5->second;
                                }
                            }
                        }
                    }
                }
            }

            for (const auto& [meta, tbl] : tables) {
                std::string currentSid = sidStr;
                auto itSid = tableToSession.find(tbl);
                if (itSid != tableToSession.end()) currentSid = itSid->second;

                std::ostringstream sql;
                sql << "SELECT * FROM " << tbl << " WHERE 1=1";
                if (startTime > 0) sql << " AND create_time >= " << startTime;
                if (endTime > 0) sql << " AND create_time <= " << endTime;
                if (!kw.empty()) {
                    sql << " AND (message_content LIKE '%" << weflow::EscapeSqlLiteral(kw) << "%'"
                        << " OR compress_content LIKE '%" << weflow::EscapeSqlLiteral(kw) << "%'"
                        << " OR wcdb_ct_message_content = 4)";
                }
                sql << " ORDER BY sort_seq DESC, create_time DESC, local_id DESC LIMIT " << (lim + off);
                WCDB::Handle dbHandle = meta->db->getHandle();
                WCDB::Statement stmt = weflow::MakeRawStatement(sql.str());
                if (dbHandle.prepare(stmt)) {
                    while (dbHandle.step()) {
                        auto r = weflow::ExtractMessageRow(dbHandle, meta, ctx->my_wxid, currentSid, tbl);
                        if (!kw.empty() && r.message_content.find(kw) == std::string::npos) {
                            continue;
                        }
                        matches.push_back(std::move(r));
                        if (static_cast<int32_t>(matches.size()) >= (lim + off) * 2) break;
                    }
                    dbHandle.finalize();
                }
                if (static_cast<int32_t>(matches.size()) >= (lim + off) * 2) break;
            }
        }

        std::sort(matches.begin(), matches.end(), [](const weflow::MsgRowData& a, const weflow::MsgRowData& b) {
            if (a.sort_seq != b.sort_seq) return a.sort_seq > b.sort_seq;
            if (a.create_time != b.create_time) return a.create_time > b.create_time;
            return a.local_id > b.local_id;
        });

        std::ostringstream oss;
        oss << "[";
        size_t count = 0;
        for (size_t i = static_cast<size_t>(off); i < matches.size() && count < static_cast<size_t>(lim); ++i, ++count) {
            if (count > 0) oss << ",";
            oss << weflow::SerializeMessageRowJson(matches[i], false);
        }
        oss << "]";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("search_messages", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_search_messages_in_session(
    int64_t handle,
    const char* sessionId,
    const char* keyword,
    int32_t limit,
    int64_t startTime,
    int64_t endTime,
    char** outJson) {
    return SearchMessagesCore(handle, sessionId, keyword, limit, 0, startTime, endTime, outJson);
}

WCDB_API int32_t wcdb_search_messages(
    int64_t handle,
    const char* sessionId,
    const char* keyword,
    int32_t limit,
    int32_t offset,
    int32_t beginTimestamp,
    int32_t endTimestamp,
    char** outJson) {
    return SearchMessagesCore(handle, sessionId, keyword, limit, offset, beginTimestamp, endTimestamp, outJson);
}

WCDB_API int32_t wcdb_search_fts_messages(
    int64_t handle,
    const char* keywordJson,
    const char* sessionId,
    int32_t limit,
    int32_t offset,
    int64_t startTime,
    int64_t endTime,
    char** outJson) {
    if (!keywordJson || !outJson) return -1;
    *outJson = nullptr;
    auto keywords = weflow::ParseJsonStringArray(keywordJson);
    std::string kw = keywords.empty() ? std::string(keywordJson) : keywords.front();
    return SearchMessagesCore(handle, sessionId, kw.c_str(), limit, offset, startTime, endTime, outJson);
}

WCDB_API int32_t wcdb_get_session_activity_heatmap(
    int64_t handle, const char* sessionId, int64_t startTime, int64_t endTime, char** outJson) {
    if (!sessionId || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto tables = weflow::ResolveSessionMessageTables(*ctx, sessionId);
        std::map<std::string, int64_t> dayCounts;
        for (const auto& [meta, tbl] : tables) {
            std::ostringstream sql;
            sql << "SELECT strftime('%Y-%m-%d', create_time, 'unixepoch', 'localtime') AS d, COUNT(*) "
                << "FROM " << tbl << " WHERE create_time > 0";
            if (startTime > 0) sql << " AND create_time >= " << startTime;
            if (endTime > 0) sql << " AND create_time <= " << endTime;
            sql << " GROUP BY d";
            WCDB::Handle dbHandle = meta->db->getHandle();
            WCDB::Statement stmt = weflow::MakeRawStatement(sql.str());
            if (dbHandle.prepare(stmt)) {
                while (dbHandle.step()) {
                    WCDB::Value dv = dbHandle.getValue(0);
                    if (!dv.isNull()) {
                        dayCounts[static_cast<std::string>(dv.textValue())] += dbHandle.getValue(1).intValue();
                    }
                }
                dbHandle.finalize();
            }
        }
        std::ostringstream oss;
        oss << "{";
        bool first = true;
        for (const auto& [d, cnt] : dayCounts) {
            if (!first) oss << ",";
            first = false;
            oss << "\"" << weflow::EscapeJson(d) << "\":" << cnt;
        }
        oss << "}";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_session_activity_heatmap", -2, e.what());
        return -2;
    }
}

} // extern "C"
