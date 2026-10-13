#include "wire/control.hpp"
#include "hw/pcan.hpp"
#include "test_harness.hpp"

#include <optional>
#include <string>

void runPcanTests();

void runPcanTests() {
    CHECK(photon::pcan::channelHandle(0) == std::nullopt);
    CHECK(photon::pcan::channelHandle(9) == std::nullopt);
    CHECK(photon::pcan::channelHandle(1) == static_cast<photon::pcan::Handle>(0x51));
    CHECK(photon::pcan::channelHandle(8) == static_cast<photon::pcan::Handle>(0x58));
    CHECK(photon::pcan::baudCode(125000) ==
          static_cast<photon::pcan::Baudrate>(photon::pcan::BaudCode::Baud125K));
    CHECK(photon::pcan::baudCode(250000) ==
          static_cast<photon::pcan::Baudrate>(photon::pcan::BaudCode::Baud250K));
    CHECK(photon::pcan::baudCode(500000) ==
          static_cast<photon::pcan::Baudrate>(photon::pcan::BaudCode::Baud500K));
    CHECK(photon::pcan::baudCode(1000000) ==
          static_cast<photon::pcan::Baudrate>(photon::pcan::BaudCode::Baud1M));
    CHECK(photon::pcan::baudCode(250) ==
          static_cast<photon::pcan::Baudrate>(photon::pcan::BaudCode::Baud250K));
    CHECK(photon::pcan::baudCode(123456) == std::nullopt);

    photon::pcan::Timestamp ts{};
    ts.millis = 1000;
    ts.millisOverflow = 1;
    CHECK(photon::pcan::hardwareTimestampMs(ts) == (1000ull + (1ull << 32)));
    CHECK(photon::pcan::toUnixMs(ts, 1'700'000'000'000ull, 1000ull) ==
          1'700'000'000'000ull + (1ull << 32));

    auto defaults = photon::ctrl::parse(R"({"type":"select_source","source":"pcan"})");
    CHECK(defaults.has_value());
    CHECK(defaults->source == photon::ctrl::SourceKind::Pcan);
    CHECK(defaults->channel == 1);
    CHECK(defaults->baud == 250000);
    CHECK(defaults->listenOnly);

    auto nested = photon::ctrl::parse(
        R"({"type":"select_source","source":"pcan","pcan":{"channel":4,"baud":125000,"listen_only":false}})");
    CHECK(nested.has_value());
    CHECK(nested->channel == 4);
    CHECK(nested->baud == 125000);
    CHECK(!nested->listenOnly);

    auto flat = photon::ctrl::parse(
        R"({"type":"select_source","source":"pcan","channel":3,"baud":500000,"listen_only":false})");
    CHECK(flat.has_value());
    CHECK(flat->channel == 3);
    CHECK(flat->baud == 500000);
    CHECK(!flat->listenOnly);

    auto src = photon::ctrl::selectSource(photon::ctrl::SourceKind::Pcan);
    src.channel = 2;
    src.baud = 1000000;
    src.listenOnly = false;
    auto srcBack = photon::ctrl::parse(photon::ctrl::serialize(src));
    CHECK(srcBack.has_value());
    CHECK(srcBack->source == photon::ctrl::SourceKind::Pcan);
    CHECK(srcBack->channel == 2);
    CHECK(srcBack->baud == 1000000);
    CHECK(!srcBack->listenOnly);

    auto tx = photon::ctrl::txCanFrame("EngineData", {{"RPM", 250.0}, {"Temp", 60.0}});
    auto txBack = photon::ctrl::parse(photon::ctrl::serialize(tx));
    CHECK(txBack.has_value());
    CHECK(txBack->type == photon::ctrl::Type::TxCanFrame);
    CHECK(txBack->message == "EngineData");
    CHECK(txBack->txSignals.size() == 2);
    CHECK(txBack->txSignals[0].name == "RPM");
    CHECK(txBack->txSignals[0].value == 250.0);
    CHECK(txBack->txSignals[1].name == "Temp");

    std::string loadError;
    auto missing = photon::pcan::Library::load("PhotonPcanDllDefinitelyMissing.dll", loadError);
    CHECK(!missing.has_value());
    CHECK(loadError == photon::pcan::kUnavailableMessage);

    std::string defaultError;
    auto probe = photon::pcan::Library::load(photon::pcan::kDllName, defaultError);
    if (!probe) {
        CHECK(defaultError == photon::pcan::kUnavailableMessage);
    } else {
        CHECK(probe->available());
    }

    std::string openError;
    auto dev = photon::pcan::Device::open(photon::pcan::Config{}, openError);
    if (!dev) {
        CHECK(!openError.empty());
    } else {
        CHECK(dev->valid());
        dev->close();
        CHECK(!dev->valid());
    }
}
