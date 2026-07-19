module;
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>
export module progressBar;

/* example of progress bars:
 █████░░░░░░░░░░░░░░░░░░░░░░░░░

 ███████████████░░░░░░░░░░░░░░░

 ████████████████████████░░░░░░
*/

export struct progressBarConfig{
    int width = 32;
    int height = 1;
    std::pair<double, double> range = {0.0, 100.0};
    double currentValue = 0.0;
};

export struct progressBar{
    void configure(const progressBarConfig& config) {
        config_ = normalizedConfig(config);
    }

    std::vector<std::string> render() const {
        const std::string line = renderLine();
        return std::vector<std::string>(static_cast<std::size_t>(config_.height), line);
    }

  private:
    static progressBarConfig normalizedConfig(progressBarConfig config) {
        if (config.width < 1) config.width = 1;
        if (config.height < 1) config.height = 1;

        if (config.range.first > config.range.second) {
            std::swap(config.range.first, config.range.second);
        }

        config.currentValue = std::clamp(
            config.currentValue, config.range.first, config.range.second);

        return config;
    }

    double normalizedValue() const {
        const double span = config_.range.second - config_.range.first;
        if (span <= 0.0) return 1.0;
        return (config_.currentValue - config_.range.first) / span;
    }

    std::string renderLine() const {
        const double fraction = std::clamp(normalizedValue(), 0.0, 1.0);
        const int filledCells = static_cast<int>(
            std::round(fraction * static_cast<double>(config_.width)));

        std::string line;
        line.reserve(static_cast<std::size_t>(config_.width) * 3);

        for (int i = 0; i < config_.width; ++i)
            line += (i < filledCells) ? "█" : "░";

        return line;
    }

    progressBarConfig config_{};
};
