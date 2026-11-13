# VdMot Link – Protokoll zwischen ESP32 und STM32

Version: **1** (Entwurf) · Stand: 2026-11-13

Dieses Dokument beschreibt das Protokoll auf der UART-Verbindung zwischen
ESP32 (WT32-ETH01) und STM32. Es ersetzt das Textprotokoll der Firmware 1.x
(`stgtp`, `gvlvd`, …). Eine Kompatibilität zu 1.x besteht nicht.

---

## 1. Rollen und Grundsätze

- **ESP32 = Master.** Nur der ESP32 sendet Requests. Der STM32 beantwortet jeden
  Request mit genau einer Response.
- **STM32 meldet Änderungen selbst.** Zustandsänderungen schickt der STM32 als
  **Event**, ohne Aufforderung (z. B. „Ventil 3 hat Ziel erreicht“, „neue
  Temperaturwerte“). Polling dient nur noch als Sicherheitsnetz.
- **Der STM32 entscheidet nichts.** Er hält keine Zuordnung Sensor→Ventil und
  keine Namen. Er kennt Kanäle 0…11 und 1-Wire-Sensoren über ihre 64-Bit-ROM-ID.
- **Jeder Request ist entweder idempotent oder gegen Wiederholung geschützt**
  (siehe 4.3), damit Retries gefahrlos sind.
- **Alle Mehrbyte-Werte sind Little-Endian.** Serialisiert wird feldweise, nicht
  per `memcpy` von C-Structs (keine Abhängigkeit von Padding/Compiler).

---

## 2. Physikalische Schicht

| Parameter | Wert |
|---|---|
| STM32 | USART1, PA9 (TX) / PA10 (RX) |
| ESP32 | `Serial2`, GPIO17 (TX) / GPIO5 (RX) |
| Format | 8N1, kein Handshake |
| Baudrate | 115200 (Standard); später ggf. 460800 per `SET_LINK` – nicht Teil von v1 |
| Reset / Boot | ESP32 steuert STM32-NRST (GPIO15) und BOOT0 (GPIO14), wie bisher |

Die Firmware-Aktualisierung des STM32 läuft weiterhin über den **ROM-Bootloader**
(AN3155) auf derselben UART: Der ESP32 setzt BOOT0, pulst NRST und spricht dann
das ST-Bootloader-Protokoll. Das 3-Sekunden-Fenster mit „DEADBEEF“ der Firmware
1.x entfällt.

---

## 3. Rahmen (Framing)

### 3.1 Aufbau

Auf der Leitung wird jeder Rahmen **COBS-kodiert** und mit einem
**`0x00`-Byte abgeschlossen**. `0x00` kommt im kodierten Inhalt nie vor, so dass
sich der Empfänger nach jedem Fehler beim nächsten `0x00` neu synchronisiert.

Dekodierter Inhalt:

```
 0        1        2        3        4 … 4+n-1     4+n   4+n+1
+--------+--------+--------+--------+-------------+------+------+
| ver    | kind   | seq    | msg    | payload (n) | crc_lo crc_hi|
+--------+--------+--------+--------+-------------+------+------+
```

| Feld | Größe | Bedeutung |
|---|---|---|
| `ver` | u8 | Protokollversion, aktuell `1`. Rahmen mit anderer Version werden verworfen (Zähler `rx_bad_version`). |
| `kind` | u8 | `0x01` REQUEST, `0x02` RESPONSE, `0x03` EVENT. Andere Werte: verwerfen. |
| `seq` | u8 | REQUEST: vom ESP vergeben (1…255, `0` ist reserviert). RESPONSE: Kopie aus dem Request. EVENT: eigener Zähler des STM32 (1…255, umlaufend). |
| `msg` | u8 | Nachrichten-ID (Kapitel 5/6). Eine RESPONSE trägt dieselbe ID wie ihr Request. |
| `payload` | 0…240 | nachrichtenspezifisch |
| `crc` | u16 | CRC-16/CCITT-FALSE (Poly `0x1021`, Init `0xFFFF`, keine Reflexion, kein XorOut) über `ver` … Ende `payload` |

- Die Payload-Länge ergibt sich aus der dekodierten Rahmenlänge (`len - 6`);
  ein eigenes Längenfeld gibt es nicht.
- Maximale dekodierte Rahmenlänge: **246 Byte**, kodiert ≤ 248 Byte inkl. `0x00`.
- Rahmen kürzer als 6 Byte, länger als 246 Byte, mit COBS- oder CRC-Fehler werden
  **still verworfen**. Je Fehlerart zählt ein eigener Zähler (siehe `GET_DIAG`).
  Auf fehlerhafte Rahmen wird **nie** geantwortet, weil `seq` nicht vertrauenswürdig ist.

### 3.2 Empfangspuffer

- Jede Seite hält einen Rahmenpuffer von mindestens 256 Byte. Läuft er ohne `0x00`
  voll, wird bis zum nächsten `0x00` alles verworfen (`rx_overflow`).
- Der STM32 nimmt UART-Bytes per Interrupt oder DMA in einen Ringpuffer von
  ≥ 512 Byte auf. Die Auswertung läuft im Hauptloop. Innerhalb eines Loop-Durchlaufs
  werden **alle** vollständigen Rahmen verarbeitet.

---

## 4. Ablauf

### 4.1 Request / Response

- Der ESP hat **höchstens einen offenen Request** (Stop-and-Wait). Das reicht bei
  der geringen Datenmenge und hält beide Seiten einfach.
- Der STM32 antwortet innerhalb von **20 ms** (Ziel), spätestens nach 50 ms.
  Aktionen, die länger dauern (Fahren, Kalibrieren, 1-Wire-Suche), werden nur
  **angenommen**. Ihr Ergebnis kommt später als Event.
- **Timeout auf ESP-Seite: 100 ms.** Danach wird derselbe Request mit **derselben
  `seq`** wiederholt, höchstens 3 Mal.
- Nach 3 erfolglosen Wiederholungen gilt die Verbindung als **gestört**
  (`LINK_DOWN`):
  - Der ESP versucht alle 1 s `HELLO`.
  - Nach 30 s ohne Antwort setzt er den STM32 per NRST hart zurück, höchstens
    einmal pro 5 min.
  - Er meldet `LINK_DOWN` an das Datenmodell. Home Assistant zeigt die Ventile dann
    als „nicht verfügbar“.

### 4.2 Statuscode jeder Response

Das **erste Payload-Byte jeder Response** ist ein Statuscode:

| Code | Name | Bedeutung |
|---|---|---|
| `0x00` | `OK` | ausgeführt bzw. angenommen |
| `0x01` | `ERR_UNKNOWN_MSG` | Nachrichten-ID unbekannt |
| `0x02` | `ERR_BAD_LEN` | Payload-Länge passt nicht |
| `0x03` | `ERR_RANGE` | Wert außerhalb des gültigen Bereichs; das zweite Byte nennt den Index des ersten ungültigen Felds |
| `0x04` | `ERR_CHANNEL` | Kanal ungültig oder deaktiviert |
| `0x05` | `ERR_BUSY` | Ressource belegt (z. B. 1-Wire-Suche läuft bereits) |
| `0x06` | `ERR_STATE` | im aktuellen Zustand nicht erlaubt (z. B. Ventil nicht kalibriert) |
| `0x07` | `ERR_STORAGE` | EEPROM-Fehler |
| `0x08` | `ERR_NOT_READY` | STM32 noch in der Initialisierung (vor `HELLO`) |
| `0xFF` | `ERR_INTERNAL` | sonstiger Fehler |

Die weiteren Payload-Bytes folgen nur bei `OK`, sofern unten nicht anders angegeben.

### 4.3 Schutz vor doppelter Ausführung

Der STM32 merkt sich `seq` und den Antwortrahmen des **letzten** Requests.
Kommt ein Request mit derselben `seq` und derselben `msg` erneut, wird er **nicht
noch einmal ausgeführt**: Der STM32 sendet nur die gespeicherte Antwort erneut.
So startet z. B. ein wiederholtes `CALIBRATE` keine zweite Kalibrierung.

Der ESP vergibt `seq` fortlaufend (1…255 umlaufend, `0` wird übersprungen). Nach
`HELLO` mit gesetztem `reset_seq` vergisst der STM32 die gespeicherte `seq`.

### 4.4 Events

- Events werden **nicht quittiert**. Verlorene Events schaden nicht:
  - Jedes Event mit Zustandsdaten trägt den **vollständigen** aktuellen Zustand
    seines Objekts, nie nur die Differenz.
  - Der ESP fragt zusätzlich alle **10 s** `GET_VALVE_STATES` ab. Das ist das
    Sicherheitsnetz und gleichzeitig das Lebenszeichen.
- Über die Event-`seq` erkennt der ESP Lücken (Zähler `evt_lost`). Bei einer Lücke
  holt er den kompletten Zustand sofort statt erst nach 10 s.
- Events werden nur gesendet, wenn gerade kein Response-Rahmen läuft. Eine
  Response hat Vorrang. Beide Richtungen sind voll duplex.
- Ein Rate-Limit gilt pro Kanal: höchstens 10 `EVT_VALVE` pro Sekunde. Bei
  schnelleren Änderungen wird nur der jeweils letzte Zustand gesendet.

### 4.5 Start und Handshake

1. Nach dem Reset sendet der STM32 `EVT_BOOT`. Das Event wird alle 1 s wiederholt,
   bis ein `HELLO` eintrifft.
2. Der ESP sendet `HELLO`, entweder auf `EVT_BOOT` hin oder beim eigenen Start.
3. Der ESP prüft in der Antwort:
   - Ist `proto_version` ≠ 1, wird keine weitere Nachricht gesendet. Der Status
     wird `INCOMPATIBLE`, die Web-UI bietet das STM32-Update an.
   - `channel_count`, `hw_rev` und `capabilities` werden ins Datenmodell übernommen.
4. Der ESP überträgt die Konfiguration:
   - `SET_SYSTEM_CONFIG`
   - `SET_MOTOR_PARAMS`
   - `SET_CHANNELS`
   - danach die Sollwerte per `SET_TARGETS`
5. Danach folgt der Normalbetrieb.

Bis zum ersten `HELLO` beantwortet der STM32 alle anderen Requests mit
`ERR_NOT_READY`. Er arbeitet trotzdem mit seiner zuletzt gespeicherten
Konfiguration weiter (siehe 7).

---

## 5. Requests (ESP → STM32)

Bereiche der Nachrichten-IDs:

| Bereich | Inhalt |
|---|---|
| `0x01–0x0F` | System |
| `0x10–0x2F` | Ventile |
| `0x30–0x3F` | Sensoren |
| `0x40–0x4F` | Diagnose |
| `0x80–0xBF` | Events |

Angegeben ist jeweils die Payload. Bei der Antwort ist der Statuscode aus 4.2
nicht noch einmal aufgeführt.

### 5.1 System

#### `0x01 HELLO`
Request:

| Feld | Typ | Bedeutung |
|---|---|---|
| `esp_proto_version` | u8 | Protokollversion des ESP |
| `flags` | u8 | Bit 0 `reset_seq` |

Antwort:

| Feld | Typ | Bedeutung |
|---|---|---|
| `proto_version` | u8 | `1` |
| `fw_major`, `fw_minor`, `fw_patch` | u8 ×3 | Firmware-Version |
| `fw_build` | u32 | Build-Zeitstempel (Unix) oder CI-Buildnummer |
| `hw_rev` | u8 | `1` = C1, `2` = C2/C3 |
| `mcu` | u8 | `1` = F401, `2` = F411 |
| `mcu_uid` | u8[12] | 96-Bit Unique-ID des STM32 |
| `channel_count` | u8 | Anzahl Kanäle (12) |
| `capabilities` | u32 | Bit 0 DS18B20, Bit 1 DS2438, Bit 2 RS485, Bit 3 CAN |
| `reset_cause` | u8 | `0` unbekannt, `1` Power-on, `2` Pin/NRST, `3` Software, `4` IWDG, `5` Brown-out, `6` Low-power |
| `boot_count` | u32 | Anzahl Starts (EEPROM) |
| `config_crc` | u16 | CRC der aktuell aktiven System- und Motor-Konfiguration; der ESP kann damit das erneute Senden überspringen |

#### `0x02 PING`
Request: beliebige 0…16 Byte. Antwort: dieselben Bytes zurück. Dient zum Test
des Links.

#### `0x03 RESET`
Request:

| Feld | Typ | Bedeutung |
|---|---|---|
| `magic` | u32 | `0x52535431` („RST1“) |

Der STM32 antwortet `OK` und hält alle Motoren an. Er schreibt ausstehende
EEPROM-Daten und startet danach neu (`NVIC_SystemReset`).

#### `0x04 SET_SYSTEM_CONFIG`

| Feld | Typ | Bereich | Bedeutung |
|---|---|---|---|
| `link_timeout_s` | u16 | 0, 10…3600 | Hört der STM32 so lange keinen gültigen Request, gilt der ESP als ausgefallen. `0` = nie. |
| `failsafe_pos` | u16 | 0…1000, `0xFFFF` | Zielposition (0,1 %) bei ESP-Ausfall. `0xFFFF` = aktuelle Position halten. |
| `startup_pos` | u16 | 0…1000, `0xFFFF` | Zielposition nach dem Boot, bis der ESP Sollwerte schickt. `0xFFFF` = zuletzt gespeicherte Zielposition. |
| `sensor_interval_s` | u16 | 5…3600 | Intervall der 1-Wire-Messung |
| `recal_after_moves` | u16 | 0, 50…10000 | automatische Nachkalibrierung nach so vielen Fahrten. `0` = aus. |
| `recal_after_days` | u8 | 0, 1…60 | automatische Nachkalibrierung nach so vielen Tagen Laufzeit. `0` = aus. |

Wird persistent gespeichert. Antwort: `config_crc` (u16).

#### `0x05 GET_SYSTEM_CONFIG`
Antwort: dieselben Felder wie `SET_SYSTEM_CONFIG`.

### 5.2 Ventile

Positionen werden immer in **0,1 % (0…1000)** angegeben: 0 = geschlossen,
1000 = ganz offen. Kanäle zählen von 0 bis 11. Kanalmasken sind `u16`; Bit n
steht für Kanal n.

#### `0x10 GET_VALVE_STATES`
Request: leer. Antwort:

| Feld | Typ | Bedeutung |
|---|---|---|
| `count` | u8 | Anzahl Kanäle |
| `count` × `ValveState` | | siehe 5.4 |

#### `0x11 SET_TARGETS`
Request: `count` (u8, 1…12), danach `count` × { `channel` u8, `target` u16 }.

- Die Ziele werden übernommen und in die Fahrwarteschlange eingereiht.
- Ist ein Ziel ungültig, wird **keines** übernommen: Antwort `ERR_RANGE` bzw.
  `ERR_CHANNEL`, im zweiten Byte der Index des fehlerhaften Eintrags.
- Ein Ziel gleich der aktuellen Position ist erlaubt und bewirkt nichts.
- Läuft auf dem Kanal eine Kalibrierung, wird das Ziel gespeichert und danach
  angefahren.

#### `0x12 CALIBRATE`

| Feld | Typ | Bedeutung |
|---|---|---|
| `mask` | u16 | Kanäle, die angelernt werden sollen |

Die Kanäle werden nacheinander angelernt (immer nur ein Motor zur Zeit). Die
Antwort `OK` bedeutet nur „eingeplant“. Das Ergebnis kommt pro Kanal als
`EVT_CALIBRATION`.

#### `0x13 STOP`

| Feld | Typ | Bedeutung |
|---|---|---|
| `mask` | u16 | Kanäle, die angehalten werden sollen |

Laufende Fahrt bzw. Kalibrierung wird abgebrochen, wartende Aufträge der Kanäle
werden verworfen. Eine abgebrochene Fahrt lässt die Position ungenau; der Kanal
bekommt dann das Flag `POS_UNCERTAIN` (siehe 5.4).

#### `0x14 SET_CHANNELS`

| Feld | Typ | Bedeutung |
|---|---|---|
| `enabled_mask` | u16 | Nur Kanäle, deren Bit gesetzt ist, werden gefahren, geprüft und kalibriert |

Wird persistent gespeichert.

#### `0x15 DETECT`

| Feld | Typ | Bedeutung |
|---|---|---|
| `mask` | u16 | Kanäle, deren Anschluss geprüft wird |

Prüft per kurzem Testpuls (ohne Positionsänderung), ob ein Motor angeschlossen
ist. Das Ergebnis kommt als `EVT_VALVE`.

#### `0x16 GET_VALVE_DETAIL`
Request: `channel` (u8). Antwort:

| Feld | Typ | Bedeutung |
|---|---|---|
| `channel` | u8 | |
| `state` | `ValveState` | siehe 5.4 |
| `open_counts` | u32 | Impulse für den Hub 0 → 100 % (letzte Kalibrierung) |
| `close_counts` | u32 | Impulse für den Hub 100 → 0 % |
| `deadzone_counts` | i32 | `close_counts − open_counts`, mit Vorzeichen |
| `calib_age_h` | u32 | Stunden seit der letzten erfolgreichen Kalibrierung, `0xFFFFFFFF` = nie |
| `moves_since_calib` | u32 | |
| `moves_total` | u32 | |
| `mean_current_open_ma10` | u16 | mittlerer Strom beim Öffnen, 0,1 mA |
| `mean_current_close_ma10` | u16 | mittlerer Strom beim Schließen, 0,1 mA |
| `peak_current_ma10` | u16 | höchster Strom der letzten Fahrt, 0,1 mA |
| `last_move_counts` | i32 | gezählte Impulse der letzten Fahrt, mit Vorzeichen |
| `last_move_ms` | u32 | Dauer der letzten Fahrt |
| `calib_retries` | u8 | Wiederholungen bei der letzten Kalibrierung |
| `last_fault` | u8 | letzter Fehlercode (siehe 5.4), bleibt nach Behebung stehen |

#### `0x20 SET_MOTOR_PARAMS` / `0x21 GET_MOTOR_PARAMS`
Gilt für alle Kanäle gemeinsam. Felder:

| Feld | Typ | Bereich | Standard | Bedeutung |
|---|---|---|---|---|
| `endstop_factor_open` | u8 | 11…50 | 17 | Endanschlag erkannt, wenn der Strom über `mean × factor/10` liegt (Öffnen) |
| `endstop_factor_close` | u8 | 11…50 | 17 | wie oben, beim Schließen |
| `endstop_abs_ma` | u16 | 20…300 | 100 | absolute Stromgrenze, die immer einen Endanschlag auslöst |
| `inrush_blank_ms` | u16 | 50…1000 | 250 | Ausblendung des Anlaufstroms |
| `opencircuit_ma10` | u16 | 5…100 | 20 | darunter gilt „kein Motor“ (0,1 mA) … |
| `opencircuit_ms` | u16 | 100…5000 | 500 | … wenn das so lange **ununterbrochen** anliegt |
| `move_timeout_s` | u16 | 10…600 | 120 | Höchstdauer einer einzelnen Fahrt |
| `min_stroke_counts` | u16 | 100…60000 | 3000 | Mindestimpulse für einen gültigen Hub |
| `max_calib_retries` | u8 | 0…5 | 2 | Wiederholungen bei misslungener Kalibrierung |

Bei `SET` werden alle Felder geprüft. Das erste ungültige Feld wird mit
`ERR_RANGE` und seinem Feldindex gemeldet (0-basiert, in Tabellenreihenfolge);
dann wird nichts übernommen. Die Werte werden persistent gespeichert. Antwort
auf `SET`: `config_crc` (u16).

### 5.3 Sensoren (1-Wire)

Sensoren werden immer über ihre **ROM-ID (u8[8], Bus-Reihenfolge, Byte 0 =
Familiencode)** angesprochen. Listenpositionen haben keine dauerhafte Bedeutung.

#### `0x30 SENSOR_SCAN`
Request: leer. Startet eine neue Bus-Suche. Antwort `OK` oder `ERR_BUSY`. Das
Ergebnis kommt als `EVT_SENSOR_SCAN_DONE`.

#### `0x31 GET_SENSORS`
Request:

| Feld | Typ | Bedeutung |
|---|---|---|
| `offset` | u8 | erste gewünschte Listenposition |

Antwort:

| Feld | Typ | Bedeutung |
|---|---|---|
| `total` | u8 | Gesamtzahl der Sensoren |
| `offset` | u8 | wie angefragt |
| `count` | u8 | Anzahl Einträge in dieser Antwort |
| `count` × `SensorValue` | | siehe 5.4 |

Höchstens 12 Einträge pro Antwort (12 × 16 = 192 Byte). Bei mehr Sensoren fragt
der ESP mit erhöhtem `offset` nach.

### 5.4 Gemeinsame Strukturen

#### `ValveState` (10 Byte)

| Feld | Typ | Bedeutung |
|---|---|---|
| `channel` | u8 | |
| `mode` | u8 | `0` DISABLED, `1` UNKNOWN (noch nicht geprüft), `2` NOT_CONNECTED, `3` IDLE, `4` OPENING, `5` CLOSING, `6` CALIBRATING, `7` QUEUED (wartet auf Motorfreigabe), `8` FAULT |
| `flags` | u8 | siehe unten |
| `fault` | u8 | aktueller Fehler, siehe unten |
| `actual` | u16 | Ist-Position, 0,1 %; `0xFFFF` = unbekannt |
| `target` | u16 | Ziel-Position, 0,1 % |
| `current_ma10` | u16 | Strom: während der Fahrt aktuell, sonst Mittelwert der letzten Fahrt (0,1 mA) |

`flags`:

| Bit | Name | Bedeutung |
|---|---|---|
| 0 | `CALIBRATED` | gültige Kalibrierdaten vorhanden |
| 1 | `CALIB_PENDING` | Kalibrierung eingeplant |
| 2 | `POS_UNCERTAIN` | Position nicht sicher (Abbruch, Boot ohne Referenzfahrt) |
| 3 | `AT_ENDSTOP` | Endanschlag bei der letzten Fahrt erkannt |
| 4 | `FAILSAFE` | Ziel stammt aus `failsafe_pos` |

Bits 5–7 sind reserviert (0).

`fault`:

| Code | Name | Bedeutung |
|---|---|---|
| `0` | NONE | kein Fehler |
| `1` | OPEN_CIRCUIT | kein Strom, Motor abgesteckt |
| `2` | BLOCKED | Endanschlag deutlich vor dem erwarteten Hubende, mehrfach |
| `3` | TIMEOUT | `move_timeout_s` überschritten |
| `4` | OVERCURRENT | `endstop_abs_ma` sofort nach der Anlaufausblendung |
| `5` | CALIB_FAILED | Hub < `min_stroke_counts` nach allen Wiederholungen |
| `6` | NO_PULSES | Strom fließt, aber es kommen keine Back-EMF-Impulse |
| `7` | COUNT_MISMATCH | Impulse bei der Referenzfahrt weichen > 15 % ab |

#### `SensorValue` (16 Byte)

| Feld | Typ | Bedeutung |
|---|---|---|
| `rom` | u8[8] | ROM-ID |
| `kind` | u8 | `1` Temperatur (DS18B20), `2` Spannung VAD (DS2438), `3` Spannung VDD (DS2438), `4` Temperatur (DS2438) |
| `status` | u8 | `0` OK, `1` CRC-Fehler, `2` nicht erreichbar, `3` noch nicht gemessen, `4` außerhalb des Bereichs (z. B. 85 °C-Resetwert) |
| `age_s` | u16 | Sekunden seit der letzten gültigen Messung, höchstens `0xFFFF` |
| `value` | i32 | milli-Einheit: m°C bzw. mV |

Ein DS2438 erscheint als bis zu drei Einträge mit derselben ROM-ID und
unterschiedlichem `kind`.

### 5.5 Diagnose

#### `0x40 GET_DIAG`
Request: leer. Antwort:

| Feld | Typ | Bedeutung |
|---|---|---|
| `uptime_s` | u32 | |
| `rx_frames_ok` | u32 | |
| `rx_crc_err` | u16 | |
| `rx_cobs_err` | u16 | |
| `rx_overflow` | u16 | |
| `rx_bad_version` | u16 | |
| `rx_duplicate` | u16 | Wiederholungen, die über 4.3 beantwortet wurden |
| `uart_hw_err` | u16 | Framing- oder Overrun-Fehler der UART-Hardware |
| `ow_crc_err` | u16 | |
| `ow_bus_err` | u16 | Bus-Kurzschluss, kein Presence-Puls |
| `eeprom_writes` | u32 | |
| `eeprom_err` | u16 | |
| `loop_max_us` | u32 | längster Hauptloop-Durchlauf seit dem Start |
| `mcu_temp_c10` | i16 | interne Temperatur des STM32, 0,1 °C |

#### `0x41 CLEAR_DIAG`
Request: leer. Setzt die Zähler zurück; Uptime und EEPROM-Zähler bleiben.

---

## 6. Events (STM32 → ESP)

| ID | Name | Payload | Wann |
|---|---|---|---|
| `0x80` | `EVT_BOOT` | wie die `HELLO`-Antwort ohne Statuscode | nach dem Reset, wiederholt bis `HELLO` |
| `0x81` | `EVT_VALVE` | `ValveState` | bei jeder Änderung von `mode`, `flags` oder `fault`; bei Fahrtende; während einer Fahrt höchstens alle 500 ms mit aktueller Position |
| `0x82` | `EVT_CALIBRATION` | `channel` u8, `result` u8 (`0` OK, sonst `fault`-Code), `open_counts` u32, `close_counts` u32, `retries` u8 | Ende einer Kalibrierung |
| `0x83` | `EVT_SENSORS` | wie die `GET_SENSORS`-Antwort ohne Statuscode | nach jedem Messzyklus; mehrere Events, wenn > 12 Sensoren |
| `0x84` | `EVT_SENSOR_SCAN_DONE` | `total` u8, `ds18_count` u8, `ds2438_count` u8, `bus_ok` u8 | Ende von `SENSOR_SCAN` |
| `0x85` | `EVT_LINK_TIMEOUT` | `failsafe_pos` u16 | `link_timeout_s` abgelaufen; gesendet, sobald der ESP wieder antwortet |
| `0x86` | `EVT_LOG` | `level` u8 (`0` Error, `1` Warn, `2` Info, `3` Debug), `text` (UTF-8, ≤ 200 Byte, ohne Nullterminierung) | Diagnosemeldungen; der ESP leitet sie an Syslog bzw. die Web-UI weiter |

`EVT_LOG` mit Level Debug wird nur gesendet, wenn der ESP es per
`SET_SYSTEM_CONFIG` freigibt. Dafür ist ein zusätzliches Feld in v1.1 vorgesehen,
in v1 werden nur Error, Warn und Info gesendet. Damit entfällt die Debug-UART
des STM32 im Normalbetrieb.

---

## 7. Verhalten des STM32 ohne ESP

- Der STM32 arbeitet mit der zuletzt gespeicherten Konfiguration (System, Motor,
  Kanäle) auch ohne ESP.
- Nach dem Boot:
  1. Jeder freigegebene Kanal mit gültiger Kalibrierung fährt eine
     **Referenzfahrt** in Richtung geschlossen bis zum Endanschlag.
  2. Danach fährt er auf `startup_pos`.
  3. Weicht die Impulszahl der Referenzfahrt vom Erwarteten ab, setzt der STM32
     `POS_UNCERTAIN` und plant eine Kalibrierung ein.
- Ist `link_timeout_s` ≠ 0 und kommt so lange kein gültiger Request:
  - Ist `failsafe_pos` ≠ `0xFFFF`, wird jeder Kanal dorthin gefahren, mit Flag
    `FAILSAFE`.
  - Der erste gültige Request danach beendet den Failsafe-Zustand. Die
    Sollwerte bleiben dabei, bis der ESP neue sendet.

---

## 8. Beispiel

ESP setzt Kanal 3 auf 45,0 %:

```
REQ  ver=1 kind=1 seq=17 msg=0x11  payload: 01 03 C2 01        (count=1, ch=3, target=450)
RSP  ver=1 kind=2 seq=17 msg=0x11  payload: 00                 (OK)
EVT  ver=1 kind=3 seq=88 msg=0x81  payload: 03 04 01 00 2C 01 C2 01 D2 00
                                   (ch=3, OPENING, CALIBRATED, actual=300, target=450, 21.0 mA)
EVT  ver=1 kind=3 seq=89 msg=0x81  payload: 03 03 01 00 C1 01 C2 01 CA 00
                                   (ch=3, IDLE, actual=449, target=450, 20.2 mA)
```

Auf der Leitung wird jeder Rahmen dekodiert so geschrieben:
`ver kind seq msg payload… crc_lo crc_hi`. Er wird COBS-kodiert und mit `00`
abgeschlossen.

---

## 9. Auslastung (Plausibilitätsrechnung)

115200 Baud ≈ 11,5 kB/s.

- Grundlast: `GET_VALVE_STATES` alle 10 s, etwa 140 Byte in beide Richtungen.
- Sensor-Events alle 30 s mit 30 Sensoren, etwa 500 Byte.
- `EVT_VALVE` während einer Fahrt alle 500 ms, je etwa 20 Byte.

Zusammen sind das **deutlich unter 1 %** der Kapazität. Die Baudrate muss nicht
erhöht werden.

---

## 10. Umsetzungshinweise

- Gemeinsamer C99-Code unter `common/protocol/` ohne Arduino-Abhängigkeit. Beide
  Firmwares binden ihn ein, getestet wird er auf dem Host (`env:native`).
- Encoder und Decoder je Nachricht als Funktionspaar
  (`vdm_enc_set_targets()`, `vdm_dec_set_targets()`). Der Decoder prüft Länge
  und Bereiche, nicht der Aufrufer.
- Auf dem STM32 wird kein dynamischer Speicher verwendet, auf dem ESP32 im
  Link-Pfad keine `String`s.
- Testvektoren (Rahmen als Hex) liegen unter `common/protocol/test/vectors/`
  und werden von beiden Seiten verwendet.

---

## 11. Offene Punkte

- [ ] Baudratenwechsel (`SET_LINK`) – vermutlich unnötig, siehe 9.
- [ ] Freigabe von `EVT_LOG` Debug (v1.1).
- [ ] RS485/CAN: bleiben in v1 unbenutzt; ein späterer Ausbau bekommt den
      eigenen ID-Bereich `0x50–0x5F`.
