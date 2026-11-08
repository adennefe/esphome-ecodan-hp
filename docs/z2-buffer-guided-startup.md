# Z2: THW10-Pufferführung und Mindestvorlauf ab 20 °C

Firmware: `asgard-z2-en-2026-11-08.02-buffer-guided-startup`.
Branch: `feat/guard-delta-tolerance`, Basis `e8b9533`.

## Änderungen und Grenzen

Dies erweitert dieselbe `FlowAnchoredStartup`-Instanz. Es gibt keinen zweiten
Startup-Regler und keinen zusätzlichen FTC-Schreibpfad. Die Frequenz-HOLD-Logik
der Version 07.01 ist ersetzt; die Frequenz wird hier nur noch protokolliert.
Die normalen Frequenzfunktionen anderer bestehender Optimizer-Funktionen, etwa
Guard Recovery, bleiben erhalten.

`minimum_heating_flow_temp_z2` hat weiterhin den Default 24 °C, ist jetzt aber
zwischen 20 und 40 °C in 0,5-K-Schritten einstellbar. Entity-ID, Name und
`restore_value: true` bleiben erhalten. Z1 behält seine bisherige Untergrenze
24 °C und seinen bisherigen Initialwert 25 °C. Gespeicherte Benutzerwerte werden
nicht überschrieben. Die technische Startup-Untergrenze bleibt 20 °C.

Normalbetrieb ohne Guard-Anhebung, Beispiel AA vor Minimum 22,5 °C:

| Konfiguriertes Z2-Minimum | Normale Z2-Anforderung |
|---:|---:|
| 20 °C | 22,5 °C |
| 22 °C | 22,5 °C |
| 24 °C | 24,0 °C |

Während Warmup und kontrollierter Übergabe kann der vorhandene lokale
Minimum-Ausnahmeweg niedrigere Anforderungen erlauben, jeweils ab 20 °C.
Nach Beendigung ist das konfigurierte Minimum vollständig wirksam. Geräteinterne
Schutzfunktionen und bestehende Asgard-Guards behalten ihre Priorität.

## Eine Zustandsmaschine

| Phase | Verhalten |
|---|---|
| WAITING | Warten auf bestätigten echten Verdichterstart |
| BUFFER_WARMUP | Feste Startreferenz, zeitabhängige Obergrenze; optional THW10-Bereitschaft |
| HANDOVER | Normale AA kontrolliert vom zuletzt angeforderten Wert aus freigeben |
| NORMAL_AA | Normale AA ohne Startup-Begrenzung |

Ein empfangenes echtes OFF -> ON im zulässigen unabhängigen Z2-Heizbetrieb ist
erforderlich. Erstes ON nach ESP-Neustart ist kein Start. Ein Kommunikationsloch
darf keinen künstlichen Start erzeugen. Nach Beendigung bleibt derselbe Takt
reserviert; erst ein bestätigtes OFF gibt einen neuen Start frei. Initiale
OFF-/Modusmeldungen dürfen den ersten späteren echten Start nicht verbrauchen.

Bei ausgeschaltetem Flow-Anchored-Schalter bleibt die bisherige Priorität der
alten Startrampen erhalten. Ist Flow-Anchored beim Start ausgewählt, bleiben
Return-Guided und beide Soft-Starts für diesen Takt ausgesetzt, auch nach früher
Freigabe oder einem nicht berechtigten Start. Während Warmup und HANDOVER ist
Guard Recovery ausgesetzt. Z1, DHW, ODIN, Defrost und Lockout behalten ihre
vorhandenen Zuständigkeiten.

## Feste Referenz und Zeitrampe

Nur beim bestätigten Start speichern:

```text
start_return = status.Z2ReturnTemperature
startup_base = start_return + initial_delta
ramp_cap = startup_base + floor(elapsed / interval) * step
ramp_request = min(normal_AA_before_minimum, ramp_cap)
```

Bei 20,5 °C Z2-Start-Rücklauf, Initial Delta 1,0 K, Schritt 0,5 K und Intervall
15 Minuten ist der Cap nach 0/15/30/45 Minuten 21,5/22,0/22,5/23,0 °C.
Weder der aktuelle Z2-Rücklauf noch das konfigurierte Z2-Minimum werden als neue
Startreferenz verwendet. Eine niedrige normale AA bleibt erlaubt. Der Cap
erzwingt keinen Temperaturanstieg. Guards können ihn übersteuern.

## Vorhandener THW10, ausdrücklich ohne Altersprüfung

Quelle ist ausschließlich `status.MixingTankTemperature`, bereits über
`TEMPERATURE_STATE_D` eingelesen und als `Mixing Tank Temp` veröffentlicht.
Keine neue Sensor-Entität, kein zusätzlicher Fühler, keine HA-Synchronisierung.

Gültig genau dann, wenn:

- `status.has_mixing_tank()` die Mitsubishi-Konfiguration bestätigt;
- der Wert endlich ist;
- der Wert im inklusiven Bereich 0..80 °C liegt.

**Es gibt keinen THW10-Empfangszeitstempel, keine THW10-Altersprüfung und keine
Prüfung auf Wertänderungen.** Ein 30 Minuten oder länger unveränderter plausibler
Wert bleibt gültig. Bereits vorhandene NaN-Initialisierung des Statuswerts
verhindert eine erfundene Messung vor dem ersten tatsächlichen Empfang.

Bei OFF, fehlendem oder ungültigem THW10 bleibt die zeitbasierte Rampe wirksam.
Die bisherigen Voraussetzungen für unabhängige Z2-Regelung bleiben erforderlich.
Insbesondere koppelt das bestehende Ecodan-Statusmodell die Zwei-Zonen-Erkennung
an Mitsubishi-DIP-Einstellungen: Entfernt man dort die gesamte Mischkonfiguration,
kann auch die bisher notwendige unabhängige Z2-Freigabe entfallen. Diese allgemeine
Modusprüfung wird durch den THW10-Fallback nicht umgangen.

Der bestehende 120-s-Schutz für WP-/Z2-Vor-/Rücklauf, Durchfluss und bestätigten
Verdichterstatus bleibt erhalten. Ein allgemeiner Kommunikationsausfall beendet
die Startup-Sonderregelung auch dann, wenn der letzte THW10-Wert plausibel ist.
Die Frequenz ist für diese neue Rampe kein Freigabe-, HOLD- oder Abbruchkriterium.
Guard Recovery behält seine eigenen bisherigen Telemetrieprüfungen.

## Dynamische Pufferbereitschaft

```text
normal_aa_target = clamp(normal_AA_before_minimum, configured_z2_min, configured_z2_max)
buffer_ready_threshold = max(configured_z2_min, normal_aa_target - buffer_margin)
```

Die Bereitschaftsbedingung ist erfüllt, wenn Buffer Mode ON und THW10 gültig sind
und **sowohl THW10 als auch der tatsächliche Z2-Vorlauf mindestens die Schwelle
erreichen**. Das verhindert eine Freigabe allein aufgrund eines warmen
Puffermesspunkts bei noch unzureichendem Z2-Vorlauf.

Beispiel: normale AA 26 °C, Minimum 22 °C, Margin 0,5 K -> Schwelle 25,5 °C.
THW10 26 °C bei Z2-Vorlauf 23 °C reicht nicht. Sind beide Werte mindestens
25,5 °C, beginnt die fünfminütige Bedingungsdauer. Ein Unterschreiten, ein
ungültiger THW10 oder eine geänderte höhere AA-Schwelle unterbricht sie.
Wiederholte gleiche Messwerte unterbrechen sie nicht.

Der Fünf-Sekunden-Service beobachtet die Bedingung mit dem zuletzt berechneten
AA-Ziel. Die tatsächliche Übergabe wird nur innerhalb einer neuen AA-Berechnung
mit erneut ausgewerteter dynamischer Schwelle begonnen. Diese Zeiten messen die
**Dauer einer erfüllten Bedingung**, nicht das Alter des THW10-Sensors.

## Raumwärmebedarf

Der tatsächliche Raumwert aus der ausgewählten vorhandenen Quelle wird verwendet:

- Bis 0,1 K unter Soll: Warmup kann weiterlaufen.
- Mindestens 0,2 K unter Soll: kontinuierliche Zehn-Minuten-Bedingung starten.
  Eine Erholung unter diese Defizitgrenze setzt den Timer zurück.
- Mindestens 0,5 K unter Soll: sofort zu normaler AA, ohne langsame Übergabe.
- Ungültiger Raumwert: Begrenzung beenden.

Die Grenzen werden mit kleiner Float-Toleranz statt mit Ganzzahl-Kelvin-Vergleichen
geprüft. Aus einem Defizit von 0,15 K wird nicht durch Rundung künstlich 0,2 K.
Die Prüfungen gelten in BUFFER_WARMUP und HANDOVER.

## Guards und Übergabe

Der vorhandene Buffer-/Stop-Guard wird unverändert nach der Startup-Berechnung
ausgeführt und darf sofort höher anfordern. Die frühere Zwei-Eingriffe-Regel ist
ersetzt: Erst eine zusammenhängende Stop-Guard-Anhebung von mindestens 0,5 K
über fünf Minuten gibt normale AA direkt frei. Einzelne oder wiederholte kurze
Eingriffe, Abweichungen unter 0,5 K und Zwischenzyklen ohne relevanten Eingriff
erfüllen diese Bedingung nicht.

Reguläre THW10-Bereitschaft führt zu HANDOVER. Ausgangspunkt ist der zuletzt vom
bestehenden AA-Writer tatsächlich in die Queue gestellte oder als unverändert
bestätigte Sollwert **nach Guards**, nicht der alte Rampen-Cap und nicht pauschal
das Minimum. Ein abgelehnter Schreibversuch verschiebt die Referenz nicht.

Pro drei Minuten darf die Anforderung höchstens 0,5 K steigen. Wiederholte
AA-Aufrufe, verspätete Aufrufe und nicht angenommene Schreibversuche akkumulieren
keine zusätzlichen Stufen. Schutzfunktionen dürfen schneller anheben; eine solche
Anhebung wird für die folgenden Übergabeschritte berücksichtigt. Niedrigere
normale AA ist sofort zulässig, soweit Minimum und Guards sie erlauben.

Beispiel: letzter tatsächlich angeforderter Wert nach Guard 25 °C, normale AA
26 °C: bei HANDOVER zunächst 25 °C, frühestens nach drei Minuten 25,5 °C,
frühestens nach weiteren drei Minuten 26 °C und NORMAL_AA.

Warmup und HANDOVER teilen dieselbe konfigurierte Gesamtdauer, maximal 60 Minuten.
Der Timer wird bei Bereitschaft nicht zurückgesetzt. Am Gesamtlimit wird normale
AA unmittelbar freigegeben, auch bei noch kaltem Puffer. Dieser erzwungene
Übergang sowie dringender Bedarf oder Schutzbedarf können einen Sprung verursachen.
Ohne THW10 läuft die Zeitrampe bis zum Gesamtlimit; danach gilt direkt normale AA.

## Einstellungen

Alle Namen beginnen mit `Auto-Adaptive:`.

| Einstellung | Default / Bereich |
|---|---|
| Z2 Flow-Anchored Startup Ramp | ALWAYS_OFF nach jedem ESP-Neustart |
| Z2 Buffer-Guided Mode | neu, ALWAYS_OFF nach jedem ESP-Neustart |
| Z2 Buffer Ready Margin | neu, 0,5 K; 0..3 K in 0,1-K-Schritten |
| Z2 Buffer Ready Duration | neu, 5 min; 1..30 min in Minutenschritten |
| Z2 Flow Ramp Initial Delta | neuer Default 1,0 K; bisheriger Bereich 0..5 K |
| Z2 Flow Ramp Step | unverändert 0,5 K; Bereich 0,1..2 K |
| Z2 Flow Ramp Interval | neuer Default 15 min; bisheriger Bereich 1..30 min |
| Z2 Startup Duration | neuer Default 60 min; bisheriger Bereich 5..60 min |
| Min. Heating Flow Temperature z2 | Default 24 °C; jetzt 20..40 °C, Schritt 0,5 K |

Alle Zahlen behalten `restore_value: true`. Bestehende gespeicherte Werte haben
Vorrang vor neuen Defaults. Buffer Mode allein ist ohne eingeschaltete
Flow-Anchored-Rampe wirkungslos. Keine neue Dashboard-Karte; Einstellungen sind
über ESPHome-Weboberfläche und native Home-Assistant-Integration verfügbar.

Erster Vergleichstest nach freigegebenem Update:

1. Neue Version und Entitäten kontrollieren; beide neuen Startup-Schalter zunächst OFF.
2. Initial Delta 1,0 K, Step 0,5 K, Interval 15 min, Gesamtdauer 60 min einstellen.
3. Für einen vergleichbaren ersten Lauf Z2-Minimum zunächst 24 °C beibehalten;
   anschließend kann 22 °C separat verglichen werden. Maximum 31 °C und vorhandene
   Guard-Toleranz 5 K entsprechend der bisherigen Anlagenkonfiguration beibehalten.
4. Buffer Margin 0,5 K, Ready Duration 5 min. Alte Return-/Soft-Starts und Guard
   Recovery für den Vergleich OFF; bisherige Defrost-/Short-Cycle-Einstellungen beibehalten.
5. Nach eigener Freigabe des Heiztests Flow-Anchored und Buffer Mode selbst ON.
   Tatsächlichen Raumwert weiter zuverlässig in den gewählten virtuellen Eingang liefern.
6. Nächsten natürlichen Verdichterstart abwarten; kein künstlicher Start.

## Diagnose, Tests und Build

Prefix `[Z2 FLOW RAMP]`: Phasenwechsel mit Grund, THW10 und Gültigkeit, optionale
Pufferführung/Fallback, Schwelle, erfüllte Bedingungsdauer, gespeicherter Start-
Rücklauf, Cap, normale AA vor Minimum, konfiguriertes Minimum, Raum/Soll,
Guard-Dauer und -Eingriffe, finale Anforderung und FTC-Readback. Frequenz nur
Diagnose. Kein THW10-Messwertalter und kein entsprechender Zähler im Log.

Eine gequeuete Anforderung ist keine Bestätigung, dass die Mitsubishi sie schon
übernommen hat. Der Writer protokolliert Queue/Unverändert und bisherigen
FTC-Readback getrennt. Tests ersetzen Gerät und Prozessführung; sie beweisen
keine tatsächliche Frequenz, COP-Verbesserung oder vollständige Pufferladung.

```powershell
python tests/run_return_guided_startup.py --zig <Pfad-zur-zig.exe>
esphome config ecodan-asgard-z2-en-flow-ramp.yaml
esphome compile ecodan-asgard-z2-en-flow-ramp.yaml
python tests/check_flow_ramp_build.py <ESPHome-build_path>
```

Alle sieben bisherigen Return-/Guard-/Thermostat-Testprogramme bleiben erhalten.
Die zwei vorhandenen Flow-Ramp-Programme prüfen nun die ausdrücklich ersetzten
HOLD-, Raum- und Guard-Regeln; neue THW10-/HANDOVER-Programme ergänzen sie.
Keine bestehende Schutzprüfung wurde zur Umgehung eines Fehlers deaktiviert.
Die Buildprüfung kontrolliert auch lokale C++-Quellidentität, Entity-Registrierung,
Z2-Mindestgrenze 20, unveränderte Z1-Grenze und beide Schalter ALWAYS_OFF.

Abschließende Host-Testserie: **615 Prüfungen in elf Programmen bestanden**:

| Programm | Prüfungen |
|---|---:|
| return_guided_startup_test | 91 |
| startup_guard_integration_test | 31 |
| startup_control_integration_test | 47 |
| startup_aa_integration_test | 15 |
| compressor_observer_test | 21 |
| guard_recovery_test | 86 |
| thermostat_restart_test | 6 |
| flow_anchored_startup_test | 69 |
| flow_ramp_integration_test | 57 |
| buffer_guided_startup_test | 130 |
| buffer_startup_integration_test | 62 |

Zusätzlich **64 Prüfungen** der erzeugten Konfiguration, Registrierungen und
Quellidentität erfolgreich. YAML des tatsächlichen lokalen Buildprofils gültig.

Buildprofil bleibt die zuletzt verwendete Asgard-Z2/ESP32-S3-Konfiguration mit
lokalen Komponenten und Paketen. UART-Pins, Board, Partitionierung, WLAN und
OTA-Methoden bleiben erhalten. Die generische Upstream-Release-Manifest-Entität
bleibt im speziellen Testprofil entfernt. Keine Secrets oder Binärdateien in Git.
Ältere Firmwarepakete bleiben erhalten.

OTA wird erst nach ausdrücklicher abschließender Freigabe ausgeführt. Vorher
Verdichter-/Defrost-OFF erneut prüfen. Nach Upload Version, Einstellungen,
Schalter OFF und Kommunikation kontrollieren. Kein automatisches Einschalten
der Pufferführung und kein erzwungener Heizstart.

## Erfolgreicher Ziel-Build

ESPHome 2026.9.1 / ESP-IDF 5.5.5, ESP32-S3; vollständiger Firmware-Build
erfolgreich, Exitcode 0. Build-Zeitstempel: `2026-11-08 16:27:17 +0100`,
Konfigurationshash `0x2f262856`.

- RAM: 155.531 von 341.760 Byte (45,5 %).
- Anwendung im Flash: 1.456.743 von 3.932.160 Byte (37,0 %).
- OTA- und Factory-Binärdatei erzeugt; beide enthalten die neue Versionskennung.
- Eine SDK-Makrokollision im ersten Build wurde behoben; ein zusätzlicher
  Host-Test prüft die Verträglichkeit mit dem vorhandenen SDK-Makro `EPS`.
- Verbleibende Build-Hinweise: Windows-Pfadlängenwarnung und eine Formatwarnung
  im unveränderten ESPHome-MQTT-Code. Der Ziel-Build ist dennoch erfolgreich.

## Geänderte Dateien

Insgesamt 32 Dateien; Binärdateien und lokale Build-/Prüfprotokolle liegen nur im
Firmwarepaket außerhalb des Git-Repositories.

```text
components/optimizer/auto_adaptive.cpp
components/optimizer/events.cpp
components/optimizer/flow_anchored_startup.cpp
components/optimizer/flow_anchored_startup.h
components/optimizer/guard_recovery.cpp
components/optimizer/optimizer.h
components/optimizer/optimizer_state.h
confs/auto-adaptive.yaml
confs/ecodan-labels-ca.yaml
confs/ecodan-labels-da.yaml
confs/ecodan-labels-de.yaml
confs/ecodan-labels-en.yaml
confs/ecodan-labels-es.yaml
confs/ecodan-labels-fi.yaml
confs/ecodan-labels-fr.yaml
confs/ecodan-labels-it.yaml
confs/ecodan-labels-nl.yaml
confs/ecodan-labels-no.yaml
confs/ecodan-labels-pl.yaml
confs/ecodan-labels-sk.yaml
confs/ecodan-labels-sl.yaml
confs/ecodan-labels-sv.yaml
docs/z2-buffer-guided-startup.md
docs/z2-flow-anchored-startup.md
ecodan-asgard-z2-en-flow-ramp.yaml
tests/buffer_guided_startup_test.cpp
tests/buffer_startup_integration_test.cpp
tests/check_flow_ramp_build.py
tests/flow_anchored_startup_test.cpp
tests/flow_ramp_integration_test.cpp
tests/run_return_guided_startup.py
tests/startup_control_integration_test.cpp
```

## Vorbereitetes OTA und Rückfallversion

Zielgerät: `ecodan-heatpump.local`, zuletzt `10.10.7.128`, ESPHome-OTA auf Port 3232.
Die erreichbare OTA-Schnittstelle wurde ausschließlich lesend geprüft. Die dort
installierte 07.01-Version bleibt bis zu einer ausdrücklichen Upload-Freigabe aktiv.

Erst nach Freigabe, erneuter Prüfung von Verdichter-OFF und Defrost-OFF, aus dem
entpackten neuen Firmwareverzeichnis ausführen:

```powershell
esphome upload ecodan-asgard-z2-en-flow-ramp.yaml --device 10.10.7.128 --ota-platform esphome --file ecodan-asgard-z2-en-2026-11-08.02-buffer-guided-startup.ota.bin
```

Für manuelles OTA ausschließlich die Datei mit Endung `.ota.bin` verwenden.
Die `.factory.bin` ist für eine USB-Erstinstallation bestimmt.

Nach einem genehmigten Update die Versionskennung `.08.02`, THW10, Z2-Minimum
und die drei neuen Buffer-Entitäten prüfen. Flow-Anchored und Buffer Mode müssen
nach dem Neustart OFF sein. Bestehende Zahlenwerte und Schutzschalter vergleichen,
Ecodan-Kommunikation kontrollieren und erst danach einen natürlichen Heiztakt testen.

Die bisherige 07.01-Firmware bleibt im alten Verzeichnis erhalten und liegt
zusätzlich unter `previous-2026-11-07.01/` im neuen Paket. Ihre SHA256-Summen wurden
mit der bisherigen Prüfsummendatei verglichen. Falls ein Rückwechsel erforderlich
wird, nach derselben OFF-/Defrost-Prüfung deren `.ota.bin` über denselben OTA-Pfad
hochladen. Zahlenwerte nach einem Rückwechsel prüfen, da gespeicherte Einstellungen
auf dem Gerät weiterbestehen. Es wurde kein OTA-Upload ausgeführt.
