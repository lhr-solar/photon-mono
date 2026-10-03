#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace photon::ui {

struct GgImu {
    const char* id = "";
    const char* label = "";
    const char* axRole = "";
    const char* ayRole = "";
    const char* message = "";
    const char* axName = "Accel_X";
    const char* ayName = "Accel_Y";
};

inline constexpr double kGgMaxAbsG = 8.0;
// Display box. Held peak sticks can exceed this; a single 7 g glitch
// must not zoom the 1 g circles down to a speck.
inline constexpr double kGgViewMaxG = 3.5;
inline constexpr int kGgImuCount = 3;
inline constexpr GgImu kGgImus[kGgImuCount] = {
    {"b", "IMU B", "imu_ax", "imu_ay", "IMU_B_Accel", "Accel_X", "Accel_Y"},
    {"fr", "IMU FR", "imu_fr_ax", "imu_fr_ay", "IMU_Accel_FR", "Accel_X", "Accel_Y"},
    {"fl", "IMU FL", "imu_fl_ax", "imu_fl_ay", "IMU_Accel_FL", "Accel_X", "Accel_Y"},
};

int clampGgImuIndex(int index);
int ggImuIndexFromMessage(std::string_view message);
bool drawGgImuCombo(const char* id, int* index, float width = 108.0f);

struct GgPeak {
    bool have = false;
    double x = 0.0;
    double y = 0.0;
    double value = 0.0;
    std::uint64_t timestampMs = 0;
};

struct GgPeaks {
    GgPeak longPos;
    GgPeak longNeg;
    GgPeak latPos;
    GgPeak latNeg;
    GgPeak combined;
};

void findGgPeaks(const std::vector<double>& x, const std::vector<double>& y, GgPeaks* out);
void findGgPeaks(const std::vector<double>& x, const std::vector<double>& y,
                 const std::vector<double>& timestamps, GgPeaks* out);
void mergeGgPeaks(GgPeaks* dst, const GgPeaks& src);
double ggPeakSpan(const GgPeaks& peaks, double floor = 2.0);
double ggViewSpan(const GgPeaks& window, double floor = 2.0);
void drawGgPeaks(const GgPeaks& peaks);
void drawGgNow(double x, double y);

std::string ggPeakSessionKey(const std::string& sessionId, const std::string& sourcePath);

}  // namespace photon::ui
