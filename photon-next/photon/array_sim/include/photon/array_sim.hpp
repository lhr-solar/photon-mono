#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace photon::array_sim {

struct Request {
    std::string stlPath;
    double azimuthDeg = 125.75;
    double elevationDeg = 72.05125;
    double dniWm2 = 850.0;
    double dhiWm2 = 110.0;
    std::uint32_t directSamplesPerAxis = 3;
    std::uint32_t hemisphereSamples = 64;
    std::uint32_t reflectionDepth = 2;
};

struct CellResult {
    std::uint32_t row = 0;
    std::uint32_t column = 0;
    std::uint32_t moduleId = 0;
    double directWm2 = 0.0;
    double diffuseWm2 = 0.0;
    double reflectedWm2 = 0.0;
    double totalWm2 = 0.0;
    double activeAreaM2 = 0.0;
    double incidentW = 0.0;
    double directVisibility = 0.0;
};

struct Result {
    std::string error;
    std::size_t triangleCount = 0;
    std::size_t cellCount = 0;
    std::size_t twoByThreeModules = 0;
    std::size_t oneByThreeModules = 0;
    double activeAreaM2 = 0.0;
    double meanIrradianceWm2 = 0.0;
    double incidentPowerW = 0.0;
    std::vector<CellResult> cells;
};

bool available();
Result run(const Request& request);

}  // namespace photon::array_sim
