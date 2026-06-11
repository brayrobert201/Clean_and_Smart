var Clay = require('@rebble/clay');
var clayConfig = require('./config.json');
var clay = new Clay(clayConfig, null, { autoHandleEvents: false });

// TfNSW API key and station config, baked in at build time (gitignored;
// copy secrets.json.example to secrets.json and fill in your values)
var SECRETS = require('./secrets.json');

var current_settings;
var last_weather_fetch = 0;

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

// fetches next train from the TfNSW Trip Planner API and sends a countdown payload to the watch
function fetchTrain() {
  var s = SECRETS || {};

  if (!s.tfnswApiKey || !s.homeStopId || !s.workStopId) {
    // not configured - tell the watch to fall back to day of week
    Pebble.sendAppMessage({ 'KEY_TRAIN_DEPARTURE': 0 }, function () {}, function () {});
    return;
  }

  var toWork = new Date().getHours() < (s.afternoonCutoffHour || 12);
  var origin = toWork ? s.homeStopId : s.workStopId;
  var dest   = toWork ? s.workStopId : s.homeStopId;

  // trains + metro only (exclude light rail, bus, coach, ferry, school bus); no itdDate/itdTime = depart now
  var url = 'https://api.transport.nsw.gov.au/v1/tp/trip?outputFormat=rapidJSON' +
            '&coordOutputFormat=EPSG:4326&depArrMacro=dep' +
            '&type_origin=any&name_origin=' + encodeURIComponent(origin) +
            '&type_destination=any&name_destination=' + encodeURIComponent(dest) +
            '&calcNumberOfTrips=3&excludedMeans=checkbox' +
            '&exclMOT_4=1&exclMOT_5=1&exclMOT_7=1&exclMOT_9=1&exclMOT_11=1' +
            '&TfNSWTR=true&version=10.2.1.42';

  var xhr = new XMLHttpRequest();
  xhr.onload = function () {
    var dep = 0;
    var platform = '';

    try {
      var journeys = JSON.parse(this.responseText).journeys || [];
      var nowSec = Math.floor(Date.now() / 1000);

      for (var j = 0; j < journeys.length && !dep; j++) {
        var legs = journeys[j].legs || [];

        for (var l = 0; l < legs.length; l++) {
          var product = legs[l].transportation && legs[l].transportation.product;
          var cls = product && product['class'];
          if (cls !== 1 && cls !== 2) continue; // skip walking and other non-rail legs

          var o = legs[l].origin;
          var epoch = Math.floor(Date.parse(o.departureTimeEstimated || o.departureTimePlanned) / 1000);

          if (epoch > nowSec) {
            dep = epoch;
            var m = /Platform (\w+)/.exec(o.disassembledName || o.name || '');
            platform = m ? 'P' + m[1] : '';
          }
          break; // only the first rail leg of each journey matters
        }
      }
    } catch (ex) {
      dep = 0;
    }

    Pebble.sendAppMessage({
      'KEY_TRAIN_DEPARTURE': dep,
      'KEY_TRAIN_PLATFORM': platform,
      'KEY_TRAIN_DIRECTION': toWork ? 0 : 1
    }, function () {}, function () {});
  };
  xhr.onerror = function () {}; // silent: watch keeps counting down old data or falls back
  xhr.open('GET', url);
  xhr.setRequestHeader('Authorization', 'apikey ' + s.tfnswApiKey);
  xhr.send();
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

  Pebble.sendAppMessage({ 'KEY_JSREADY': 1 }, function () {}, function () {});
});

Pebble.addEventListener('appmessage', function () {
  // watch pings every 15 minutes: trains every time, weather only if it has gone stale
  fetchTrain();

  if (Date.now() - last_weather_fetch > 55 * 60 * 1000) {
    getLocation();
  }
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

  var msg = {};
  msg.KEY_HOURS_MINUTES_SEPARATOR = val('KEY_HOURS_MINUTES_SEPARATOR');
  msg.KEY_DATE_FORMAT             = val('KEY_DATE_FORMAT');
  msg.KEY_BLUETOOTH_ALERT         = val('KEY_BLUETOOTH_ALERT');
  msg.KEY_LANGUAGE                = val('KEY_LANGUAGE');
  msg.KEY_TEXT_COLOR              = val('KEY_TEXT_COLOR');
  msg.KEY_BG_COLOR                = val('KEY_BG_COLOR');

  var newTempFormat = val('KEY_TEMPERATURE_FORMAT');
  if (!current_settings || current_settings.temperatureFormat !== newTempFormat) {
    msg.KEY_TEMPERATURE_FORMAT = newTempFormat;
  }

  current_settings = {
    temperatureFormat:     newTempFormat,
    hoursMinutesSeparator: val('KEY_HOURS_MINUTES_SEPARATOR'),
    dateFormat:            val('KEY_DATE_FORMAT'),
    bluetoothAlert:        val('KEY_BLUETOOTH_ALERT'),
    language:              val('KEY_LANGUAGE'),
    textColor:             val('KEY_TEXT_COLOR'),
    bgColor:               val('KEY_BG_COLOR')
  };
  localStorage.setItem('current_settings', JSON.stringify(current_settings));

  Pebble.sendAppMessage(msg, function () {}, function () {});
});
