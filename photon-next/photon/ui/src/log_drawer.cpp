#include "photon_ui/log_drawer.hpp"

#include "photon_ui/arena.hpp"
#include "photon_ui/client.hpp"
#include "photon_ui/clock_format.hpp"
#include "photon_ui/scene3d.hpp"
#include "photon_ui/screen.hpp"
#include "photon_ui/signal_map.hpp"
#include "photon_ui/theme.hpp"

#include "imgui.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef __EMSCRIPTEN__
EM_JS(void, photon_open_url, (const char* url), {
    var href = UTF8ToString(url);
    var a = document.createElement("a");
    a.href = href;
    a.download = "";
    a.rel = "noopener";
    a.target = "_blank";
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
});
#else
void photon_open_url(const char* url) {
    (void)url;
}
#endif

namespace photon::ui {
namespace {

constexpr float kOpenW = 300.0f;
constexpr float kTabW = 28.0f;

bool pathMentions(const std::string& path, const std::string& token) {
    return !token.empty() && path.find(token) != std::string::npos;
}

bool parseYmd(const std::string& date, int* year, int* month, int* day) {
    if (date.size() < 10) {
        return false;
    }
    int y = 0;
    int m = 0;
    int d = 0;
    if (std::sscanf(date.c_str(), "%d-%d-%d", &y, &m, &d) != 3) {
        return false;
    }
    if (y < 1970 || m < 1 || m > 12 || d < 1 || d > 31) {
        return false;
    }
    if (year != nullptr) {
        *year = y;
    }
    if (month != nullptr) {
        *month = m;
    }
    if (day != nullptr) {
        *day = d;
    }
    return true;
}

int daysInMonth(int year, int month) {
    static constexpr int kDays[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))) {
        return 29;
    }
    if (month < 1 || month > 12) {
        return 30;
    }
    return kDays[month];
}

int firstWeekday(int year, int month) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = 1;
    tm.tm_hour = 12;
    std::mktime(&tm);
    return tm.tm_wday;
}

const char* monthName(int month) {
    static constexpr const char* kNames[] = {"",    "January", "February", "March",     "April",   "May",
                                             "June", "July",    "August",   "September", "October", "November",
                                             "December"};
    if (month < 1 || month > 12) {
        return "";
    }
    return kNames[month];
}

std::string formatDuration(double seconds) {
    if (seconds >= 3600.0) {
        const int h = static_cast<int>(seconds / 3600.0);
        const int m = static_cast<int>((seconds - static_cast<double>(h) * 3600.0) / 60.0);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%dh %02dm", h, m);
        return buf;
    }
    if (seconds >= 60.0) {
        const int m = static_cast<int>(seconds / 60.0);
        const int s = static_cast<int>(seconds - static_cast<double>(m) * 60.0);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%dm %02ds", m, s);
        return buf;
    }
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%.0fs", seconds);
    return buf;
}

void formatClock(std::uint64_t ms, char* buf, std::size_t n) {
    formatLocalClock(ms, buf, n, ClockParts::TimeMs);
}

int displayHour(int hour) {
    const int h = hour % 12;
    return h == 0 ? 12 : h;
}

std::uint64_t setTimelinePart(std::uint64_t ms, int level, int value) {
    const auto sec = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &sec);
#else
    localtime_r(&sec, &tm);
#endif
    if (level == 0) {
        tm.tm_hour = value;
    } else if (level == 1) {
        tm.tm_min = value;
    } else {
        tm.tm_sec = value;
    }
    tm.tm_isdst = -1;
    const auto out = std::mktime(&tm);
    if (out < 0) {
        return ms;
    }
    return static_cast<std::uint64_t>(out) * 1000ull + (ms % 1000);
}

struct TimelineNav {
    int value = 0;
    bool changed = false;
    bool committed = false;
    bool hidden = false;
};

TimelineNav drawTimelineNavigator(int level, int value, int lo, int hi, ImVec2 size, std::uint8_t& dragging,
                                  const char* clock, bool closable) {
    static constexpr const char* kNames[] = {"HOUR", "MIN", "SEC"};
    if (lo > hi) {
        std::swap(lo, hi);
    }
    if (lo < 0) {
        lo = 0;
    }
    TimelineNav result{};
    result.value = value < lo ? lo : (value > hi ? hi : value);
    ImGui::InvisibleButton("##TimelineTime", size);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float height = size.y;
    const ImU32 fill = theme::u32(theme::withAlpha(hovered || active ? theme::foreground() : theme::card(),
                                                    hovered || active ? 0.10f : 0.92f));
    draw->AddRectFilled(min, max, fill, theme::kFrameRounding);
    draw->AddRect(min, max, theme::u32(theme::withAlpha(theme::border(), 0.45f)), theme::kFrameRounding);

    const float closeW = (level > 0 && closable) ? height : 0.0f;
    const float labelW = std::min(height * 2.2f, size.x * 0.28f);
    float clockW = size.x >= 200.0f ? std::min(height * 3.4f, size.x * 0.34f) : 0.0f;
    float railMin = min.x + closeW + labelW + 6.0f;
    float railMax = max.x - clockW - 8.0f;
    if (railMax < railMin + 20.0f) {
        clockW = 0.0f;
        railMin = min.x + closeW + 8.0f;
        railMax = max.x - 6.0f;
    }

    const float mouseX = ImGui::GetIO().MousePos.x;
    const std::uint8_t bit = static_cast<std::uint8_t>(1u << level);
    const bool closeHover = closable && level > 0 && hovered && mouseX < min.x + closeW;
    if (ImGui::IsItemActivated()) {
        if (closeHover) {
            result.hidden = true;
            dragging = 0;
        } else if (mouseX >= railMin && mouseX <= railMax) {
            dragging = static_cast<std::uint8_t>(dragging | bit);
        } else {
            dragging = static_cast<std::uint8_t>(dragging & ~bit);
        }
    }
    const int span = hi - lo;
    if (active && (dragging & bit) != 0 && railMax > railMin) {
        const float n = std::clamp((mouseX - railMin) / (railMax - railMin), 0.0f, 1.0f);
        result.value = lo + static_cast<int>(std::round(n * static_cast<float>(span)));
        if (result.value < lo) {
            result.value = lo;
        }
        if (result.value > hi) {
            result.value = hi;
        }
        result.changed = true;
    }
    if (ImGui::IsItemDeactivated()) {
        result.committed = (dragging & bit) != 0;
        dragging = static_cast<std::uint8_t>(dragging & ~bit);
    }

    if (closable && level > 0) {
        const ImVec2 c0(min.x + 3.0f, min.y + 3.0f);
        const ImVec2 c1(min.x + closeW - 3.0f, max.y - 3.0f);
        draw->AddRectFilled(c0, c1, theme::u32(theme::withAlpha(theme::danger(), closeHover ? 0.45f : 0.22f)),
                            theme::kFrameRounding);
        const ImVec2 xsz = ImGui::CalcTextSize("x");
        draw->AddText(ImVec2(c0.x + (c1.x - c0.x - xsz.x) * 0.5f, c0.y + (c1.y - c0.y - xsz.y) * 0.5f),
                      theme::u32(theme::foreground()), "x");
    }

    theme::pushSmallFont();
    const ImVec2 nameSz = ImGui::CalcTextSize(kNames[level]);
    draw->AddText(ImVec2(min.x + closeW + 8.0f, min.y + (height - nameSz.y) * 0.5f), theme::u32(theme::mutedFg()),
                  kNames[level]);
    theme::popSmallFont();

    const float baseline = max.y - 6.0f;
    draw->AddLine(ImVec2(railMin, baseline), ImVec2(railMax, baseline),
                  theme::u32(theme::withAlpha(theme::border(), 0.7f)), 1.0f);
    const int count = hi - lo + 1;
    const float railW = railMax - railMin;
    const int major = count <= 10 ? 1 : count <= 20 ? 2 : (level == 0 ? (railW >= 240.0f ? 3 : 6)
                                                                     : (railW >= 220.0f ? 10 : 15));
    for (int tick = lo; tick <= hi; ++tick) {
        const float x = count <= 1 ? (railMin + railMax) * 0.5f
                                   : railMin + railW * static_cast<float>(tick - lo) / static_cast<float>(count - 1);
        const bool selected = tick == result.value;
        const bool isMajor = ((tick - lo) % major) == 0;
        const float h = selected ? 12.0f : isMajor ? 8.0f : 3.5f;
        const ImU32 col = selected ? theme::u32(theme::primary())
                                   : theme::u32(theme::withAlpha(theme::mutedFg(), 0.7f));
        draw->AddLine(ImVec2(x, baseline - h), ImVec2(x, baseline), col, selected ? 2.0f : 1.0f);
        if (isMajor && railW >= 120.0f && (level > 0 || ((tick - lo) % (major * 2)) == 0 || selected)) {
            char label[8];
            if (level == 0) {
                std::snprintf(label, sizeof(label), "%d%s", displayHour(tick), tick < 12 ? "a" : "p");
            } else {
                std::snprintf(label, sizeof(label), "%02d", tick);
            }
            theme::pushSmallFont();
            const ImVec2 tsz = ImGui::CalcTextSize(label);
            draw->AddText(ImVec2(x - tsz.x * 0.5f, min.y + 2.0f), theme::u32(theme::mutedFg()), label);
            theme::popSmallFont();
        }
    }
    if (clockW > 0.0f && clock != nullptr) {
        theme::pushSmallFont();
        const ImVec2 tsz = ImGui::CalcTextSize(clock);
        draw->AddText(ImVec2(max.x - clockW, min.y + (height - tsz.y) * 0.5f), theme::u32(theme::foreground()),
                      clock);
        theme::popSmallFont();
    }
    return result;
}

const ctrl::Message::DriveDay* findDay(const ClientArena& arena, const std::string& date) {
    for (const auto& day : arena.driveDays) {
        if (day.date == date) {
            return &day;
        }
    }
    return nullptr;
}

std::string ymdFromMs(std::uint64_t ms) { return localYmdFromUnixMs(ms); }

std::string currentDate(const ClientArena& arena) {
    for (const auto& day : arena.driveDays) {
        if (arena.source == ctrl::SourceKind::LogReplay && pathMentions(arena.sourcePath, day.date)) {
            return day.date;
        }
    }
    if (!arena.driveDays.empty()) {
        return arena.driveDays.back().date;
    }
    return {};
}

std::string latestCatalogDate(const Client& client) {
    std::string best;
    std::uint64_t bestMs = 0;
    for (const auto& row : client.historyCatalog()) {
        if (row.minMs >= bestMs && row.maxMs > row.minMs) {
            bestMs = row.minMs;
            best = ymdFromMs(row.minMs);
        }
    }
    return best;
}

bool catalogHasDate(const Client& client, const std::string& ymd) {
    if (ymd.empty()) {
        return false;
    }
    for (const auto& row : client.historyCatalog()) {
        if (row.maxMs > row.minMs && sessionOverlapsLocalYmd(row.minMs, row.maxMs, ymd)) {
            return true;
        }
    }
    return false;
}

void drawSectionLabel(const char* text) {
    theme::pushSmallFont();
    ImGui::TextColored(theme::mutedFg(), "%s", text);
    theme::popSmallFont();
}

std::string catalogKey(const ctrl::CatalogSignal& sig) {
    if (sig.message.empty()) {
        return sig.name;
    }
    return sig.message + "." + sig.name;
}

std::string urlEncode(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (const unsigned char c : in) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out.append(buf);
        }
    }
    return out;
}

bool filterMatch(const std::string& hay, const char* needle) {
    if (needle == nullptr || needle[0] == '\0') {
        return true;
    }
    auto lower = [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    const std::size_t n = std::strlen(needle);
    if (hay.size() < n) {
        return false;
    }
    for (std::size_t i = 0; i + n <= hay.size(); ++i) {
        bool ok = true;
        for (std::size_t j = 0; j < n; ++j) {
            if (lower(hay[i + j]) != lower(needle[j])) {
                ok = false;
                break;
            }
        }
        if (ok) {
            return true;
        }
    }
    return false;
}

const char* rateLabel(int level) {
    if (level == 0) {
        return "Raw";
    }
    if (level == 1) {
        return "10 Hz";
    }
    return "1 Hz";
}

std::string formatBytes(double bytes) {
    if (bytes >= 1024.0 * 1024.0 * 1024.0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f GB", bytes / (1024.0 * 1024.0 * 1024.0));
        return buf;
    }
    if (bytes >= 1024.0 * 1024.0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0f MB", bytes / (1024.0 * 1024.0));
        return buf;
    }
    if (bytes >= 1024.0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0f KB", bytes / 1024.0);
        return buf;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f B", bytes);
    return buf;
}

}  // namespace

void LogDrawer::ensureCalendar(const UiContext& ui) {
    // Drive-day logs are the archive. A two-minute catalog row must not
    // hide them by opening the calendar on today.
    if (!catalogDateApplied_) {
        std::string latestDay;
        for (const auto& day : ui.arena.driveDays) {
            if (isReplayLogDate(day.date) && day.date > latestDay) {
                latestDay = day.date;
            }
        }
        if (!latestDay.empty()) {
            selectedDate_ = latestDay;
            catalogDateApplied_ = true;
            calReady_ = false;
        } else if (ui.client.haveHistoryCatalog()) {
            const auto latest = latestCatalogDate(ui.client);
            if (!latest.empty()) {
                selectedDate_ = latest;
            }
            catalogDateApplied_ = true;
            calReady_ = false;
        }
    }
    if (selectedDate_.empty()) {
        selectedDate_ = currentDate(ui.arena);
    }
    if (!calReady_) {
        int y = 2026;
        int m = 7;
        if (!parseYmd(selectedDate_, &y, &m, nullptr) && !ui.arena.driveDays.empty()) {
            parseYmd(ui.arena.driveDays.back().date, &y, &m, nullptr);
        }
        calYear_ = y;
        calMonth_ = m;
        calReady_ = true;
    }
}

void LogDrawer::drawTab(UiContext& ui, ImVec2 size) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1 = ImVec2(p0.x + tabW_, p0.y + size.y);
    const ImU32 bg = theme::u32(open_ ? theme::card() : theme::titlebar());
    draw->AddRectFilled(p0, p1, bg);
    draw->AddLine(ImVec2(p1.x - 1.0f, p0.y), p1, theme::u32(theme::border()));

    ImGui::SetCursorScreenPos(p0);
    if (ImGui::InvisibleButton("##logs-tab", ImVec2(tabW_, size.y))) {
        open_ = !open_;
        if (open_ && ui.client.helloOk()) {
            ui.client.requestCatalogList();
        }
    }
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) {
        draw->AddRectFilled(p0, p1, theme::u32(theme::withAlpha(theme::foreground(), 0.06f)));
    }

    const char* label = "LOGS";
    theme::pushSmallFont();
    float y = p0.y + 16.0f;
    for (const char* c = label; *c != '\0'; ++c) {
        char ch[2] = {*c, 0};
        const ImVec2 sz = ImGui::CalcTextSize(ch);
        draw->AddText(ImVec2(p0.x + (tabW_ - sz.x) * 0.5f, y),
                      theme::u32(open_ || hovered ? theme::foreground() : theme::mutedFg()), ch);
        y += sz.y + 2.0f;
    }
    theme::popSmallFont();
    const char* chevron = open_ ? "<" : ">";
    const ImVec2 csz = ImGui::CalcTextSize(chevron);
    draw->AddText(ImVec2(p0.x + (tabW_ - csz.x) * 0.5f, p1.y - 22.0f), theme::u32(theme::mutedFg()),
                  chevron);
}

void LogDrawer::sendScrub(UiContext& ui, std::uint64_t cursorMs, bool force) {
    if (ui.arena.timeline().minTimeMs != 0 && cursorMs < ui.arena.timeline().minTimeMs) {
        cursorMs = ui.arena.timeline().minTimeMs;
    }
    if (ui.arena.timeline().maxTimeMs != 0 && cursorMs > ui.arena.timeline().maxTimeMs) {
        cursorMs = ui.arena.timeline().maxTimeMs;
    }
    localCursorMs_ = cursorMs;
    ui.arena.setPreviewCursor(cursorMs, true);
    const double now = ImGui::GetTime();
    if (!force && now - lastTlSendS_ < 0.2) {
        return;
    }
    lastTlSendS_ = now;
    // Drag used to preview-only (no locate). Jumping the rail then sat on
    // Buffering / the previous GOP until mouse-up, and a missed commit never
    // opened the new time at all.
    ui.client.sendTimeline(ctrl::TimelineMode::Pause, cursorMs);
}

void LogDrawer::drawTimeline(UiContext& ui) {
    const auto& tl = ui.arena.timeline();
    if (!draggingTl_ && tl.cursorMs != 0) {
        localCursorMs_ = tl.cursorMs;
        if (localCursorMs_ != 0 &&
            (tl.cursorMs >= localCursorMs_ ? tl.cursorMs - localCursorMs_ : localCursorMs_ - tl.cursorMs) < 80) {
            ui.arena.setPreviewCursor(0, false);
        }
    }
    if (localCursorMs_ == 0) {
        localCursorMs_ = ui.arena.displayCursorMs();
    }

    drawSectionLabel("Timeline");
    char clock[40];
    formatClock(localCursorMs_, clock, sizeof(clock));
    const char* mode = "----";
    switch (static_cast<proto::TimelineMode>(tl.mode)) {
        case proto::TimelineMode::Pause:
            mode = "PAUSE";
            break;
        case proto::TimelineMode::Play:
            mode = "PLAY";
            break;
        case proto::TimelineMode::Live:
            mode = "LIVE";
            break;
        case proto::TimelineMode::Buffering:
            mode = "BUFFERING";
            break;
        default:
            break;
    }
    ImGui::TextUnformatted(clock);
    ImGui::SameLine();
    ImGui::TextColored(tl.mode == static_cast<std::uint8_t>(proto::TimelineMode::Buffering) ? theme::warning()
                                                                                           : theme::mutedFg(),
                       "%s", mode);

    const auto sec = static_cast<std::time_t>(localCursorMs_ / 1000);
    std::tm cursorDate{};
#if defined(_WIN32)
    localtime_s(&cursorDate, &sec);
#else
    localtime_r(&sec, &cursorDate);
#endif
    const bool recorded = tl.maxTimeMs > tl.minTimeMs &&
                          static_cast<proto::TimelineMode>(tl.mode) != proto::TimelineMode::Live;
    if (recorded) {
        tlLevel_ = 2;
    } else if (static_cast<proto::TimelineMode>(tl.mode) == proto::TimelineMode::Live && !draggingTl_) {
        tlLevel_ = 0;
    }
    const TimelinePartRange hours =
        recorded ? timelineHourRange(tl.minTimeMs, tl.maxTimeMs) : TimelinePartRange{0, 23};
    const TimelinePartRange minutes =
        recorded ? timelineMinuteRange(tl.minTimeMs, tl.maxTimeMs, cursorDate.tm_hour)
                 : TimelinePartRange{0, 59};
    const TimelinePartRange seconds =
        recorded ? timelineSecondRange(tl.minTimeMs, tl.maxTimeMs, cursorDate.tm_hour, cursorDate.tm_min)
                 : TimelinePartRange{0, 59};

    const auto applyNav = [&](int level, const TimelineNav& nav) {
        if (nav.hidden) {
            tlLevel_ = 0;
            tlDragging_ = 0;
            draggingTl_ = false;
            return;
        }
        if (nav.changed) {
            draggingTl_ = true;
            tlLevel_ = recorded ? 2 : level;
            sendScrub(ui, setTimelinePart(localCursorMs_, level, nav.value), false);
            formatClock(localCursorMs_, clock, sizeof(clock));
        }
        if (nav.committed) {
            draggingTl_ = false;
            sendScrub(ui, localCursorMs_, true);
            tlLevel_ = recorded ? 2 : std::min(level + 1, 2);
        }
    };

    const float barH = 28.0f;
    const float barW = ImGui::GetContentRegionAvail().x;
    ImGui::PushID("tl-hour");
    applyNav(0, drawTimelineNavigator(0, cursorDate.tm_hour, hours.lo, hours.hi, ImVec2(barW, barH),
                                      tlDragging_, clock, !recorded));
    ImGui::PopID();
    if (tlLevel_ >= 1) {
        ImGui::PushID("tl-min");
        applyNav(1, drawTimelineNavigator(1, cursorDate.tm_min, minutes.lo, minutes.hi, ImVec2(barW, barH),
                                          tlDragging_, clock, !recorded));
        ImGui::PopID();
    }
    if (tlLevel_ >= 2) {
        ImGui::PushID("tl-sec");
        applyNav(2, drawTimelineNavigator(2, cursorDate.tm_sec, seconds.lo, seconds.hi, ImVec2(barW, barH),
                                          tlDragging_, clock, !recorded));
        ImGui::PopID();
    }

    const float btnW = (ImGui::GetContentRegionAvail().x - theme::kGridGap * 2.0f) / 3.0f;
    if (ImGui::Button("Live", ImVec2(btnW, 0.0f))) {
        draggingTl_ = false;
        ui.arena.setPreviewCursor(0, false);
        ui.client.goLive();
    }
    ImGui::SameLine(0.0f, theme::kGridGap);
    if (ImGui::Button("Play", ImVec2(btnW, 0.0f))) {
        draggingTl_ = false;
        std::uint64_t t = ui.arena.displayCursorMs();
        if (t == 0) {
            t = ui.arena.timeline().minTimeMs;
        }
        ui.client.sendTimeline(ctrl::TimelineMode::Play, t);
    }
    ImGui::SameLine(0.0f, theme::kGridGap);
    if (ImGui::Button("Pause", ImVec2(btnW, 0.0f))) {
        draggingTl_ = false;
        ui.client.sendTimeline(ctrl::TimelineMode::Pause, ui.arena.displayCursorMs());
    }
}

void LogDrawer::drawCalendar(UiContext& ui) {
    drawSectionLabel("Calendar");

    const float avail = ImGui::GetContentRegionAvail().x;
    const ImVec2 nav0 = ImGui::GetCursorScreenPos();
    const float navH = ImGui::GetFrameHeight();
    if (ImGui::SmallButton("<##cal-prev")) {
        --calMonth_;
        if (calMonth_ < 1) {
            calMonth_ = 12;
            --calYear_;
        }
    }
    char header[40];
    std::snprintf(header, sizeof(header), "%s %d", monthName(calMonth_), calYear_);
    const ImVec2 headerSz = ImGui::CalcTextSize(header);
    ImGui::SetCursorScreenPos(ImVec2(nav0.x + std::max(0.0f, (avail - headerSz.x) * 0.5f), nav0.y));
    ImGui::TextUnformatted(header);
    ImGui::SetCursorScreenPos(ImVec2(nav0.x + avail - 20.0f, nav0.y));
    if (ImGui::SmallButton(">##cal-next")) {
        ++calMonth_;
        if (calMonth_ > 12) {
            calMonth_ = 1;
            ++calYear_;
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(nav0.x, nav0.y + navH + 6.0f));
    ImGui::Dummy(ImVec2(avail, 0.0f));

    const float gap = 3.0f;
    const float cellW = (avail - gap * 6.0f) / 7.0f;
    const float cellH = 28.0f;
    const float dowH = 16.0f;
    static constexpr const char* kDow[] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
    const ImVec2 grid0 = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    theme::pushSmallFont();
    for (int i = 0; i < 7; ++i) {
        const ImVec2 p(grid0.x + (cellW + gap) * static_cast<float>(i), grid0.y);
        const ImVec2 sz = ImGui::CalcTextSize(kDow[i]);
        draw->AddText(ImVec2(p.x + (cellW - sz.x) * 0.5f, p.y), theme::u32(theme::mutedFg()), kDow[i]);
    }
    theme::popSmallFont();

    const int dim = daysInMonth(calYear_, calMonth_);
    const int wd = firstWeekday(calYear_, calMonth_);
    const int cells = wd + dim;
    const int weeks = (cells + 6) / 7;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 4.0f));
    int day = 1;
    for (int row = 0; row < weeks; ++row) {
        for (int col = 0; col < 7; ++col) {
            const int slot = row * 7 + col;
            const ImVec2 p(grid0.x + (cellW + gap) * static_cast<float>(col),
                           grid0.y + dowH + 4.0f + (cellH + gap) * static_cast<float>(row));
            ImGui::SetCursorScreenPos(p);
            if (slot < wd || day > dim) {
                ImGui::Dummy(ImVec2(cellW, cellH));
                continue;
            }
            char ymd[16];
            std::snprintf(ymd, sizeof(ymd), "%04d-%02d-%02d", calYear_, calMonth_, day);
            const bool hasLog = (findDay(ui.arena, ymd) != nullptr && isReplayLogDate(ymd)) ||
                                catalogHasDate(ui.client, ymd);
            const bool selected = selectedDate_ == ymd;
            char label[12];
            std::snprintf(label, sizeof(label), "%d##cal%d", day, day);
            if (selected) {
                ImGui::PushStyleColor(ImGuiCol_Button, theme::withAlpha(theme::active(), 0.85f));
                ImGui::PushStyleColor(ImGuiCol_Text, theme::foreground());
            } else if (hasLog) {
                ImGui::PushStyleColor(ImGuiCol_Button, theme::withAlpha(theme::primary(), 0.28f));
                ImGui::PushStyleColor(ImGuiCol_Text, theme::foreground());
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, theme::withAlpha(theme::muted(), 0.12f));
                ImGui::PushStyleColor(ImGuiCol_Text, theme::mutedFg());
            }
            if (ImGui::Button(label, ImVec2(cellW, cellH))) {
                selectedDate_ = ymd;
                if (hasLog) {
                    ui.client.playDate(ymd);
                }
            }
            ImGui::PopStyleColor(2);
            ++day;
        }
    }
    ImGui::PopStyleVar();
    ImGui::SetCursorScreenPos(
        ImVec2(grid0.x, grid0.y + dowH + 4.0f + (cellH + gap) * static_cast<float>(weeks)));
    ImGui::Dummy(ImVec2(avail, 0.0f));
}

void LogDrawer::drawRuns(UiContext& ui) {
    drawSectionLabel("Runs");
    const auto* day = isReplayLogDate(selectedDate_) ? findDay(ui.arena, selectedDate_) : nullptr;
    if (!ui.client.haveHistoryCatalog() && day == nullptr) {
        ImGui::TextColored(theme::mutedFg(), "Loading runs…");
        return;
    }

    struct Row {
        std::uint64_t minMs = 0;
        std::uint64_t maxMs = 0;
        std::string id;
        bool hasVideo = false;
        std::uint64_t sampleCount = 0;
        const ctrl::Message::CatalogSession* catalog = nullptr;
        const ctrl::Message::DriveDay* extractDay = nullptr;
        std::uint64_t extractCursor = 0;
    };
    std::vector<Row> rows;
    if (ui.client.haveHistoryCatalog()) {
        for (const auto& sess : ui.client.historyCatalog()) {
            if (sess.maxMs <= sess.minMs) {
                continue;
            }
            if (!selectedDate_.empty() &&
                !sessionOverlapsLocalYmd(sess.minMs, sess.maxMs, selectedDate_)) {
                continue;
            }
            Row row;
            row.minMs = sess.minMs;
            row.maxMs = sess.maxMs;
            row.id = sess.id;
            row.hasVideo = sess.hasVideo;
            row.sampleCount = sess.sampleCount;
            row.catalog = &sess;
            rows.push_back(std::move(row));
        }
    }

    const auto rangesOverlap = [](std::uint64_t a0, std::uint64_t a1, std::uint64_t b0, std::uint64_t b1) {
        return a1 > b0 && a0 < b1;
    };
    if (day != nullptr) {
        auto coveredByCatalog = [&](std::uint64_t t0, std::uint64_t t1) {
            if (t1 <= t0) {
                t1 = t0 + 1;
            }
            for (const auto& row : rows) {
                if (row.catalog != nullptr && rangesOverlap(row.minMs, row.maxMs, t0, t1)) {
                    return true;
                }
            }
            return false;
        };
        if (day->sessions.empty()) {
            if (rows.empty()) {
                Row row;
                row.id = day->date;
                row.extractDay = day;
                rows.push_back(std::move(row));
            }
        } else {
            for (int i = 0; i < static_cast<int>(day->sessions.size()); ++i) {
                const auto& sess = day->sessions[static_cast<std::size_t>(i)];
                std::uint64_t t0 = sess.startMs;
                if (t0 == 0 && i > 0) {
                    double acc = 0.0;
                    for (int j = 0; j < i; ++j) {
                        acc += day->sessions[static_cast<std::size_t>(j)].durationS;
                    }
                    t0 = static_cast<std::uint64_t>(acc * 1000.0);
                }
                const std::uint64_t t1 =
                    sess.durationS > 0.0 ? t0 + static_cast<std::uint64_t>(sess.durationS * 1000.0 + 0.5)
                                         : t0 + 1;
                if (t0 != 0 && unixMsLooksLikeWall(t0) && coveredByCatalog(t0, t1)) {
                    continue;
                }
                Row row;
                row.minMs = t0;
                row.maxMs = t1;
                row.id = sess.id.empty() ? day->date : sess.id;
                row.extractDay = day;
                row.extractCursor = t0;
                rows.push_back(std::move(row));
            }
        }
    }

    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.minMs != b.minMs) {
            return a.minMs > b.minMs;
        }
        return a.id > b.id;
    });
    if (rows.empty()) {
        ImGui::TextColored(theme::mutedFg(), "No runs this day.");
        return;
    }

    const std::string& pinned = ui.arena.pinnedCatalogId();
    const bool onExtractDay =
        day != nullptr && ui.arena.source == ctrl::SourceKind::LogReplay &&
        (ui.arena.sourcePath == day->path || pathMentions(ui.arena.sourcePath, day->date));
    const std::uint64_t cursor = ui.arena.timeline().cursorMs;
    ImGui::BeginChild("##runs-list", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const Row& row = rows[static_cast<std::size_t>(i)];
        const double durS = row.maxMs > row.minMs ? static_cast<double>(row.maxMs - row.minMs) / 1000.0
                                                  : (row.extractDay != nullptr ? row.extractDay->durationS : 0.0);
        char label[160];
        if (row.minMs != 0 && unixMsLooksLikeWall(row.minMs)) {
            char t0[32];
            char t1[32];
            formatLocalClock(row.minMs, t0, sizeof(t0), ClockParts::Time);
            formatLocalClock(row.maxMs != 0 ? row.maxMs : row.minMs, t1, sizeof(t1), ClockParts::Time);
            std::snprintf(label, sizeof(label), "%s–%s  %s%s", t0, t1, formatDuration(durS).c_str(),
                          row.hasVideo ? "  vid" : "");
        } else {
            std::snprintf(label, sizeof(label), "%s  %s%s", row.id.c_str(), formatDuration(durS).c_str(),
                          row.hasVideo ? "  vid" : "");
        }
        bool sel = row.catalog != nullptr && pinned == row.catalog->id;
        if (!sel && row.extractDay != nullptr && onExtractDay) {
            sel = row.extractCursor == 0
                      ? pinned.empty()
                      : (cursor >= row.minMs && (row.maxMs == 0 || cursor < row.maxMs));
        }
        ImGui::PushID(i);
        const float csvW = row.catalog != nullptr ? 44.0f : 0.0f;
        const float labelW = std::max(16.0f, ImGui::GetContentRegionAvail().x - csvW);
        if (ImGui::Selectable(label, sel, ImGuiSelectableFlags_None, ImVec2(labelW, 0.0f))) {
            if (row.catalog != nullptr) {
                ui.client.playCatalogSession(*row.catalog);
            } else if (row.extractDay != nullptr) {
                ui.client.playLog(row.extractDay->path, row.extractCursor);
            }
        }
        if (ImGui::IsItemHovered()) {
            if (row.catalog != nullptr) {
                ImGui::SetTooltip("%s\nsamples=%llu", row.catalog->id.c_str(),
                                  static_cast<unsigned long long>(row.sampleCount));
            } else {
                ImGui::SetTooltip("%s", row.id.c_str());
            }
        }
        if (row.catalog != nullptr) {
            ImGui::SameLine(0.0f, 4.0f);
            if (ImGui::SmallButton("CSV")) {
                exportOpen_ = true;
                exportSession_ = row.catalog->id;
                exportDurMs_ = row.maxMs > row.minMs ? row.maxMs - row.minMs : 0;
                exportSamples_ = row.sampleCount;
                setMapsOverlayHoldOff(true);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Download selected signals as CSV");
            }
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void LogDrawer::drawPanel(UiContext& ui, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::card());
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    ImGui::BeginChild("##logs-panel", size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    ImGui::Text("Logs");
    theme::pushSmallFont();
    ImGui::TextColored(theme::mutedFg(), "Pick a run. Live to leave.");
    theme::popSmallFont();
    std::vector<std::string> dates;
    for (const auto& day : ui.arena.driveDays) {
        if (isReplayLogDate(day.date)) {
            dates.push_back(day.date);
        }
    }
    for (const auto& row : ui.client.historyCatalog()) {
        if (row.maxMs <= row.minMs) {
            continue;
        }
        const auto a = localYmdFromUnixMs(row.minMs);
        const auto b = localYmdFromUnixMs(row.maxMs);
        if (isReplayLogDate(a)) {
            dates.push_back(a);
        }
        if (isReplayLogDate(b) && b != a) {
            dates.push_back(b);
        }
    }
    std::sort(dates.begin(), dates.end());
    dates.erase(std::unique(dates.begin(), dates.end()), dates.end());
    if (!dates.empty()) {
        theme::pushSmallFont();
        for (int i = 0; i < static_cast<int>(dates.size()); ++i) {
            if (i > 0) {
                ImGui::SameLine();
            }
            if (ImGui::SmallButton((dates[static_cast<std::size_t>(i)] + "##logdate").c_str())) {
                selectedDate_ = dates[static_cast<std::size_t>(i)];
                calReady_ = false;
                ui.client.playDate(selectedDate_);
            }
        }
        theme::popSmallFont();
    }
    ImGui::Separator();
    drawTimeline(ui);
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    ImGui::Separator();
    drawCalendar(ui);
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    ImGui::Separator();
    drawRuns(ui);
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void LogDrawer::draw(UiContext& ui, ImVec2 size) {
    tabW_ = kTabW;
    ensureCalendar(ui);
    const float target = open_ ? kOpenW : 0.0f;
    const float step = ImGui::GetIO().DeltaTime * 1400.0f;
    if (width_ < target) {
        width_ = std::min(target, width_ + step);
    } else if (width_ > target) {
        width_ = std::max(target, width_ - step);
    }

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    drawTab(ui, size);
    if (width_ > 1.0f) {
        ImGui::SetCursorScreenPos(ImVec2(origin.x + tabW_, origin.y));
        drawPanel(ui, ImVec2(width_, size.y));
    }
    setMapsOverlayHoldOff(exportOpen_);
}

void LogDrawer::startExportDownload(const std::vector<std::string>& keys) const {
    if (exportSession_.empty()) {
        return;
    }
    std::string url = "/export.csv?session=";
    url += urlEncode(exportSession_);
    url += "&level=";
    url += std::to_string(exportLevel_);
    if (exportScope_ == ExportScope::All) {
        url += "&signals=all";
    } else {
        for (const auto& key : keys) {
            url += "&sig=";
            url += urlEncode(key);
        }
    }
    photon_open_url(url.c_str());
}

void LogDrawer::drawExportPopup(UiContext& ui) {
    if (exportOpen_ && !ImGui::IsPopupOpen("Export CSV")) {
        ImGui::OpenPopup("Export CSV");
    }
    setMapsOverlayHoldOff(exportOpen_);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 center = vp != nullptr ? vp->GetCenter() : ImGui::GetCursorScreenPos();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520.0f, 560.0f), ImGuiCond_Appearing);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, theme::kContentWindowPadding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, theme::kPopupRounding);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, theme::card());
    ImGui::PushStyleColor(ImGuiCol_Border, theme::border());
    if (!ImGui::BeginPopupModal("Export CSV", &exportOpen_,
                                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
        return;
    }

    ImGui::TextUnformatted(exportSession_.c_str());
    theme::pushSmallFont();
    ImGui::TextColored(theme::mutedFg(), "%s  %s", formatDuration(static_cast<double>(exportDurMs_) / 1000.0).c_str(),
                       rateLabel(exportLevel_));
    theme::popSmallFont();
    ImGui::Separator();

    ImGui::TextColored(theme::mutedFg(), "Rate");
    if (ImGui::RadioButton("1 Hz", exportLevel_ == 2)) {
        exportLevel_ = 2;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("10 Hz", exportLevel_ == 1)) {
        exportLevel_ = 1;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Raw", exportLevel_ == 0)) {
        exportLevel_ = 0;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Every stored sample. A full run is about a gigabyte.");
    }

    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    ImGui::TextColored(theme::mutedFg(), "Signals");
    if (ImGui::RadioButton("Mapped", exportScope_ == ExportScope::Mapped)) {
        exportScope_ = ExportScope::Mapped;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Channels already on Pit, Dynamics, and 3D.");
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Selected", exportScope_ == ExportScope::Pick)) {
        exportScope_ = ExportScope::Pick;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("All", exportScope_ == ExportScope::All)) {
        exportScope_ = ExportScope::All;
    }

    std::vector<std::string> mapped;
    ui.arena.signalMap().collectMappedKeys(mapped, MapScreen::Pit | MapScreen::Dynamics | MapScreen::Scene3d);
    std::vector<std::string> downloadKeys = mapped;
    if (exportScope_ == ExportScope::Pick) {
        downloadKeys.assign(exportPicked_.begin(), exportPicked_.end());
    }

    if (exportScope_ == ExportScope::Pick) {
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##export-filter", "Filter signals", exportFilter_, sizeof(exportFilter_));
        if (ImGui::SmallButton("Mapped##export-add-mapped")) {
            exportPicked_.insert(mapped.begin(), mapped.end());
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##export-clear")) {
            exportPicked_.clear();
        }
        ImGui::BeginChild("##export-sigs", ImVec2(0.0f, -88.0f),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        if (!ui.arena.haveSignalCatalog) {
            ImGui::TextColored(theme::mutedFg(), "Signal list is still loading.");
        } else {
            int shown = 0;
            for (const auto& sig : ui.arena.signalCatalog) {
                const std::string key = catalogKey(sig);
                if (!filterMatch(key, exportFilter_)) {
                    continue;
                }
                ++shown;
                bool on = exportPicked_.count(key) != 0;
                ImGui::PushID(static_cast<int>(sig.id));
                if (ImGui::Checkbox(key.c_str(), &on)) {
                    if (on) {
                        exportPicked_.insert(key);
                    } else {
                        exportPicked_.erase(key);
                    }
                }
                ImGui::PopID();
            }
            if (shown == 0) {
                ImGui::TextColored(theme::mutedFg(), "No signals match.");
            }
        }
        ImGui::EndChild();
    } else {
        ImGui::BeginChild("##export-summary", ImVec2(0.0f, -88.0f),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        if (exportScope_ == ExportScope::Mapped) {
            if (mapped.empty()) {
                ImGui::TextColored(theme::mutedFg(), "No mapped dash signals yet.");
            } else {
                ImGui::Text("%d mapped signals", static_cast<int>(mapped.size()));
                theme::pushSmallFont();
                for (const auto& key : mapped) {
                    ImGui::TextUnformatted(key.c_str());
                }
                theme::popSmallFont();
            }
        } else {
            ImGui::TextUnformatted("Every signal stored in this run.");
            if (exportLevel_ == 0) {
                ImGui::TextColored(theme::warning(), "Raw + all signals is about a gigabyte.");
            }
        }
        ImGui::EndChild();
    }

    const double durS = static_cast<double>(exportDurMs_) / 1000.0;
    double rows = 0.0;
    if (exportScope_ == ExportScope::All && exportLevel_ == 0 && exportSamples_ > 0) {
        rows = static_cast<double>(exportSamples_);
    } else if (exportScope_ != ExportScope::All) {
        const double hz = exportLevel_ == 0 ? 50.0 : (exportLevel_ == 1 ? 10.0 : 1.0);
        rows = static_cast<double>(downloadKeys.size()) * durS * hz;
    } else {
        const double hz = exportLevel_ == 1 ? 10.0 : 1.0;
        rows = std::max(1.0, durS) * hz * 200.0;
    }
    theme::pushSmallFont();
    if (exportScope_ == ExportScope::All && exportLevel_ == 0) {
        ImGui::TextColored(theme::warning(), "About %s. Excel will not open this.",
                           formatBytes(rows * 70.0).c_str());
    } else if (exportScope_ == ExportScope::Pick && downloadKeys.empty()) {
        ImGui::TextColored(theme::mutedFg(), "Pick at least one signal.");
    } else if (exportScope_ == ExportScope::Mapped && mapped.empty()) {
        ImGui::TextColored(theme::mutedFg(), "Wait for the signal map, or pick signals.");
    } else {
        ImGui::TextColored(theme::mutedFg(), "About %s", formatBytes(rows * 70.0).c_str());
    }
    theme::popSmallFont();

    const bool canGo = !exportSession_.empty() &&
                       (exportScope_ == ExportScope::All ||
                        (exportScope_ == ExportScope::Mapped && !mapped.empty()) ||
                        (exportScope_ == ExportScope::Pick && !downloadKeys.empty()));
    if (!canGo) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Download", ImVec2(120.0f, 0.0f))) {
        startExportDownload(downloadKeys);
        exportOpen_ = false;
        ImGui::CloseCurrentPopup();
    }
    if (!canGo) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f))) {
        exportOpen_ = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
    ImGui::EndPopup();
}

}  // namespace photon::ui
