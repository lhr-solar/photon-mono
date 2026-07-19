module;
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
export module linePlot;
/* example of what it may look like:
       │                                                          
 15.00 ├──╮                        ╭────╮                         
 10.00 │  ╰──╮                   ╭─╯    ╰──╮                      
  5.00 │     ╰╮                ╭─╯         ╰╮                ╭    
  0.00 │      ╰─╮            ╭─╯            ╰─╮            ╭─╯    
 -5.00 │        ╰─╮         ╭╯                ╰─╮         ╭╯      
-10.00 │          ╰─╮    ╭──╯                   ╰─╮    ╭──╯       
-15.00 │            ╰────╯                        ╰────╯          
       │                                                          
       └──────────────────────────────────────────────────────────
  Time  1  2  3  4  5  6  7  8  9  10 ...                           
*/

export struct linePlotConfig{
    int width = 56;
    int height = 10;
    std::string xAxisName = "x";
    std::string yAxisName = "y";
    std::pair<double, double> xRange = {0.0, 1.0};
    std::pair<double, double> yRange = {0.0, 1.0};
    std::vector<double> xData;
    std::vector<double> yData;
};

export struct linePlot{
    void configure(const linePlotConfig& config) {
        config_ = normalizedConfig(config);
    }

    std::vector<std::string> render() const {
        return renderFrame();
    }

  private:
    enum Direction {
        left = 1 << 0,
        right = 1 << 1,
        up = 1 << 2,
        down = 1 << 3,
    };

    struct CanvasPoint {
        int x;
        int y;
    };

    static linePlotConfig normalizedConfig(linePlotConfig config) {
        if (config.width < 6) config.width = 6;
        if (config.height < 4) config.height = 4;

        if (config.xRange.first > config.xRange.second)
            std::swap(config.xRange.first, config.xRange.second);
        if (config.yRange.first > config.yRange.second)
            std::swap(config.yRange.first, config.yRange.second);

        const std::size_t pairedSize = std::min(config.xData.size(), config.yData.size());
        config.xData.resize(pairedSize);
        config.yData.resize(pairedSize);

        return config;
    }

    static std::string formatTick(double value) {
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(2) << value;
        return stream.str();
    }

    static std::string fitLabel(std::string text, int width) {
        if (width <= 0) return "";
        if (static_cast<int>(text.size()) <= width) return text;
        if (width == 1) return text.substr(0, 1);
        return text.substr(0, static_cast<std::size_t>(width - 1)) + ".";
    }

    static std::string formatTickToWidth(double value, int width) {
        if (width <= 0) return "";
        for (int precision = 2; precision >= 0; --precision) {
            std::ostringstream stream;
            stream << std::fixed << std::setprecision(precision) << value;
            const std::string text = stream.str();
            if (static_cast<int>(text.size()) <= width) return text;
        }

        std::ostringstream scientific;
        scientific << std::scientific << std::setprecision(1) << value;
        return fitLabel(scientific.str(), width);
    }

    static int labelWidth(const linePlotConfig& config) {
        const int maxWidth = std::max(
            static_cast<int>(formatTick(config.yRange.first).size()),
            static_cast<int>(formatTick(config.yRange.second).size()));
        const int available = std::max(0, config.width - 5);
        return std::min(maxWidth, available);
    }

    double normalizedX(double x) const {
        const double span = config_.xRange.second - config_.xRange.first;
        if (span <= 0.0) return 0.0;
        return (x - config_.xRange.first) / span;
    }

    double normalizedY(double y) const {
        const double span = config_.yRange.second - config_.yRange.first;
        if (span <= 0.0) return 0.0;
        return (y - config_.yRange.first) / span;
    }

    CanvasPoint mapPoint(double x, double y, int plotWidth, int plotHeight) const {
        const double xRatio = std::clamp(normalizedX(x), 0.0, 1.0);
        const double yRatio = std::clamp(normalizedY(y), 0.0, 1.0);
        const int px = static_cast<int>(std::lround(
            xRatio * static_cast<double>(std::max(0, plotWidth - 1))));
        const int py = static_cast<int>(std::lround(
            (1.0 - yRatio) * static_cast<double>(std::max(0, plotHeight - 1))));
        return {px, py};
    }

    static void stamp(std::vector<std::vector<unsigned char>>& grid, int x, int y,
                      unsigned char mask) {
        if (y < 0 || y >= static_cast<int>(grid.size())) return;
        if (x < 0 || x >= static_cast<int>(grid[0].size())) return;
        grid[y][x] |= mask;
    }

    static std::string glyphForMask(unsigned char mask) {
        switch (mask) {
            case 0:
                return " ";
            case left:
            case right:
            case static_cast<unsigned char>(left | right):
                return "─";
            case up:
            case down:
            case static_cast<unsigned char>(up | down):
                return "│";
            case static_cast<unsigned char>(right | down):
                return "╭";
            case static_cast<unsigned char>(left | down):
                return "╮";
            case static_cast<unsigned char>(right | up):
                return "╰";
            case static_cast<unsigned char>(left | up):
                return "╯";
            case static_cast<unsigned char>(left | right | down):
                return "┬";
            case static_cast<unsigned char>(left | right | up):
                return "┴";
            case static_cast<unsigned char>(left | up | down):
                return "┤";
            case static_cast<unsigned char>(right | up | down):
                return "├";
            default:
                return "┼";
        }
    }

    static void connect(std::vector<std::vector<unsigned char>>& grid, int x, int y,
                        int nx, int ny) {
        if (nx == x + 1 && ny == y) {
            stamp(grid, x, y, right);
            stamp(grid, nx, ny, left);
            return;
        }
        if (nx == x - 1 && ny == y) {
            stamp(grid, x, y, left);
            stamp(grid, nx, ny, right);
            return;
        }
        if (nx == x && ny == y + 1) {
            stamp(grid, x, y, down);
            stamp(grid, nx, ny, up);
            return;
        }
        if (nx == x && ny == y - 1) {
            stamp(grid, x, y, up);
            stamp(grid, nx, ny, down);
        }
    }

    static void drawSegment(std::vector<std::vector<unsigned char>>& grid, CanvasPoint from,
                            CanvasPoint to) {
        int x = from.x;
        int y = from.y;
        const int dx = std::abs(to.x - from.x);
        const int sx = from.x < to.x ? 1 : -1;
        const int dy = -std::abs(to.y - from.y);
        const int sy = from.y < to.y ? 1 : -1;
        int err = dx + dy;

        while (true) {
            if (x == to.x && y == to.y) break;
            const int twiceErr = 2 * err;
            const int previousX = x;
            const int previousY = y;
            bool moved = false;
            if (twiceErr >= dy) {
                err += dy;
                x += sx;
                connect(grid, previousX, previousY, x, y);
                moved = true;
            }
            if (twiceErr <= dx) {
                const int startX = x;
                const int startY = y;
                err += dx;
                y += sy;
                if (moved) {
                    connect(grid, startX, startY, x, y);
                } else {
                    connect(grid, previousX, previousY, x, y);
                }
            }
        }
    }

    std::vector<std::string> renderFrame() const {
        const int yLabelWidth = labelWidth(config_);
        const int plotHeight = std::max(1, config_.height - 2);
        const int plotWidth = std::max(1, config_.width - yLabelWidth - 1);
        std::vector<std::vector<unsigned char>> plot(
            static_cast<std::size_t>(plotHeight),
            std::vector<unsigned char>(static_cast<std::size_t>(plotWidth), 0));

        for (std::size_t i = 1; i < config_.xData.size(); ++i) {
            drawSegment(plot,
                        mapPoint(config_.xData[i - 1], config_.yData[i - 1], plotWidth, plotHeight),
                        mapPoint(config_.xData[i], config_.yData[i], plotWidth, plotHeight));
        }

        std::vector<std::string> lines;
        lines.reserve(static_cast<std::size_t>(config_.height));

        for (int row = 0; row < plotHeight; ++row) {
            const double ratio = (plotHeight == 1)
                                     ? 0.0
                                     : static_cast<double>(row)
                                           / static_cast<double>(plotHeight - 1);
            const double value =
                config_.yRange.second - ratio * (config_.yRange.second - config_.yRange.first);
            std::string line;
            line.reserve(static_cast<std::size_t>(config_.width) * 3);

            const std::string yLabel = formatTickToWidth(value, yLabelWidth);
            line.append(static_cast<std::size_t>(std::max(0, yLabelWidth - static_cast<int>(yLabel.size()))), ' ');
            line += yLabel;
            line += "│";

            for (unsigned char cell : plot[static_cast<std::size_t>(row)])
                line += glyphForMask(cell);

            lines.push_back(line);
        }

        std::string axisRule(static_cast<std::size_t>(yLabelWidth), ' ');
        axisRule += "└";
        for (int col = 0; col < plotWidth; ++col) axisRule += "─";
        lines.push_back(axisRule);

        std::string axisLine(static_cast<std::size_t>(config_.width), ' ');
        const std::string xAxisName = fitLabel(config_.xAxisName, yLabelWidth);
        for (std::size_t i = 0; i < xAxisName.size(); ++i) axisLine[i] = xAxisName[i];

        if (plotWidth > 0) {
            const std::string minTick = formatTickToWidth(config_.xRange.first, plotWidth);
            const std::string maxTick = formatTickToWidth(config_.xRange.second, plotWidth);
            const int plotOffset = yLabelWidth + 1;
            for (std::size_t i = 0; i < minTick.size() && (plotOffset + static_cast<int>(i)) < config_.width; ++i)
                axisLine[static_cast<std::size_t>(plotOffset + static_cast<int>(i))] = minTick[i];
            const int maxStart = std::max(plotOffset,
                yLabelWidth + 1 + plotWidth - static_cast<int>(maxTick.size()));
            for (std::size_t i = 0; i < maxTick.size() && (maxStart + static_cast<int>(i)) < config_.width; ++i)
                axisLine[static_cast<std::size_t>(maxStart + static_cast<int>(i))] = maxTick[i];
        }

        lines.push_back(axisLine);
        return lines;
    }

    linePlotConfig config_{};
};
