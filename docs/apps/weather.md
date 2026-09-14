# Weather

What it is doing outside, in as much detail as a 480x800 panel can hold.

Three views of one fetch -- **NOW**, **HOURS**, **WEEK** -- for any number of
saved places, and a plain-text copy of every forecast on the card.

## Where the numbers come from

[Open-Meteo](https://open-meteo.com), and the deciding reason is that it needs
**no API key**. A key means a signup, a secret stored on a card that is also a
USB drive, and a screen for typing 32 hex characters on an on-screen keyboard;
the first time it expires the app is a brick with a nice font.

What it costs to give that up is nothing much: Open-Meteo publishes the output
of national weather services -- DWD ICON, NOAA GFS, ECMWF, Meteo-France --
which is the same modelling most paid APIs resell. It is free for
non-commercial use. And it runs its own geocoder, so choosing a place needs no
second provider with a second set of terms.

Two endpoints, both plain GETs:

| | |
| --- | --- |
| `api.open-meteo.com/v1/forecast` | the numbers |
| `geocoding-api.open-meteo.com/v1/search` | the places |

### TLS, and the file that fixes it when it breaks

The forecast is fetched over verified TLS against the roots this firmware
already bakes for its own services (ISRG Root X1 and X2, GTS Root R1 and R4 --
see `study/StudySyncRoots.h`). That is a bet: those roots cover Let's Encrypt
and Google Trust Services, which is what this host uses today and what
Cloudflare rotates between, but it is not a host this fork controls and an
issuing CA can change under it.

When it does, every symptom looks like "no internet". So the failure screen
names the escape hatch, and the escape hatch is a file copy rather than a
reflash: put a current root bundle at **`/.crosspoint/weather/roots.pem`** and
it wins over the baked one. Same mechanism the Study sync has, for the same
reason.

## The three views

**NOW** is a headline and a table. The headline is the two numbers anyone came
for -- what it is and what it feels like -- and the table is up to eleven rows:
humidity, wind with its compass point, gusts, pressure, cloud cover,
visibility, UV index, dew point, rain now, sunrise/sunset with the length of
the day, and elevation.

Each row carries a **plain-words note** beside the number: `78% Humid`,
`14 km/h SSW Gentle breeze`, `1009 hPa Normal`. That note is the difference
between a readout and a description, and it is the reason the table columns are
measured from their contents rather than split into fixed fractions -- two
fixed splits were tried and each one elided something the screen exists to
carry. See the note in `WeatherScreens.cpp`.

**HOURS** is the next 24 hours, twelve to a page, under a heading line that
names the columns so the numbers under it can stay bare.

**WEEK** is seven days: conditions in words, the chance of rain, the high and
low, and the sun times.

## Absence is not zero

Open-Meteo answers with the variables a given model carries, and any hour can
be JSON `null`. A struct of plain floats renders every one of those as `0` --
and 0% humidity, 0 hPa and 0 km of visibility are all real readings.

So every number in `WeatherCore.h` is a `Value`, which knows whether it was
reported. A field that was not reported **draws no row at all** and prints no
line in the export. `host-tests/weather` and the NOW block in `host-tests/ui`
both assert it, on the parse and on the pixels.

## What is on the card

```
/.crosspoint/weather/places.txt   the saved places
/.crosspoint/weather/<id>.json    the last body fetched for that place
/.crosspoint/weather/roots.pem    optional CA override, see above
/Weather/<slug>-<id>.txt          the forecast as a report a person reads
```

The cache is the **raw response**, not a second serialization of the parsed
model, so it is re-read through the same parser the live fetch uses and the two
cannot drift. It is also why the app opens on the cache rather than on the
radio: tapping a place draws the last forecast instantly and says how old it
is, instead of spending fifteen seconds bringing up Wi-Fi on a device that is
usually asleep and often has no network at all. The one control that fetches
anything is the button on the band.

The export under `/Weather` is one-way and never read back. Reading it would
mean merging two copies with a clock this device cannot trust, and getting that
wrong loses what the service already has.

## Testing it without a network

`WeatherFetch` takes its base URLs from `CROSSPLAY_WEATHER_BASE` and
`CROSSPLAY_GEOCODE_BASE` in the **simulator build only** -- the device build has
no such door. Point them at anything that answers `/v1/forecast` and
`/v1/search` and the real app runs against it:

```bash
CROSSPLAY_WEATHER_BASE=http://127.0.0.1:8765 \
CROSSPLAY_GEOCODE_BASE=http://127.0.0.1:8765 \
  ./scripts_local/sim-shot.sh '<input>' '<shots>'
```

This is the same `urlEnv` door `apps_local/bridge` uses, and it is how every
screenshot of this app was taken.
