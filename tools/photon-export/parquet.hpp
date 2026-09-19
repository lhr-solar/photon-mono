// Minimal Apache Parquet v1 writer.
//
// Scope is deliberately narrow: flat schemas, PLAIN-encoded uncompressed v1
// data pages, RLE definition levels for optional columns. That is enough for an
// analytics export and it avoids vendoring Arrow, which would be two orders of
// magnitude more dependency than this tool justifies.
//
// What is NOT implemented, so callers do not assume it: nested/repeated groups,
// dictionary encoding, compression, page-level statistics, column indexes,
// bloom filters, encryption, INT96.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace photon::pq {

enum class Type : std::uint8_t { Boolean, Int32, Int64, Double, ByteArray };

// Annotations this writer knows how to emit. Each sets both the legacy
// ConvertedType and the modern LogicalType so old and new readers agree.
enum class Logical : std::uint8_t { None, Utf8, Uint32, TimestampMillisUtc };

struct ColumnSpec {
    std::string name;
    Type type = Type::Double;
    Logical logical = Logical::None;
    // Optional columns cost one RLE definition-level run per page and store no
    // bytes for null entries, so an all-null column in a row group is nearly
    // free. That is what lets one schema carry both raw and rollup rows.
    bool optional = false;
};

struct WriterOptions {
    // Rows buffered before a row group is serialized and written. This is the
    // writer's entire memory footprint, so it is also the export's.
    std::uint32_t rowGroupRows = 262144;
    std::uint32_t pageRows = 32768;
    bool statistics = true;
};

class Writer {
public:
    Writer() = default;
    ~Writer();
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    bool open(const std::filesystem::path& path, std::vector<ColumnSpec> columns, WriterOptions opts,
              std::string* error = nullptr);

    // Footer key/value metadata. Must be called before close().
    void setMetadata(std::string key, std::string value);

    // One call per column per row, then endRow(). A column left unset or set
    // twice is a caller bug and latches a failure rather than writing a torn
    // row group.
    void setNull(std::size_t col);
    void setBool(std::size_t col, bool v);
    void setInt32(std::size_t col, std::int32_t v);
    void setInt64(std::size_t col, std::int64_t v);
    void setDouble(std::size_t col, double v);
    void setString(std::size_t col, std::string_view v);

    bool endRow(std::string* error = nullptr);
    bool close(std::string* error = nullptr);

    std::uint64_t rows() const { return totalRows_; }
    std::uint64_t bytes() const { return bytesWritten_; }
    std::uint32_t rowGroups() const { return rowGroupCount_; }
    bool failed() const { return failed_; }
    const std::string& error() const { return error_; }

private:
    struct Stats {
        bool any = false;
        std::int64_t minI = 0;
        std::int64_t maxI = 0;
        double minD = 0.0;
        double maxD = 0.0;
        std::string minS;
        std::string maxS;
        std::int64_t nullCount = 0;
    };

    struct Column {
        ColumnSpec spec;
        std::vector<std::int32_t> i32;
        std::vector<std::int64_t> i64;
        std::vector<double> f64;
        std::vector<std::uint8_t> boolv;
        std::vector<std::string> str;
        // 0/1 per row, only for optional columns.
        std::vector<std::uint8_t> defLevels;
        std::uint64_t rowCount = 0;
        Stats stats;
    };

    struct ChunkMeta {
        std::int64_t dataPageOffset = 0;
        std::int64_t totalBytes = 0;
        std::int64_t numValues = 0;
        Stats stats;
    };

    struct RowGroupMeta {
        std::vector<ChunkMeta> chunks;
        std::int64_t numRows = 0;
        std::int64_t totalBytes = 0;
    };

    bool fail(std::string message, std::string* error = nullptr);
    bool writeBytes(const void* data, std::size_t n, std::string* error);
    bool flushRowGroup(std::string* error);
    void serializeColumn(Column& col, std::vector<std::uint8_t>& out, ChunkMeta& meta) const;
    void resetColumns();

    std::ofstream out_;
    std::filesystem::path path_;
    std::vector<Column> cols_;
    WriterOptions opts_{};
    std::vector<std::pair<std::string, std::string>> kv_;
    std::vector<RowGroupMeta> groups_;
    std::uint64_t groupRows_ = 0;
    std::uint64_t totalRows_ = 0;
    std::uint64_t bytesWritten_ = 0;
    std::uint32_t rowGroupCount_ = 0;
    bool open_ = false;
    bool closed_ = false;
    bool failed_ = false;
    std::string error_;
};

// Exposed for unit tests: the primitives everything above is built on, checked
// against hand-computed byte sequences rather than against this writer.
namespace detail {
void writeVarint(std::vector<std::uint8_t>& out, std::uint64_t v);
void writeZigzag32(std::vector<std::uint8_t>& out, std::int32_t v);
void writeZigzag64(std::vector<std::uint8_t>& out, std::int64_t v);
// Thrift compact field header for (id, compactType) given the previous id.
void writeFieldHeader(std::vector<std::uint8_t>& out, std::int16_t id, std::int16_t& lastId, std::uint8_t compactType);
// RLE/bit-packing-hybrid run list for 1-bit definition levels.
void writeDefLevels(std::vector<std::uint8_t>& out, const std::vector<std::uint8_t>& levels);
void writePlainBooleans(std::vector<std::uint8_t>& out, const std::vector<std::uint8_t>& values);
}  // namespace detail

}  // namespace photon::pq
