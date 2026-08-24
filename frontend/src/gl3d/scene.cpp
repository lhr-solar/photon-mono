#include "photon_ui/scene3d.hpp"

#include "photon/protocol.hpp"
#include "photon_ui/gl3d/model.hpp"
#include "photon_ui/gl3d/renderer.hpp"
#include "photon_ui/signal_map.hpp"
#include "photon_ui/theme.hpp"
#include "photon_ui/widgets.hpp"

#include "imgui.h"

#include <emscripten.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace photon::ui {
namespace {

struct Pose {
    float steerDeg = 0.0f;
    float steerWheelDeg = 0.0f;
    float spinFl = 0.0f;
    float spinFr = 0.0f;
    float spinRl = 0.0f;
    float spinRr = 0.0f;
    float suspFl = 0.0f;
    float suspFr = 0.0f;
    float suspRear = 0.0f;
    float rollDeg = 0.0f;
    float pitchDeg = 0.0f;
};

struct Camera {
    float yaw = 68.0f;
    float pitch = 16.0f;
    float distance = 8.0f;
    float targetX = 0.0f;
    float targetY = 0.0f;
    float targetZ = 0.0f;
};

constexpr float kFovYDeg = 42.0f;
constexpr float kFrameMargin = 1.15f;

enum class CarSlot : int { NewDyn = 0, NewCar = 1 };

constexpr const char* kCarLabels[] = {"Dynamics car (new_dyn)", "Display car (newCar)"};
constexpr const char* kCarUrls[] = {"models/new_dyn.glb", "models/newCar.glb"};
constexpr const char* kCarNames[] = {"new_dyn.glb", "newCar.glb"};
constexpr const char* kTrackUrl = "models/s26_simple_track.glb";

struct CachedModel {
    gl3d::LoadedModel model;
    bool ready = false;
    bool loading = false;
    bool failed = false;
};

class Scene3dRuntime;

struct FetchJob {
    Scene3dRuntime* rt = nullptr;
    int kind = 0;  // 0/1 = car slot, 2 = track
};

class Scene3dRuntime {
public:
    void ensure() {
        if (ready_) {
            return;
        }
        if (!renderer_.ensureProgram()) {
            return;
        }
        gl3d::buildGround(renderer_, ground_);
        ready_ = true;
        requestCar(CarSlot::NewDyn);
    }

    void resetView() { frameCamera(activeModel()); }

    void setWheelSpin(bool on) { wheelSpin_ = on; }
    bool wheelSpin() const { return wheelSpin_; }

    const gl3d::LoadedModel& model() const { return activeModel(); }
    float modelRadius() const { return modelRadius_; }
    float cameraDistance() const { return cam_.distance; }
    float framedDistance() const { return framedDistance_; }
    int drawCalls() const { return lastDraws_; }
    bool synthetic() const { return synthetic_; }
    double speedValue() const { return speedValue_; }
    const char* speedUnit() const { return speedUnit_; }
    bool ready() const { return ready_; }
    bool loading() const { return cars_[selected_].loading || (wantTrack_ && track_.loading); }
    const char* loadStatus() const { return loadStatus_; }

    void updateProgress(int kind, int loaded, int total) {
        const char* name = (kind == 2) ? "s26_simple_track.glb" : (kind >= 0 && kind < 2) ? kCarNames[kind] : "?";
        if (total > 0) {
            const int pct = static_cast<int>((100.0 * static_cast<double>(loaded)) / static_cast<double>(total));
            std::snprintf(loadStatus_, sizeof(loadStatus_), "loading %s %d%%", name, pct);
        } else {
            std::snprintf(loadStatus_, sizeof(loadStatus_), "loading %s", name);
        }
    }
    int selectedCar() const { return selected_; }
    bool wantTrack() const { return wantTrack_; }
    bool trackReady() const { return track_.ready; }
    const char* trackError() const { return track_.model.error.c_str(); }

    void selectCar(int slot) {
        if (slot < 0 || slot > 1) {
            return;
        }
        selected_ = slot;
        requestCar(static_cast<CarSlot>(slot));
        if (cars_[selected_].ready) {
            useCar(selected_);
        } else {
            displayed_ = -1;
            itemMats_.clear();
            cam_ = Camera{};
        }
    }

    void setWantTrack(bool on) {
        wantTrack_ = on;
        if (on) {
            requestTrack();
        }
    }

    void finishFetch(int kind, const void* data, int size) {
        if (kind == 2) {
            track_.loading = false;
            gl3d::LoadedModel loaded;
            if (!gl3d::loadGltfMemory(renderer_, loaded, data, static_cast<std::size_t>(size),
                                      "s26_simple_track.glb")) {
                std::fprintf(stderr, "[gl3d] track: %s\n", loaded.error.c_str());
                track_.failed = true;
                track_.model.error = loaded.error.empty() ? "track parse failed" : loaded.error;
                std::snprintf(loadStatus_, sizeof(loadStatus_), "track failed");
                return;
            }
            if (track_.ready) {
                renderer_.destroyModel(track_.model.gpu);
            }
            track_.model = std::move(loaded);
            track_.ready = true;
            track_.failed = false;
            trackMats_.assign(track_.model.gpu.draws.size(), gl3d::identity());
            std::snprintf(loadStatus_, sizeof(loadStatus_), "track ready  %u verts",
                          static_cast<unsigned>(track_.model.gpu.mesh.vertexCount));
            return;
        }
        if (kind < 0 || kind > 1) {
            return;
        }
        cars_[kind].loading = false;
        gl3d::LoadedModel loaded;
        if (!gl3d::loadGltfMemory(renderer_, loaded, data, static_cast<std::size_t>(size), kCarNames[kind])) {
            std::fprintf(stderr, "[gl3d] %s: %s\n", kCarNames[kind], loaded.error.c_str());
            cars_[kind].failed = true;
            cars_[kind].model.error = loaded.error.empty() ? "parse failed" : loaded.error;
            std::snprintf(loadStatus_, sizeof(loadStatus_), "%s failed", kCarNames[kind]);
            return;
        }
        if (cars_[kind].ready) {
            renderer_.destroyModel(cars_[kind].model.gpu);
        }
        cars_[kind].model = std::move(loaded);
        cars_[kind].ready = true;
        cars_[kind].failed = false;
        std::snprintf(loadStatus_, sizeof(loadStatus_), "%s ready", kCarNames[kind]);
        if (selected_ == kind) {
            useCar(kind);
        }
    }

    void fetchFailed(int kind) {
        if (kind == 2) {
            track_.loading = false;
            track_.failed = true;
            track_.model.error = "fetch failed";
            std::snprintf(loadStatus_, sizeof(loadStatus_), "track fetch failed");
            return;
        }
        if (kind < 0 || kind > 1) {
            return;
        }
        cars_[kind].loading = false;
        cars_[kind].failed = true;
        cars_[kind].model.error = "fetch failed";
        std::snprintf(loadStatus_, sizeof(loadStatus_), "%s fetch failed", kCarNames[kind]);
    }

    unsigned render(const ClientArena& arena, int width, int height) {
        ensure();
        if (!ready_) {
            return 0;
        }
        step(arena);
        const gl3d::LoadedModel& car = activeModel();
        if (car.gpu.loaded) {
            if (itemMats_.size() != car.gpu.draws.size()) {
                itemMats_.assign(car.gpu.draws.size(), gl3d::identity());
            }
            const gl3d::Mat4 chassis =
                car.rigid ? gl3d::identity() : gl3d::chassisModel(pose_.rollDeg, pose_.pitchDeg);
            for (std::size_t i = 0; i < car.gpu.draws.size(); ++i) {
                itemMats_[i] = chassis * gl3d::itemModel(car.gpu.draws[i], car.kin, pose_.steerDeg,
                                                         pose_.steerWheelDeg, pose_.spinFl, pose_.spinFr, pose_.spinRl,
                                                         pose_.spinRr, pose_.suspFl, pose_.suspFr, pose_.suspRear);
            }
        } else {
            itemMats_.clear();
        }
        const bool useTrack = wantTrack_ && track_.ready;
        const float r = std::max(modelRadius_, 1.0f);
        const float farPlane = std::max(useTrack ? 260.0f : 16.0f, cam_.distance + r * 2.0f + 16.0f);
        const float aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
        const gl3d::Mat4 proj = gl3d::perspective(gl3d::radians(kFovYDeg), aspect, 0.15f, farPlane);
        const float yaw = gl3d::radians(cam_.yaw);
        const float pitch = gl3d::radians(cam_.pitch);
        const gl3d::Vec3 target{cam_.targetX, cam_.targetY, cam_.targetZ};
        const gl3d::Vec3 eye{target.x + cam_.distance * std::cos(pitch) * std::sin(yaw),
                             target.y + cam_.distance * std::sin(pitch),
                             target.z + cam_.distance * std::cos(pitch) * std::cos(yaw)};
        const gl3d::Mat4 view = gl3d::lookAt(eye, target, {0.0f, 1.0f, 0.0f});
        const gl3d::Mat4 groundM = gl3d::identity();
        const gl3d::MeshGpu* extra = useTrack ? nullptr : &ground_;
        const gl3d::ModelGpu* trackGpu = useTrack ? &track_.model.gpu : nullptr;
        const gl3d::Mat4* trackMats = useTrack && !trackMats_.empty() ? trackMats_.data() : nullptr;
        const gl3d::Vec3 toCam = gl3d::normalize(eye - target);
        gl3d::Lights lights{};
        lights.fillDir = gl3d::normalize({toCam.x, toCam.y + 0.35f, toCam.z});
        const ImVec4 clear = theme::mixColor(theme::background(), theme::muted(), 0.55f);
        lights.clearColor = {clear.x, clear.y, clear.z};
        lights.exposure = kExposure;
        const unsigned tex =
            renderer_.render(width, height, view, proj, car.gpu, itemMats_.data(), extra, &groundM, trackGpu,
                             trackMats, lights);
        lastDraws_ = renderer_.lastDrawCalls();
        return tex;
    }

    void orbit(float dx, float dy) {
        cam_.yaw += dx * 0.35f;
        cam_.pitch = std::clamp(cam_.pitch + dy * 0.25f, -8.0f, 78.0f);
    }

    void zoom(float wheel) {
        const float r = std::max(modelRadius_, 1.0f);
        cam_.distance = std::clamp(cam_.distance * (1.0f - wheel * 0.12f), r * 0.2f, r * 8.0f);
    }

private:
    gl3d::Renderer renderer_{};
    CachedModel cars_[2]{};
    CachedModel track_{};
    gl3d::LoadedModel empty_{};
    gl3d::MeshGpu ground_{};
    std::vector<gl3d::Mat4> itemMats_{};
    std::vector<gl3d::Mat4> trackMats_{};
    Camera cam_{};
    Pose pose_{};
    int selected_ = static_cast<int>(CarSlot::NewDyn);
    int displayed_ = -1;
    bool ready_ = false;
    bool wheelSpin_ = true;
    static constexpr float kExposure = 2.0f;
    bool synthetic_ = true;
    bool wantTrack_ = false;
    bool suspRefValid_[3]{};
    float suspRef_[3]{};
    double speedValue_ = 0.0;
    const char* speedUnit_ = "m/s";
    int lastDraws_ = 0;
    float modelRadius_ = 0.0f;
    float framedDistance_ = 0.0f;
    char loadStatus_[128]{"loading new_dyn.glb"};

    const gl3d::LoadedModel& activeModel() const {
        if (displayed_ >= 0 && displayed_ < 2 && cars_[displayed_].ready) {
            return cars_[displayed_].model;
        }
        return empty_;
    }

    void useCar(int slot) {
        if (slot < 0 || slot > 1 || !cars_[slot].ready) {
            return;
        }
        displayed_ = slot;
        itemMats_.assign(cars_[slot].model.gpu.draws.size(), gl3d::identity());
        frameCamera(cars_[slot].model);
    }

    void frameCamera(const gl3d::LoadedModel& model) {
        if (!model.haveAabb) {
            cam_ = Camera{};
            modelRadius_ = 0.0f;
            framedDistance_ = cam_.distance;
            return;
        }
        const gl3d::Vec3 c = (model.aabbMin + model.aabbMax) * 0.5f;
        const gl3d::Vec3 e = model.aabbMax - model.aabbMin;
        const float radius = std::max(model.radius > 0.0f ? model.radius : gl3d::length(e) * 0.5f, 1e-3f);
        const float halfFov = gl3d::radians(kFovYDeg) * 0.5f;
        const float fitted = (radius / std::sin(halfFov)) * kFrameMargin;
        cam_.targetX = c.x;
        cam_.targetY = c.y;
        cam_.targetZ = c.z;
        cam_.distance = std::max(fitted, radius * 1.05f);
        cam_.yaw = 68.0f;
        cam_.pitch = 16.0f;
        modelRadius_ = radius;
        framedDistance_ = cam_.distance;
        std::fprintf(stderr, "[gl3d] frame %s radius=%.3f distance=%.3f center=(%.3f,%.3f,%.3f)\n",
                     model.name.c_str(), radius, cam_.distance, c.x, c.y, c.z);
    }

    void requestCar(CarSlot slot) {
        const int i = static_cast<int>(slot);
        if (i < 0 || i > 1) {
            return;
        }
        if (cars_[i].ready || cars_[i].loading) {
            return;
        }
        cars_[i].loading = true;
        cars_[i].failed = false;
        std::snprintf(loadStatus_, sizeof(loadStatus_), "loading %s 0%%", kCarNames[i]);
        startFetch(kCarUrls[i], i);
    }

    void requestTrack() {
        if (track_.ready || track_.loading) {
            return;
        }
        track_.loading = true;
        track_.failed = false;
        std::snprintf(loadStatus_, sizeof(loadStatus_), "loading s26_simple_track.glb 0%%");
        startFetch(kTrackUrl, 2);
    }

    void startFetch(const char* url, int kind) {
        auto* job = new FetchJob{this, kind};
        emscripten_async_wget2_data(url, "GET", "", job, 1, &Scene3dRuntime::onWget2Ok,
                                   &Scene3dRuntime::onWget2Err, &Scene3dRuntime::onWget2Progress);
    }

    static void onWget2Ok(unsigned /*handle*/, void* arg, void* data, unsigned size) {
        auto* job = static_cast<FetchJob*>(arg);
        if (job != nullptr && job->rt != nullptr) {
            job->rt->finishFetch(job->kind, data, static_cast<int>(size));
        }
        delete job;
    }

    static void onWget2Err(unsigned /*handle*/, void* arg, int /*http*/, const char* /*desc*/) {
        auto* job = static_cast<FetchJob*>(arg);
        if (job != nullptr && job->rt != nullptr) {
            job->rt->fetchFailed(job->kind);
        }
        delete job;
    }

    static void onWget2Progress(unsigned /*handle*/, void* arg, int loaded, int total) {
        auto* job = static_cast<FetchJob*>(arg);
        if (job != nullptr && job->rt != nullptr) {
            job->rt->updateProgress(job->kind, loaded, total);
        }
    }

    static float lerpToward(float cur, float target, float dt) {
        const float a = 1.0f - std::exp(-10.0f * dt);
        return cur + (target - cur) * a;
    }

    void step(const ClientArena& arena) {
        const ImGuiIO& io = ImGui::GetIO();
        const bool paused = static_cast<proto::TimelineMode>(arena.timeline().mode) == proto::TimelineMode::Pause;
        const float dt = paused ? 0.0f : io.DeltaTime;

        const SignalMap& map = arena.signalMap();
        const Vital steer = map.vital("steering_angle", arena);
        const Vital rpmFl = map.vital("wheel_rpm_fl", arena);
        const Vital rpmFr = map.vital("wheel_rpm_fr", arena);
        const Vital suspFl = map.vital("susp_fl", arena);
        const Vital suspFr = map.vital("susp_fr", arena);
        const Vital suspRr = map.vital("susp_rear", arena);
        const Vital speed = map.vital("vehicle_speed", arena);

        speedValue_ = speed.have ? speed.value : 0.0;
        speedUnit_ = (speed.unit != nullptr && speed.unit[0] != '\0') ? speed.unit : "m/s";

        const bool mappedLive = (steer.mapped && steer.have) || (rpmFl.mapped && rpmFl.have) ||
                                (rpmFr.mapped && rpmFr.have) || (suspFl.mapped && suspFl.have) ||
                                (suspFr.mapped && suspFr.have) || (suspRr.mapped && suspRr.have);
        synthetic_ = !mappedLive;

        if (synthetic_) {
            // Photon's applyDynamicsJiggle (gui.cpp): 5s period, ±35° road, ±540° wheel.
            const float t = static_cast<float>(ImGui::GetTime());
            const float phase = t * (gl3d::kPi * 2.0f / 5.0f);
            pose_.steerDeg = std::sin(phase) * 35.0f;
            pose_.steerWheelDeg = std::sin(phase) * 540.0f;
            if (wheelSpin_) {
                pose_.spinFl = std::remainder(t * 360.0f, 360.0f);
                pose_.spinFr = std::remainder(t * 420.0f, 360.0f);
                pose_.spinRl = std::remainder(t * 480.0f, 360.0f);
                pose_.spinRr = pose_.spinRl;
            }
            pose_.suspFl = std::sin(phase * 1.31f) * 0.12f;
            pose_.suspFr = std::sin(phase * 1.31f + 2.1f) * 0.12f;
            pose_.suspRear = std::sin(phase * 1.31f + 4.2f) * 0.12f;
            pose_.rollDeg = std::sin(phase * 0.73f) * 4.0f;
            pose_.pitchDeg = std::sin(phase * 1.07f + 1.2f) * 3.0f;
            if (!speed.have) {
                speedValue_ = 4.0 + 1.2 * std::sin(static_cast<double>(phase));
                speedUnit_ = "m/s";
            }
            return;
        }

        const float responseDt = io.DeltaTime > 0.0f ? io.DeltaTime : dt;
        if (steer.have) {
            const float road = std::clamp(static_cast<float>(steer.value) / 12.0f, -35.0f, 35.0f);
            const float wheel = std::clamp(static_cast<float>(steer.value), -540.0f, 540.0f);
            pose_.steerDeg = lerpToward(pose_.steerDeg, road, responseDt);
            pose_.steerWheelDeg = lerpToward(pose_.steerWheelDeg, wheel, responseDt);
        }
        auto clampRpm = [](const Vital& v) {
            if (!v.have) {
                return 0.0f;
            }
            const float rpm = static_cast<float>(v.value);
            return (rpm >= 0.0f && rpm <= 1200.0f) ? rpm : 0.0f;
        };
        const float fl = clampRpm(rpmFl);
        const float fr = clampRpm(rpmFr);
        const float rear = (fl > 0.0f || fr > 0.0f) ? (fl + fr) * 0.5f : 0.0f;
        if (wheelSpin_ && dt > 0.0f) {
            pose_.spinFl = std::remainder(pose_.spinFl + fl * 6.0f * dt, 360.0f);
            pose_.spinFr = std::remainder(pose_.spinFr + fr * 6.0f * dt, 360.0f);
            pose_.spinRl = std::remainder(pose_.spinRl + rear * 6.0f * dt, 360.0f);
            pose_.spinRr = std::remainder(pose_.spinRr + rear * 6.0f * dt, 360.0f);
        }

        const Vital susp[3] = {suspFl, suspFr, suspRr};
        float visual[3]{};
        for (int i = 0; i < 3; ++i) {
            if (!susp[i].have) {
                continue;
            }
            const float raw = static_cast<float>(susp[i].value);
            if (!suspRefValid_[i]) {
                suspRef_[i] = raw;
                suspRefValid_[i] = true;
            }
            visual[i] = std::clamp((raw - suspRef_[i]) / 12.0f * 0.12f, -0.12f, 0.12f);
        }
        pose_.suspFl = lerpToward(pose_.suspFl, visual[0], responseDt);
        pose_.suspFr = lerpToward(pose_.suspFr, visual[1], responseDt);
        pose_.suspRear = lerpToward(pose_.suspRear, visual[2], responseDt);
        const Vital imuAx = map.vital("imu_ax", arena);
        const Vital imuAy = map.vital("imu_ay", arena);
        const Vital imuGx = map.vital("imu_gx", arena);
        const Vital imuGy = map.vital("imu_gy", arena);
        const float accelRoll = imuAy.have ? -static_cast<float>(imuAy.value) * 10.0f : 0.0f;
        const float accelPitch = imuAx.have ? static_cast<float>(imuAx.value) * 8.0f : 0.0f;
        const float gyroRoll = imuGx.have ? static_cast<float>(imuGx.value) * 0.01f : 0.0f;
        const float gyroPitch = imuGy.have ? static_cast<float>(imuGy.value) * 0.01f : 0.0f;
        const float roll =
            std::clamp((visual[1] - visual[0]) * 100.0f + accelRoll + gyroRoll, -4.0f, 4.0f);
        const float pitch = std::clamp((visual[2] - (visual[0] + visual[1]) * 0.5f) * 60.0f + accelPitch + gyroPitch,
                                       -3.0f, 3.0f);
        pose_.rollDeg = lerpToward(pose_.rollDeg, roll, responseDt);
        pose_.pitchDeg = lerpToward(pose_.pitchDeg, pitch, responseDt);
    }
};

Scene3dRuntime& runtime() {
    static Scene3dRuntime r;
    return r;
}

void handleOrbit() {
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        runtime().orbit(d.x, d.y);
    }
    if (ImGui::IsItemHovered()) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (std::fabs(wheel) > 0.0f) {
            runtime().zoom(wheel);
        }
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }
}

void drawHud(ImVec2 origin, ImVec2 size) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    char line[64];
    std::snprintf(line, sizeof(line), "%0.1f %s", runtime().speedValue(), runtime().speedUnit());
    const ImVec2 ts = ImGui::CalcTextSize(line);
    const ImVec2 p(origin.x + size.x - ts.x - theme::kHudInset, origin.y + theme::kHudInset);
    draw->AddRectFilled(ImVec2(p.x - 8.0f, p.y - 4.0f), ImVec2(p.x + ts.x + 8.0f, p.y + ts.y + 4.0f),
                        theme::u32(theme::withAlpha(theme::card(), 0.82f)), 6.0f);
    draw->AddText(p, theme::u32(theme::primary()), line);
    if (runtime().loading()) {
        const ImVec2 w = ImGui::CalcTextSize(runtime().loadStatus());
        const ImVec2 lp(origin.x + theme::kHudInset, origin.y + theme::kHudInset);
        draw->AddRectFilled(ImVec2(lp.x - 6.0f, lp.y - 3.0f), ImVec2(lp.x + w.x + 6.0f, lp.y + w.y + 3.0f),
                            theme::u32(theme::withAlpha(theme::card(), 0.82f)), 6.0f);
        draw->AddText(lp, theme::u32(theme::warning()), runtime().loadStatus());
    }
    if (runtime().synthetic()) {
        const ImVec2 w = ImGui::CalcTextSize("SYNTHETIC");
        draw->AddText(ImVec2(origin.x + theme::kHudInset, origin.y + size.y - w.y - theme::kHudInset),
                      theme::u32(theme::warning()),
                      "SYNTHETIC");
    }
}

void drawOverlayPanel() {
    ImGui::SetCursorPos(ImVec2(theme::kHudInset, theme::kHudInset));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::withAlpha(theme::card(), 0.88f));
    ImGui::BeginChild("##gl3d_overlay", ImVec2(300.0f, 360.0f), ImGuiChildFlags_Borders);
    ImGui::TextColored(theme::primary(), "3D");
    if (ImGui::Button("Reset camera", ImVec2(140.0f, 0.0f))) {
        runtime().resetView();
    }
    bool spin = runtime().wheelSpin();
    if (ImGui::Checkbox("Wheel spin", &spin)) {
        runtime().setWheelSpin(spin);
    }
    int car = runtime().selectedCar();
    if (ImGui::Combo("Model", &car, kCarLabels, 2)) {
        runtime().selectCar(car);
    }
    bool track = runtime().wantTrack();
    if (ImGui::Checkbox("Track", &track)) {
        runtime().setWantTrack(track);
    }
    if (runtime().wantTrack() && !runtime().trackReady() && runtime().trackError()[0] != '\0') {
        ImGui::TextColored(theme::danger(), "%s", runtime().trackError());
    }
    const auto& model = runtime().model();
    ImGui::TextColored(theme::mutedFg(), "%s  %u tris  %d draws", model.name.c_str(),
                       static_cast<unsigned>(model.triangleCount), runtime().drawCalls());
    ImGui::TextColored(theme::mutedFg(), "radius %0.2f  dist %0.2f", runtime().modelRadius(),
                       runtime().framedDistance());
    if (!model.bindNote.empty()) {
        ImGui::TextWrapped("%s", model.bindNote.c_str());
    }
    if (!model.bindNodes.empty()) {
        ImGui::TextWrapped("%s", model.bindNodes.c_str());
    }
    ImGui::TextColored(theme::mutedFg(), "%s  %0.0f fps", runtime().loadStatus(),
                       static_cast<double>(ImGui::GetIO().Framerate));
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

}  // namespace

void drawScene3DView(ClientArena& arena, ImVec2 size, bool overlay) {
    if (size.x < 8.0f || size.y < 8.0f) {
        return;
    }
    runtime().ensure();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 cursor = ImGui::GetCursorPos();
    const float fb = ImGui::GetIO().DisplayFramebufferScale.x;
    int tw = static_cast<int>(size.x * (fb > 0.0f ? fb : 1.0f));
    int th = static_cast<int>(size.y * (fb > 0.0f ? fb : 1.0f));
    if (tw < 1) {
        tw = 1;
    }
    if (th < 1) {
        th = 1;
    }
    if (tw > 1920) {
        tw = 1920;
    }
    if (th > 1080) {
        th = 1080;
    }

    const unsigned tex = runtime().render(arena, tw, th);
    ImGui::InvisibleButton("##gl3d_orbit", size);
    handleOrbit();
    if (tex != 0) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddImage(ImTextureRef(static_cast<ImTextureID>(tex)), ImGui::GetItemRectMin(),
                       ImGui::GetItemRectMax(), ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
    } else {
        ImGui::SetCursorPos(cursor);
        ImGui::TextColored(theme::danger(), "3D renderer failed to initialize");
    }
    drawHud(origin, size);
    if (overlay) {
        ImGui::SetCursorPos(cursor);
        drawOverlayPanel();
    }
}

void drawScene3DTile(ClientArena& arena, ImVec2 size) {
    runtime().ensure();
    theme::beginCard("dock3d", size, theme::card());
    ImGui::TextColored(theme::primary(), "3D");
    ImGui::SameLine();
    ImGui::TextColored(theme::mutedFg(), "%s", runtime().model().name.c_str());
    if (runtime().loading()) {
        ImGui::SameLine();
        ImGui::TextColored(theme::warning(), "loading");
    }
    if (runtime().synthetic()) {
        ImGui::SameLine();
        drawSyntheticWatermark();
    }
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    drawScene3DView(arena, avail, false);
    theme::endCard();
}

}  // namespace photon::ui
