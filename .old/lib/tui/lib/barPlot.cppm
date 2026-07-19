module;
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
export module barPlot;
/* example of what it may look like
 0.009080 │                      █  
 0.008172 │                      █  
 0.007264 │                   █  █  
 0.006356 │                █  █  █  
 0.005448 │             █  █  █  █  
 0.004540 │             █  █  █  █  
 0.003632 │          █  █  █  █  █  
 0.002724 │    █  █  █  █  █  █  █  
 0.001816 │ █  █  █  █  █  █  █  █  
 0.000908 │ █  █  █  █  █  █  █  █  
          └──────────────────────── 
  threads   1  2  3  4  5  6  7  8  
 */
export struct barPlotConfig{
    int width = 32;
    int height = 10;
    std::string xAxisName = "x";
    std::string yAxisName = "y";
    std::pair<double, double> xRange = {0.0, 1.0};
    std::pair<double, double> yRange = {0.0, 1.0};
    std::vector<double> xData;
    std::vector<double> yData;
};

export struct barPlot{
    void configure(const barPlotConfig& config) {
        config_ = normalizedConfig(config);
    }

    std::vector<std::string> render() const {
        return renderFrame();
    }

  private:
    static barPlotConfig normalizedConfig(barPlotConfig config) {
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

    static int labelWidth(const barPlotConfig& config) {
        const int maxWidth = std::max(
            static_cast<int>(formatTick(config.yRange.first).size()),
            static_cast<int>(formatTick(config.yRange.second).size()));
        const int available = std::max(0, config.width - 5);
        return std::min(maxWidth, available);
    }

    double normalizedY(double y) const {
        const double span = config_.yRange.second - config_.yRange.first;
        if (span <= 0.0) return 0.0;
        return (y - config_.yRange.first) / span;
    }

    std::vector<std::string> renderFrame() const {
        const int yLabelWidth = labelWidth(config_);
        const int plotHeight = std::max(1, config_.height - 2);
        const int plotWidth = std::max(1, config_.width - yLabelWidth - 1);
        std::vector<std::vector<std::string>> plot(
            static_cast<std::size_t>(plotHeight),
            std::vector<std::string>(static_cast<std::size_t>(plotWidth), " "));

        const std::size_t barCount = std::min(config_.xData.size(), config_.yData.size());
        if (barCount > 0) {
            for (std::size_t i = 0; i < barCount; ++i) {
                const int x = static_cast<int>(
                    std::lround((static_cast<double>(i) + 0.5)
                                * static_cast<double>(plotWidth)
                                / static_cast<double>(barCount) - 0.5));
                const double ratio = std::clamp(normalizedY(config_.yData[i]), 0.0, 1.0);
                const int filled = static_cast<int>(
                    std::lround(ratio * static_cast<double>(plotHeight)));

                for (int row = plotHeight - 1; row >= std::max(0, plotHeight - filled); --row) {
                    if (x >= 0 && x < plotWidth)
                        plot[static_cast<std::size_t>(row)][static_cast<std::size_t>(x)] = "█";
                }
            }
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
            line.reserve(static_cast<std::size_t>(config_.width));

            const std::string yLabel = formatTickToWidth(value, yLabelWidth);
            line.append(static_cast<std::size_t>(
                            std::max(0, yLabelWidth - static_cast<int>(yLabel.size()))),
                        ' ');
            line += yLabel;
            line += "│";
            for (const std::string& cell : plot[static_cast<std::size_t>(row)]) line += cell;
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

            const int maxStart = std::max(
                plotOffset, yLabelWidth + 1 + plotWidth - static_cast<int>(maxTick.size()));
            for (std::size_t i = 0; i < maxTick.size() && (maxStart + static_cast<int>(i)) < config_.width; ++i)
                axisLine[static_cast<std::size_t>(maxStart + static_cast<int>(i))] = maxTick[i];
        }

        lines.push_back(axisLine);
        return lines;
    }

    barPlotConfig config_{};
};
