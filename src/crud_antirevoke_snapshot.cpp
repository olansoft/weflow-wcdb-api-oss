#include "internal.hpp"

namespace weflow {

namespace {

std::mutex g_anti_revoke_mutex;
std::string g_anti_revoke_self_label = "你";
std::string g_anti_revoke_other_prefix = "对方";

bool IsValidSqlIdentifier(const std::string& name) {
    if (name.empty() || name.size() > 128) return false;
    for (unsigned char c : name) {
        if (!std::isalnum(c) && c != '_') return false;
    }
    return true;
}

std::string ImportTableSnapshotIntoDb(
    WCDB::Database& db, const std::string& tableName, const std::string& inputPath) {
    std::ifstream in(inputPath, std::ios::in | std::ios::binary);
    if (!in.is_open()) {
        return "{\"success\":false,\"error\":\"open_input_failed\"}";
    }
    std::string header;
    if (!std::getline(in, header)) {
        return "{\"success\":false,\"error\":\"invalid_snapshot\"}";
    }
    if (!header.empty() && header.back() == '\r') header.pop_back();
    if (header != "WEFLOW_SNAPSHOT_V1") {
        return "{\"success\":false,\"error\":\"invalid_snapshot\"}";
    }
    std::string colLine;
    if (!std::getline(in, colLine)) {
        return "{\"success\":false,\"error\":\"empty_columns\"}";
    }
    if (!colLine.empty() && colLine.back() == '\r') colLine.pop_back();

    std::vector<std::string> cols;
    {
        std::istringstream ss(colLine);
        std::string col;
        while (std::getline(ss, col, '\t')) {
            if (!col.empty()) cols.push_back(col);
        }
    }
    if (cols.empty()) {
        return "{\"success\":false,\"error\":\"empty_columns\"}";
    }

    std::ostringstream insertSql;
    insertSql << "INSERT OR IGNORE INTO " << tableName << " (";
    for (size_t i = 0; i < cols.size(); ++i) {
        if (i > 0) insertSql << ",";
        insertSql << cols[i];
    }
    insertSql << ") VALUES (";
    for (size_t i = 0; i < cols.size(); ++i) {
        if (i > 0) insertSql << ",";
        insertSql << "?";
    }
    insertSql << ")";

    int64_t totalRows = 0, insertedRows = 0, ignoredRows = 0, malformedRows = 0;
    WCDB::Handle rootHandle = db.getHandle();
    bool txOk = rootHandle.asHandleOperation()->runTransaction([&](WCDB::Handle& handle) -> bool {
        WCDB::Statement stmt = MakeRawStatement(insertSql.str());
        if (!handle.prepare(stmt)) return false;

        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            ++totalRows;

            std::vector<std::string> fields;
            std::istringstream ls(line);
            std::string tok;
            while (std::getline(ls, tok, '\t')) {
                fields.push_back(tok);
            }
            if (fields.size() != cols.size()) {
                ++malformedRows;
                continue;
            }
            handle.reset();
            handle.clearBindings();
            std::vector<std::string> decodedStrings(cols.size());
            std::vector<std::vector<uint8_t>> decodedBlobs(cols.size());

            for (size_t i = 0; i < cols.size(); ++i) {
                int bindIdx = static_cast<int>(i + 1);
                const std::string& f = fields[i];
                if (f == "n" || f.empty()) {
                    handle.bindNull(bindIdx);
                } else if (f.rfind("i:", 0) == 0) {
                    int64_t iv = std::strtoll(f.c_str() + 2, nullptr, 10);
                    handle.bindInteger(iv, bindIdx);
                } else if (f.rfind("f:", 0) == 0) {
                    double dv = std::strtod(f.c_str() + 2, nullptr);
                    handle.bindDouble(dv, bindIdx);
                } else if (f.rfind("t:", 0) == 0) {
                    auto bytes = Base64Decode(f.substr(2));
                    decodedStrings[i].assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                    WCDB::UnsafeStringView sv(decodedStrings[i].c_str(), decodedStrings[i].size());
                    handle.bindText(sv, bindIdx);
                } else if (f.rfind("b:", 0) == 0) {
                    decodedBlobs[i] = Base64Decode(f.substr(2));
                    WCDB::UnsafeData bd(decodedBlobs[i].data(), decodedBlobs[i].size());
                    handle.bindBLOB(bd, bindIdx);
                } else {
                    handle.bindNull(bindIdx);
                }
            }
            if (handle.rawStep()) {
                if (handle.getChanges() > 0) ++insertedRows;
                else ++ignoredRows;
            } else {
                ++ignoredRows;
            }
        }
        handle.finalize();
        return true;
    });

    if (!txOk) {
        return "{\"success\":false,\"error\":\"import_transaction_failed\"}";
    }
    std::ostringstream oss;
    oss << "{\"success\":true,\"rows\":" << totalRows
        << ",\"inserted\":" << insertedRows
        << ",\"ignored\":" << ignoredRows
        << ",\"malformed\":" << malformedRows
        << ",\"columns\":" << cols.size() << "}";
    return oss.str();
}

} // namespace

std::string GetAntiRevokeSelfLabel() {
    std::lock_guard<std::mutex> lk(g_anti_revoke_mutex);
    return g_anti_revoke_self_label;
}

std::string GetAntiRevokeOtherPrefix() {
    std::lock_guard<std::mutex> lk(g_anti_revoke_mutex);
    return g_anti_revoke_other_prefix;
}

} // namespace weflow

extern "C" {

// ============================================================================
// Table Introspection & WEFLOW_SNAPSHOT_V1 Export/Import
// ============================================================================

WCDB_API int32_t wcdb_list_tables(
    int64_t handle, const char* kind, const char* dbPath, char** outJson) {
    if (!outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::string target = (dbPath && dbPath[0] != '\0') ? std::string(dbPath) : (kind ? std::string(kind) : "session");
        auto db = weflow::ResolveDbByKindOrPath(*ctx, target);
        if (!db) {
            *outJson = weflow::AllocCString("[]");
            return 0;
        }
        WCDB::Handle dbHandle = db->getHandle();
        WCDB::Statement stmt = weflow::MakeRawStatement(
            "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name");
        std::ostringstream oss;
        oss << "[";
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
        oss << "]";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("list_tables", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_table_schema(
    int64_t handle, const char* kind, const char* dbPath, const char* tableName, char** outJson) {
    if (!tableName || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    if (!weflow::IsValidSqlIdentifier(tableName)) {
        *outJson = weflow::AllocCString("{\"success\":false,\"error\":\"invalid_table_name\"}");
        return 0;
    }
    try {
        std::string target = (dbPath && dbPath[0] != '\0') ? std::string(dbPath) : (kind ? std::string(kind) : "session");
        auto db = weflow::ResolveDbByKindOrPath(*ctx, target);
        if (!db) {
            *outJson = weflow::AllocCString("{\"success\":false,\"error\":\"db_not_found\"}");
            return 0;
        }
        WCDB::Handle dbHandle = db->getHandle();
        std::string sql = "SELECT sql FROM sqlite_master WHERE type='table' AND name='" +
                          weflow::EscapeSqlLiteral(tableName) + "' LIMIT 1";
        WCDB::Statement stmt = weflow::MakeRawStatement(sql);
        std::string schemaSql;
        if (dbHandle.prepare(stmt)) {
            if (dbHandle.step() && !dbHandle.getValue(0).isNull()) {
                schemaSql = static_cast<std::string>(dbHandle.getValue(0).textValue());
            }
            dbHandle.finalize();
        }
        if (schemaSql.empty()) {
            *outJson = weflow::AllocCString("{\"success\":false,\"error\":\"table_not_found\"}");
            return 0;
        }
        std::ostringstream oss;
        oss << "{\"success\":true,\"schema\":\"" << weflow::EscapeJson(schemaSql) << "\"}";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception&) {
        *outJson = weflow::AllocCString("{\"success\":false,\"error\":\"exception\"}");
        return -2;
    }
}

WCDB_API int32_t wcdb_get_message_table_columns(
    int64_t handle, const char* dbPath, const char* tableName, char** outJson) {
    if (!tableName || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::shared_ptr<WCDB::Database> db;
        std::string tbl = tableName;
        if (dbPath && dbPath[0] != '\0') {
            db = weflow::ResolveDbByKindOrPath(*ctx, dbPath);
        } else {
            auto tables = weflow::ResolveSessionMessageTables(*ctx, tableName);
            if (!tables.empty()) {
                db = tables.front().first->db;
                tbl = tables.front().second;
            }
        }
        if (!db) {
            *outJson = weflow::AllocCString("[]");
            return 0;
        }
        WCDB::Handle dbHandle = db->getHandle();
        WCDB::Statement stmt = weflow::MakeRawStatement("PRAGMA table_info(" + tbl + ")");
        std::ostringstream oss;
        oss << "[";
        bool first = true;
        if (dbHandle.prepare(stmt)) {
            while (dbHandle.step()) {
                WCDB::Value nv = dbHandle.getValue(1);
                if (!nv.isNull()) {
                    if (!first) oss << ",";
                    first = false;
                    oss << "\"" << weflow::EscapeJson(static_cast<std::string>(nv.textValue())) << "\"";
                }
            }
            dbHandle.finalize();
        }
        oss << "]";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_message_table_columns", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_message_table_time_range(
    int64_t handle, const char* dbPath, const char* tableName, char** outJson) {
    if (!tableName || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::shared_ptr<WCDB::Database> db;
        std::string tbl = tableName;
        if (dbPath && dbPath[0] != '\0') {
            db = weflow::ResolveDbByKindOrPath(*ctx, dbPath);
        } else {
            auto tables = weflow::ResolveSessionMessageTables(*ctx, tableName);
            if (!tables.empty()) {
                db = tables.front().first->db;
                tbl = tables.front().second;
            }
        }
        if (!db) {
            *outJson = weflow::AllocCString("{\"column\":\"\",\"first_ts\":0,\"last_ts\":0}");
            return 0;
        }
        int64_t firstTs = 0, lastTs = 0;
        WCDB::Handle dbHandle = db->getHandle();
        WCDB::Statement stmt = weflow::MakeRawStatement(
            "SELECT MIN(create_time), MAX(create_time) FROM " + tbl + " WHERE create_time > 0");
        if (dbHandle.prepare(stmt)) {
            if (dbHandle.step()) {
                int64_t mn = dbHandle.getValue(0).intValue();
                int64_t mx = dbHandle.getValue(1).intValue();
                if (mn > 0 && (firstTs == 0 || mn < firstTs)) firstTs = mn;
                if (mx > lastTs) lastTs = mx;
            }
            dbHandle.finalize();
        }
        std::ostringstream oss;
        oss << "{\"column\":\"create_time\",\"first_ts\":" << firstTs << ",\"last_ts\":" << lastTs << "}";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_message_table_time_range", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_export_table_snapshot(
    int64_t handle,
    const char* kind,
    const char* dbPath,
    const char* tableName,
    const char* outputPath,
    char** outJson) {
    if (!tableName || !outputPath || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    if (!weflow::IsValidSqlIdentifier(tableName)) {
        *outJson = weflow::AllocCString("{\"success\":false,\"error\":\"invalid_table_name\"}");
        return 0;
    }
    try {
        std::string target = (dbPath && dbPath[0] != '\0') ? std::string(dbPath) : (kind ? std::string(kind) : "session");
        auto db = weflow::ResolveDbByKindOrPath(*ctx, target);
        if (!db) {
            *outJson = weflow::AllocCString("{\"success\":false,\"error\":\"db_not_found\"}");
            return 0;
        }
        std::error_code ec;
        std::filesystem::path outP(outputPath);
        if (outP.has_parent_path()) {
            std::filesystem::create_directories(outP.parent_path(), ec);
        }
        std::ofstream out(outputPath, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            *outJson = weflow::AllocCString("{\"success\":false,\"error\":\"open_output_failed\"}");
            return 0;
        }

        WCDB::Handle dbHandle = db->getHandle();
        WCDB::Statement stmt = weflow::MakeRawStatement("SELECT * FROM " + std::string(tableName));
        if (!dbHandle.prepare(stmt)) {
            *outJson = weflow::AllocCString("{\"success\":false,\"error\":\"prepare_select_failed\"}");
            return 0;
        }

        int numCols = dbHandle.getNumberOfColumns();
        out << "WEFLOW_SNAPSHOT_V1\n";
        for (int i = 0; i < numCols; ++i) {
            if (i > 0) out << "\t";
            out << static_cast<std::string>(dbHandle.getColumnName(i));
        }
        out << "\n";

        int64_t rows = 0;
        while (dbHandle.step()) {
            for (int i = 0; i < numCols; ++i) {
                if (i > 0) out << "\t";
                WCDB::Value v = dbHandle.getValue(i);
                switch (v.getType()) {
                    case WCDB::Value::Type::Null:
                        out << "n";
                        break;
                    case WCDB::Value::Type::Integer:
                        out << "i:" << v.intValue();
                        break;
                    case WCDB::Value::Type::Float:
                        out << "f:" << v.floatValue();
                        break;
                    case WCDB::Value::Type::Text: {
                        std::string s = static_cast<std::string>(v.textValue());
                        out << "t:" << weflow::Base64Encode(reinterpret_cast<const uint8_t*>(s.data()), s.size());
                        break;
                    }
                    case WCDB::Value::Type::BLOB: {
                        WCDB::Data d = v.blobValue();
                        out << "b:" << weflow::Base64Encode(d.buffer(), d.size());
                        break;
                    }
                }
            }
            out << "\n";
            ++rows;
        }
        dbHandle.finalize();
        out.close();

        std::ostringstream oss;
        oss << "{\"success\":true,\"rows\":" << rows << ",\"columns\":" << numCols << "}";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception&) {
        *outJson = weflow::AllocCString("{\"success\":false,\"error\":\"exception\"}");
        return -2;
    }
}

WCDB_API int32_t wcdb_import_table_snapshot(
    int64_t handle,
    const char* kind,
    const char* dbPath,
    const char* tableName,
    const char* inputPath,
    char** outJson) {
    if (!tableName || !inputPath || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    if (!weflow::IsValidSqlIdentifier(tableName)) {
        *outJson = weflow::AllocCString("{\"success\":false,\"error\":\"invalid_table_name\"}");
        return 0;
    }
    try {
        std::string target = (dbPath && dbPath[0] != '\0') ? std::string(dbPath) : (kind ? std::string(kind) : "session");
        auto db = weflow::ResolveDbByKindOrPath(*ctx, target);
        if (!db) {
            *outJson = weflow::AllocCString("{\"success\":false,\"error\":\"db_not_found\"}");
            return 0;
        }
        std::string res = weflow::ImportTableSnapshotIntoDb(*db, tableName, inputPath);
        *outJson = weflow::AllocCString(res);
        return 0;
    } catch (const std::exception&) {
        *outJson = weflow::AllocCString("{\"success\":false,\"error\":\"exception\"}");
        return -2;
    }
}

WCDB_API int32_t wcdb_import_table_snapshot_with_schema(
    int64_t handle,
    const char* kind,
    const char* dbPath,
    const char* tableName,
    const char* inputPath,
    const char* createTableSql,
    char** outJson) {
    if (!tableName || !createTableSql || !inputPath || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    if (!weflow::IsValidSqlIdentifier(tableName)) {
        *outJson = weflow::AllocCString("{\"success\":false,\"error\":\"invalid_table_name\"}");
        return 0;
    }
    std::string lowerSchema = weflow::ToLower(createTableSql);
    if (lowerSchema.find("create table") == std::string::npos ||
        lowerSchema.find("delete ") != std::string::npos ||
        lowerSchema.find("update ") != std::string::npos ||
        lowerSchema.find("insert ") != std::string::npos ||
        lowerSchema.find("attach ") != std::string::npos ||
        lowerSchema.find("pragma ") != std::string::npos) {
        *outJson = weflow::AllocCString("{\"success\":false,\"error\":\"missing_or_invalid_schema\"}");
        return 0;
    }

    try {
        std::string target = (dbPath && dbPath[0] != '\0') ? std::string(dbPath) : (kind ? std::string(kind) : "session");
        auto db = weflow::ResolveDbByKindOrPath(*ctx, target);
        if (!db) {
            *outJson = weflow::AllocCString("{\"success\":false,\"error\":\"db_not_found\"}");
            return 0;
        }
        WCDB::Handle dbHandle = db->getHandle();
        if (!dbHandle.asHandleOperation()->execute(weflow::MakeRawStatement(createTableSql))) {
            *outJson = weflow::AllocCString("{\"success\":false,\"error\":\"create_table_failed\"}");
            return 0;
        }
        std::string res = weflow::ImportTableSnapshotIntoDb(*db, tableName, inputPath);
        *outJson = weflow::AllocCString(res);
        return 0;
    } catch (const std::exception&) {
        *outJson = weflow::AllocCString("{\"success\":false,\"error\":\"exception\"}");
        return -2;
    }
}

WCDB_API int32_t wcdb_exec_query(
    int64_t handle, const char* kind, const char* path, const char* sql, char** outJson) {
    if (!sql || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::string target = (path && path[0] != '\0') ? std::string(path) : (kind ? std::string(kind) : "session");
        auto db = weflow::ResolveDbByKindOrPath(*ctx, target);
        if (!db) {
            *outJson = weflow::AllocCString("[]");
            return 0;
        }
        WCDB::Handle dbHandle = db->getHandle();
        WCDB::Statement stmt = weflow::MakeRawStatement(sql);
        std::ostringstream oss;
        oss << "[";
        bool firstRow = true;
        if (dbHandle.prepare(stmt)) {
            int numCols = dbHandle.getNumberOfColumns();
            while (dbHandle.step()) {
                if (!firstRow) oss << ",";
                firstRow = false;
                oss << "{";
                for (int i = 0; i < numCols; ++i) {
                    if (i > 0) oss << ",";
                    std::string colName = static_cast<std::string>(dbHandle.getColumnName(i));
                    oss << "\"" << weflow::EscapeJson(colName) << "\":";
                    WCDB::Value v = dbHandle.getValue(i);
                    switch (v.getType()) {
                        case WCDB::Value::Type::Null:
                            oss << "null";
                            break;
                        case WCDB::Value::Type::Integer:
                            oss << v.intValue();
                            break;
                        case WCDB::Value::Type::Float:
                            oss << v.floatValue();
                            break;
                        case WCDB::Value::Type::Text:
                            oss << "\"" << weflow::EscapeJson(static_cast<std::string>(v.textValue())) << "\"";
                            break;
                        case WCDB::Value::Type::BLOB: {
                            WCDB::Data d = v.blobValue();
                            oss << "\"" << weflow::BytesToHex(d.buffer(), d.size()) << "\"";
                            break;
                        }
                    }
                }
                oss << "}";
            }
            dbHandle.finalize();
        }
        oss << "]";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("exec_query", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_test_create_custom_db(
    int64_t handle, const char* dbPath, const char* tableName, char** outJson) {
    if (!dbPath || !tableName || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        WCDB::UnsafeStringView pv(dbPath);
        WCDB::Database db(pv, false);
        if (!ctx->raw_key.empty()) {
            WCDB::UnsafeData kd(ctx->raw_key.data(), ctx->raw_key.size());
            db.setCipherKey(kd, ctx->cipher_page_size, WCDB::Database::CipherVersion::DefaultVersion);
        }
        WCDB::Handle h = db.getHandle();
        std::string sql = "CREATE TABLE IF NOT EXISTS " + std::string(tableName) + " (id INTEGER PRIMARY KEY, val TEXT)";
        bool ok = h.asHandleOperation()->execute(weflow::MakeRawStatement(sql));
        *outJson = weflow::AllocCString(ok ? "{\"success\":true}" : "{\"success\":false}");
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("test_create_custom_db", -2, e.what());
        return -2;
    }
}

// ============================================================================
// Message & Session CRUD
// ============================================================================

WCDB_API int32_t wcdb_insert_text_message(
    int64_t handle,
    const char* sessionId,
    const char* content,
    int64_t status,
    int64_t createTime,
    char** outJson) {
    if (!sessionId || !content || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto tables = weflow::ResolveSessionMessageTables(*ctx, sessionId);
        if (tables.empty()) {
            *outJson = weflow::AllocCString("{\"success\":false,\"error\":\"message_table_not_found\"}");
            return 0;
        }
        auto [meta, tbl] = tables.front();
        int64_t ct = (createTime > 0) ? createTime : static_cast<int64_t>(std::time(nullptr));
        int64_t st = (status > 0) ? status : 2;
        int64_t senderId = 0;
        if (!ctx->my_wxid.empty()) {
            auto it = meta->username_to_id.find(ctx->my_wxid);
            if (it != meta->username_to_id.end()) {
                senderId = it->second;
            }
        }
        int64_t sortSeq = ct * 1000;
        WCDB::Handle dbHandle = meta->db->getHandle();
        std::ostringstream sql;
        sql << "INSERT INTO " << tbl
            << " (server_id, local_type, sort_seq, real_sender_id, create_time, status, message_content) VALUES ("
            << "0, 1, " << sortSeq << ", " << senderId << ", " << ct << ", " << st << ", '"
            << weflow::EscapeSqlLiteral(content) << "')";
        if (!dbHandle.asHandleOperation()->execute(weflow::MakeRawStatement(sql.str()))) {
            *outJson = weflow::AllocCString("{\"success\":false,\"error\":\"insert_failed\"}");
            return 0;
        }
        int64_t newLocalId = dbHandle.getLastInsertedRowID();
        std::ostringstream oss;
        oss << "{\"success\":true,\"local_id\":" << newLocalId << ",\"create_time\":" << ct << "}";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("insert_text_message", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_update_message_content(
    int64_t handle,
    const char* sessionId,
    int64_t localId,
    int64_t createTimeHint,
    const char* newContent,
    char** outJson) {
    if (!sessionId || !newContent || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto tables = weflow::ResolveSessionMessageTables(*ctx, sessionId);
        for (const auto& [meta, tbl] : tables) {
            WCDB::Handle dbHandle = meta->db->getHandle();
            std::ostringstream sql;
            sql << "UPDATE " << tbl
                << " SET message_content = '" << weflow::EscapeSqlLiteral(newContent) << "', "
                << "compress_content = NULL, WCDB_CT_message_content = 0 "
                << "WHERE local_id = " << localId;
            if (createTimeHint > 0) sql << " AND create_time = " << createTimeHint;
            if (dbHandle.asHandleOperation()->execute(weflow::MakeRawStatement(sql.str())) &&
                dbHandle.getChanges() > 0) {
                *outJson = weflow::AllocCString("{\"success\":true}");
                return 0;
            }
        }
        *outJson = weflow::AllocCString("{\"success\":false,\"error\":\"message_not_found\"}");
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("update_message_content", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_update_message_create_time(
    int64_t handle,
    const char* sessionId,
    int64_t localId,
    int64_t oldCreateTimeHint,
    int64_t newCreateTime,
    char** outJson) {
    if (!sessionId || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto tables = weflow::ResolveSessionMessageTables(*ctx, sessionId);
        for (const auto& [meta, tbl] : tables) {
            WCDB::Handle dbHandle = meta->db->getHandle();
            std::ostringstream sql;
            sql << "UPDATE " << tbl << " SET create_time = " << newCreateTime
                << " WHERE local_id = " << localId;
            if (oldCreateTimeHint > 0) sql << " AND create_time = " << oldCreateTimeHint;
            if (dbHandle.asHandleOperation()->execute(weflow::MakeRawStatement(sql.str())) &&
                dbHandle.getChanges() > 0) {
                *outJson = weflow::AllocCString("{\"success\":true}");
                return 0;
            }
        }
        *outJson = weflow::AllocCString("{\"success\":false,\"error\":\"message_not_found\"}");
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("update_message_create_time", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_update_message_sender(
    int64_t handle,
    const char* sessionId,
    int64_t localId,
    int64_t createTimeHint,
    const char* newSenderUsername,
    char** outJson) {
    if (!sessionId || !newSenderUsername || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto tables = weflow::ResolveSessionMessageTables(*ctx, sessionId);
        for (const auto& [meta, tbl] : tables) {
            auto it = meta->username_to_id.find(newSenderUsername);
            if (it == meta->username_to_id.end()) continue;
            WCDB::Handle dbHandle = meta->db->getHandle();
            std::ostringstream sql;
            sql << "UPDATE " << tbl << " SET real_sender_id = " << it->second
                << " WHERE local_id = " << localId;
            if (createTimeHint > 0) sql << " AND create_time = " << createTimeHint;
            if (dbHandle.asHandleOperation()->execute(weflow::MakeRawStatement(sql.str())) &&
                dbHandle.getChanges() > 0) {
                *outJson = weflow::AllocCString("{\"success\":true}");
                return 0;
            }
        }
        *outJson = weflow::AllocCString("{\"success\":false,\"error\":\"sender_or_message_not_found\"}");
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("update_message_sender", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_swap_two_messages_order(
    int64_t handle,
    const char* sessionId,
    int64_t localIdA,
    int64_t /*createTimeHintA*/,
    int64_t localIdB,
    int64_t /*createTimeHintB*/,
    char** outJson) {
    if (!sessionId || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto tables = weflow::ResolveSessionMessageTables(*ctx, sessionId);
        for (const auto& [meta, tbl] : tables) {
            WCDB::Handle dbHandle = meta->db->getHandle();
            int64_t seqA = 0, ctA = 0, seqB = 0, ctB = 0;
            bool gotA = false, gotB = false;
            {
                WCDB::Statement sA = weflow::MakeRawStatement(
                    "SELECT sort_seq, create_time FROM " + tbl + " WHERE local_id = " + std::to_string(localIdA));
                if (dbHandle.prepare(sA)) {
                    if (dbHandle.step()) {
                        seqA = dbHandle.getValue(0).intValue();
                        ctA = dbHandle.getValue(1).intValue();
                        gotA = true;
                    }
                    dbHandle.finalize();
                }
                WCDB::Statement sB = weflow::MakeRawStatement(
                    "SELECT sort_seq, create_time FROM " + tbl + " WHERE local_id = " + std::to_string(localIdB));
                if (dbHandle.prepare(sB)) {
                    if (dbHandle.step()) {
                        seqB = dbHandle.getValue(0).intValue();
                        ctB = dbHandle.getValue(1).intValue();
                        gotB = true;
                    }
                    dbHandle.finalize();
                }
            }
            if (gotA && gotB) {
                std::ostringstream uA, uB;
                uA << "UPDATE " << tbl << " SET sort_seq = " << seqB << ", create_time = " << ctB
                   << " WHERE local_id = " << localIdA;
                uB << "UPDATE " << tbl << " SET sort_seq = " << seqA << ", create_time = " << ctA
                   << " WHERE local_id = " << localIdB;
                dbHandle.asHandleOperation()->execute(weflow::MakeRawStatement(uA.str()));
                dbHandle.asHandleOperation()->execute(weflow::MakeRawStatement(uB.str()));
                *outJson = weflow::AllocCString("{\"success\":true}");
                return 0;
            }
        }
        *outJson = weflow::AllocCString("{\"success\":false,\"error\":\"messages_not_in_same_table\"}");
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("swap_two_messages_order", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_delete_message(
    int64_t handle,
    const char* sessionId,
    int64_t localId,
    int32_t createTime,
    const char* /*dbPathHint*/,
    char** outError) {
    if (!sessionId) return -1;
    if (outError) *outError = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto tables = weflow::ResolveSessionMessageTables(*ctx, sessionId);
        for (const auto& [meta, tbl] : tables) {
            WCDB::Handle dbHandle = meta->db->getHandle();
            std::ostringstream sql;
            sql << "DELETE FROM " << tbl << " WHERE local_id = " << localId;
            if (createTime > 0) sql << " AND create_time = " << createTime;
            if (dbHandle.asHandleOperation()->execute(weflow::MakeRawStatement(sql.str())) &&
                dbHandle.getChanges() > 0) {
                if (outError) *outError = weflow::AllocCString("Success");
                return 0;
            }
        }
        if (outError) *outError = weflow::AllocCString("message_not_found");
        return -3;
    } catch (const std::exception& e) {
        if (outError) *outError = weflow::AllocCString(e.what());
        weflow::SetLastError("delete_message", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_insert_session(
    int64_t handle,
    const char* username,
    int32_t type,
    const char* summary,
    int64_t lastTimestamp,
    int32_t unreadCount,
    int32_t isHidden,
    char** outJson) {
    if (!username || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx || !ctx->session_db) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        int64_t ts = (lastTimestamp > 0) ? lastTimestamp : static_cast<int64_t>(std::time(nullptr));
        std::ostringstream sql;
        sql << "INSERT OR REPLACE INTO SessionTable "
            << "(username, type, unread_count, is_hidden, summary, last_timestamp, sort_timestamp) VALUES ('"
            << weflow::EscapeSqlLiteral(username) << "', " << type << ", " << unreadCount << ", " << isHidden << ", '"
            << weflow::EscapeSqlLiteral(summary ? summary : "") << "', " << ts << ", " << ts << ")";
        WCDB::Handle dbHandle = ctx->session_db->getHandle();
        bool ok = dbHandle.asHandleOperation()->execute(weflow::MakeRawStatement(sql.str()));
        *outJson = weflow::AllocCString(ok ? "{\"success\":true}" : "{\"success\":false}");
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("insert_session", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_update_session(
    int64_t handle, const char* username, const char* patchJson, char** outJson) {
    if (!username || !patchJson || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx || !ctx->session_db) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        std::string j(patchJson);
        std::string summary = weflow::ParseJsonStringField(j, "summary", "\x01");
        int64_t lastTs = weflow::ParseJsonIntField(j, "last_timestamp", -1);
        int64_t sortTs = weflow::ParseJsonIntField(j, "sort_timestamp", -1);
        int64_t unread = weflow::ParseJsonIntField(j, "unread_count", -1);

        std::ostringstream sets;
        bool hasSet = false;
        auto addComma = [&]() { if (hasSet) sets << ", "; hasSet = true; };
        if (summary != "\x01") {
            addComma();
            sets << "summary = '" << weflow::EscapeSqlLiteral(summary) << "'";
        }
        if (lastTs >= 0) {
            addComma();
            sets << "last_timestamp = " << lastTs;
        }
        if (sortTs >= 0) {
            addComma();
            sets << "sort_timestamp = " << sortTs;
        }
        if (unread >= 0) {
            addComma();
            sets << "unread_count = " << unread;
        }
        if (!hasSet) {
            *outJson = weflow::AllocCString("{\"success\":true}");
            return 0;
        }
        std::string sql = "UPDATE SessionTable SET " + sets.str() +
                          " WHERE username = '" + weflow::EscapeSqlLiteral(username) + "'";
        WCDB::Handle dbHandle = ctx->session_db->getHandle();
        bool ok = dbHandle.asHandleOperation()->execute(weflow::MakeRawStatement(sql));
        *outJson = weflow::AllocCString(ok ? "{\"success\":true}" : "{\"success\":false}");
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("update_session", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_delete_session(
    int64_t handle, const char* username, int32_t deleteMessages, char** outJson) {
    if (!username || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx || !ctx->session_db) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        WCDB::Handle dbHandle = ctx->session_db->getHandle();
        std::string sql = "DELETE FROM SessionTable WHERE username = '" + weflow::EscapeSqlLiteral(username) + "'";
        dbHandle.asHandleOperation()->execute(weflow::MakeRawStatement(sql));
        if (deleteMessages != 0) {
            char* dummy = nullptr;
            wcdb_delete_message_table(handle, username, &dummy);
            wcdb_free_string(dummy);
        }
        *outJson = weflow::AllocCString("{\"success\":true}");
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("delete_session", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_reorder_sessions_by_time(int64_t handle, char** outJson) {
    if (!outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx || !ctx->session_db) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        WCDB::Handle dbHandle = ctx->session_db->getHandle();
        bool ok = dbHandle.asHandleOperation()->execute(
            weflow::MakeRawStatement("UPDATE SessionTable SET sort_timestamp = last_timestamp WHERE last_timestamp > 0"));
        *outJson = weflow::AllocCString(ok ? "{\"success\":true}" : "{\"success\":false}");
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("reorder_sessions_by_time", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_delete_message_table(int64_t handle, const char* sessionId, char** outJson) {
    if (!sessionId || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto tables = weflow::ResolveSessionMessageTables(*ctx, sessionId);
        int dropped = 0;
        for (const auto& [meta, tbl] : tables) {
            WCDB::Handle dbHandle = meta->db->getHandle();
            if (dbHandle.asHandleOperation()->execute(weflow::MakeRawStatement("DROP TABLE IF EXISTS " + tbl))) {
                ++dropped;
            }
        }
        weflow::EnsureMessageCatalog(*ctx, true);
        std::ostringstream oss;
        oss << "{\"success\":true,\"dropped\":" << dropped << "}";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("delete_message_table", -2, e.what());
        return -2;
    }
}

// ============================================================================
// Anti-Revoke SQLite Triggers (Reconstructed from 0x11c770..0x124000)
// ============================================================================

WCDB_API int32_t wcdb_setup_anti_revoke_triggers(int64_t handle, const char* sessionId, char** outStatusMsg) {
    if (!sessionId || !outStatusMsg) return -1;
    *outStatusMsg = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) {
        *outStatusMsg = weflow::AllocCString("Invalid handle");
        return -1;
    }

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto tables = weflow::ResolveSessionMessageTables(*ctx, sessionId);
        if (tables.empty()) {
            // Record in _weflow_anti_revoke_pending on message_0.db
            auto catalog = weflow::EnsureMessageCatalog(*ctx, false);
            if (catalog && !catalog->dbs.empty()) {
                WCDB::Handle h = catalog->dbs.front()->db->getHandle();
                h.asHandleOperation()->execute(weflow::MakeRawStatement(
                    "CREATE TABLE IF NOT EXISTS _weflow_anti_revoke_pending ("
                    "  session_id TEXT PRIMARY KEY, "
                    "  updated_at INTEGER)"));
                std::ostringstream ins;
                ins << "INSERT OR REPLACE INTO _weflow_anti_revoke_pending (session_id, updated_at) VALUES ('"
                    << weflow::EscapeSqlLiteral(sessionId) << "', CAST(strftime('%s','now') AS INTEGER))";
                h.asHandleOperation()->execute(weflow::MakeRawStatement(ins.str()));
            }
            *outStatusMsg = weflow::AllocCString("Success(queued_pending)");
            return 0;
        }

        std::string selfLabel = weflow::GetAntiRevokeSelfLabel();
        std::string otherPrefix = weflow::GetAntiRevokeOtherPrefix();
        std::string guardText = otherPrefix + "尝试撤回上一条消息 (已被 WeFlow 拦截)";

        for (const auto& [meta, tbl] : tables) {
            WCDB::Handle dbHandle = meta->db->getHandle();
            auto* op = dbHandle.asHandleOperation();

            // 1. Ensure _weflow_anti_revoke_deleted_cache table exists (exact schema from 0x11d9c8)
            op->execute(weflow::MakeRawStatement(
                "CREATE TABLE IF NOT EXISTS _weflow_anti_revoke_deleted_cache ("
                "  id INTEGER PRIMARY KEY AUTOINCREMENT, "
                "  tbl TEXT NOT NULL, "
                "  local_id INTEGER, "
                "  server_id INTEGER, "
                "  local_type INTEGER, "
                "  sort_seq INTEGER, "
                "  real_sender_id INTEGER, "
                "  create_time INTEGER, "
                "  status INTEGER, "
                "  upload_status INTEGER, "
                "  download_status INTEGER, "
                "  server_seq INTEGER, "
                "  origin_source INTEGER, "
                "  source TEXT, "
                "  message_content TEXT, "
                "  compress_content TEXT, "
                "  packed_info_data BLOB, "
                "  WCDB_CT_message_content INTEGER, "
                "  WCDB_CT_source INTEGER, "
                "  deleted_at INTEGER)"));

            std::string capTrig = "anti_revoke_capture_delete_" + tbl;
            std::string blkTrig = "anti_revoke_block_insert_" + tbl;

            op->execute(weflow::MakeRawStatement("DROP TRIGGER IF EXISTS " + capTrig));
            op->execute(weflow::MakeRawStatement("DROP TRIGGER IF EXISTS " + blkTrig));

            // 2. AFTER DELETE trigger capturing deleted non-system messages into _weflow_anti_revoke_deleted_cache
            std::ostringstream capSql;
            capSql << "CREATE TRIGGER IF NOT EXISTS " << capTrig
                   << " AFTER DELETE ON " << tbl
                   << " FOR EACH ROW WHEN OLD.local_type != 10000 "
                   << "BEGIN "
                   << "  INSERT INTO _weflow_anti_revoke_deleted_cache ("
                   << "    tbl, local_id, server_id, local_type, sort_seq, real_sender_id, create_time, "
                   << "    status, upload_status, download_status, server_seq, origin_source, source, "
                   << "    message_content, compress_content, packed_info_data, WCDB_CT_message_content, "
                   << "    WCDB_CT_source, deleted_at"
                   << "  ) VALUES ("
                   << "    '" << weflow::EscapeSqlLiteral(tbl) << "', "
                   << "    OLD.local_id, OLD.server_id, OLD.local_type, OLD.sort_seq, OLD.real_sender_id, OLD.create_time, "
                   << "    OLD.status, OLD.upload_status, OLD.download_status, OLD.server_seq, OLD.origin_source, OLD.source, "
                   << "    OLD.message_content, OLD.compress_content, OLD.packed_info_data, OLD.WCDB_CT_message_content, "
                   << "    OLD.WCDB_CT_source, CAST(strftime('%s','now') AS INTEGER)"
                   << "  ); "
                   << "END";
            op->execute(weflow::MakeRawStatement(capSql.str()));

            // 3. BEFORE INSERT trigger restoring deleted row and inserting custom anti-revoke notice
            std::string cacheWhere = "tbl = '" + weflow::EscapeSqlLiteral(tbl) +
                                     "' AND (CAST(strftime('%s','now') AS INTEGER) - COALESCE(deleted_at, 0)) <= 15";
            std::ostringstream blkSql;
            blkSql << "CREATE TRIGGER IF NOT EXISTS " << blkTrig
                   << " BEFORE INSERT ON " << tbl
                   << " FOR EACH ROW WHEN NEW.local_type = 10000 "
                   << " AND COALESCE(NEW.message_content, '') <> '" << weflow::EscapeSqlLiteral(guardText) << "' "
                   << " AND EXISTS (SELECT 1 FROM _weflow_anti_revoke_deleted_cache WHERE " << cacheWhere << ") "
                   << "BEGIN "
                   << "  INSERT INTO " << tbl << " ("
                   << "    server_id, local_type, sort_seq, real_sender_id, create_time, "
                   << "    status, upload_status, download_status, server_seq, origin_source, "
                   << "    source, message_content, compress_content, packed_info_data, "
                   << "    WCDB_CT_message_content, WCDB_CT_source"
                   << "  ) "
                   << "  SELECT "
                   << "    COALESCE(server_id, COALESCE(NEW.server_id, 0)), "
                   << "    COALESCE(local_type, 1), "
                   << "    COALESCE(sort_seq, COALESCE(NEW.sort_seq, 0)), "
                   << "    COALESCE(real_sender_id, COALESCE(NEW.real_sender_id, 0)), "
                   << "    COALESCE(create_time, COALESCE(NEW.create_time, 0)), "
                   << "    COALESCE(status, 3), "
                   << "    COALESCE(upload_status, 0), "
                   << "    COALESCE(download_status, 0), "
                   << "    COALESCE(server_seq, COALESCE(NEW.server_seq, 0)), "
                   << "    COALESCE(origin_source, 0), "
                   << "    COALESCE(source, ''), "
                   << "    COALESCE(message_content, ''), "
                   << "    compress_content, packed_info_data, WCDB_CT_message_content, WCDB_CT_source "
                   << "  FROM _weflow_anti_revoke_deleted_cache "
                   << "  WHERE id = (SELECT id FROM _weflow_anti_revoke_deleted_cache WHERE " << cacheWhere
                   << " ORDER BY id DESC LIMIT 1); "
                   << "  DELETE FROM _weflow_anti_revoke_deleted_cache "
                   << "  WHERE id = (SELECT id FROM _weflow_anti_revoke_deleted_cache WHERE " << cacheWhere
                   << " ORDER BY id DESC LIMIT 1); "
                   << "  INSERT INTO " << tbl << " ("
                   << "    server_id, local_type, sort_seq, real_sender_id, create_time, "
                   << "    status, upload_status, download_status, server_seq, origin_source, "
                   << "    source, message_content, compress_content, packed_info_data, "
                   << "    WCDB_CT_message_content, WCDB_CT_source"
                   << "  ) VALUES ("
                   << "    COALESCE(NEW.server_id, 0), 10000, COALESCE(NEW.sort_seq, 0) + 1, "
                   << "    COALESCE(NEW.real_sender_id, 0), COALESCE(NEW.create_time, 0) + 1, "
                   << "    4, 0, 0, COALESCE(NEW.server_seq, 0), 0, '', '"
                   << weflow::EscapeSqlLiteral(guardText) << "', NULL, NULL, NULL, NULL"
                   << "  ); "
                   << "  SELECT RAISE(IGNORE); "
                   << "END";
            op->execute(weflow::MakeRawStatement(blkSql.str()));
        }

        *outStatusMsg = weflow::AllocCString("Success");
        return 0;
    } catch (const std::exception& e) {
        *outStatusMsg = weflow::AllocCString(std::string("Exception: ") + e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_remove_anti_revoke_triggers(int64_t handle, const char* sessionId, char** outStatusMsg) {
    if (!sessionId || !outStatusMsg) return -1;
    *outStatusMsg = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) {
        *outStatusMsg = weflow::AllocCString("Invalid handle");
        return -1;
    }

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto tables = weflow::ResolveSessionMessageTables(*ctx, sessionId);
        for (const auto& [meta, tbl] : tables) {
            WCDB::Handle dbHandle = meta->db->getHandle();
            auto* op = dbHandle.asHandleOperation();
            for (const char* prefix : {
                     "anti_revoke_capture_delete_",
                     "anti_revoke_block_insert_",
                     "anti_revoke_block_delete_",
                     "anti_revoke_restore_",
                     "anti_revoke_notify_"}) {
                op->execute(weflow::MakeRawStatement(std::string("DROP TRIGGER IF EXISTS ") + prefix + tbl));
            }
            op->execute(weflow::MakeRawStatement(
                "DELETE FROM _weflow_anti_revoke_deleted_cache WHERE tbl = '" + weflow::EscapeSqlLiteral(tbl) + "'"));
        }
        *outStatusMsg = weflow::AllocCString(tables.empty() ? "Success(noop_no_table)" : "Success");
        return 0;
    } catch (const std::exception& e) {
        *outStatusMsg = weflow::AllocCString(std::string("Exception: ") + e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_cleanup_all_anti_revoke_triggers(int64_t handle, char** outJson) {
    if (!outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto catalog = weflow::EnsureMessageCatalog(*ctx, false);
        int dropped = 0;
        if (catalog) {
            for (const auto& meta : catalog->dbs) {
                WCDB::Handle dbHandle = meta->db->getHandle();
                std::vector<std::string> triggers;
                WCDB::Statement stmt = weflow::MakeRawStatement(
                    "SELECT name FROM sqlite_master WHERE type='trigger' AND name LIKE 'anti_revoke_%'");
                if (dbHandle.prepare(stmt)) {
                    while (dbHandle.step()) {
                        WCDB::Value v = dbHandle.getValue(0);
                        if (!v.isNull()) triggers.push_back(static_cast<std::string>(v.textValue()));
                    }
                    dbHandle.finalize();
                }
                for (const auto& trig : triggers) {
                    if (dbHandle.asHandleOperation()->execute(weflow::MakeRawStatement("DROP TRIGGER IF EXISTS " + trig))) {
                        ++dropped;
                    }
                }
            }
        }
        std::ostringstream oss;
        oss << "{\"success\":true,\"dropped\":" << dropped << "}";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("cleanup_all_anti_revoke_triggers", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_check_and_install_anti_revoke_for_session(
    int64_t handle, const char* sessionId, char** outStatusMsg) {
    return wcdb_setup_anti_revoke_triggers(handle, sessionId, outStatusMsg);
}

WCDB_API int32_t wcdb_set_anti_revoke_self_label(const char* label) {
    if (!label) return -1;
    std::lock_guard<std::mutex> lk(weflow::g_anti_revoke_mutex);
    weflow::g_anti_revoke_self_label = label;
    return 0;
}

WCDB_API int32_t wcdb_set_anti_revoke_other_prefix(const char* prefix) {
    if (!prefix) return -1;
    std::lock_guard<std::mutex> lk(weflow::g_anti_revoke_mutex);
    weflow::g_anti_revoke_other_prefix = prefix;
    return 0;
}

WCDB_API int32_t wcdb_get_anti_revoke_Labels(char** outJson) {
    if (!outJson) return -1;
    std::lock_guard<std::mutex> lk(weflow::g_anti_revoke_mutex);
    std::ostringstream oss;
    oss << "{\"self_label\":\"" << weflow::EscapeJson(weflow::g_anti_revoke_self_label) << "\","
        << "\"other_prefix\":\"" << weflow::EscapeJson(weflow::g_anti_revoke_other_prefix) << "\"}";
    *outJson = weflow::AllocCString(oss.str());
    return 0;
}

} // extern "C"
