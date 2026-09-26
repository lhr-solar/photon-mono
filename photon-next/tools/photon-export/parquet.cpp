#include "parquet.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace photon::pq {
namespace {

// Thrift compact protocol element types.
constexpr std::uint8_t kTStop = 0x00;
constexpr std::uint8_t kTBoolTrue = 1;
constexpr std::uint8_t kTBoolFalse = 2;
constexpr std::uint8_t kTByte = 3;
constexpr std::uint8_t kTI16 = 4;
constexpr std::uint8_t kTI32 = 5;
constexpr std::uint8_t kTI64 = 6;
constexpr std::uint8_t kTBinary = 8;
constexpr std::uint8_t kTList = 9;
constexpr std::uint8_t kTStruct = 12;

// parquet.thrift enum values.
constexpr std::int32_t kPqBoolean = 0;
constexpr std::int32_t kPqInt32 = 1;
constexpr std::int32_t kPqInt64 = 2;
constexpr std::int32_t kPqDouble = 5;
constexpr std::int32_t kPqByteArray = 6;

constexpr std::int32_t kConvUtf8 = 0;
constexpr std::int32_t kConvTimestampMillis = 9;
constexpr std::int32_t kConvUint32 = 13;

constexpr std::int32_t kRepRequired = 0;
constexpr std::int32_t kRepOptional = 1;

constexpr std::int32_t kEncPlain = 0;
constexpr std::int32_t kEncRle = 3;

constexpr std::int32_t kCodecUncompressed = 0;
constexpr std::int32_t kPageTypeData = 0;

void appendU32Le(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 24));
}

void appendU64Le(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}

std::int32_t physicalTypeOf(Type t) {
    switch (t) {
        case Type::Boolean:
            return kPqBoolean;
        case Type::Int32:
            return kPqInt32;
        case Type::Int64:
            return kPqInt64;
        case Type::Double:
            return kPqDouble;
        case Type::ByteArray:
            break;
    }
    return kPqByteArray;
}

// Thrift compact protocol writer. Field ids inside a struct are always emitted
// in ascending order so the short-form delta header is always usable.
class Thrift {
public:
    explicit Thrift(std::vector<std::uint8_t>& out) : out_(out) {}

    void structBegin() {
        stack_.push_back(last_);
        last_ = 0;
    }
    void structEnd() {
        out_.push_back(kTStop);
        last_ = stack_.back();
        stack_.pop_back();
    }
    void field(std::int16_t id, std::uint8_t type) { detail::writeFieldHeader(out_, id, last_, type); }

    void boolField(std::int16_t id, bool v) { field(id, v ? kTBoolTrue : kTBoolFalse); }
    void byteField(std::int16_t id, std::uint8_t v) {
        field(id, kTByte);
        out_.push_back(v);
    }
    void i16Field(std::int16_t id, std::int16_t v) {
        field(id, kTI16);
        detail::writeZigzag32(out_, v);
    }
    void i32Field(std::int16_t id, std::int32_t v) {
        field(id, kTI32);
        detail::writeZigzag32(out_, v);
    }
    void i64Field(std::int16_t id, std::int64_t v) {
        field(id, kTI64);
        detail::writeZigzag64(out_, v);
    }
    void binaryField(std::int16_t id, std::string_view v) {
        field(id, kTBinary);
        detail::writeVarint(out_, v.size());
        out_.insert(out_.end(), v.begin(), v.end());
    }
    void structField(std::int16_t id) { field(id, kTStruct); }
    void emptyStructField(std::int16_t id) {
        field(id, kTStruct);
        structBegin();
        structEnd();
    }
    void listHeader(std::int16_t id, std::uint8_t elemType, std::size_t size) {
        field(id, kTList);
        if (size <= 14) {
            out_.push_back(static_cast<std::uint8_t>((size << 4) | elemType));
        } else {
            out_.push_back(static_cast<std::uint8_t>(0xF0u | elemType));
            detail::writeVarint(out_, size);
        }
    }
    void listI32(std::int16_t id, const std::vector<std::int32_t>& values) {
        listHeader(id, kTI32, values.size());
        for (const auto v : values) {
            detail::writeZigzag32(out_, v);
        }
    }
    void listBinary(std::int16_t id, const std::vector<std::string>& values) {
        listHeader(id, kTBinary, values.size());
        for (const auto& v : values) {
            detail::writeVarint(out_, v.size());
            out_.insert(out_.end(), v.begin(), v.end());
        }
    }

private:
    std::vector<std::uint8_t>& out_;
    std::vector<std::int16_t> stack_;
    std::int16_t last_ = 0;
};

// SchemaElement, with both the legacy ConvertedType and the LogicalType union
// so readers of either vintage agree on what the column means.
void writeSchemaElement(Thrift& t, const ColumnSpec& spec) {
    t.structBegin();
    t.i32Field(1, physicalTypeOf(spec.type));
    t.i32Field(3, spec.optional ? kRepOptional : kRepRequired);
    t.binaryField(4, spec.name);
    switch (spec.logical) {
        case Logical::Utf8:
            t.i32Field(6, kConvUtf8);
            break;
        case Logical::Uint32:
            t.i32Field(6, kConvUint32);
            break;
        case Logical::TimestampMillisUtc:
            t.i32Field(6, kConvTimestampMillis);
            break;
        case Logical::None:
            break;
    }
    if (spec.logical != Logical::None) {
        t.structField(10);
        t.structBegin();
        switch (spec.logical) {
            case Logical::Utf8:
                t.emptyStructField(1);  // LogicalType.STRING = StringType {}
                break;
            case Logical::Uint32:
                t.structField(10);  // LogicalType.INTEGER = IntType
                t.structBegin();
                t.byteField(1, 32);      // bitWidth
                t.boolField(2, false);   // isSigned
                t.structEnd();
                break;
            case Logical::TimestampMillisUtc:
                t.structField(8);  // LogicalType.TIMESTAMP = TimestampType
                t.structBegin();
                t.boolField(1, true);  // isAdjustedToUTC
                t.structField(2);      // TimeUnit union
                t.structBegin();
                t.emptyStructField(1);  // MILLIS = MilliSeconds {}
                t.structEnd();
                t.structEnd();
                break;
            case Logical::None:
                break;
        }
        t.structEnd();
    }
    t.structEnd();
}

std::string statBytes(Type type, std::int64_t i, double d, const std::string& s) {
    std::vector<std::uint8_t> buf;
    switch (type) {
        case Type::Boolean:
            buf.push_back(i != 0 ? 1u : 0u);
            break;
        case Type::Int32:
            appendU32Le(buf, static_cast<std::uint32_t>(static_cast<std::int32_t>(i)));
            break;
        case Type::Int64:
            appendU64Le(buf, static_cast<std::uint64_t>(i));
            break;
        case Type::Double:
            appendU64Le(buf, std::bit_cast<std::uint64_t>(d));
            break;
        case Type::ByteArray:
            return s;
    }
    return std::string(reinterpret_cast<const char*>(buf.data()), buf.size());
}

}  // namespace

namespace detail {

void writeVarint(std::vector<std::uint8_t>& out, std::uint64_t v) {
    while (v >= 0x80u) {
        out.push_back(static_cast<std::uint8_t>(v | 0x80u));
        v >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

void writeZigzag32(std::vector<std::uint8_t>& out, std::int32_t v) {
    const auto u = static_cast<std::uint32_t>(v);
    writeVarint(out, static_cast<std::uint64_t>((u << 1) ^ static_cast<std::uint32_t>(v >> 31)));
}

void writeZigzag64(std::vector<std::uint8_t>& out, std::int64_t v) {
    const auto u = static_cast<std::uint64_t>(v);
    writeVarint(out, (u << 1) ^ static_cast<std::uint64_t>(v >> 63));
}

void writeFieldHeader(std::vector<std::uint8_t>& out, std::int16_t id, std::int16_t& lastId,
                      std::uint8_t compactType) {
    const int delta = static_cast<int>(id) - static_cast<int>(lastId);
    if (delta > 0 && delta <= 15) {
        out.push_back(static_cast<std::uint8_t>((static_cast<unsigned>(delta) << 4) | compactType));
    } else {
        out.push_back(compactType);
        writeZigzag32(out, id);
    }
    lastId = id;
}

void writeDefLevels(std::vector<std::uint8_t>& out, const std::vector<std::uint8_t>& levels) {
    // RLE/bit-packing hybrid at bit width 1. Definition levels for a flat
    // optional column are long runs of 0 or 1, so pure RLE runs are both
    // smaller and simpler than bit-packed runs; the reader accepts either.
    std::size_t i = 0;
    while (i < levels.size()) {
        const std::uint8_t v = levels[i];
        std::size_t j = i;
        while (j < levels.size() && levels[j] == v) {
            ++j;
        }
        writeVarint(out, static_cast<std::uint64_t>(j - i) << 1);  // low bit 0 == RLE run
        out.push_back(v);                                          // ceil(1 bit / 8) == 1 byte
        i = j;
    }
}

void writePlainBooleans(std::vector<std::uint8_t>& out, const std::vector<std::uint8_t>& values) {
    const std::size_t base = out.size();
    out.resize(base + (values.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (values[i] != 0) {
            out[base + i / 8] |= static_cast<std::uint8_t>(1u << (i % 8));
        }
    }
}

}  // namespace detail

Writer::~Writer() {
    if (open_ && !closed_) {
        std::string ignored;
        (void)close(&ignored);
    }
}

bool Writer::fail(std::string message, std::string* error) {
    if (!failed_) {
        failed_ = true;
        error_ = std::move(message);
    }
    if (error != nullptr) {
        *error = error_;
    }
    return false;
}

bool Writer::open(const std::filesystem::path& path, std::vector<ColumnSpec> columns, WriterOptions opts,
                  std::string* error) {
    if (columns.empty()) {
        return fail("parquet: schema has no columns", error);
    }
    opts_ = opts;
    if (opts_.rowGroupRows == 0) {
        opts_.rowGroupRows = 1;
    }
    if (opts_.pageRows == 0) {
        opts_.pageRows = 1;
    }
    path_ = path;
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    out_.open(path, std::ios::binary | std::ios::trunc);
    if (!out_) {
        return fail("parquet: cannot open " + path.string(), error);
    }
    cols_.clear();
    cols_.reserve(columns.size());
    for (auto& spec : columns) {
        Column c;
        c.spec = std::move(spec);
        cols_.push_back(std::move(c));
    }
    open_ = true;
    closed_ = false;
    failed_ = false;
    totalRows_ = 0;
    groupRows_ = 0;
    bytesWritten_ = 0;
    rowGroupCount_ = 0;
    groups_.clear();
    kv_.clear();
    static const char kMagic[4] = {'P', 'A', 'R', '1'};
    return writeBytes(kMagic, 4, error);
}

void Writer::setMetadata(std::string key, std::string value) { kv_.emplace_back(std::move(key), std::move(value)); }

bool Writer::writeBytes(const void* data, std::size_t n, std::string* error) {
    if (failed_) {
        if (error != nullptr) {
            *error = error_;
        }
        return false;
    }
    out_.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
    if (!out_) {
        return fail("parquet: write failed on " + path_.string(), error);
    }
    bytesWritten_ += n;
    return true;
}

void Writer::setNull(std::size_t col) {
    if (failed_ || col >= cols_.size()) {
        (void)fail("parquet: setNull out of range");
        return;
    }
    Column& c = cols_[col];
    if (!c.spec.optional) {
        (void)fail("parquet: null written to required column " + c.spec.name);
        return;
    }
    c.defLevels.push_back(0);
    ++c.rowCount;
    ++c.stats.nullCount;
}

void Writer::setBool(std::size_t col, bool v) {
    if (failed_ || col >= cols_.size() || cols_[col].spec.type != Type::Boolean) {
        (void)fail("parquet: setBool type mismatch");
        return;
    }
    Column& c = cols_[col];
    if (c.spec.optional) {
        c.defLevels.push_back(1);
    }
    c.boolv.push_back(v ? 1u : 0u);
    ++c.rowCount;
    const std::int64_t iv = v ? 1 : 0;
    if (!c.stats.any) {
        c.stats.any = true;
        c.stats.minI = iv;
        c.stats.maxI = iv;
    } else {
        c.stats.minI = std::min(c.stats.minI, iv);
        c.stats.maxI = std::max(c.stats.maxI, iv);
    }
}

void Writer::setInt32(std::size_t col, std::int32_t v) {
    if (failed_ || col >= cols_.size() || cols_[col].spec.type != Type::Int32) {
        (void)fail("parquet: setInt32 type mismatch");
        return;
    }
    Column& c = cols_[col];
    if (c.spec.optional) {
        c.defLevels.push_back(1);
    }
    c.i32.push_back(v);
    ++c.rowCount;
    // UINT_32 columns sort unsigned; using signed comparison here would publish
    // statistics that make a reader skip row groups that do contain the value.
    const bool unsignedCmp = c.spec.logical == Logical::Uint32;
    if (!c.stats.any) {
        c.stats.any = true;
        c.stats.minI = v;
        c.stats.maxI = v;
    } else if (unsignedCmp) {
        const auto uv = static_cast<std::uint32_t>(v);
        if (uv < static_cast<std::uint32_t>(static_cast<std::int32_t>(c.stats.minI))) {
            c.stats.minI = v;
        }
        if (uv > static_cast<std::uint32_t>(static_cast<std::int32_t>(c.stats.maxI))) {
            c.stats.maxI = v;
        }
    } else {
        c.stats.minI = std::min<std::int64_t>(c.stats.minI, v);
        c.stats.maxI = std::max<std::int64_t>(c.stats.maxI, v);
    }
}

void Writer::setInt64(std::size_t col, std::int64_t v) {
    if (failed_ || col >= cols_.size() || cols_[col].spec.type != Type::Int64) {
        (void)fail("parquet: setInt64 type mismatch");
        return;
    }
    Column& c = cols_[col];
    if (c.spec.optional) {
        c.defLevels.push_back(1);
    }
    c.i64.push_back(v);
    ++c.rowCount;
    if (!c.stats.any) {
        c.stats.any = true;
        c.stats.minI = v;
        c.stats.maxI = v;
    } else {
        c.stats.minI = std::min(c.stats.minI, v);
        c.stats.maxI = std::max(c.stats.maxI, v);
    }
}

void Writer::setDouble(std::size_t col, double v) {
    if (failed_ || col >= cols_.size() || cols_[col].spec.type != Type::Double) {
        (void)fail("parquet: setDouble type mismatch");
        return;
    }
    Column& c = cols_[col];
    if (c.spec.optional) {
        c.defLevels.push_back(1);
    }
    c.f64.push_back(v);
    ++c.rowCount;
    // NaN has no ordering, so it must not participate in min/max. A reader that
    // saw NaN as a bound would prune row groups arbitrarily.
    if (!std::isnan(v)) {
        if (!c.stats.any) {
            c.stats.any = true;
            c.stats.minD = v;
            c.stats.maxD = v;
        } else {
            c.stats.minD = std::min(c.stats.minD, v);
            c.stats.maxD = std::max(c.stats.maxD, v);
        }
    }
}

void Writer::setString(std::size_t col, std::string_view v) {
    if (failed_ || col >= cols_.size() || cols_[col].spec.type != Type::ByteArray) {
        (void)fail("parquet: setString type mismatch");
        return;
    }
    Column& c = cols_[col];
    if (c.spec.optional) {
        c.defLevels.push_back(1);
    }
    c.str.emplace_back(v);
    ++c.rowCount;
    if (!c.stats.any) {
        c.stats.any = true;
        c.stats.minS = c.str.back();
        c.stats.maxS = c.str.back();
    } else {
        if (c.str.back() < c.stats.minS) {
            c.stats.minS = c.str.back();
        }
        if (c.str.back() > c.stats.maxS) {
            c.stats.maxS = c.str.back();
        }
    }
}

bool Writer::endRow(std::string* error) {
    if (failed_) {
        if (error != nullptr) {
            *error = error_;
        }
        return false;
    }
    const std::uint64_t want = groupRows_ + 1;
    for (const auto& c : cols_) {
        if (c.rowCount != want) {
            return fail("parquet: column " + c.spec.name + " has " + std::to_string(c.rowCount) +
                            " entries but row " + std::to_string(want) + " was ended",
                        error);
        }
    }
    groupRows_ = want;
    ++totalRows_;
    if (groupRows_ >= opts_.rowGroupRows) {
        return flushRowGroup(error);
    }
    return true;
}

void Writer::serializeColumn(Column& col, std::vector<std::uint8_t>& out, ChunkMeta& meta) const {
    const std::size_t n = static_cast<std::size_t>(col.rowCount);
    meta.numValues = static_cast<std::int64_t>(n);
    meta.stats = col.stats;
    std::size_t valueCursor = 0;
    std::size_t row = 0;
    std::vector<std::uint8_t> body;
    std::vector<std::uint8_t> header;
    std::vector<std::uint8_t> levelRun;
    while (row < n) {
        const std::size_t rows = std::min<std::size_t>(opts_.pageRows, n - row);
        body.clear();
        std::size_t present = rows;
        if (col.spec.optional) {
            levelRun.clear();
            std::vector<std::uint8_t> slice(col.defLevels.begin() + static_cast<std::ptrdiff_t>(row),
                                            col.defLevels.begin() + static_cast<std::ptrdiff_t>(row + rows));
            present = static_cast<std::size_t>(std::count(slice.begin(), slice.end(), std::uint8_t{1}));
            detail::writeDefLevels(levelRun, slice);
            appendU32Le(body, static_cast<std::uint32_t>(levelRun.size()));
            body.insert(body.end(), levelRun.begin(), levelRun.end());
        }
        switch (col.spec.type) {
            case Type::Boolean: {
                std::vector<std::uint8_t> slice(col.boolv.begin() + static_cast<std::ptrdiff_t>(valueCursor),
                                                col.boolv.begin() +
                                                    static_cast<std::ptrdiff_t>(valueCursor + present));
                detail::writePlainBooleans(body, slice);
                break;
            }
            case Type::Int32:
                for (std::size_t k = 0; k < present; ++k) {
                    appendU32Le(body, static_cast<std::uint32_t>(col.i32[valueCursor + k]));
                }
                break;
            case Type::Int64:
                for (std::size_t k = 0; k < present; ++k) {
                    appendU64Le(body, static_cast<std::uint64_t>(col.i64[valueCursor + k]));
                }
                break;
            case Type::Double:
                for (std::size_t k = 0; k < present; ++k) {
                    appendU64Le(body, std::bit_cast<std::uint64_t>(col.f64[valueCursor + k]));
                }
                break;
            case Type::ByteArray:
                for (std::size_t k = 0; k < present; ++k) {
                    const auto& s = col.str[valueCursor + k];
                    appendU32Le(body, static_cast<std::uint32_t>(s.size()));
                    body.insert(body.end(), s.begin(), s.end());
                }
                break;
        }
        valueCursor += present;

        header.clear();
        Thrift t(header);
        t.structBegin();
        t.i32Field(1, kPageTypeData);
        t.i32Field(2, static_cast<std::int32_t>(body.size()));
        t.i32Field(3, static_cast<std::int32_t>(body.size()));
        t.structField(5);  // data_page_header
        t.structBegin();
        t.i32Field(1, static_cast<std::int32_t>(rows));
        t.i32Field(2, kEncPlain);
        t.i32Field(3, kEncRle);  // definition_level_encoding
        t.i32Field(4, kEncRle);  // repetition_level_encoding
        t.structEnd();
        t.structEnd();

        out.insert(out.end(), header.begin(), header.end());
        out.insert(out.end(), body.begin(), body.end());
        row += rows;
    }
}

void Writer::resetColumns() {
    for (auto& c : cols_) {
        c.i32.clear();
        c.i64.clear();
        c.f64.clear();
        c.boolv.clear();
        c.str.clear();
        c.defLevels.clear();
        c.rowCount = 0;
        c.stats = Stats{};
    }
    groupRows_ = 0;
}

bool Writer::flushRowGroup(std::string* error) {
    if (failed_) {
        if (error != nullptr) {
            *error = error_;
        }
        return false;
    }
    if (groupRows_ == 0) {
        return true;
    }
    RowGroupMeta group;
    group.numRows = static_cast<std::int64_t>(groupRows_);
    group.chunks.reserve(cols_.size());
    std::vector<std::uint8_t> chunkBytes;
    for (auto& c : cols_) {
        ChunkMeta meta;
        meta.dataPageOffset = static_cast<std::int64_t>(bytesWritten_);
        chunkBytes.clear();
        serializeColumn(c, chunkBytes, meta);
        meta.totalBytes = static_cast<std::int64_t>(chunkBytes.size());
        if (!writeBytes(chunkBytes.data(), chunkBytes.size(), error)) {
            return false;
        }
        group.totalBytes += meta.totalBytes;
        group.chunks.push_back(std::move(meta));
    }
    groups_.push_back(std::move(group));
    ++rowGroupCount_;
    resetColumns();
    return true;
}

bool Writer::close(std::string* error) {
    if (closed_) {
        return !failed_;
    }
    if (!open_) {
        return fail("parquet: close before open", error);
    }
    if (failed_) {
        closed_ = true;
        out_.close();
        if (error != nullptr) {
            *error = error_;
        }
        return false;
    }
    if (!flushRowGroup(error)) {
        closed_ = true;
        out_.close();
        return false;
    }

    std::vector<std::uint8_t> meta;
    Thrift t(meta);
    t.structBegin();
    t.i32Field(1, 1);  // version

    // schema: root group element followed by one element per leaf column.
    t.listHeader(2, kTStruct, cols_.size() + 1);
    {
        t.structBegin();
        t.i32Field(3, kRepRequired);
        t.binaryField(4, "photon_export");
        t.i32Field(5, static_cast<std::int32_t>(cols_.size()));
        t.structEnd();
    }
    for (const auto& c : cols_) {
        writeSchemaElement(t, c.spec);
    }

    t.i64Field(3, static_cast<std::int64_t>(totalRows_));

    t.listHeader(4, kTStruct, groups_.size());
    for (const auto& g : groups_) {
        t.structBegin();
        t.listHeader(1, kTStruct, g.chunks.size());
        for (std::size_t ci = 0; ci < g.chunks.size(); ++ci) {
            const auto& chunk = g.chunks[ci];
            const auto& spec = cols_[ci].spec;
            t.structBegin();
            t.i64Field(2, chunk.dataPageOffset);  // file_offset
            t.structField(3);                     // meta_data
            t.structBegin();
            t.i32Field(1, physicalTypeOf(spec.type));
            if (spec.optional) {
                t.listI32(2, {kEncPlain, kEncRle});
            } else {
                t.listI32(2, {kEncPlain});
            }
            t.listBinary(3, {spec.name});
            t.i32Field(4, kCodecUncompressed);
            t.i64Field(5, chunk.numValues);
            t.i64Field(6, chunk.totalBytes);
            t.i64Field(7, chunk.totalBytes);
            t.i64Field(9, chunk.dataPageOffset);
            if (opts_.statistics) {
                t.structField(12);
                t.structBegin();
                const auto maxBytes =
                    chunk.stats.any
                        ? statBytes(spec.type, chunk.stats.maxI, chunk.stats.maxD, chunk.stats.maxS)
                        : std::string{};
                const auto minBytes =
                    chunk.stats.any
                        ? statBytes(spec.type, chunk.stats.minI, chunk.stats.minD, chunk.stats.minS)
                        : std::string{};
                // Fields 1/2 are the deprecated min/max and 5/6 the current
                // min_value/max_value. Arrow's reader only trusts statistics
                // from a writer it recognises unless the legacy pair is present
                // too, so both are written, exactly as parquet-cpp does.
                if (chunk.stats.any) {
                    t.binaryField(1, maxBytes);
                    t.binaryField(2, minBytes);
                }
                t.i64Field(3, chunk.stats.nullCount);
                if (chunk.stats.any) {
                    t.binaryField(5, maxBytes);
                    t.binaryField(6, minBytes);
                }
                t.structEnd();
            }
            t.structEnd();
            t.structEnd();
        }
        t.i64Field(2, g.totalBytes);
        t.i64Field(3, g.numRows);
        t.i64Field(5, g.chunks.empty() ? 0 : g.chunks.front().dataPageOffset);
        t.i64Field(6, g.totalBytes);
        t.structEnd();
    }

    if (!kv_.empty()) {
        t.listHeader(5, kTStruct, kv_.size());
        for (const auto& [k, v] : kv_) {
            t.structBegin();
            t.binaryField(1, k);
            t.binaryField(2, v);
            t.structEnd();
        }
    }
    t.binaryField(6, "photon-export 0.1 (minimal parquet v1 writer)");
    t.structEnd();

    const auto metaLen = static_cast<std::uint32_t>(meta.size());
    if (!writeBytes(meta.data(), meta.size(), error)) {
        closed_ = true;
        out_.close();
        return false;
    }
    std::vector<std::uint8_t> tail;
    appendU32Le(tail, metaLen);
    tail.push_back('P');
    tail.push_back('A');
    tail.push_back('R');
    tail.push_back('1');
    if (!writeBytes(tail.data(), tail.size(), error)) {
        closed_ = true;
        out_.close();
        return false;
    }
    out_.flush();
    const bool ok = static_cast<bool>(out_);
    out_.close();
    closed_ = true;
    if (!ok) {
        return fail("parquet: flush failed on " + path_.string(), error);
    }
    return true;
}

}  // namespace photon::pq
