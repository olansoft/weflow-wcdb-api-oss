#include "internal.hpp"

extern "C" {

WCDB_API int32_t wcdb_fetch_message_batch(
    int64_t handle, int64_t cursor, char** outJson, int32_t* outHasMore) {
    return wcdb_fetch_message_cursor_batch(handle, cursor, outJson, outHasMore);
}

WCDB_API int32_t wcdb_set_message_cursor_projection(
    int64_t /*handle*/, int64_t /*cursor*/, int32_t /*projection*/) {
    return 0;
}

WCDB_API int32_t wcdb_get_avatar_urls(int64_t handle, const char* usernamesJson, char** outJson) {
    return wcdb_get_avatars(handle, usernamesJson, outJson);
}



WCDB_API int32_t wcdb_get_message_tables(int64_t handle, const char* sessionId, char** outJson) {
    if (!sessionId || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        auto tables = weflow::ResolveSessionMessageTables(*ctx, sessionId);
        std::ostringstream oss;
        oss << "[";
        for (size_t i = 0; i < tables.size(); ++i) {
            if (i > 0) oss << ",";
            oss << "{\"db_path\":\"" << weflow::EscapeJson(tables[i].first->db_path) << "\","
                << "\"table_name\":\"" << weflow::EscapeJson(tables[i].second) << "\"}";
        }
        oss << "]";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_message_tables", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_message_table_stats(int64_t handle, const char* sessionId, char** outJson) {
    if (!sessionId || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        auto tables = weflow::ResolveSessionMessageTables(*ctx, sessionId);
        std::ostringstream oss;
        oss << "[";
        for (size_t i = 0; i < tables.size(); ++i) {
            if (i > 0) oss << ",";
            const auto& [meta, tbl] = tables[i];
            int64_t cnt = 0, minTs = 0, maxTs = 0;
            WCDB::Handle dbHandle = meta->db->getHandle();
            WCDB::Statement stmt = weflow::MakeRawStatement(
                "SELECT COUNT(*), MIN(create_time), MAX(create_time) FROM " + tbl);
            if (dbHandle.prepare(stmt)) {
                if (dbHandle.step()) {
                    cnt = dbHandle.getValue(0).intValue();
                    minTs = dbHandle.getValue(1).intValue();
                    maxTs = dbHandle.getValue(2).intValue();
                }
                dbHandle.finalize();
            }
            oss << "{\"db_path\":\"" << weflow::EscapeJson(meta->db_path) << "\","
                << "\"table_name\":\"" << weflow::EscapeJson(tbl) << "\","
                << "\"count\":" << cnt << ","
                << "\"first_timestamp\":" << minTs << ","
                << "\"last_timestamp\":" << maxTs << "}";
        }
        oss << "]";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_message_table_stats", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_message_dates(int64_t handle, const char* sessionId, char** outJson) {
    if (!sessionId || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto tables = weflow::ResolveSessionMessageTables(*ctx, sessionId);
        std::set<std::string> dates;
        for (const auto& [meta, tbl] : tables) {
            std::string sql =
                "SELECT DISTINCT strftime('%Y-%m-%d', create_time, 'unixepoch', 'localtime') AS d "
                "FROM " + tbl + " WHERE create_time > 0";
            WCDB::Handle dbHandle = meta->db->getHandle();
            WCDB::Statement stmt = weflow::MakeRawStatement(sql);
            if (dbHandle.prepare(stmt)) {
                while (dbHandle.step()) {
                    WCDB::Value dv = dbHandle.getValue(0);
                    if (!dv.isNull()) {
                        dates.insert(static_cast<std::string>(dv.textValue()));
                    }
                }
                dbHandle.finalize();
            }
        }
        std::ostringstream oss;
        oss << "[";
        bool first = true;
        for (const auto& d : dates) {
            if (!first) oss << ",";
            first = false;
            oss << "\"" << weflow::EscapeJson(d) << "\"";
        }
        oss << "]";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_message_dates", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_message_by_id(
    int64_t handle, const char* sessionId, int32_t localId, char** outJson) {
    if (!sessionId || !outJson) return -1;
    *outJson = nullptr;
    std::string spec = "{\"local_id\":" + std::to_string(localId) + "}";
    char* anchorJson = nullptr;
    if (wcdb_locate_message_anchor(handle, sessionId, spec.c_str(), &anchorJson) == 0 && anchorJson) {
        std::string s(anchorJson);
        wcdb_free_string(anchorJson);
        size_t pos = s.find("\"message\":");
        if (pos != std::string::npos && s.size() > pos + 11) {
            // Strip trailing '}' from locate_message_anchor object
            std::string msgObj = s.substr(pos + 10, s.size() - (pos + 10) - 1);
            *outJson = weflow::AllocCString(msgObj);
            return 0;
        }
    }
    *outJson = weflow::AllocCString("{}");
    return 0;
}

WCDB_API int32_t wcdb_get_message_by_svrid(
    int64_t handle, const char* sessionId, const char* svrid, char** outJson) {
    if (!sessionId || !svrid || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    int64_t sid = std::strtoll(svrid, nullptr, 10);
    auto tables = weflow::ResolveSessionMessageTables(*ctx, sessionId);
    for (const auto& [meta, tbl] : tables) {
        std::string sql = "SELECT local_id FROM " + tbl + " WHERE server_id = " + std::to_string(sid) + " LIMIT 1";
        WCDB::Handle dbHandle = meta->db->getHandle();
        WCDB::Statement stmt = weflow::MakeRawStatement(sql);
        if (dbHandle.prepare(stmt)) {
            if (dbHandle.step()) {
                int32_t lid = static_cast<int32_t>(dbHandle.getValue(0).intValue());
                dbHandle.finalize();
                return wcdb_get_message_by_id(handle, sessionId, lid, outJson);
            }
            dbHandle.finalize();
        }
    }
    *outJson = weflow::AllocCString("{}");
    return 0;
}

WCDB_API int32_t wcdb_list_message_dbs(int64_t handle, char** outJson) {
    if (!outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    auto catalog = weflow::EnsureMessageCatalog(*ctx, false);
    std::ostringstream oss;
    oss << "[";
    if (catalog) {
        for (size_t i = 0; i < catalog->dbs.size(); ++i) {
            if (i > 0) oss << ",";
            oss << "\"" << weflow::EscapeJson(catalog->dbs[i]->db_path) << "\"";
        }
    }
    oss << "]";
    *outJson = weflow::AllocCString(oss.str());
    return 0;
}

WCDB_API int32_t wcdb_list_media_dbs(int64_t handle, char** outJson) {
    if (!outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;
    *outJson = weflow::AllocCString("[]");
    return 0;
}

WCDB_API int32_t wcdb_get_db_status(int64_t handle, char** outJson) {
    if (!outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;
    auto catalog = weflow::EnsureMessageCatalog(*ctx, false);
    size_t totalDbs = catalog ? catalog->dbs.size() : 0;
    std::ostringstream oss;
    oss << "{\"is_indexing\":false,\"indexed_count\":0,\"total_dbs\":" << totalDbs << "}";
    *outJson = weflow::AllocCString(oss.str());
    return 0;
}

WCDB_API int32_t wcdb_get_logs(char** outJson) {
    if (!outJson) return -1;
    *outJson = weflow::AllocCString("[]");
    return 0;
}

WCDB_API int32_t wcdb_get_emoticon_cdn_url(
    int64_t handle, const char* /*dbPath*/, const char* md5, char** outUrl) {
    if (!md5 || !outUrl) return -1;
    *outUrl = nullptr;
    char* infoJson = nullptr;
    if (wcdb_get_emoticon_info(handle, md5, &infoJson) == 0 && infoJson) {
        std::string url = weflow::ParseJsonStringField(infoJson, "cdn_url");
        wcdb_free_string(infoJson);
        *outUrl = weflow::AllocCString(url);
        return 0;
    }
    *outUrl = weflow::AllocCString("");
    return 0;
}



WCDB_API int32_t wcdb_get_media_schema_summary(int64_t handle, const char* /*dbPath*/, char** outJson) {
    if (!outJson) return -1;
    *outJson = weflow::AllocCString("{}");
    (void)handle;
    return 0;
}

WCDB_API int32_t wcdb_get_session_message_counts(int64_t handle, const char* sessionIdsJson, char** outJson) {
    if (!sessionIdsJson || !outJson) return -1;
    *outJson = nullptr;
    auto sids = weflow::ParseJsonStringArray(sessionIdsJson);
    std::ostringstream oss;
    oss << "{";
    bool first = true;
    for (const auto& sid : sids) {
        int32_t cnt = 0;
        if (wcdb_get_message_count(handle, sid.c_str(), &cnt) == 0) {
            if (!first) oss << ",";
            first = false;
            oss << "\"" << weflow::EscapeJson(sid) << "\":" << cnt;
        }
    }
    oss << "}";
    *outJson = weflow::AllocCString(oss.str());
    return 0;
}



WCDB_API int32_t wcdb_get_session_message_date_counts(
    int64_t handle, const char* sessionId, char** outJson) {
    return wcdb_get_session_activity_heatmap(handle, sessionId, 0, 0, outJson);
}


WCDB_API int32_t wcdb_ai_query_session_candidates(
    int64_t handle, const char* /*optionsJson*/, char** outJson) {
    return wcdb_get_sessions(handle, outJson);
}



WCDB_API int32_t wcdb_resolve_image_hardlink_batch(
    int64_t handle, const char* requestsJson, char** outJson) {
    if (!requestsJson || !outJson) return -1;
    *outJson = nullptr;

    auto reqObjects = weflow::ParseJsonObjectArray(requestsJson);
    std::vector<std::pair<std::string, std::string>> items;
    if (!reqObjects.empty()) {
        for (const auto& obj : reqObjects) {
            std::string md5 = weflow::ParseJsonStringField(obj, "md5");
            std::string dir = weflow::ParseJsonStringField(obj, "account_dir");
            if (dir.empty()) dir = weflow::ParseJsonStringField(obj, "accountDir");
            items.emplace_back(std::move(md5), std::move(dir));
        }
    } else {
        auto strList = weflow::ParseJsonStringArray(requestsJson);
        for (auto& s : strList) {
            items.emplace_back(std::move(s), "");
        }
    }

    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) oss << ",";
        const auto& [md5, accDir] = items[i];
        oss << "{\"index\":" << i
            << ",\"md5\":\"" << weflow::EscapeJson(md5) << "\"";

        char* item = nullptr;
        int32_t rc = wcdb_resolve_image_hardlink(
            handle, md5.c_str(), accDir.empty() ? nullptr : accDir.c_str(), &item);
        if (rc == 0 && item && std::string(item) != "{}") {
            oss << ",\"success\":true,\"data\":" << item;
            wcdb_free_string(item);
        } else {
            if (item) wcdb_free_string(item);
            oss << ",\"success\":false,\"error\":\"not found\"";
        }
        oss << "}";
    }
    oss << "]";
    *outJson = weflow::AllocCString(oss.str());
    return 0;
}

WCDB_API int32_t wcdb_resolve_video_hardlink_md5_batch(
    int64_t handle, const char* requestsJson, char** outJson) {
    if (!requestsJson || !outJson) return -1;
    *outJson = nullptr;

    auto reqObjects = weflow::ParseJsonObjectArray(requestsJson);
    std::vector<std::pair<std::string, std::string>> items;
    if (!reqObjects.empty()) {
        for (const auto& obj : reqObjects) {
            std::string md5 = weflow::ParseJsonStringField(obj, "md5");
            std::string dbPath = weflow::ParseJsonStringField(obj, "db_path");
            if (dbPath.empty()) dbPath = weflow::ParseJsonStringField(obj, "dbPath");
            items.emplace_back(std::move(md5), std::move(dbPath));
        }
    } else {
        auto strList = weflow::ParseJsonStringArray(requestsJson);
        for (auto& s : strList) {
            items.emplace_back(std::move(s), "");
        }
    }

    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) oss << ",";
        const auto& [md5, dbPath] = items[i];
        oss << "{\"index\":" << i
            << ",\"md5\":\"" << weflow::EscapeJson(md5) << "\"";

        char* item = nullptr;
        int32_t rc = wcdb_resolve_video_hardlink_md5(
            handle, md5.c_str(), dbPath.empty() ? nullptr : dbPath.c_str(), &item);
        if (rc == 0 && item && std::string(item) != "{}") {
            oss << ",\"success\":true,\"data\":" << item;
            wcdb_free_string(item);
        } else {
            if (item) wcdb_free_string(item);
            oss << ",\"success\":false,\"error\":\"not found\"";
        }
        oss << "}";
    }
    oss << "]";
    *outJson = weflow::AllocCString(oss.str());
    return 0;
}

WCDB_API int32_t wcdb_install_message_anti_revoke_trigger(
    int64_t handle, const char* sessionId, char** outError) {
    return wcdb_setup_anti_revoke_triggers(handle, sessionId, outError);
}

WCDB_API int32_t wcdb_uninstall_message_anti_revoke_trigger(
    int64_t handle, const char* sessionId, char** outError) {
    return wcdb_remove_anti_revoke_triggers(handle, sessionId, outError);
}

WCDB_API int32_t wcdb_check_message_anti_revoke_trigger(
    int64_t handle, const char* sessionId, int32_t* outInstalled) {
    if (!sessionId || !outInstalled) return -1;
    *outInstalled = 0;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    auto tables = weflow::ResolveSessionMessageTables(*ctx, sessionId);
    for (const auto& [meta, tbl] : tables) {
        WCDB::Handle dbHandle = meta->db->getHandle();
        std::string trigName = "anti_revoke_capture_delete_" + tbl;
        std::string sql = "SELECT 1 FROM sqlite_master WHERE type='trigger' AND name='" +
                          weflow::EscapeSqlLiteral(trigName) + "' LIMIT 1";
        WCDB::Statement stmt = weflow::MakeRawStatement(sql);
        if (dbHandle.prepare(stmt)) {
            if (dbHandle.step()) {
                *outInstalled = 1;
                dbHandle.finalize();
                return 0;
            }
            dbHandle.finalize();
        }
    }
    return 0;
}

WCDB_API int32_t wcdb_check_sns_block_delete_trigger(int64_t handle, int32_t* outInstalled) {
    if (!outInstalled) return -1;
    *outInstalled = 0;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -2;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto snsDb = weflow::ResolveDbByKindOrPath(*ctx, "sns");
        if (!snsDb) return -3;
        WCDB::Handle dbHandle = snsDb->getHandle();
        WCDB::Statement stmt = weflow::MakeRawStatement(
            "SELECT COUNT(*) FROM sqlite_master WHERE type='trigger' AND name='block_delete_SnsTimeLine'");
        if (dbHandle.prepare(stmt)) {
            if (dbHandle.step()) {
                int64_t cnt = dbHandle.getValue(0).intValue();
                *outInstalled = (cnt > 0) ? 1 : 0;
            }
            dbHandle.finalize();
        }
        return 0;
    } catch (...) {
        return -2;
    }
}

WCDB_API int32_t wcdb_install_sns_block_delete_trigger(int64_t handle, char** outError) {
    if (outError) *outError = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) {
        if (outError) *outError = weflow::AllocCString("Invalid handle");
        return -2;
    }

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto snsDb = weflow::ResolveDbByKindOrPath(*ctx, "sns");
        if (!snsDb) {
            if (outError) *outError = weflow::AllocCString("sns.db not found");
            return -3;
        }

        int32_t installed = 0;
        wcdb_check_sns_block_delete_trigger(handle, &installed);
        if (installed == 1) {
            if (outError) *outError = weflow::AllocCString("already_installed");
            return 1;
        }

        WCDB::Handle dbHandle = snsDb->getHandle();
        bool ok = dbHandle.asHandleOperation()->execute(weflow::MakeRawStatement(
            "CREATE TRIGGER IF NOT EXISTS block_delete_SnsTimeLine BEFORE DELETE ON SnsTimeLine BEGIN SELECT RAISE(IGNORE); END"));
        if (!ok) {
            if (outError) *outError = weflow::AllocCString("Failed to create trigger");
            return -2;
        }
        if (outError) *outError = weflow::AllocCString("success");
        return 0;
    } catch (const std::exception& e) {
        if (outError) *outError = weflow::AllocCString(e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_uninstall_sns_block_delete_trigger(int64_t handle, char** outError) {
    if (outError) *outError = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) {
        if (outError) *outError = weflow::AllocCString("Invalid handle");
        return -2;
    }

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto snsDb = weflow::ResolveDbByKindOrPath(*ctx, "sns");
        if (!snsDb) {
            if (outError) *outError = weflow::AllocCString("sns.db not found");
            return -3;
        }

        int32_t installed = 0;
        wcdb_check_sns_block_delete_trigger(handle, &installed);
        if (installed == 0) {
            if (outError) *outError = weflow::AllocCString("not_installed");
            return 1;
        }

        WCDB::Handle dbHandle = snsDb->getHandle();
        bool ok = dbHandle.asHandleOperation()->execute(weflow::MakeRawStatement(
            "DROP TRIGGER IF EXISTS block_delete_SnsTimeLine"));
        if (!ok) {
            if (outError) *outError = weflow::AllocCString("Failed to drop trigger");
            return -2;
        }
        if (outError) *outError = weflow::AllocCString("success");
        return 0;
    } catch (const std::exception& e) {
        if (outError) *outError = weflow::AllocCString(e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_delete_sns_post(int64_t handle, const char* postId, char** outError) {
    if (!postId) return -1;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;
    auto snsDb = weflow::ResolveDbByKindOrPath(*ctx, "sns");
    if (snsDb) {
        WCDB::Handle dbHandle = snsDb->getHandle();
        int64_t tid = std::strtoll(postId, nullptr, 10);
        dbHandle.asHandleOperation()->execute(
            weflow::MakeRawStatement("DELETE FROM SnsTimeLine WHERE tid = " + std::to_string(tid)));
    }
    if (outError) *outError = weflow::AllocCString("Success");
    return 0;
}

WCDB_API int32_t wcdb_mark_all_sessions_read(int64_t handle, char** outError) {
    auto ctx = weflow::GetAccount(handle);
    if (!ctx || !ctx->session_db) return -1;
    WCDB::Handle dbHandle = ctx->session_db->getHandle();
    dbHandle.asHandleOperation()->execute(
        weflow::MakeRawStatement("UPDATE SessionTable SET unread_count = 0 WHERE unread_count > 0"));
    if (outError) *outError = weflow::AllocCString("Success");
    return 0;
}

WCDB_API int32_t wcdb_update_message(
    int64_t handle,
    const char* sessionId,
    int64_t localId,
    int32_t createTime,
    const char* newContent,
    char** outError) {
    return wcdb_update_message_content(handle, sessionId, localId, createTime, newContent, outError);
}

WCDB_API int32_t wcdb_start_monitor_pipe(void) {
    wcdb_set_monitor(0, true);
    return 0;
}

WCDB_API void wcdb_stop_monitor_pipe(void) {
    wcdb_set_monitor(0, false);
}

WCDB_API int32_t wcdb_cloud_init(int32_t /*intervalSeconds*/) {
    return 0;
}

WCDB_API int32_t wcdb_cloud_report(const char* statsJson) {
    char* resp = nullptr;
    int32_t rc = wcdb_submit_cloud_report(statsJson ? statsJson : "{}", &resp);
    wcdb_free_string(resp);
    return rc;
}

WCDB_API void wcdb_cloud_stop(void) {
}

} // extern "C"
