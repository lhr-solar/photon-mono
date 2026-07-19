module;
#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>
#include <vector>
export module spinner;

struct spinnerPreset {
    std::string name;
    std::vector<std::string> frames;
    double defaultRate = 12.5;
};

export struct spinnerConfig {
    std::string spinner = "dots";
    double rate = 12.5;
};

static std::vector<spinnerPreset> availableSpinners() {
    return {
        {"line", {"|", "/", "-", "\\"}, 10.0},
        {"dots", {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"}, 12.5},
        {"dots2", {"⣾", "⣽", "⣻", "⢿", "⡿", "⣟", "⣯", "⣷"}, 12.5},
        {"bounce", {"⠁", "⠂", "⠄", "⡀", "⠄", "⠂"}, 8.3333333333},
        {"pulse", {"⠀⠶⠀", "⠰⣿⠆", "⢾⣉⡷", "⣏⠀⣹", "⡁⠀⢈"}, 5.5555555556},
        {"wave", {"⠁⠂⠄⡀", "⠂⠄⡀⢀", "⠄⡀⢀⠠", "⡀⢀⠠⠐", "⢀⠠⠐⠈", "⠠⠐⠈⠁", "⠐⠈⠁⠂", "⠈⠁⠂⠄"}, 10.0},
        {"orbit", {"⠃", "⠉", "⠘", "⠰", "⢠", "⣀", "⡄", "⠆"}, 10.0},
        {"dots_circle", {"⢎ ", "⠎⠁", "⠊⠑", "⠈⠱", " ⡱", "⢀⡰", "⢄⡠", "⢆⡀"}, 12.5}
    };
}

export struct spinner {
    void configure(const spinnerConfig& config) {
        const spinnerConfig normalized = normalizedConfig(config);
        if (!initialized_ || normalized.spinner != config_.spinner || normalized.rate != config_.rate) {
            start_ = Clock::now();
            initialized_ = true;
        }
        config_ = normalized;
    }

    std::vector<std::string> render() {
        if (!initialized_) configure(spinnerConfig{});
        return {currentFrame()};
    }

  private:
    using Clock = std::chrono::steady_clock;

    static const spinnerPreset& resolvePreset(std::string_view name) {
        static const std::vector<spinnerPreset> presets = availableSpinners();
        for (const spinnerPreset& preset : presets) {
            if (preset.name == name) return preset;
        }
        return presets.front();
    }

    static spinnerConfig normalizedConfig(spinnerConfig config) {
        const spinnerPreset& preset = resolvePreset(config.spinner);
        config.spinner = preset.name;
        if (config.rate <= 0.0) config.rate = preset.defaultRate;
        return config;
    }

    std::string currentFrame() const {
        const spinnerPreset& preset = resolvePreset(config_.spinner);
        if (preset.frames.empty()) return "";
        if (config_.rate <= 0.0) return preset.frames.front();

        const std::chrono::duration<double> elapsed = Clock::now() - start_;
        const double framePosition = elapsed.count() * config_.rate;
        const std::size_t index = static_cast<std::size_t>(
            std::max(0.0, framePosition)) % preset.frames.size();
        return preset.frames[index];
    }

    spinnerConfig config_{};
    Clock::time_point start_{};
    bool initialized_ = false;
};
