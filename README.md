# IoT-Based Glucose Drip Monitoring System

Real-time IoT system that monitors glucose drip flow rate, detects anomalies
(too slow / too fast / stopped), and prevents backflow or overflow — with an
automated alert system that notifies medical staff and reduces manual
supervision.

## Repo structure

```
glucose-drip-monitor/
├── firmware/
│   └── glucose_drip_monitor.ino   # ESP32 firmware: sensors + alert/notification logic
└── prototype/
    └── index.html                 # Live browser prototype (simulated sensors + alerts)
```

## Live Prototype

Open - <a href="http://127.0.0.1:5500/index.html">`prototype/index.html`<a/> 
in any browser (no install needed). It simulates the IR drop-counter and ultrasonic level sensor feeding into the same alert
logic used on the real hardware, and shows:

- Live flow rate, chamber level, and time-since-last-drop readings
- A status banner + audible alarm when an anomaly is detected
- A "Medical Staff Notification Feed" showing each alert as it would be sent
- Scenario buttons to simulate: normal flow, slow flow, fast flow, blocked
  line, overflow risk, and backflow risk

## Firmware (ESP32)

  `firmware/glucose_drip_monitor.ino` is the real hardware code:

- IR drop-counter sensor → measures drops/min via interrupt
- Ultrasonic sensor → measures chamber/bag liquid level
- Local alarm (buzzer + LED) triggers instantly on any anomaly
- Push notification to medical staff via Telegram Bot API (swap
  `sendTelegramAlert()` for any notification service — SMS gateway,
  email, hospital app backend, etc.)

### Setup
1. Install the ESP32 board package in Arduino IDE.
2. Wire sensors per the pin comments at the top of the `.ino` file.
3. Create a Telegram bot via `@BotFather`, get your chat ID via `@myidbot`.
4. Fill in `WIFI_SSID`, `WIFI_PASSWORD`, `TELEGRAM_BOT_TOKEN`, `TELEGRAM_CHAT_ID`.
5. Flash to the ESP32 and open Serial Monitor at 115200 baud to confirm.

## Alert logic (shared between firmware and prototype)

| Condition | Trigger | Severity |
|---|---|---|
| Flow too slow | < 10 drops/min | Warning |
| Flow too fast | > 60 drops/min | Warning |
| Flow stopped | No drop for 15s (8s in demo) | Critical |
| Overflow risk | Liquid level above threshold | Critical |
| Backflow risk | No flow + bag near empty | Critical |

Thresholds are configurable constants at the top of each file.
