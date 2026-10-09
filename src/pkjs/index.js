// Fetches the next 24h of weather from Open-Meteo (no API key needed)
// and ships it to the watch as compact byte arrays.

var Clay = require('pebble-clay');
var clayConfig = require('./config');
var clay = new Clay(clayConfig);

var HOURS = 24;
var MANUAL_LOCATION_KEY = 'manualLocation';

// Pinned location from the settings page. Returns null when unset/invalid so
// fetchWeather falls back to the phone's geolocation.
function getManualLocation() {
  var raw = null;
  try {
    raw = localStorage.getItem(MANUAL_LOCATION_KEY);
  } catch (e) {
    return null;
  }
  if (!raw) return null;
  try {
    var c = JSON.parse(raw);
    if (typeof c.lat !== 'number' || typeof c.lon !== 'number') return null;
    if (c.lat < -90 || c.lat > 90 || c.lon < -180 || c.lon > 180) return null;
    if (!isFinite(c.lat) || !isFinite(c.lon)) return null;
    return c;
  } catch (e) {
    return null;
  }
}

function fetchWeather() {
  var manual = getManualLocation();
  if (manual) {
    getForecast(manual.lat, manual.lon);
    return;
  }

  navigator.geolocation.getCurrentPosition(
    function (pos) {
      getForecast(pos.coords.latitude, pos.coords.longitude);
    },
    function (err) {
      console.log('geolocation error: ' + err.message);
      var cached = localStorage.getItem('lastCoords');
      if (cached) {
        var c = JSON.parse(cached);
        getForecast(c.lat, c.lon);
      }
    },
    { timeout: 15000, maximumAge: 30 * 60 * 1000 }
  );
}

function getForecast(lat, lon) {
  localStorage.setItem('lastCoords', JSON.stringify({ lat: lat, lon: lon }));

  var url =
    'https://api.open-meteo.com/v1/forecast' +
    '?latitude=' + lat.toFixed(4) +
    '&longitude=' + lon.toFixed(4) +
    '&hourly=temperature_2m,precipitation' +
    '&daily=sunrise,sunset' +
    '&timeformat=unixtime' +
    '&timezone=auto' +
    '&forecast_days=3';

  var xhr = new XMLHttpRequest();
  xhr.onload = function () {
    if (xhr.status !== 200) {
      console.log('open-meteo HTTP ' + xhr.status);
      return;
    }
    try {
      sendPayload(buildPayload(JSON.parse(xhr.responseText)));
    } catch (e) {
      console.log('parse error: ' + e);
    }
  };
  xhr.onerror = function () { console.log('open-meteo request failed'); };
  xhr.open('GET', url);
  xhr.send();
}

function buildPayload(json) {
  var now = Math.floor(Date.now() / 1000);
  var times = json.hourly.time;
  var temps = json.hourly.temperature_2m;
  var rains = json.hourly.precipitation;

  // First sample: the hour containing "now"
  var start = 0;
  while (start < times.length - 1 && times[start + 1] <= now) start++;

  var tempBytes = [];
  var rainBytes = [];
  var tMin = 999, tMax = -999, rainTotal = 0;
  for (var i = 0; i < HOURS; i++) {
    var t = temps[start + i];
    var r = rains[start + i];
    if (t === null || t === undefined) t = temps[start];
    if (r === null || r === undefined) r = 0;
    if (t < tMin) tMin = t;
    if (t > tMax) tMax = t;
    rainTotal += r;
    // temp encoded with +100 offset into a byte; rain as mm*10 capped
    tempBytes.push(Math.max(0, Math.min(255, Math.round(t) + 100)));
    rainBytes.push(Math.max(0, Math.min(255, Math.round(r * 10))));
  }

  // Tonight: first (sunset, next sunrise) pair whose sunrise is still ahead
  var sunsets = json.daily.sunset;
  var sunrises = json.daily.sunrise;
  var sunset = 0, sunrise = 0;
  for (var d = 0; d < sunsets.length - 1; d++) {
    if (sunrises[d + 1] > now) {
      sunset = sunsets[d];
      sunrise = sunrises[d + 1];
      break;
    }
  }

  return {
    PAYLOAD_VERSION: 1,
    BASE_TIMESTAMP: times[start],
    TEMP: tempBytes,
    RAIN: rainBytes,
    SUNRISE: sunrise,
    SUNSET: sunset,
    TEMP_MIN: Math.round(tMin),
    TEMP_MAX: Math.round(tMax),
    RAIN_TOTAL: Math.min(65535, Math.round(rainTotal * 10))
  };
}

function sendPayload(payload) {
  Pebble.sendAppMessage(
    payload,
    function () { console.log('weather sent'); },
    function (e) { console.log('send failed'); }
  );
}

Pebble.addEventListener('ready', function () {
  console.log('pkjs ready');
  fetchWeather();
});

// Any message from the watch is a refresh request
Pebble.addEventListener('appmessage', function () {
  fetchWeather();
});

// Settings page closed: apply the pinned location (both fields blank clears
// it). Runs alongside Clay's own listener, which sends the theme to the watch.
Pebble.addEventListener('webviewclosed', function (e) {
  if (!e || !e.response) return;

  var settings;
  try {
    var raw = e.response.match(/^\{/) ? e.response : decodeURIComponent(e.response);
    settings = JSON.parse(raw);
  } catch (err) {
    console.log('config parse error: ' + err);
    return;
  }

  function field(key) {
    var item = settings[key];
    if (!item || typeof item !== 'object' || item.value === undefined) return '';
    return String(item.value).trim();
  }

  var lat = field('LATITUDE');
  var lon = field('LONGITUDE');
  var latN = Number(lat);
  var lonN = Number(lon);

  if (lat === '' && lon === '') {
    localStorage.removeItem(MANUAL_LOCATION_KEY);
    console.log('manual location cleared, using geolocation');
  } else if (
    lat !== '' && lon !== '' &&
    isFinite(latN) && isFinite(lonN) &&
    latN >= -90 && latN <= 90 && lonN >= -180 && lonN <= 180
  ) {
    localStorage.setItem(
      MANUAL_LOCATION_KEY,
      JSON.stringify({ lat: latN, lon: lonN })
    );
    console.log('manual location set to ' + latN + ', ' + lonN);
  } else {
    console.log(
      'invalid location "' + lat + '","' + lon + '" ignored, keeping previous'
    );
    return;
  }

  fetchWeather();
});
