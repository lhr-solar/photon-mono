#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace photon::dbc {

enum class ValueType : std::uint8_t {
    Integer = 0,
    Float = 1,
    Double = 2,
};

struct Signal {
    std::uint32_t signalId = 0;
    std::uint32_t canId = 0;
    std::string message;
    std::string name;
    int startBit = 0;
    int length = 0;
    bool littleEndian = true;  // DBC @1 Intel / little, @0 Motorola / big
    bool isSigned = false;
    ValueType valueType = ValueType::Integer;
    double factor = 1.0;
    double offset = 0.0;
    double min = 0.0;
    double max = 0.0;
    std::string unit;
};

struct Message {
    std::uint32_t canId = 0;
    std::uint8_t dlc = 0;
    std::string name;
    std::vector<Signal> signals;
};

struct CatalogEntry {
    std::uint32_t id = 0;
    std::string message;
    std::string name;
    std::string unit;
    double min = 0.0;
    double max = 0.0;
};

struct DecodedSample {
    std::uint32_t signalId = 0;
    double value = 0.0;
};

class Database {
public:
    std::vector<Message> messages;
    std::string label;
    std::string path;

    const Message* findMessage(std::uint32_t canId) const;
    const Message* findMessageByName(std::string_view name) const;
    const Signal* findSignal(std::uint32_t signalId) const;
    std::vector<CatalogEntry> catalog() const;
    bool empty() const { return messages.empty(); }

    void rebuildIndex();

private:
    std::unordered_map<std::uint32_t, std::size_t> byCanId_;
    std::unordered_map<std::uint32_t, std::pair<std::size_t, std::size_t>> bySignalId_;
};

// FNV-1a of "MessageName.SignalName". Never returns 0.
std::uint32_t signalIdOf(std::string_view message, std::string_view name);

std::optional<Database> parse(std::string_view text, std::string* error = nullptr);
std::optional<Database> loadFile(const std::string& path, std::string* error = nullptr);

struct SignalValue {
    std::string name;
    double value = 0.0;
};

struct EncodedFrame {
    std::uint32_t canId = 0;
    std::uint8_t dlc = 0;
    std::uint8_t data[8]{};
    bool extended = false;
};

bool extractRaw(const Signal& sig, const std::uint8_t data[8], std::uint8_t dlc, std::uint64_t& raw);
bool insertRaw(const Signal& sig, std::uint8_t data[8], std::uint8_t dlc, std::uint64_t raw);
bool decodeSignal(const Signal& sig, const std::uint8_t data[8], std::uint8_t dlc, double& value);
bool encodeSignal(const Signal& sig, std::uint8_t data[8], std::uint8_t dlc, double value,
                  std::string* error = nullptr);
std::vector<DecodedSample> decodeFrame(const Database& db, std::uint32_t canId, const std::uint8_t data[8],
                                       std::uint8_t dlc);
bool encodeFrame(const Database& db, std::string_view messageName, std::span<const SignalValue> signals,
                 EncodedFrame& out, std::string* error = nullptr);

}  // namespace photon::dbc
