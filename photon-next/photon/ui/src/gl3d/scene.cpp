#include "photon_ui/scene3d.hpp"

#include "photon/interp.hpp"
#include "photon/protocol.hpp"
#include "photon_ui/gl3d/model.hpp"
#include "photon_ui/gl3d/renderer.hpp"
#include "photon_ui/signal_map.hpp"
#include "photon_ui/theme.hpp"
#include "photon_ui/widgets.hpp"

#include "photon_ui/platform.hpp"

#include "imgui.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#ifdef __EMSCRIPTEN__
EM_JS(void, photon_maps3d_show, (int on), {
    try {
        if (window.photonMaps3d) {
            window.photonMaps3d.show(!!on);
        }
    } catch (e) {}
});
EM_JS(void, photon_maps3d_layout, (float x, float y, float w, float h), {
    try {
        if (window.photonMaps3d) {
            window.photonMaps3d.setRect(x, y, w, h);
        }
    } catch (e) {}
});
EM_JS(void, photon_maps3d_start, (double lat, double lng, const char* url), {
    try {
        if (window.photonMaps3d) {
            window.photonMaps3d.start(lat, lng, UTF8ToString(url));
        }
    } catch (e) {}
});
EM_JS(void, photon_maps3d_pose, (double lat, double lng, double heading, double scale, double range,
                                 double tilt, int follow), {
    try {
        if (window.photonMaps3d && isFinite(lat) && isFinite(lng) && isFinite(heading)) {
            window.photonMaps3d.pose(lat, lng, heading, scale, range, tilt, !!follow);
        }
    } catch (e) {}
});
EM_JS(int, photon_maps3d_ready, (), {
    try {
        return (window.photonMaps3d && window.photonMaps3d.ready()) ? 1 : 0;
    } catch (e) {
        return 0;
    }
});
EM_JS(int, photon_maps3d_hosted, (), {
    try {
        return (window.photonMaps3d && window.photonMaps3d.ready()) ? 1 : 0;
    } catch (e) {
        return 0;
    }
});
EM_JS(void, photon_maps3d_status, (char* buf, int n), {
    try {
        var s = (window.photonMaps3d && window.photonMaps3d.status()) || "";
        stringToUTF8(s, buf, n);
    } catch (e) {
        if (n > 0) {
            HEAPU8[buf] = 0;
        }
    }
});
EM_JS(void, photon_maps3d_reset_cam, (), {
    try {
        if (window.photonMaps3d && window.photonMaps3d.resetCam) {
            window.photonMaps3d.resetCam();
        }
    } catch (e) {}
});
EM_JS(void, photon_maps3d_interactive, (int on), {
    try {
        if (window.photonMaps3d && window.photonMaps3d.setInteractive) {
            window.photonMaps3d.setInteractive(!!on);
        }
    } catch (e) {}
});
#else
extern "C" {
void photon_maps3d_show(int on);
void photon_maps3d_layout(float x, float y, float w, float h);
void photon_maps3d_start(double lat, double lng, const char* url);
void photon_maps3d_pose(double lat, double lng, double heading, double scale, double range, double tilt,
                        int follow);
int photon_maps3d_ready();
int photon_maps3d_hosted();
void photon_maps3d_status(char* buf, int n);
void photon_maps3d_reset_cam();
void photon_maps3d_interactive(int on);
}
#endif
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

// OSM / MapLibre 3D (OpenFreeMap + Mapterhorn DEM). COTA until GPS arrives.
constexpr double kPresetCotaLat = 30.132792;
constexpr double kPresetCotaLon = -97.641144;
constexpr double kPresetAustinLat = 30.2672;
constexpr double kPresetAustinLon = -97.7431;

struct Maps3dUi {
    bool on = true;
    int viewMode = 0;  // 0: perspective, 1: north-up map
    bool restart = false;
    bool website = false;
    bool websiteQueryDone = false;
    bool follow = true;
    int preset = 0;
    float scale = 10.0f;
    float headingOff = 0.0f;
    float range = 55.0f;
    float tilt = 62.0f;
    double lastLat = 0.0;
    double lastLon = 0.0;
    bool haveFix = false;
    double heading = 0.0;
    interp::GpsPlayback gps{};
};

Maps3dUi gMaps{};

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
        requestCar(CarSlot::NewCar);
    }

    void tick(const ClientArena& arena) {
        ensure();
        step(arena);
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
        if (selected_ == slot && (cars_[slot].ready || cars_[slot].loading)) {
            return;
        }
        selected_ = slot;
        requestCar(static_cast<CarSlot>(slot));
        if (cars_[selected_].ready) {
            useCar(selected_);
        }
        // Keep the last ready car on screen while the new GLB fetches/parses.
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
        const float dist = cam_.distance;
        const float farPlane = std::max(useTrack ? 260.0f : 16.0f, cam_.distance + r * 2.0f + 16.0f);
        const float aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
        const gl3d::Mat4 proj = gl3d::perspective(gl3d::radians(kFovYDeg), aspect, 0.15f, farPlane);
        const float yaw = gl3d::radians(cam_.yaw);
        const float pitch = gl3d::radians(cam_.pitch);
        const gl3d::Vec3 target{cam_.targetX, cam_.targetY, cam_.targetZ};
        const gl3d::Vec3 eye{target.x + dist * std::cos(pitch) * std::sin(yaw),
                             target.y + dist * std::sin(pitch),
                             target.z + dist * std::cos(pitch) * std::cos(yaw)};
        const gl3d::Mat4 view = gl3d::lookAt(eye, target, {0.0f, 1.0f, 0.0f});
        const gl3d::Mat4 groundM = gl3d::identity();
        const gl3d::MeshGpu* extra = useTrack ? nullptr : &ground_;
        const gl3d::ModelGpu* trackGpu = useTrack ? &track_.model.gpu : nullptr;
        const gl3d::Mat4* trackMats = useTrack && !trackMats_.empty() ? trackMats_.data() : nullptr;
        const gl3d::Vec3 toCam = gl3d::normalize(eye - target);
        gl3d::Lights lights{};
        lights.camPos = eye;
        lights.fillDir = gl3d::normalize({toCam.x, toCam.y + 0.35f, toCam.z});
        const ImVec4 clear = theme::background();
        lights.clearColor = gl3d::Vec3{clear.x, clear.y, clear.z};
        lights.clearAlpha = 1.0f;
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

    void pan(float dx, float dy) {
        const float yaw = gl3d::radians(cam_.yaw);
        const float pitch = gl3d::radians(cam_.pitch);
        const float rx = std::cos(yaw);
        const float rz = -std::sin(yaw);
        const float ux = -std::sin(pitch) * std::sin(yaw);
        const float uy = std::cos(pitch);
        const float uz = -std::sin(pitch) * std::cos(yaw);
        const float s = std::max(cam_.distance, 1.0f) * 0.0022f;
        cam_.targetX += (-dx * rx - dy * ux) * s;
        cam_.targetY += (-dy * uy) * s;
        cam_.targetZ += (-dx * rz - dy * uz) * s;
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
    int selected_ = static_cast<int>(CarSlot::NewCar);
    int displayed_ = -1;
    bool ready_ = false;
    bool wheelSpin_ = true;
    static constexpr float kExposure = 2.0f;
    bool synthetic_ = true;
    bool wantTrack_ = false;
    bool suspRefValid_[3]{};
    float suspRef_[3]{};
    double speedValue_ = 0.0;
    const char* speedUnit_ = "mph";
    int lastDraws_ = 0;
    float modelRadius_ = 0.0f;
    float framedDistance_ = 0.0f;
    char loadStatus_[128]{"loading newCar.glb"};

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
#ifdef __EMSCRIPTEN__
        auto* job = new FetchJob{this, kind};
        emscripten_async_wget2_data(url, "GET", "", job, 1, &Scene3dRuntime::onWget2Ok,
                                   &Scene3dRuntime::onWget2Err, &Scene3dRuntime::onWget2Progress);
#else
        const std::string path = photon::ui::assetPath(url);
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            fetchFailed(kind);
            return;
        }
        std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (bytes.empty()) {
            fetchFailed(kind);
            return;
        }
        finishFetch(kind, bytes.data(), static_cast<int>(bytes.size()));
#endif
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

        speedValue_ = speed.have ? speed.value * kMpsToMph : 0.0;
        speedUnit_ = "mph";

        const bool mappedLive = (steer.mapped && steer.have) || (rpmFl.mapped && rpmFl.have) ||
                                (rpmFr.mapped && rpmFr.have) || (suspFl.mapped && suspFl.have) ||
                                (suspFr.mapped && suspFr.have) || (suspRr.mapped && suspRr.have) ||
                                (speed.mapped && speed.have);
        const bool liveSource = arena.source == ctrl::SourceKind::LogReplay ||
                                arena.source == ctrl::SourceKind::DaqTcp ||
                                arena.source == ctrl::SourceKind::Pcan;
        // A catalog pin is the log. Do not keep the sine-wave car running
        // beside history tiles.
        synthetic_ = !arena.catalogPinned() && !mappedLive && !liveSource;

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
                speedValue_ = (4.0 + 1.2 * std::sin(static_cast<double>(phase))) * kMpsToMph;
                speedUnit_ = "mph";
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
    const ImGuiIO& io = ImGui::GetIO();
    static bool camDrag = false;
    if (ImGui::IsItemHovered() && (io.MouseDown[0] || io.MouseDown[1] || io.MouseDown[2])) {
        camDrag = true;
    }
    if (!io.MouseDown[0] && !io.MouseDown[1] && !io.MouseDown[2]) {
        camDrag = false;
    }
    if (camDrag) {
        const ImVec2 d = io.MouseDelta;
        const bool pan = io.MouseDown[1] || io.MouseDown[2] || (io.MouseDown[0] && io.KeyShift);
        if (pan) {
            runtime().pan(d.x, d.y);
        } else if (io.MouseDown[0]) {
            runtime().orbit(d.x, d.y);
        }
    }
    if (ImGui::IsItemHovered()) {
        const float wheel = io.MouseWheel;
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
    char gps[80];
    if (gMaps.haveFix) {
        std::snprintf(gps, sizeof(gps), "GPS  %0.6f  %0.6f", gMaps.lastLat, gMaps.lastLon);
    } else {
        std::snprintf(gps, sizeof(gps), "No GPS fix");
    }
    const ImVec2 gs = ImGui::CalcTextSize(gps);
    const ImVec2 gp(origin.x + theme::kHudInset, origin.y + size.y - gs.y - theme::kHudInset);
    draw->AddRectFilled(ImVec2(gp.x - 6.0f, gp.y - 3.0f), ImVec2(gp.x + gs.x + 6.0f, gp.y + gs.y + 3.0f),
                        theme::u32(theme::withAlpha(theme::card(), 0.82f)), 6.0f);
    draw->AddText(gp, theme::u32(gMaps.haveFix ? theme::success() : theme::warning()), gps);
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
    ImGui::BeginChild("##gl3d_overlay", ImVec2(288.0f, 400.0f), ImGuiChildFlags_Borders);
    ImGui::TextColored(theme::primary(), "%s", gMaps.viewMode == 1 ? "Map" : "3D");
    if (ImGui::Button("Reset camera", ImVec2(140.0f, 0.0f))) {
        runtime().resetView();
        gMaps.follow = true;
        photon_maps3d_reset_cam();
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
    ImGui::Separator();
    if (ImGui::Checkbox("Map", &gMaps.on)) {
        if (!gMaps.on) {
            photon_maps3d_show(0);
        }
        gMaps.restart = true;
    }
    theme::pushSmallFont();
    ImGui::TextWrapped(
        "OpenStreetMap tiles and DEM are cached in this browser. "
        "No API key. Tab switches keep the world warm.");
    theme::popSmallFont();
    ImGui::Combo("Map view", &gMaps.viewMode, "3D perspective\0 2D north-up\0");
    ImGui::Combo("No-GPS fallback", &gMaps.preset, "COTA\0Austin downtown\0");
    if (gMaps.haveFix) {
        ImGui::TextColored(theme::success(), "GPS  %.5f  %.5f", gMaps.lastLat, gMaps.lastLon);
    } else {
        ImGui::TextColored(theme::warning(), "No GPS fix: using fallback");
    }
    ImGui::Checkbox("Follow car", &gMaps.follow);
    theme::pushSmallFont();
    ImGui::TextWrapped("Left-drag pans around the car. Zoom and orientation follow the view mode.");
    theme::popSmallFont();
    ImGui::SliderFloat("Model scale", &gMaps.scale, 0.2f, 20.0f, "%.2f");
    ImGui::SliderFloat("Heading offset", &gMaps.headingOff, -180.0f, 180.0f, "%.0f deg");
    ImGui::SliderFloat("Camera range", &gMaps.range, 20.0f, 400.0f, "%.0f m");
    if (gMaps.viewMode == 0) {
        ImGui::SliderFloat("Camera tilt", &gMaps.tilt, 20.0f, 85.0f, "%.0f deg");
    }
    char mapsStatus[160]{};
    photon_maps3d_status(mapsStatus, static_cast<int>(sizeof(mapsStatus)));
    ImGui::TextColored(theme::mutedFg(), "%s", mapsStatus[0] != '\0' ? mapsStatus : "off");
    if (gMaps.haveFix) {
        ImGui::TextColored(theme::success(), "Car GPS  %0.6f  %0.6f", gMaps.lastLat, gMaps.lastLon);
    } else {
        ImGui::TextColored(theme::warning(), "Waiting for car GPS…");
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

#ifndef __EMSCRIPTEN__
bool scene3dGpuEnabled() {
    const char* off = std::getenv("PHOTON_GL3D");
    // Default on. PHOTON_GL3D=0 disables GPU if it misbehaves.
    if (off != nullptr && off[0] == '0') {
        return false;
    }
    return true;
}
#else
bool scene3dGpuEnabled() { return true; }
#endif

void drawScene3DView(ClientArena& arena, ImVec2 size, bool overlay, bool hostMaps) {
    if (size.x < 8.0f || size.y < 8.0f) {
        return;
    }
#ifndef __EMSCRIPTEN__
    if (!scene3dGpuEnabled()) {
        ImGui::BeginChild("gl3d-desktop-stub", size, ImGuiChildFlags_None);
        ImGui::TextColored(theme::mutedFg(), "3D GPU view is off in the desktop build.");
        ImGui::TextColored(theme::mutedFg(), "Use Plots / Analysis to debug catalog. PHOTON_GL3D=1 forces it.");
        ImGui::EndChild();
        return;
    }
    // Parsing newCar.glb on the first frame blocked the WS hello for ~40s.
    if (!arena.haveActiveSource) {
        ImGui::BeginChild("gl3d-wait", size, ImGuiChildFlags_None);
        ImGui::TextColored(theme::mutedFg(), "Connecting…");
        ImGui::EndChild();
        return;
    }
    static int sWarmFrames = 0;
    if (sWarmFrames < 8) {
        ++sWarmFrames;
        ImGui::BeginChild("gl3d-warm", size, ImGuiChildFlags_None);
        ImGui::TextColored(theme::mutedFg(), "Loading 3D model…");
        ImGui::EndChild();
        return;
    }
#endif
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
    // Keep the pane's aspect. A hard 1920x1080 cap stretched the car on
    // anything wider or taller than that framebuffer.
    const int longEdge = std::max(tw, th);
    constexpr int kMaxEdge = 2560;
    if (longEdge > kMaxEdge) {
        const float shrink = static_cast<float>(kMaxEdge) / static_cast<float>(longEdge);
        tw = std::max(1, static_cast<int>(static_cast<float>(tw) * shrink));
        th = std::max(1, static_cast<int>(static_cast<float>(th) * shrink));
    }

    auto mapsAnchor = [&](double& useLat, double& useLon) {
        const SignalMap& map = arena.signalMap();
        const ResolvedScalar* latSlot = map.scalar("gps_lat");
        const ResolvedScalar* lonSlot = map.scalar("gps_lon");
        const SignalRing* latRing = (latSlot != nullptr && latSlot->mapped) ? arena.ring(latSlot->id) : nullptr;
        const SignalRing* lonRing = (lonSlot != nullptr && lonSlot->mapped) ? arena.ring(lonSlot->id) : nullptr;
        useLat = gMaps.preset == 1 ? kPresetAustinLat : kPresetCotaLat;
        useLon = gMaps.preset == 1 ? kPresetAustinLon : kPresetCotaLon;
        if (arena.consumeMapsReset()) {
            gMaps.gps.reset();
            gMaps.haveFix = false;
        }
        auto takeFix = [&](double lat, double lon) {
            // History sometimes ships DBC raw (microdegrees) instead of physical.
            if (std::fabs(lat) > 90.0 && std::fabs(lat) < 9.0e7) {
                lat *= 1.0e-6;
            }
            if (std::fabs(lon) > 180.0 && std::fabs(lon) < 1.8e8) {
                lon *= 1.0e-6;
            }
            if (!std::isfinite(lat) || !std::isfinite(lon) || (lat == 0.0 && lon == 0.0) ||
                std::fabs(lat) > 90.0 || std::fabs(lon) > 180.0) {
                return false;
            }
            const ResolvedScalar* validSlot = map.scalar("gps_valid");
            if (validSlot != nullptr && validSlot->mapped) {
                double valid = 1.0;
                if (!arena.cursorValue(validSlot->id, valid)) {
                    valid = arena.readout(validSlot->id, 1.0);
                }
                // Valid=0 still carries the last WGS84 sample. Rejecting it
                // left OSM on COTA even when the catalog pin had coordinates.
                if (valid <= 0.5) {
                    std::fprintf(stderr, "[maps] gps_valid=%.2f: using lat/lon anyway\n", valid);
                    std::fflush(stderr);
                }
            }
            if (!gMaps.haveFix) {
                gMaps.follow = true;
            }
            gMaps.lastLat = lat;
            gMaps.lastLon = lon;
            gMaps.haveFix = true;
            useLat = lat;
            useLon = lon;
            return true;
        };
        if (arena.catalogPinned() || latRing == nullptr || lonRing == nullptr || latRing->count == 0 ||
            lonRing->count == 0) {
            double tileLat = 0.0;
            double tileLon = 0.0;
            const std::uint64_t cursor = arena.effectiveCursorMs();
            auto readGps = [&]() {
                if (latSlot == nullptr || !latSlot->mapped || lonSlot == nullptr || !lonSlot->mapped) {
                    return false;
                }
                if (arena.cursorValue(latSlot->id, tileLat) && arena.cursorValue(lonSlot->id, tileLon) &&
                    !(tileLat == 0.0 && tileLon == 0.0)) {
                    return true;
                }
                const auto span = arena.tiles().sessionMaxMs() > arena.tiles().sessionMinMs()
                                      ? arena.tiles().sessionMaxMs() - arena.tiles().sessionMinMs()
                                      : 600000ull;
                if (arena.tiles().sampleNear(latSlot->id, cursor, tileLat, span) &&
                    arena.tiles().sampleNear(lonSlot->id, cursor, tileLon, span) &&
                    !(tileLat == 0.0 && tileLon == 0.0)) {
                    return true;
                }
                return arena.tiles().sampleFirstAbs(latSlot->id, tileLat, 0.01) &&
                       arena.tiles().sampleFirstAbs(lonSlot->id, tileLon, 0.01);
            };
            if (readGps() && takeFix(tileLat, tileLon)) {
                static double sLoggedLat = 1e9;
                if (std::fabs(tileLat - sLoggedLat) > 1e-5) {
                    sLoggedLat = tileLat;
                    std::fprintf(stderr, "[maps] catalog GPS %.6f %.6f cursor=%llu ids=%u,%u\n", tileLat,
                                 tileLon, static_cast<unsigned long long>(cursor), latSlot->id, lonSlot->id);
                    std::fflush(stderr);
                }
                return;
            }
            static int sMiss = 0;
            if (sMiss < 12) {
                ++sMiss;
                std::fprintf(stderr,
                             "[maps] catalog GPS miss mapped=%d/%d cursor=%llu lat=%.6f lon=%.6f read=%d\n",
                             latSlot != nullptr && latSlot->mapped ? 1 : 0,
                             lonSlot != nullptr && lonSlot->mapped ? 1 : 0,
                             static_cast<unsigned long long>(cursor), tileLat, tileLon, readGps() ? 1 : 0);
                std::fflush(stderr);
            }
            if (gMaps.haveFix) {
                useLat = gMaps.lastLat;
                useLon = gMaps.lastLon;
            }
            return;
        }
        const double oldest = std::min(latRing->firstTs(), lonRing->firstTs());
        const double newest = std::max(latRing->lastTs(), lonRing->lastTs());
        const auto& tl = arena.timeline();
        const bool playing = tl.mode == static_cast<std::uint8_t>(proto::TimelineMode::Play) ||
                             tl.mode == static_cast<std::uint8_t>(proto::TimelineMode::Live);
        const std::uint64_t cursorMs = arena.displayCursorMs();
        const bool haveCursor = tl.maxTimeMs > tl.minTimeMs && cursorMs != 0;
        if (gMaps.haveFix && (std::fabs(latRing->latest - gMaps.lastLat) > 0.08 ||
                              std::fabs(lonRing->latest - gMaps.lastLon) > 0.08)) {
            gMaps.gps.reset();
            gMaps.haveFix = false;
        }
        const float dtMs = playing ? ImGui::GetIO().DeltaTime * 1000.0f : 0.0f;
        gMaps.gps.advance(newest, oldest, static_cast<double>(dtMs), playing, static_cast<double>(cursorMs),
                          haveCursor);
        const interp::GpsFix fix =
            gMaps.gps.sample(latRing->t.data(), latRing->v.data(), latRing->count, latRing->cap, latRing->write,
                             lonRing->t.data(), lonRing->v.data(), lonRing->count, lonRing->cap, lonRing->write);
        if (!fix.ok) {
            if (gMaps.haveFix) {
                useLat = gMaps.lastLat;
                useLon = gMaps.lastLon;
            }
            return;
        }
        double valid = 1.0;
        const ResolvedScalar* validSlot = map.scalar("gps_valid");
        const bool validMapped = validSlot != nullptr && validSlot->mapped;
        if (validMapped && !arena.interpolate(validSlot->id, fix.playMs, valid)) {
            valid = arena.readout(validSlot->id, 1.0);
        }
        if (validMapped && valid <= 0.5) {
            if (gMaps.haveFix) {
                useLat = gMaps.lastLat;
                useLon = gMaps.lastLon;
            }
            return;
        }
        if (!gMaps.haveFix) {
            gMaps.follow = true;
        }
        gMaps.lastLat = fix.lat;
        gMaps.lastLon = fix.lon;
        gMaps.heading = fix.headingDeg;
        gMaps.haveFix = true;
        useLat = fix.lat;
        useLon = fix.lon;
    };

    double useLat = kPresetAustinLat;
    double useLon = kPresetAustinLon;
    mapsAnchor(useLat, useLon);
    const bool wantMaps = gMaps.on && (overlay || hostMaps);
    static float sLayout[4]{-1.0f, -1.0f, -1.0f, -1.0f};
    static int sCar = -1;
    static bool sStarted = false;
    static double sStartLat = 1e9;
    static double sStartLon = 1e9;
    if (gMaps.restart) {
        sStarted = false;
        sCar = -1;
        gMaps.restart = false;
    }
    if (wantMaps) {
        // ImGui positions are already logical pixels. The native host
        // multiplies by DPI, and the page overlay treats these as CSS pixels.
        // Dividing by the framebuffer scale first placed the 3D view too small
        // and up-left whenever the display was not 100%.
        const float panel = overlay ? std::min(308.0f, std::max(0.0f, size.x * 0.34f)) : 0.0f;
        const float lx = origin.x + panel;
        const float ly = origin.y;
        const float lw = std::max(8.0f, size.x - panel);
        const float lh = size.y;
        if (std::fabs(lx - sLayout[0]) > 0.5f || std::fabs(ly - sLayout[1]) > 0.5f ||
            std::fabs(lw - sLayout[2]) > 0.5f || std::fabs(lh - sLayout[3]) > 0.5f) {
            sLayout[0] = lx;
            sLayout[1] = ly;
            sLayout[2] = lw;
            sLayout[3] = lh;
        }
        photon_maps3d_layout(lx, ly, lw, lh);
        // OSM overlay always uses the display body. new_dyn is chassis-only;
        // native GLES keeps the Model combo for dynamics when OSM is off.
        const int car = static_cast<int>(CarSlot::NewCar);
        const bool jumped = sStarted && (std::fabs(useLat - sStartLat) > 0.002 ||
                                         std::fabs(useLon - sStartLon) > 0.002);
        if (sCar != car || !sStarted || jumped) {
            sCar = car;
            sStarted = true;
            sStartLat = useLat;
            sStartLon = useLon;
            std::fprintf(stderr, "[maps] start %.6f %.6f haveFix=%d\n", useLat, useLon, gMaps.haveFix ? 1 : 0);
            std::fflush(stderr);
            photon_maps3d_start(useLat, useLon, kCarUrls[car]);
        }
    } else {
        sCar = -1;
        sStarted = false;
        sStartLat = 1e9;
        sStartLon = 1e9;
    }
    const bool mapsLive = wantMaps && photon_maps3d_ready() != 0;
    const bool mapsHosted = wantMaps && photon_maps3d_hosted() != 0;
    if (!overlay) {
        gMaps.follow = true;
    }
    if (mapsHosted || mapsLive) {
        runtime().tick(arena);
        ImGui::Dummy(size);
        if (mapsOverlayBlockedByPopup()) {
            photon_maps3d_show(0);
        } else {
            photon_maps3d_pose(useLat, useLon,
                               gMaps.viewMode == 1 ? 0.0 : gMaps.heading + static_cast<double>(gMaps.headingOff),
                               static_cast<double>(gMaps.scale), static_cast<double>(gMaps.range),
                               gMaps.viewMode == 1 ? 0.0 : static_cast<double>(gMaps.tilt), gMaps.follow ? 1 : 0);
            photon_maps3d_show(1);
        }
    } else {
        if (!wantMaps) {
            photon_maps3d_show(0);
        }
        ImGui::InvisibleButton("##gl3d_orbit", size);
        handleOrbit();
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const unsigned tex = runtime().render(arena, tw, th);
        if (tex != 0) {
            draw->AddImage(ImTextureRef(static_cast<ImTextureID>(static_cast<std::uintptr_t>(tex))), min,
                           max, ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
        } else {
            ImGui::SetCursorPos(cursor);
            ImGui::TextColored(theme::danger(), "3D renderer failed to initialize");
        }
        if (wantMaps) {
            char mapsStatus[160]{};
            photon_maps3d_status(mapsStatus, static_cast<int>(sizeof(mapsStatus)));
            const char* line = mapsStatus[0] != '\0' ? mapsStatus : "Loading OSM 3D…";
            const ImVec2 ts = ImGui::CalcTextSize(line);
            const ImVec2 tp((min.x + max.x - ts.x) * 0.5f, min.y + 18.0f);
            draw->AddRectFilled(ImVec2(tp.x - 10.0f, tp.y - 6.0f), ImVec2(tp.x + ts.x + 10.0f, tp.y + ts.y + 6.0f),
                                theme::u32(theme::withAlpha(theme::card(), 0.9f)), 6.0f);
            draw->AddText(tp, theme::u32(theme::warning()), line);
        }
    }
    drawHud(origin, size);
    if (overlay) {
        ImGui::SetCursorPos(cursor);
        drawOverlayPanel();
    }
}

namespace {
bool gMapsHoldOff = false;
}

void hideMaps3d() { photon_maps3d_show(0); }

void setMapsOverlayHoldOff(bool on) {
    gMapsHoldOff = on;
    if (on) {
        photon_maps3d_show(0);
    }
}

bool mapsOverlayHoldOff() { return gMapsHoldOff; }

void setMaps3dInteractive(bool on) { photon_maps3d_interactive(on ? 1 : 0); }

void drawMaps3dKeyRow() {
    char mapsStatus[160]{};
    photon_maps3d_status(mapsStatus, static_cast<int>(sizeof(mapsStatus)));
    ImGui::TextColored(theme::mutedFg(), "%s", mapsStatus[0] != '\0' ? mapsStatus : "Map");
}

void drawScene3DTile(ClientArena& arena, ImVec2 size) {
    theme::beginCard("dock3d", size, theme::card());
    ImGui::TextColored(theme::primary(), "%s", gMaps.viewMode == 1 ? "Map" : "3D");
    ImGui::SameLine();
    if (gMaps.haveFix) {
        ImGui::TextColored(theme::success(), "GPS  %0.5f  %0.5f", gMaps.lastLat, gMaps.lastLon);
    } else {
        ImGui::TextColored(theme::warning(), "No GPS fix");
    }
    ImGui::SameLine();
#ifndef __EMSCRIPTEN__
    if (!scene3dGpuEnabled()) {
        ImGui::TextColored(theme::mutedFg(), "GPU off");
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        drawScene3DView(arena, avail, false, true);
        theme::endCard();
        return;
    }
#endif
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
    drawScene3DView(arena, avail, false, true);
    theme::endCard();
}

}  // namespace photon::ui
