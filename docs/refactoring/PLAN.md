# VdMot Controller – Neuaufbau der Firmware (v2)

Stand: 2026-11-13 · Status: Planung

Dieses Dokument hält fest, **warum** die Firmware neu aufgebaut wird, **welche
Entscheidungen** getroffen wurden und **in welcher Reihenfolge** gearbeitet wird.
Details zum UART-Protokoll stehen in
[`../protocol/stm32-esp32-link.md`](../protocol/stm32-esp32-link.md).

---

## 1. Ausgangslage

Die Hardware (C2-Revision, STM32F411 „BlackPill“ WeAct V3.0, WT32-ETH01) ist gut.
Die vorhandene Firmware (v1.4.x) hat strukturelle Probleme, die sich nicht durch
Einzelfixes beheben lassen. Die wichtigsten (Auszug aus dem Code-Review):

| Bereich | Problem | Auswirkung |
|---|---|---|
| STM32 Motor | Kommando (`valvenr`, `command`, `poschangecmd`) wird ohne Schutz zwischen Main-Loop und 10-ms-ISR übergeben | falsches Ventil / falscher Weg, Drift |
| STM32 Motor | Position wird nur nominell aufaddiert, gezählte Impulse nicht zurückgeführt, `scaler` ganzzahlig, `deadzone` unbenutzt und `unsigned` (Unterlauf) | Positionsdrift |
| STM32 Motor | Unterstrom-Zähler kumulativ statt konsekutiv | Ventil wird als „open circuit“ markiert und verschwindet |
| STM32 Motor | `delay(100)` im ISR-Pfad (MUX-Umschaltung) | Strommessung steht, UART-Bytes gehen verloren |
| STM32 | Kalibrierung/Position nicht persistent, EEPROM ohne Version/CRC, kein Watchdog, kein BOR | nach jedem Boot Neulernen aller Ventile; Hänger ohne Recovery |
| STM32 1-Wire | Bus-Index und DS18-Index vermischt, blockierende ROM-Suchen | Temperaturen am falschen Ventil |
| UART-Protokoll | Text, kein Framing/CRC/Sequenznummer, Pufferüberläufe auf beiden Seiten, nur erste Zeile im Puffer wird verarbeitet, `gvlst`-Parser zählt Index doppelt | verlorene/falsch zugeordnete Antworten |
| ESP32 | AsyncWebServer-Callbacks schreiben ohne Locks in globalen Zustand, `yieldTask()` startet den Scheduler im falschen Thread, alle Watchdogs aus | sporadische Fehler, Hänger |
| ESP32 Config | NVS-Struct-Blobs ohne Version/CRC | Layoutänderung zerstört Konfiguration |
| ESP32 MQTT/HA | kein LWT/Availability, Client-ID = Stationsname, Ist-Position nie publiziert, Discovery-Entitäten ohne Daten bzw. ohne Subscription, Discovery blockiert ~20 s | HA-Anbindung unzuverlässig |
| ESP32 Sicherheit | kein Endpoint wirklich geschützt (Reboot, OTA, Config), SMTP-Passwort im Klartext an die UI | – |
| PI-Regler | Δt aus `time()` (SNTP-Sprung), Startposition = Temperatur statt % | Integrator-Schläge |

Ein schrittweises Refactoring würde fast jede Zeile anfassen. Deshalb:
**Neuaufbau nach dem Strangler-Prinzip** – Hardwarewissen und brauchbare
Algorithmen (Endstop-Erkennung über Strom, Back-EMF-Zählung, PI-Grundstruktur,
STM32-ROM-Bootloader-Update) werden übernommen, die Struktur nicht.

---

## 2. Getroffene Entscheidungen

| # | Entscheidung | Begründung |
|---|---|---|
| D1 | Neuaufbau im **eigenen Fork** (`apoapoapo/VdMot_Controller`), neue Firmware in neuen Verzeichnissen; der alte Code bleibt bis zur Ablösung unverändert liegen | Historie und Urheberschaft bleiben sichtbar, kein Big-Bang |
| D2 | Lizenz bleibt **GPLv3**; vorhandene Copyright-Hinweise bleiben in übernommenen Teilen erhalten, neue Dateien tragen eigenen Header + GPLv3-Hinweis | Pflicht aus der GPL bei abgeleiteten Werken |
| D3 | Eigene Versionierung ab **2.0.0** (SemVer), eigene GitHub-Releases im Fork; README verweist auf Upstream (Lenti84 / SurfGargano) | klare Abgrenzung zu den 1.x-Releases von Upstream |
| D4 | STM32 = reiner Echtzeit-Aktor, ESP32 = Logik/Konfiguration/Integration | eine Quelle der Wahrheit pro Thema |
| D5 | Neues **binäres UART-Protokoll** (COBS + CRC16, Sequenznummern, Request/Response + Events), gemeinsamer Header für beide Seiten | siehe Protokoll-Spezifikation |
| D6 | STM32 und ESP32 werden immer **zusammen** aktualisiert (ESP flasht STM über ROM-Bootloader); keine Kompatibilität zum 1.x-Textprotokoll | spart Übergangscode |
| D7 | Integration zuerst **MQTT / Home Assistant**; KNX wird später als eigener Adapter besprochen | HA ist fast vorhanden, schnellster Nutzen |
| D8 | Zielhardware zunächst **C2 + STM32F411**; F401 und C1 bleiben über Build-Environments möglich, werden aber nicht primär getestet | vorhandene Hardware |

Offen / später:

- **KNX**: Weg noch offen (KNX-IP über ESP32 ohne ETS-Produktdatenbank vs.
  TP-UART-Transceiver mit ETS-Applikation, z. B. im Stil von OpenKNX).
  Die frühere Bastellösung (TP-Wandler an der STM-Debug-UART) wird nicht
  weiterverfolgt.
- **Zusatzkondensator 3,3 µF / 50 V am ESP-Connector** (vorhanden auf dem
  eigenen Board, Zweck unbekannt). Wahrscheinlich entweder Stützkondensator
  für die 3,3-V-/5-V-Versorgung des WT32-ETH01 (Stromspitzen von WLAN/PHY →
  Brown-out-Resets) oder RC-Verzögerung am EN-Pin (bekanntes Bootproblem des
  WT32-ETH01, weil GPIO0 vom LAN8720-Takt belegt ist). **Klären, an welchen
  Pins er hängt**, und im Hardware-Dokument festhalten.

---

## 3. Zielstruktur des Repos

```
common/
  protocol/            gemeinsamer Protokoll-Code (C99, ohne Arduino-Abhängigkeit)
    vdm_link.h         Nachrichten-IDs, Strukturen, Fehlercodes
    vdm_cobs.[ch]      COBS-Encode/Decode
    vdm_crc16.[ch]     CRC-16/CCITT-FALSE
    vdm_codec.[ch]     (De)Serialisierung der Nachrichten, Little-Endian, feldweise
firmware/
  stm32/               neue STM32-Firmware (PlatformIO)
    src/hal/           Pins, Timer, ADC, Impulszähler, EEPROM, 1-Wire (einzige Stelle mit Arduino/HAL)
    src/motor/         MotorChannel (Zustandsautomat), ValveManager, Kalibrierung – hardwarefrei
    src/sensors/       1-Wire-Scheduler (nicht blockierend), Sensorliste nach ROM-ID
    src/link/          Protokoll-Server
    src/storage/       EEPROM-Records mit Version + CRC
    test/              Unity-Tests (env:native)
  esp32/               neue ESP32-Firmware (PlatformIO)
    src/core/          Datenmodell (Mutex-geschützt), Event-Bus
    src/link/          Protokoll-Client, STM32-Update über ROM-Bootloader
    src/config/        versionierte Konfiguration (JSON in LittleFS) + Migrationen
    src/control/       PI-Regler (monotone Zeitbasis)
    src/adapters/      web (REST + WebSocket), mqtt_ha, später knx
    test/
  webui/               Web-Oberfläche mit reproduzierbarem Build (ohne mktfs.exe)
docs/
  protocol/            Protokoll-Spezifikation
  refactoring/         dieses Dokument
software_stm32/, software_esp32/   Altbestand (v1.4.x), wird nach Ablösung entfernt
```

Release-Binaries (`releases/`, `*.bin`) gehören nicht ins Repo, sondern an
GitHub-Releases (CI baut sie).

---

## 4. Phasen

### Phase 0 – Fundament
- Verzeichnisstruktur, `.gitignore`, `.editorconfig`, clang-format.
- Protokoll-Spezifikation (dieses Paket) → Review → `common/protocol` implementieren.
- Host-Tests (PlatformIO `env:native` + Unity) für COBS, CRC, Codec.
- GitHub Actions: Builds für STM32 (F411/F401) und ESP32, Host-Tests.

### Phase 1 – STM32-Firmware (kritischster Teil)
- HAL-Schicht; definierte Pin-Zustände direkt nach Reset (Motoren aus, PSU aus).
- `MotorChannel`-Zustandsautomat mit `tick()`, ISRs liefern nur Rohdaten
  (Impulszähler, Stromwerte) über atomare/volatile Übergabe.
- Positionsführung über gezählte Impulse, getrennte Faktoren für Auf/Zu,
  Totzone berücksichtigt, Fixed-Point statt Ganzzahl-Scaler.
- Robuste Endstop-/Unterstrom-Erkennung (konsekutive Zähler, Plausibilitätsgrenzen).
- Kalibrierdaten + letzte Position persistent (EEPROM-Record, Version + CRC,
  schreibschonend). Nach Boot: Referenzfahrt statt komplettem Neulernen.
- Nicht blockierender 1-Wire-Scheduler, Sensoren nur über ROM-ID.
- IWDG, BOR-Level, Reset-Ursache, Fehlerzähler.
- Protokoll-Server.
- Tests auf dem Host für Zustandsautomat, Kalibrierauswertung, Storage.
- **Hardwaretest am eigenen Board** (Daniel).

### Phase 2 – ESP32-Kern
- FreeRTOS-Tasks mit klaren Zuständigkeiten statt TaskManagerIO/`yieldTask`.
- Datenmodell + Event-Bus, Protokoll-Client mit Retry/Timeout/Resync.
- Versionierte Konfiguration mit Migration.
- STM32-Update über ROM-Bootloader (BOOT0/NRST von ESP aus, wie bisher).
- REST-API + WebSocket, echte Authentifizierung.
- PI-Regler mit monotoner Zeitbasis, sauberem Anti-Windup, Staleness-Check.
- Watchdogs an.

### Phase 3 – MQTT / Home Assistant
- LWT + Birth, `availability_topic`, Client-ID aus MAC.
- `unique_id` aus MAC + Kanal (stabil bei Umbenennung).
- Discovery bei Verbindung und bei `homeassistant/status = online`, nicht blockierend.
- Pro Ventil: `valve` (echte Position), `climate` (wenn PI aktiv), `sensor`
  (Strom, Temperaturen, Zähler als `total_increasing`), `button` (Kalibrieren),
  `binary_sensor` (Fehler), `number` mit min/max/step.
- JSON-Payloads + `value_template`; keine Format-Varianten mehr.

### Phase 4 – Web-UI
- Neu, klein, gegen die REST/WebSocket-API; Build-Schritt in PlatformIO integriert.

### Phase 5 – KNX (nach gesonderter Besprechung)
- Als Adapter am Datenmodell (DPT 5.001 Stellgröße, DPT 9.001 Temperatur,
  Status/Fehler).

---

## 5. Test- und Abnahmekriterien (Auszug)

- 72 h Dauerlauf mit zufälligen Sollwerten: Positionsfehler nach Referenzfahrt < 2 %.
- Neustart des STM32: kein Neulernen, nur Referenzfahrt; Positionen korrekt.
- Abziehen/Anstecken eines Ventils: Erkennung, Fehlermeldung, Wiederaufnahme.
- UART gestört (Bytes verworfen/verfälscht): keine falsche Aktion, Resync < 1 s.
- ESP32-Neustart: Ventile behalten Position, HA zeigt kurz „nicht verfügbar“.
- Home Assistant: alle Entitäten haben Daten, alle Befehle wirken.
