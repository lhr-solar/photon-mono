#include "photon/pcan.hpp"

#include <cstdio>
#include <cstring>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace photon::pcan {
namespace {

#if defined(_WIN32)
using InitializeFn = Status(WINAPI*)(Handle, Baudrate, std::uint8_t, std::uint32_t, std::uint16_t);
using UninitializeFn = Status(WINAPI*)(Handle);
using ReadFn = Status(WINAPI*)(Handle, Msg*, Timestamp*);
using WriteFn = Status(WINAPI*)(Handle, Msg*);
using GetErrorTextFn = Status(WINAPI*)(Status, std::uint16_t, char*);
using SetValueFn = Status(WINAPI*)(Handle, std::uint8_t, void*, std::uint32_t);

template <typename Fn>
Fn asFn(void* p) {
    return reinterpret_cast<Fn>(p);
}
#endif

void failUnavailable(std::string& error) {
    error = kUnavailableMessage;
}

}  // namespace

std::optional<Handle> channelHandle(int channel) {
    if (channel < 1 || channel > 8) {
        return std::nullopt;
    }
    return static_cast<Handle>(static_cast<int>(kUsbBus1) + channel - 1);
}

std::optional<Baudrate> baudCode(int baud) {
    switch (baud) {
        case 125:
        case 125000:
            return static_cast<Baudrate>(BaudCode::Baud125K);
        case 250:
        case 250000:
            return static_cast<Baudrate>(BaudCode::Baud250K);
        case 500:
        case 500000:
            return static_cast<Baudrate>(BaudCode::Baud500K);
        case 1000:
        case 1000000:
            return static_cast<Baudrate>(BaudCode::Baud1M);
        default:
            return std::nullopt;
    }
}

std::uint64_t hardwareTimestampMs(const Timestamp& ts) {
    return static_cast<std::uint64_t>(ts.millis) +
           (static_cast<std::uint64_t>(ts.millisOverflow) << 32);
}

std::uint64_t toUnixMs(const Timestamp& ts, std::uint64_t anchorUnixMs, std::uint64_t anchorHwMs) {
    const std::uint64_t hw = hardwareTimestampMs(ts);
    if (hw >= anchorHwMs) {
        return anchorUnixMs + (hw - anchorHwMs);
    }
    if (anchorHwMs - hw > anchorUnixMs) {
        return 0;
    }
    return anchorUnixMs - (anchorHwMs - hw);
}

Library::Library(Library&& other) noexcept {
    *this = std::move(other);
}

Library& Library::operator=(Library&& other) noexcept {
    if (this != &other) {
        reset();
        module_ = other.module_;
        initialize_ = other.initialize_;
        uninitialize_ = other.uninitialize_;
        read_ = other.read_;
        write_ = other.write_;
        getErrorText_ = other.getErrorText_;
        setValue_ = other.setValue_;
        other.module_ = nullptr;
        other.initialize_ = nullptr;
        other.uninitialize_ = nullptr;
        other.read_ = nullptr;
        other.write_ = nullptr;
        other.getErrorText_ = nullptr;
        other.setValue_ = nullptr;
    }
    return *this;
}

Library::~Library() {
    reset();
}

void Library::reset() noexcept {
#if defined(_WIN32)
    if (module_ != nullptr) {
        FreeLibrary(static_cast<HMODULE>(module_));
    }
#endif
    module_ = nullptr;
    initialize_ = nullptr;
    uninitialize_ = nullptr;
    read_ = nullptr;
    write_ = nullptr;
    getErrorText_ = nullptr;
    setValue_ = nullptr;
}

std::optional<Library> Library::load(std::string_view dllName, std::string& error) {
    Library lib;
    const std::string name = dllName.empty() ? std::string(kDllName) : std::string(dllName);
#if defined(_WIN32)
    const UINT previous = SetErrorMode(SEM_FAILCRITICALERRORS);
    HMODULE module = LoadLibraryA(name.c_str());
    SetErrorMode(previous);
    if (module == nullptr) {
        failUnavailable(error);
        return std::nullopt;
    }
    lib.module_ = module;
    lib.initialize_ = reinterpret_cast<void*>(GetProcAddress(module, "CAN_Initialize"));
    lib.uninitialize_ = reinterpret_cast<void*>(GetProcAddress(module, "CAN_Uninitialize"));
    lib.read_ = reinterpret_cast<void*>(GetProcAddress(module, "CAN_Read"));
    lib.write_ = reinterpret_cast<void*>(GetProcAddress(module, "CAN_Write"));
    lib.getErrorText_ = reinterpret_cast<void*>(GetProcAddress(module, "CAN_GetErrorText"));
    lib.setValue_ = reinterpret_cast<void*>(GetProcAddress(module, "CAN_SetValue"));
    if (lib.initialize_ == nullptr || lib.uninitialize_ == nullptr || lib.read_ == nullptr ||
        lib.write_ == nullptr) {
        failUnavailable(error);
        return std::nullopt;
    }
    return lib;
#else
    (void)name;
    failUnavailable(error);
    return std::nullopt;
#endif
}

Status Library::initialize(Handle channel, Baudrate baud) const {
#if defined(_WIN32)
    if (initialize_ == nullptr) {
        return 0xFFFFFFFFu;
    }
    return asFn<InitializeFn>(initialize_)(channel, baud, 0, 0, 0);
#else
    (void)channel;
    (void)baud;
    return 0xFFFFFFFFu;
#endif
}

Status Library::uninitialize(Handle channel) const {
#if defined(_WIN32)
    if (uninitialize_ == nullptr) {
        return 0xFFFFFFFFu;
    }
    return asFn<UninitializeFn>(uninitialize_)(channel);
#else
    (void)channel;
    return 0xFFFFFFFFu;
#endif
}

Status Library::read(Handle channel, Msg& msg, Timestamp& ts) const {
#if defined(_WIN32)
    if (read_ == nullptr) {
        return 0xFFFFFFFFu;
    }
    return asFn<ReadFn>(read_)(channel, &msg, &ts);
#else
    (void)channel;
    (void)msg;
    (void)ts;
    return kQrcvEmpty;
#endif
}

Status Library::write(Handle channel, const Msg& msg) const {
#if defined(_WIN32)
    if (write_ == nullptr) {
        return 0xFFFFFFFFu;
    }
    Msg copy = msg;
    return asFn<WriteFn>(write_)(channel, &copy);
#else
    (void)channel;
    (void)msg;
    return 0xFFFFFFFFu;
#endif
}

Status Library::setValue(Handle channel, std::uint8_t param, void* buf, std::uint32_t len) const {
#if defined(_WIN32)
    if (setValue_ == nullptr) {
        return 0xFFFFFFFFu;
    }
    return asFn<SetValueFn>(setValue_)(channel, param, buf, len);
#else
    (void)channel;
    (void)param;
    (void)buf;
    (void)len;
    return 0xFFFFFFFFu;
#endif
}

std::string Library::statusText(Status status) const {
#if defined(_WIN32)
    if (getErrorText_ != nullptr) {
        char text[256]{};
        if (asFn<GetErrorTextFn>(getErrorText_)(status, 0, text) == kOk && text[0] != '\0') {
            return text;
        }
    }
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "PCAN status 0x%X", static_cast<unsigned>(status));
    return buf;
}

Device::Device(Device&& other) noexcept {
    *this = std::move(other);
}

Device& Device::operator=(Device&& other) noexcept {
    if (this != &other) {
        close();
        lib_ = std::move(other.lib_);
        channel_ = other.channel_;
        config_ = other.config_;
        initialized_ = other.initialized_;
        other.channel_ = 0;
        other.initialized_ = false;
    }
    return *this;
}

Device::~Device() {
    close();
}

void Device::close() {
    if (initialized_) {
        lib_.uninitialize(channel_);
        initialized_ = false;
    }
    lib_ = Library{};
    channel_ = 0;
}

std::optional<Device> Device::open(const Config& cfg, std::string& error) {
    const auto handle = channelHandle(cfg.channel);
    const auto baud = baudCode(cfg.baud);
    if (!handle || !baud) {
        error = "invalid pcan channel or baud";
        return std::nullopt;
    }

    auto lib = Library::load(kDllName, error);
    if (!lib) {
        return std::nullopt;
    }

    if (cfg.listenOnly && lib->hasSetValue()) {
        std::uint32_t enabled = 1;
        lib->setValue(*handle, kParamListenOnly, &enabled, sizeof(enabled));
    }

    const Status initStatus = lib->initialize(*handle, *baud);
    if (initStatus != kOk) {
        failUnavailable(error);
        return std::nullopt;
    }

    if (lib->hasSetValue()) {
        std::uint32_t enabled = 1;
        lib->setValue(*handle, kParamBusoffAutoreset, &enabled, sizeof(enabled));
    }

    Device dev;
    dev.lib_ = std::move(*lib);
    dev.channel_ = *handle;
    dev.config_ = cfg;
    dev.initialized_ = true;
    return dev;
}

Status Device::read(Msg& msg, Timestamp& ts) const {
    if (!initialized_) {
        return kQrcvEmpty;
    }
    return lib_.read(channel_, msg, ts);
}

Status Device::write(const Msg& msg) const {
    if (!initialized_) {
        return 0xFFFFFFFFu;
    }
    return lib_.write(channel_, msg);
}

}  // namespace photon::pcan
