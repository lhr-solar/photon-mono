#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace photon::pcan {

using Handle = std::uint16_t;
using Status = std::uint32_t;
using Baudrate = std::uint16_t;

enum class UsbChannel : Handle {
    Bus1 = 0x51,
    Bus2 = 0x52,
    Bus3 = 0x53,
    Bus4 = 0x54,
    Bus5 = 0x55,
    Bus6 = 0x56,
    Bus7 = 0x57,
    Bus8 = 0x58,
};

enum class BaudCode : Baudrate {
    Baud125K = 0x031C,
    Baud250K = 0x011C,
    Baud500K = 0x001C,
    Baud1M = 0x0014,
};

enum class MessageType : std::uint8_t {
    Standard = 0x00,
    Rtr = 0x01,
    Extended = 0x02,
    Status = 0x80,
};

inline constexpr Status kOk = 0x00000;
inline constexpr Status kQrcvEmpty = 0x00020;
inline constexpr std::uint8_t kParamListenOnly = 0x08;
inline constexpr std::uint8_t kParamBusoffAutoreset = 0x07;
inline constexpr Handle kUsbBus1 = static_cast<Handle>(UsbChannel::Bus1);

inline constexpr const char* kDllName = "PCANBasic.dll";
inline constexpr const char* kUnavailableMessage = "PCANBasic.dll not found or device unavailable";

struct Msg {
    std::uint32_t id = 0;
    std::uint8_t msgType = 0;
    std::uint8_t len = 0;
    std::uint8_t data[8]{};
};

struct Timestamp {
    std::uint32_t millis = 0;
    std::uint16_t millisOverflow = 0;
    std::uint16_t micros = 0;
};

struct Config {
    int channel = 1;
    int baud = 250000;
    bool listenOnly = true;
};

std::optional<Handle> channelHandle(int channel);
std::optional<Baudrate> baudCode(int baud);

// Legacy PCAN millis + (overflow << 32). toUnixMs anchors that timeline to a wall-clock sample.
std::uint64_t hardwareTimestampMs(const Timestamp& ts);
std::uint64_t toUnixMs(const Timestamp& ts, std::uint64_t anchorUnixMs, std::uint64_t anchorHwMs);

class Library {
public:
    Library() = default;
    Library(Library&& other) noexcept;
    Library& operator=(Library&& other) noexcept;
    ~Library();
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;

    static std::optional<Library> load(std::string_view dllName, std::string& error);

    bool available() const { return module_ != nullptr; }
    Status initialize(Handle channel, Baudrate baud) const;
    Status uninitialize(Handle channel) const;
    Status read(Handle channel, Msg& msg, Timestamp& ts) const;
    Status write(Handle channel, const Msg& msg) const;
    Status setValue(Handle channel, std::uint8_t param, void* buf, std::uint32_t len) const;
    bool hasSetValue() const { return setValue_ != nullptr; }
    std::string statusText(Status status) const;

private:
    void* module_ = nullptr;
    void* initialize_ = nullptr;
    void* uninitialize_ = nullptr;
    void* read_ = nullptr;
    void* write_ = nullptr;
    void* getErrorText_ = nullptr;
    void* setValue_ = nullptr;

    void reset() noexcept;
};

class Device {
public:
    Device() = default;
    Device(Device&& other) noexcept;
    Device& operator=(Device&& other) noexcept;
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    static std::optional<Device> open(const Config& cfg, std::string& error);
    void close();
    bool valid() const { return initialized_; }
    const Config& config() const { return config_; }

    Status read(Msg& msg, Timestamp& ts) const;
    Status write(const Msg& msg) const;

private:
    Library lib_;
    Handle channel_ = 0;
    Config config_{};
    bool initialized_ = false;
};

}  // namespace photon::pcan
