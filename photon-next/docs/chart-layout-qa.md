# Photon Mono chart cleanup verification

Checked 2026-10-10 in `F:/lhr/photon-mono/photon-next`.

- Native clang/Ninja build: `.artifacts-layout-ui/photon-ui.exe`.
- Release Emscripten build: `.artifacts-layout-web/photon.html`, JS, WASM, and data.
- CTest: all three suites pass. `photon-protocol` reports 23,596 assertions;
  chart interaction tests report 25 checks; custom view tests validate the
  version 1 migration, version 2 round trip, and unlocked state persistence.
- Exporter: 296 assertions pass.
- `node tests/test_video_webcodecs.cjs`: pause/still behavior, stale decoder
  callbacks, and recovery from a resent IDR pass using the actual embedded glue.
- Grid tests exercise header capture, body/layout separation, minimum resize
  bounds, collision reflow and reversal, Escape, Undo, locked input, narrow
  rendering without geometry mutation, and storage writes only after release.
- Y range tests cover expansion without shrinking, separate left/right ranges,
  and rejection of non-finite bounds. Existing time-window tests cover bounded
  shifts and zooming.

The 24-panel interaction fixture renders 2,048 samples per chart through ImPlot
with normal viewport clipping. The final run averaged **0.775 ms per CPU frame**
over 300 frames on this machine. It excludes GPU presentation, video decoding,
history I/O, and the cost of the complete application. No end-to-end 60 FPS
claim is made.

Native hidden-window frame captures were inspected at 1440?900 and 600?900.
Mechanical distributions, Plots, Dynamics, Track Sim, Live View, the Connection
popover, and legacy screen redirects render correctly. Track Sim controls stack
above charts at narrow widths. Capture files are under `.artifacts-layout-ui/`.
The capture sweep exits successfully; it uses an isolated QA settings store and
an unused loopback endpoint. PDU is checked in its missing-catalog state; its
populated chart uses the same tested editor.

Desktop/browser automation was unavailable in the session. WASM compilation
and WebCodecs regression tests pass, but browser rendering and the running
camera feed were not inspected. The video recovery changes still need a live
feed check. Native WebView2 overlays are excluded from hidden-window captures.

Live status follow-up (2026-10-10): Pit tiles and Live View share one readiness
indicator. Backend status/FPS and GOP lookup completion do not imply a decoded
picture: until a texture is uploaded the state stays Buffering. An existing live
picture is retained through a pause; after two seconds without another upload
it is labelled stalled, and resumes immediately when an upload succeeds. Video
polling is limited to one upload per UI frame, including when status and image
both draw. Regression cases cover initial buffering, brief gaps, sustained stalls,
recovery, replay stills, and idle. Live camera validation is still outstanding.

UI follow-up (2026-10-10): Mechanical section tabs use a contrasting selected
fill, border, and selection line. Distribution/scatter choices use outlined tab
buttons instead of radio circles. Wide/narrow native captures were inspected.
External map overlays are hidden at the end of any frame that did not present
a map, so Pit cannot leave its map over Dashboard or a closed/clipped scene.
The Logs date-shortcut strip and Runs list are removed; calendar and timeline
remain. Native and WASM builds pass. Browser map transitions still require a
visual check on the running page.
Dynamics context follow-up (2026-10-10): Removed the six-value header grid from Dynamics charts. Race context now appears in Dynamics, with the six values in two columns below the scene/video tiles. Long/Lat G follow the same selected IMU as the charts. Native captures inspected at 1440x900 and 600x900 (narrow context collapsed); native/WASM builds pass.
Video reconnect follow-up (2026-10-10): Hello acknowledgements join the existing source instead of calling goLive(), which reselected the shared source and cleared the decoder. The native socket fixture tests an actual disconnect/reconnect and asserts video resubscription with no select_source, timeline_control, or video_stop. Run: python tests/test_ui_reconnect.py <photon-ui.exe>. It passes; WebCodecs recovery regressions pass. Continuous camera playback still needs verification after page refresh.
