#include <cstdio>
#include "base/json.hpp"
#include "ui/chart_grid_model.hpp"
#include "ui/view_document.hpp"

int main() {
    using namespace photon::ui;
    CustomViewDocument doc;
    std::string error;
    const char* legacy =
        R"({"schemaVersion":1,"panels":[{"id":"legacy","name":"Legacy","layout":{"columns":12,"rowHeight":160,"locked":false},"widgets":[{"id":"trace","kind":"plot","rect":{"x":6,"y":2,"w":6,"h":2},"plot":{"type":"line","title":"Trace","timeWindowSeconds":30,"sources":[{"signalName":"speed","label":"Speed"}]}}]}]})";
    if (!parseCustomViews(legacy, &doc, &error)) {
        std::printf("parse: %s\n", error.c_str());
        return 1;
    }
    const auto& v = doc.views.at(0);
    const auto& c = v.cells.at(0);
    if (v.columns != 24 || v.rowHeight != 24 || v.locked || c.rect.x != 12 || c.rect.y != 13 ||
        c.rect.w != 12 || c.rect.h != 13)
        return 2;
    doc.views[0].cells[0].yUser = true;
    doc.views[0].cells[0].yUserLo = -10;
    doc.views[0].cells[0].yUserHi = 10;
    doc.views[0].cells[0].yUserRightLo = 100;
    doc.views[0].cells[0].yUserRightHi = 300;
    const auto saved = serializeCustomViews(doc);
    CustomViewDocument again;
    if (!parseCustomViews(saved, &again, &error) || again.schemaVersion != 2) return 3;
    if (!sameGridRect(c.rect, again.views[0].cells[0].rect)) return 4;
    if (again.views[0].locked) return 7;
    if (!again.views[0].cells[0].yUser || again.views[0].cells[0].yUserRightLo != 100 ||
        again.views[0].cells[0].yUserRightHi != 300) return 8;
    if (again.views[0].cells[0].sources[0].signalName != "speed") return 5;
    if (parseCustomViews(R"({"schemaVersion":99})", &again, &error)) return 6;
    std::printf("custom view v1 migration and v2 round trip passed\n");
    return 0;
}
