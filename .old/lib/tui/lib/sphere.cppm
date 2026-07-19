module;
#include <algorithm>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
export module sphere;

export struct sphereConfig {
  int width = 16;
  int height = 8;
  double speed = 0.20;  // Revolutions per second.
  double angle = 0.0;   // Tilt angle in degrees.
};

export struct Sphere {
  void configure(const sphereConfig& config) {
    const auto now = Clock::now();
    if (!initialized_){
      std::setlocale(LC_ALL, "");
      lastRender_ = now;
      initialized_ = true;
    } else { advanceTo(now); }
    config_ = normalizedConfig(config);
  }

  std::vector<std::string> render() {
    if(!initialized_)configure(sphereConfig{});
    const auto now = Clock::now();
    advanceTo(now);
    return renderFrame();
  }

 private:
  using Clock = std::chrono::steady_clock;

  static constexpr double kPi = 3.14159265358979323846;
  static constexpr double kTwoPi = 2.0 * kPi;
  static constexpr int kMovingMeridians = 7;

  struct Vec2 {
    double x;
    double y;
  };

  struct Vec3 {
    double x;
    double y;
    double z;
  };

  struct DotGrid {
    int width;
    int height;
    std::vector<double> values;
  };

  static Vec3 rotateY(const Vec3& v, double angle) {
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    return {c * v.x + s * v.z, v.y, -s * v.x + c * v.z};
  }

  static Vec3 rotateZ(const Vec3& v, double angle) {
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    return {c * v.x - s * v.y, s * v.x + c * v.y, v.z};
  }

  static Vec3 orientPoint(const Vec3& v, double spin, double tilt) {
    return rotateZ(rotateY(v, spin), tilt);
  }

  static double wrapAngle(double angle) {
    while (angle <= -kPi) angle += kTwoPi;
    while (angle > kPi) angle -= kTwoPi;
    return angle;
  }

  static sphereConfig normalizedConfig(sphereConfig config) {
    if (config.width < 8) config.width = 8;
    if (config.height < 8) config.height = 8;
    return config;
  }

  void advanceTo(Clock::time_point now) {
    const std::chrono::duration<double> delta = now - lastRender_;
    lastRender_ = now;
    spinRadians_ = wrapAngle(spinRadians_ + delta.count() * config_.speed * kTwoPi);
  }

  DotGrid makeGrid() const {
    return {config_.width * 2, config_.height * 4,
            std::vector<double>(config_.width * 2 * config_.height * 4, 0.0)};
  }

  static Vec2 mapToGrid(double x, double y, const DotGrid& grid) {
    const double rx = (grid.width - 4) * 0.5;
    const double ry = (grid.height - 4) * 0.5;
    return {grid.width * 0.5 + x * rx, grid.height * 0.5 - y * ry};
  }

  static void stampDot(DotGrid& grid, int x, int y, double strength) {
    if(x < 0 || x >= grid.width || y < 0 || y >= grid.height)return;
    const int index = y * grid.width + x;
    grid.values[index] = std::max(grid.values[index], strength);
  }

  static void drawSegment(DotGrid& grid, const Vec2& a, const Vec2& b, double strength) {
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const int steps = std::max(
        2, static_cast<int>(std::ceil(std::max(std::abs(dx), std::abs(dy)) * 1.8)));

    for (int i = 0; i <= steps; ++i) {
      const double t = static_cast<double>(i) / steps;
      const int xi = static_cast<int>(std::round(a.x + dx * t));
      const int yi = static_cast<int>(std::round(a.y + dy * t));
      stampDot(grid, xi, yi, strength);
    }
  }

  static void drawPolyline(DotGrid& grid, const std::vector<Vec2>& points, double strength) {
    if(points.size() < 2) return;
    for(size_t i = 1; i < points.size(); ++i)
      drawSegment(grid, points[i - 1], points[i], strength);
  }

  static std::vector<Vec2> makeCircle(const DotGrid& grid) {
    std::vector<Vec2> points;
    const int steps = 128;
    points.reserve(steps + 1);

    for (int i = 0; i <= steps; ++i) {
      const double t = kTwoPi * i / steps;
      points.push_back(mapToGrid(std::cos(t), std::sin(t), grid));
    }

    return points;
  }

  std::vector<std::vector<Vec2>> makeVisibleParallel(const DotGrid& grid, double latitude) const {
    std::vector<std::vector<Vec2>> curves;
    std::vector<Vec2> current;
    const int steps = 120;

    for (int i = 0; i <= steps; ++i) {
      const double lon = kTwoPi * i / steps;
      const Vec3 point = orientPoint(
          {std::cos(latitude) * std::cos(lon), std::sin(latitude),
           std::cos(latitude) * std::sin(lon)},
          spinRadians_, tiltRadians());

      if(point.z < 0.0){
        if(current.size() > 1) curves.push_back(current);
        current.clear();
        continue;
      }

      current.push_back(mapToGrid(point.x, point.y, grid));
    }

    if(current.size() > 1)curves.push_back(current);

    return curves;
  }

  std::vector<std::vector<Vec2>> makeVisibleMeridian(const DotGrid& grid,
                                                     double longitude) const {
    std::vector<std::vector<Vec2>> curves;
    std::vector<Vec2> current;
    const int steps = 96;

    for (int i = 0; i <= steps; ++i) {
      const double lat = -kPi * 0.5 + kPi * i / steps;
      const Vec3 point = orientPoint(
          {std::cos(lat) * std::cos(longitude), std::sin(lat),
           std::cos(lat) * std::sin(longitude)},
          spinRadians_, tiltRadians());

      if (point.z < 0.0) {
        if(current.size() > 1)curves.push_back(current);
        current.clear();
        continue;
      }

      current.push_back(mapToGrid(point.x, point.y, grid));
    }

    if(current.size() > 1)curves.push_back(current);

    return curves;
  }

  void drawSphereSprite(DotGrid& grid) const {
    drawPolyline(grid, makeCircle(grid), 1.0);

    for (const auto& curve : makeVisibleParallel(grid, 0.0))
      drawPolyline(grid, curve, 0.72);
    for (const auto& curve : makeVisibleParallel(grid, 0.52))
      drawPolyline(grid, curve, 0.46);
    for (const auto& curve : makeVisibleParallel(grid, -0.52))
      drawPolyline(grid, curve, 0.46);

    for (int i = 0; i < kMovingMeridians; ++i) {
      const double longitude = kPi * 0.5 + i * (kTwoPi / kMovingMeridians);
      const double delta = wrapAngle(longitude + spinRadians_ - kPi * 0.5);
      const double frontness = std::max(0.0, std::cos(delta));
      const double weight = 0.24 + 0.58 * std::pow(frontness, 0.65);

      for(const auto& curve : makeVisibleMeridian(grid, longitude))
        drawPolyline(grid, curve, weight);
    }
  }

  static wchar_t brailleChar(const DotGrid& grid, int cellX, int cellY) {
    static constexpr int kBits[4][2] = {
        {0x01, 0x08},
        {0x02, 0x10},
        {0x04, 0x20},
        {0x40, 0x80},
    };

    int mask = 0;
    for (int dy = 0; dy < 4; ++dy) {
      for (int dx = 0; dx < 2; ++dx) {
        const int px = cellX * 2 + dx;
        const int py = cellY * 4 + dy;
        const int index = py * grid.width + px;
        if (grid.values[index] > 0.18) mask |= kBits[dy][dx];
      }
    }

    return static_cast<wchar_t>(0x2800 + mask);
  }

  std::vector<std::string> renderFrame() const {
    DotGrid grid = makeGrid();
    drawSphereSprite(grid);

    std::vector<std::string> lines;
    lines.reserve(config_.height);
    char buffer[8] = {};

    for (int row = 0; row < config_.height; ++row) {
      std::string line;
      for (int col = 0; col < config_.width; ++col) {
        const wchar_t wc = brailleChar(grid, col, row);
        if (wc == 0x2800) {
          line.push_back(' ');
          continue;
        }

        const int written = std::wctomb(buffer, wc);
        if (written > 0) {
          line.append(buffer, buffer + written);
        } else {
          line.push_back('?');
        }
      }
      lines.push_back(line);
    }

    return lines;
  }

  double tiltRadians() const { return config_.angle * kPi / 180.0; }

  sphereConfig config_{};
  double spinRadians_ = 0.0;
  Clock::time_point lastRender_{};
  bool initialized_ = false;
};
