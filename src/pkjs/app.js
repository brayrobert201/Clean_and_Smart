var Clay = require('@rebble/clay');
var clayConfig = require('./config.json');
var clay = new Clay(clayConfig, null, { autoHandleEvents: false });

// TfNSW API key and station config, baked in at build time (gitignored;
// copy secrets.json.example to secrets.json and fill in your values)
var SECRETS = require('./secrets.json');

var current_settings;
var last_weather_fetch = 0;
var last_usage_fetch = 0;   // last successful usage poll
var last_usage_attempt = 0; // last poll attempt (success or fail) - rate-limit floor

/*  ****************************************** Weather Section **************************************************** */

// converts open-meteo weather icon code to Yahoo weather icon code (to reuse current bitmap with icon set)
var OpenMetroCodeToYahooIcon = function (weather_code, is_day) {
  var yahoo_icon = 3200; //initially not defined

  if (weather_code === 0) {
    yahoo_icon = is_day === 1 ? 32 : 31; // sunny or clear night
  } else if ([51, 53, 55, 61, 63, 65, 80, 81, 82, 95, 96, 99].includes(weather_code)) {
    yahoo_icon = 11; //showers
  } else if ([71, 73, 75, 77, 85, 86].includes(weather_code)) {
    yahoo_icon = 16; //snow
  } else if ([56, 57, 66, 67].includes(weather_code)) {
    yahoo_icon = 18; //sleet
  } else if ([45, 48].includes(weather_code)) {
    yahoo_icon = 20; //foggy
  } else if (weather_code === 3) {
    yahoo_icon = 26; //cloudy
  } else if ([1, 2].includes(weather_code)) {
    yahoo_icon = is_day === 1 ? 30 : 29; //partly cloudy day or night
  }

  return yahoo_icon;
};

function getWeather(coords) {
  var url = 'https://api.open-meteo.com/v1/forecast?latitude=' + coords.latitude +
            '&longitude=' + coords.longitude +
            '&current=temperature_2m,weather_code,is_day&temperature_unit=' +
            (current_settings.temperatureFormat === 0 ? 'fahrenheit' : 'celsius');

  var xhr = new XMLHttpRequest();
  xhr.onload = function () {
    var json = JSON.parse(this.responseText);
    var temperature = json.current.temperature_2m;
    var code = json.current.weather_code;
    var is_day = json.current.is_day;

    last_weather_fetch = Date.now();

    Pebble.sendAppMessage({
      'KEY_WEATHER_CODE': OpenMetroCodeToYahooIcon(code, is_day),
      'KEY_WEATHER_TEMP': temperature
    }, function () {}, function () {});
  };
  xhr.onerror = function () {};
  xhr.open('GET', url);
  xhr.send();
}

function locationSuccess(pos) {
  getWeather(pos.coords);
}

function locationError() {}

function getLocation() {
  navigator.geolocation.getCurrentPosition(
    locationSuccess,
    locationError,
    { timeout: 15000, maximumAge: 60000 }
  );
}

/*  ****************************************** Train Section **************************************************** */

function noop() {}

// resolves the effective train settings: Clay config (current_settings) wins, secrets.json fills the gaps.
// secrets.json keeps the API key out of git and lets the emulator run with zero config.
function getTrainSettings() {
  var c = current_settings || {};
  var s = SECRETS || {};
  return {
    apiKey:        c.tfnswApiKey || s.tfnswApiKey || '',
    home:          c.homeStation || s.homeStationName || s.homeStopId || '',
    work:          c.workStation || s.workStationName || s.workStopId || '',
    directionMode: (c.directionMode != null) ? c.directionMode : 0, // 0 = time of day, 1 = alternate
    cutoffHour:    (c.cutoffHour   != null) ? c.cutoffHour
                 : (s.afternoonCutoffHour != null ? s.afternoonCutoffHour : 12),
    fastest:       (c.fastestMarker != null) ? c.fastestMarker : 1
  };
}

// resolves a station NAME to a TfNSW global stop id via stop_finder (cached in localStorage).
// a purely numeric input is treated as a stop id and used directly.
function resolveStop(name, apiKey, cb) {
  name = String(name || '').trim();
  if (!name) { cb(null); return; }
  if (/^\d+$/.test(name)) { cb(name); return; }

  var cacheKey = 'stopid:' + name.toLowerCase();
  var cached = localStorage.getItem(cacheKey);
  if (cached) { cb(cached); return; }

  var url = 'https://api.transport.nsw.gov.au/v1/tp/stop_finder?outputFormat=rapidJSON' +
            '&type_sf=stop&name_sf=' + encodeURIComponent(name) + '&TfNSWSF=true';

  var xhr = new XMLHttpRequest();
  xhr.onload = function () {
    var id = null;
    try {
      var locs = (JSON.parse(this.responseText).locations) || [];
      var stops = locs.filter(function (l) { return l.type === 'stop'; });
      var best = stops.filter(function (l) { return l.isBest; })[0];
      if (!best && stops.length) {
        best = stops.sort(function (a, b) {
          return (b.matchQuality || 0) - (a.matchQuality || 0);
        })[0];
      }
      if (best && best.id) {
        id = best.id;
        localStorage.setItem(cacheKey, id);
      }
    } catch (ex) { id = null; }
    cb(id);
  };
  xhr.onerror = function () { cb(null); };
  xhr.open('GET', url);
  xhr.setRequestHeader('Authorization', 'apikey ' + apiKey);
  xhr.send();
}

// queries the Trip Planner for the next departures origin -> dest and sends two of them to the watch
function queryTrip(cfg, origin, dest, toWork) {
  // trains + metro only (exclude light rail, bus, coach, ferry, school bus); no itdDate/itdTime = depart now
  var url = 'https://api.transport.nsw.gov.au/v1/tp/trip?outputFormat=rapidJSON' +
            '&coordOutputFormat=EPSG:4326&depArrMacro=dep' +
            '&type_origin=any&name_origin=' + encodeURIComponent(origin) +
            '&type_destination=any&name_destination=' + encodeURIComponent(dest) +
            '&calcNumberOfTrips=6&excludedMeans=checkbox' +
            '&exclMOT_4=1&exclMOT_5=1&exclMOT_7=1&exclMOT_9=1&exclMOT_11=1' +
            '&TfNSWTR=true&version=10.2.1.42';

  var xhr = new XMLHttpRequest();
  xhr.onload = function () {
    var trips = [];
    try {
      var journeys = JSON.parse(this.responseText).journeys || [];
      var nowSec = Math.floor(Date.now() / 1000);

      for (var j = 0; j < journeys.length; j++) {
        var legs = journeys[j].legs || [];
        var dep = 0, arr = 0, platform = '';

        // first rail/metro leg -> departure time + platform
        for (var l = 0; l < legs.length; l++) {
          var p = legs[l].transportation && legs[l].transportation.product;
          if (p && (p['class'] === 1 || p['class'] === 2)) {
            var o = legs[l].origin;
            dep = Math.floor(Date.parse(o.departureTimeEstimated || o.departureTimePlanned) / 1000);
            var m = /Platform (\w+)/.exec(o.disassembledName || o.name || '');
            platform = m ? 'P' + m[1] : '';
            break;
          }
        }
        // last rail/metro leg -> end-to-end arrival time (handles interchanges)
        for (var k = legs.length - 1; k >= 0; k--) {
          var p2 = legs[k].transportation && legs[k].transportation.product;
          if (p2 && (p2['class'] === 1 || p2['class'] === 2)) {
            var d = legs[k].destination;
            arr = Math.floor(Date.parse(d.arrivalTimeEstimated || d.arrivalTimePlanned) / 1000);
            break;
          }
        }

        if (dep > nowSec && arr > 0) {
          trips.push({ dep: dep, arr: arr, platform: platform });
        }
      }
    } catch (ex) { trips = []; }

    // dedupe by departure, sort ascending
    var seen = {};
    trips = trips.filter(function (t) {
      if (seen[t.dep]) return false;
      seen[t.dep] = 1;
      return true;
    }).sort(function (a, b) { return a.dep - b.dep; });

    // express marker: flag a train only when it overtakes an earlier-departing one
    // (a genuine express worth waiting for). The first train is never an "express".
    var express = 0;
    if (cfg.fastest) {
      for (var i = 0; i < Math.min(2, trips.length); i++) {
        for (var n = 0; n < trips.length; n++) {
          if (trips[n].dep < trips[i].dep && trips[n].arr > trips[i].arr) { express |= (1 << i); break; }
        }
      }
    }

    Pebble.sendAppMessage({
      'KEY_TRAIN_DEPARTURE':  trips[0] ? trips[0].dep : 0,
      'KEY_TRAIN_PLATFORM':   trips[0] ? trips[0].platform : '',
      'KEY_TRAIN_DEPARTURE2': trips[1] ? trips[1].dep : 0,
      'KEY_TRAIN_PLATFORM2':  trips[1] ? trips[1].platform : '',
      'KEY_TRAIN_DIRECTION':  toWork ? 0 : 1,
      'KEY_TRAIN_EXPRESS':    express
    }, function () {
      // in alternate mode, flip direction only after a successful send
      if (cfg.directionMode === 1) {
        localStorage.setItem('trainFlip', toWork ? '1' : '0');
      }
    }, noop);
  };
  xhr.onerror = noop; // silent: watch keeps showing old departures or falls back
  xhr.open('GET', url);
  xhr.setRequestHeader('Authorization', 'apikey ' + cfg.apiKey);
  xhr.send();
}

// chooses direction, resolves both stations, then queries the next departures
function fetchTrain() {
  var cfg = getTrainSettings();

  if (!cfg.apiKey || !cfg.home || !cfg.work) {
    // not configured - tell the watch to fall back to day of week
    Pebble.sendAppMessage({ 'KEY_TRAIN_DEPARTURE': 0 }, noop, noop);
    return;
  }

  var toWork;
  if (cfg.directionMode === 1) {
    toWork = localStorage.getItem('trainFlip') !== '1'; // alternate each refresh
  } else {
    toWork = new Date().getHours() < cfg.cutoffHour;    // time of day
  }

  var originName = toWork ? cfg.home : cfg.work;
  var destName   = toWork ? cfg.work : cfg.home;

  resolveStop(originName, cfg.apiKey, function (origin) {
    resolveStop(destName, cfg.apiKey, function (dest) {
      if (!origin || !dest) {
        Pebble.sendAppMessage({ 'KEY_TRAIN_DEPARTURE': 0 }, noop, noop);
        return;
      }
      queryTrip(cfg, origin, dest, toWork);
    });
  });
}

// number coercion with a default (for schedule settings)
function num(v, dflt) {
  v = parseInt(v, 10);
  return isNaN(v) ? dflt : v;
}

// pushes the polling-cadence config to the watch (it drives refresh timing)
function pushSchedule() {
  var c = current_settings || {};
  Pebble.sendAppMessage({
    'KEY_PEAK1_START':      num(c.peak1Start, 7),
    'KEY_PEAK1_END':        num(c.peak1End, 9),
    'KEY_PEAK2_START':      num(c.peak2Start, 16),
    'KEY_PEAK2_END':        num(c.peak2End, 19),
    'KEY_PEAK_INTERVAL':    num(c.peakInterval, 15),
    'KEY_OFFPEAK_INTERVAL': num(c.offpeakInterval, 15)
  }, noop, noop);
}

/*  ****************************************** Claude Usage Section **************************************************** */

// Undocumented endpoint that backs Claude Code's /usage. Returns five_hour / seven_day objects
// with a 0-100 `utilization` and an ISO `resets_at`. Phone-side only; the token never reaches the
// watch. The User-Agent must look like claude-code or the request lands in a harshly throttled
// bucket (forbidden XHR header in strict browsers, but pkjs is not a browser - we try anyway).
var CLAUDE_USAGE_URL = 'https://api.anthropic.com/api/oauth/usage';
var CLAUDE_USER_AGENT = 'claude-code/1.0.0';

function getUsageSettings() {
  var c = current_settings || {};
  var s = SECRETS || {};
  return { token: String(c.claudeToken || s.claudeToken || '').trim() };
}

function readCachedUsage() {
  try { return JSON.parse(localStorage.getItem('claude_usage')); } catch (ex) { return null; }
}

// pulls a 0-100 integer out of an endpoint sub-object, defensively
function usageUtil(o) {
  if (!o) return 0;
  var u = parseFloat((o.utilization != null) ? o.utilization : o.used_pct);
  if (isNaN(u)) u = 0;
  return Math.max(0, Math.min(100, Math.round(u)));
}

// pulls an epoch-seconds reset time out of an endpoint sub-object, defensively
function usageReset(o) {
  if (!o) return 0;
  var r = o.resets_at || o.reset_at || o.resetsAt || o.reset;
  var t = r ? Date.parse(r) : NaN;
  return isNaN(t) ? 0 : Math.floor(t / 1000);
}

function sendUsage(d, stale) {
  Pebble.sendAppMessage({
    'KEY_USAGE_5H_PCT':   d.five_pct,
    'KEY_USAGE_5H_RESET': d.five_reset,
    'KEY_USAGE_7D_PCT':   d.seven_pct,
    'KEY_USAGE_7D_RESET': d.seven_reset,
    'KEY_USAGE_STALE':    stale ? 1 : 0
  }, noop, noop);
}

// on any failure, re-send the last good reading flagged stale so the watch keeps showing something
function sendCachedUsage() {
  var d = readCachedUsage();
  if (d) sendUsage(d, 1);
}

function getClaudeUsage() {
  var cfg = getUsageSettings();
  if (!cfg.token) return; // not configured -> watch never shows the bars

  last_usage_attempt = Date.now();

  var xhr = new XMLHttpRequest();
  xhr.onload = function () {
    if (this.status === 200) {
      try {
        var j = JSON.parse(this.responseText);
        var d = {
          five_pct:    usageUtil(j.five_hour),
          five_reset:  usageReset(j.five_hour),
          seven_pct:   usageUtil(j.seven_day),
          seven_reset: usageReset(j.seven_day)
        };
        localStorage.setItem('claude_usage', JSON.stringify(d));
        last_usage_fetch = Date.now();
        sendUsage(d, 0);
        return;
      } catch (ex) { /* fall through to stale */ }
    }
    sendCachedUsage(); // 429 / 401 / parse error -> keep last good, flagged stale
  };
  xhr.onerror = function () { sendCachedUsage(); };
  xhr.open('GET', CLAUDE_USAGE_URL);
  xhr.setRequestHeader('Authorization', 'Bearer ' + cfg.token);
  xhr.setRequestHeader('anthropic-beta', 'oauth-2025-04-20');
  try { xhr.setRequestHeader('User-Agent', CLAUDE_USER_AGENT); } catch (ex) {}
  xhr.send();
}

// adaptive cadence: poll rarely with headroom, more often as quota fills or a 5h reset nears
function usageInterval() {
  var d = readCachedUsage();
  if (!d) return 15 * 60 * 1000; // no data yet -> try reasonably promptly
  var u = Math.max(d.five_pct || 0, d.seven_pct || 0);
  var mins = (u >= 95) ? 5 : (u >= 80) ? 10 : (u >= 50) ? 20 : 30;
  var now = Math.floor(Date.now() / 1000);
  if (d.five_reset && d.five_reset - now > 0 && d.five_reset - now < 15 * 60) mins = Math.min(mins, 5);
  return mins * 60 * 1000;
}

// called on every watch ping; decides whether to actually hit the network this time
function maybeFetchUsage() {
  if (!getUsageSettings().token) return;
  var now = Date.now();
  // hard floor between network attempts so we never hammer the throttled endpoint
  if (now - last_usage_attempt < 5 * 60 * 1000) { sendCachedUsage(); return; }
  if (now - last_usage_fetch >= usageInterval()) {
    getClaudeUsage();
  } else {
    sendCachedUsage();
  }
}

/*  ****************************************** Ready / AppMessage **************************************************** */

Pebble.addEventListener('ready', function () {
  try {
    current_settings = JSON.parse(localStorage.getItem('current_settings'));
  } catch (ex) {
    current_settings = null;
  }

  if (current_settings === null) {
    current_settings = {
      temperatureFormat:     0,
      hoursMinutesSeparator: 0,
      dateFormat:            0,
      bluetoothAlert:        0,
      language:              255,
      textColor:             16777215,
      bgColor:               0
    };
  }

  // tell the watch JS is ready, then push the refresh schedule (so a reinstalled
  // watch gets the cadence without the user reopening config)
  Pebble.sendAppMessage({ 'KEY_JSREADY': 1 }, function () {
    pushSchedule();
    maybeFetchUsage(); // initial usage population on launch
  }, noop);
});

Pebble.addEventListener('appmessage', function () {
  // watch pings every 15 minutes: trains every time, weather only if it has gone stale
  fetchTrain();

  if (Date.now() - last_weather_fetch > 55 * 60 * 1000) {
    getLocation();
  }

  // Claude usage on its own adaptive cadence (no-op until a token is configured)
  maybeFetchUsage();
});

/*  ****************************************** Config Section **************************************************** */

Pebble.addEventListener('showConfiguration', function () {
  Pebble.openURL(clay.generateUrl());
});

Pebble.addEventListener('webviewclosed', function (e) {
  if (!e || !e.response) return;

  var clayData = clay.getSettings(e.response, false);

  function val(key) {
    var v = clayData[key];
    return parseInt((v && typeof v === 'object') ? v.value : v, 10);
  }
  function str(key) {
    var v = clayData[key];
    v = (v && typeof v === 'object') ? v.value : v;
    return (v == null) ? '' : String(v).trim();
  }
  function bool(key) {
    var v = clayData[key];
    v = (v && typeof v === 'object') ? v.value : v;
    return v ? 1 : 0;
  }

  function color(key) {
    var v = clayData[key];
    v = (v && typeof v === 'object') ? v.value : v;
    if (typeof v === 'number') return v;
    return parseInt(String(v), 16);
  }

  // visual settings -> watch
  var msg = {};
  msg.KEY_HOURS_MINUTES_SEPARATOR = val('KEY_HOURS_MINUTES_SEPARATOR');
  msg.KEY_DATE_FORMAT             = val('KEY_DATE_FORMAT');
  msg.KEY_BLUETOOTH_ALERT         = val('KEY_BLUETOOTH_ALERT');
  msg.KEY_LANGUAGE                = val('KEY_LANGUAGE');
  msg.KEY_TEXT_COLOR              = val('KEY_TEXT_COLOR');
  msg.KEY_BG_COLOR                = val('KEY_BG_COLOR');

  // emery usage band display config -> watch
  msg.KEY_USAGE_BAND_MODE    = val('KEY_USAGE_BAND_MODE');
  msg.KEY_USAGE_DISPLAY_MODE = val('KEY_USAGE_DISPLAY_MODE');
  msg.KEY_USAGE_PACE_OFFSET  = val('KEY_USAGE_PACE_OFFSET');
  msg.KEY_USAGE_ABS_WARN     = val('KEY_USAGE_ABS_WARN');
  msg.KEY_USAGE_COLOR_GOOD   = color('KEY_USAGE_COLOR_GOOD');
  msg.KEY_USAGE_COLOR_OVER   = color('KEY_USAGE_COLOR_OVER');
  msg.KEY_USAGE_COLOR_CRIT   = color('KEY_USAGE_COLOR_CRIT');

  var newTempFormat = val('KEY_TEMPERATURE_FORMAT');
  if (!current_settings || current_settings.temperatureFormat !== newTempFormat) {
    msg.KEY_TEMPERATURE_FORMAT = newTempFormat;
  }

  // clear cached stop-id resolutions for the previous station names (so edits re-resolve)
  var prev = current_settings || {};
  if (prev.homeStation) localStorage.removeItem('stopid:' + String(prev.homeStation).toLowerCase());
  if (prev.workStation) localStorage.removeItem('stopid:' + String(prev.workStation).toLowerCase());

  current_settings = {
    temperatureFormat:     newTempFormat,
    hoursMinutesSeparator: val('KEY_HOURS_MINUTES_SEPARATOR'),
    dateFormat:            val('KEY_DATE_FORMAT'),
    bluetoothAlert:        val('KEY_BLUETOOTH_ALERT'),
    language:              val('KEY_LANGUAGE'),
    textColor:             val('KEY_TEXT_COLOR'),
    bgColor:               val('KEY_BG_COLOR'),
    // train settings stay phone-side (used by the JS, never sent to the watch)
    tfnswApiKey:           str('KEY_TFNSW_API_KEY'),
    homeStation:           str('KEY_HOME_STATION_NAME'),
    workStation:           str('KEY_WORK_STATION_NAME'),
    directionMode:         val('KEY_DIRECTION_MODE'),
    cutoffHour:            val('KEY_CUTOFF_HOUR'),
    fastestMarker:         bool('KEY_FASTEST_MARKER'),
    // Claude usage OAuth token (phone-only; never sent to the watch)
    claudeToken:           str('KEY_CLAUDE_TOKEN'),
    // refresh schedule (also pushed to the watch below)
    peak1Start:            val('KEY_PEAK1_START'),
    peak1End:              val('KEY_PEAK1_END'),
    peak2Start:            val('KEY_PEAK2_START'),
    peak2End:              val('KEY_PEAK2_END'),
    peakInterval:          val('KEY_PEAK_INTERVAL'),
    offpeakInterval:       val('KEY_OFFPEAK_INTERVAL')
  };
  localStorage.setItem('current_settings', JSON.stringify(current_settings));

  // send visual settings, then the schedule, then refresh trains immediately
  Pebble.sendAppMessage(msg, function () {
    pushSchedule();
    fetchTrain();
    // a token change should take effect now, bypassing the inter-attempt floor
    last_usage_attempt = 0;
    maybeFetchUsage();
  }, function () {
    fetchTrain();
  });
});
