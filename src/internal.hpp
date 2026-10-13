#pragma once

#include "wcdb_abi.hpp"
#include "wcdb_api.h"

#include "md5.hpp"
#include <zstd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace weflow {

namespace fs = std::filesystem;

// ============================================================================
// RawIdentifier: Custom WCDB::Syntax::Identifier subclass reconstructed from
// RTTI `N12_GLOBAL__N_113RawIdentifierE` (vtable @ 0x1b52f8, methods @ 0x1550c8)
// Allows passing arbitrary raw SQL strings into WCDB::Statement.
// ============================================================================
class RawIdentifier final : public WCDB::Syntax::Identifier {
public:
    explicit RawIdentifier(std::string sql) : sql_(std::move(sql)) {}
    ~RawIdentifier() override = default;

    Type getType() const override { return Type::Invalid; }
    bool describable() const override { return true; }
    bool describe(std::ostream& stream) const override {
        stream.write(sql_.data(), static_cast<std::streamsize>(sql_.size()));
        return true;
    }

private:
    std::string sql_;
};

inline WCDB::Statement MakeRawStatement(const std::string& sql) {
    std::shared_ptr<WCDB::Syntax::Identifier> id = std::make_shared<RawIdentifier>(sql);
    return WCDB::Statement(std::move(id));
}

// ============================================================================
// Reconstructed Metadata Structures (from RTTI in __TEXT,__const)
// - MessageDbMeta    (N12_GLOBAL__N_113MessageDbMetaE)
// - MessageDbCatalog (N12_GLOBAL__N_116MessageDbCatalogE)
// - MediaDbMeta      (N12_GLOBAL__N_111MediaDbMetaE)
// - AccountContext   (N12_GLOBAL__N_114AccountContextE)
// - ExportCursorState(N12_GLOBAL__N_117ExportCursorStateE)
// ============================================================================

struct MessageDbMeta {
    std::string db_path;
    std::shared_ptr<WCDB::Database> db;
    std::unordered_set<std::string> tables; // Lowercase table name -> exists
    std::unordered_map<std::string, std::string> canonical_table_names; // lower -> actual
    std::unordered_map<std::string, std::vector<std::string>> table_columns;
    std::unordered_map<int64_t, std::string> id_to_username; // Name2Id rowid -> user_name
    std::unordered_map<std::string, int64_t> username_to_id; // Name2Id user_name -> rowid
    std::unordered_map<std::string, std::string> table_to_username; // "msg_<md5>" -> user_name
    bool name2id_loaded = false;
};

struct MessageDbCatalog {
    std::vector<std::shared_ptr<MessageDbMeta>> dbs;
    std::chrono::steady_clock::time_point built_at;
    bool initialized = false;
};

struct MediaDbMeta {
    std::string db_path;
    std::shared_ptr<WCDB::Database> db;
    std::vector<std::string> voice_tables;
};

struct AccountContext {
    int64_t handle = 0;
    std::string session_db_path;
    std::string db_storage_dir; // Parent ".../db_storage"
    std::string account_root_dir;
    std::string hex_key;
    std::vector<uint8_t> raw_key;
    int cipher_page_size = 4096; // 4096 (ps4096) or 1024 (ps1024)
    std::string my_wxid;

    std::shared_ptr<WCDB::Database> session_db;
    std::unordered_map<std::string, std::shared_ptr<WCDB::Database>> opened_dbs;
    std::shared_ptr<MessageDbCatalog> msg_catalog;
    std::vector<std::shared_ptr<MediaDbMeta>> media_dbs;

    // WAL monitor thread state (POSIX socket or Windows named pipe)
    std::atomic<bool> monitor_enabled{false};
    std::string monitor_pipe_name;
#if defined(_WIN32)
    void* monitor_listen_handle = nullptr;
    std::vector<void*> monitor_client_handles;
#else
    int monitor_listen_fd = -1;
    std::vector<int> monitor_client_fds;
#endif
    std::mutex monitor_clients_mutex;
    std::thread monitor_thread;

    mutable std::recursive_mutex mutex;
};

struct CursorRowItem {
    int64_t sort_seq = 0;
    int64_t create_time = 0;
    int64_t local_id = 0;
    size_t source_idx = 0;
    std::string json_row;
};

struct CursorSource {
    std::shared_ptr<MessageDbMeta> db_meta;
    std::string table_name;
    std::vector<std::string> selected_columns;
    std::deque<CursorRowItem> buffer;
    bool exhausted = false;
    bool has_last_key = false;
    int64_t last_sort_seq = 0;
    int64_t last_create_time = 0;
    int64_t last_local_id = 0;
};

struct ExportCursorState {
    int64_t cursor_id = 0;
    int64_t account_handle = 0;
    std::string session_id;
    int32_t batch_size = 200;
    bool ascending = false;
    int64_t start_timestamp = 0;
    int64_t end_timestamp = 0;
    std::string projection_mode = "full"; // "full" or "journey"
    bool has_initial_key = false;
    int64_t init_sort_seq = 0;
    int64_t init_create_time = 0;
    int64_t init_local_id = 0;
    std::vector<CursorSource> sources;
    std::mutex mutex;
};

// ============================================================================
// Global State & Helpers Declared Across Modules
// ============================================================================

void SetLastError(const std::string& operation, int code, const std::string& message);
char* AllocCString(const std::string& str);
std::shared_ptr<AccountContext> GetAccount(int64_t handle);

std::vector<uint8_t> HexToBytes(const std::string& hex);
std::string BytesToHex(const uint8_t* data, size_t len);
std::string Base64Encode(const uint8_t* data, size_t len);
std::vector<uint8_t> Base64Decode(const std::string& input);

std::string Md5HexLower(const std::string& input);
std::string EscapeJson(const std::string& s);
std::string EscapeSqlLiteral(const std::string& s);
std::string UnescapeXmlEntities(const std::string& s);
std::string ToLower(std::string s);

// Minimal JSON parsers matching sub_421c..sub_5000 in libwcdb_api.dylib
std::vector<std::string> ParseJsonStringArray(const char* jsonStr);
std::vector<std::string> ParseJsonObjectArray(const char* jsonStr);
std::vector<std::string> ParseJsonStringArrayField(const std::string& json, const std::string& key);
std::string ParseJsonStringField(const std::string& json, const std::string& key, const std::string& defaultVal = "");
int64_t ParseJsonIntField(const std::string& json, const std::string& key, int64_t defaultVal = 0);
bool ParseJsonBoolField(const std::string& json, const std::string& key, bool defaultVal = false);

// Zstd decompression & Protobuf wire-format extraction
bool TryDecompressZstd(const void* src, size_t srcLen, std::string& outDecompressed);

struct ParsedPackedInfo {
    std::string image_md5;
    std::string image_dat_name;
    std::string image_path;
    std::string video_md5;
    std::string video_file_name;
    std::string emoji_md5;
    std::string emoji_cdn_url;
    std::string aes_key;
    std::string file_name;
    int64_t file_size = 0;
    std::string extra_text;
};
ParsedPackedInfo ParsePackedInfoProto(const uint8_t* data, size_t len);

// WCDB Database Opening & Read Optimizations
void ApplyReadOptimizations(WCDB::Database& db);
std::shared_ptr<WCDB::Database> OpenEncryptedDb(AccountContext& ctx, const std::string& dbPath, bool readonly = false);
std::shared_ptr<WCDB::Database> ResolveDbByKindOrPath(AccountContext& ctx, const std::string& kindOrPath);
std::shared_ptr<MessageDbCatalog> EnsureMessageCatalog(AccountContext& ctx, bool forceRefresh = false);
std::vector<std::pair<std::shared_ptr<MessageDbMeta>, std::string>> ResolveSessionMessageTables(
    AccountContext& ctx, const std::string& sessionId);

// Anti-revoke label state
std::string GetAntiRevokeSelfLabel();
std::string GetAntiRevokeOtherPrefix();

} // namespace weflow
