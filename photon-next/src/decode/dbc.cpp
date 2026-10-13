#include "decode/dbc.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <locale>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace photon::dbc {
namespace {

void trim(std::string& line) {
    const auto begin = line.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        line.clear();
        return;
    }
    const auto end = line.find_last_not_of(" \t\r\n");
    line.erase(end + 1);
    line.erase(0, begin);
}

std::istringstream lineStream(const std::string& line) {
    std::istringstream iss(line);
    iss.imbue(std::locale::classic());
    return iss;
}

std::int64_t signExtend(std::uint64_t raw, int bits) {
    if (bits <= 0 || bits >= 64) {
        return static_cast<std::int64_t>(raw);
    }
    const std::uint64_t signBit = std::uint64_t{1} << (bits - 1);
    const std::uint64_t mask = (std::uint64_t{1} << bits) - 1;
    raw &= mask;
    if ((raw & signBit) != 0) {
        raw |= ~mask;
    }
    return static_cast<std::int64_t>(raw);
}

std::uint32_t assignId(std::string_view message, std::string_view name,
                       std::unordered_set<std::uint32_t>& used) {
    std::uint32_t id = signalIdOf(message, name);
    while (used.contains(id)) {
        id = id + 1;
        if (id == 0) {
            id = 1;
        }
    }
    used.insert(id);
    return id;
}

bool parseMessageHeader(const std::string& line, Message& msg) {
    auto iss = lineStream(line);
    std::string tag;
    std::string nameTok;
    std::string dlcTok;
    std::string sender;
    std::uint32_t canId = 0;
    iss >> tag >> canId >> nameTok;
    if (tag != "BO_" || nameTok.empty()) {
        return false;
    }
    std::string name = nameTok;
    if (name.back() == ':') {
        name.pop_back();
    } else {
        std::string colon;
        iss >> colon;
        if (colon != ":") {
            return false;
        }
    }
    iss >> dlcTok >> sender;
    if (name.empty() || dlcTok.empty()) {
        return false;
    }
    try {
        const int dlc = std::stoi(dlcTok);
        if (dlc < 0 || dlc > 8) {
            return false;
        }
        msg.dlc = static_cast<std::uint8_t>(dlc);
    } catch (...) {
        return false;
    }
    msg.canId = canId & 0x1FFFFFFFu;
    msg.name = std::move(name);
    return true;
}

bool parseSignalLine(const std::string& line, const Message& msg, Signal& sig) {
    auto iss = lineStream(line);
    std::string tag;
    std::string name;
    iss >> tag >> name;
    if (tag != "SG_" || name.empty()) {
        return false;
    }
    if (name.back() == ':') {
        name.pop_back();
    } else {
        std::string next;
        iss >> next;
        if (next != ":") {
            // Multiplexed (`m0`) / multiplexor (`M`) — ignored for now.
            return false;
        }
    }
    if (name.empty()) {
        return false;
    }

    int startBit = 0;
    int length = 0;
    int endian = 1;
    char sign = '+';
    iss >> startBit;
    iss.ignore(1, '|');
    iss >> length;
    iss.ignore(1, '@');
    iss >> endian >> sign;
    if (!iss || length <= 0 || length > 64 || startBit < 0) {
        return false;
    }

    double factor = 1.0;
    double offset = 0.0;
    char c = '\0';
    if (iss >> c && c == '(') {
        iss >> factor;
        iss.ignore(1, ',');
        iss >> offset;
        iss.ignore(1, ')');
    }
    double minV = 0.0;
    double maxV = 0.0;
    if (iss >> c && c == '[') {
        iss >> minV;
        iss.ignore(1, '|');
        iss >> maxV;
        iss.ignore(1, ']');
    }
    std::string unit;
    if (iss >> std::ws && iss.peek() == '"') {
        iss.get();
        std::getline(iss, unit, '"');
    }

    sig.canId = msg.canId;
    sig.message = msg.name;
    sig.name = std::move(name);
    sig.startBit = startBit;
    sig.length = length;
    sig.littleEndian = (endian == 1);
    sig.isSigned = (sign == '-');
    sig.factor = factor;
    sig.offset = offset;
    sig.min = minV;
    sig.max = maxV;
    sig.unit = std::move(unit);
    return true;
}

}  // namespace

std::uint32_t signalIdOf(std::string_view message, std::string_view name) {
    std::uint32_t h = 2166136261u;
    const auto mix = [&](unsigned char c) {
        h ^= c;
        h *= 16777619u;
    };
    for (char c : message) {
        mix(static_cast<unsigned char>(c));
    }
    mix(static_cast<unsigned char>('.'));
    for (char c : name) {
        mix(static_cast<unsigned char>(c));
    }
    return h == 0 ? 1u : h;
}

const Message* Database::findMessage(std::uint32_t canId) const {
    const auto it = byCanId_.find(canId & 0x1FFFFFFFu);
    if (it == byCanId_.end()) {
        return nullptr;
    }
    return &messages[it->second];
}

const Message* Database::findMessageByName(std::string_view name) const {
    for (const auto& msg : messages) {
        if (msg.name == name) {
            return &msg;
        }
    }
    return nullptr;
}

const Signal* Database::findSignal(std::uint32_t signalId) const {
    const auto it = bySignalId_.find(signalId);
    if (it == bySignalId_.end()) {
        return nullptr;
    }
    return &messages[it->second.first].signals[it->second.second];
}

std::vector<CatalogEntry> Database::catalog() const {
    std::vector<CatalogEntry> out;
    for (const auto& msg : messages) {
        for (const auto& sig : msg.signals) {
            CatalogEntry e;
            e.id = sig.signalId;
            e.message = sig.message;
            e.name = sig.name;
            e.unit = sig.unit;
            e.min = sig.min;
            e.max = sig.max;
            out.push_back(std::move(e));
        }
    }
    return out;
}

void Database::rebuildIndex() {
    byCanId_.clear();
    bySignalId_.clear();
    byCanId_.reserve(messages.size());
    for (std::size_t mi = 0; mi < messages.size(); ++mi) {
        byCanId_[messages[mi].canId & 0x1FFFFFFFu] = mi;
        for (std::size_t si = 0; si < messages[mi].signals.size(); ++si) {
            bySignalId_[messages[mi].signals[si].signalId] = {mi, si};
        }
    }
}

std::optional<Database> parse(std::string_view text, std::string* error) {
    Database db;
    std::unordered_set<std::uint32_t> usedIds;
    Message current;
    bool haveMsg = false;
    std::istringstream in{std::string(text)};
    in.imbue(std::locale::classic());
    std::string line;
    std::vector<std::pair<std::uint32_t, std::pair<std::string, ValueType>>> valTypes;

    while (std::getline(in, line)) {
        trim(line);
        if (line.empty()) {
            continue;
        }
        if (line.rfind("BO_", 0) == 0) {
            if (haveMsg) {
                db.messages.push_back(std::move(current));
            }
            current = Message{};
            haveMsg = parseMessageHeader(line, current);
            continue;
        }
        if (line.rfind("SG_", 0) == 0 && haveMsg) {
            Signal sig;
            if (!parseSignalLine(line, current, sig)) {
                continue;
            }
            sig.signalId = assignId(sig.message, sig.name, usedIds);
            current.signals.push_back(std::move(sig));
            continue;
        }
        if (line.rfind("SIG_VALTYPE_", 0) == 0) {
            auto iss = lineStream(line);
            std::string tag;
            std::string sigName;
            std::string colon;
            std::string typeTok;
            std::uint32_t canId = 0;
            iss >> tag >> canId >> sigName >> colon >> typeTok;
            if (typeTok.empty()) {
                continue;
            }
            if (typeTok.back() == ';') {
                typeTok.pop_back();
            }
            try {
                const int raw = std::stoi(typeTok);
                ValueType vt = ValueType::Integer;
                if (raw == 1) {
                    vt = ValueType::Float;
                } else if (raw == 2) {
                    vt = ValueType::Double;
                }
                valTypes.push_back({canId & 0x1FFFFFFFu, {std::move(sigName), vt}});
            } catch (...) {
            }
        }
    }
    if (haveMsg) {
        db.messages.push_back(std::move(current));
    }
    if (db.messages.empty()) {
        if (error) {
            *error = "DBC contains no BO_ messages";
        }
        return std::nullopt;
    }
    for (const auto& [canId, typed] : valTypes) {
        for (auto& msg : db.messages) {
            if (msg.canId != canId) {
                continue;
            }
            for (auto& sig : msg.signals) {
                if (sig.name == typed.first) {
                    sig.valueType = typed.second;
                }
            }
        }
    }
    db.rebuildIndex();
    return db;
}

std::optional<Database> loadFile(const std::string& path, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) {
            *error = "cannot open DBC file";
        }
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    auto db = parse(ss.str(), error);
    if (db) {
        db->path = path;
        db->label = std::filesystem::path(path).stem().string();
    }
    return db;
}

namespace {

bool safeBundledId(std::string_view id) {
    if (id.empty() || id.size() > 96) {
        return false;
    }
    for (char c : id) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

}  // namespace

std::vector<std::string> listBundledIds(const std::string& assetsDir) {
    std::vector<std::string> ids;
    if (assetsDir.empty()) {
        return ids;
    }
    std::error_code ec;
    const auto dir = std::filesystem::path(assetsDir) / "dbc";
    if (!std::filesystem::is_directory(dir, ec)) {
        return ids;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".dbc") {
            continue;
        }
        const auto stem = entry.path().stem().string();
        if (safeBundledId(stem)) {
            ids.push_back(stem);
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

bool extractRaw(const Signal& sig, const std::uint8_t data[8], std::uint8_t dlc, std::uint64_t& raw) {
    raw = 0;
    if (sig.startBit < 0 || sig.length <= 0 || sig.length > 64 || dlc > 8) {
        return false;
    }
    const int availableBits = static_cast<int>(dlc) * 8;
    if (sig.littleEndian) {
        if (sig.startBit + sig.length > availableBits) {
            return false;
        }
        for (int i = 0; i < sig.length; ++i) {
            const int bit = sig.startBit + i;
            raw |= static_cast<std::uint64_t>((data[bit / 8] >> (bit % 8)) & 0x1u) << i;
        }
        return true;
    }
    int bit = sig.startBit;
    for (int i = 0; i < sig.length; ++i) {
        if (bit < 0 || bit >= availableBits) {
            return false;
        }
        raw = (raw << 1) | static_cast<std::uint64_t>((data[bit / 8] >> (bit % 8)) & 0x1u);
        bit = (bit % 8 == 0) ? bit + 15 : bit - 1;
    }
    return true;
}

std::int64_t rawInteger(const Signal& sig, std::uint64_t bits) {
    switch (sig.valueType) {
        case ValueType::Float:
        case ValueType::Double:
            return static_cast<std::int64_t>(bits);
        case ValueType::Integer:
        default:
            return sig.isSigned ? signExtend(bits, sig.length) : static_cast<std::int64_t>(bits);
    }
}

double physicalFromRaw(const Signal& sig, std::int64_t raw) {
    switch (sig.valueType) {
        case ValueType::Float: {
            const auto bits = static_cast<std::uint32_t>(static_cast<std::uint64_t>(raw));
            return static_cast<double>(std::bit_cast<float>(bits)) * sig.factor + sig.offset;
        }
        case ValueType::Double: {
            const auto bits = static_cast<std::uint64_t>(raw);
            return std::bit_cast<double>(bits) * sig.factor + sig.offset;
        }
        case ValueType::Integer:
        default:
            if (!sig.isSigned) {
                return static_cast<double>(static_cast<std::uint64_t>(raw)) * sig.factor + sig.offset;
            }
            return static_cast<double>(raw) * sig.factor + sig.offset;
    }
}

bool extractRawInt(const Signal& sig, const std::uint8_t data[8], std::uint8_t dlc, std::int64_t& raw) {
    std::uint64_t bits = 0;
    if (!extractRaw(sig, data, dlc, bits)) {
        return false;
    }
    raw = rawInteger(sig, bits);
    return true;
}

bool decodeSignal(const Signal& sig, const std::uint8_t data[8], std::uint8_t dlc, double& value) {
    std::int64_t raw = 0;
    if (!extractRawInt(sig, data, dlc, raw)) {
        return false;
    }
    if (sig.valueType == ValueType::Float && sig.length != 32) {
        return false;
    }
    if (sig.valueType == ValueType::Double && sig.length != 64) {
        return false;
    }
    value = physicalFromRaw(sig, raw);
    return true;
}

std::vector<DecodedSample> decodeFrame(const Database& db, std::uint32_t canId, const std::uint8_t data[8],
                                       std::uint8_t dlc) {
    std::vector<DecodedSample> out;
    const Message* msg = db.findMessage(canId);
    if (msg == nullptr) {
        return out;
    }
    out.reserve(msg->signals.size());
    for (const auto& sig : msg->signals) {
        double value = 0.0;
        if (decodeSignal(sig, data, dlc, value)) {
            out.push_back(DecodedSample{sig.signalId, value});
        }
    }
    return out;
}

std::vector<RawDecodedSample> decodeFrameRaw(const Database& db, std::uint32_t canId, const std::uint8_t data[8],
                                             std::uint8_t dlc) {
    std::vector<RawDecodedSample> out;
    const Message* msg = db.findMessage(canId);
    if (msg == nullptr) {
        return out;
    }
    out.reserve(msg->signals.size());
    for (const auto& sig : msg->signals) {
        std::int64_t raw = 0;
        if (extractRawInt(sig, data, dlc, raw)) {
            if (sig.valueType == ValueType::Float && sig.length != 32) {
                continue;
            }
            if (sig.valueType == ValueType::Double && sig.length != 64) {
                continue;
            }
            out.push_back(RawDecodedSample{sig.signalId, raw});
        }
    }
    return out;
}

bool insertRaw(const Signal& sig, std::uint8_t data[8], std::uint8_t dlc, std::uint64_t raw) {
    if (sig.startBit < 0 || sig.length <= 0 || sig.length > 64 || dlc > 8) {
        return false;
    }
    const int availableBits = static_cast<int>(dlc) * 8;
    if (sig.littleEndian) {
        if (sig.startBit + sig.length > availableBits) {
            return false;
        }
        for (int i = 0; i < sig.length; ++i) {
            const int bit = sig.startBit + i;
            const int byteIndex = bit / 8;
            const int bitIndex = bit % 8;
            const std::uint8_t mask = static_cast<std::uint8_t>(1u << bitIndex);
            if (((raw >> i) & 1u) != 0) {
                data[byteIndex] = static_cast<std::uint8_t>(data[byteIndex] | mask);
            } else {
                data[byteIndex] = static_cast<std::uint8_t>(data[byteIndex] & static_cast<std::uint8_t>(~mask));
            }
        }
        return true;
    }
    int bit = sig.startBit;
    for (int i = 0; i < sig.length; ++i) {
        if (bit < 0 || bit >= availableBits) {
            return false;
        }
        const std::uint64_t bitVal = (raw >> (sig.length - 1 - i)) & 1u;
        const int byteIndex = bit / 8;
        const int bitIndex = bit % 8;
        const std::uint8_t mask = static_cast<std::uint8_t>(1u << bitIndex);
        if (bitVal != 0) {
            data[byteIndex] = static_cast<std::uint8_t>(data[byteIndex] | mask);
        } else {
            data[byteIndex] = static_cast<std::uint8_t>(data[byteIndex] & static_cast<std::uint8_t>(~mask));
        }
        bit = (bit % 8 == 0) ? bit + 15 : bit - 1;
    }
    return true;
}

bool encodeSignal(const Signal& sig, std::uint8_t data[8], std::uint8_t dlc, double value, std::string* error) {
    std::uint64_t raw = 0;
    switch (sig.valueType) {
        case ValueType::Float: {
            if (sig.length != 32) {
                if (error) {
                    *error = "float signal must be 32 bits";
                }
                return false;
            }
            const float phys = static_cast<float>((value - sig.offset) / (sig.factor == 0.0 ? 1.0 : sig.factor));
            raw = std::bit_cast<std::uint32_t>(phys);
            break;
        }
        case ValueType::Double: {
            if (sig.length != 64) {
                if (error) {
                    *error = "double signal must be 64 bits";
                }
                return false;
            }
            const double phys = (value - sig.offset) / (sig.factor == 0.0 ? 1.0 : sig.factor);
            raw = std::bit_cast<std::uint64_t>(phys);
            break;
        }
        case ValueType::Integer:
        default: {
            if (sig.factor == 0.0) {
                if (error) {
                    *error = "signal factor is zero";
                }
                return false;
            }
            const double rawD = (value - sig.offset) / sig.factor;
            const auto rawI = std::llround(rawD);
            if (sig.length >= 64) {
                raw = static_cast<std::uint64_t>(rawI);
            } else {
                const std::uint64_t mask = (std::uint64_t{1} << sig.length) - 1;
                raw = static_cast<std::uint64_t>(rawI) & mask;
            }
            break;
        }
    }
    if (!insertRaw(sig, data, dlc, raw)) {
        if (error) {
            *error = "signal does not fit in DLC";
        }
        return false;
    }
    return true;
}

bool encodeFrame(const Database& db, std::string_view messageName, std::span<const SignalValue> signals,
                 EncodedFrame& out, std::string* error) {
    const Message* msg = db.findMessageByName(messageName);
    if (msg == nullptr) {
        if (error) {
            *error = "unknown DBC message '" + std::string(messageName) + "'";
        }
        return false;
    }
    EncodedFrame frame;
    frame.canId = msg->canId & 0x1FFFFFFFu;
    frame.dlc = msg->dlc;
    frame.extended = frame.canId > 0x7FFu;
    for (const auto& req : signals) {
        const Signal* sig = nullptr;
        for (const auto& candidate : msg->signals) {
            if (candidate.name == req.name) {
                sig = &candidate;
                break;
            }
        }
        if (sig == nullptr) {
            if (error) {
                *error = "unknown DBC signal '" + std::string(messageName) + "." + req.name + "'";
            }
            return false;
        }
        std::string encError;
        if (!encodeSignal(*sig, frame.data, frame.dlc, req.value, &encError)) {
            if (error) {
                *error = encError.empty() ? ("failed to encode '" + req.name + "'") : encError;
            }
            return false;
        }
    }
    out = frame;
    return true;
}

}  // namespace photon::dbc
