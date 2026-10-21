#include "photon/array_sim.hpp"
#include "photon/control.hpp"
#include "test_harness.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace {

void writeU32(std::ofstream& out, std::uint32_t value) {
    const std::array<char, 4> bytes{static_cast<char>(value), static_cast<char>(value >> 8),
                                    static_cast<char>(value >> 16), static_cast<char>(value >> 24)};
    out.write(bytes.data(), bytes.size());
}

void writeF32(std::ofstream& out, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    writeU32(out, bits);
}

void writeTriangle(std::ofstream& out, const std::array<std::array<float, 3>, 3>& points) {
    for (int i = 0; i < 3; ++i) writeF32(out, 0.0f);
    for (const auto& point : points) {
        for (float coordinate : point) writeF32(out, coordinate);
    }
    const std::array<char, 2> attribute{};
    out.write(attribute.data(), attribute.size());
}

void testProtocol() {
    photon::ctrl::Message request;
    request.type = photon::ctrl::Type::ArraySimRequest;
    request.path = "C:\\solar\\car.stl";
    request.arrayAzimuthDeg = 132.0;
    request.arrayDniWm2 = 820.0;
    const auto decoded = photon::ctrl::parse(photon::ctrl::serialize(request));
    CHECK(decoded.has_value());
    CHECK(decoded->type == photon::ctrl::Type::ArraySimRequest);
    CHECK(decoded->path == request.path);
    CHECK(near(decoded->arrayAzimuthDeg, 132.0));
    CHECK(near(decoded->arrayDniWm2, 820.0));

    photon::ctrl::Message result;
    result.type = photon::ctrl::Type::ArraySimResult;
    result.arrayCellCount = 1;
    result.arrayIncidentPowerW = 12.24;
    result.arrayCells.push_back({0, 1, 2, 700.0, 80.0, 20.0, 800.0, 0.0153, 12.24, 0.5});
    const auto decodedResult = photon::ctrl::parse(photon::ctrl::serialize(result));
    CHECK(decodedResult.has_value());
    CHECK(decodedResult->type == photon::ctrl::Type::ArraySimResult);
    CHECK(decodedResult->arrayCellCount == 1);
    CHECK(decodedResult->arrayCells.size() == 1);
    CHECK(near(decodedResult->arrayCells[0].incidentW, 12.24));
    CHECK(near(decodedResult->arrayCells[0].directVisibility, 0.5));
}

void testFlatShell() {
    photon::array_sim::Request input;
    CHECK(!photon::array_sim::run(input).error.empty());
    if (!photon::array_sim::available()) return;

    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("photon-array-plane-" + std::to_string(nonce) + ".stl");
    {
        std::ofstream out(path, std::ios::binary);
        const std::array<char, 80> header{};
        out.write(header.data(), header.size());
        writeU32(out, 2);
        // Raw STL coordinates: the existing ArrayShading transform rotates X
        // 90 degrees and translates Y/Z, yielding a horizontal world-space plane.
        writeTriangle(out, {{{-0.68f, 0.3347f, 2.2437f}, {0.68f, 0.3347f, 2.2437f},
                             {-0.68f, 0.3347f, 0.8037f}}});
        writeTriangle(out, {{{0.68f, 0.3347f, 2.2437f}, {0.68f, 0.3347f, 0.8037f},
                             {-0.68f, 0.3347f, 0.8037f}}});
    }
    input.stlPath = path.string();
    input.azimuthDeg = 0.0;
    input.elevationDeg = 90.0;
    input.dniWm2 = 800.0;
    input.dhiWm2 = 0.0;
    input.directSamplesPerAxis = 1;
    input.hemisphereSamples = 1;
    input.reflectionDepth = 0;
    const auto result = photon::array_sim::run(input);
    std::filesystem::remove(path);
    CHECK(result.error.empty());
    CHECK(result.triangleCount == 2);
    CHECK(result.cellCount > 0);
    CHECK(near(result.meanIrradianceWm2, 800.0, 0.1));
    CHECK(near(result.incidentPowerW, 800.0 * result.activeAreaM2, 0.1));
    CHECK(result.cells.size() == result.cellCount);
}

}  // namespace

int main() {
    testProtocol();
    testFlatShell();
    return gFailed == 0 ? 0 : 1;
}
