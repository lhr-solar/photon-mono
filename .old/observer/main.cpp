#include <array>
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>
#include "../lib/tcp.hpp"
#include "../lib/shared.hpp"
import tui;

inline bool getData(metaData& m, Tcp& t){
    uint8_t* p = reinterpret_cast<uint8_t*>(&m);
    size_t remaining = sizeof(m);
    while(remaining > 0){
        ssize_t n = recv(t.sock, p, remaining, 0);
        if(n < 0 && errno == EINTR) continue;
        if(n <= 0) return false;
        p += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

inline std::string fmtb(uint64_t b){
    static constexpr std::array<const char*, 6> u{"B", "KB", "MB", "GB", "TB", "PB"};
    double value = static_cast<double>(b);
    std::size_t unit = 0;
    while(value >= 1000.0 && unit < u.size() - 1){
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream s;
    s << std::fixed << std::setprecision(2) << value << ' ' << u[unit];
    return s.str();
}

inline std::string fmtms(uint64_t ms){
    static constexpr std::array<std::pair<double, const char*>, 8> units{{
        {1.0, "ms"},
        {1000.0, "sec"},
        {60.0 * 1000.0, "min"},
        {60.0 * 60.0 * 1000.0, "hr"},
        {24.0 * 60.0 * 60.0 * 1000.0, "day"},
        {7.0 * 24.0 * 60.0 * 60.0 * 1000.0, "wk"},
        {30.0 * 24.0 * 60.0 * 60.0 * 1000.0, "mon"},
        {365.0 * 24.0 * 60.0 * 60.0 * 1000.0, "yr"},
    }};

    double value = static_cast<double>(ms);
    std::size_t unit = 0;
    while(unit < units.size() - 1 && value >= units[unit + 1].first){
        ++unit;
    }
    std::ostringstream s;
    s << std::fixed << std::setprecision(2) << (value / units[unit].first) << ' ' << units[unit].second;
    return s.str();
}

inline std::string fmtms_or_never(uint64_t ms, bool seen){
    return seen ? fmtms(ms) : "--.-- --";
}

inline std::string fmtcount(uint64_t count){
    static constexpr std::array<const char*, 5> units{"", "K", "M", "B", "T"};
    double value = static_cast<double>(count);
    std::size_t unit = 0;
    while(value >= 1000.0 && unit < units.size() - 1){
        value /= 1000.0;
        ++unit;
    }
    std::ostringstream s;
    s << std::fixed << std::setprecision(2) << value;
    if(units[unit][0] != '\0') s << ' ' << units[unit];
    return s.str();
}

inline const char* yesno(bool v){
    return v ? "yes" : "no";
}

inline std::string maskedPort(uint16_t){
    return "****";
}

struct UiField{
    std::string label;
    std::string value;
};

struct UiSection{
    std::string title;
    std::vector<UiField> fields;
};

struct WidgetLayout{
    int labelWidth;
    int valueWidth;
    int fieldColumns;
};

struct DashboardState{
    std::vector<double> serverFrameRate;
    std::vector<double> tcpSourcePackets;
    std::vector<double> tcpSourceBytes;
    std::vector<double> udpSourcePackets;
    std::vector<double> udpSourceBytes;
    std::vector<double> tcpClientPackets;
    std::vector<double> tcpClientBytes;
    std::vector<double> tcpClientFanout;
    std::vector<double> udpClientPackets;
    std::vector<double> udpClientBytes;
    std::vector<double> udpClientFanout;
    spinnerConfig healthySpinner{"dots", 10.0};
    spinnerConfig idleSpinner{"wave", 8.0};
    spinnerConfig warnSpinner{"pulse", 6.0};
    spinner healthy{};
    spinner idle{};
    spinner warn{};
    sphereConfig serverSphereConfig{};
    Sphere serverSphere{};
    bool hasPrevious = false;
    metaData previous{};
};

inline int glyphCount(std::string_view text){
    int count = 0;
    for(std::size_t i = 0; i < text.size(); ++count){
        unsigned char byte = static_cast<unsigned char>(text[i]);
        std::size_t length = 1;
        if((byte & 0x80u) == 0u) length = 1;
        else if((byte & 0xE0u) == 0xC0u) length = 2;
        else if((byte & 0xF0u) == 0xE0u) length = 3;
        else if((byte & 0xF8u) == 0xF0u) length = 4;
        if(i + length > text.size()) length = 1;
        i += length;
    }
    return count;
}

inline std::string padRight(std::string text, int width){
    const int missing = std::max(0, width - glyphCount(text));
    text.append(static_cast<std::size_t>(missing), ' ');
    return text;
}

inline uint64_t saturatingDelta(uint64_t current, uint64_t previous){
    return current >= previous ? current - previous : 0;
}

inline void pushSample(std::vector<double>& history, double value, std::size_t maxSize = 24){
    if(history.size() >= maxSize) history.erase(history.begin());
    history.push_back(value);
}

inline double maxOrOne(const std::vector<double>& values){
    double maxValue = 0.0;
    for(double value : values)
        maxValue = std::max(maxValue, value);
    return maxValue > 0.0 ? maxValue : 1.0;
}

inline std::vector<double> xAxisFor(const std::vector<double>& values){
    std::vector<double> xData;
    xData.reserve(values.size());
    for(std::size_t i = 0; i < values.size(); ++i)
        xData.push_back(static_cast<double>(i));
    return xData;
}

inline double ratio(uint64_t part, uint64_t whole){
    if(whole == 0) return 0.0;
    return std::clamp(static_cast<double>(part) / static_cast<double>(whole), 0.0, 1.0);
}

inline std::string statusText(bool active, bool connected, bool stale){
    if(!active) return "offline";
    if(connected) return stale ? "connected stale" : "connected";
    return stale ? "waiting stale" : "waiting";
}

inline std::string observerStatusText(const metaData& m){
    if(!m.serverMetaData.observerListenerActive) return "offline";
    if(m.serverMetaData.observerClientsCurrentlyConnected > 0) return "connected";
    return "listening";
}

inline std::string renderSpinner(spinner& widget, const spinnerConfig& config){
    widget.configure(config);
    return widget.render().front();
}

inline void updateDashboardState(DashboardState& state, const metaData& current){
    if(state.hasPrevious){
        pushSample(state.serverFrameRate, static_cast<double>(saturatingDelta(
            current.serverMetaData.observerFramesPublished,
            state.previous.serverMetaData.observerFramesPublished)));

        pushSample(state.tcpSourcePackets, static_cast<double>(saturatingDelta(
            current.tcpSourceMetaData.totalPacketsReceived,
            state.previous.tcpSourceMetaData.totalPacketsReceived)));
        pushSample(state.tcpSourceBytes, static_cast<double>(saturatingDelta(
            current.tcpSourceMetaData.totalBytesTransmitted,
            state.previous.tcpSourceMetaData.totalBytesTransmitted)));

        pushSample(state.udpSourcePackets, static_cast<double>(saturatingDelta(
            current.udpSourceMetaData.totalPacketsReceived,
            state.previous.udpSourceMetaData.totalPacketsReceived)));
        pushSample(state.udpSourceBytes, static_cast<double>(saturatingDelta(
            current.udpSourceMetaData.totalBytesTransmitted,
            state.previous.udpSourceMetaData.totalBytesTransmitted)));

        pushSample(state.tcpClientPackets, static_cast<double>(saturatingDelta(
            current.tcpClientMetaData.totalPacketsTransmitted,
            state.previous.tcpClientMetaData.totalPacketsTransmitted)));
        pushSample(state.tcpClientBytes, static_cast<double>(saturatingDelta(
            current.tcpClientMetaData.totalBytesTransmitted,
            state.previous.tcpClientMetaData.totalBytesTransmitted)));
        pushSample(state.tcpClientFanout, static_cast<double>(current.tcpClientMetaData.lastWriteFanout));

        pushSample(state.udpClientPackets, static_cast<double>(saturatingDelta(
            current.udpClientMetaData.totalPacketsTransmitted,
            state.previous.udpClientMetaData.totalPacketsTransmitted)));
        pushSample(state.udpClientBytes, static_cast<double>(saturatingDelta(
            current.udpClientMetaData.totalBytesTransmitted,
            state.previous.udpClientMetaData.totalBytesTransmitted)));
        pushSample(state.udpClientFanout, static_cast<double>(current.udpClientMetaData.lastWriteFanout));
    } else {
        pushSample(state.serverFrameRate, 0.0);
        pushSample(state.tcpSourcePackets, 0.0);
        pushSample(state.tcpSourceBytes, 0.0);
        pushSample(state.udpSourcePackets, 0.0);
        pushSample(state.udpSourceBytes, 0.0);
        pushSample(state.tcpClientPackets, 0.0);
        pushSample(state.tcpClientBytes, 0.0);
        pushSample(state.tcpClientFanout, static_cast<double>(current.tcpClientMetaData.lastWriteFanout));
        pushSample(state.udpClientPackets, 0.0);
        pushSample(state.udpClientBytes, 0.0);
        pushSample(state.udpClientFanout, static_cast<double>(current.udpClientMetaData.lastWriteFanout));
    }
    state.previous = current;
    state.hasPrevious = true;
}

static constexpr WidgetLayout SERVER_LAYOUT{7, 10, 3};
static constexpr WidgetLayout SOURCE_LAYOUT{7, 10, 2};
static constexpr WidgetLayout CLIENT_LAYOUT{7, 10, 2};

inline std::string fieldLine(const UiField& field, int labelWidth, int valueWidth){
    return padRight(field.label, labelWidth) +
           ": " +
           padRight(field.value, valueWidth) +
           "  ";
}

inline void drawSectionBox(TUI& tui, const UiSection& section, WidgetLayout layout, int boxWidth, int boxHeight){
    tui.drawBox({boxWidth, boxHeight});
    tui.writeLine(section.title);
    tui.beginRow();
    const int rowsPerColumn = std::max(
        1,
        (static_cast<int>(section.fields.size()) + layout.fieldColumns - 1) / layout.fieldColumns);
    for(int column = 0; column < layout.fieldColumns; ++column){
        tui.beginColumn();
        const int start = column * rowsPerColumn;
        const int end = std::min(start + rowsPerColumn, static_cast<int>(section.fields.size()));
        for(int i = start; i < end; ++i)
            tui.writeLine(fieldLine(section.fields[static_cast<std::size_t>(i)], layout.labelWidth, layout.valueWidth));
        tui.endColumn();
    }
    tui.endRow();
    tui.endRegion();
}

inline void writeKeyValue(TUI& tui, std::string_view key, const std::string& value, int labelWidth){
    tui.writeLine(padRight(std::string(key), labelWidth) + ": " + value);
}

inline std::string compactField(const UiField& field, int labelWidth, int valueWidth){
    return padRight(field.label, labelWidth) + ": " + padRight(field.value, valueWidth);
}

struct FieldRunLayout{
    int labelWidth;
    int valueWidth;
    int fieldsPerLine;
};

inline FieldRunLayout fitFieldRunLayout(int regionWidth, int preferredFieldsPerLine){
    FieldRunLayout layout{};
    layout.fieldsPerLine = std::max(1, preferredFieldsPerLine);
    while(layout.fieldsPerLine > 1){
        const int gaps = (layout.fieldsPerLine - 1) * 3;
        const int perField = (regionWidth - gaps) / layout.fieldsPerLine;
        if(perField >= 14) break;
        --layout.fieldsPerLine;
    }
    const int gaps = (layout.fieldsPerLine - 1) * 3;
    const int perField = std::max(8, (regionWidth - gaps) / layout.fieldsPerLine);
    layout.labelWidth = std::clamp(perField / 3, 4, 7);
    layout.valueWidth = std::max(4, perField - layout.labelWidth - 2);
    return layout;
}

inline void writeFieldRuns(
    TUI& tui,
    const std::vector<UiField>& fields,
    std::size_t start,
    std::size_t count,
    int labelWidth,
    int valueWidth,
    int fieldsPerLine){
    const std::size_t end = std::min(fields.size(), start + count);
    for(std::size_t i = start; i < end; i += static_cast<std::size_t>(std::max(1, fieldsPerLine))){
        std::string line;
        for(int j = 0; j < fieldsPerLine; ++j){
            const std::size_t index = i + static_cast<std::size_t>(j);
            if(index >= end) break;
            if(!line.empty()) line += "   ";
            line += compactField(fields[index], labelWidth, valueWidth);
        }
        tui.writeLine(line);
    }
}

inline void beginSubRegion(TUI& tui, Rect parent, int x, int y, int width, int height){
    tui.beginRegion({parent.x + x, parent.y + y, width, height});
}

inline void writeFieldGrid(
    TUI& tui,
    const std::vector<UiField>& fields,
    std::size_t start,
    std::size_t count,
    int labelWidth,
    int valueWidth,
    int columnCount){
    if(start >= fields.size() || count == 0) return;
    const std::size_t end = std::min(fields.size(), start + count);
    const int rowsPerColumn = std::max(
        1,
        static_cast<int>((end - start + static_cast<std::size_t>(columnCount) - 1) / static_cast<std::size_t>(columnCount)));
    tui.beginRow();
    for(int column = 0; column < columnCount; ++column){
        tui.beginColumn();
        const std::size_t columnStart = start + static_cast<std::size_t>(column * rowsPerColumn);
        const std::size_t columnEnd = std::min(end, columnStart + static_cast<std::size_t>(rowsPerColumn));
        for(std::size_t i = columnStart; i < columnEnd; ++i)
            tui.writeLine(fieldLine(fields[i], labelWidth, valueWidth));
        tui.endColumn();
    }
    tui.endRow();
}

inline void writePlot(TUI& tui, const std::vector<std::string>& lines){
    for(const std::string& line : lines)
        tui.writeLine(line);
}

inline std::vector<std::string> renderLineHistory(
    const std::vector<double>& values, int width, int height, std::string xAxis, std::string yAxis){
    linePlot plot{};
    linePlotConfig cfg{};
    cfg.width = std::max(12, width);
    cfg.height = std::max(4, height);
    cfg.xAxisName = std::move(xAxis);
    cfg.yAxisName = std::move(yAxis);
    cfg.xData = xAxisFor(values);
    cfg.yData = values;
    cfg.xRange = {0.0, std::max(1.0, static_cast<double>(std::max<std::size_t>(1, values.size() - 1)))};
    cfg.yRange = {0.0, maxOrOne(values)};
    plot.configure(cfg);
    return plot.render();
}

inline std::vector<std::string> renderBarHistory(
    const std::vector<double>& values, int width, int height, std::string xAxis, std::string yAxis){
    barPlot plot{};
    barPlotConfig cfg{};
    cfg.width = std::max(12, width);
    cfg.height = std::max(4, height);
    cfg.xAxisName = std::move(xAxis);
    cfg.yAxisName = std::move(yAxis);
    cfg.xData = xAxisFor(values);
    cfg.yData = values;
    cfg.xRange = {0.0, std::max(1.0, static_cast<double>(std::max<std::size_t>(1, values.size())))};
    cfg.yRange = {0.0, maxOrOne(values)};
    plot.configure(cfg);
    return plot.render();
}

inline std::vector<std::string> renderProgress(double current, double maxValue, int width){
    progressBar bar{};
    progressBarConfig cfg{};
    cfg.width = std::max(4, width);
    cfg.height = 1;
    cfg.range = {0.0, std::max(1.0, maxValue)};
    cfg.currentValue = std::clamp(current, 0.0, std::max(1.0, maxValue));
    bar.configure(cfg);
    return bar.render();
}

inline UiSection buildServerSection(const metaData& m){
    return {
        "SERVER STATUS",
        {
            {"Port", maskedPort(m.serverMetaData.observerPort)},
            {"Active", yesno(m.serverMetaData.observerListenerActive)},
            {"Uptime", fmtms_or_never(m.serverMetaData.uptime, m.serverMetaData.uptime > 0)},
            {"CurrentObservers", fmtcount(m.serverMetaData.observerClientsCurrentlyConnected)},
            {"TotalConnections", fmtcount(m.serverMetaData.observerTotalConnections)},
            {"TotalDisconnects", fmtcount(m.serverMetaData.observerTotalDisconnects)},
            {"PeakObservers", fmtcount(m.serverMetaData.observerPeakConnections)},
            {"Frames", fmtcount(m.serverMetaData.observerFramesPublished)},
            {"SendErrors", fmtcount(m.serverMetaData.observerTransmissionFailures)},
            {"SinceConnection", fmtms_or_never(
                m.serverMetaData.timeSinceLastObserverConnection,
                m.serverMetaData.observerTotalConnections > 0)},
            {"SinceDisconnect", fmtms_or_never(
                m.serverMetaData.timeSinceLastObserverDisconnect,
                m.serverMetaData.observerTotalDisconnects > 0)},
            {"SincePublish", fmtms_or_never(
                m.serverMetaData.timeSinceLastObserverPublish,
                m.serverMetaData.observerFramesPublished > 0)},
        },
    };
}

inline UiSection buildTcpSourceSection(const metaData& m){
    return {
        "TCP SOURCE",
        {
            {"Port", maskedPort(m.tcpSourceMetaData.listenPort)},
            {"Active", yesno(m.tcpSourceMetaData.listenerActive)},
            {"CurrentSource", yesno(m.tcpSourceMetaData.sourceCurrentlyConnected)},
            {"SourceSeen", yesno(m.tcpSourceMetaData.sourceEverConnected)},
            {"Stale", yesno(m.tcpSourceMetaData.sourceDataStale)},
            {"TotalConnections", fmtcount(m.tcpSourceMetaData.totalConnections)},
            {"TotalDisconnects", fmtcount(m.tcpSourceMetaData.totalDisconnects)},
            {"ReceiveFailures", fmtcount(m.tcpSourceMetaData.receiveFailures)},
            {"Packets", fmtcount(m.tcpSourceMetaData.totalPacketsReceived)},
            {"LastPacket", fmtb(m.tcpSourceMetaData.lastPacketBytes)},
            {"ConnectionBytes", fmtb(m.tcpSourceMetaData.bytesTxThisConnection)},
            {"TotalBytes", fmtb(m.tcpSourceMetaData.totalBytesTransmitted)},
            {"SinceConnection", fmtms_or_never(
                m.tcpSourceMetaData.timeSinceLastConnection,
                m.tcpSourceMetaData.totalConnections > 0)},
            {"SinceDisconnect", fmtms_or_never(
                m.tcpSourceMetaData.timeSinceLastDisconnect,
                m.tcpSourceMetaData.totalDisconnects > 0)},
            {"SinceReceive", fmtms_or_never(
                m.tcpSourceMetaData.timeSinceLastReceive,
                m.tcpSourceMetaData.totalPacketsReceived > 0)},
        },
    };
}

inline UiSection buildUdpSourceSection(const metaData& m){
    return {
        "UDP SOURCE",
        {
            {"Port", maskedPort(m.udpSourceMetaData.listenPort)},
            {"Active", yesno(m.udpSourceMetaData.listenerActive)},
            {"CurrentSource", yesno(m.udpSourceMetaData.sourceCurrentlyConnected)},
            {"SourceSeen", yesno(m.udpSourceMetaData.sourceEverConnected)},
            {"Stale", yesno(m.udpSourceMetaData.sourceDataStale)},
            {"TotalConnections", fmtcount(m.udpSourceMetaData.totalConnections)},
            {"TotalDisconnects", fmtcount(m.udpSourceMetaData.totalDisconnects)},
            {"ReceiveFailures", fmtcount(m.udpSourceMetaData.receiveFailures)},
            {"Packets", fmtcount(m.udpSourceMetaData.totalPacketsReceived)},
            {"LastPacket", fmtb(m.udpSourceMetaData.lastPacketBytes)},
            {"ConnectionBytes", fmtb(m.udpSourceMetaData.bytesTxThisConnection)},
            {"TotalBytes", fmtb(m.udpSourceMetaData.totalBytesTransmitted)},
            {"SinceConnection", fmtms_or_never(
                m.udpSourceMetaData.timeSinceLastConnection,
                m.udpSourceMetaData.totalConnections > 0)},
            {"SinceDisconnect", fmtms_or_never(
                m.udpSourceMetaData.timeSinceLastDisconnect,
                m.udpSourceMetaData.totalDisconnects > 0)},
            {"SinceReceive", fmtms_or_never(
                m.udpSourceMetaData.timeSinceLastReceive,
                m.udpSourceMetaData.totalPacketsReceived > 0)},
        },
    };
}

inline UiSection buildTcpClientSection(const metaData& m){
    return {
        "TCP CLIENTS",
        {
            {"Port", maskedPort(m.tcpClientMetaData.listenPort)},
            {"Active", yesno(m.tcpClientMetaData.listenerActive)},
            {"CurrentClients", fmtcount(m.tcpClientMetaData.clientsCurrentlyConnected)},
            {"TotalConnections", fmtcount(m.tcpClientMetaData.totalClientConnections)},
            {"TotalDisconnects", fmtcount(m.tcpClientMetaData.totalClientDisconnects)},
            {"PeakClients", fmtcount(m.tcpClientMetaData.peakClientConnections)},
            {"TransmissionFailures", fmtcount(m.tcpClientMetaData.transmissionFailures)},
            {"PartialTransmissions", fmtcount(m.tcpClientMetaData.partialTransmissions)},
            {"DispatchCycles", fmtcount(m.tcpClientMetaData.totalDispatchCycles)},
            {"IdleDispatches", fmtcount(m.tcpClientMetaData.idleDispatchCycles)},
            {"Fanout", fmtcount(m.tcpClientMetaData.lastWriteFanout)},
            {"LastWrite", fmtb(m.tcpClientMetaData.lastWriteBytes)},
            {"Packets", fmtcount(m.tcpClientMetaData.totalPacketsTransmitted)},
            {"TotalBytes", fmtb(m.tcpClientMetaData.totalBytesTransmitted)},
            {"SinceConnection", fmtms_or_never(
                m.tcpClientMetaData.timeSinceLastConnection,
                m.tcpClientMetaData.totalClientConnections > 0)},
            {"SinceDisconnect", fmtms_or_never(
                m.tcpClientMetaData.timeSinceLastDisconnect,
                m.tcpClientMetaData.totalClientDisconnects > 0)},
            {"SinceTransmit", fmtms_or_never(
                m.tcpClientMetaData.timeSinceLastTransmission,
                m.tcpClientMetaData.totalPacketsTransmitted > 0)},
        },
    };
}

inline UiSection buildUdpClientSection(const metaData& m){
    return {
        "UDP CLIENTS",
        {
            {"Port", maskedPort(m.udpClientMetaData.listenPort)},
            {"Active", yesno(m.udpClientMetaData.listenerActive)},
            {"NeedsSubscription", yesno(m.udpClientMetaData.requiresSubscription)},
            {"CurrentClients", fmtcount(m.udpClientMetaData.clientsCurrentlyConnected)},
            {"TotalConnections", fmtcount(m.udpClientMetaData.totalClientConnections)},
            {"TotalDisconnects", fmtcount(m.udpClientMetaData.totalClientDisconnects)},
            {"PeakClients", fmtcount(m.udpClientMetaData.peakClientConnections)},
            {"Subscriptions", fmtcount(m.udpClientMetaData.subscribeMessages)},
            {"Unsubscriptions", fmtcount(m.udpClientMetaData.unsubscribeMessages)},
            {"ErrorDisconnects", fmtcount(m.udpClientMetaData.errorQueueDisconnects)},
            {"TransmissionFailures", fmtcount(m.udpClientMetaData.transmissionFailures)},
            {"DispatchCycles", fmtcount(m.udpClientMetaData.totalDispatchCycles)},
            {"IdleDispatches", fmtcount(m.udpClientMetaData.idleDispatchCycles)},
            {"Fanout", fmtcount(m.udpClientMetaData.lastWriteFanout)},
            {"LastWrite", fmtb(m.udpClientMetaData.lastWriteBytes)},
            {"Packets", fmtcount(m.udpClientMetaData.totalPacketsTransmitted)},
            {"TotalBytes", fmtb(m.udpClientMetaData.totalBytesTransmitted)},
            {"SinceConnection", fmtms_or_never(
                m.udpClientMetaData.timeSinceLastConnection,
                m.udpClientMetaData.totalClientConnections > 0)},
            {"SinceDisconnect", fmtms_or_never(
                m.udpClientMetaData.timeSinceLastDisconnect,
                m.udpClientMetaData.totalClientDisconnects > 0)},
            {"SinceTransmit", fmtms_or_never(
                m.udpClientMetaData.timeSinceLastTransmission,
                m.udpClientMetaData.totalPacketsTransmitted > 0)},
        },
    };
}

inline void drawServerPanel(TUI& tui, const metaData& m, DashboardState& state, int boxWidth, int boxHeight){
    const Rect content = tui.drawBox({boxWidth, boxHeight});
    const int innerWidth = std::max(8, content.width);
    const int innerHeight = std::max(4, content.height);
    const int titleRows = 1;
    const int bodyY = titleRows;
    const int bodyHeight = std::max(1, innerHeight - titleRows);
    const int statusWidth = std::max(1, innerWidth / 5);
    const int summaryWidth = std::max(1, (innerWidth * 3) / 10);
    const int timingTextWidth = std::max(1, (innerWidth * 3) / 10);
    const int sphereWidth = std::max(0, innerWidth - statusWidth - summaryWidth - timingTextWidth);
    const FieldRunLayout summaryLayout = fitFieldRunLayout(summaryWidth, 2);
    const FieldRunLayout timingLayout = fitFieldRunLayout(timingTextWidth, 2);
    tui.writeLine("SERVER STATUS");

    beginSubRegion(tui, content, 0, bodyY, statusWidth, bodyHeight);
    const std::string serverSpin = renderSpinner(
        m.serverMetaData.observerListenerActive ? state.healthy : state.warn,
        m.serverMetaData.observerListenerActive ? state.healthySpinner : state.warnSpinner);
    const std::string tcpSpin = renderSpinner(
        m.tcpSourceMetaData.sourceCurrentlyConnected ? state.healthy : state.idle,
        m.tcpSourceMetaData.sourceCurrentlyConnected ? state.healthySpinner : state.idleSpinner);
    const std::string udpSpin = renderSpinner(
        m.udpSourceMetaData.sourceCurrentlyConnected ? state.healthy : state.idle,
        m.udpSourceMetaData.sourceCurrentlyConnected ? state.healthySpinner : state.idleSpinner);
    tui.writeLine("");
    tui.writeLine(serverSpin + " obs " + observerStatusText(m));
    tui.writeLine(tcpSpin + " tcp  " + statusText(
        m.tcpSourceMetaData.listenerActive,
        m.tcpSourceMetaData.sourceCurrentlyConnected,
        m.tcpSourceMetaData.sourceDataStale));
    tui.writeLine(udpSpin + " udp  " + statusText(
        m.udpSourceMetaData.listenerActive,
        m.udpSourceMetaData.sourceCurrentlyConnected,
        m.udpSourceMetaData.sourceDataStale));
    tui.endRegion();

    beginSubRegion(tui, content, statusWidth, bodyY, summaryWidth, bodyHeight);
    tui.writeLine("SUMMARY");
    tui.writeLine("");
    writeFieldRuns(tui, {
        {"Port", std::to_string(m.serverMetaData.observerPort)},
        {"Uptime", fmtms_or_never(m.serverMetaData.uptime, m.serverMetaData.uptime > 0)},
        {"CurrentObservers", fmtcount(m.serverMetaData.observerClientsCurrentlyConnected)},
        {"Frames", fmtcount(m.serverMetaData.observerFramesPublished)},
        {"SendErrors", fmtcount(m.serverMetaData.observerTransmissionFailures)},
        {"PeakObservers", fmtcount(m.serverMetaData.observerPeakConnections)}
    }, 0, 6, summaryLayout.labelWidth, summaryLayout.valueWidth, summaryLayout.fieldsPerLine);
    tui.endRegion();

    beginSubRegion(tui, content, statusWidth + summaryWidth, bodyY, timingTextWidth, bodyHeight);
    tui.writeLine("TIMING");
    tui.writeLine("");
    writeFieldRuns(tui, {
        {"SinceConnection", fmtms_or_never(
            m.serverMetaData.timeSinceLastObserverConnection,
            m.serverMetaData.observerTotalConnections > 0)},
        {"SinceDisconnect", fmtms_or_never(
            m.serverMetaData.timeSinceLastObserverDisconnect,
            m.serverMetaData.observerTotalDisconnects > 0)},
        {"SincePublish", fmtms_or_never(
            m.serverMetaData.timeSinceLastObserverPublish,
            m.serverMetaData.observerFramesPublished > 0)},
        {"TotalConnections", fmtcount(m.serverMetaData.observerTotalConnections)},
        {"TotalDisconnects", fmtcount(m.serverMetaData.observerTotalDisconnects)},
        {"Active", yesno(m.serverMetaData.observerListenerActive)}
    }, 0, 6, timingLayout.labelWidth, timingLayout.valueWidth, timingLayout.fieldsPerLine);
    tui.endRegion();

    beginSubRegion(tui, content, statusWidth + summaryWidth + timingTextWidth, bodyY, sphereWidth, bodyHeight);
    const int availableWidth = sphereWidth;
    const int availableHeight = bodyHeight;
    if(availableWidth >= 4 && availableHeight >= 4){
        int sphereWidthFit = std::max(4, std::min(availableWidth, availableHeight * 2));
        int sphereHeightFit = std::max(4, std::min(availableHeight, sphereWidthFit / 2));
        if((sphereHeightFit * 2) > sphereWidthFit){
            sphereHeightFit = std::max(4, sphereWidthFit / 2);
        }
        if(sphereWidthFit >= 4 && sphereHeightFit >= 4){
            state.serverSphereConfig.width = sphereWidthFit;
            state.serverSphereConfig.height = sphereHeightFit;
            state.serverSphereConfig.speed = 0.18;
            state.serverSphereConfig.angle = 18.0;
            state.serverSphere.configure(state.serverSphereConfig);
            tui.writeLines(state.serverSphere.render());
        }
    }
    tui.endRegion();

    tui.endRegion();
}

inline void drawSourcePanel(
    TUI& tui,
    const UiSection& section,
    const std::vector<double>& packetHistory,
    const std::vector<double>& byteHistory,
    bool connected,
    bool stale,
    DashboardState& state,
    int boxWidth,
    int boxHeight){
    const Rect content = tui.drawBox({boxWidth, boxHeight});
    const int innerWidth = std::max(8, content.width);
    const int innerHeight = std::max(6, content.height);
    const int topHeight = std::max(4, innerHeight / 2 - 1);
    const int bottomY = topHeight + 1;
    const int bottomHeight = std::max(4, innerHeight - bottomY);
    const int halfWidth = innerWidth / 2;
    tui.writeLine(section.title);

    beginSubRegion(tui, content, 0, 1, innerWidth, topHeight);
    const std::string spin = renderSpinner(
        connected ? state.healthy : (stale ? state.warn : state.idle),
        connected ? state.healthySpinner : (stale ? state.warnSpinner : state.idleSpinner));
    tui.writeLine(spin + "  " + (connected ? std::string("streaming") : (stale ? std::string("stale") : std::string("idle"))));
    tui.writeLine("");
    writeFieldRuns(tui, section.fields, 0, 12, 7, 10, 3);
    tui.endRegion();

    beginSubRegion(tui, content, 0, bottomY, halfWidth, bottomHeight);
    tui.writeLine("packet history");
    writePlot(tui, renderLineHistory(packetHistory, std::max(12, halfWidth), bottomHeight - 1, "t", "pkt"));
    tui.endRegion();

    beginSubRegion(tui, content, halfWidth, bottomY, innerWidth - halfWidth, bottomHeight);
    tui.writeLine("bytes");
    writePlot(tui, renderBarHistory(byteHistory, std::max(12, innerWidth - halfWidth), bottomHeight - 1, "t", "B"));
    tui.endRegion();
    tui.endRegion();
}

inline void drawClientPanel(
    TUI& tui,
    const UiSection& section,
    const std::vector<double>& packetHistory,
    const std::vector<double>& fanoutHistory,
    bool active,
    DashboardState& state,
    int boxWidth,
    int boxHeight){
    const Rect content = tui.drawBox({boxWidth, boxHeight});
    const int innerWidth = std::max(8, content.width);
    const int innerHeight = std::max(6, content.height);
    const int topHeight = std::max(4, innerHeight / 2 - 1);
    const int bottomY = topHeight + 1;
    const int bottomHeight = std::max(4, innerHeight - bottomY);
    const int halfWidth = innerWidth / 2;
    tui.writeLine(section.title);

    beginSubRegion(tui, content, 0, 1, innerWidth, topHeight);
    const std::string spin = renderSpinner(
        active ? state.healthy : state.idle,
        active ? state.healthySpinner : state.idleSpinner);
    tui.writeLine(spin + "  " + (active ? std::string("delivering") : std::string("idle")));
    tui.writeLine("");
    writeFieldRuns(tui, section.fields, 0, section.fields.size(), 7, 10, 3);
    tui.endRegion();

    beginSubRegion(tui, content, 0, bottomY, halfWidth, bottomHeight);
    tui.writeLine("tx packet history");
    writePlot(tui, renderLineHistory(packetHistory, std::max(12, halfWidth), bottomHeight - 1, "t", "pkt"));
    tui.endRegion();

    beginSubRegion(tui, content, halfWidth, bottomY, innerWidth - halfWidth, bottomHeight);
    tui.writeLine("fanout history");
    writePlot(tui, renderBarHistory(fanoutHistory, std::max(12, innerWidth - halfWidth), bottomHeight - 1, "t", "fan"));
    tui.endRegion();
    tui.endRegion();
}

inline void drawUI(TUI& tui, const metaData& m, DashboardState& state){
    const UiSection serverSection = buildServerSection(m);
    const UiSection tcpSourceSection = buildTcpSourceSection(m);
    const UiSection udpSourceSection = buildUdpSourceSection(m);
    const UiSection tcpClientSection = buildTcpClientSection(m);
    const UiSection udpClientSection = buildUdpClientSection(m);
    const int frameWidth = static_cast<int>(tui.width);
    const int frameHeight = static_cast<int>(tui.height);
    const int topHeight = std::max(3, frameHeight / 5);
    const int middleHeight = std::max(3, (frameHeight * 2) / 5);
    const int bottomHeight = std::max(3, frameHeight - topHeight - middleHeight);
    const int leftWidth = frameWidth / 2;
    const int rightWidth = frameWidth - leftWidth;

    drawServerPanel(tui, m, state, frameWidth, topHeight);
    tui.beginRow();
    drawSourcePanel(
        tui, tcpSourceSection, state.tcpSourcePackets, state.tcpSourceBytes,
        m.tcpSourceMetaData.sourceCurrentlyConnected, m.tcpSourceMetaData.sourceDataStale,
        state, leftWidth, middleHeight);
    drawSourcePanel(
        tui, udpSourceSection, state.udpSourcePackets, state.udpSourceBytes,
        m.udpSourceMetaData.sourceCurrentlyConnected, m.udpSourceMetaData.sourceDataStale,
        state, rightWidth, middleHeight);
    tui.endRow();

    tui.beginRow();
    drawClientPanel(
        tui, tcpClientSection, state.tcpClientPackets, state.tcpClientFanout,
        m.tcpClientMetaData.clientsCurrentlyConnected > 0, state, leftWidth, bottomHeight);
    drawClientPanel(
        tui, udpClientSection, state.udpClientPackets, state.udpClientFanout,
        m.udpClientMetaData.clientsCurrentlyConnected > 0, state, rightWidth, bottomHeight);
    tui.endRow();
}

int main() {
    TUI  tui{};
    DashboardState dashboard{};
    tui.enterTui();
    auto frameTime = std::chrono::milliseconds(16);
    auto t0 = std::chrono::steady_clock::now();
    Tcp tcpConnection;
    tcpConnection.createConnection(OBS_OPORT, aws_ip, 0);
    while(tui.running.load(std::memory_order_relaxed)){
        metaData m{};
        if(!getData(m, tcpConnection)){tcpConnection.closeConnection(); break;}
        updateDashboardState(dashboard, m);
        auto t1 = std::chrono::steady_clock::now();
        if(t1 >= t0){
            tui.clear();
            tui.handleInput();
            drawUI(tui, m, dashboard);
            tui.flush();
            if(t1 > ((t0 += frameTime) + frameTime)) t0 = t1;
        }
    } tui.exitTui();
}
