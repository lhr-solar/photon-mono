module;
#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <string_view>
#include <string>
#include <termios.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <utility>
#include <vector>
export module tui;
export import sphere;
export import progressBar;
export import linePlot;
export import barPlot;
export import spinner;

export struct Rect{
    int x{};
    int y{};
    int width{};
    int height{};
};

struct RegionFrame{
    Rect rect{};
    int cursorX{};
    int cursorY{};
    int resumeX{};
    int resumeY{};
};

struct RowFrame{
    int originX{};
    int originY{};
    int nextX{};
    int maxHeight{};
    std::size_t regionDepth{};
};

struct ColumnFrame{
    int originX{};
    int originY{};
    int nextY{};
    int maxWidth{};
    std::size_t regionDepth{};
};

export struct TUI{
    static inline TUI* self = nullptr;
    std::atomic<bool> resize{};
    std::atomic<bool> running = true;
    termios oldTerminalConfig{};
    termios terminalConfig{};
    uint32_t width{};
    uint32_t height{};
    std::vector<std::vector<std::string>> framebuffer{};
    std::vector<std::string> previousRows{};
    std::vector<RegionFrame> regions{};
    std::vector<RowFrame> rows{};
    std::vector<ColumnFrame> columns{};
    void enterTui();
    void exitTui();
    void toOrigin();
    void clear();
    void flush();
    void configure();
    void handleInput();
    void handleResize();
    std::pair<int, int> getPos();
    void beginRow();
    void endRow();
    void beginColumn();
    void endColumn();
    void beginRegion(Rect rect);
    void endRegion();
    void write(std::string_view text);
    void writeLine(std::string_view text);
    void writeLines(const std::vector<std::string>& lines);
    Rect drawBox(std::pair<int, int> res);
    Rect drawBoxAt(Rect rect);
    static void on_sig_winch(int);
    static void on_sig_int(int);
};

void TUI::enterTui(){
    configure();
    std::printf("\033[?1049h\033[?25l");
    clear();
};

void TUI::exitTui(){
    std::printf("\033[?25h\033[?1049l");
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldTerminalConfig);
};

void TUI::toOrigin(){ std::printf("\033[H"); };

static std::vector<std::string> glyphsFrom(std::string_view text){
    std::vector<std::string> glyphs;
    for(std::size_t i = 0; i < text.size(); ){
        unsigned char byte = static_cast<unsigned char>(text[i]);
        std::size_t length = 1;
        if((byte & 0x80u) == 0u) length = 1;
        else if((byte & 0xE0u) == 0xC0u) length = 2;
        else if((byte & 0xF0u) == 0xE0u) length = 3;
        else if((byte & 0xF8u) == 0xF0u) length = 4;
        if(i + length > text.size()) length = 1;
        glyphs.emplace_back(text.substr(i, length));
        i += length;
    }
    return glyphs;
}

static Rect clipped(Rect rect, Rect bounds){
    const int left = std::max(rect.x, bounds.x);
    const int top = std::max(rect.y, bounds.y);
    const int right = std::min(rect.x + rect.width, bounds.x + bounds.width);
    const int bottom = std::min(rect.y + rect.height, bounds.y + bounds.height);
    return {left, top, std::max(0, right - left), std::max(0, bottom - top)};
}

static Rect inner(Rect rect){
    return {rect.x + 1, rect.y + 1, std::max(0, rect.width - 2), std::max(0, rect.height - 2)};
}

static int glyphCount(std::string_view text){
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

void TUI::clear(){
    framebuffer.assign(
        static_cast<std::size_t>(height),
        std::vector<std::string>(static_cast<std::size_t>(width), " "));
    previousRows.assign(static_cast<std::size_t>(height), "");
    regions.clear();
    regions.push_back({{0, 0, static_cast<int>(width), static_cast<int>(height)}, 0, 0, 0, 0});
    rows.clear();
    columns.clear();
}

void TUI::flush(){
    std::string output;
    output.reserve(static_cast<std::size_t>(height) * static_cast<std::size_t>(width + 16));

    for(std::size_t y = 0; y < framebuffer.size(); ++y){
        std::string line;
        line.reserve(framebuffer[y].size() * 3);
        for(const std::string& cell : framebuffer[y]) line += cell;
        if(y >= previousRows.size()) previousRows.resize(y + 1);
        if(line == previousRows[y]) continue;
        output += "\033[" + std::to_string(y + 1) + ";1H";
        output += line;
        previousRows[y] = std::move(line);
    }

    if(!output.empty()){
        std::fwrite(output.data(), 1, output.size(), stdout);
        std::fflush(stdout);
    }
}

void TUI::handleResize(){
    if(resize.load(std::memory_order_relaxed)){
        resize = false;
        winsize ws{};
        if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0){
            width = ws.ws_col; height = ws.ws_row;
        }
        clear();
        std::printf("\033[2J\033[H");
        std::fflush(stdout);
    }
}

void TUI::handleInput(){
    handleResize();
}

void TUI::on_sig_winch(int){ if(self) self->resize.store(true, std::memory_order_relaxed); };

void TUI::on_sig_int(int){ if(self) self->running.store(false, std::memory_order_relaxed); };

void TUI::configure(){
    self = this;
    struct sigaction sa{};
    // configure terminal mode
    tcgetattr(STDIN_FILENO, &oldTerminalConfig);
    terminalConfig = oldTerminalConfig;
    terminalConfig.c_lflag &= ~ECHO;
    terminalConfig.c_lflag &= ~ICANON;
    terminalConfig.c_cc[VMIN]  = 0;
    terminalConfig.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &terminalConfig);
    winsize ws{};
    if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0){
        width = ws.ws_col; height = ws.ws_row;
    }
    clear();
    sa = {};
    sa.sa_handler = &TUI::on_sig_winch;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGWINCH, &sa, nullptr);
    sa = {};
    sa.sa_handler = &TUI::on_sig_int;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
}

std::pair<int, int> TUI::getPos(){
    termios queryConfig = terminalConfig;
    queryConfig.c_cc[VMIN] = 0;
    queryConfig.c_cc[VTIME] = 5;
    tcsetattr(STDIN_FILENO, TCSANOW, &queryConfig);
    tcflush(STDIN_FILENO, TCIFLUSH);
    std::printf("\033[6n");
    std::fflush(stdout);

    std::string response;
    char ch{};
    while(read(STDIN_FILENO, &ch, 1) == 1){
        response.push_back(ch);
        if(ch == 'R') break;
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &terminalConfig);

    int row = 1;
    int col = 1;
    if(std::sscanf(response.c_str(), "\033[%d;%dR", &row, &col) == 2)
        return {col, row};
    return {1, 1};
}

void TUI::beginRow(){
    RegionFrame& region = regions.back();
    rows.push_back({region.cursorX, region.cursorY, region.cursorX, 0, regions.size()});
}

void TUI::endRow(){
    if(rows.empty()) return;
    RowFrame finished = rows.back();
    rows.pop_back();
    RegionFrame& region = regions[finished.regionDepth - 1];
    region.cursorX = finished.originX;
    region.cursorY = std::clamp(
        finished.originY + finished.maxHeight,
        region.rect.y,
        region.rect.y + region.rect.height);
}

void TUI::beginColumn(){
    RegionFrame& region = regions.back();
    int originX = region.cursorX;
    int originY = region.cursorY;
    if(!rows.empty() && rows.back().regionDepth == regions.size() &&
       (columns.empty() || columns.back().regionDepth != regions.size())){
        originX = rows.back().nextX;
        originY = rows.back().originY;
        region.cursorX = originX;
        region.cursorY = originY;
    }
    columns.push_back({originX, originY, originY, 0, regions.size()});
}

void TUI::endColumn(){
    if(columns.empty()) return;
    ColumnFrame finished = columns.back();
    columns.pop_back();

    const int totalHeight = std::max(0, finished.nextY - finished.originY);
    if(!columns.empty() && columns.back().regionDepth == finished.regionDepth){
        ColumnFrame& parent = columns.back();
        parent.nextY = finished.originY + totalHeight;
        parent.maxWidth = std::max(parent.maxWidth, finished.maxWidth);
        RegionFrame& region = regions[parent.regionDepth - 1];
        region.cursorX = parent.originX;
        region.cursorY = parent.nextY;
        return;
    }

    if(!rows.empty() && rows.back().regionDepth == finished.regionDepth){
        RowFrame& row = rows.back();
        row.nextX = finished.originX + finished.maxWidth;
        row.maxHeight = std::max(row.maxHeight, totalHeight);
        RegionFrame& region = regions[row.regionDepth - 1];
        region.cursorX = row.nextX;
        region.cursorY = row.originY;
        return;
    }

    RegionFrame& region = regions[finished.regionDepth - 1];
    region.cursorX = finished.originX;
    region.cursorY = std::clamp(
        finished.originY + totalHeight,
        region.rect.y,
        region.rect.y + region.rect.height);
}

void TUI::beginRegion(Rect rect){
    RegionFrame parent = regions.back();
    Rect next = clipped(rect, parent.rect);
    regions.push_back({next, next.x, next.y, parent.cursorX, parent.cursorY});
}

void TUI::endRegion(){
    if(regions.size() <= 1) return;
    RegionFrame finished = regions.back();
    regions.pop_back();
    regions.back().cursorX = std::clamp(
        finished.resumeX, regions.back().rect.x, regions.back().rect.x + regions.back().rect.width);
    regions.back().cursorY = std::clamp(
        finished.resumeY, regions.back().rect.y, regions.back().rect.y + regions.back().rect.height);
}

void TUI::write(std::string_view text){
    if(regions.empty()) return;
    RegionFrame& region = regions.back();
    if(region.rect.width <= 0 || region.rect.height <= 0 ||
       region.cursorY >= region.rect.y + region.rect.height) return;
    std::vector<std::string> glyphs = glyphsFrom(text);
    int x = region.cursorX;
    for(const std::string& glyph : glyphs){
        if(x >= region.rect.x + region.rect.width) break;
        if(x >= region.rect.x && region.cursorY >= region.rect.y &&
           region.cursorY < region.rect.y + region.rect.height)
            framebuffer[static_cast<std::size_t>(region.cursorY)][static_cast<std::size_t>(x)] = glyph;
        ++x;
    }
    region.cursorX = std::min(x, region.rect.x + region.rect.width);
}

void TUI::writeLine(std::string_view text){
    if(regions.empty()) return;
    if(!columns.empty() && columns.back().regionDepth == regions.size()){
        RegionFrame& region = regions.back();
        ColumnFrame& column = columns.back();
        if(region.rect.width <= 0 || region.rect.height <= 0 ||
           column.nextY >= region.rect.y + region.rect.height) return;
        region.cursorX = column.originX;
        region.cursorY = column.nextY;
        write(text);
        column.maxWidth = std::max(column.maxWidth, glyphCount(text));
        column.nextY = region.cursorY + 1;
        region.cursorX = column.originX;
        region.cursorY = column.nextY;
        return;
    }
    if(!rows.empty() && rows.back().regionDepth == regions.size()){
        RegionFrame& region = regions.back();
        RowFrame& row = rows.back();
        if(region.rect.width <= 0 || region.rect.height <= 0 ||
           row.originY >= region.rect.y + region.rect.height) return;
        region.cursorX = row.nextX;
        region.cursorY = row.originY;
        write(text);
        row.nextX = region.cursorX;
        row.maxHeight = std::max(row.maxHeight, 1);
        return;
    }
    RegionFrame& region = regions.back();
    if(region.rect.width <= 0 || region.rect.height <= 0 ||
       region.cursorY >= region.rect.y + region.rect.height) return;
    region.cursorX = region.rect.x;
    write(text);
    for(int x = region.cursorX; x < region.rect.x + region.rect.width; ++x)
        framebuffer[static_cast<std::size_t>(region.cursorY)][static_cast<std::size_t>(x)] = " ";
    region.cursorX = region.rect.x;
    ++region.cursorY;
}

void TUI::writeLines(const std::vector<std::string>& lines){
    if(!columns.empty() && columns.back().regionDepth == regions.size()){
        RegionFrame& region = regions.back();
        ColumnFrame& column = columns.back();
        const int startX = column.originX;
        const int startY = column.nextY;
        int maxWidth = 0;
        for(std::size_t i = 0; i < lines.size(); ++i){
            region.cursorX = startX;
            region.cursorY = startY + static_cast<int>(i);
            if(region.cursorY >= region.rect.y + region.rect.height) break;
            write(lines[i]);
            maxWidth = std::max(maxWidth, glyphCount(lines[i]));
        }
        column.maxWidth = std::max(column.maxWidth, maxWidth);
        column.nextY = startY + static_cast<int>(lines.size());
        region.cursorX = column.originX;
        region.cursorY = column.nextY;
        return;
    }
    if(!rows.empty() && rows.back().regionDepth == regions.size()){
        RegionFrame& region = regions.back();
        RowFrame& row = rows.back();
        const int startX = row.nextX;
        const int startY = row.originY;
        int maxWidth = 0;
        region.cursorX = startX;
        region.cursorY = startY;
        for(std::size_t i = 0; i < lines.size(); ++i){
            region.cursorX = startX;
            region.cursorY = startY + static_cast<int>(i);
            write(lines[i]);
            maxWidth = std::max(maxWidth, glyphCount(lines[i]));
        }
        row.nextX = startX + maxWidth;
        row.maxHeight = std::max(row.maxHeight, static_cast<int>(lines.size()));
        region.cursorX = row.nextX;
        region.cursorY = row.originY;
        return;
    }
    for(const std::string& line : lines) writeLine(line);
}

Rect TUI::drawBox(std::pair<int, int> res){
    RegionFrame parent = regions.back();
    int startX = parent.cursorX;
    int startY = parent.cursorY;
    if(!columns.empty() && columns.back().regionDepth == regions.size()){
        startX = columns.back().originX;
        startY = columns.back().nextY;
    } else if(!rows.empty() && rows.back().regionDepth == regions.size()){
        startX = rows.back().nextX;
        startY = rows.back().originY;
    }
    Rect outer = clipped({startX, startY, res.first, res.second}, parent.rect);
    if(outer.width <= 0 || outer.height <= 0) return {outer.x, outer.y, 0, 0};
    if(!columns.empty() && columns.back().regionDepth == regions.size()){
        columns.back().nextY = outer.y + outer.height;
        columns.back().maxWidth = std::max(columns.back().maxWidth, outer.width);
        regions.back().cursorX = columns.back().originX;
        regions.back().cursorY = columns.back().nextY;
    } else if(!rows.empty() && rows.back().regionDepth == regions.size()){
        rows.back().nextX = outer.x + outer.width;
        rows.back().maxHeight = std::max(rows.back().maxHeight, outer.height);
        regions.back().cursorX = rows.back().nextX;
        regions.back().cursorY = rows.back().originY;
    } else {
        regions.back().cursorX = outer.x;
        regions.back().cursorY = std::min(
            outer.y + outer.height,
            regions.back().rect.y + regions.back().rect.height);
    }
    auto setCell = [&](int x, int y, std::string glyph) {
        if(x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height)) return;
        framebuffer[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] = std::move(glyph);
    };
    if(outer.height == 1){
        setCell(outer.x, outer.y, "┌");
        for(int x = 1; x < outer.width - 1; ++x) setCell(outer.x + x, outer.y, "─");
        if(outer.width > 1) setCell(outer.x + outer.width - 1, outer.y, "┐");
    } else if(outer.width == 1){
        setCell(outer.x, outer.y, "┌");
        for(int y = 1; y < outer.height - 1; ++y) setCell(outer.x, outer.y + y, "│");
        if(outer.height > 1) setCell(outer.x, outer.y + outer.height - 1, "└");
    } else {
        setCell(outer.x, outer.y, "┌");
        setCell(outer.x + outer.width - 1, outer.y, "┐");
        setCell(outer.x, outer.y + outer.height - 1, "└");
        setCell(outer.x + outer.width - 1, outer.y + outer.height - 1, "┘");
        for(int x = 1; x < outer.width - 1; ++x){
            setCell(outer.x + x, outer.y, "─");
            setCell(outer.x + x, outer.y + outer.height - 1, "─");
        }
        for(int y = 1; y < outer.height - 1; ++y){
            setCell(outer.x, outer.y + y, "│");
            setCell(outer.x + outer.width - 1, outer.y + y, "│");
        }
    }
    Rect content = inner(outer);
    regions.push_back({content, content.x, content.y, outer.x, outer.y + outer.height});
    return content;
}

Rect TUI::drawBoxAt(Rect rect){
    Rect outer = clipped(rect, regions.back().rect);
    if(outer.width <= 0 || outer.height <= 0) return {outer.x, outer.y, 0, 0};

    auto setCell = [&](int x, int y, std::string glyph) {
        if(x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height)) return;
        framebuffer[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] = std::move(glyph);
    };

    if(outer.height == 1){
        setCell(outer.x, outer.y, "┌");
        for(int x = 1; x < outer.width - 1; ++x) setCell(outer.x + x, outer.y, "─");
        if(outer.width > 1) setCell(outer.x + outer.width - 1, outer.y, "┐");
    } else if(outer.width == 1){
        setCell(outer.x, outer.y, "┌");
        for(int y = 1; y < outer.height - 1; ++y) setCell(outer.x, outer.y + y, "│");
        if(outer.height > 1) setCell(outer.x, outer.y + outer.height - 1, "└");
    } else {
        setCell(outer.x, outer.y, "┌");
        setCell(outer.x + outer.width - 1, outer.y, "┐");
        setCell(outer.x, outer.y + outer.height - 1, "└");
        setCell(outer.x + outer.width - 1, outer.y + outer.height - 1, "┘");
        for(int x = 1; x < outer.width - 1; ++x){
            setCell(outer.x + x, outer.y, "─");
            setCell(outer.x + x, outer.y + outer.height - 1, "─");
        }
        for(int y = 1; y < outer.height - 1; ++y){
            setCell(outer.x, outer.y + y, "│");
            setCell(outer.x + outer.width - 1, outer.y + y, "│");
        }
    }

    Rect content = inner(outer);
    regions.push_back({content, content.x, content.y, regions.back().cursorX, regions.back().cursorY});
    return content;
}

/*
["⣾ " "⣽ " "⣻ " "⢿ " "⡿ " "⣟ " "⣯ " "⣷ "]
["⠋" "⠙" "⠹" "⠸" "⠼" "⠴" "⠦" "⠧" "⠇" "⠏"]
*/
