#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define WCDB_API __declspec(dllexport)
#else
#define WCDB_API __attribute__((visibility("default")))
#endif

// ============================================================================
// 1. Core Lifecycle & Account Management
// ============================================================================
WCDB_API int32_t InitProtection(const char* resourcePath);
WCDB_API int32_t wcdb_runtime_policy_version(void);
WCDB_API int32_t wcdb_authenticate(const char* token, int32_t port, int64_t issuedAtMs);
WCDB_API void VerifyUser(int64_t hwnd, const char* message, char* outResult, int maxLen);

WCDB_API int32_t wcdb_init(void);
WCDB_API int32_t wcdb_shutdown(void);
WCDB_API int32_t wcdb_purge_memory(void);

WCDB_API int32_t wcdb_open_account(const char* sessionDbPath, const char* hexKey, int64_t* outHandle);
WCDB_API int32_t wcdb_close_account(int64_t handle);
WCDB_API int32_t wcdb_set_my_wxid(int64_t handle, const char* wxid);
WCDB_API int32_t wcdb_set_my_account_id(int64_t handle, const char* accountId);
WCDB_API int32_t wcdb_set_monitor(int64_t handle, bool enable);
WCDB_API int32_t wcdb_get_monitor_pipe_name(char** outPipeName);

WCDB_API void wcdb_free_string(char* str);
WCDB_API void wcdb_free_binary(uint8_t* ptr);
WCDB_API int32_t wcdb_get_last_error(char** outErrorJson);

// ============================================================================
// 2. Sessions, Contacts & Avatars
// ============================================================================
WCDB_API int32_t wcdb_get_sessions(int64_t handle, char** outJson);
WCDB_API int32_t wcdb_get_contact(int64_t handle, const char* username, char** outJson);
WCDB_API int32_t wcdb_get_contacts(int64_t handle, char** outJson);
WCDB_API int32_t wcdb_get_contacts_Info(int64_t handle, char** outJson);
WCDB_API int32_t wcdb_search_contacts(int64_t handle, const char* keyword, int32_t limit, char** outJson);
WCDB_API int32_t wcdb_get_display_names(int64_t handle, const char* usernamesJson, char** outJson);
WCDB_API int32_t wcdb_get_avatars(int64_t handle, const char* usernamesJson, char** outJson);
WCDB_API int32_t wcdb_get_contact_alias_map(int64_t handle, const char* usernamesJson, char** outJson);
WCDB_API int32_t wcdb_get_contact_friend_flags(int64_t handle, const char* usernamesJson, char** outJson);
WCDB_API int32_t wcdb_get_chatroom_info(int64_t handle, const char* chatroomId, char** outJson);
WCDB_API int32_t wcdb_get_chatroom_members(int64_t handle, const char* chatroomId, char** outJson);
WCDB_API int32_t wcdb_get_chat_room_ext_buffer(int64_t handle, const char* chatroomId, char** outJson);
WCDB_API int32_t wcdb_get_head_image_buffers(int64_t handle, const char* usernamesJson, char** outJson);

// ============================================================================
// 3. Messages, Cursors, Search & Media Stream
// ============================================================================
WCDB_API int32_t wcdb_get_messages(int64_t handle, const char* sessionId, int32_t limit, int32_t offset, char** outJson);
WCDB_API int32_t wcdb_get_message_count(int64_t handle, const char* sessionId, int32_t* outCount);
WCDB_API int32_t wcdb_get_session_message_TotalCount(int64_t handle, const char* sessionId, int32_t* outCount);
WCDB_API int32_t wcdb_get_all_messages_count(int64_t handle, char** outJson);
WCDB_API int32_t wcdb_get_messages_by_type(int64_t handle, const char* sessionId, int64_t localType, int32_t ascending, int32_t limit, int32_t offset, char** outJson);
WCDB_API int32_t wcdb_get_message_meta(int64_t handle, const char* dbPath, const char* tableName, int32_t limit, int32_t offset, char** outJson);
WCDB_API int32_t wcdb_get_last_messages_batch(int64_t handle, const char* sessionIdsJson, char** outJson);
WCDB_API int32_t wcdb_scan_media_stream(int64_t handle, const char* sessionIdsJson, int32_t mediaType, int32_t beginTimestamp, int32_t endTimestamp, int32_t limit, int32_t offset, char** outJson, int32_t* outHasMore);

WCDB_API int32_t wcdb_open_message_cursor(int64_t handle, const char* sessionId, int32_t batchSize, int32_t ascending, int32_t beginTimestamp, int32_t endTimestamp, int64_t* outCursor);
WCDB_API int32_t wcdb_open_message_cursor_with_key(int64_t handle, const char* sessionId, int32_t batchSize, int32_t ascending, int64_t startTimestamp, int64_t endTimestamp, const char* projectionMode, int32_t hasKey, int64_t keySortSeq, int64_t keyCreateTime, int64_t keyLocalId, int64_t* outCursor);
WCDB_API int32_t wcdb_fetch_message_cursor_batch(int64_t handle, int64_t cursor, char** outJson, int32_t* outHasMore);
WCDB_API int32_t wcdb_close_message_cursor(int64_t handle, int64_t cursor);

WCDB_API int32_t wcdb_get_session_timeline(int64_t handle, const char* sessionId, char** outJson);
WCDB_API int32_t wcdb_locate_message_anchor(int64_t handle, const char* sessionId, const char* anchorSpecJson, char** outJson);
WCDB_API int32_t wcdb_get_messages_around_anchor(int64_t handle, const char* sessionId, int64_t anchorSortSeq, int64_t anchorCreateTime, int64_t anchorLocalId, int32_t beforeCount, int32_t afterCount, int32_t includeAnchor, char** outJson);

WCDB_API int32_t wcdb_search_fts_messages(int64_t handle, const char* keywordJson, const char* sessionId, int32_t limit, int32_t offset, int64_t startTime, int64_t endTime, char** outJson);
WCDB_API int32_t wcdb_probe_fts_schema(int64_t handle, char** outJson);
WCDB_API int32_t wcdb_search_messages_in_session(int64_t handle, const char* sessionId, const char* keyword, int32_t limit, int64_t startTime, int64_t endTime, char** outJson);
WCDB_API int32_t wcdb_get_session_activity_heatmap(int64_t handle, const char* sessionId, int64_t startTime, int64_t endTime, char** outJson);

// ============================================================================
// 4. Statistics & Reports
// ============================================================================
WCDB_API int32_t wcdb_get_aggregate_stats(int64_t handle, const char* sessionIdsJson, int32_t begin, int32_t end, char** outJson);
WCDB_API int32_t wcdb_get_global_message_type_counts(int64_t handle, char** outJson);
WCDB_API int32_t wcdb_get_available_years(int64_t handle, const char* sessionIdsJson, char** outJson);
WCDB_API int32_t wcdb_get_annual_report_stats(int64_t handle, const char* sessionIdsJson, int32_t begin, int32_t end, char** outJson);
WCDB_API int32_t wcdb_get_annual_report_extras(int64_t handle, const char* sessionIdsJson, int32_t begin, int32_t end, int32_t peakBegin, int32_t peakEnd, char** outJson);
WCDB_API int32_t wcdb_get_dual_report_stats(int64_t handle, const char* sessionId, int32_t begin, int32_t end, char** outJson);
WCDB_API int32_t wcdb_get_group_stats(int64_t handle, const char* chatroomId, int32_t begin, int32_t end, char** outJson);
WCDB_API int32_t wcdb_get_my_footprint_stats(int64_t handle, const char* optionsJson, char** outJson);
WCDB_API int32_t wcdb_get_session_message_type_stats_batch(int64_t handle, const char* sessionIdsJson, const char* optionsJson, char** outJson);
WCDB_API int32_t wcdb_get_session_message_date_counts_batch(int64_t handle, const char* sessionIdsJson, char** outJson);
WCDB_API int32_t wcdb_get_recent_session_heat(int64_t handle, int64_t sinceTimestamp, char** outJson);

WCDB_API int32_t wcdb_get_Hourly_Status(int64_t handle, const char* sessionId, char** outJson);
WCDB_API int32_t wcdb_get_Daily_Status(int64_t handle, const char* sessionId, char** outJson);
WCDB_API int32_t wcdb_get_Weekly_Status(int64_t handle, const char* sessionId, char** outJson);
WCDB_API int32_t wcdb_get_Monthly_Status(int64_t handle, const char* sessionId, char** outJson);

// ============================================================================
// 5. Media, Hardlinks, Voice, Emoticons, SNS & AI
// ============================================================================
WCDB_API int32_t wcdb_resolve_image_hardlink(int64_t handle, const char* md5, const char* accountDir, char** outJson);
WCDB_API int32_t wcdb_resolve_video_hardlink_md5(int64_t handle, const char* md5, const char* dbPath, char** outJson);
WCDB_API int32_t wcdb_get_voice_data(int64_t handle, const char* sessionId, int32_t createTime, int32_t localId, int64_t svrId, const char* candidatesJson, char** outHex);
WCDB_API int32_t wcdb_get_voice_raw_data(int64_t handle, const char* sessionId, int64_t serverId, int64_t localId, uint8_t** outBuf, int32_t* outLen);

WCDB_API int32_t wcdb_get_emoticon_info(int64_t handle, const char* md5, char** outJson);
WCDB_API int32_t wcdb_get_custom_emoticons(int64_t handle, int32_t typeFilter, int32_t limit, int32_t offset, char** outJson);
WCDB_API int32_t wcdb_get_emoticon_packages(int64_t handle, int32_t limit, int32_t offset, char** outJson);
WCDB_API int32_t wcdb_add_custom_emoticon(int64_t handle, const char* emoticonJson, char** outJson);
WCDB_API int32_t wcdb_delete_custom_emoticon(int64_t handle, const char* md5, char** outJson);
WCDB_API int32_t wcdb_update_custom_emoticon(int64_t handle, const char* md5, const char* patchJson, char** outJson);

WCDB_API int32_t wcdb_get_sns_feed(int64_t handle, int32_t limit, int32_t offset, const char* usernamesJson, const char* keyword, int64_t startTime, int64_t endTime, char** outJson);
WCDB_API int32_t wcdb_get_sns_count(int64_t handle, const char* usernamesJson, const char* keyword, int64_t startTime, int64_t endTime, int32_t* outCount);
WCDB_API int32_t wcdb_search_sns(int64_t handle, const char* keyword, int32_t limit, int32_t offset, char** outJson);
WCDB_API int32_t wcdb_get_sns_Bounds(int64_t handle, char** outJson);
WCDB_API int32_t wcdb_get_sns_Notification(int64_t handle, int32_t limit, int32_t offset, char** outJson);

WCDB_API int32_t wcdb_ai_query_timeline(int64_t handle, const char* queryJson, char** outJson);
WCDB_API int32_t wcdb_ai_query_topic_stats(int64_t handle, const char* queryJson, char** outJson);
WCDB_API int32_t wcdb_ai_query_source_refs(int64_t handle, const char* queryJson, char** outJson);
WCDB_API int32_t wcdb_ai_get_recent_messages(int64_t handle, const char* sessionId, int32_t limit, char** outJson);
WCDB_API int32_t wcdb_ai_get_messages_before(int64_t handle, const char* sessionId, int64_t beforeSeqOrTime, int32_t limit, char** outJson);
WCDB_API int32_t wcdb_ai_get_messages_after(int64_t handle, const char* sessionId, int64_t afterSeqOrTime, int32_t limit, char** outJson);
WCDB_API int32_t wcdb_ai_get_message_context(int64_t handle, const char* sessionId, int64_t localId, int32_t radius, char** outJson);
WCDB_API int32_t wcdb_ai_get_search_message_context(void);
WCDB_API int32_t wcdb_ai_get_conversation_between(void);
WCDB_API int32_t wcdb_ai_search_sessions(void);
WCDB_API int32_t wcdb_ai_get_session_messages(void);
WCDB_API int32_t wcdb_ai_get_session_summaries(void);

// ============================================================================
// 6. Table Introspection, Snapshots, CRUD & Anti-Revoke
// ============================================================================
WCDB_API int32_t wcdb_list_tables(int64_t handle, const char* kind, const char* dbPath, char** outJson);
WCDB_API int32_t wcdb_get_table_schema(int64_t handle, const char* kind, const char* dbPath, const char* tableName, char** outJson);
WCDB_API int32_t wcdb_get_message_table_columns(int64_t handle, const char* dbPath, const char* tableName, char** outJson);
WCDB_API int32_t wcdb_get_message_table_time_range(int64_t handle, const char* dbPath, const char* tableName, char** outJson);

WCDB_API int32_t wcdb_export_table_snapshot(int64_t handle, const char* kind, const char* dbPath, const char* tableName, const char* outputPath, char** outJson);
WCDB_API int32_t wcdb_import_table_snapshot(int64_t handle, const char* kind, const char* dbPath, const char* tableName, const char* inputPath, char** outJson);
WCDB_API int32_t wcdb_import_table_snapshot_with_schema(int64_t handle, const char* kind, const char* dbPath, const char* tableName, const char* inputPath, const char* createTableSql, char** outJson);

WCDB_API int32_t wcdb_exec_query(int64_t handle, const char* kind, const char* path, const char* sql, char** outJson);
WCDB_API int32_t wcdb_test_create_custom_db(int64_t handle, const char* dbPath, const char* tableName, char** outJson);

WCDB_API int32_t wcdb_insert_text_message(int64_t handle, const char* sessionId, const char* content, int64_t status, int64_t createTime, char** outJson);
WCDB_API int32_t wcdb_update_message_content(int64_t handle, const char* sessionId, int64_t localId, int64_t createTimeHint, const char* newContent, char** outJson);
WCDB_API int32_t wcdb_update_message_create_time(int64_t handle, const char* sessionId, int64_t localId, int64_t oldCreateTimeHint, int64_t newCreateTime, char** outJson);
WCDB_API int32_t wcdb_update_message_sender(int64_t handle, const char* sessionId, int64_t localId, int64_t createTimeHint, const char* newSenderUsername, char** outJson);
WCDB_API int32_t wcdb_swap_two_messages_order(int64_t handle, const char* sessionId, int64_t localIdA, int64_t createTimeHintA, int64_t localIdB, int64_t createTimeHintB, char** outJson);
WCDB_API int32_t wcdb_delete_message(int64_t handle, const char* sessionId, int64_t localId, int32_t createTime, const char* dbPathHint, char** outError);

WCDB_API int32_t wcdb_insert_session(int64_t handle, const char* username, int32_t type, const char* summary, int64_t lastTimestamp, int32_t unreadCount, int32_t isHidden, char** outJson);
WCDB_API int32_t wcdb_update_session(int64_t handle, const char* username, const char* patchJson, char** outJson);
WCDB_API int32_t wcdb_delete_session(int64_t handle, const char* username, int32_t deleteMessages, char** outJson);
WCDB_API int32_t wcdb_reorder_sessions_by_time(int64_t handle, char** outJson);
WCDB_API int32_t wcdb_delete_message_table(int64_t handle, const char* sessionId, char** outJson);

WCDB_API int32_t wcdb_setup_anti_revoke_triggers(int64_t handle, const char* sessionId, char** outStatusMsg);
WCDB_API int32_t wcdb_remove_anti_revoke_triggers(int64_t handle, const char* sessionId, char** outStatusMsg);
WCDB_API int32_t wcdb_cleanup_all_anti_revoke_triggers(int64_t handle, char** outJson);
WCDB_API int32_t wcdb_check_and_install_anti_revoke_for_session(int64_t handle, const char* sessionId, char** outStatusMsg);
WCDB_API int32_t wcdb_set_anti_revoke_self_label(const char* label);
WCDB_API int32_t wcdb_set_anti_revoke_other_prefix(const char* prefix);
WCDB_API int32_t wcdb_get_anti_revoke_Labels(char** outJson);

// ============================================================================
// 7. Cloud Telemetry
// ============================================================================
WCDB_API int32_t wcdb_submit_cloud_report(const char* payloadJson, char** outResponseJson);
WCDB_API int32_t wcdb_submit_cloud_reports_batch(const char* payloadsJson, char** outResponseJson);
WCDB_API int32_t wcdb_set_cloud_token(const char* token);
WCDB_API int32_t wcdb_clear_cloud_token(void);
WCDB_API int32_t wcdb_fetch_cloud_token(char** outResponseJson);
WCDB_API int32_t wcdb_fetch_and_set_cloud_token(char** outResponseJson);
WCDB_API int32_t wcdb_get_cloud_token_status(char** outStatusJson);
WCDB_API int32_t wcdb_set_cloud_base_url(const char* baseUrl);
WCDB_API int32_t wcdb_get_cloud_base_url(char** outBaseUrl);

// ============================================================================
// 8. Binary & Worker Compatibility Aliases (wcdbWorker.js / libwcdb_api.dylib)
// ============================================================================
WCDB_API int32_t wcdb_fetch_message_batch(int64_t handle, int64_t cursor, char** outJson, int32_t* outHasMore);
WCDB_API int32_t wcdb_set_message_cursor_projection(int64_t handle, int64_t cursor, int32_t projection);
WCDB_API int32_t wcdb_get_avatar_urls(int64_t handle, const char* usernamesJson, char** outJson);
WCDB_API int32_t wcdb_get_contact_status(int64_t handle, const char* usernamesJson, char** outJson);
WCDB_API int32_t wcdb_get_contact_type_counts(int64_t handle, char** outJson);
WCDB_API int32_t wcdb_get_contacts_compact(int64_t handle, const char* usernamesJson, char** outJson);
WCDB_API int32_t wcdb_get_group_member_count(int64_t handle, const char* chatroomId, int32_t* outCount);
WCDB_API int32_t wcdb_get_group_member_counts(int64_t handle, const char* chatroomIdsJson, char** outJson);
WCDB_API int32_t wcdb_get_group_members(int64_t handle, const char* chatroomId, char** outJson);
WCDB_API int32_t wcdb_get_group_nicknames(int64_t handle, const char* chatroomId, char** outJson);
WCDB_API int32_t wcdb_get_message_tables(int64_t handle, const char* sessionId, char** outJson);
WCDB_API int32_t wcdb_get_message_table_stats(int64_t handle, const char* sessionId, char** outJson);
WCDB_API int32_t wcdb_get_message_dates(int64_t handle, const char* sessionId, char** outJson);
WCDB_API int32_t wcdb_get_message_by_id(int64_t handle, const char* sessionId, int32_t localId, char** outJson);
WCDB_API int32_t wcdb_get_message_by_svrid(int64_t handle, const char* sessionId, const char* svrid, char** outJson);
WCDB_API int32_t wcdb_list_message_dbs(int64_t handle, char** outJson);
WCDB_API int32_t wcdb_list_media_dbs(int64_t handle, char** outJson);
WCDB_API int32_t wcdb_get_db_status(int64_t handle, char** outJson);
WCDB_API int32_t wcdb_get_logs(char** outJson);
WCDB_API int32_t wcdb_get_emoticon_cdn_url(int64_t handle, const char* dbPath, const char* md5, char** outUrl);
WCDB_API int32_t wcdb_get_emoticon_caption(int64_t handle, const char* dbPath, const char* md5, char** outCaption);
WCDB_API int32_t wcdb_get_emoticon_caption_strict(int64_t handle, const char* md5, char** outCaption);
WCDB_API int32_t wcdb_get_voice_data_batch(int64_t handle, const char* requestsJson, char** outJson);
WCDB_API int32_t wcdb_get_media_schema_summary(int64_t handle, const char* dbPath, char** outJson);
WCDB_API int32_t wcdb_get_session_message_counts(int64_t handle, const char* sessionIdsJson, char** outJson);
WCDB_API int32_t wcdb_get_session_message_type_stats(int64_t handle, const char* sessionId, int32_t beginTimestamp, int32_t endTimestamp, char** outJson);
WCDB_API int32_t wcdb_get_session_message_date_counts(int64_t handle, const char* sessionId, char** outJson);
WCDB_API int32_t wcdb_search_messages(int64_t handle, const char* sessionId, const char* keyword, int32_t limit, int32_t offset, int32_t beginTimestamp, int32_t endTimestamp, char** outJson);
WCDB_API int32_t wcdb_ai_query_session_candidates(int64_t handle, const char* optionsJson, char** outJson);
WCDB_API int32_t wcdb_get_sns_timeline(int64_t handle, int32_t limit, int32_t offset, const char* username, const char* keyword, int32_t startTime, int32_t endTime, char** outJson);
WCDB_API int32_t wcdb_get_sns_annual_stats(int64_t handle, int32_t begin, int32_t end, char** outJson);
WCDB_API int32_t wcdb_get_sns_usernames(int64_t handle, char** outJson);
WCDB_API int32_t wcdb_get_sns_export_stats(int64_t handle, const char* myWxid, char** outJson);
WCDB_API int32_t wcdb_resolve_image_hardlink_batch(int64_t handle, const char* requestsJson, char** outJson);
WCDB_API int32_t wcdb_resolve_video_hardlink_md5_batch(int64_t handle, const char* requestsJson, char** outJson);
WCDB_API int32_t wcdb_install_message_anti_revoke_trigger(int64_t handle, const char* sessionId, char** outError);
WCDB_API int32_t wcdb_uninstall_message_anti_revoke_trigger(int64_t handle, const char* sessionId, char** outError);
WCDB_API int32_t wcdb_check_message_anti_revoke_trigger(int64_t handle, const char* sessionId, int32_t* outInstalled);
WCDB_API int32_t wcdb_install_sns_block_delete_trigger(int64_t handle, char** outError);
WCDB_API int32_t wcdb_uninstall_sns_block_delete_trigger(int64_t handle, char** outError);
WCDB_API int32_t wcdb_check_sns_block_delete_trigger(int64_t handle, int32_t* outInstalled);
WCDB_API int32_t wcdb_delete_sns_post(int64_t handle, const char* postId, char** outError);
WCDB_API int32_t wcdb_mark_all_sessions_read(int64_t handle, char** outError);
WCDB_API int32_t wcdb_update_message(int64_t handle, const char* sessionId, int64_t localId, int32_t createTime, const char* newContent, char** outError);
WCDB_API int32_t wcdb_start_monitor_pipe(void);
WCDB_API void wcdb_stop_monitor_pipe(void);
WCDB_API int32_t wcdb_cloud_init(int32_t intervalSeconds);
WCDB_API int32_t wcdb_cloud_report(const char* statsJson);
WCDB_API void wcdb_cloud_stop(void);

#ifdef __cplusplus
} // extern "C"
#endif
