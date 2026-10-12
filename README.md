# 📬 iMailBox

🇫🇷 [Version française](README_FR.md)

A self-powered connected mailbox: a motion sensor detects incoming mail and sends a Discord notification, along with the battery status.

The circuit draws almost nothing at rest (about 20 µA): the ESP8266 is **fully powered off** and only turns on when the sensor detects something. A LiPo battery topped up by a small solar panel is enough.

## How it works

1. At rest, only the PIR sensor (AM312) is powered.
2. The PIR detects motion: an NPN transistor drives a P-channel MOSFET, which powers the D1 Mini Pro.
3. The ESP immediately sets **D6 HIGH** to keep itself powered (self-latching), because the PIR output only stays HIGH for a few seconds.
4. It measures the battery, connects to Wi-Fi, gets the time (NTP) and sends the Discord notification.
5. It sets **D6 LOW**: the power is cut.

At most one notification is sent every 5 minutes (anti-spam). The time of the last notification and the statistics are stored in flash (LittleFS), so they survive power cuts.

### Example notification

```
📬 Boîte aux lettres
Du courrier vient d'arriver !

🔋 Batterie    ████████░░ 78 %
               3,98 V (+12 mV)
📶 Wi-Fi       -67 dBm, connexion 2340 ms
📊 Activité    42 réveils, 38 notifs, 1 échec
🗓️ En service  27 jours
```

The messages are in French; edit `sendDiscordNotification()` in [src/main.cpp](src/main.cpp) to translate them. The Discord embed color follows the battery: green above 50 %, orange from 21 to 50 %, red at 20 % and below (with a warning message).

## Hardware

| Ref | Part | Purpose |
|---|---|---|
| | WEMOS D1 Mini Pro (ESP8266) + 2.4 GHz u.FL antenna | Microcontroller and Wi-Fi |
| | AM312 PIR sensor | Mail detection |
| | LiPo 3.7 V 3000 mAh | Battery |
| | TP4056 **with protection** (DW01 + 8205A) | Battery charging and protection |
| | 5 V solar panel, 1 to 2 W | Charging |
| Q1 | NDP6020P (P-channel MOSFET, TO-220) | Power switch |
| Q2 | 2N2222 (NPN, TO-92) | Drives Q1 |
| D1, D2 | 1N4148 | "OR" between the PIR and D6 |
| R1, R3, R5 | 100 kΩ | Gate pull-up, base pull-down, battery sensing |
| R2, R4 | 10 kΩ | Base current, D5 protection |
| C1 | 1000 µF 10 V electrolytic | Absorbs Wi-Fi current spikes |

## Wiring

![Schematic](docs/schematic.svg)

Text version:

```
Solar 5V ────► TP4056 IN+/IN−
LiPo ────────► TP4056 B+/B−

TP4056 OUT+ (VBAT) ──┬──► AM312 VCC
                     ├──► R1 100k ──► Q1 gate
                     └──► Q1 source

Q1 drain (VSYS) ──┬──► D1 Mini 5V
                  ├──► C1 + (C1 − to GND)
                  └──► R5 100k ──► A0

Q1 gate ──► Q2 collector,  Q2 emitter ──► GND
Q2 base ◄── R2 10k ◄──┬── D1 ◄── AM312 OUT
                      └── D2 ◄── D6 (GPIO12)
Q2 base ── R3 100k ──► GND

AM312 OUT ── R4 10k ──► D5 (GPIO14)
All grounds tied together (TP4056 OUT−, AM312, D1 Mini, C1)
```

| D1 Mini pin | GPIO | Use |
|---|---|---|
| D5 | 14 | PIR reading |
| D6 | 12 | Power self-latch |
| A0 | | Battery voltage |
| 5V | | Power input (VSYS) |

### Watch out for

- **External antenna**: on the D1 Mini Pro, the 0 Ω resistor next to the ceramic antenna must be moved to enable the u.FL connector.
- **Pinouts**: NDP6020P seen from the front = `G D S` (tab connected to Drain). 2N2222 in TO-92, flat side facing you = usually `E B C`, but some parts (P2N2222A) are reversed. Check the datasheet.
- **Polarity**: the diode stripes face R2, and C1's "−" leg goes to GND.
- **Battery**: solder one wire at a time and insulate it right away to avoid a short circuit.

## Setup

### Requirements

- VS Code with the **PlatformIO** extension
- Optional: the **Wokwi Simulator** extension to test without hardware

### Configuration

Copy the example file and fill it in:

```sh
cp include/secrets.h.example include/secrets.h
```

```cpp
#define WIFI_SSID "MyWiFi"
#define WIFI_PASSWORD "MyPassword"
#define DISCORD_WEBHOOK_URL "https://discord.com/api/webhooks/XXXX/YYYY"
```

`include/secrets.h` is ignored by git. Create the webhook in Discord: *Channel settings → Integrations → Webhooks*.

### Build and upload

```sh
pio run -e d1_mini_pro -t upload
pio device monitor
```

While developing over USB, the power is never cut: the program then runs `loop()` and reacts to every new detection.

### Settings

In [src/main.cpp](src/main.cpp):

| Constant | Default | Purpose |
|---|---|---|
| `COOLDOWN_S` | 300 s | Minimum delay between two notifications |
| `LOW_BATTERY_PERCENT` | 20 % | Low battery warning threshold |
| `WIFI_TIMEOUT_MS` | 15 s | Wi-Fi connection timeout |
| `ADC_FULL_SCALE_V` | 4.2 V | Battery measurement calibration |

**Battery calibration**: measure the battery voltage with a multimeter, compare it with the value shown on Discord, then scale `ADC_FULL_SCALE_V` accordingly.

## Simulation (Wokwi)

Wokwi does not simulate the ESP8266: the `wokwi` environment builds the same code for an **ESP32** (`#ifdef ESP32` blocks handle the differences).

```sh
pio run -e wokwi
```

Then, in VS Code: *Wokwi: Start Simulator*.

| Diagram part | Simulates |
|---|---|
| PIR on GPIO14 | The sensor (click it to trigger) |
| "HOLD" LED on GPIO13 | The power self-latch |
| Potentiometer on GPIO34 | The battery voltage (0 to 4.2 V) |

Limitations:
- Wokwi only simulates logic: the power cut, the MOSFET, the diodes and the charging are not simulated. For the power circuit, use [Falstad CircuitJS](https://www.falstad.com/circuit/) or LTspice.
- The simulated flash is wiped on every restart, so the statistics start from 0.
- The simulated network can drop some HTTPS connections. The code retries the request 3 times.

The [scripts/merge_bin.py](scripts/merge_bin.py) script builds a full flash image (`firmware_merged.bin`); without it, LittleFS is not available in Wokwi.

## Project structure

```
├── docs/
│   └── schematic.svg       # wiring diagram
├── include/
│   ├── secrets.h.example   # configuration template
│   └── secrets.h           # credentials (ignored by git)
├── scripts/
│   └── merge_bin.py        # full flash image for Wokwi
├── src/
│   └── main.cpp            # firmware
├── diagram.json            # Wokwi diagram
├── wokwi.toml              # Wokwi configuration
└── platformio.ini          # d1_mini_pro and wokwi environments
```

## Stored statistics

The `/stats` file in flash holds a fixed-size structure (about 40 bytes), rewritten on every wake-up: it never grows.

| Field | Content |
|---|---|
| `lastSent` | Time of the last successful notification (anti-spam) |
| `wakeups` | Number of wake-ups |
| `sent` | Notifications sent |
| `failures` | Wi-Fi or HTTP failures |
| `firstBoot` | Commissioning date |
| `lastVoltage` | Voltage at the previous wake-up |

To reset everything: `pio run -e d1_mini_pro -t erase`, then upload the firmware again (`erase` also wipes the firmware).
