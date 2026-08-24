#include "photon/dbc.hpp"
#include "test_harness.hpp"

#include <cstdint>
#include <cstring>
#include <string>

void runDbcTests();

namespace {

constexpr const char* kFixture = R"(
VERSION ""

BO_ 256 EngineData: 8 Vector
 SG_ RPM : 0|16@1+ (0.25,0) [0|8000] "rpm" Vector
 SG_ Temp : 16|8@1- (1,-40) [-40|215] "C" Vector

BO_ 512 Status: 8 Vector
 SG_ Flags : 7|16@0+ (1,0) [0|65535] "" Vector
 SG_ Current : 23|16@0- (0.1,0) [-3276.8|3276.7] "A" Vector
)";

}  // namespace

void runDbcTests() {
    std::string error;
    auto db = photon::dbc::parse(kFixture, &error);
    CHECK(db.has_value());
    CHECK(error.empty());
    CHECK(db->messages.size() == 2);
    CHECK(db->messages[0].name == "EngineData");
    CHECK(db->messages[0].canId == 256);
    CHECK(db->messages[0].signals.size() == 2);
    CHECK(db->messages[1].name == "Status");
    CHECK(db->messages[1].signals.size() == 2);

    const auto* rpm = db->findSignal(photon::dbc::signalIdOf("EngineData", "RPM"));
    const auto* temp = db->findSignal(photon::dbc::signalIdOf("EngineData", "Temp"));
    const auto* flags = db->findSignal(photon::dbc::signalIdOf("Status", "Flags"));
    const auto* current = db->findSignal(photon::dbc::signalIdOf("Status", "Current"));
    CHECK(rpm != nullptr);
    CHECK(temp != nullptr);
    CHECK(flags != nullptr);
    CHECK(current != nullptr);
    CHECK(rpm->littleEndian);
    CHECK(!rpm->isSigned);
    CHECK(temp->littleEndian);
    CHECK(temp->isSigned);
    CHECK(!flags->littleEndian);
    CHECK(!flags->isSigned);
    CHECK(!current->littleEndian);
    CHECK(current->isSigned);

    const auto idA = photon::dbc::signalIdOf("EngineData", "RPM");
    const auto idB = photon::dbc::signalIdOf("EngineData", "RPM");
    CHECK(idA == idB);
    CHECK(idA != 0);
    auto db2 = photon::dbc::parse(kFixture);
    CHECK(db2.has_value());
    CHECK(db2->findSignal(idA) != nullptr);
    CHECK(db2->findSignal(idA)->name == "RPM");
    CHECK(db->findSignal(idA)->signalId == db2->findSignal(idA)->signalId);
    CHECK(db->findSignal(photon::dbc::signalIdOf("Status", "Current"))->signalId ==
          db2->findSignal(photon::dbc::signalIdOf("Status", "Current"))->signalId);

    // EngineData: little-endian RPM raw 1000 → 250.0; Temp raw 100 → 60.0
    std::uint8_t engine[8]{};
    engine[0] = 0xE8;
    engine[1] = 0x03;
    engine[2] = 100;
    double rpmV = 0.0;
    double tempV = 0.0;
    CHECK(photon::dbc::decodeSignal(*rpm, engine, 8, rpmV));
    CHECK(photon::dbc::decodeSignal(*temp, engine, 8, tempV));
    CHECK(near(rpmV, 250.0));
    CHECK(near(tempV, 60.0));

    // Signed little-endian 8-bit at start 16: 0xF6 → -10, offset -40 → -50
    engine[2] = 0xF6;
    CHECK(photon::dbc::decodeSignal(*temp, engine, 8, tempV));
    CHECK(near(tempV, -50.0));

    // Status: Motorola 16-bit Flags 0x1234; signed Current 0xFF9C → -100 * 0.1
    std::uint8_t status[8]{};
    status[0] = 0x12;
    status[1] = 0x34;
    status[2] = 0xFF;
    status[3] = 0x9C;
    double flagsV = 0.0;
    double currentV = 0.0;
    CHECK(photon::dbc::decodeSignal(*flags, status, 8, flagsV));
    CHECK(photon::dbc::decodeSignal(*current, status, 8, currentV));
    CHECK(near(flagsV, 4660.0));
    CHECK(near(currentV, -10.0));

    const auto samples = photon::dbc::decodeFrame(*db, 256, engine, 8);
    CHECK(samples.size() == 2);

    photon::dbc::EncodedFrame encoded{};
    const photon::dbc::SignalValue engineVals[] = {{"RPM", 250.0}, {"Temp", 60.0}};
    CHECK(photon::dbc::encodeFrame(*db, "EngineData", engineVals, encoded));
    CHECK(encoded.canId == 256);
    CHECK(encoded.dlc == 8);
    CHECK(!encoded.extended);
    CHECK(encoded.data[0] == 0xE8);
    CHECK(encoded.data[1] == 0x03);
    CHECK(encoded.data[2] == 100);
    double rpmRound = 0.0;
    double tempRound = 0.0;
    CHECK(photon::dbc::decodeSignal(*rpm, encoded.data, encoded.dlc, rpmRound));
    CHECK(photon::dbc::decodeSignal(*temp, encoded.data, encoded.dlc, tempRound));
    CHECK(near(rpmRound, 250.0));
    CHECK(near(tempRound, 60.0));

    const photon::dbc::SignalValue statusVals[] = {{"Flags", 4660.0}, {"Current", -10.0}};
    CHECK(photon::dbc::encodeFrame(*db, "Status", statusVals, encoded));
    CHECK(encoded.canId == 512);
    double flagsRound = 0.0;
    double currentRound = 0.0;
    CHECK(photon::dbc::decodeSignal(*flags, encoded.data, encoded.dlc, flagsRound));
    CHECK(photon::dbc::decodeSignal(*current, encoded.data, encoded.dlc, currentRound));
    CHECK(near(flagsRound, 4660.0));
    CHECK(near(currentRound, -10.0));

    std::string encError;
    CHECK(!photon::dbc::encodeFrame(*db, "MissingMsg", engineVals, encoded, &encError));
    CHECK(!encError.empty());

    const auto cat = db->catalog();
    CHECK(cat.size() == 4);
    CHECK(cat[0].id == idA);
    CHECK(cat[0].message == "EngineData");
    CHECK(cat[0].name == "RPM");
    CHECK(cat[0].unit == "rpm");
}
