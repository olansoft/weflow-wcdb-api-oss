#include "internal.hpp"

namespace weflow {

namespace {

std::pair<int64_t, int64_t> GetYearUnixBounds(int32_t year) {
    struct tm tmStart{};
    tmStart.tm_year = year - 1900;
    tmStart.tm_mon = 0;
    tmStart.tm_mday = 1;
    tmStart.tm_isdst = -1;
    time_t startTs = ::mktime(&tmStart);

    struct tm tmEnd{};
    tmEnd.tm_year = (year + 1) - 1900;
    tmEnd.tm_mon = 0;
    tmEnd.tm_mday = 1;
    tmEnd.tm_isdst = -1;
    time_t endTs = ::mktime(&tmEnd) - 1;
    return {static_cast<int64_t>(startTs), static_cast<int64_t>(endTs)};
}

std::string BuildIsSendExpr(const std::shared_ptr<MessageDbMeta>& meta, const std::string& myWxid) {
    if (!myWxid.empty() && meta) {
        auto it = meta->username_to_id.find(myWxid);
        if (it != meta->username_to_id.end()) {
            return "(CASE WHEN real_sender_id = " + std::to_string(it->second) + " THEN 1 ELSE 0 END)";
        }
    }
    return "(CASE WHEN status = 2 THEN 1 ELSE 0 END)";
}

int32_t QuerySessionTimeStatus(
    int64_t handle, const char* sessionId, const std::string& strftimeFmt, char** outJson) {
    if (!sessionId || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto tables = ResolveSessionMessageTables(*ctx, sessionId);
        struct SentRecv {
            int64_t sent = 0;
            int64_t received = 0;
        };
        std::map<std::string, SentRecv> buckets;
        for (const auto& [meta, tbl] : tables) {
            std::string isSend = BuildIsSendExpr(meta, ctx->my_wxid);
            std::ostringstream sql;
            sql << "SELECT strftime('" << strftimeFmt << "', create_time, 'unixepoch', 'localtime') AS k, "
                << isSend << " AS is_sent, COUNT(*) FROM " << tbl
                << " WHERE create_time > 0 GROUP BY k, is_sent";
            WCDB::Handle dbHandle = meta->db->getHandle();
            WCDB::Statement stmt = MakeRawStatement(sql.str());
            if (dbHandle.prepare(stmt)) {
                while (dbHandle.step()) {
                    WCDB::Value kv = dbHandle.getValue(0);
                    if (kv.isNull()) continue;
                    std::string k = static_cast<std::string>(kv.textValue());
                    int64_t isSent = dbHandle.getValue(1).intValue();
                    int64_t cnt = dbHandle.getValue(2).intValue();
                    if (isSent) {
                        buckets[k].sent += cnt;
                    } else {
                        buckets[k].received += cnt;
                    }
                }
                dbHandle.finalize();
            }
        }
        std::ostringstream oss;
        oss << "{";
        bool first = true;
        for (const auto& [k, sr] : buckets) {
            if (!first) oss << ",";
            first = false;
            oss << "\"" << EscapeJson(k) << "\":{\"sent\":" << sr.sent
                << ",\"received\":" << sr.received
                << ",\"total\":" << (sr.sent + sr.received) << "}";
        }
        oss << "}";
        *outJson = AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        SetLastError("query_session_time_status", -2, e.what());
        return -2;
    }
}

} // namespace

} // namespace weflow

extern "C" {

WCDB_API int32_t wcdb_get_session_message_type_stats(
    int64_t handle, const char* sessionId, int32_t beginTimestamp, int32_t endTimestamp, char** outJson) {
    if (!sessionId || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto tables = weflow::ResolveSessionMessageTables(*ctx, sessionId);
        int64_t total = 0;
        int64_t voiceCnt = 0;
        int64_t imageCnt = 0;
        int64_t videoCnt = 0;
        int64_t emojiCnt = 0;
        int64_t fileCnt = 0;
        int64_t callCnt = 0;
        int64_t transferCnt = 0;
        int64_t redPacketCnt = 0;
        int64_t minTs = 0, maxTs = 0;
        int64_t groupMyMsg = 0;
        std::set<int64_t> senders;

        std::string sidStr(sessionId);
        bool isGroup = (sidStr.size() > 9 && sidStr.substr(sidStr.size() - 9) == "@chatroom");

        for (const auto& [meta, tbl] : tables) {
            std::string isSend = weflow::BuildIsSendExpr(meta, ctx->my_wxid);
            std::ostringstream sql;
            sql << "SELECT local_type, " << isSend << " AS is_sent, real_sender_id, COUNT(*), MIN(create_time), MAX(create_time) "
                << "FROM " << tbl << " WHERE 1=1";
            if (beginTimestamp > 0) sql << " AND create_time >= " << beginTimestamp;
            if (endTimestamp > 0) sql << " AND create_time <= " << endTimestamp;
            sql << " GROUP BY local_type, is_sent, real_sender_id";

            WCDB::Handle dbHandle = meta->db->getHandle();
            WCDB::Statement stmt = weflow::MakeRawStatement(sql.str());
            if (dbHandle.prepare(stmt)) {
                while (dbHandle.step()) {
                    int64_t lt = dbHandle.getValue(0).intValue();
                    int64_t isSent = dbHandle.getValue(1).intValue();
                    int64_t senderId = dbHandle.getValue(2).intValue();
                    int64_t cnt = dbHandle.getValue(3).intValue();
                    int64_t mn = dbHandle.getValue(4).intValue();
                    int64_t mx = dbHandle.getValue(5).intValue();

                    total += cnt;
                    if (isSent) groupMyMsg += cnt;
                    senders.insert(senderId);

                    if (minTs == 0 || (mn > 0 && mn < minTs)) minTs = mn;
                    if (mx > maxTs) maxTs = mx;

                    if (lt == 34) voiceCnt += cnt;
                    else if (lt == 3) imageCnt += cnt;
                    else if (lt == 43 || lt == 62) videoCnt += cnt;
                    else if (lt == 47) emojiCnt += cnt;
                    else if (lt == 49 || lt == 10000 || (lt & 0xFF) == 49) fileCnt += cnt;
                    else if (lt == 50) callCnt += cnt;
                    else if (lt == 2000) transferCnt += cnt;
                    else if (lt == 2001) redPacketCnt += cnt;
                }
                dbHandle.finalize();
            }
        }

        std::ostringstream oss;
        oss << "{\"total_messages\":" << total
            << ",\"voice_messages\":" << voiceCnt
            << ",\"image_messages\":" << imageCnt
            << ",\"video_messages\":" << videoCnt
            << ",\"emoji_messages\":" << emojiCnt
            << ",\"file_messages\":" << fileCnt
            << ",\"call_messages\":" << callCnt
            << ",\"transfer_messages\":" << transferCnt
            << ",\"red_packet_messages\":" << redPacketCnt
            << ",\"first_timestamp\":" << minTs
            << ",\"last_timestamp\":" << maxTs;
        if (isGroup) {
            oss << ",\"group_my_messages\":" << groupMyMsg
                << ",\"group_sender_count\":" << senders.size();
        }
        oss << "}";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_session_message_type_stats", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_aggregate_stats(
    int64_t handle, const char* sessionIdsJson, int32_t begin, int32_t end, char** outJson) {
    if (!sessionIdsJson || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto sessionIds = weflow::ParseJsonStringArray(sessionIdsJson);
        if (sessionIds.empty() && sessionIdsJson[0] != '[') {
            sessionIds.emplace_back(sessionIdsJson);
        }

        if (sessionIds.empty() && ctx->session_db) {
            WCDB::Handle sh = ctx->session_db->getHandle();
            WCDB::Statement ss = weflow::MakeRawStatement("SELECT username FROM SessionTable");
            if (sh.prepare(ss)) {
                while (sh.step()) {
                    WCDB::Value uv = sh.getValue(0);
                    if (!uv.isNull()) sessionIds.push_back(static_cast<std::string>(uv.textValue()));
                }
                sh.finalize();
            }
        }

        int64_t total = 0, sent = 0, received = 0;
        int64_t minTs = 0, maxTs = 0;
        std::map<int64_t, int64_t> typeCounts;
        std::map<std::string, int64_t> hourly;
        std::map<std::string, int64_t> weekday;
        std::map<std::string, int64_t> daily;
        std::map<std::string, int64_t> monthly;
        std::map<int64_t, std::string> idMap;

        struct SessionAgg {
            int64_t total = 0;
            int64_t sent = 0;
            int64_t received = 0;
            int64_t lastTime = 0;
            std::map<std::string, int64_t> monthly;
            std::map<std::string, int64_t> senders;
        };
        std::map<std::string, SessionAgg> sessionAggs;

        for (const auto& sessionId : sessionIds) {
            auto tables = weflow::ResolveSessionMessageTables(*ctx, sessionId);
            auto& sAgg = sessionAggs[sessionId];

            for (const auto& [meta, tbl] : tables) {
                if (meta) {
                    for (const auto& [id, name] : meta->id_to_username) {
                        idMap[id] = name;
                    }
                }
                std::string isSend = weflow::BuildIsSendExpr(meta, ctx->my_wxid);
                std::ostringstream sql;
                sql << "SELECT local_type, " << isSend << " AS is_sent, "
                    << "strftime('%H', create_time, 'unixepoch', 'localtime') AS hr, "
                    << "strftime('%w', create_time, 'unixepoch', 'localtime') AS wd, "
                    << "strftime('%Y-%m-%d', create_time, 'unixepoch', 'localtime') AS dy, "
                    << "strftime('%Y-%m', create_time, 'unixepoch', 'localtime') AS mo, "
                    << "real_sender_id, COUNT(*), MIN(create_time), MAX(create_time) "
                    << "FROM " << tbl << " WHERE 1=1";
                if (begin > 0) sql << " AND create_time >= " << begin;
                if (end > 0) sql << " AND create_time <= " << end;
                sql << " GROUP BY local_type, is_sent, hr, wd, dy, mo, real_sender_id";

                WCDB::Handle dbHandle = meta->db->getHandle();
                WCDB::Statement stmt = weflow::MakeRawStatement(sql.str());
                if (dbHandle.prepare(stmt)) {
                    while (dbHandle.step()) {
                        int64_t lt = dbHandle.getValue(0).intValue();
                        int64_t isSent = dbHandle.getValue(1).intValue();
                        std::string hr = dbHandle.getValue(2).isNull() ? "" : static_cast<std::string>(dbHandle.getValue(2).textValue());
                        std::string wd = dbHandle.getValue(3).isNull() ? "" : static_cast<std::string>(dbHandle.getValue(3).textValue());
                        std::string dy = dbHandle.getValue(4).isNull() ? "" : static_cast<std::string>(dbHandle.getValue(4).textValue());
                        std::string mo = dbHandle.getValue(5).isNull() ? "" : static_cast<std::string>(dbHandle.getValue(5).textValue());
                        int64_t senderId = dbHandle.getValue(6).intValue();
                        int64_t cnt = dbHandle.getValue(7).intValue();
                        int64_t mn = dbHandle.getValue(8).intValue();
                        int64_t mx = dbHandle.getValue(9).intValue();

                        total += cnt;
                        if (isSent) sent += cnt; else received += cnt;
                        typeCounts[lt] += cnt;

                        if (!hr.empty()) hourly[hr] += cnt;
                        if (!wd.empty()) weekday[wd] += cnt;
                        if (!dy.empty()) daily[dy] += cnt;
                        if (!mo.empty()) {
                            monthly[mo] += cnt;
                            sAgg.monthly[mo] += cnt;
                        }

                        sAgg.total += cnt;
                        if (isSent) sAgg.sent += cnt; else sAgg.received += cnt;
                        if (mx > sAgg.lastTime) sAgg.lastTime = mx;

                        std::string senderStr = std::to_string(senderId);
                        if (meta) {
                            auto it = meta->id_to_username.find(senderId);
                            if (it != meta->id_to_username.end()) senderStr = it->second;
                        }
                        sAgg.senders[senderStr] += cnt;

                        if (minTs == 0 || (mn > 0 && mn < minTs)) minTs = mn;
                        if (mx > maxTs) maxTs = mx;
                    }
                    dbHandle.finalize();
                }
            }
        }

        std::ostringstream oss;
        oss << "{\"total\":" << total
            << ",\"sent\":" << sent
            << ",\"received\":" << received
            << ",\"firstTime\":" << minTs
            << ",\"lastTime\":" << maxTs
            << ",\"first_timestamp\":" << minTs
            << ",\"last_timestamp\":" << maxTs
            << ",\"typeCounts\":{";
        bool first = true;
        for (const auto& [lt, cnt] : typeCounts) {
            if (!first) oss << ",";
            first = false;
            oss << "\"" << lt << "\":" << cnt;
        }
        oss << "},\"hourly\":{";
        first = true;
        for (const auto& [h, cnt] : hourly) {
            if (!first) oss << ",";
            first = false;
            oss << "\"" << weflow::EscapeJson(h) << "\":" << cnt;
        }
        oss << "},\"weekday\":{";
        first = true;
        for (const auto& [w, cnt] : weekday) {
            if (!first) oss << ",";
            first = false;
            oss << "\"" << weflow::EscapeJson(w) << "\":" << cnt;
        }
        oss << "},\"daily\":{";
        first = true;
        for (const auto& [d, cnt] : daily) {
            if (!first) oss << ",";
            first = false;
            oss << "\"" << weflow::EscapeJson(d) << "\":" << cnt;
        }
        oss << "},\"monthly\":{";
        first = true;
        for (const auto& [m, cnt] : monthly) {
            if (!first) oss << ",";
            first = false;
            oss << "\"" << weflow::EscapeJson(m) << "\":" << cnt;
        }
        oss << "},\"sessions\":{";
        first = true;
        for (const auto& [sid, sAgg] : sessionAggs) {
            if (!first) oss << ",";
            first = false;
            oss << "\"" << weflow::EscapeJson(sid) << "\":{\"total\":" << sAgg.total
                << ",\"sent\":" << sAgg.sent
                << ",\"received\":" << sAgg.received
                << ",\"lastTime\":" << sAgg.lastTime
                << ",\"monthly\":{";
            bool firstM = true;
            for (const auto& [m, cnt] : sAgg.monthly) {
                if (!firstM) oss << ",";
                firstM = false;
                oss << "\"" << weflow::EscapeJson(m) << "\":" << cnt;
            }
            oss << "},\"senders\":{";
            bool firstS = true;
            for (const auto& [s, cnt] : sAgg.senders) {
                if (!firstS) oss << ",";
                firstS = false;
                oss << "\"" << weflow::EscapeJson(s) << "\":" << cnt;
            }
            oss << "}}";
        }
        oss << "},\"idMap\":{";
        first = true;
        for (const auto& [id, name] : idMap) {
            if (!first) oss << ",";
            first = false;
            oss << "\"" << id << "\":\"" << weflow::EscapeJson(name) << "\"";
        }
        oss << "}}";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_aggregate_stats", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_global_message_type_counts(int64_t handle, char** outJson) {
    if (!outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto catalog = weflow::EnsureMessageCatalog(*ctx, false);
        std::map<int64_t, int64_t> typeCounts;
        if (catalog) {
            for (const auto& meta : catalog->dbs) {
                WCDB::Handle dbHandle = meta->db->getHandle();
                for (const auto& [lowerTbl, actualTbl] : meta->canonical_table_names) {
                    if (lowerTbl.rfind("msg_", 0) != 0) continue;
                    std::string sql = "SELECT local_type, COUNT(*) FROM " + actualTbl + " GROUP BY local_type";
                    WCDB::Statement stmt = weflow::MakeRawStatement(sql);
                    if (dbHandle.prepare(stmt)) {
                        while (dbHandle.step()) {
                            typeCounts[dbHandle.getValue(0).intValue()] += dbHandle.getValue(1).intValue();
                        }
                        dbHandle.finalize();
                    }
                }
            }
        }
        std::ostringstream oss;
        oss << "{";
        bool first = true;
        for (const auto& [lt, cnt] : typeCounts) {
            if (!first) oss << ",";
            first = false;
            oss << "\"" << lt << "\":" << cnt;
        }
        oss << "}";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_global_message_type_counts", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_available_years(int64_t handle, const char* /*sessionIdsJson*/, char** outJson) {
    if (!outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto catalog = weflow::EnsureMessageCatalog(*ctx, false);
        int64_t globalMin = 0, globalMax = 0;
        if (catalog) {
            for (const auto& meta : catalog->dbs) {
                WCDB::Handle dbHandle = meta->db->getHandle();
                for (const auto& [lowerTbl, actualTbl] : meta->canonical_table_names) {
                    if (lowerTbl.rfind("msg_", 0) != 0) continue;
                    std::string sql = "SELECT MIN(create_time) AS min_t, MAX(create_time) AS max_t FROM " + actualTbl +
                                      " WHERE create_time > 946684800";
                    WCDB::Statement stmt = weflow::MakeRawStatement(sql);
                    if (dbHandle.prepare(stmt)) {
                        if (dbHandle.step()) {
                            int64_t mn = dbHandle.getValue(0).intValue();
                            int64_t mx = dbHandle.getValue(1).intValue();
                            if (mn > 0 && (globalMin == 0 || mn < globalMin)) globalMin = mn;
                            if (mx > globalMax) globalMax = mx;
                        }
                        dbHandle.finalize();
                    }
                }
            }
        }
        std::set<int, std::greater<int>> years;
        if (globalMin > 0 && globalMax >= globalMin) {
            time_t tMin = static_cast<time_t>(globalMin);
            time_t tMax = static_cast<time_t>(globalMax);
            struct tm tmMin{}, tmMax{};
            ::localtime_r(&tMin, &tmMin);
            ::localtime_r(&tMax, &tmMax);
            for (int y = tmMin.tm_year + 1900; y <= tmMax.tm_year + 1900; ++y) {
                years.insert(y);
            }
        }
        std::ostringstream oss;
        oss << "[";
        bool first = true;
        for (int y : years) {
            if (!first) oss << ",";
            first = false;
            oss << y;
        }
        oss << "]";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_available_years", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_annual_report_stats(
    int64_t handle, const char* sessionIdsJson, int32_t begin, int32_t end, char** outJson) {
    return wcdb_get_aggregate_stats(handle, sessionIdsJson, begin, end, outJson);
}

WCDB_API int32_t wcdb_get_annual_report_extras(
    int64_t handle,
    const char* sessionIdsJson,
    int32_t begin,
    int32_t end,
    int32_t peakBegin,
    int32_t peakEnd,
    char** outJson) {
    if (!outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        int64_t startTs = begin;
        int64_t endTs = end;
        auto sessionIds = weflow::ParseJsonStringArray(sessionIdsJson);

        int64_t heatmap[7][24]{};
        std::map<std::string, int64_t> midnight;
        std::map<std::string, int64_t> dailyTotal;

        for (const auto& sid : sessionIds) {
            auto tables = weflow::ResolveSessionMessageTables(*ctx, sid);
            for (const auto& [meta, tbl] : tables) {
                WCDB::Handle dbHandle = meta->db->getHandle();
                std::ostringstream sql;
                sql << "SELECT strftime('%w', create_time, 'unixepoch', 'localtime') AS wd, "
                    << "strftime('%H', create_time, 'unixepoch', 'localtime') AS hr, "
                    << "strftime('%Y-%m-%d', create_time, 'unixepoch', 'localtime') AS dy, "
                    << "COUNT(*) AS cnt "
                    << "FROM " << tbl << " WHERE 1=1";
                if (startTs > 0) sql << " AND create_time >= " << startTs;
                if (endTs > 0) sql << " AND create_time <= " << endTs;
                sql << " GROUP BY wd, hr, dy";

                WCDB::Statement stmt = weflow::MakeRawStatement(sql.str());
                if (dbHandle.prepare(stmt)) {
                    while (dbHandle.step()) {
                        int wd = std::atoi(static_cast<std::string>(dbHandle.getValue(0).textValue()).c_str());
                        int hr = std::atoi(static_cast<std::string>(dbHandle.getValue(1).textValue()).c_str());
                        std::string dy = dbHandle.getValue(2).isNull() ? "" : static_cast<std::string>(dbHandle.getValue(2).textValue());
                        int64_t cnt = dbHandle.getValue(3).intValue();

                        if (wd >= 0 && wd < 7 && hr >= 0 && hr < 24) {
                            heatmap[wd][hr] += cnt;
                        }
                        if (hr >= 0 && hr < 5) {
                            midnight[sid] += cnt;
                        }
                        if (!dy.empty()) {
                            dailyTotal[dy] += cnt;
                        }
                    }
                    dbHandle.finalize();
                }
            }
        }

        std::string peakDayKey;
        int64_t peakDayCount = 0;
        for (const auto& [dy, cnt] : dailyTotal) {
            if (cnt > peakDayCount) {
                peakDayCount = cnt;
                peakDayKey = dy;
            }
        }

        std::ostringstream oss;
        oss << "{\"heatmap\":[";
        for (int wd = 0; wd < 7; ++wd) {
            if (wd > 0) oss << ",";
            oss << "[";
            for (int hr = 0; hr < 24; ++hr) {
                if (hr > 0) oss << ",";
                oss << heatmap[wd][hr];
            }
            oss << "]";
        }
        oss << "],\"midnight\":{";
        bool firstM = true;
        for (const auto& [sid, cnt] : midnight) {
            if (!firstM) oss << ",";
            firstM = false;
            oss << "\"" << weflow::EscapeJson(sid) << "\":" << cnt;
        }
        oss << "},\"conversation\":{},\"response\":{\"avg\":0,\"fastest\":0,\"count\":0},\"peakDay\":{";
        if (!peakDayKey.empty()) {
            oss << "\"key\":\"" << weflow::EscapeJson(peakDayKey) << "\",\"count\":" << peakDayCount;
        }
        oss << "},\"streak\":{\"sessionId\":\"\",\"days\":0,\"startDate\":\"\",\"endDate\":\"\"},\"topPhrases\":[]}";

        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_annual_report_extras", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_dual_report_stats(
    int64_t handle, const char* sessionId, int32_t begin, int32_t end, char** outJson) {
    if (!sessionId || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto tables = weflow::ResolveSessionMessageTables(*ctx, sessionId);
        int64_t heatmap[7][24]{};
        std::map<std::string, int64_t> monthly;
        int64_t total = 0, wordsCnt = 0, imageCnt = 0, voiceCnt = 0, emojiCnt = 0;

        for (const auto& [meta, tbl] : tables) {
            WCDB::Handle dbHandle = meta->db->getHandle();
            std::ostringstream sql;
            sql << "SELECT strftime('%w', create_time, 'unixepoch', 'localtime') AS wd, "
                << "strftime('%H', create_time, 'unixepoch', 'localtime') AS hr, "
                << "strftime('%Y-%m', create_time, 'unixepoch', 'localtime') AS mo, "
                << "local_type, COUNT(*) AS cnt FROM " << tbl << " WHERE 1=1";
            if (begin > 0) sql << " AND create_time >= " << begin;
            if (end > 0) sql << " AND create_time <= " << end;
            sql << " GROUP BY wd, hr, mo, local_type";
            WCDB::Statement stmt = weflow::MakeRawStatement(sql.str());
            if (dbHandle.prepare(stmt)) {
                while (dbHandle.step()) {
                    int wd = std::atoi(static_cast<std::string>(dbHandle.getValue(0).textValue()).c_str());
                    int hr = std::atoi(static_cast<std::string>(dbHandle.getValue(1).textValue()).c_str());
                    std::string mo = dbHandle.getValue(2).isNull() ? "" : static_cast<std::string>(dbHandle.getValue(2).textValue());
                    int64_t lt = dbHandle.getValue(3).intValue();
                    int64_t cnt = dbHandle.getValue(4).intValue();
                    if (wd >= 0 && wd < 7 && hr >= 0 && hr < 24) heatmap[wd][hr] += cnt;
                    if (!mo.empty()) monthly[mo] += cnt;
                    total += cnt;
                    if (lt == 1) wordsCnt += (cnt * 10);
                    else if (lt == 3) imageCnt += cnt;
                    else if (lt == 34) voiceCnt += cnt;
                    else if (lt == 47) emojiCnt += cnt;
                }
                dbHandle.finalize();
            }
        }
        std::ostringstream oss;
        oss << "{\"heatmap\":[";
        for (int wd = 0; wd < 7; ++wd) {
            if (wd > 0) oss << ",";
            oss << "[";
            for (int hr = 0; hr < 24; ++hr) {
                if (hr > 0) oss << ",";
                oss << heatmap[wd][hr];
            }
            oss << "]";
        }
        oss << "],\"initiative\":{\"initiated\":0,\"received\":0},"
            << "\"response\":{\"avg\":0,\"fastest\":0,\"slowest\":0,\"count\":0},"
            << "\"monthly\":{";
        bool first = true;
        for (const auto& [m, c] : monthly) {
            if (!first) oss << ",";
            first = false;
            oss << "\"" << weflow::EscapeJson(m) << "\":" << c;
        }
        oss << "},\"streak\":{\"days\":0,\"startDate\":\"\",\"endDate\":\"\"},"
            << "\"counts\":{\"total\":" << total << ",\"words\":" << wordsCnt << ",\"image\":" << imageCnt
            << ",\"voice\":" << voiceCnt << ",\"emoji\":" << emojiCnt << "},"
            << "\"phrases\":[],\"myPhrases\":[],\"friendPhrases\":[],\"emojis\":[]}";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_dual_report_stats", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_group_stats(
    int64_t handle, const char* chatroomId, int32_t begin, int32_t end, char** outJson) {
    if (!chatroomId || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto tables = weflow::ResolveSessionMessageTables(*ctx, chatroomId);
        int64_t total = 0, sent = 0, received = 0;
        std::map<int64_t, int64_t> typeCounts;
        std::map<std::string, int64_t> hourly;
        std::map<std::string, int64_t> sendersByUsername;
        std::map<std::string, int64_t> sendersById;
        std::map<int64_t, std::string> idMap;

        for (const auto& [meta, tbl] : tables) {
            if (meta) {
                for (const auto& [id, name] : meta->id_to_username) {
                    idMap[id] = name;
                }
            }
            std::string isSend = weflow::BuildIsSendExpr(meta, ctx->my_wxid);
            WCDB::Handle dbHandle = meta->db->getHandle();
            {
                std::ostringstream sql;
                sql << "SELECT local_type AS type, " << isSend << " AS is_sent, COUNT(*) AS cnt FROM " << tbl << " WHERE 1=1";
                if (begin > 0) sql << " AND create_time >= " << begin;
                if (end > 0) sql << " AND create_time <= " << end;
                sql << " GROUP BY type, is_sent";
                WCDB::Statement stmt = weflow::MakeRawStatement(sql.str());
                if (dbHandle.prepare(stmt)) {
                    while (dbHandle.step()) {
                        int64_t lt = dbHandle.getValue(0).intValue();
                        int64_t isSent = dbHandle.getValue(1).intValue();
                        int64_t cnt = dbHandle.getValue(2).intValue();
                        total += cnt;
                        if (isSent) sent += cnt; else received += cnt;
                        typeCounts[lt] += cnt;
                    }
                    dbHandle.finalize();
                }
            }
            {
                std::ostringstream sql;
                sql << "SELECT strftime('%H', create_time, 'unixepoch', 'localtime') AS hr, COUNT(*) AS cnt FROM " << tbl << " WHERE 1=1";
                if (begin > 0) sql << " AND create_time >= " << begin;
                if (end > 0) sql << " AND create_time <= " << end;
                sql << " GROUP BY hr";
                WCDB::Statement stmt = weflow::MakeRawStatement(sql.str());
                if (dbHandle.prepare(stmt)) {
                    while (dbHandle.step()) {
                        WCDB::Value hv = dbHandle.getValue(0);
                        if (!hv.isNull()) {
                            std::string rawH = static_cast<std::string>(hv.textValue());
                            int hVal = std::atoi(rawH.c_str());
                            int64_t count = dbHandle.getValue(1).intValue();
                            hourly[rawH] += count;
                            hourly[std::to_string(hVal)] += count;
                        }
                    }
                    dbHandle.finalize();
                }
            }
            {
                std::ostringstream sql;
                sql << "SELECT real_sender_id AS sender_id, COUNT(*) AS cnt FROM " << tbl << " WHERE 1=1";
                if (begin > 0) sql << " AND create_time >= " << begin;
                if (end > 0) sql << " AND create_time <= " << end;
                sql << " GROUP BY sender_id";
                WCDB::Statement stmt = weflow::MakeRawStatement(sql.str());
                if (dbHandle.prepare(stmt)) {
                    while (dbHandle.step()) {
                        int64_t sid = dbHandle.getValue(0).intValue();
                        int64_t cnt = dbHandle.getValue(1).intValue();
                        std::string uname = std::to_string(sid);
                        if (meta) {
                            auto it = meta->id_to_username.find(sid);
                            if (it != meta->id_to_username.end()) uname = it->second;
                        }
                        sendersByUsername[uname] += cnt;
                        sendersById[std::to_string(sid)] += cnt;
                    }
                    dbHandle.finalize();
                }
            }
        }
        std::ostringstream oss;
        oss << "{\"total\":" << total << ",\"sent\":" << sent << ",\"received\":" << received << ",\"typeCounts\":{";
        bool first = true;
        for (const auto& [lt, c] : typeCounts) {
            if (!first) oss << ",";
            first = false;
            oss << "\"" << lt << "\":" << c;
        }
        oss << "},\"hourly\":{";
        first = true;
        for (int h = 0; h < 24; ++h) {
            std::string hStr = std::to_string(h);
            int64_t val = hourly[hStr];
            if (!first) oss << ",";
            first = false;
            oss << "\"" << h << "\":" << val;
        }
        for (const auto& [h, c] : hourly) {
            if (h.size() > 1 && h[0] == '0') {
                oss << ",\"" << weflow::EscapeJson(h) << "\":" << c;
            }
        }
        oss << "},\"senders\":{";
        first = true;
        for (const auto& [u, c] : sendersByUsername) {
            if (!first) oss << ",";
            first = false;
            oss << "\"" << weflow::EscapeJson(u) << "\":" << c;
        }
        oss << "},\"sessions\":{\"" << weflow::EscapeJson(chatroomId) << "\":{\"total\":" << total
            << ",\"sent\":" << sent << ",\"received\":" << received << ",\"senders\":{";
        first = true;
        for (const auto& [sid, c] : sendersById) {
            if (!first) oss << ",";
            first = false;
            oss << "\"" << weflow::EscapeJson(sid) << "\":" << c;
        }
        oss << "}}},\"idMap\":{";
        first = true;
        for (const auto& [id, name] : idMap) {
            if (!first) oss << ",";
            first = false;
            oss << "\"" << id << "\":\"" << weflow::EscapeJson(name) << "\"";
        }
        oss << "}}";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_group_stats", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_my_footprint_stats(
    int64_t handle, const char* optionsJson, char** outJson) {
    if (!outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        int64_t beginTs = 0, endTs = 0;
        std::vector<std::string> privSessionIds;
        std::vector<std::string> grpSessionIds;
        int64_t mentionLimit = 100;
        int64_t privateLimit = 100;
        std::string myWxid = ctx->my_wxid;

        if (optionsJson && optionsJson[0] != '\0') {
            std::string opts(optionsJson);
            beginTs = weflow::ParseJsonIntField(opts, "begin", 0);
            if (beginTs == 0) beginTs = weflow::ParseJsonIntField(opts, "beginTimestamp", 0);
            endTs = weflow::ParseJsonIntField(opts, "end", 0);
            if (endTs == 0) endTs = weflow::ParseJsonIntField(opts, "endTimestamp", 0);
            privSessionIds = weflow::ParseJsonStringArrayField(opts, "private_session_ids");
            if (privSessionIds.empty()) privSessionIds = weflow::ParseJsonStringArrayField(opts, "privateSessionIds");
            grpSessionIds = weflow::ParseJsonStringArrayField(opts, "group_session_ids");
            if (grpSessionIds.empty()) grpSessionIds = weflow::ParseJsonStringArrayField(opts, "groupSessionIds");
            mentionLimit = weflow::ParseJsonIntField(opts, "mention_limit", 100);
            privateLimit = weflow::ParseJsonIntField(opts, "private_limit", 100);
            std::string parsedWxid = weflow::ParseJsonStringField(opts, "my_wxid");
            if (!parsedWxid.empty()) myWxid = parsedWxid;
        }

        if (privSessionIds.empty() && ctx->session_db) {
            WCDB::Handle sessHandle = ctx->session_db->getHandle();
            WCDB::Statement stmt = weflow::MakeRawStatement(
                "SELECT username FROM SessionAbstract WHERE username NOT LIKE '%@chatroom' AND username NOT LIKE 'gh_%' AND username NOT IN ('filehelper', 'fmessage', 'medianote', 'floatbottle', 'newsapp', 'weixin')");
            if (sessHandle.prepare(stmt)) {
                while (sessHandle.step()) {
                    WCDB::Value uv = sessHandle.getValue(0);
                    if (!uv.isNull()) privSessionIds.push_back(static_cast<std::string>(uv.textValue()));
                }
                sessHandle.finalize();
            }
        }
        if (grpSessionIds.empty() && ctx->session_db) {
            WCDB::Handle sessHandle = ctx->session_db->getHandle();
            WCDB::Statement stmt = weflow::MakeRawStatement(
                "SELECT username FROM SessionAbstract WHERE username LIKE '%@chatroom'");
            if (sessHandle.prepare(stmt)) {
                while (sessHandle.step()) {
                    WCDB::Value uv = sessHandle.getValue(0);
                    if (!uv.isNull()) grpSessionIds.push_back(static_cast<std::string>(uv.textValue()));
                }
                sessHandle.finalize();
            }
        }

        int64_t inboundPeople = 0;
        int64_t outboundPeople = 0;
        int64_t repliedPeople = 0;

        std::ostringstream privSessionsOss;
        privSessionsOss << "[";
        bool firstPriv = true;
        size_t privCount = 0;

        for (const auto& sid : privSessionIds) {
            auto tables = weflow::ResolveSessionMessageTables(*ctx, sid);
            int64_t incoming = 0, outgoing = 0;
            int64_t firstInTs = 0, firstReplyTs = 0, latestTs = 0;
            int64_t anchorLocalId = 0, anchorCreateTime = 0;

            for (const auto& [meta, tbl] : tables) {
                std::string isSend = weflow::BuildIsSendExpr(meta, myWxid);
                std::ostringstream sql;
                sql << "SELECT " << isSend << " AS is_sent, create_time, local_id FROM " << tbl << " WHERE 1=1";
                if (beginTs > 0) sql << " AND create_time >= " << beginTs;
                if (endTs > 0) sql << " AND create_time <= " << endTs;
                sql << " ORDER BY create_time ASC";
                WCDB::Handle dbHandle = meta->db->getHandle();
                WCDB::Statement stmt = weflow::MakeRawStatement(sql.str());
                if (dbHandle.prepare(stmt)) {
                    while (dbHandle.step()) {
                        int64_t isSent = dbHandle.getValue(0).intValue();
                        int64_t ts = dbHandle.getValue(1).intValue();
                        int64_t lid = dbHandle.getValue(2).intValue();
                        if (isSent) {
                            outgoing++;
                            if (incoming > 0 && firstReplyTs == 0) firstReplyTs = ts;
                        } else {
                            incoming++;
                            if (firstInTs == 0) firstInTs = ts;
                        }
                        if (ts > latestTs) {
                            latestTs = ts;
                            anchorLocalId = lid;
                            anchorCreateTime = ts;
                        }
                    }
                    dbHandle.finalize();
                }
            }
            if (incoming > 0) inboundPeople++;
            if (outgoing > 0) outboundPeople++;
            bool replied = (incoming > 0 && outgoing > 0);
            if (replied) repliedPeople++;

            if (incoming > 0 || outgoing > 0) {
                if (privateLimit <= 0 || static_cast<int64_t>(privCount) < privateLimit) {
                    if (!firstPriv) privSessionsOss << ",";
                    firstPriv = false;
                    privSessionsOss << "{\"session_id\":\"" << weflow::EscapeJson(sid) << "\","
                                    << "\"incoming_count\":" << incoming << ","
                                    << "\"outgoing_count\":" << outgoing << ","
                                    << "\"replied\":" << (replied ? "true" : "false") << ","
                                    << "\"first_incoming_ts\":" << firstInTs << ","
                                    << "\"first_reply_ts\":" << firstReplyTs << ","
                                    << "\"latest_ts\":" << latestTs << ","
                                    << "\"anchor_local_id\":" << anchorLocalId << ","
                                    << "\"anchor_create_time\":" << anchorCreateTime << "}";
                    privCount++;
                }
            }
        }
        privSessionsOss << "]";

        // Mentions scanning in groups
        struct MentionItem {
            std::string sessionId;
            int64_t localId{0};
            int64_t createTime{0};
            std::string sender;
            std::string content;
            std::string source;
        };
        std::vector<MentionItem> mentionList;
        std::map<std::string, std::pair<int64_t, int64_t>> mentionGroups;

        for (const auto& gid : grpSessionIds) {
            auto tables = weflow::ResolveSessionMessageTables(*ctx, gid);
            for (const auto& [meta, tbl] : tables) {
                std::ostringstream sql;
                sql << "SELECT local_id, create_time, real_sender_id, message_content, source FROM " << tbl
                    << " WHERE (instr(message_content, '@') > 0 OR instr(message_content, '＠') > 0)";
                if (beginTs > 0) sql << " AND create_time >= " << beginTs;
                if (endTs > 0) sql << " AND create_time <= " << endTs;
                sql << " ORDER BY create_time DESC LIMIT 100";
                WCDB::Handle dbHandle = meta->db->getHandle();
                WCDB::Statement stmt = weflow::MakeRawStatement(sql.str());
                if (dbHandle.prepare(stmt)) {
                    while (dbHandle.step()) {
                        MentionItem item;
                        item.sessionId = gid;
                        item.localId = dbHandle.getValue(0).intValue();
                        item.createTime = dbHandle.getValue(1).intValue();
                        int64_t senderId = dbHandle.getValue(2).intValue();
                        item.sender = std::to_string(senderId);
                        if (meta) {
                            auto itU = meta->id_to_username.find(senderId);
                            if (itU != meta->id_to_username.end()) item.sender = itU->second;
                        }
                        item.content = dbHandle.getValue(3).isNull() ? "" : static_cast<std::string>(dbHandle.getValue(3).textValue());
                        item.source = dbHandle.getValue(4).isNull() ? "" : static_cast<std::string>(dbHandle.getValue(4).textValue());

                        mentionList.push_back(std::move(item));
                        auto& grp = mentionGroups[gid];
                        grp.first++;
                        if (item.createTime > grp.second) grp.second = item.createTime;
                        if (mentionLimit > 0 && static_cast<int64_t>(mentionList.size()) >= mentionLimit) break;
                    }
                    dbHandle.finalize();
                }
                if (mentionLimit > 0 && static_cast<int64_t>(mentionList.size()) >= mentionLimit) break;
            }
            if (mentionLimit > 0 && static_cast<int64_t>(mentionList.size()) >= mentionLimit) break;
        }

        std::ostringstream mentionsOss;
        mentionsOss << "[";
        for (size_t i = 0; i < mentionList.size(); ++i) {
            if (i > 0) mentionsOss << ",";
            const auto& m = mentionList[i];
            mentionsOss << "{\"session_id\":\"" << weflow::EscapeJson(m.sessionId) << "\","
                        << "\"local_id\":" << m.localId << ","
                        << "\"create_time\":" << m.createTime << ","
                        << "\"sender_username\":\"" << weflow::EscapeJson(m.sender) << "\","
                        << "\"message_content\":\"" << weflow::EscapeJson(m.content) << "\","
                        << "\"source\":\"" << weflow::EscapeJson(m.source) << "\"}";
        }
        mentionsOss << "]";

        std::ostringstream grpMentionsOss;
        grpMentionsOss << "[";
        bool firstGrp = true;
        for (const auto& [gid, stat] : mentionGroups) {
            if (!firstGrp) grpMentionsOss << ",";
            firstGrp = false;
            grpMentionsOss << "{\"session_id\":\"" << weflow::EscapeJson(gid) << "\","
                           << "\"count\":" << stat.first << ","
                           << "\"latest_ts\":" << stat.second << "}";
        }
        grpMentionsOss << "]";

        double replyRate = (inboundPeople > 0) ? (static_cast<double>(repliedPeople) / inboundPeople) : 0.0;

        std::ostringstream oss;
        oss << "{\"summary\":{"
            << "\"private_inbound_people\":" << inboundPeople << ","
            << "\"private_replied_people\":" << repliedPeople << ","
            << "\"private_outbound_people\":" << outboundPeople << ","
            << "\"private_reply_rate\":" << replyRate << ","
            << "\"mention_count\":" << mentionList.size() << ","
            << "\"mention_group_count\":" << mentionGroups.size() << "},"
            << "\"private_sessions\":" << privSessionsOss.str() << ","
            << "\"private_segments\":[],"
            << "\"mentions\":" << mentionsOss.str() << ","
            << "\"mention_groups\":" << grpMentionsOss.str() << ","
            << "\"diagnostics\":{\"scanned_dbs\":0,\"elapsed_ms\":0,\"truncated\":false,\"mention_truncated\":false,\"private_truncated\":false}}";

        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_my_footprint_stats", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_session_message_type_stats_batch(
    int64_t handle, const char* sessionIdsJson, const char* optionsJson, char** outJson) {
    if (!sessionIdsJson || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        int32_t startTime = 0, endTime = 0;
        if (optionsJson) {
            std::string opts(optionsJson);
            startTime = static_cast<int32_t>(weflow::ParseJsonIntField(opts, "begin", 0));
            endTime = static_cast<int32_t>(weflow::ParseJsonIntField(opts, "end", 0));
        }
        auto sessionIds = weflow::ParseJsonStringArray(sessionIdsJson);
        std::ostringstream oss;
        oss << "{";
        bool firstSess = true;
        for (const auto& sid : sessionIds) {
            char* itemJson = nullptr;
            if (wcdb_get_session_message_type_stats(handle, sid.c_str(), startTime, endTime, &itemJson) == 0 && itemJson) {
                if (!firstSess) oss << ",";
                firstSess = false;
                oss << "\"" << weflow::EscapeJson(sid) << "\":" << itemJson;
                wcdb_free_string(itemJson);
            }
        }
        oss << "}";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_session_message_type_stats_batch", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_session_message_date_counts_batch(
    int64_t handle, const char* sessionIdsJson, char** outJson) {
    if (!sessionIdsJson || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto sessionIds = weflow::ParseJsonStringArray(sessionIdsJson);
        std::ostringstream oss;
        oss << "{";
        bool firstSess = true;
        for (const auto& sid : sessionIds) {
            char* hmJson = nullptr;
            if (wcdb_get_session_activity_heatmap(handle, sid.c_str(), 0, 0, &hmJson) == 0 && hmJson) {
                if (!firstSess) oss << ",";
                firstSess = false;
                oss << "\"" << weflow::EscapeJson(sid) << "\":" << hmJson;
                wcdb_free_string(hmJson);
            }
        }
        oss << "}";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_session_message_date_counts_batch", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_recent_session_heat(int64_t handle, int64_t sinceTimestamp, char** outJson) {
    if (!outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx || !ctx->session_db) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        WCDB::Handle dbHandle = ctx->session_db->getHandle();
        std::ostringstream sql;
        sql << "SELECT username, last_timestamp, unread_count FROM SessionTable";
        if (sinceTimestamp > 0) {
            sql << " WHERE last_timestamp >= " << sinceTimestamp;
        }
        sql << " ORDER BY last_timestamp DESC";
        WCDB::Statement stmt = weflow::MakeRawStatement(sql.str());
        std::ostringstream oss;
        oss << "[";
        bool first = true;
        if (dbHandle.prepare(stmt)) {
            while (dbHandle.step()) {
                std::string u = dbHandle.getValue(0).isNull() ? "" : static_cast<std::string>(dbHandle.getValue(0).textValue());
                int64_t ts = dbHandle.getValue(1).intValue();
                int64_t unread = dbHandle.getValue(2).intValue();
                if (u.empty()) continue;
                if (!first) oss << ",";
                first = false;
                oss << "{\"username\":\"" << weflow::EscapeJson(u) << "\","
                    << "\"last_timestamp\":" << ts << ","
                    << "\"unread_count\":" << unread << "}";
            }
            dbHandle.finalize();
        }
        oss << "]";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_recent_session_heat", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_Hourly_Status(int64_t handle, const char* sessionId, char** outJson) {
    return weflow::QuerySessionTimeStatus(handle, sessionId, "%H", outJson);
}

WCDB_API int32_t wcdb_get_Daily_Status(int64_t handle, const char* sessionId, char** outJson) {
    return weflow::QuerySessionTimeStatus(handle, sessionId, "%Y-%m-%d", outJson);
}

WCDB_API int32_t wcdb_get_Weekly_Status(int64_t handle, const char* sessionId, char** outJson) {
    return weflow::QuerySessionTimeStatus(handle, sessionId, "%w", outJson);
}

WCDB_API int32_t wcdb_get_Monthly_Status(int64_t handle, const char* sessionId, char** outJson) {
    return weflow::QuerySessionTimeStatus(handle, sessionId, "%Y-%m", outJson);
}

} // extern "C"
