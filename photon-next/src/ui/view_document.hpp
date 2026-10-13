#pragma once
#include <string>
#include <string_view>
#include "ui/view_types.hpp"
namespace photon::ui {
std::string serializeCustomViews(const CustomViewDocument& doc);
bool parseCustomViews(std::string_view text, CustomViewDocument* out, std::string* error);
void migrateGgAxes(ViewCell& cell);
}  // namespace photon::ui
