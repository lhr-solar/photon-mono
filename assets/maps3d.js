      window.photonMaps3d = (function() {
        var host = document.getElementById('photon-maps3d');
        var map = null;
        var carLayer = null;
        var started = false;
        var interactive = true;
        var starting = false;
        var visible = false;
        var lastSrc = '';
        var pendingStart = null;
        var msg = 'off';
        var lastElev = 0;
        var haveElev = false;
        var last = { lat: 30.132792, lng: -97.641144, heading: 0, scale: 10, range: 90, tilt: 62, follow: true };
        var lastRect = { x: 1e9, y: 1e9, w: 0, h: 0 };
        var lastGood = { x: 0, y: 0, w: 0, h: 0 };
        var lastCam = { lat: 1e9, lng: 1e9, heading: 1e9, range: -1, tilt: -1, follow: -1 };
        var panOff = { dlat: 0, dlng: 0 };
        var camPan = false;
        var motion = {
          inited: false,
          fromLat: 0, fromLng: 0, fromHdg: 0,
          toLat: 0, toLng: 0, toHdg: 0,
          t0: 0, dur: 0, lastAt: 0
        };
        var threeMods = null;
        var styleJson = null;
        var TILE_RE = /openfreemap\.org|mapterhorn\.com|openmaptiles|arcgisonline\.com/;
        var MAX_ZOOM = 18.25;

        function installTileCache() {
          if (window.__photonOsmFetch) {
            return;
          }
          var orig = window.fetch.bind(window);
          var mem = new Map();
          window.__photonOsmFetch = true;
          window.fetch = function(input, init) {
            var url = typeof input === 'string' ? input : (input && input.url) || '';
            var method = (init && init.method) || (input && input.method) || 'GET';
            if (method !== 'GET' || !TILE_RE.test(url)) {
              return orig(input, init);
            }
            if (mem.has(url)) {
              return Promise.resolve(mem.get(url).clone());
            }
            // Cache API exists only on HTTPS / localhost. The public Oracle page is HTTP.
            var cacheApi = (window.isSecureContext && typeof caches !== 'undefined') ? caches : null;
            if (!cacheApi) {
              return orig(input, init).then(function(res) {
                if (res.ok) {
                  if (mem.size > 160) {
                    mem.clear();
                  }
                  mem.set(url, res.clone());
                }
                return res;
              });
            }
            return cacheApi.open('photon-osm-v1').then(function(cache) {
              return cache.match(url).then(function(hit) {
                if (hit) {
                  mem.set(url, hit.clone());
                  return hit;
                }
                return orig(url).then(function(res) {
                  if (res.ok) {
                    if (mem.size > 160) {
                      mem.clear();
                    }
                    Promise.resolve(cache.put(url, res.clone())).catch(function() {});
                    mem.set(url, res.clone());
                  }
                  return res;
                });
              });
            }).catch(function() {
              return orig(input, init);
            });
          };
        }
        installTileCache();

        function status() { return msg; }
        function ready() { return started && !!map; }
        function absUrl(u) {
          try { return new URL(u, window.location.href).href; } catch (e) { return u; }
        }
        function setRect(x, y, w, h) {
          if (!host) return;
          if (Math.abs(x - lastRect.x) < 0.5 && Math.abs(y - lastRect.y) < 0.5 &&
              Math.abs(w - lastRect.w) < 0.5 && Math.abs(h - lastRect.h) < 0.5) {
            return;
          }
          lastRect = { x: x, y: y, w: w, h: h };
          lastGood = { x: x, y: y, w: w, h: h };
          host.style.left = x + 'px';
          host.style.top = y + 'px';
          host.style.width = Math.max(0, w) + 'px';
          host.style.height = Math.max(0, h) + 'px';
          if (map && visible) {
            map.resize();
          }
        }

        function show(on) {
          if (!host) return;
          host.setAttribute('aria-hidden', on ? 'false' : 'true');
          if (on) {
            visible = true;
            host.style.display = 'block';
            host.style.visibility = 'visible';
            host.style.pointerEvents = interactive ? 'auto' : 'none';
            if (lastGood.w > 0) {
              host.style.left = lastGood.x + 'px';
              host.style.top = lastGood.y + 'px';
              host.style.width = Math.max(0, lastGood.w) + 'px';
              host.style.height = Math.max(0, lastGood.h) + 'px';
            }
            if (map) {
              map.resize();
              applyCamera(true);
            }
            return;
          }
          visible = false;
          if (started) {
            host.style.display = 'block';
            host.style.visibility = 'hidden';
            host.style.pointerEvents = 'none';
            host.style.left = '-12000px';
            host.style.top = '0px';
            lastRect.x = 1e9;
            return;
          }
          host.style.display = 'none';
        }

        function rangeToZoom(range, lat) {
          var earth = 40075016.686 * Math.cos((lat || 0) * Math.PI / 180);
          var span = Math.max(8, range) * 0.85;
          var z = Math.log2(earth / span);
          return Math.max(15.2, Math.min(MAX_ZOOM, z));
        }

        // Coarse GLO-30 can sit a meter above the asphalt; lift so a low body isn't occluded.
        var ELEV_LIFT_M = 1.5;

        function sampleElev(lngLat) {
          if (!map || typeof map.queryTerrainElevation !== 'function') {
            return haveElev ? lastElev : null;
          }
          var e = map.queryTerrainElevation(lngLat, { exaggerated: true });
          if (e == null || !isFinite(e)) {
            return haveElev ? lastElev : null;
          }
          lastElev = e;
          haveElev = true;
          return e;
        }

        function following() {
          return !!last.follow;
        }

        var SAT_TILE = 'https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/';
        var satImgCache = {};

        function webMercTile(lng, lat, z) {
          var n = 1 << z;
          var x = Math.floor((lng + 180) / 360 * n);
          var s = Math.sin(lat * Math.PI / 180);
          var y = Math.floor((0.5 - Math.log((1 + s) / (1 - s)) / (4 * Math.PI)) * n);
          x = ((x % n) + n) % n;
          y = Math.min(n - 1, Math.max(0, y));
          return { x: x, y: y, n: n };
        }

        function loadSatTile(z, x, y) {
          var k = z + '/' + y + '/' + x;
          if (satImgCache[k]) {
            return Promise.resolve(satImgCache[k]);
          }
          return fetch(SAT_TILE + z + '/' + y + '/' + x).then(function(r) {
            if (!r.ok) {
              throw new Error('sat tile');
            }
            return r.blob();
          }).then(function(b) {
            return createImageBitmap(b);
          }).then(function(img) {
            satImgCache[k] = img;
            var keys = Object.keys(satImgCache);
            if (keys.length > 96) {
              delete satImgCache[keys[0]];
            }
            return img;
          }).catch(function() {
            return null;
          });
        }

        function groundElevAt(lng, lat) {
          var e = sampleElev(new maplibregl.LngLat(lng, lat));
          if (e == null || !isFinite(e)) {
            return haveElev ? lastElev : null;
          }
          return e;
        }

        function showBeigeBuildings(on) {
          try {
            if (map && map.getLayer('building-3d')) {
              map.setLayoutProperty('building-3d', 'visibility', on ? 'visible' : 'none');
            }
          } catch (e) {}
        }

        function shortestHeadingDelta(from, to) {
          var d = (to - from) % 360;
          if (d > 180) {
            d -= 360;
          }
          if (d < -180) {
            d += 360;
          }
          return d;
        }

        function wrapHeading(h) {
          h = h % 360;
          if (h < 0) {
            h += 360;
          }
          return h;
        }

        function approxMeters(lat1, lng1, lat2, lng2) {
          var my = (lat1 + lat2) * 0.008726646259971648;
          var dy = (lat2 - lat1) * 111320;
          var dx = (lng2 - lng1) * 111320 * Math.cos(my);
          return Math.sqrt(dx * dx + dy * dy);
        }

        function snapMotion(lat, lng, heading) {
          motion.inited = true;
          motion.fromLat = motion.toLat = last.lat = lat;
          motion.fromLng = motion.toLng = last.lng = lng;
          motion.fromHdg = motion.toHdg = last.heading = heading;
          motion.t0 = performance.now();
          motion.dur = 0;
        }

        // Linear interpolate lat/lng/heading from the last GPS sample to the new one
        // over the actual sample interval (clamped). Teleports snap.
        function pushTarget(lat, lng, heading) {
          if (!isFinite(lat) || !isFinite(lng) || !isFinite(heading)) {
            return;
          }
          if (!motion.inited) {
            snapMotion(lat, lng, heading);
            motion.lastAt = performance.now();
            return;
          }
          var dlat = lat - motion.toLat;
          var dlng = lng - motion.toLng;
          var dh = shortestHeadingDelta(motion.toHdg, heading);
          if (dlat * dlat + dlng * dlng < 1.0e-16 && Math.abs(dh) < 0.05) {
            return;
          }
          var now = performance.now();
          if (approxMeters(last.lat, last.lng, lat, lng) > 80) {
            snapMotion(lat, lng, heading);
            motion.lastAt = now;
            return;
          }
          motion.fromLat = last.lat;
          motion.fromLng = last.lng;
          motion.fromHdg = last.heading;
          motion.toLat = lat;
          motion.toLng = lng;
          motion.toHdg = heading;
          motion.t0 = now;
          var gap = now - motion.lastAt;
          motion.dur = Math.max(80, Math.min(1500, gap > 1 ? gap : 250));
          motion.lastAt = now;
        }

        function stepLerp() {
          if (!motion.inited) {
            return 1;
          }
          if (motion.dur <= 0) {
            last.lat = motion.toLat;
            last.lng = motion.toLng;
            last.heading = motion.toHdg;
            return 1;
          }
          var u = (performance.now() - motion.t0) / motion.dur;
          if (u >= 1) {
            last.lat = motion.toLat;
            last.lng = motion.toLng;
            last.heading = wrapHeading(motion.toHdg);
            return 1;
          }
          last.lat = motion.fromLat + (motion.toLat - motion.fromLat) * u;
          last.lng = motion.fromLng + (motion.toLng - motion.fromLng) * u;
          last.heading = wrapHeading(
              motion.fromHdg + shortestHeadingDelta(motion.fromHdg, motion.toHdg) * u);
          return u;
        }

        function bindUserCamera() {
          if (!map) {
            return;
          }
          try { map.dragRotate.disable(); } catch (e) {}
          try { map.touchZoomRotate.disable(); } catch (e) {}
          try { map.touchPitch.disable(); } catch (e) {}
          try { map.scrollZoom.disable(); } catch (e) {}
          try { map.boxZoom.disable(); } catch (e) {}
          try { map.doubleClickZoom.disable(); } catch (e) {}
          try { map.keyboard.disable(); } catch (e) {}
          try { map.dragPan.enable(); } catch (e) {}
          function endPan() {
            if (!camPan || !map) {
              camPan = false;
              return;
            }
            var c = map.getCenter();
            panOff.dlat = c.lat - last.lat;
            panOff.dlng = c.lng - last.lng;
            camPan = false;
          }
          map.on('mousedown', function(ev) {
            if (ev && ev.originalEvent && ev.originalEvent.button === 0) {
              camPan = true;
            }
          });
          map.on('move', function() {
            if (!camPan || !map) {
              return;
            }
            var c = map.getCenter();
            panOff.dlat = c.lat - last.lat;
            panOff.dlng = c.lng - last.lng;
          });
          map.on('mouseup', endPan);
          map.on('dragend', endPan);
        }

        function resetCam() {
          panOff.dlat = 0;
          panOff.dlng = 0;
          camPan = false;
          applyCamera(true);
        }

        function bindLod() {
          // Keep one DEM exaggeration. Swapping it on zoom rebuilds the
          // terrain mesh and makes the satellite drape flicker.
        }

        function applyCamera(force) {
          if (!map || camPan) {
            return;
          }
          if (!visible && !force) {
            return;
          }
          if (!last.follow && !force) {
            return;
          }
          var z = rangeToZoom(last.range, last.lat);
          var pitch = Math.max(0, Math.min(85, last.tilt));
          var lat = last.lat + panOff.dlat;
          var lng = last.lng + panOff.dlng;
          var bearing = force ? last.heading : map.getBearing();
          if (!force && Math.abs(lat - lastCam.lat) < 8.0e-7 && Math.abs(lng - lastCam.lng) < 8.0e-7 &&
              Math.abs(last.range - lastCam.range) < 1.0 && Math.abs(last.tilt - lastCam.tilt) < 0.8) {
            return;
          }
          lastCam.lat = lat;
          lastCam.lng = lng;
          lastCam.heading = bearing;
          lastCam.range = last.range;
          lastCam.tilt = last.tilt;
          map.jumpTo({
            center: [lng, lat],
            zoom: z,
            pitch: pitch,
            bearing: bearing
          });
        }

        function applyPose(lat, lng, heading, scale, range, tilt, follow) {
          last.scale = scale;
          last.range = range;
          last.tilt = tilt;
          last.follow = follow;
          // C++ already interpolates GPS from the client ring. Snap here so a
          // second wall-clock lerp does not add lag on top of the playhead.
          if (isFinite(lat) && isFinite(lng)) {
            last.lat = lat;
            last.lng = lng;
            last.heading = wrapHeading(heading);
            motion.inited = true;
            motion.fromLat = motion.toLat = lat;
            motion.fromLng = motion.toLng = lng;
            motion.fromHdg = motion.toHdg = last.heading;
            motion.dur = 0;
            motion.t0 = performance.now();
          }
          if (!visible) {
            return;
          }
          applyCamera(false);
        }

        function loadMapLibre() {
          if (window.maplibregl) {
            return Promise.resolve(window.maplibregl);
          }
          return new Promise(function(resolve, reject) {
            var s = document.createElement('script');
            s.src = 'https://unpkg.com/maplibre-gl@5.6.2/dist/maplibre-gl.js';
            s.async = true;
            s.onload = function() {
              if (window.maplibregl) {
                resolve(window.maplibregl);
              } else {
                reject(new Error('MapLibre loaded without maplibregl'));
              }
            };
            s.onerror = function() {
              reject(new Error('MapLibre failed to load'));
            };
            document.head.appendChild(s);
          });
        }

        function loadThree() {
          if (threeMods) {
            return Promise.resolve(threeMods);
          }
          return Promise.all([
            import('three'),
            import('three/addons/loaders/GLTFLoader.js'),
            import('three/addons/environments/RoomEnvironment.js')
          ]).then(function(parts) {
            threeMods = {
              THREE: parts[0],
              GLTFLoader: parts[1].GLTFLoader,
              RoomEnvironment: parts[2].RoomEnvironment
            };
            return threeMods;
          });
        }

        function classifyCarSurface(r, g, b) {
          var lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
          var mx = Math.max(r, g, b);
          var mn = Math.min(r, g, b);
          var sat = mx > 1e-4 ? (mx - mn) / mx : 0;
          if (r > 0.72 && g > 0.72 && b < 0.38) {
            return { metal: 0.06, rough: 0.30, coat: 0.40, coatR: 0.10 };
          }
          if (r > 0.38 && g < 0.20 && b < 0.08) {
            return { metal: 0.08, rough: 0.28, coat: 0.50, coatR: 0.08 };
          }
          if (b > r + 0.03 && b > g && sat > 0.10) {
            return { metal: 0.14, rough: 0.16, coat: 1.0, coatR: 0.06 };
          }
          if (lum < 0.10) {
            return { metal: 0.0, rough: 0.92, coat: 0.0, coatR: 1.0 };
          }
          if (r > b + 0.03 && g > b && lum < 0.48 && sat > 0.07) {
            return { metal: 0.04, rough: 0.50, coat: 0.05, coatR: 0.35 };
          }
          if (sat < 0.08 && lum > 0.85) {
            return { metal: 0.08, rough: 0.20, coat: 0.55, coatR: 0.08 };
          }
          if (sat < 0.10 && lum > 0.45) {
            return { metal: 0.80, rough: 0.26, coat: 0.12, coatR: 0.18 };
          }
          if (sat < 0.10) {
            return { metal: 0.64, rough: 0.36, coat: 0.04, coatR: 0.28 };
          }
          return { metal: 0.06, rough: 0.46, coat: 0.0, coatR: 1.0 };
        }

        function applyCarShaders(root, THREE) {
          if (!root) {
            return;
          }
          root.traverse(function(obj) {
            if (!obj.isMesh || !obj.material) {
              return;
            }
            var mats = Array.isArray(obj.material) ? obj.material : [obj.material];
            var next = mats.map(function(m) {
              if (!m) {
                return m;
              }
              var col = m.color || new THREE.Color(1, 1, 1);
              var hasMap = !!m.map;
              var p = hasMap
                ? { metal: 0.02, rough: 0.40, coat: 0.14, coatR: 0.22 }
                : classifyCarSurface(col.r, col.g, col.b);
              return new THREE.MeshPhysicalMaterial({
                map: m.map || null,
                color: col,
                metalness: p.metal,
                roughness: p.rough,
                clearcoat: p.coat,
                clearcoatRoughness: p.coatR,
                envMapIntensity: 0.9,
                transparent: false,
                opacity: 1,
                transmission: 0,
                side: THREE.DoubleSide,
                depthWrite: true
              });
            });
            obj.material = next.length === 1 ? next[0] : next;
            obj.castShadow = false;
            obj.receiveShadow = false;
            obj.frustumCulled = false;
          });
        }


        function makeCarLayer(src) {
          var mods = threeMods;
          var THREE = mods.THREE;
          var loader = new mods.GLTFLoader();
          var carRoot = null;
          var loadingSrc = '';
          var loadedSrc = '';

          function placeCar() {
            if (!carRoot || !map) {
              return;
            }
            carRoot.position.set(0, 0, 0);
            // new_dyn is +Z forward in Y-up. Scene Z is north after the MapLibre
            // ENU basis; +heading around Y turns north toward east (compass).
            carRoot.rotation.set(0, last.heading * Math.PI / 180, 0);
            var s = last.scale > 0 ? last.scale : 1;
            carRoot.scale.set(s, s, s);
          }

          function groundModel(scene) {
            scene.updateMatrixWorld(true);
            var box = new THREE.Box3().setFromObject(scene);
            if (!isFinite(box.min.x) || !isFinite(box.max.y)) {
              return scene;
            }
            var mid = new THREE.Vector3();
            box.getCenter(mid);
            scene.position.x -= mid.x;
            scene.position.z -= mid.z;
            scene.position.y -= box.min.y;
            var wrap = new THREE.Group();
            wrap.add(scene);
            wrap.userData.carLen = box.max.z - box.min.z;
            wrap.userData.carH = box.max.y - box.min.y;
            return wrap;
          }

          function ensureModel(url) {
            if (!url || url === loadedSrc || url === loadingSrc) {
              return;
            }
            loadingSrc = url;
            msg = 'OSM 3D: loading car…';
            loader.load(url, function(gltf) {
              if (loadingSrc !== url) {
                return;
              }
              if (carRoot) {
                layer.scene.remove(carRoot);
              }
              applyCarShaders(gltf.scene, THREE);
              carRoot = groundModel(gltf.scene);
              layer.scene.add(carRoot);
              loadedSrc = url;
              loadingSrc = '';
              placeCar();
              var len = carRoot.userData.carLen || 0;
              var ht = carRoot.userData.carH || 0;
              if (started) {
                msg = 'OSM 3D: terrain + buildings, car ' +
                      (len > 0 ? len.toFixed(1) + '×' + ht.toFixed(1) + ' m on DEM' : 'on DEM');
              }
              if (map) {
                map.triggerRepaint();
              }
            }, function(ev) {
              if (loadingSrc !== url || !ev || !ev.total) {
                return;
              }
              msg = 'OSM 3D: loading car ' + Math.round(100 * ev.loaded / ev.total) + '%';
            }, function(err) {
              if (loadingSrc === url) {
                loadingSrc = '';
                msg = 'OSM 3D: car GLB failed to load';
              }
              console.warn('photon car GLB', url, err);
            });
          }

          var bldgBusy = false;
          var bldgKey = '';
          var lastAtlasKey = '';

          function localEnu(lng, lat, elev, originMerc, meter) {
            var m = maplibregl.MercatorCoordinate.fromLngLat(new maplibregl.LngLat(lng, lat), elev);
            return {
              x: (m.x - originMerc.x) / meter,
              y: (m.z - originMerc.z) / meter,
              z: (originMerc.y - m.y) / meter
            };
          }

          function atlasUv(lng, lat, atlas) {
            var n = atlas.n;
            var fx = (lng + 180) / 360 * n - atlas.x0;
            var s = Math.sin(lat * Math.PI / 180);
            var fy = (0.5 - Math.log((1 + s) / (1 - s)) / (4 * Math.PI)) * n - atlas.y0;
            return { u: fx / atlas.nx, v: 1 - fy / atlas.ny };
          }

          function pushQuad(buf, ax, ay, az, au, av, bx, by, bz, bu, bv,
                            cx, cy, cz, cu, cv, dx, dy, dz, du, dv,
                            br, bg, bb, tr, tg, tb) {
            var i = buf.pos.length / 3;
            buf.pos.push(ax, ay, az, bx, by, bz, cx, cy, cz, dx, dy, dz);
            buf.uv.push(au, av, bu, bv, cu, cv, du, dv);
            // Bottom pair, then top pair — vertical gradient on walls.
            buf.col.push(br, bg, bb, br, bg, bb, tr, tg, tb, tr, tg, tb);
            buf.idx.push(i, i + 1, i + 2, i, i + 2, i + 3);
          }

          function ringCentroid(pts) {
            var lng = 0;
            var lat = 0;
            var i;
            for (i = 0; i < pts.length; i++) {
              lng += pts[i].lng;
              lat += pts[i].lat;
            }
            return { lng: lng / pts.length, lat: lat / pts.length };
          }

          // Nadir aerial on a vertical wall interpolates across the roof. Sample
          // a few meters *outside* the footprint instead, and pin one UV so the
          // facade is a sidewalk/shadow color with a height gradient.
          function facadeSample(a, b, centroid, atlas, metersOut) {
            var midLng = (a.lng + b.lng) * 0.5;
            var midLat = (a.lat + b.lat) * 0.5;
            var mPerLat = 111320;
            var mPerLng = 111320 * Math.cos(midLat * Math.PI / 180);
            var ex = (b.lng - a.lng) * mPerLng;
            var ey = (b.lat - a.lat) * mPerLat;
            var len = Math.sqrt(ex * ex + ey * ey) || 1;
            var nx = -ey / len;
            var ny = ex / len;
            var toCx = (centroid.lng - midLng) * mPerLng;
            var toCy = (centroid.lat - midLat) * mPerLat;
            if (nx * toCx + ny * toCy > 0) {
              nx = -nx;
              ny = -ny;
            }
            var uv = atlasUv(midLng + nx * metersOut / mPerLng,
                             midLat + ny * metersOut / mPerLat, atlas);
            var face = 0.58 + 0.42 * Math.max(0, nx * 0.55 + ny * 0.28);
            return { u: uv.u, v: uv.v, face: face };
          }

          function insetUv(pt, centroid, atlas, metersIn) {
            var mPerLat = 111320;
            var mPerLng = 111320 * Math.cos(pt.lat * Math.PI / 180);
            var dx = (centroid.lng - pt.lng) * mPerLng;
            var dy = (centroid.lat - pt.lat) * mPerLat;
            var len = Math.sqrt(dx * dx + dy * dy) || 1;
            var t = Math.min(metersIn, len * 0.35);
            return atlasUv(pt.lng + dx / len * t / mPerLng,
                           pt.lat + dy / len * t / mPerLat, atlas);
          }

          function ringPts(ring, minH, originMerc, meter, atlas) {
            var pts = [];
            var n = ring.length;
            if (n > 1 && ring[0][0] === ring[n - 1][0] && ring[0][1] === ring[n - 1][1]) {
              n -= 1;
            }
            for (var i = 0; i < n; i++) {
              var lng = ring[i][0];
              var lat = ring[i][1];
              if (!isFinite(lng) || !isFinite(lat)) {
                continue;
              }
              var ge = groundElevAt(lng, lat);
              if (ge == null) {
                continue;
              }
              var loc = localEnu(lng, lat, ge + minH, originMerc, meter);
              var uv = atlasUv(lng, lat, atlas);
              pts.push({
                lng: lng, lat: lat, ge: ge,
                x: loc.x, y: loc.y, z: loc.z, u: uv.u, v: uv.v
              });
            }
            return pts;
          }

          function addPolygon(buf, coords, minH, ht, originMerc, meter, atlas, THREE) {
            if (!coords || !coords.length) {
              return;
            }
            var rings = [];
            for (var r = 0; r < coords.length; r++) {
              var pts = ringPts(coords[r], minH, originMerc, meter, atlas);
              if (pts.length >= 3) {
                rings.push(pts);
              }
            }
            if (!rings.length) {
              return;
            }
            var i;
            var a;
            var b;
            var at;
            var bt;
            var centroid = ringCentroid(rings[0]);
            for (r = 0; r < rings.length; r++) {
              var ring = rings[r];
              for (i = 0; i < ring.length; i++) {
                a = ring[i];
                b = ring[(i + 1) % ring.length];
                at = localEnu(a.lng, a.lat, a.ge + ht, originMerc, meter);
                bt = localEnu(b.lng, b.lat, b.ge + ht, originMerc, meter);
                var fac = facadeSample(a, b, centroid, atlas, 3.5);
                var bot = 0.40 * fac.face;
                var top = 0.92 * fac.face;
                pushQuad(buf,
                         a.x, a.y, a.z, fac.u, fac.v,
                         b.x, b.y, b.z, fac.u, fac.v,
                         bt.x, bt.y, bt.z, fac.u, fac.v,
                         at.x, at.y, at.z, fac.u, fac.v,
                         bot, bot, bot, top, top, top);
              }
            }
            var outer = rings[0];
            var contour = [];
            var holes = [];
            var tops = [];
            for (i = 0; i < outer.length; i++) {
              at = localEnu(outer[i].lng, outer[i].lat, outer[i].ge + ht, originMerc, meter);
              var ruv = insetUv(outer[i], centroid, atlas, 1.6);
              tops.push({ x: at.x, y: at.y, z: at.z, u: ruv.u, v: ruv.v });
              contour.push(new THREE.Vector2(at.x, at.z));
            }
            for (r = 1; r < rings.length; r++) {
              var hole = [];
              for (i = 0; i < rings[r].length; i++) {
                at = localEnu(rings[r][i].lng, rings[r][i].lat, rings[r][i].ge + ht, originMerc, meter);
                var huv = insetUv(rings[r][i], centroid, atlas, 1.6);
                tops.push({ x: at.x, y: at.y, z: at.z, u: huv.u, v: huv.v });
                hole.push(new THREE.Vector2(at.x, at.z));
              }
              holes.push(hole);
            }
            var faces = [];
            try {
              faces = THREE.ShapeUtils.triangulateShape(contour, holes);
            } catch (e) {
              faces = [];
            }
            var base = buf.pos.length / 3;
            for (i = 0; i < tops.length; i++) {
              buf.pos.push(tops[i].x, tops[i].y, tops[i].z);
              buf.uv.push(tops[i].u, tops[i].v);
              buf.col.push(1, 1, 1);
            }
            for (i = 0; i < faces.length; i++) {
              var f = faces[i];
              if (!f || f.length < 3) {
                continue;
              }
              buf.idx.push(base + f[0], base + f[1], base + f[2]);
            }
          }

          function disposeBldgs(group) {
            if (!group) {
              return;
            }
            var mesh = group.userData.mesh;
            if (mesh) {
              group.remove(mesh);
              if (mesh.geometry) {
                mesh.geometry.dispose();
              }
              if (mesh.material && mesh.material !== group.userData.mat) {
                mesh.material.dispose();
              }
            }
          }

          function paintAtlas(atlas, tex) {
            tex.image = atlas.canvas;
            tex.needsUpdate = true;
          }

          function fetchAtlas(done) {
            var z = Math.min(19, Math.max(16, Math.floor(map.getZoom() + 0.15)));
            var bounds = map.getBounds();
            var nw = webMercTile(bounds.getWest(), bounds.getNorth(), z);
            var se = webMercTile(bounds.getEast(), bounds.getSouth(), z);
            var x0 = Math.min(nw.x, se.x);
            var y0 = Math.min(nw.y, se.y);
            var x1 = Math.max(nw.x, se.x);
            var y1 = Math.max(nw.y, se.y);
            var nx = x1 - x0 + 1;
            var ny = y1 - y0 + 1;
            while (nx * ny > 36) {
              if (nx >= ny && nx > 1) {
                x0 += 1;
                x1 -= 1;
                nx = x1 - x0 + 1;
              } else if (ny > 1) {
                y0 += 1;
                y1 -= 1;
                ny = y1 - y0 + 1;
              } else {
                break;
              }
            }
            var coords = [];
            var jobs = [];
            var y;
            var x;
            for (y = y0; y <= y1; y++) {
              for (x = x0; x <= x1; x++) {
                coords.push({ x: x, y: y });
                jobs.push(loadSatTile(z, x, y));
              }
            }
            Promise.all(jobs).then(function(imgs) {
              var canvas = document.createElement('canvas');
              canvas.width = nx * 256;
              canvas.height = ny * 256;
              var ctx = canvas.getContext('2d');
              ctx.fillStyle = '#6a6458';
              ctx.fillRect(0, 0, canvas.width, canvas.height);
              var i;
              for (i = 0; i < imgs.length; i++) {
                if (!imgs[i]) {
                  continue;
                }
                ctx.drawImage(imgs[i], (coords[i].x - x0) * 256, (coords[i].y - y0) * 256);
              }
              done({
                canvas: canvas, z: z, x0: x0, y0: y0, nx: nx, ny: ny, n: 1 << z,
                key: z + ':' + x0 + ',' + y0 + 'x' + nx + 'x' + ny
              });
            });
          }

          function rebuildSatBuildings(force) {
            if (bldgBusy || !map || !layer.scene || !visible) {
              return;
            }
            if (!haveElev) {
              return;
            }
            var c = map.getCenter();
            var cell = Math.floor(map.getZoom()) + ':' + c.lat.toFixed(3) + ':' + c.lng.toFixed(3);
            if (!force && cell === bldgKey && layer.bldgGroup && layer.bldgGroup.userData.mesh) {
              return;
            }
            bldgBusy = true;
            fetchAtlas(function(atlas) {
              try {
                var originMerc = maplibregl.MercatorCoordinate.fromLngLat(
                    new maplibregl.LngLat(last.lng, last.lat), lastElev + ELEV_LIFT_M);
                var meter = originMerc.meterInMercatorCoordinateUnits();
                var feats = [];
                try {
                  feats = map.querySourceFeatures('openmaptiles', { sourceLayer: 'building' }) || [];
                } catch (e) {
                  feats = [];
                }
                var seen = {};
                var buf = { pos: [], uv: [], col: [], idx: [] };
                var nBuild = 0;
                var f;
                for (f = 0; f < feats.length && nBuild < 180; f++) {
                  var feat = feats[f];
                  var g = feat && feat.geometry;
                  var p = feat && feat.properties ? feat.properties : {};
                  if (!g || (g.type !== 'Polygon' && g.type !== 'MultiPolygon')) {
                    continue;
                  }
                  var ht = Number(p.render_height != null ? p.render_height : p.height);
                  if (!isFinite(ht) || ht < 5) {
                    continue;
                  }
                  var minH = Number(p.render_min_height != null ? p.render_min_height : p.min_height);
                  if (!isFinite(minH) || minH < 0) {
                    minH = 0;
                  }
                  if (ht - minH < 2) {
                    continue;
                  }
                  var c0 = g.type === 'Polygon' ? g.coordinates[0][0] : g.coordinates[0][0][0];
                  var id = (c0 ? c0[0].toFixed(5) + ',' + c0[1].toFixed(5) : f) + ':' + ht;
                  if (seen[id]) {
                    continue;
                  }
                  seen[id] = 1;
                  if (g.type === 'Polygon') {
                    addPolygon(buf, g.coordinates, minH, ht, originMerc, meter, atlas, THREE);
                  } else {
                    var pi;
                    for (pi = 0; pi < g.coordinates.length; pi++) {
                      addPolygon(buf, g.coordinates[pi], minH, ht, originMerc, meter, atlas, THREE);
                    }
                  }
                  nBuild += 1;
                }
                if (!layer._satTex) {
                  layer._satTex = new THREE.CanvasTexture(atlas.canvas);
                  layer._satTex.colorSpace = THREE.SRGBColorSpace;
                  layer._satTex.anisotropy = 4;
                  layer._satTex.minFilter = THREE.LINEAR_MIPMAP_LINEAR;
                  layer._satTex.magFilter = THREE.LINEAR;
                  layer._satTex.generateMipmaps = true;
                  lastAtlasKey = atlas.key || '';
                } else if (atlas.key !== lastAtlasKey) {
                  paintAtlas(atlas, layer._satTex);
                  lastAtlasKey = atlas.key || '';
                }
                if (!layer._satMat) {
                  layer._satMat = new THREE.MeshBasicMaterial({
                    map: layer._satTex,
                    vertexColors: true,
                    side: THREE.DoubleSide,
                    polygonOffset: true,
                    polygonOffsetFactor: -1,
                    polygonOffsetUnits: -1
                  });
                } else if (layer._satMat.map !== layer._satTex) {
                  layer._satMat.map = layer._satTex;
                  layer._satMat.needsUpdate = true;
                }
                if (buf.idx.length > 0) {
                  var geo = new THREE.BufferGeometry();
                  geo.setAttribute('position', new THREE.Float32BufferAttribute(buf.pos, 3));
                  geo.setAttribute('uv', new THREE.Float32BufferAttribute(buf.uv, 2));
                  geo.setAttribute('color', new THREE.Float32BufferAttribute(buf.col, 3));
                  geo.setIndex(buf.idx);
                  geo.computeVertexNormals();
                  var mesh = new THREE.Mesh(geo, layer._satMat);
                  mesh.frustumCulled = false;
                  var prev = layer.bldgGroup.userData.mesh;
                  layer.bldgGroup.add(mesh);
                  layer.bldgGroup.userData.mesh = mesh;
                  if (prev) {
                    layer.bldgGroup.remove(prev);
                    if (prev.geometry) {
                      prev.geometry.dispose();
                    }
                  }
                  layer._bldgAnchor = {
                    lat: last.lat, lng: last.lng, elev: lastElev + ELEV_LIFT_M
                  };
                  bldgKey = cell;
                  showBeigeBuildings(false);
                } else if (!layer.bldgGroup.userData.mesh) {
                  bldgKey = '';
                }
                if (map) {
                  map.triggerRepaint();
                }
              } finally {
                bldgBusy = false;
              }
            });
          }

          var layer = {
            id: 'photon-car',
            type: 'custom',
            renderingMode: '3d',
            camera: null,
            scene: null,
            renderer: null,
            _proj: null,
            _local: null,
            _scale: null,
            _elev: 0,
            hasModel: function() { return !!carRoot; },
            setSrc: ensureModel,
            onAdd: function(mapInst, gl) {
              this.camera = new THREE.Camera();
              this.scene = new THREE.Scene();
              this.scene.rotateX(Math.PI / 2);
              this.scene.scale.multiply(new THREE.Vector3(1, 1, -1));
              this._proj = new THREE.Matrix4();
              this._local = new THREE.Matrix4();
              this._scale = new THREE.Vector3();
              this._elev = 0;
              this.bldgGroup = new THREE.Group();
              this.scene.add(this.bldgGroup);
              this._satTex = null;
              this._satMat = null;
              this._bldgAnchor = null;
              this.refreshBuildings = rebuildSatBuildings;

              var hemi = new THREE.HemisphereLight(0xd8e6ff, 0x3d3428, 0.85);
              this.scene.add(hemi);
              var sun = new THREE.DirectionalLight(0xfff2d6, 1.35);
              sun.position.set(45, 80, -25);
              this.scene.add(sun);
              var fill = new THREE.DirectionalLight(0x8eb4ff, 0.35);
              fill.position.set(-40, 30, 20);
              this.scene.add(fill);

              this.renderer = new THREE.WebGLRenderer({
                canvas: mapInst.getCanvas(),
                context: gl,
                antialias: true
              });
              this.renderer.autoClear = false;
              this.renderer.outputColorSpace = THREE.SRGBColorSpace;
              this.renderer.toneMapping = THREE.ACESFilmicToneMapping;
              this.renderer.toneMappingExposure = 1.15;
              try {
                var pmrem = new THREE.PMREMGenerator(this.renderer);
                this.scene.environment = pmrem.fromScene(new mods.RoomEnvironment(), 0.04).texture;
              } catch (e) {}
              ensureModel(src);
            },
            render: function(gl, args) {
              if (!this.renderer || !this.camera || !visible) {
                return;
              }
              var uMove = stepLerp();
              if (carRoot) {
                placeCar();
              }
              var elev = sampleElev(new maplibregl.LngLat(last.lng, last.lat));
              if (elev == null || !isFinite(elev)) {
                // DEM tile not in yet — do not plant the car at 0 m (COTA is ~150 m).
                return;
              }
              this._elev = elev + ELEV_LIFT_M;
              var originMerc = maplibregl.MercatorCoordinate.fromLngLat(
                  new maplibregl.LngLat(last.lng, last.lat), this._elev);
              var u = originMerc.meterInMercatorCoordinateUnits();
              if (this.bldgGroup && this._bldgAnchor) {
                var a = this._bldgAnchor;
                var am = maplibregl.MercatorCoordinate.fromLngLat(
                    new maplibregl.LngLat(a.lng, a.lat), a.elev);
                this.bldgGroup.position.set(
                    (am.x - originMerc.x) / u,
                    (am.z - originMerc.z) / u,
                    (originMerc.y - am.y) / u);
              }
              var bk = Math.floor(map.getZoom()) + ':' + last.lat.toFixed(3) + ':' +
                       last.lng.toFixed(3);
              if (bk !== bldgKey) {
                rebuildSatBuildings(false);
              }
              this._scale.set(u, -u, u);
              this._proj.fromArray(args.defaultProjectionData.mainMatrix);
              this._local.makeTranslation(originMerc.x, originMerc.y, originMerc.z).scale(this._scale);
              this.camera.projectionMatrix.copy(this._proj).multiply(this._local);
              this.renderer.resetState();
              this.renderer.render(this.scene, this.camera);
              if (uMove < 1) {
                if (last.follow) {
                  applyCamera(false);
                }
                map.triggerRepaint();
              }
            }
          };
          return layer;
        }

        function enhanceStyle(style) {
          style.sources.terrainSource = {
            type: 'raster-dem',
            url: 'https://tiles.mapterhorn.com/tilejson.json'
          };
          style.terrain = { source: 'terrainSource', exaggeration: 1.0 };
          // One DEM stream for mesh + hillshade. Ground color is draped satellite, not hypsometric tint
          // (Austin/COTA is too flat for elevation color to read as anything but beige).
          style.sources.photonSat = {
            type: 'raster',
            tiles: [
              'https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}'
            ],
            tileSize: 256,
            maxzoom: 19,
            attribution: 'Esri'
          };
          style.light = {
            anchor: 'map',
            color: '#fff1dc',
            intensity: 0.52,
            position: [1.35, 215, 26]
          };
          style.sky = {
            'sky-color': '#6ea8d4',
            'horizon-color': '#f3e6d4',
            'fog-color': '#d5dee6',
            'sky-horizon-blend': 0.32,
            'horizon-fog-blend': 0.58,
            'fog-ground-blend': 0.38,
            'atmosphere-blend': 0.62
          };
          style.layers.splice(1, 0, {
            id: 'photon-sat',
            type: 'raster',
            source: 'photonSat',
              paint: {
                'raster-opacity': 1,
                'raster-fade-duration': 0,
                'raster-saturation': 0.06,
                'raster-contrast': 0.04
              }
          }, {
            id: 'photon-hills',
            type: 'hillshade',
            source: 'terrainSource',
            paint: {
              'hillshade-illumination-anchor': 'map',
              'hillshade-illumination-direction': 215,
              'hillshade-shadow-color': '#5a5348',
              'hillshade-highlight-color': '#fff6e8',
              'hillshade-accent-color': '#c4b8a4',
              'hillshade-exaggeration': 0.12
            }
          });
          style.layers.forEach(function(layer) {
            var id = layer.id || '';
            var paint = layer.paint || (layer.paint = {});
            if (layer.type === 'background') {
              paint['background-color'] = '#2a2e28';
            }
            // Esri World Imagery is the ground. OSM Liberty (roads, parks,
            // water, labels, Natural Earth, beige extrusions) is a different
            // survey and sits several meters off the photo. Keep sat +
            // hillshade only. Building footprints stay in the vector source
            // for Three.js; the painted OSM layers stay off.
            if (id !== 'photon-sat' && id !== 'photon-hills' &&
                (layer.source === 'openmaptiles' || layer.source === 'ne2_shaded')) {
              layer.layout = layer.layout || {};
              layer.layout.visibility = 'none';
            }
          });
          var b = style.layers.find(function(l) { return l.id === 'building-3d'; });
          if (b) {
            b.minzoom = 14;
            b.layout = b.layout || {};
            b.paint = b.paint || {};
            var tall = ['>=', ['coalesce', ['get', 'render_height'], 0], 5];
            b.filter = b.filter ? ['all', b.filter, tall] : tall;
            b.paint['fill-extrusion-opacity'] = 1;
            b.paint['fill-extrusion-vertical-gradient'] = true;
            b.paint['fill-extrusion-translate-anchor'] = 'map';
            b.paint['fill-extrusion-color'] = [
              'interpolate', ['linear'], ['coalesce', ['get', 'render_height'], 10],
              0, '#f2ebe0',
              14, '#e7ddd0',
              28, '#d8d0c4',
              48, '#c8cdd3',
              80, '#b0becb',
              130, '#94a9bb'
            ];
          }
          return style;
        }

        function applyAtmosphere() {
          if (!map) {
            return;
          }
          try {
            if (typeof map.setSky === 'function') {
              map.setSky({
                'sky-color': '#6ea8d4',
                'horizon-color': '#f3e6d4',
                'fog-color': '#d5dee6',
                'sky-horizon-blend': 0.32,
                'horizon-fog-blend': 0.58,
                'fog-ground-blend': 0.38,
                'atmosphere-blend': 0.62
              });
            }
          } catch (e) {}
          try {
            if (typeof map.setLight === 'function') {
              map.setLight({
                anchor: 'map',
                color: '#fff1dc',
                intensity: 0.52,
                position: [1.35, 215, 26]
              });
            }
          } catch (e) {}
        }

        function bindTerrainRefresh() {
          if (!map) {
            return;
          }
          map.on('sourcedata', function(ev) {
            if (ev && ev.sourceId === 'terrainSource' && ev.isSourceLoaded && visible) {
              map.triggerRepaint();
            }
          });
        }

        function hideOsmCartography() {
          if (!map) {
            return;
          }
          var layers = [];
          try {
            layers = map.getStyle() && map.getStyle().layers ? map.getStyle().layers : [];
          } catch (e) {
            layers = [];
          }
          var i;
          for (i = 0; i < layers.length; i++) {
            var layer = layers[i];
            var id = layer && layer.id;
            if (!id || id === 'photon-sat' || id === 'photon-hills' || id === 'photon-car') {
              continue;
            }
            if (layer.source === 'openmaptiles' || layer.source === 'ne2_shaded') {
              try {
                map.setLayoutProperty(id, 'visibility', 'none');
              } catch (err) {}
            }
          }
        }

        function bindSatBuildings() {
          if (!map) {
            return;
          }
          var t = 0;
          function kick(force) {
            clearTimeout(t);
            t = setTimeout(function() {
              if (carLayer && typeof carLayer.refreshBuildings === 'function') {
                carLayer.refreshBuildings(!!force);
              }
            }, 420);
          }
          map.on('moveend', function() { kick(false); });
          map.on('zoomend', function() { kick(false); });
          map.on('sourcedata', function(ev) {
            // Sat tiles arriving must not rebuild meshes — that flashes
            // both the imagery atlas and the 3D boxes. Only OSM building
            // vectors, and only once the source is idle.
            if (ev && ev.sourceId === 'openmaptiles' && ev.isSourceLoaded && visible) {
              kick(false);
            }
          });
          kick(true);
        }

        function start(lat, lng, modelUrl, hidden) {
          var src = absUrl(modelUrl || 'models/newCar.glb');
          last.lat = lat;
          last.lng = lng;
          snapMotion(lat, lng, last.heading);
          if (started && map) {
            last.lat = lat;
            last.lng = lng;
            snapMotion(lat, lng, last.heading);
            if (carLayer && src && lastSrc !== src) {
              lastSrc = src;
              carLayer.setSrc(src);
            }
            applyCamera(true);
            if (!hidden) {
              show(true);
            }
            return;
          }
          if (starting) {
            pendingStart = { lat: lat, lng: lng, src: src, hidden: !!hidden };
            last.lat = lat;
            last.lng = lng;
            snapMotion(lat, lng, last.heading);
            return;
          }
          starting = true;
          lastSrc = src;
          msg = hidden ? 'warming OSM 3D…' : 'loading OSM 3D: terrain and buildings…';
          Promise.all([
            loadMapLibre(),
            styleJson ? Promise.resolve(styleJson) : fetch('https://tiles.openfreemap.org/styles/liberty').then(function(r) {
              if (!r.ok) {
                throw new Error('OpenFreeMap style failed');
              }
              return r.json();
            }),
            loadThree()
          ]).then(function(parts) {
            styleJson = parts[1];
            var style = enhanceStyle(JSON.parse(JSON.stringify(styleJson)));
            host.innerHTML = '';
            map = new maplibregl.Map({
              container: host,
              style: style,
              center: [lng, lat],
              zoom: rangeToZoom(last.range, lat),
              pitch: last.tilt || 62,
              bearing: last.heading || 0,
              interactive: true,
              dragPan: true,
              dragRotate: false,
              scrollZoom: false,
              boxZoom: false,
              keyboard: false,
              doubleClickZoom: false,
              touchZoomRotate: false,
              touchPitch: false,
              attributionControl: { compact: true },
              canvasContextAttributes: {
                antialias: false,
                powerPreference: 'low-power',
                failIfMajorPerformanceCaveat: false
              },
              maxPitch: 80,
              maxZoom: MAX_ZOOM,
              minZoom: 13,
              renderWorldCopies: false,
              crossSourceCollisions: false,
              fadeDuration: 0,
              refreshExpiredTiles: false,
              maxTileCacheSize: 220,
              pixelRatio: Math.min(window.devicePixelRatio || 1, 1.25)
            });
            carLayer = makeCarLayer(src);
            map.on('load', function() {
              applyAtmosphere();
              hideOsmCartography();
              if (!map.getLayer('photon-car')) {
                map.addLayer(carLayer);
              }
              started = true;
              starting = false;
              bindTerrainRefresh();
              bindUserCamera();
              bindLod();
              bindSatBuildings();
              if (pendingStart) {
                var p = pendingStart;
                pendingStart = null;
                last.lat = p.lat;
                last.lng = p.lng;
                snapMotion(p.lat, p.lng, last.heading);
                if (p.src && p.src !== lastSrc && carLayer) {
                  lastSrc = p.src;
                  carLayer.setSrc(p.src);
                }
                hidden = p.hidden;
              }
              applyCamera(true);
              msg = hidden ? 'OSM 3D tile cache warm' : 'OSM 3D: cached tiles, car on DEM';
              show(!hidden);
            });
            map.on('error', function(ev) {
              var err = ev && ev.error ? ev.error : ev;
              var text = String(err && err.message ? err.message : err || 'map error');
              if (/insecure|securityerror/i.test(text)) {
                return;
              }
              if (/fog|sky|terrain|relief|light|image|svg|sprite|icon|failed to fetch|404/i.test(text) && started) {
                return;
              }
              msg = 'OSM 3D: ' + text;
            });
          }).catch(function(err) {
            starting = false;
            started = false;
            map = null;
            carLayer = null;
            msg = String(err && err.message ? err.message : err);
            show(false);
          });
        }

        function warm() {
          if (started || starting) {
            return;
          }
          start(30.132792, -97.641144, 'models/newCar.glb', true);
        }

        function pose(lat, lng, heading, scale, range, tilt, follow) {
          applyPose(lat, lng, heading, scale, range, tilt, follow);
        }

        function setInteractive(on) {
          interactive = !!on;
          if (!host) {
            return;
          }
          host.style.pointerEvents = (visible && interactive) ? 'auto' : 'none';
        }

        return {
          status: status, ready: ready, following: following,
          setRect: setRect, show: show, start: start, pose: pose, warm: warm,
          resetCam: resetCam, setInteractive: setInteractive
        };
      })();
