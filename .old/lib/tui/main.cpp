#include <cmath>
#include <chrono>
#include <string>
#include <thread>
#include <vector>
import tui;

int main(){
    TUI tui{};

    Sphere sphere{};
    sphereConfig cfg{};
    cfg.angle = 0;
    cfg.width = 16;
    cfg.height = 8;
    cfg.speed = 0.5;
    sphere.configure(cfg);

    progressBar pBar{};
    progressBarConfig pBarCfg{};
    pBarCfg.height = 1;
    pBarCfg.width = 16;
    pBarCfg.range = {0, 100};
    pBarCfg.currentValue = 0;
    pBar.configure(pBarCfg);

    struct SpinnerDemo {
        std::string label;
        spinner widget;
        spinnerConfig config;
    };

    std::vector<SpinnerDemo> spinnerDemos = {
        {"dots", spinner{}, {"dots", 12.5}},
        {"dots2", spinner{}, {"dots2", 10.0}},
        {"line", spinner{}, {"line", 14.0}},
        {"bounce", spinner{}, {"bounce", 8.0}},
        {"pulse", spinner{}, {"pulse", 6.0}},
        {"wave", spinner{}, {"wave", 10.0}},
        {"orbit", spinner{}, {"orbit", 11.0}},
        {"dots_circle", spinner{}, {"dots_circle", 9.0}}
    };

    for (SpinnerDemo& demo : spinnerDemos)
        demo.widget.configure(demo.config);

    linePlot plot{};
    linePlotConfig plotCfg{};
    plotCfg.width = 32;
    plotCfg.height = 8;
    plotCfg.xAxisName = "Time";
    plotCfg.yAxisName = "Value";
    plotCfg.xRange = {0.0, 15.0};
    plotCfg.yRange = {-1.0, 1.0};
    for(int i = 0; i < 16; ++i){
        plotCfg.xData.push_back(i);
        plotCfg.yData.push_back(std::sin(i * 0.5));
    }
    plot.configure(plotCfg);

    barPlot bars{};
    barPlotConfig barCfg{};
    barCfg.width = 32;
    barCfg.height = 8;
    barCfg.xAxisName = "Bins";
    barCfg.yAxisName = "Load";
    barCfg.xRange = {1.0, 8.0};
    barCfg.yRange = {0.0, 1.0};
    for(int i = 0; i < 8; ++i){
        barCfg.xData.push_back(i + 1);
        barCfg.yData.push_back(0.5 + (0.4 * std::sin(i * 0.6)));
    }
    bars.configure(barCfg);

    tui.enterTui();
    while(tui.running.load(std::memory_order_relaxed)){
        tui.clear();
        tui.handleInput();

        tui.writeLine("on new screen!");
        tui.writeLine("resolution: " + std::to_string(tui.width) + ", " + std::to_string(tui.height));

        cfg.angle = cfg.angle + 0.5;
        sphere.configure(cfg);
        tui.writeLines(sphere.render());

        pBarCfg.currentValue = std::fmod(pBarCfg.currentValue+0.5, 100);
        pBar.configure(pBarCfg);
        tui.writeLines(pBar.render());

        tui.writeLine("spinner gallery:");
        tui.beginRow();
        for (std::size_t i = 0; i < spinnerDemos.size(); ++i) {
            if (i > 0 && (i % 4) == 0) {
                tui.endRow();
                tui.beginRow();
            }

            tui.drawBox({18, 4});
            tui.writeLine(spinnerDemos[i].label);
            tui.writeLine(spinnerDemos[i].widget.render()[0]);
            tui.endRegion();
        }
        tui.endRow();

        for(int i = 0; i < 16; ++i)
            plotCfg.yData[i] = std::sin((i * 0.5) + (cfg.angle * 0.02));
        plot.configure(plotCfg);
        tui.writeLines(plot.render());

        tui.beginRow();
        tui.drawBox({48, 32});
        tui.writeLine("text inside of box!");
        tui.writeLine("new line inside of box..");
        tui.writeLine("Bar plot inside of box:");
        tui.writeLine(""); // line break
        for(int i = 0; i < 8; ++i)
            barCfg.yData[i] = 0.5 + (0.4 * std::sin((i * 0.6) + (cfg.angle * 0.03)));
        bars.configure(barCfg);
        tui.writeLines(bars.render());
        tui.drawBox({16, 8});
        tui.writeLine("internal box");
        tui.endRegion();
        tui.writeLine("This line is outside of the box");
        tui.endRegion();

        tui.drawBox({16, 8});
        tui.writeLine("right box");
        tui.endRegion();

        tui.beginRow();
        tui.drawBox({7, 10});
        tui.endRegion();
        tui.beginColumn();
        tui.drawBox({7, 5});
        tui.endRegion();
        tui.drawBox({7, 5});
        tui.endRegion();
        tui.endColumn();
        tui.endRow();

        tui.endRow();

        tui.writeLine("Outside of both boxes");
        

        tui.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    tui.exitTui();
};
