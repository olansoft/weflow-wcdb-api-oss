#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

// ============================================================================
// ABI-exact declarations for Tencent WCDB v2.1.15 (libWCDB.dylib)
// Reconstructed from LC_SYMTAB / __DATA_CONST vtables in libwcdb_api.dylib
// and libWCDB.dylib (@rpath/WCDB.framework/Versions/2.1.15/WCDB).
//
// In addition, libWCDB.dylib exports the full SQLCipher C API (sqlite3_*),
// which we also declare here for direct low-level SQL/transaction operations.
// ============================================================================

extern "C" {

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;

int sqlite3_open_v2(const char* filename, sqlite3** ppDb, int flags, const char* zVfs);
int sqlite3_close_v2(sqlite3* db);
int sqlite3_key(sqlite3* db, const void* pKey, int nKey);
int sqlite3_busy_timeout(sqlite3* db, int ms);
int sqlite3_exec(sqlite3* db, const char* sql, int (*callback)(void*, int, char**, char**), void* arg, char** errmsg);
int sqlite3_prepare_v2(sqlite3* db, const char* zSql, int nByte, sqlite3_stmt** ppStmt, const char** pzTail);
int sqlite3_step(sqlite3_stmt* pStmt);
int sqlite3_reset(sqlite3_stmt* pStmt);
int sqlite3_clear_bindings(sqlite3_stmt* pStmt);
int sqlite3_finalize(sqlite3_stmt* pStmt);

int sqlite3_column_count(sqlite3_stmt* pStmt);
const char* sqlite3_column_name(sqlite3_stmt* pStmt, int N);
int sqlite3_column_type(sqlite3_stmt* pStmt, int iCol);
const void* sqlite3_column_blob(sqlite3_stmt* pStmt, int iCol);
int sqlite3_column_bytes(sqlite3_stmt* pStmt, int iCol);
double sqlite3_column_double(sqlite3_stmt* pStmt, int iCol);
int sqlite3_column_int(sqlite3_stmt* pStmt, int iCol);
int64_t sqlite3_column_int64(sqlite3_stmt* pStmt, int iCol);
const unsigned char* sqlite3_column_text(sqlite3_stmt* pStmt, int iCol);

int sqlite3_bind_blob(sqlite3_stmt* pStmt, int idx, const void* val, int n, void (*destructor)(void*));
int sqlite3_bind_double(sqlite3_stmt* pStmt, int idx, double val);
int sqlite3_bind_int(sqlite3_stmt* pStmt, int idx, int val);
int sqlite3_bind_int64(sqlite3_stmt* pStmt, int idx, int64_t val);
int sqlite3_bind_null(sqlite3_stmt* pStmt, int idx);
int sqlite3_bind_text(sqlite3_stmt* pStmt, int idx, const char* val, int n, void (*destructor)(void*));

int sqlite3_changes(sqlite3* db);
int64_t sqlite3_last_insert_rowid(sqlite3* db);
int sqlite3_errcode(sqlite3* db);
const char* sqlite3_errmsg(sqlite3* db);
void sqlite3_free(void* ptr);

constexpr int SQLITE_OK = 0;
constexpr int SQLITE_ERROR = 1;
constexpr int SQLITE_BUSY = 5;
constexpr int SQLITE_LOCKED = 6;
constexpr int SQLITE_ROW = 100;
constexpr int SQLITE_DONE = 101;

constexpr int SQLITE_INTEGER = 1;
constexpr int SQLITE_FLOAT = 2;
constexpr int SQLITE_TEXT = 3;
constexpr int SQLITE_BLOB = 4;
constexpr int SQLITE_NULL = 5;

constexpr int SQLITE_OPEN_READONLY = 0x00000001;
constexpr int SQLITE_OPEN_READWRITE = 0x00000002;
constexpr int SQLITE_OPEN_CREATE = 0x00000004;
constexpr int SQLITE_OPEN_NOMUTEX = 0x00008000;

#define SQLITE_TRANSIENT ((void (*)(void*))-1)

} // extern "C"

namespace WCDB {

class UnsafeStringView {
public:
    UnsafeStringView(const char* str);
    UnsafeStringView(const char* str, size_t len);
    UnsafeStringView(const UnsafeStringView& other);
    ~UnsafeStringView();

    UnsafeStringView& operator=(const UnsafeStringView& other);
    UnsafeStringView& operator=(UnsafeStringView&& other);

    const char* data() const;
    size_t length() const;
    size_t size() const;
    operator std::string() const;

private:
    const char* m_data = "";
    size_t m_length = 0;
    uint64_t m_reserved = 0;
};

class StringView : public UnsafeStringView {
public:
    StringView();
};

class UnsafeData {
public:
    UnsafeData(unsigned char* buffer, size_t size);
    virtual ~UnsafeData();
    UnsafeData& operator=(UnsafeData&& other);

    static UnsafeData immutable(const unsigned char* buffer, size_t size);
    unsigned char* buffer();
    size_t size() const;

protected:
    uint8_t m_storage[88]{};
};

class Data : public UnsafeData {
public:
    ~Data() override;
};

class Error {
public:
    int code() const;
    const StringView& getMessage() const;
};

namespace Syntax {

class Identifier {
public:
    enum class Type : int {
        Invalid = 0,
    };
    using Iterator = std::function<void(Identifier&, bool, bool&)>;

    virtual ~Identifier();
    virtual std::unique_ptr<Identifier> clone() const;
    virtual Type getType() const = 0;
    virtual bool describable() const = 0;
    virtual bool describe(std::ostream& stream) const = 0;
    virtual void iterate(const Iterator& iterator, bool& stop);
};

} // namespace Syntax

class Statement {
public:
    Statement(std::shared_ptr<Syntax::Identifier>&& syntax);
    virtual ~Statement();

private:
    uint8_t m_storage[56]{};
};

class Value {
public:
    enum class Type : signed char {
        Null = 0,
        Integer = 1,
        Float = 2,
        Text = 3,
        BLOB = 4,
    };

    ~Value();
    Type getType() const;
    bool isNull() const;
    int64_t intValue() const;
    double floatValue() const;
    StringView textValue() const;
    Data blobValue() const;

private:
    alignas(8) uint8_t m_storage[104]{};
};

class StatementOperation {
public:
    virtual ~StatementOperation() = default;
    bool prepare(const Statement& statement);
    bool rawStep() asm("__ZN4WCDB18StatementOperation4stepEv");
    bool done();
    bool step() { return rawStep() && !done(); }
    void reset();
    void finalize();
    void clearBindings();

    void bindInteger(const int64_t& value, int index);
    void bindDouble(const double& value, int index);
    void bindText(const UnsafeStringView& value, int index);
    void bindBLOB(const UnsafeData& value, int index);
    void bindNull(int index);

    Value getValue(int index);
    UnsafeStringView getText(int index);
    UnsafeStringView getColumnName(int index);
    int getNumberOfColumns();
};

class Handle;

class HandleOperation {
public:
    using TransactionCallback = std::function<bool(Handle&)>;
    virtual ~HandleOperation() = default;
    bool execute(const Statement& statement);
    bool runTransaction(TransactionCallback transaction);
};

class Handle : public StatementOperation {
public:
    ~Handle() override;
    int getChanges();
    int64_t getLastInsertedRowID();

    HandleOperation* asHandleOperation() {
        return reinterpret_cast<HandleOperation*>(reinterpret_cast<uint8_t*>(this) + 8);
    }

private:
    alignas(8) uint8_t m_storage[160]{};
};

class Database {
public:
    enum class CipherVersion : int {
        DefaultVersion = 0,
        Version1 = 1,
        Version2 = 2,
        Version3 = 3,
        Version4 = 4,
    };
    enum class Priority : int {
        Highest = -2,
        High = -1,
        Default = 0,
        Low = 1,
        Lowest = 2,
    };
    using Invocation = std::function<bool(Handle&)>;

    Database(const UnsafeStringView& path, bool readonly = false);
    virtual ~Database();

    void setCipherKey(const UnsafeData& cipherKey, int pageSize = 4096, CipherVersion cipherVersion = CipherVersion::DefaultVersion);
    void setConfig(const UnsafeStringView& name, Invocation invocation, Invocation uninvocation = nullptr, Priority priority = Priority::Default);
    bool canOpen() const;
    const Error& getError() const;
    Handle getHandle();
    static void purgeAll();

    HandleOperation* asHandleOperation() {
        return reinterpret_cast<HandleOperation*>(reinterpret_cast<uint8_t*>(this) + 8);
    }

private:
    alignas(8) uint8_t m_storage[72]{};
};

} // namespace WCDB
