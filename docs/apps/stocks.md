# Stocks

A watchlist from **NASDAQ, NYSE, NSE and BSE**, what the holdings in it are
worth, and each stock's day or last ten trading days as a chart.

Apps > STOCKS. The list opens on the prices the card already has; the refresh
button in the band fetches new ones.

## The watchlist

One file, `/Stocks/watchlist.txt`, in a visible folder because it is meant to
be edited on a computer:

```
# SYMBOL  EXCHANGE  [SHARES]
AAPL      NASDAQ  10
MSFT      NASDAQ
RELIANCE  NSE     25
INFY      NSE
```

- `EXCHANGE` is `NASDAQ`, `NYSE`, `NSE` or `BSE`, in any case.
- `SHARES` is how many you hold, fractions allowed. Leave it out to just watch.
- Anything after `#` is ignored. At most 24 stocks, because each is one request.

A line that cannot be read is **reported, never guessed at**: the list shows
`watchlist.txt line 8: Exchange must be NASDAQ, NYSE, NSE or BSE: LSE` above
the toggle and skips that line. A holding silently read as zero shares would be
a total that is quietly wrong.

The first time the app opens on a card with no watchlist it writes a sample
with the format explained in its own comments.

**On the device**, EDIT turns the list into the watchlist editor. ADD A STOCK
takes one line in the same format (`TCS NSE 5`) and fetches just that stock,
which is also the check that the symbol exists. Tapping a stock offers CHANGE
SHARES and REMOVE. Both edit the file line by line, so comments and ordering
written on a computer survive.

## The toggle: TODAY or 10 DAYS

One segmented bar, in the same pixels on the list and on a stock.

| | fetches | the chart | the change is measured from |
| --- | --- | --- | --- |
| **TODAY** | the session so far, five-minute steps | a line across the whole session's width, so a morning stops part way | yesterday's close |
| **10 DAYS** | a month of daily closes, cut to the last ten | a line with a dot on each close, and the ten closes in a table under it | the close the day before the ten |

Flipping it changes every number on screen at once, the holdings total
included, so no screen ever shows one span's price beside another span's
change. A refresh fetches only the span on screen: one request per stock, not
two. Switching to a span the card has never fetched fetches it.

The ten-day screen was chosen from three rendered arrangements: a line over a
table of closes, candlesticks over four summary numbers, and bars of each
day's move over the same table. The line won because the question the view
exists for is "what did it close at", and only the table answers that
without arithmetic.

## What the holdings are worth

A line per currency at the top of the list: dollars for NASDAQ and NYSE,
rupees for NSE and BSE. They are **never added together**. A combined total
needs an exchange rate the app would have to fetch, date and trust, and two
true numbers are better than one derived one. Rupee amounts use the Indian
grouping (`Rs 1,37,733.77`); the fonts have no rupee sign.

A stock whose price has never arrived is left out of the total rather than
counted as zero, and its row says so.

## Every price has a time

Free prices are delayed (up to fifteen minutes), markets close, and a reader
can be opened offline on a week-old card. So every stock's screen names the
exchange time its price was true at, `Tue 29, 3:59 PM exchange time`, and adds
`saved` until this visit has refreshed it.

## Where the numbers come from

Yahoo Finance's chart endpoint, the only free source with no key that covers
both New York and Mumbai:

```
https://query1.finance.yahoo.com/v8/finance/chart/<SYMBOL>?range=1d&interval=5m
https://query1.finance.yahoo.com/v8/finance/chart/<SYMBOL>?range=1mo&interval=1d
```

NSE symbols gain `.NS` and BSE `.BO`; `BRK.B` is asked for as `BRK-B`. The
request carries a browser's User-Agent, because Yahoo answers anything else
with 429.

It is **unofficial**. The day it changes shape, the app says so in words
("Yahoo sent a chart with no price in it") rather than drawing zeros, and a
symbol Yahoo does not know comes back as Yahoo's own "No data found".

### TLS

Verified against the DigiCert roots (`StocksRoots.h`: Global Root CA, G2, G3
and High Assurance EV), which is who has issued Yahoo's certificates. If that
changes, put a current bundle at **`/.crosspoint/stocks/roots.pem`** and it
wins over the baked one; the failure screen names the file.

## On the card

| | |
| --- | --- |
| `/Stocks/watchlist.txt` | the watchlist |
| `/.crosspoint/stocks/<SYM>-today.txt`, `-days.txt` | the last series fetched, plain text |
| `/.crosspoint/stocks/span.txt` | which side of the toggle was last used |
| `/.crosspoint/stocks/roots.pem` | optional CA override |

## Buttons

| | List | A stock |
| --- | --- | --- |
| Back | leave (or leave the editor) | back to the list |
| Up / Down | previous / next page | previous / next stock |

A refresh fetches one stock per loop pass and repaints `FETCHING 3 OF 8`
between them, so Back stops it after the stock in flight and keeps what has
arrived. A stock that fails keeps its old price; only a refresh where nothing
at all arrived is an error screen.

## Not yet

A sleep-screen version of the watchlist, drawn when the reader goes to sleep.

## Code

`src/apps_local/stocks/`: `StocksCore` (freestanding: the watchlist format,
moves, worth, formatting, the cache format; `host-tests/stocks`),
`StocksFetch` (the one file that knows Yahoo exists), `StocksStore` (the card),
`StocksScreens` (freestanding builders; `host-tests/ui`), `StocksActivity`.

In the simulator, `CROSSPLAY_STOCKS_BASE=http://127.0.0.1:<port>` points the
fetch at a local server speaking the same JSON.
