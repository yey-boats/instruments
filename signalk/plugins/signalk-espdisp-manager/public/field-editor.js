/* Slice 5 — manifest-gated field editor (browser).
 *
 * A device-and-manifest oriented CRUD surface, distinct from the profile/display
 * editor in layout-editor.html. Flow:
 *   1. Pick a connected device. We fetch its capability manifest
 *      (GET /devices/:id/capabilities) — or fall back to a built-in default set
 *      when the device reports none (offline / pre-manifest firmware) — and the
 *      editable layout (GET /devices/:id/editor/layout).
 *   2. CRUD of views (add/rename/reorder/delete, blocked past manifest.maxViews)
 *      and fields per view (add/remove/configure, blocked past
 *      manifest.maxTilesPerScreen). All writes hit /editor/* JSON routes; the
 *      server re-validates against the manifest, persists into the assigned
 *      profile, and reloads the device.
 *   3. Per field: a view-type picker (manifest types only), named SignalK path
 *      binding(s) with autocomplete, title, format mask, font-size select
 *      (from fontSizes), unit select (from the path's unit family), color
 *      (scheme presets + inline swatches), and — for gauge/bar — range + zones.
 *   4. SignalK metadata: when a path is bound we fetch its meta (units,
 *      displayScale, zones) same-origin and prefill unit + range/zones; a
 *      "save limits" action persists them and can opt-in write them back to the
 *      SK path meta.
 *
 * The server is authoritative on gating (lib/field-schema.js); the browser does
 * optimistic gating from the same manifest so the UI only offers legal choices.
 */
(function () {
  'use strict';

  var API = '/plugins/espdisp-manager';

  // Built-in default manifest mirrors lib/field-schema.js DEFAULT_MANIFEST. Used
  // when the device reports no ui.capabilities so the editor still works.
  var DEFAULT_MANIFEST = {
    viewTypes: {
      numeric: { paths: ['value'], attrs: ['title', 'format', 'size', 'unit', 'color'], zoom: ['auto'] },
      compass: { paths: ['value'], markers: true, attrs: ['title', 'size', 'color', 'reference'], zoom: ['auto', 'screenRef'] },
      windCircle: { paths: ['value', 'dir'], markers: true, attrs: ['title', 'format', 'size', 'unit', 'color', 'reference'], zoom: ['auto', 'screenRef'] },
      gauge: { paths: ['value'], attrs: ['title', 'size', 'unit', 'color', 'range', 'zones'], zoom: ['auto'] },
      bar: { paths: ['value'], attrs: ['title', 'size', 'unit', 'color', 'range', 'zones'], zoom: ['auto'] },
      trend: { paths: ['value'], attrs: ['title', 'size', 'unit', 'color'], zoom: ['auto'] },
      text: { paths: ['value'], attrs: ['title', 'size', 'color'], zoom: [] },
      control: { paths: ['value'], attrs: ['title', 'size', 'color'], controls: ['autopilot'], zoom: ['screenRef'] }
    },
    fontSizes: [12, 14, 16, 20, 28, 32, 38, 48, 64],
    units: { speed: ['kn', 'm/s'], angle: ['deg'], depth: ['m', 'ft'], temp: ['C', 'F'], ratio: ['%'], voltage: ['V'] },
    maxViews: 8, maxTilesPerScreen: 4, maxMarkersPerDial: 12,
    paths: 'open', controls: ['autopilot'], themes: ['day', 'night', 'high-contrast']
  };

  var RANGED = ['gauge', 'bar'];
  var COMPASS_LIKE = ['compass', 'windCircle'];

  // Color element keys per view type (mirrors COLOR_ELEMENTS in field-schema.js).
  var COLOR_ELEMENTS = {
    numeric: ['value', 'label'], text: ['value', 'label'], trend: ['value', 'label', 'line'],
    gauge: ['value', 'label', 'needle', 'arc'], bar: ['value', 'label', 'fill'],
    compass: ['value', 'label', 'needle'], windCircle: ['value', 'label', 'needle', 'dir'],
    control: ['value', 'label']
  };

  // Color-scheme presets (Day/Night/High-contrast). Element -> hex per scheme.
  // Night matches the editor :root palette; the others are sensible variants.
  var SCHEMES = {
    night: { value: '#4fc3f7', label: '#8fa7bd', needle: '#ffb84d', fill: '#36d399', dir: '#288cff', arc: '#1f2d3d', line: '#4fc3f7' },
    day: { value: '#0277bd', label: '#37474f', needle: '#ef6c00', fill: '#2e7d32', dir: '#1565c0', arc: '#b0bec5', line: '#0277bd' },
    'high-contrast': { value: '#ffffff', label: '#ffff00', needle: '#ff0000', fill: '#00ff00', dir: '#00ffff', arc: '#ffffff', line: '#ffffff' }
  };

  // Theme named swatches the inline picker offers (consistent with the editor).
  var THEME_SWATCHES = ['#4fc3f7', '#36d399', '#ffb84d', '#ff5252', '#288cff', '#8fa7bd', '#eef4fa'];

  // path -> physical-quantity family (mirrors quantityForPath in field-schema.js).
  function quantityForPath (p) {
    p = String(p || '');
    if (!p) return null;
    if (/speed|drift|velocity|sog|stw|tws|aws/i.test(p)) return 'speed';
    if (/angle|heading|course|bearing|direction|setTrue|cog|track/i.test(p)) return 'angle';
    if (/depth/i.test(p)) return 'depth';
    if (/temperature|temp/i.test(p)) return 'temp';
    if (/stateOfCharge|currentLevel|relativeHumidity|ratio/i.test(p)) return 'ratio';
    if (/voltage/i.test(p)) return 'voltage';
    return null;
  }
  function unitsForPath (manifest, p) {
    var fam = quantityForPath(p);
    if (!fam) return [];
    return (manifest.units && manifest.units[fam]) || [];
  }

  // SignalK meta -> field defaults (mirrors metaToFieldDefaults in field-schema.js).
  var SK_UNIT = { 'm/s': 'm/s', rad: 'deg', K: 'C', m: 'm', V: 'V', ratio: '%', '%': '%' };
  var ZONE_COLORS = { nominal: '#4caf50', normal: '#4caf50', alert: '#ffb300', warn: '#ff9800', alarm: '#ff5252', emergency: '#d50000' };
  function metaToDefaults (meta) {
    var out = {};
    if (!meta || typeof meta !== 'object') return out;
    if (typeof meta.units === 'string' && SK_UNIT[meta.units]) out.unit = SK_UNIT[meta.units];
    var ds = meta.displayScale;
    if (ds && typeof ds.lower === 'number' && typeof ds.upper === 'number' && ds.lower < ds.upper) {
      out.range = { min: ds.lower, max: ds.upper };
    }
    if (Array.isArray(meta.zones) && meta.zones.length) {
      out.zones = meta.zones.map(function (z) {
        var zone = {};
        if (typeof z.lower === 'number') zone.lower = z.lower;
        if (typeof z.upper === 'number') zone.upper = z.upper;
        if (typeof z.state === 'string') { zone.state = z.state; if (ZONE_COLORS[z.state]) zone.color = ZONE_COLORS[z.state]; }
        return zone;
      }).filter(function (z) { return z.lower != null || z.upper != null; });
    }
    return out;
  }

  // --- API client (same auth convention as the rest of the editor) --------
  function authHeaders () {
    var h = { 'content-type': 'application/json' };
    var t = localStorage.getItem('SK_TOKEN');
    if (t) h.authorization = 'Bearer ' + t;
    h['x-espdisp-authorization'] = 'Bearer ' + (localStorage.getItem('ESPDISP_DEV_TOKEN') || 'espdisp-dev');
    return h;
  }
  function api (path, opts) {
    return fetch(API + path, Object.assign({ credentials: 'include', headers: authHeaders() }, opts || {}))
      .then(function (r) {
        var ct = r.headers.get('content-type') || '';
        var body = ct.indexOf('application/json') >= 0 ? r.json() : r.text();
        if (!r.ok) return body.then(function (b) { throw new Error((b && b.error && b.error.message) || ('HTTP ' + r.status)); });
        return body;
      });
  }
  // Same-origin SignalK meta fetch (how live-preview reaches SignalK).
  function skMeta (path) {
    var url = '/signalk/v1/api/vessels/self/' + path.split('.').join('/') + '/meta';
    return fetch(url, { credentials: 'include' }).then(function (r) { return r.ok ? r.json() : null; }).catch(function () { return null; });
  }

  // --- DOM helpers --------------------------------------------------------
  function el (tag, props) {
    var n = document.createElement(tag);
    if (props) for (var k in props) {
      if (k === 'class') n.className = props[k];
      else if (k === 'text') n.textContent = props[k];
      else if (k.slice(0, 2) === 'on') n.addEventListener(k.slice(2).toLowerCase(), props[k]);
      else if (props[k] != null) n.setAttribute(k, props[k]);
    }
    for (var i = 2; i < arguments.length; i++) {
      var c = arguments[i];
      if (c == null) continue;
      if (Array.isArray(c)) c.forEach(function (x) { if (x) n.append(x.nodeType ? x : document.createTextNode(String(x))); });
      else n.append(c.nodeType ? c : document.createTextNode(String(c)));
    }
    return n;
  }

  var elDevice = document.getElementById('fe-device');
  var elScreens = document.getElementById('fe-screens');
  var elStatus = document.getElementById('fe-status');
  var elManifestSrc = document.getElementById('fe-manifest-src');
  var elAddScreen = document.getElementById('fe-add-screen');
  if (!elDevice || !elScreens) return; // editor markup not present

  var skPaths = []; // autocomplete catalogue
  var state = { deviceId: '', manifest: DEFAULT_MANIFEST, manifestReported: false, screens: [], items: {} };

  function status (msg, kind) {
    elStatus.textContent = msg || '';
    elStatus.className = 'status' + (kind ? ' ' + kind : '');
  }

  function viewTypeKeys () { return Object.keys(state.manifest.viewTypes || {}); }
  function typeSpec (type) { return (state.manifest.viewTypes || {})[type] || null; }
  function requiredPaths (type) { var s = typeSpec(type); return (s && s.paths) || ['value']; }
  function zoomModes (type) { var s = typeSpec(type); return (s && s.zoom) || []; }

  // The full field tuple stored on a widget item (server is canonical).
  function fieldFromItem (item) {
    item = item || {};
    return {
      type: item.type || 'numeric',
      title: item.title || '',
      paths: item.paths || (item.path ? { value: item.path } : {}),
      format: item.format || '',
      size: item.size != null ? item.size : null,
      unit: item.unit || '',
      color: item.color || {},
      range: item.range || null,
      zones: item.zones || null,
      zoomable: item.zoomable,
      zoom: item.zoom || '',
      reference: item.reference || '',
      markers: item.markers || []
    };
  }

  // ---------------------------------------------------------------------
  // Load
  function loadDevices () {
    return api('/devices').then(function (d) {
      var list = (d && d.devices) || [];
      elDevice.replaceChildren(el('option', { value: '', text: '— select a device —' }));
      list.forEach(function (dev) {
        var id = dev.id || dev.deviceId;
        elDevice.append(el('option', { value: id, text: (dev.name || id) }));
      });
      if (state.deviceId) elDevice.value = state.deviceId;
    }).catch(function (e) { status('device list failed: ' + e.message, 'err'); });
  }

  function loadPaths () {
    return api('/presets/widgets').then(function (d) { skPaths = (d && d.paths) || []; }).catch(function () { skPaths = []; });
  }

  function loadLayout () {
    if (!state.deviceId) { elScreens.replaceChildren(); elAddScreen.disabled = true; elManifestSrc.textContent = ''; return Promise.resolve(); }
    status('loading…');
    return api('/devices/' + encodeURIComponent(state.deviceId) + '/capabilities').then(function (c) {
      var reported = c && c.capabilities;
      state.manifestReported = !!(reported && reported.viewTypes);
      state.manifest = state.manifestReported ? reported : DEFAULT_MANIFEST;
      elManifestSrc.textContent = state.manifestReported ? 'manifest: device-reported' : 'manifest: built-in default (device offline / pre-manifest)';
      elManifestSrc.className = 'status' + (state.manifestReported ? ' ok' : ' warn');
      return api('/devices/' + encodeURIComponent(state.deviceId) + '/editor/layout');
    }).then(function (layout) {
      state.screens = (layout && layout.screens) || [];
      state.items = (layout && layout.items) || {};
      if (layout && layout.manifest && layout.manifest.viewTypes) state.manifest = layout.manifest;
      render();
      status('ready', 'ok');
    }).catch(function (e) { status('load failed: ' + e.message, 'err'); });
  }

  function applyLayout (layout) {
    state.screens = (layout && layout.screens) || [];
    state.items = (layout && layout.items) || {};
    if (layout && layout.manifest && layout.manifest.viewTypes) state.manifest = layout.manifest;
    render();
  }

  // ---------------------------------------------------------------------
  // Mutations
  function addScreen () {
    var max = Number(state.manifest.maxViews || 8);
    if (state.screens.length >= max) { status('max views reached (' + max + ')', 'warn'); return; }
    var title = prompt('New view title', 'New View');
    if (title == null) return;
    api('/devices/' + encodeURIComponent(state.deviceId) + '/editor/screens', { method: 'POST', body: JSON.stringify({ title: title }) })
      .then(applyLayout).then(function () { status('view added', 'ok'); }).catch(function (e) { status(e.message, 'err'); });
  }
  function renameScreen (sid, title) {
    api('/devices/' + encodeURIComponent(state.deviceId) + '/editor/screens/' + encodeURIComponent(sid), { method: 'PATCH', body: JSON.stringify({ title: title }) })
      .then(applyLayout).catch(function (e) { status(e.message, 'err'); });
  }
  function deleteScreen (sid) {
    if (!confirm('Delete view "' + sid + '"?')) return;
    api('/devices/' + encodeURIComponent(state.deviceId) + '/editor/screens/' + encodeURIComponent(sid), { method: 'DELETE' })
      .then(applyLayout).then(function () { status('view deleted', 'ok'); }).catch(function (e) { status(e.message, 'err'); });
  }
  function moveScreen (sid, dir) {
    var order = state.screens.map(function (s) { return s.id; });
    var i = order.indexOf(sid);
    var j = i + dir;
    if (i < 0 || j < 0 || j >= order.length) return;
    order.splice(j, 0, order.splice(i, 1)[0]);
    api('/devices/' + encodeURIComponent(state.deviceId) + '/editor/screens/reorder', { method: 'POST', body: JSON.stringify({ order: order }) })
      .then(applyLayout).catch(function (e) { status(e.message, 'err'); });
  }
  function addField (sid) {
    var max = Number(state.manifest.maxTilesPerScreen || 4);
    var screen = state.screens.filter(function (s) { return s.id === sid; })[0];
    if (screen && (screen.tiles || []).length >= max) { status('max tiles reached (' + max + ')', 'warn'); return; }
    var firstType = viewTypeKeys()[0] || 'numeric';
    api('/devices/' + encodeURIComponent(state.deviceId) + '/editor/screens/' + encodeURIComponent(sid) + '/fields',
      { method: 'POST', body: JSON.stringify({ field: { type: firstType, paths: { value: '' } } }) })
      .then(applyLayout).then(function () { status('field added', 'ok'); }).catch(function (e) { status(e.message, 'err'); });
  }
  function saveField (sid, widgetId, field) {
    return api('/devices/' + encodeURIComponent(state.deviceId) + '/editor/screens/' + encodeURIComponent(sid) + '/fields/' + encodeURIComponent(widgetId),
      { method: 'PATCH', body: JSON.stringify({ field: field }) }).then(applyLayout);
  }
  function removeField (sid, widgetId) {
    api('/devices/' + encodeURIComponent(state.deviceId) + '/editor/screens/' + encodeURIComponent(sid) + '/fields/' + encodeURIComponent(widgetId), { method: 'DELETE' })
      .then(applyLayout).then(function () { status('field removed', 'ok'); }).catch(function (e) { status(e.message, 'err'); });
  }
  function saveLimits (sid, widgetId, field, writeBack) {
    return api('/devices/' + encodeURIComponent(state.deviceId) + '/editor/screens/' + encodeURIComponent(sid) + '/fields/' + encodeURIComponent(widgetId) + '/limits',
      { method: 'POST', body: JSON.stringify({ range: field.range, zones: field.zones, writeBack: !!writeBack, path: field.paths && field.paths.value }) })
      .then(function (r) { applyLayout(r); return r; });
  }

  // ---------------------------------------------------------------------
  // Render — one card per screen, one block per field.
  function pathInput (value, onChange) {
    var input = el('input', { value: value || '', placeholder: 'navigation.speedOverGround', list: 'sk-paths' });
    input.addEventListener('change', function () { onChange(input.value.trim()); });
    return input;
  }

  function colorSwatches (field, element, onPick) {
    var wrap = el('div', { class: 'swatches' });
    THEME_SWATCHES.forEach(function (hex) {
      var s = el('span', { class: 'swatch' + ((field.color && field.color[element]) === hex ? ' active' : ''), title: hex });
      s.style.background = hex;
      s.addEventListener('click', function () { onPick(hex); });
      wrap.append(s);
    });
    var custom = el('input', { type: 'color', value: (field.color && field.color[element]) || '#4fc3f7', title: 'custom' });
    custom.addEventListener('change', function () { onPick(custom.value); });
    wrap.append(custom);
    var clear = el('button', { class: 'fe-mini ghost', text: 'theme', title: 'use theme default (unset)' });
    clear.addEventListener('click', function () { onPick(null); });
    wrap.append(clear);
    return wrap;
  }

  function renderField (sid, widgetId, item) {
    var field = fieldFromItem(item);
    var box = el('div', { class: 'fe-field' });
    var spec = typeSpec(field.type) || {};
    var attrs = spec.attrs || [];

    // Row: type + title
    var typeSel = el('select');
    viewTypeKeys().forEach(function (t) { typeSel.append(el('option', { value: t, text: t })); });
    typeSel.value = field.type;
    typeSel.addEventListener('change', function () { field.type = typeSel.value; saveField(sid, widgetId, field).catch(function (e) { status(e.message, 'err'); }); });

    var titleInp = el('input', { value: field.title, placeholder: 'TITLE' });
    titleInp.addEventListener('change', function () { field.title = titleInp.value; saveField(sid, widgetId, field).catch(function (e) { status(e.message, 'err'); }); });

    box.append(el('div', { class: 'row' },
      el('label', {}, 'type', typeSel),
      attrs.indexOf('title') >= 0 ? el('label', {}, 'title', titleInp) : null,
      el('span', { class: 'grow' }),
      el('button', { class: 'fe-mini danger', text: 'remove', onClick: function () { removeField(sid, widgetId); } })
    ));

    // Row: named path bindings
    var pathRow = el('div', { class: 'row' });
    requiredPaths(field.type).forEach(function (key) {
      var inp = pathInput(field.paths[key] || '', function (v) {
        field.paths[key] = v;
        // On bind of the value path, prefill unit/range/zones from SK meta.
        if (key === 'value' && v) {
          skMeta(v).then(function (meta) {
            var d = metaToDefaults(meta);
            if (d.unit && !field.unit) field.unit = d.unit;
            if (d.range && RANGED.indexOf(field.type) >= 0 && !field.range) field.range = d.range;
            if (d.zones && RANGED.indexOf(field.type) >= 0 && !field.zones) field.zones = d.zones;
            saveField(sid, widgetId, field).catch(function (e) { status(e.message, 'err'); });
          });
        } else {
          saveField(sid, widgetId, field).catch(function (e) { status(e.message, 'err'); });
        }
      });
      pathRow.append(el('label', {}, key, inp));
    });
    box.append(pathRow);

    // Row: presentation attrs (size, unit, format) gated to manifest
    var presRow = el('div', { class: 'row' });
    if (attrs.indexOf('size') >= 0) {
      var sizeSel = el('select');
      sizeSel.append(el('option', { value: '', text: 'auto' }));
      (state.manifest.fontSizes || []).forEach(function (s) { sizeSel.append(el('option', { value: String(s), text: String(s) })); });
      sizeSel.value = field.size != null ? String(field.size) : '';
      sizeSel.addEventListener('change', function () { field.size = sizeSel.value ? Number(sizeSel.value) : null; saveField(sid, widgetId, field).catch(function (e) { status(e.message, 'err'); }); });
      presRow.append(el('label', {}, 'size', sizeSel));
    }
    if (attrs.indexOf('unit') >= 0) {
      var units = unitsForPath(state.manifest, field.paths.value);
      var unitSel = el('select');
      unitSel.append(el('option', { value: '', text: '—' }));
      units.forEach(function (u) { unitSel.append(el('option', { value: u, text: u })); });
      // include current unit even if family unknown so we don't silently drop it
      if (field.unit && units.indexOf(field.unit) < 0) unitSel.append(el('option', { value: field.unit, text: field.unit }));
      unitSel.value = field.unit || '';
      unitSel.addEventListener('change', function () { field.unit = unitSel.value; saveField(sid, widgetId, field).catch(function (e) { status(e.message, 'err'); }); });
      presRow.append(el('label', {}, 'unit', unitSel));
    }
    if (attrs.indexOf('format') >= 0) {
      var fmtInp = el('input', { value: field.format, placeholder: 'XX.X', size: 6 });
      fmtInp.addEventListener('change', function () { field.format = fmtInp.value; saveField(sid, widgetId, field).catch(function (e) { status(e.message, 'err'); }); });
      presRow.append(el('label', {}, 'format', fmtInp));
    }
    if (zoomModes(field.type).length) {
      var zoomSel = el('select');
      zoomSel.append(el('option', { value: '', text: 'no zoom' }));
      if (zoomModes(field.type).indexOf('auto') >= 0) zoomSel.append(el('option', { value: 'auto', text: 'auto' }));
      if (zoomModes(field.type).indexOf('screenRef') >= 0) {
        state.screens.forEach(function (s) { if (s.id !== sid) zoomSel.append(el('option', { value: s.id, text: 'open: ' + s.id })); });
      }
      zoomSel.value = field.zoom || '';
      zoomSel.addEventListener('change', function () { field.zoom = zoomSel.value; saveField(sid, widgetId, field).catch(function (e) { status(e.message, 'err'); }); });
      presRow.append(el('label', {}, 'zoom', zoomSel));
    }
    if (presRow.childNodes.length) box.append(presRow);

    // Row: color scheme preset + per-element inline pickers
    if (attrs.indexOf('color') >= 0) {
      var schemeSel = el('select');
      schemeSel.append(el('option', { value: '', text: 'scheme…' }));
      (state.manifest.themes || ['day', 'night', 'high-contrast']).forEach(function (t) { schemeSel.append(el('option', { value: t, text: t })); });
      schemeSel.addEventListener('change', function () {
        var sch = SCHEMES[schemeSel.value];
        if (!sch) return;
        field.color = field.color || {};
        (COLOR_ELEMENTS[field.type] || ['value', 'label']).forEach(function (elm) { if (sch[elm]) field.color[elm] = sch[elm]; });
        saveField(sid, widgetId, field).catch(function (e) { status(e.message, 'err'); });
      });
      box.append(el('div', { class: 'row' }, el('label', {}, 'color scheme', schemeSel)));
      (COLOR_ELEMENTS[field.type] || ['value', 'label']).forEach(function (elm) {
        box.append(el('div', { class: 'row' }, el('label', {}, elm), colorSwatches(field, elm, function (hex) {
          field.color = field.color || {};
          if (hex == null) delete field.color[elm]; else field.color[elm] = hex;
          saveField(sid, widgetId, field).catch(function (e) { status(e.message, 'err'); });
        })));
      });
    }

    // Row: range + zones (gauge/bar only)
    if (attrs.indexOf('range') >= 0) {
      var rmin = el('input', { type: 'number', value: field.range ? field.range.min : '', placeholder: 'min', size: 5 });
      var rmax = el('input', { type: 'number', value: field.range ? field.range.max : '', placeholder: 'max', size: 5 });
      function commitRange () {
        if (rmin.value !== '' && rmax.value !== '') field.range = { min: Number(rmin.value), max: Number(rmax.value) };
        saveField(sid, widgetId, field).catch(function (e) { status(e.message, 'err'); });
      }
      rmin.addEventListener('change', commitRange); rmax.addEventListener('change', commitRange);
      var zonesTa = el('textarea', { class: 'zones', placeholder: '[{"lower":0,"upper":5,"state":"alarm","color":"#ff5252"}]' });
      zonesTa.value = field.zones ? JSON.stringify(field.zones) : '';
      zonesTa.addEventListener('change', function () {
        try { field.zones = zonesTa.value.trim() ? JSON.parse(zonesTa.value) : null; status(''); } catch (e) { status('zones JSON invalid', 'err'); return; }
        saveField(sid, widgetId, field).catch(function (e) { status(e.message, 'err'); });
      });
      var saveLimBtn = el('button', { class: 'fe-mini', text: 'save limits' });
      saveLimBtn.addEventListener('click', function () { saveLimits(sid, widgetId, field, false).then(function () { status('limits saved', 'ok'); }).catch(function (e) { status(e.message, 'err'); }); });
      var wbBtn = el('button', { class: 'fe-mini', text: 'save + write to SK meta' });
      wbBtn.addEventListener('click', function () {
        saveLimits(sid, widgetId, field, true).then(function (r) { status('limits saved · SK meta: ' + (r.metaWriteBack || '?'), r.metaWriteBack === 'ok' ? 'ok' : 'warn'); }).catch(function (e) { status(e.message, 'err'); });
      });
      box.append(el('div', { class: 'row' }, el('label', {}, 'range', rmin, rmax), saveLimBtn, wbBtn));
      box.append(el('div', { class: 'row' }, el('label', { style: 'align-items:flex-start' }, 'zones', zonesTa)));
    }

    // Reserved compass marker note (UI is the follow-on marker slice).
    if (COMPASS_LIKE.indexOf(field.type) >= 0 && attrs.indexOf('reference') >= 0) {
      var refInp = pathInput(field.reference || '', function (v) { field.reference = v; saveField(sid, widgetId, field).catch(function (e) { status(e.message, 'err'); }); });
      box.append(el('div', { class: 'row' }, el('label', {}, 'reference', refInp),
        el('span', { class: 'count', text: 'markers reserved for marker-rings slice (' + (field.markers || []).length + ')' })));
    }

    return box;
  }

  function renderScreen (screen) {
    var card = el('div', { class: 'fe-screen' });
    var titleInp = el('input', { class: 'title', value: screen.title || screen.id });
    titleInp.addEventListener('change', function () { renameScreen(screen.id, titleInp.value); });
    var tiles = screen.tiles || [];
    var maxTiles = Number(state.manifest.maxTilesPerScreen || 4);
    card.append(el('header', {},
      titleInp,
      el('button', { class: 'fe-mini', text: '↑', title: 'move up', onClick: function () { moveScreen(screen.id, -1); } }),
      el('button', { class: 'fe-mini', text: '↓', title: 'move down', onClick: function () { moveScreen(screen.id, 1); } }),
      el('button', { class: 'fe-mini danger', text: 'delete', onClick: function () { deleteScreen(screen.id); } })
    ));
    card.append(el('div', { class: 'count', text: screen.id + ' · ' + tiles.length + '/' + maxTiles + ' tiles' }));
    if (!tiles.length) card.append(el('div', { class: 'fe-empty', text: 'no fields yet' }));
    tiles.forEach(function (tile) {
      if (!tile || !tile.widget) return;
      var item = state.items[tile.widget];
      if (!item) { card.append(el('div', { class: 'fe-empty', text: 'tile ' + tile.widget + ' (legacy preset — edit above)' })); return; }
      card.append(renderField(screen.id, tile.widget, item));
    });
    var addBtn = el('button', { class: 'fe-mini', text: '+ add field' });
    addBtn.disabled = tiles.length >= maxTiles;
    addBtn.addEventListener('click', function () { addField(screen.id); });
    card.append(addBtn);
    return card;
  }

  function render () {
    elScreens.replaceChildren();
    elAddScreen.disabled = !state.deviceId || state.screens.length >= Number(state.manifest.maxViews || 8);
    // populate the shared sk-paths datalist (also used by the profile editor)
    var dl = document.getElementById('sk-paths');
    if (dl && !dl.childNodes.length && skPaths.length) skPaths.forEach(function (p) { dl.append(el('option', { value: p })); });
    if (!state.deviceId) { elScreens.append(el('div', { class: 'fe-empty', text: 'select a device to edit its views' })); return; }
    if (!state.screens.length) { elScreens.append(el('div', { class: 'fe-empty', text: 'no views yet — add one' })); return; }
    state.screens.forEach(function (s) { elScreens.append(renderScreen(s)); });
  }

  // --- wire up ------------------------------------------------------------
  elDevice.addEventListener('change', function () { state.deviceId = elDevice.value; loadLayout(); });
  document.getElementById('fe-reload').addEventListener('click', function () { loadDevices().then(loadLayout); });
  elAddScreen.addEventListener('click', addScreen);

  Promise.all([loadPaths(), loadDevices()]).then(function () { render(); });
}());
