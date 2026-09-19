// Tests for the photon-export Parquet writer and the store->row exporter.
//
// The primitives are checked against hand-computed byte sequences, and the file
// structure is checked with a Thrift-compact reader written independently of the
// writer in this same file. Neither substitutes for reading the output back with
// pyarrow, which is the only check that proves a real reader accepts it.
#include "export.hpp"
#include "parquet.hpp"
#include "test_harness.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace photon;

namespace {

bool bytesAre(const std::vector<std::uint8_t>& got, std::initializer_list<int> want) {
    if (got.size() != want.size()) {
        return false;
    }
    std::size_t i = 0;
    for (const auto v : want) {
        if (got[i++] != static_cast<std::uint8_t>(v)) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Thrift compact protocol reader, only as much as the parquet footer needs.
// ---------------------------------------------------------------------------

struct TReader {
    std::span<const std::uint8_t> b;
    std::size_t off = 0;
    std::vector<std::int16_t> stack;
    std::int16_t last = 0;
    bool bad = false;

    bool varint(std::uint64_t& v) {
        v = 0;
        int shift = 0;
        while (off < b.size()) {
            const std::uint8_t byte = b[off++];
            v |= static_cast<std::uint64_t>(byte & 0x7fu) << shift;
            if ((byte & 0x80u) == 0) {
                return true;
            }
            shift += 7;
            if (shift > 63) {
                break;
            }
        }
        bad = true;
        return false;
    }
    bool zigzag(std::int64_t& v) {
        std::uint64_t u = 0;
        if (!varint(u)) {
            return false;
        }
        v = static_cast<std::int64_t>((u >> 1) ^ static_cast<std::uint64_t>(-static_cast<std::int64_t>(u & 1u)));
        return true;
    }
    bool binary(std::string& s) {
        std::uint64_t n = 0;
        if (!varint(n) || off + n > b.size()) {
            bad = true;
            return false;
        }
        s.assign(reinterpret_cast<const char*>(b.data() + off), static_cast<std::size_t>(n));
        off += static_cast<std::size_t>(n);
        return true;
    }
    void structBegin() {
        stack.push_back(last);
        last = 0;
    }
    void structEnd() {
        if (!stack.empty()) {
            last = stack.back();
            stack.pop_back();
        }
    }
    // false at STOP (or on error).
    bool field(std::int16_t& id, std::uint8_t& type) {
        if (off >= b.size()) {
            bad = true;
            return false;
        }
        const std::uint8_t h = b[off++];
        if (h == 0) {
            return false;
        }
        type = h & 0x0fu;
        const std::uint8_t delta = h >> 4;
        if (delta == 0) {
            std::int64_t raw = 0;
            if (!zigzag(raw)) {
                return false;
            }
            id = static_cast<std::int16_t>(raw);
        } else {
            id = static_cast<std::int16_t>(last + delta);
        }
        last = id;
        return true;
    }
    bool listHeader(std::uint8_t& elemType, std::uint64_t& size) {
        if (off >= b.size()) {
            bad = true;
            return false;
        }
        const std::uint8_t h = b[off++];
        elemType = h & 0x0fu;
        const std::uint8_t s = h >> 4;
        if (s == 0x0f) {
            return varint(size);
        }
        size = s;
        return true;
    }
    bool skip(std::uint8_t type) {
        switch (type) {
            case 1:
            case 2:
                return true;
            case 3:
                if (off >= b.size()) {
                    bad = true;
                    return false;
                }
                ++off;
                return true;
            case 4:
            case 5:
            case 6: {
                std::int64_t v = 0;
                return zigzag(v);
            }
            case 7:
                if (off + 8 > b.size()) {
                    bad = true;
                    return false;
                }
                off += 8;
                return true;
            case 8: {
                std::string s;
                return binary(s);
            }
            case 9:
            case 10: {
                std::uint8_t et = 0;
                std::uint64_t n = 0;
                if (!listHeader(et, n)) {
                    return false;
                }
                for (std::uint64_t i = 0; i < n; ++i) {
                    if (!skip(et)) {
                        return false;
                    }
                }
                return true;
            }
            case 12: {
                structBegin();
                std::int16_t id = 0;
                std::uint8_t t = 0;
                while (field(id, t)) {
                    if (!skip(t)) {
                        return false;
                    }
                }
                structEnd();
                return !bad;
            }
            default:
                bad = true;
                return false;
        }
    }
};

struct RSchema {
    std::string name;
    int type = -1;
    int rep = -1;
    int converted = -1;
    int numChildren = -1;
    bool hasLogical = false;
};

struct RStats {
    bool present = false;
    bool hasBounds = false;
    std::int64_t nullCount = 0;
    std::string minValue;
    std::string maxValue;
};

struct RChunk {
    int type = -1;
    std::string path;
    std::int64_t numValues = 0;
    std::int64_t totalUncompressed = 0;
    std::int64_t totalCompressed = 0;
    std::int64_t dataPageOffset = 0;
    std::vector<std::int32_t> encodings;
    RStats stats;
};

struct RGroup {
    std::vector<RChunk> chunks;
    std::int64_t numRows = 0;
    std::int64_t totalBytes = 0;
};

struct RFooter {
    bool ok = false;
    std::int64_t version = 0;
    std::int64_t numRows = 0;
    std::vector<RSchema> schema;
    std::vector<RGroup> groups;
    std::vector<std::pair<std::string, std::string>> kv;
    std::string createdBy;
};

RStats readStats(TReader& r) {
    RStats s;
    s.present = true;
    r.structBegin();
    std::int16_t id = 0;
    std::uint8_t t = 0;
    while (r.field(id, t)) {
        if (id == 3 && t == 6) {
            (void)r.zigzag(s.nullCount);
        } else if (id == 5 && t == 8) {
            (void)r.binary(s.maxValue);
            s.hasBounds = true;
        } else if (id == 6 && t == 8) {
            (void)r.binary(s.minValue);
            s.hasBounds = true;
        } else if (!r.skip(t)) {
            break;
        }
    }
    r.structEnd();
    return s;
}

RChunk readColumnChunk(TReader& r) {
    RChunk c;
    r.structBegin();
    std::int16_t id = 0;
    std::uint8_t t = 0;
    while (r.field(id, t)) {
        if (id == 3 && t == 12) {
            r.structBegin();
            std::int16_t mid = 0;
            std::uint8_t mt = 0;
            while (r.field(mid, mt)) {
                std::int64_t v = 0;
                if (mid == 1 && mt == 5) {
                    (void)r.zigzag(v);
                    c.type = static_cast<int>(v);
                } else if (mid == 2 && mt == 9) {
                    std::uint8_t et = 0;
                    std::uint64_t n = 0;
                    (void)r.listHeader(et, n);
                    for (std::uint64_t i = 0; i < n; ++i) {
                        std::int64_t e = 0;
                        (void)r.zigzag(e);
                        c.encodings.push_back(static_cast<std::int32_t>(e));
                    }
                } else if (mid == 3 && mt == 9) {
                    std::uint8_t et = 0;
                    std::uint64_t n = 0;
                    (void)r.listHeader(et, n);
                    for (std::uint64_t i = 0; i < n; ++i) {
                        std::string s;
                        (void)r.binary(s);
                        c.path = c.path.empty() ? s : c.path + "." + s;
                    }
                } else if (mid == 5 && mt == 6) {
                    (void)r.zigzag(c.numValues);
                } else if (mid == 6 && mt == 6) {
                    (void)r.zigzag(c.totalUncompressed);
                } else if (mid == 7 && mt == 6) {
                    (void)r.zigzag(c.totalCompressed);
                } else if (mid == 9 && mt == 6) {
                    (void)r.zigzag(c.dataPageOffset);
                } else if (mid == 12 && mt == 12) {
                    c.stats = readStats(r);
                } else if (!r.skip(mt)) {
                    break;
                }
            }
            r.structEnd();
        } else if (!r.skip(t)) {
            break;
        }
    }
    r.structEnd();
    return c;
}

RFooter parseFooter(const std::vector<std::uint8_t>& file) {
    RFooter f;
    if (file.size() < 12 || std::string(reinterpret_cast<const char*>(file.data()), 4) != "PAR1" ||
        std::string(reinterpret_cast<const char*>(file.data() + file.size() - 4), 4) != "PAR1") {
        return f;
    }
    std::uint32_t metaLen = 0;
    for (int i = 0; i < 4; ++i) {
        metaLen |= static_cast<std::uint32_t>(file[file.size() - 8 + static_cast<std::size_t>(i)])
                   << (8 * i);
    }
    if (static_cast<std::size_t>(metaLen) + 12 > file.size()) {
        return f;
    }
    TReader r;
    r.b = std::span<const std::uint8_t>(file.data() + file.size() - 8 - metaLen, metaLen);
    r.structBegin();
    std::int16_t id = 0;
    std::uint8_t t = 0;
    while (r.field(id, t)) {
        if (id == 1 && t == 5) {
            (void)r.zigzag(f.version);
        } else if (id == 2 && t == 9) {
            std::uint8_t et = 0;
            std::uint64_t n = 0;
            (void)r.listHeader(et, n);
            for (std::uint64_t i = 0; i < n; ++i) {
                RSchema s;
                r.structBegin();
                std::int16_t sid = 0;
                std::uint8_t st = 0;
                while (r.field(sid, st)) {
                    std::int64_t v = 0;
                    if (sid == 1 && st == 5) {
                        (void)r.zigzag(v);
                        s.type = static_cast<int>(v);
                    } else if (sid == 3 && st == 5) {
                        (void)r.zigzag(v);
                        s.rep = static_cast<int>(v);
                    } else if (sid == 4 && st == 8) {
                        (void)r.binary(s.name);
                    } else if (sid == 5 && st == 5) {
                        (void)r.zigzag(v);
                        s.numChildren = static_cast<int>(v);
                    } else if (sid == 6 && st == 5) {
                        (void)r.zigzag(v);
                        s.converted = static_cast<int>(v);
                    } else if (sid == 10 && st == 12) {
                        s.hasLogical = true;
                        if (!r.skip(st)) {
                            break;
                        }
                    } else if (!r.skip(st)) {
                        break;
                    }
                }
                r.structEnd();
                f.schema.push_back(std::move(s));
            }
        } else if (id == 3 && t == 6) {
            (void)r.zigzag(f.numRows);
        } else if (id == 4 && t == 9) {
            std::uint8_t et = 0;
            std::uint64_t n = 0;
            (void)r.listHeader(et, n);
            for (std::uint64_t i = 0; i < n; ++i) {
                RGroup g;
                r.structBegin();
                std::int16_t gid = 0;
                std::uint8_t gt = 0;
                while (r.field(gid, gt)) {
                    if (gid == 1 && gt == 9) {
                        std::uint8_t cet = 0;
                        std::uint64_t cn = 0;
                        (void)r.listHeader(cet, cn);
                        for (std::uint64_t c = 0; c < cn; ++c) {
                            g.chunks.push_back(readColumnChunk(r));
                        }
                    } else if (gid == 2 && gt == 6) {
                        (void)r.zigzag(g.totalBytes);
                    } else if (gid == 3 && gt == 6) {
                        (void)r.zigzag(g.numRows);
                    } else if (!r.skip(gt)) {
                        break;
                    }
                }
                r.structEnd();
                f.groups.push_back(std::move(g));
            }
        } else if (id == 5 && t == 9) {
            std::uint8_t et = 0;
            std::uint64_t n = 0;
            (void)r.listHeader(et, n);
            for (std::uint64_t i = 0; i < n; ++i) {
                std::string k;
                std::string v;
                r.structBegin();
                std::int16_t kid = 0;
                std::uint8_t kt = 0;
                while (r.field(kid, kt)) {
                    if (kid == 1 && kt == 8) {
                        (void)r.binary(k);
                    } else if (kid == 2 && kt == 8) {
                        (void)r.binary(v);
                    } else if (!r.skip(kt)) {
                        break;
                    }
                }
                r.structEnd();
                f.kv.emplace_back(std::move(k), std::move(v));
            }
        } else if (id == 6 && t == 8) {
            (void)r.binary(f.createdBy);
        } else if (!r.skip(t)) {
            break;
        }
    }
    r.structEnd();
    f.ok = !r.bad;
    return f;
}

struct RPage {
    bool ok = false;
    int type = -1;
    std::int64_t uncompressed = 0;
    std::int64_t compressed = 0;
    std::int64_t numValues = 0;
    std::int64_t encoding = -1;
    std::int64_t defEncoding = -1;
    std::size_t headerBytes = 0;
};

RPage parsePageHeader(const std::vector<std::uint8_t>& file, std::size_t at) {
    RPage p;
    TReader r;
    r.b = std::span<const std::uint8_t>(file.data() + at, file.size() - at);
    r.structBegin();
    std::int16_t id = 0;
    std::uint8_t t = 0;
    while (r.field(id, t)) {
        std::int64_t v = 0;
        if (id == 1 && t == 5) {
            (void)r.zigzag(v);
            p.type = static_cast<int>(v);
        } else if (id == 2 && t == 5) {
            (void)r.zigzag(p.uncompressed);
        } else if (id == 3 && t == 5) {
            (void)r.zigzag(p.compressed);
        } else if (id == 5 && t == 12) {
            r.structBegin();
            std::int16_t did = 0;
            std::uint8_t dt = 0;
            while (r.field(did, dt)) {
                if (did == 1 && dt == 5) {
                    (void)r.zigzag(p.numValues);
                } else if (did == 2 && dt == 5) {
                    (void)r.zigzag(p.encoding);
                } else if (did == 3 && dt == 5) {
                    (void)r.zigzag(p.defEncoding);
                } else if (!r.skip(dt)) {
                    break;
                }
            }
            r.structEnd();
        } else if (!r.skip(t)) {
            break;
        }
    }
    r.structEnd();
    p.headerBytes = r.off;
    p.ok = !r.bad;
    return p;
}

std::vector<std::uint8_t> readWhole(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::int64_t leInt(const std::string& s) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < s.size() && i < 8; ++i) {
        v |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(s[i])) << (8 * i);
    }
    if (s.size() == 4) {
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(v));
    }
    return static_cast<std::int64_t>(v);
}

double leDouble(const std::string& s) {
    if (s.size() != 8) {
        return 0.0;
    }
    return std::bit_cast<double>(static_cast<std::uint64_t>(leInt(s)));
}

// ---------------------------------------------------------------------------
// Primitive encoders
// ---------------------------------------------------------------------------

void testVarint() {
    std::vector<std::uint8_t> b;
    pq::detail::writeVarint(b, 0);
    CHECK(bytesAre(b, {0x00}));
    b.clear();
    pq::detail::writeVarint(b, 127);
    CHECK(bytesAre(b, {0x7f}));
    b.clear();
    pq::detail::writeVarint(b, 128);
    CHECK(bytesAre(b, {0x80, 0x01}));
    b.clear();
    pq::detail::writeVarint(b, 300);
    CHECK(bytesAre(b, {0xac, 0x02}));
    b.clear();
    pq::detail::writeVarint(b, 0xFFFFFFFFFFFFFFFFull);
    CHECK(bytesAre(b, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01}));
}

void testZigzag() {
    std::vector<std::uint8_t> b;
    pq::detail::writeZigzag32(b, 0);
    CHECK(bytesAre(b, {0x00}));
    b.clear();
    pq::detail::writeZigzag32(b, -1);
    CHECK(bytesAre(b, {0x01}));
    b.clear();
    pq::detail::writeZigzag32(b, 1);
    CHECK(bytesAre(b, {0x02}));
    b.clear();
    pq::detail::writeZigzag32(b, -2);
    CHECK(bytesAre(b, {0x03}));
    b.clear();
    pq::detail::writeZigzag32(b, 64);
    CHECK(bytesAre(b, {0x80, 0x01}));
    b.clear();
    pq::detail::writeZigzag64(b, -1);
    CHECK(bytesAre(b, {0x01}));
    b.clear();
    pq::detail::writeZigzag64(b, INT64_MIN);
    CHECK(bytesAre(b, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01}));
}

void testFieldHeader() {
    std::vector<std::uint8_t> b;
    std::int16_t last = 0;
    pq::detail::writeFieldHeader(b, 1, last, 5);
    CHECK(bytesAre(b, {0x15}));
    CHECK(last == 1);
    b.clear();
    pq::detail::writeFieldHeader(b, 3, last, 8);
    CHECK(bytesAre(b, {0x28}));
    b.clear();
    // Delta 17 exceeds the 4-bit field, so the long form carries a zigzag id.
    pq::detail::writeFieldHeader(b, 20, last, 12);
    CHECK(bytesAre(b, {0x0c, 0x28}));
    CHECK(last == 20);
}

void testDefLevels() {
    std::vector<std::uint8_t> b;
    pq::detail::writeDefLevels(b, {1, 1, 1});
    CHECK(bytesAre(b, {0x06, 0x01}));
    b.clear();
    pq::detail::writeDefLevels(b, {1, 0, 1});
    CHECK(bytesAre(b, {0x02, 0x01, 0x02, 0x00, 0x02, 0x01}));
    b.clear();
    pq::detail::writeDefLevels(b, {0, 0, 0, 0, 1, 1});
    CHECK(bytesAre(b, {0x08, 0x00, 0x04, 0x01}));
}

void testPlainBooleans() {
    std::vector<std::uint8_t> b;
    pq::detail::writePlainBooleans(b, {1, 0, 1, 1});
    CHECK(bytesAre(b, {0x0d}));
    b.clear();
    pq::detail::writePlainBooleans(b, {0, 0, 0, 0, 0, 0, 0, 0, 1});
    CHECK(bytesAre(b, {0x00, 0x01}));
    b.clear();
    pq::detail::writePlainBooleans(b, {});
    CHECK(b.empty());
}

// ---------------------------------------------------------------------------
// File structure
// ---------------------------------------------------------------------------

fs::path scratch() {
    const auto dir = fs::temp_directory_path() / "photon-export-tests";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

std::vector<pq::ColumnSpec> smallSchema() {
    return {
        pq::ColumnSpec{"id", pq::Type::Int32, pq::Logical::Uint32, false},
        pq::ColumnSpec{"t", pq::Type::Int64, pq::Logical::TimestampMillisUtc, false},
        pq::ColumnSpec{"v", pq::Type::Double, pq::Logical::None, true},
        pq::ColumnSpec{"tag", pq::Type::ByteArray, pq::Logical::Utf8, false},
        pq::ColumnSpec{"flag", pq::Type::Boolean, pq::Logical::None, false},
    };
}

void testFileStructure() {
    const auto path = scratch() / "structure.parquet";
    pq::Writer w;
    pq::WriterOptions opts;
    opts.rowGroupRows = 1000;
    opts.pageRows = 1000;
    std::string err;
    CHECK(w.open(path, smallSchema(), opts, &err));
    w.setMetadata("photon.test", "hello");
    // Row 2 has a null v; the rest are present.
    const std::int32_t ids[5] = {1, 7, -3, 42, 9};
    const std::int64_t ts[5] = {100, 200, 300, 400, 500};
    for (int i = 0; i < 5; ++i) {
        w.setInt32(0, ids[i]);
        w.setInt64(1, ts[i]);
        if (i == 2) {
            w.setNull(2);
        } else {
            w.setDouble(2, 0.5 * static_cast<double>(i));
        }
        w.setString(3, std::string("s") + std::to_string(i));
        w.setBool(4, (i % 2) == 0);
        CHECK(w.endRow(&err));
    }
    CHECK(w.close(&err));
    CHECK(!w.failed());
    CHECK(w.rows() == 5);
    CHECK(w.rowGroups() == 1);

    const auto file = readWhole(path);
    CHECK(file.size() == w.bytes());
    const auto f = parseFooter(file);
    CHECK(f.ok);
    CHECK(f.version == 1);
    CHECK(f.numRows == 5);
    // Root group element plus one leaf per column.
    CHECK(f.schema.size() == 6);
    // Everything below indexes into the parsed footer, so stop if it did not
    // parse. The checks above have already recorded the failure; continuing
    // would be undefined behaviour and would hide every later test.
    if (!f.ok || f.schema.size() != 6 || f.groups.size() != 1 || f.groups[0].chunks.size() != 5) {
        return;
    }
    CHECK(f.schema[0].numChildren == 5);
    CHECK(f.schema[0].type == -1);
    CHECK(f.schema[1].name == "id");
    CHECK(f.schema[1].type == 1);        // INT32
    CHECK(f.schema[1].rep == 0);         // REQUIRED
    CHECK(f.schema[1].converted == 13);  // UINT_32
    CHECK(f.schema[1].hasLogical);
    CHECK(f.schema[2].name == "t");
    CHECK(f.schema[2].type == 2);       // INT64
    CHECK(f.schema[2].converted == 9);  // TIMESTAMP_MILLIS
    CHECK(f.schema[3].name == "v");
    CHECK(f.schema[3].type == 5);  // DOUBLE
    CHECK(f.schema[3].rep == 1);   // OPTIONAL
    CHECK(f.schema[4].name == "tag");
    CHECK(f.schema[4].type == 6);       // BYTE_ARRAY
    CHECK(f.schema[4].converted == 0);  // UTF8
    CHECK(f.schema[5].name == "flag");
    CHECK(f.schema[5].type == 0);  // BOOLEAN

    CHECK(f.groups.size() == 1);
    CHECK(f.groups[0].numRows == 5);
    CHECK(f.groups[0].chunks.size() == 5);
    bool sawMeta = false;
    for (const auto& [k, v] : f.kv) {
        if (k == "photon.test") {
            sawMeta = v == "hello";
        }
    }
    CHECK(sawMeta);
    CHECK(f.createdBy.rfind("photon-export", 0) == 0);

    // Column chunks must tile the data region exactly: each chunk's declared
    // size has to equal the gap to the next chunk, and the last has to run up to
    // the footer. A wrong size or offset breaks this.
    const auto footerStart = file.size() - 8 - static_cast<std::size_t>(
                                                    [&] {
                                                        std::uint32_t n = 0;
                                                        for (int i = 0; i < 4; ++i) {
                                                            n |= static_cast<std::uint32_t>(
                                                                     file[file.size() - 8 +
                                                                          static_cast<std::size_t>(i)])
                                                                 << (8 * i);
                                                        }
                                                        return n;
                                                    }());
    CHECK(f.groups[0].chunks[0].dataPageOffset == 4);
    std::int64_t running = 4;
    std::int64_t declared = 0;
    for (const auto& c : f.groups[0].chunks) {
        CHECK(c.dataPageOffset == running);
        CHECK(c.numValues == 5);
        CHECK(c.totalUncompressed == c.totalCompressed);
        running += c.totalUncompressed;
        declared += c.totalUncompressed;
    }
    CHECK(running == static_cast<std::int64_t>(footerStart));
    CHECK(f.groups[0].totalBytes == declared);

    // Required columns advertise PLAIN only; optional ones also advertise RLE
    // for the definition levels.
    CHECK(f.groups[0].chunks[0].encodings.size() == 1);
    CHECK(f.groups[0].chunks[0].encodings[0] == 0);
    CHECK(f.groups[0].chunks[2].encodings.size() == 2);
    CHECK(f.groups[0].chunks[2].encodings[1] == 3);

    // The first page of a required INT32 column is 5 values with no level data.
    const auto page = parsePageHeader(file, static_cast<std::size_t>(f.groups[0].chunks[0].dataPageOffset));
    CHECK(page.ok);
    CHECK(page.type == 0);
    CHECK(page.numValues == 5);
    CHECK(page.encoding == 0);
    CHECK(page.uncompressed == 20);
    CHECK(page.compressed == 20);
    CHECK(static_cast<std::int64_t>(page.headerBytes) + page.uncompressed ==
          f.groups[0].chunks[0].totalUncompressed);

    // The optional DOUBLE column stores 4 of 5 values plus a 4-byte level-length
    // prefix and a 3-run RLE level stream (present, absent, present).
    const auto vPage = parsePageHeader(file, static_cast<std::size_t>(f.groups[0].chunks[2].dataPageOffset));
    CHECK(vPage.ok);
    CHECK(vPage.numValues == 5);
    CHECK(vPage.defEncoding == 3);
    CHECK(vPage.uncompressed == 4 + 6 + 4 * 8);
}

void testStatistics() {
    const auto path = scratch() / "stats.parquet";
    pq::Writer w;
    std::string err;
    CHECK(w.open(path, smallSchema(), pq::WriterOptions{}, &err));
    // 0xFFFFFFFF is the unsigned maximum but the most negative int32; if the
    // UINT_32 column compared signed, it would be reported as the minimum.
    const std::int32_t ids[3] = {1, static_cast<std::int32_t>(0xFFFFFFFFu), 5};
    const double vs[3] = {-2.5, 7.25, 0.0};
    for (int i = 0; i < 3; ++i) {
        w.setInt32(0, ids[i]);
        w.setInt64(1, 1000 + i);
        if (i == 1) {
            w.setNull(2);
        } else {
            w.setDouble(2, vs[i]);
        }
        w.setString(3, i == 0 ? "aaa" : "zzz");
        w.setBool(4, true);
        CHECK(w.endRow(&err));
    }
    CHECK(w.close(&err));
    const auto f = parseFooter(readWhole(path));
    CHECK(f.ok);
    CHECK(f.groups.size() == 1);
    if (!f.ok || f.groups.size() != 1 || f.groups[0].chunks.size() != 5) {
        return;
    }
    const auto& chunks = f.groups[0].chunks;
    CHECK(chunks[0].stats.present);
    CHECK(chunks[0].stats.hasBounds);
    CHECK(static_cast<std::uint32_t>(leInt(chunks[0].stats.minValue)) == 1u);
    CHECK(static_cast<std::uint32_t>(leInt(chunks[0].stats.maxValue)) == 0xFFFFFFFFu);
    CHECK(chunks[0].stats.nullCount == 0);
    CHECK(leInt(chunks[1].stats.minValue) == 1000);
    CHECK(leInt(chunks[1].stats.maxValue) == 1002);
    CHECK(chunks[2].stats.nullCount == 1);
    CHECK(near(leDouble(chunks[2].stats.minValue), -2.5));
    CHECK(near(leDouble(chunks[2].stats.maxValue), 0.0));
    CHECK(chunks[3].stats.minValue == "aaa");
    CHECK(chunks[3].stats.maxValue == "zzz");
}

void testNanIsNotABound() {
    // NaN has no ordering, so it must never become a bound: a reader that saw it
    // as one would prune row groups arbitrarily. NaN has to arrive first for this
    // to bite, because a later NaN loses every comparison anyway.
    const auto path = scratch() / "nan.parquet";
    std::vector<pq::ColumnSpec> schema;
    schema.push_back({"v", pq::Type::Double, pq::Logical::None, true});
    pq::Writer w;
    std::string err;
    CHECK(w.open(path, schema, pq::WriterOptions{}, &err));
    const double vals[3] = {std::numeric_limits<double>::quiet_NaN(), 1.5, -0.5};
    for (const double v : vals) {
        w.setDouble(0, v);
        CHECK(w.endRow(&err));
    }
    CHECK(w.close(&err));
    const auto f = parseFooter(readWhole(path));
    CHECK(f.ok);
    CHECK(f.groups.size() == 1);
    if (!f.ok || f.groups.size() != 1 || f.groups[0].chunks.empty()) {
        return;
    }
    const auto& st = f.groups[0].chunks[0].stats;
    CHECK(st.present);
    CHECK(st.hasBounds);
    const double lo = leDouble(st.minValue);
    const double hi = leDouble(st.maxValue);
    CHECK(!std::isnan(lo));
    CHECK(!std::isnan(hi));
    CHECK(near(lo, -0.5));
    CHECK(near(hi, 1.5));
    CHECK(st.nullCount == 0);  // NaN is a value, not a null
}

void testRowGroupsAndPages() {
    const auto path = scratch() / "groups.parquet";
    pq::Writer w;
    pq::WriterOptions opts;
    opts.rowGroupRows = 2;
    opts.pageRows = 1;
    std::string err;
    CHECK(w.open(path, smallSchema(), opts, &err));
    for (int i = 0; i < 5; ++i) {
        w.setInt32(0, i);
        w.setInt64(1, i);
        w.setDouble(2, i);
        w.setString(3, "x");
        w.setBool(4, false);
        CHECK(w.endRow(&err));
    }
    CHECK(w.close(&err));
    const auto file = readWhole(path);
    const auto f = parseFooter(file);
    CHECK(f.ok);
    CHECK(f.numRows == 5);
    CHECK(f.groups.size() == 3);
    if (!f.ok || f.groups.size() != 3 || f.groups[0].chunks.empty()) {
        return;
    }
    CHECK(f.groups[0].numRows == 2);
    CHECK(f.groups[1].numRows == 2);
    CHECK(f.groups[2].numRows == 1);
    // pageRows = 1, so the first chunk of the first group is two 1-value pages.
    const auto p0 = parsePageHeader(file, static_cast<std::size_t>(f.groups[0].chunks[0].dataPageOffset));
    CHECK(p0.numValues == 1);
    CHECK(p0.uncompressed == 4);
    const auto p1 = parsePageHeader(
        file, static_cast<std::size_t>(f.groups[0].chunks[0].dataPageOffset) + p0.headerBytes + 4);
    CHECK(p1.ok);
    CHECK(p1.numValues == 1);
}

void testWriterRejectsBadRows() {
    const auto path = scratch() / "bad.parquet";
    pq::Writer w;
    std::string err;
    CHECK(w.open(path, smallSchema(), pq::WriterOptions{}, &err));
    // Column 4 never set: the row is incomplete and must not be accepted.
    w.setInt32(0, 1);
    w.setInt64(1, 1);
    w.setDouble(2, 1.0);
    w.setString(3, "x");
    CHECK(!w.endRow(&err));
    CHECK(w.failed());
    CHECK(!w.close(&err));

    pq::Writer w2;
    CHECK(w2.open(scratch() / "bad2.parquet", smallSchema(), pq::WriterOptions{}, &err));
    w2.setNull(0);  // required column
    CHECK(w2.failed());
}

// ---------------------------------------------------------------------------
// Exporter over a synthetic store
// ---------------------------------------------------------------------------

constexpr std::uint32_t kSigAlpha = 287454020u;    // fits in int32
constexpr std::uint32_t kSigBeta = 2596069104u;    // above INT32_MAX
constexpr std::uint32_t kSigFloat = 555000111u;
constexpr std::uint32_t kSigSlow = 909090909u;
constexpr std::uint32_t kSigNegFactor = 121212121u;
constexpr std::uint64_t kBase = 1700000000000ull;  // multiple of 100, 1000, ...
constexpr int kSamples = 500;                      // 50 Hz for 10 s
constexpr std::uint64_t kStepMs = 20;
constexpr int kSlowSamples = 20;  // 2 Hz for 10 s, so L1 and L2 are not built
constexpr std::uint64_t kSlowStepMs = 500;

void testWideRollupMean() {
    // The persisted sum is one or two 64-bit words (spec §5.1). A two-word sum
    // needs a session of large values long enough to overflow int64, which the
    // synthetic store cannot reach, so the fold is driven directly here. Without
    // this the sumHi read in meanRaw would be a field nothing ever checks.
    tsdb::RollupBucket b;
    b.count = 4;
    b.sum = 10;
    CHECK(near(pexport::meanRaw(b, true), 2.5));
    CHECK(near(pexport::meanRaw(b, false), 2.5));

    // Signed, one word: sum is a plain negative int64 and wide is clear.
    b.sum = -10;
    b.wide = false;
    CHECK(near(pexport::meanRaw(b, true), -2.5));

    // Unsigned, two words: sumHi carries 2^64 and is read unconditionally, the
    // way core's wideSumOf reads it. The low word is 2^62 so that 2^64 + 2^62 is
    // exactly representable as a double and differs from 2^64 alone; a low word
    // small enough to round away could not tell the two apart.
    constexpr double kTwo62 = 4611686018427387904.0;   // 2^62
    constexpr double kTwo64 = 18446744073709551616.0;  // 2^64
    b.wide = false;
    b.count = 2;
    b.sum = 0;
    b.sumHi = 1;
    CHECK(near(pexport::meanRaw(b, false), kTwo64 / 2.0));
    b.count = 1;
    b.sum = static_cast<std::int64_t>(1) << 62;
    CHECK(near(pexport::meanRaw(b, false), kTwo64 + kTwo62));

    // Signed, two words: sumHi is the signed high half, so 0xFFFF...F is -1 and
    // the total is -2^64 + 2^62. Only the wide flag selects this path.
    b.wide = true;
    b.sumHi = ~std::uint64_t{0};
    CHECK(near(pexport::meanRaw(b, true), -kTwo64 + kTwo62));
    // With wide clear the same words must fold as a single signed word instead.
    b.wide = false;
    CHECK(near(pexport::meanRaw(b, true), kTwo62));

    // An empty bucket has no mean and must not divide by zero.
    b.count = 0;
    CHECK(pexport::meanRaw(b, true) == 0.0);
    CHECK(pexport::meanRaw(b, false) == 0.0);
}

std::int64_t alphaRaw(int i) { return static_cast<std::int64_t>((i % 7) * 3 - 9); }
std::int64_t betaRaw(int i) { return static_cast<std::int64_t>(i % 200); }

class Collector final : public pexport::RowSink {
public:
    std::vector<pexport::ExportRow> rows;
    bool row(const pexport::ExportRow& r) override {
        rows.push_back(r);
        return true;
    }
};

// Reading past the end yields a default row rather than undefined behaviour, so
// a break that empties the export fails the value checks below instead of
// crashing the process and hiding every test that would have run after it.
const pexport::ExportRow& at(const Collector& c, std::size_t i) {
    static const pexport::ExportRow kMissing{};
    return i < c.rows.size() ? c.rows[i] : kMissing;
}

const pexport::ExportRow& lastRow(const Collector& c) {
    return at(c, c.rows.empty() ? 0 : c.rows.size() - 1);
}

bool buildStore(const fs::path& dir) {
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::remove_all(fs::path(dir.string() + ".building"), ec);
    tsdb::SessionManifest man;
    man.id = "synthetic";
    man.minMs = kBase;
    man.maxMs = kBase + kStepMs * (kSamples - 1);
    man.dbcSha = "0";
    man.dbcLabel = "synthetic";
    tsdb::Store store;
    if (!store.create(dir, man)) {
        return false;
    }
    tsdb::SignalInfo a;
    a.id = kSigAlpha;
    a.message = "Test";
    a.name = "Alpha";
    a.unit = "v";
    a.factor = 0.5;
    a.offset = -3.0;
    a.bits = 16;
    a.isSigned = true;
    a.valueType = dbc::ValueType::Integer;
    store.setSignal(a);
    tsdb::SignalInfo b = a;
    b.id = kSigBeta;
    b.name = "Beta";
    b.factor = 1.0;
    b.offset = 0.0;
    b.bits = 8;
    b.isSigned = false;
    store.setSignal(b);
    tsdb::SignalInfo c = a;
    c.id = kSigFloat;
    c.name = "Gamma";
    c.factor = 1.0;
    c.offset = 0.0;
    c.bits = 32;
    c.isSigned = true;
    c.valueType = dbc::ValueType::Float;
    store.setSignal(c);
    tsdb::SignalInfo d = a;
    d.id = kSigSlow;
    d.name = "Delta";
    store.setSignal(d);
    // A negative DBC factor, as IMU_Accel_FR.Accel_X really has: the raw-domain
    // min becomes the physical max.
    tsdb::SignalInfo e = a;
    e.id = kSigNegFactor;
    e.name = "Epsilon";
    e.factor = -0.25;
    e.offset = 2.0;
    store.setSignal(e);
    for (int i = 0; i < kSlowSamples; ++i) {
        store.append(kSigSlow, kBase + kSlowStepMs * static_cast<std::uint64_t>(i),
                     static_cast<std::int64_t>(i));
    }
    for (int i = 0; i < kSamples; ++i) {
        const auto t = kBase + kStepMs * static_cast<std::uint64_t>(i);
        store.append(kSigAlpha, t, alphaRaw(i));
        store.append(kSigBeta, t, betaRaw(i));
        store.append(kSigNegFactor, t, alphaRaw(i));
        const float fv = -1.0f * static_cast<float>(i);
        store.append(kSigFloat, t,
                     static_cast<std::int64_t>(static_cast<std::int32_t>(std::bit_cast<std::uint32_t>(fv))));
    }
    std::string err;
    return store.finish(&err);
}

void testRawExport(const tsdb::Store& store) {
    pexport::Request req;
    req.fromMs = kBase;
    req.toMs = kBase + kStepMs * (kSamples - 1);
    req.signalIds = {kSigAlpha};
    req.level = 0;
    Collector sink;
    const auto rep = pexport::exportRange(store, req, sink);
    CHECK(rep.ok);
    CHECK(rep.rows == static_cast<std::uint64_t>(kSamples));
    CHECK(sink.rows.size() == static_cast<std::size_t>(kSamples));
    CHECK(rep.signals.size() == 1);
    CHECK(rep.signals[0].level == 0);
    bool timesOk = true;
    bool valuesOk = true;
    for (int i = 0; i < kSamples && i < static_cast<int>(sink.rows.size()); ++i) {
        const auto& r = sink.rows[static_cast<std::size_t>(i)];
        if (r.tMs != kBase + kStepMs * static_cast<std::uint64_t>(i)) {
            timesOk = false;
        }
        if (r.raw != alphaRaw(i) || !near(r.value, static_cast<double>(alphaRaw(i)) * 0.5 - 3.0)) {
            valuesOk = false;
        }
        if (r.hasEnvelope || !r.hasValue || r.level != 0 || r.bucketMs != 0) {
            valuesOk = false;
        }
    }
    CHECK(timesOk);
    CHECK(valuesOk);
    // A raw sample is in range iff from <= t <= to, both ends inclusive.
    CHECK(at(sink, 0).tMs == kBase);
    CHECK(lastRow(sink).tMs == kBase + kStepMs * (kSamples - 1));
}

void testRawRangeClipping(const tsdb::Store& store) {
    pexport::Request req;
    req.signalIds = {kSigAlpha};
    req.level = 0;
    req.fromMs = kBase + 2 * kStepMs;  // exactly sample 2
    req.toMs = kBase + 7 * kStepMs;    // exactly sample 7
    Collector inclusive;
    CHECK(pexport::exportRange(store, req, inclusive).ok);
    CHECK(inclusive.rows.size() == 6);
    CHECK(at(inclusive, 0).tMs == req.fromMs);
    CHECK(lastRow(inclusive).tMs == req.toMs);

    // One ms inside on each side drops both endpoints.
    req.fromMs += 1;
    req.toMs -= 1;
    Collector tight;
    CHECK(pexport::exportRange(store, req, tight).ok);
    CHECK(tight.rows.size() == 4);
    CHECK(at(tight, 0).tMs == kBase + 3 * kStepMs);
    CHECK(lastRow(tight).tMs == kBase + 6 * kStepMs);
}

void testRollupExport(const tsdb::Store& store) {
    // 50 Hz over 10 s: L1 (100 ms) buckets hold exactly 5 samples each.
    pexport::Request req;
    req.fromMs = kBase;
    req.toMs = kBase + kStepMs * (kSamples - 1);
    req.signalIds = {kSigAlpha};
    req.level = 1;
    Collector sink;
    const auto rep = pexport::exportRange(store, req, sink);
    CHECK(rep.ok);
    CHECK(rep.signals.size() == 1);
    CHECK(rep.signals[0].level == 1);
    CHECK(sink.rows.size() == 100);
    bool ok = true;
    for (std::size_t k = 0; k < sink.rows.size(); ++k) {
        const auto& r = sink.rows[k];
        std::int64_t lo = alphaRaw(static_cast<int>(5 * k));
        std::int64_t hi = lo;
        std::int64_t sum = 0;
        for (int j = 0; j < 5; ++j) {
            const auto v = alphaRaw(static_cast<int>(5 * k) + j);
            lo = std::min(lo, v);
            hi = std::max(hi, v);
            sum += v;
        }
        const auto phys = [](std::int64_t raw) { return static_cast<double>(raw) * 0.5 - 3.0; };
        if (r.tMs != kBase + 100ull * k || r.level != 1 || r.bucketMs != 100) {
            ok = false;
        }
        if (r.hasValue || !r.hasEnvelope || !r.hasMinMax || !r.hasMean) {
            ok = false;
        }
        if (r.count != 5) {
            ok = false;
        }
        if (!near(r.vMin, phys(lo)) || !near(r.vMax, phys(hi))) {
            ok = false;
        }
        if (!near(r.vMean, (static_cast<double>(sum) / 5.0) * 0.5 - 3.0)) {
            ok = false;
        }
        if (!near(r.vFirst, phys(alphaRaw(static_cast<int>(5 * k)))) ||
            !near(r.vLast, phys(alphaRaw(static_cast<int>(5 * k) + 4)))) {
            ok = false;
        }
    }
    CHECK(ok);
    // The mean is a real average, not the midpoint of min and max: bucket 0
    // holds raws -9,-6,-3,0,3 whose mean is -3 while (min+max)/2 is -3... use
    // bucket 1 (raws 6,9,-9,-6,-3) where mean 0.6*0.5-3 differs from midpoint.
    CHECK(!near(at(sink, 1).vMean, (at(sink, 1).vMin + at(sink, 1).vMax) / 2.0));
}

void testUnsignedRollup(const tsdb::Store& store) {
    pexport::Request req;
    req.fromMs = kBase;
    req.toMs = kBase + kStepMs * (kSamples - 1);
    req.signalIds = {kSigBeta};
    req.level = 1;
    Collector sink;
    CHECK(pexport::exportRange(store, req, sink).ok);
    CHECK(sink.rows.size() == 100);
    bool ok = true;
    for (std::size_t k = 0; k < sink.rows.size(); ++k) {
        std::int64_t lo = betaRaw(static_cast<int>(5 * k));
        std::int64_t hi = lo;
        std::int64_t sum = 0;
        for (int j = 0; j < 5; ++j) {
            const auto v = betaRaw(static_cast<int>(5 * k) + j);
            lo = std::min(lo, v);
            hi = std::max(hi, v);
            sum += v;
        }
        const auto& r = sink.rows[k];
        if (!near(r.vMin, static_cast<double>(lo)) || !near(r.vMax, static_cast<double>(hi)) ||
            !near(r.vMean, static_cast<double>(sum) / 5.0) || r.count != 5) {
            ok = false;
        }
    }
    CHECK(ok);
}

void testNegativeFactorEnvelope(const tsdb::Store& store) {
    pexport::Request req;
    req.fromMs = kBase;
    req.toMs = kBase + kStepMs * (kSamples - 1);
    req.signalIds = {kSigNegFactor};
    req.level = 1;
    Collector sink;
    CHECK(pexport::exportRange(store, req, sink).ok);
    CHECK(sink.rows.size() == 100);
    const auto phys = [](std::int64_t raw) { return static_cast<double>(raw) * -0.25 + 2.0; };
    bool ordered = true;
    bool matchesRaw = true;
    for (std::size_t k = 0; k < sink.rows.size(); ++k) {
        std::int64_t lo = alphaRaw(static_cast<int>(5 * k));
        std::int64_t hi = lo;
        for (int j = 0; j < 5; ++j) {
            const auto v = alphaRaw(static_cast<int>(5 * k) + j);
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        const auto& r = sink.rows[k];
        if (r.vMin > r.vMax) {
            ordered = false;
        }
        // The physical min comes from the raw max because the factor is negative.
        if (!near(r.vMin, phys(hi)) || !near(r.vMax, phys(lo))) {
            matchesRaw = false;
        }
    }
    CHECK(ordered);
    CHECK(matchesRaw);
    // The scale really is inverting: raw min and max differ in this data, so a
    // naive physical(min)/physical(max) assignment would have failed above.
    CHECK(at(sink, 0).vMin < at(sink, 0).vMax);
    CHECK(near(at(sink, 0).vMin, phys(3)));
    CHECK(near(at(sink, 0).vMax, phys(-9)));
}

void testBucketEdgeSemantics(const tsdb::Store& store) {
    // Spec §6: a bucket is in range iff [start, start+bucketMs) intersects
    // [from, to], so a bucket ending exactly at from is excluded.
    CHECK(!pexport::bucketInRange(kBase, 100, kBase + 100, kBase + 500));
    CHECK(pexport::bucketInRange(kBase + 100, 100, kBase + 100, kBase + 500));
    CHECK(pexport::bucketInRange(kBase + 500, 100, kBase + 100, kBase + 500));
    CHECK(!pexport::bucketInRange(kBase + 600, 100, kBase + 100, kBase + 500));

    pexport::Request req;
    req.fromMs = kBase + 300;
    req.toMs = kBase + 500;
    req.signalIds = {kSigAlpha};
    req.level = 1;
    Collector sink;
    CHECK(pexport::exportRange(store, req, sink).ok);
    // Buckets starting at 300, 400 and 500. The one ending at 300 is out.
    CHECK(sink.rows.size() == 3);
    CHECK(at(sink, 0).tMs == kBase + 300);
    CHECK(lastRow(sink).tMs == kBase + 500);
}

void testFloatValueTypeSuppression(const tsdb::Store& store) {
    pexport::Request req;
    req.fromMs = kBase;
    req.toMs = kBase + kStepMs * (kSamples - 1);
    req.signalIds = {kSigFloat};
    req.level = 1;
    Collector sink;
    const auto rep = pexport::exportRange(store, req, sink);
    CHECK(rep.ok);
    CHECK(rep.signals.size() == 1);
    CHECK(rep.signals[0].envelopeSuppressed);
    CHECK(!sink.rows.empty());
    bool suppressed = true;
    bool firstLastOk = true;
    for (const auto& r : sink.rows) {
        if (r.hasMinMax || r.hasMean) {
            suppressed = false;
        }
        if (!r.hasEnvelope || r.count == 0) {
            firstLastOk = false;
        }
    }
    CHECK(suppressed);
    CHECK(firstLastOk);
    // first/last stay real samples: bucket 0 starts at -0.0 and ends at -4.0.
    CHECK(near(at(sink, 0).vFirst, -0.0));
    CHECK(near(at(sink, 0).vLast, -4.0));

    // At L0 the float signal is exact and nothing is suppressed.
    req.level = 0;
    Collector raw;
    const auto rawRep = pexport::exportRange(store, req, raw);
    CHECK(rawRep.ok);
    CHECK(!rawRep.signals[0].envelopeSuppressed);
    CHECK(raw.rows.size() == static_cast<std::size_t>(kSamples));
    CHECK(near(at(raw, 3).value, -3.0));
}

void testLevelSelection(const tsdb::Store& store) {
    const auto span = kStepMs * (kSamples - 1);  // 9980 ms
    // 500 raw samples against a 2*2000 budget fits, so raw wins.
    CHECK(pexport::autoLevel(store, kSigAlpha, kBase, kBase + span, 2000) == 0);
    // Budget 2*50 = 100 cannot hold 500 raw samples, and 9980/100 = 99 slots at
    // L1 does fit, so L1 is the finest rollup that works.
    CHECK(pexport::autoLevel(store, kSigAlpha, kBase, kBase + span, 50) == 1);
    // Budget 50: L1 would need 99 slots, L2 needs 9.
    CHECK(pexport::autoLevel(store, kSigAlpha, kBase, kBase + span, 25) == 2);
    // Budget 8: L2's 9 slots overshoot, L3 needs 0.
    CHECK(pexport::autoLevel(store, kSigAlpha, kBase, kBase + span, 4) == 3);
    // The slow signal has only 20 samples in the same window, so the same budget
    // that pushes the 50 Hz signal to L1 still fits its raw data. This is the
    // per-signal divergence spec §6 requires.
    CHECK(pexport::autoLevel(store, kSigSlow, kBase, kBase + span, 50) == 0);
    // Squeeze the budget below 20 raw samples and it does roll up, but to L3:
    // L1 and L2 were never built for 2 Hz, and 9980/10000 needs no slots.
    CHECK(pexport::autoLevel(store, kSigSlow, kBase, kBase + span, 4) == 3);
    // L1 and L2 were never built for a 2 Hz signal, so a forced request degrades.
    CHECK((store.levelsOf(kSigSlow) & 0x02u) == 0);
    CHECK((store.levelsOf(kSigAlpha) & 0x02u) != 0);
    CHECK(pexport::clampLevel(1, store.levelsOf(kSigSlow)) == 0);
    CHECK(pexport::clampLevel(1, store.levelsOf(kSigAlpha)) == 1);
    CHECK(pexport::clampLevel(4, store.levelsOf(kSigSlow)) == 4);

    // Degradation against explicit level masks. 0b000011 is L0 and L1 only.
    CHECK(pexport::clampLevel(4, 0x03) == 1);
    CHECK(pexport::clampLevel(1, 0x03) == 1);
    CHECK(pexport::clampLevel(0, 0x03) == 0);
    CHECK(pexport::clampLevel(5, 0x01) == 0);
    CHECK(pexport::clampLevel(3, 0x0D) == 3);  // L0, L2, L3 built
    CHECK(pexport::clampLevel(4, 0x0D) == 3);
}

void testAutoLevelExportMixesLevels(const tsdb::Store& store) {
    // Auto selection is per signal, so one export legitimately carries more than
    // one level. A 50 Hz signal rolls up to L1 here while a 2 Hz signal in the
    // same window still fits raw, and the long format has to express both.
    pexport::Request req;
    req.fromMs = kBase;
    req.toMs = kBase + kStepMs * (kSamples - 1);
    req.signalIds = {kSigAlpha, kSigSlow};
    req.level = -1;
    req.maxPoints = 50;
    Collector sink;
    const auto rep = pexport::exportRange(store, req, sink);
    CHECK(rep.ok);
    CHECK(rep.signals.size() == 2);
    CHECK(rep.signals[0].signalId == kSigAlpha);
    CHECK(rep.signals[0].level == 1);
    CHECK(rep.signals[0].rows == 100);
    CHECK(rep.signals[1].signalId == kSigSlow);
    CHECK(rep.signals[1].level == 0);
    CHECK(rep.signals[1].rows == static_cast<std::uint64_t>(kSlowSamples));
    CHECK(rep.rows == 100 + static_cast<std::uint64_t>(kSlowSamples));
    std::size_t rollupRows = 0;
    std::size_t rawRows = 0;
    for (const auto& r : sink.rows) {
        if (r.level == 1 && r.bucketMs == 100 && r.hasEnvelope && !r.hasValue) {
            ++rollupRows;
        } else if (r.level == 0 && r.bucketMs == 0 && r.hasValue && !r.hasEnvelope) {
            ++rawRows;
        }
    }
    CHECK(rollupRows == 100);
    CHECK(rawRows == static_cast<std::size_t>(kSlowSamples));
}

void testBoundarySamples(const tsdb::Store& store) {
    pexport::Request req;
    req.fromMs = kBase + 100;
    req.toMs = kBase + 200;
    req.signalIds = {kSigAlpha};
    req.level = 0;
    Collector sink;
    const auto rep = pexport::exportRange(store, req, sink);
    CHECK(rep.ok);
    CHECK(rep.signals.size() == 1);
    CHECK(rep.signals[0].prior.has_value());
    CHECK(rep.signals[0].next.has_value());
    // prior is the last sample at or before from; samples land every 20 ms and
    // from is exactly on one, so prior is that sample itself.
    CHECK(rep.signals[0].prior->timestampMs == kBase + 100);
    CHECK(rep.signals[0].next->timestampMs == kBase + 200);
    CHECK(rep.signals[0].prior->raw == alphaRaw(5));
}

void testEmptyRange(const tsdb::Store& store) {
    pexport::Request req;
    req.fromMs = kBase + 1000000;
    req.toMs = kBase + 2000000;
    req.signalIds = {kSigAlpha};
    req.level = 0;
    Collector sink;
    const auto rep = pexport::exportRange(store, req, sink);
    CHECK(rep.ok);
    CHECK(rep.rows == 0);
    CHECK(sink.rows.empty());
    CHECK(rep.signals.size() == 1);
}

void testPageStreamingIsBounded(const tsdb::Store& store) {
    // The exporter decodes one page at a time, so the largest page it touched
    // must stay near the format's payload cap regardless of how many rows come
    // out. This is the property that lets a 120 M sample session export.
    pexport::Request req;
    req.fromMs = kBase;
    req.toMs = kBase + kStepMs * (kSamples - 1);
    req.signalIds = {kSigAlpha, kSigBeta, kSigFloat};
    req.level = 0;
    Collector sink;
    const auto rep = pexport::exportRange(store, req, sink);
    CHECK(rep.ok);
    CHECK(rep.rows == static_cast<std::uint64_t>(3 * kSamples));
    CHECK(rep.pages >= 3);
    CHECK(rep.maxPageBytes > 0);
    CHECK(rep.maxPageBytes <= tsdb::kPagePayloadCap + tsdb::kPageHeaderBytes);
    // The cap alone is a weak bound: it would still hold if the exporter kept
    // every page alive at once. Recompute the largest and the total page size
    // from the index and require the report to be the largest, not the total.
    std::uint64_t widest = 0;
    std::uint64_t allPages = 0;
    std::uint64_t counted = 0;
    for (const auto& rec : store.index(0)) {
        if (rec.signalId != kSigAlpha && rec.signalId != kSigBeta && rec.signalId != kSigFloat) {
            continue;
        }
        widest = std::max<std::uint64_t>(widest, rec.pageBytes);
        allPages += rec.pageBytes;
        ++counted;
    }
    CHECK(counted == rep.pages);
    CHECK(rep.pageBytes == allPages);
    CHECK(rep.maxPageBytes == widest);
    // Only meaningful if there really is more than one page to confuse for one.
    CHECK(counted > 1);
    CHECK(allPages > widest);
}

void testBadRequest(const tsdb::Store& store) {
    pexport::Request req;
    req.fromMs = kBase + 1000;
    req.toMs = kBase;
    req.signalIds = {kSigAlpha};
    Collector sink;
    const auto rep = pexport::exportRange(store, req, sink);
    CHECK(!rep.ok);
    CHECK(!rep.error.empty());
    CHECK(sink.rows.empty());
}

class Rejector final : public pexport::RowSink {
public:
    bool row(const pexport::ExportRow&) override { return false; }
};

void testCsvExport(const tsdb::Store& store) {
    pexport::Request req;
    req.fromMs = kBase;
    req.toMs = kBase + kStepMs * 2;
    req.signalIds = {kSigAlpha};
    req.level = 0;
    std::string csv;
    pexport::Report rep;
    CHECK(pexport::writeCsv(store, req, [&](std::string_view chunk) {
        csv.append(chunk);
        return true;
    }, &rep));
    CHECK(rep.ok);
    CHECK(csv.find("t_ms,message,signal,unit,value\n") == 0);
    CHECK(csv.find("Test,Alpha,v,") != std::string::npos);

    pexport::CsvQuery q;
    CHECK(pexport::parseCsvQuery("/export.csv?session=20260907-021613-00", &q));
    CHECK(q.session == "20260907-021613-00");
    CHECK(q.level == 2);
    CHECK(!q.allSignals);
    CHECK(q.selectors.empty());
    CHECK(pexport::parseCsvQuery("/export.csv?session=20260907-021613-00&level=0&signals=all", &q));
    CHECK(q.level == 0);
    CHECK(q.allSignals);
    CHECK(pexport::parseCsvQuery(
        "/export.csv?session=20260907-021613-00&sig=Test.Alpha&sig=Beta&signals=Test.Gamma,Delta", &q));
    CHECK(!q.allSignals);
    CHECK(q.selectors.size() == 4);
    CHECK(q.selectors[0] == "Test.Alpha");
    CHECK(q.selectors[1] == "Beta");
    CHECK(q.selectors[2] == "Test.Gamma");
    CHECK(q.selectors[3] == "Delta");
    const auto resolved = pexport::resolveSignalSelectors(store, q.selectors);
    CHECK(resolved.size() == 4);
    CHECK(resolved[0] == kSigAlpha);
    CHECK(resolved[1] == kSigBeta);
    CHECK(resolved[2] == kSigFloat);
    CHECK(resolved[3] == kSigSlow);
    CHECK(pexport::resolveSignalSelectors(store, {"Nope"}).empty());
    CHECK(pexport::validSessionId("20260907-021613-00"));
    CHECK(!pexport::validSessionId("../secret"));
    CHECK(!pexport::parseCsvQuery("/export.csv", &q));
    CHECK(!pexport::parseCsvQuery("/other?session=20260907-021613-00", &q));

    std::string quoted;
    pexport::csvEscape(&quoted, "a,b");
    CHECK(quoted == "\"a,b\"");
}

void testSinkFailurePropagates(const tsdb::Store& store) {
    pexport::Request req;
    req.fromMs = kBase;
    req.toMs = kBase + 1000;
    req.signalIds = {kSigAlpha};
    req.level = 0;
    Rejector sink;
    const auto rep = pexport::exportRange(store, req, sink);
    CHECK(!rep.ok);
    CHECK(rep.error == "sink rejected a row");
}

}  // namespace

int main() {
    testVarint();
    testZigzag();
    testFieldHeader();
    testDefLevels();
    testPlainBooleans();
    testFileStructure();
    testStatistics();
    testNanIsNotABound();
    testRowGroupsAndPages();
    testWriterRejectsBadRows();
    testWideRollupMean();

    const auto storeDir = scratch() / "synthetic-store";
    if (!buildStore(storeDir)) {
        std::fprintf(stderr, "FAIL could not build the synthetic store at %s\n", storeDir.string().c_str());
        ++gFailed;
    } else {
        tsdb::Store store;
        std::string err;
        if (!store.open(storeDir, &err)) {
            std::fprintf(stderr, "FAIL could not open the synthetic store: %s\n", err.c_str());
            ++gFailed;
        } else {
            testRawExport(store);
            testRawRangeClipping(store);
            testRollupExport(store);
            testUnsignedRollup(store);
            testNegativeFactorEnvelope(store);
            testBucketEdgeSemantics(store);
            testFloatValueTypeSuppression(store);
            testLevelSelection(store);
            testAutoLevelExportMixesLevels(store);
            testBoundarySamples(store);
            testEmptyRange(store);
            testPageStreamingIsBounded(store);
            testBadRequest(store);
            testCsvExport(store);
            testSinkFailurePropagates(store);
        }
    }

    std::fprintf(stdout, "photon-export-tests: %d passed, %d failed\n", gPassed, gFailed);
    return gFailed == 0 ? 0 : 1;
}
