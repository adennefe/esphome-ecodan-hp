# Z2 Flow-Anchored Startup Ramp

Historischer Stand 07.01. Der aktuelle Nachfolger mit optionalem THW10,
entfernter Frequenz-HOLD-Logik und Z2-Minimum ab 20 °C ist in
[z2-buffer-guided-startup.md](z2-buffer-guided-startup.md) dokumentiert.

Firmware: `asgard-z2-en-2026-11-07.01-flow-anchored-ramp-1`.
Basis: `cbd7b48`, Branch `feat/guard-delta-tolerance`.
Die Versionskennung bezeichnet diesen Entwicklungsauftrag; der vollständige Build
wurde am 08.11.2026 ausgeführt.

## Verhalten

Die Funktion begrenzt ausschließlich die Z2-Vorlaufanforderung im unabhängigen
Z2-Heizbetrieb. Sie setzt weder Verdichterfrequenz noch Relais oder Raumzielwerte.
Ein Betrieb bei 26/16 Hz ist ein Versuchsziel, kein durch Software erzwungener Zustand.

Nur ein durch empfangene ACTIVE_TIME-Pakete bestätigter Übergang OFF -> ON startet
die Rampe. Eine erste ON-Beobachtung nach ESP-Neustart, ein Kommunikationsausfall
oder das Einschalten des neuen Schalters während eines laufenden Taktes starten
keine Rampe. Ein späterer echter OFF -> ON kann sie erneut starten.

Beim Start wird **Z2 Return**, nicht der primäre WP-Rücklauf, einmal gespeichert:

```text
base = Z2_return_at_start + initial_delta
cap = base + floor(elapsed_minutes / interval_minutes) * step
request_before_guards = min(normal_AA_before_minimum, cap)
```

Der laufende Rücklauf ändert weiterhin die normale AA-Berechnung, aber nicht die
gespeicherte Referenz der Rampe. Die vorhandene technische Untergrenze 20 °C,
Rundung auf 0,1 °C und das konfigurierte Z2-Maximum bleiben wirksam.

Beispiel: Z2-Rücklauf beim Start 20,5 °C, Initial Delta 2,0 K, Schritt 0,5 K,
Intervall 5 min, Dauer 45 min, normale AA jeweils oberhalb der Obergrenze:

| Laufzeit | Obergrenze vor Guards |
|---:|---:|
| 0 min | 22,5 °C |
| 5 min | 23,0 °C |
| 10 min | 23,5 °C |
| 15 min | 24,0 °C |
| 20 min | 24,5 °C |
| 25 min | 25,0 °C |
| 30 min | 25,5 °C |
| 35 min | 26,0 °C |
| 40 min | 26,5 °C |
| ab 45 min | normale AA; rechnerischer Rampenwert 27,0 °C wird nicht mehr begrenzt ausgegeben |

Bei normaler AA von 22,1 °C und Cap 23,0 °C wird 22,1 °C angefordert, bevor die
Guards eingreifen. Steigt der Rücklauf von 20,5 auf 23 °C, bleibt die Referenz 22,5 °C.

## HOLD und Freigabe

Die Frequenzqualifikation verwendet tatsächliche Frequenzmeldungen nach dem
Start, nicht die Anzahl der AA-Aufrufe. Fünf Minuten beobachtete Frequenz im
inklusiven Bereich 10..30 Hz und Raum >= Soll - 0,1 K erlauben HOLD.
Die Obergrenze zum Qualifikationszeitpunkt wird eingefroren. Ein Frequenzwert
außerhalb dieses Bereichs unterbricht die Qualifikation auch zwischen AA-Zyklen.
Raumwerte werden spätestens alle fünf Sekunden auf gestiegenen Bedarf geprüft.
Auch eine Frequenzlücke >120 s unterbricht die Qualifikation.

Beispiel: Cap 25,5 °C, Raum 18,9 °C / Soll 19,0 °C, stabile 26 oder 16 Hz:
HOLD hält Cap 25,5 °C. Normale AA 26,1 °C bleibt begrenzt; normale AA 25,0 °C
darf absenken. Guards dürfen den ausgegebenen Wert über den Cap anheben.

Bei Frequenz >30 Hz (auch <10 Hz/ungültig) oder Raum <Soll - 0,1 K endet HOLD.
Die Rampe setzt dann mit ihrer **absoluten Laufzeit seit Start** fort. Aufgelaufene
Stufen können dadurch sichtbar werden. Es wird keine zusätzliche Übergangsrampe
eingeführt, die benötigte Temperaturerhöhungen über die Maximaldauer hinaus blockiert.
Bei Raum <=Soll - 0,2 K endet die gesamte Begrenzung sofort.
Die maximale Startup-Dauer läuft während HOLD unverändert weiter.

## Schutz und Priorität

- Start und Betrieb erfordern aktive Z2-Heizanforderung, AA, unabhängige Zonen,
  die vorhandenen Fixed-Flow-Schreibvoraussetzungen und mindestens 10 l/min.
- WP-/Z2-Vorlauf, WP-/Z2-Rücklauf, Frequenz und Durchfluss müssen empfangen,
  endlich und höchstens 120 s alt sein. Wasserwerte müssen in 0..80 °C liegen.
  Raum-/Zieltemperatur und AA-/Min-/Max-Werte müssen gültig sein.
- DHW, Defrost, Defrost-Recovery, Post-DHW-Fenster, Lockout, ODIN-Übernahme,
  AA OFF, inaktive Z2, Verdichterstopp und fehlende Daten beenden/verhindern die Rampe.
- Die bestehenden Buffer- und Verdichterstopp-Guards werden nach der Rampe
  angewandt. Ihre Algorithmen wurden nicht abgeschwächt. Das Z2-Maximum bleibt stärker.
- Zwei aufeinanderfolgende tatsächliche **Anhebungen** durch den Stop-Guard,
  mindestens 30 s auseinander, geben die Begrenzung frei. Unveränderte Grenzwerte,
  doppelte Aufrufe und alleinige Buffer-Eingriffe zählen nicht als Stop-Anhebung.
  Ein Zyklus ohne Stop-Anhebung setzt den Zähler zurück.
- Der normale Z2-Mindestwert hat jetzt 24 °C als Neugeräte-Standard. Nur die
  begrenzte Startup-Phase darf ihn lokal unterschreiten. Gespeicherte Min-Werte
  werden nicht überschrieben; zum Test 24 °C selbst einstellen.
- Nach Freigabe gilt normale AA mit regulärem Minimum/Maximum und Guards, ohne
  weitere Haltephase unterhalb des Minimums. Die Übergabe nutzt denselben
  Schreibpfad und verwirft die alte private Predictive-Boost-Basis.

Auswahl pro echtem Verdichterstart:

1. Neuer Schalter ON: Flow-Anchored ist ausgewählt; alte Startrampen bleiben für
   diesen gesamten Verdichtertakt ausgesetzt, auch nach früher Freigabe oder wenn
   die neue Variante beim Start nicht berechtigt war. In diesem Fall gilt normale AA.
2. Neuer Schalter OFF: bisherige Return-Guided-Priorität und alte Soft-Starts gelten
   unverändert. Alle alten Schalter, Regler und Implementierungen bleiben erhalten.

Guard Recovery ist während der neuen aktiven Rampe ausgesetzt. Defrost Mitigation,
Short-Cycle Prevention, Z1 und DHW behalten ihre vorhandenen Aufgaben.
Der neue Service prüft alle 5 s und ruft AA höchstens alle 30 s während der Rampe
zusätzlich für Z2 auf; reguläre AA setzt denselben Zeitstempel. Der Predictive-Pfad
verwendet währenddessen denselben Writer. Die allgemeinen Z1- und Relay-Historien
werden von zusätzlichen Z2-Zyklen nicht fortgeschrieben.

## Einstellungen in Home Assistant / ESPHome

| ESPHome-ID | Name nach `Auto-Adaptive:` | Bereich / Standard |
|---|---|---|
| `z2_flow_anchored_startup` | Z2 Flow-Anchored Startup Ramp | ALWAYS_OFF bei jedem ESP-Neustart |
| `z2_flow_ramp_initial_delta` | Z2 Flow Ramp Initial Delta | 0..5 K / 0,1 K; Standard 2,0 K |
| `z2_flow_ramp_step` | Z2 Flow Ramp Step | 0,1..2 K / 0,1 K; Standard 0,5 K |
| `z2_flow_ramp_interval` | Z2 Flow Ramp Interval | 1..30 min / 1 min; Standard 5 min |
| `z2_startup_duration` (vorhanden) | Z2 Startup Duration | 5..60 min / 5 min; neuer Standard 45 min |

Die drei neuen Zahlen und die vorhandene Dauer speichern Benutzerwerte. Eine
gespeicherte Dauer von beispielsweise 15 min bleibt 15 min: für den Versuch
ausdrücklich 45 min einstellen. Der neue Schalter restauriert niemals ON.
Keine Boot-Automation schaltet die neue Regelung ein. HA-Automationen außerhalb
dieses Projektes werden nicht verändert. Sichtbar über native ESPHome-Integration
und Standard-Weboberfläche; keine neue Asgard-Dashboard-Karte. Der bisherige
`Z2 Startup Active`-Binärsensor bezeichnet weiterhin die Return-Guided-Funktion.

Erster Heiztest nach kontrolliertem Update:

- Zuerst neue Version und alle vier neuen Entitäten prüfen, neuer Schalter OFF.
- AA ON; Z2 Min 24 °C, gewohntes Maximum (hier 31 °C), Toleranz wie bisher (hier 5 K).
- Initial Delta 2,0 K, Step 0,5 K, Interval 5 min, Startup Duration 45 min.
- Return-Guided Startup, Z2 Soft Start, Target Delta Soft Start und Guard Recovery
  für den Vergleich OFF. Bestehende Schutzfunktionen nicht abschalten.
- Neuen Schalter selbst einschalten und den nächsten natürlichen Verdichterstart
  abwarten. Bei laufendem Verdichter wird nicht nachträglich gestartet.

## Diagnose und Prüfungen

`[Z2 FLOW RAMP]` protokolliert Startreferenz, Laufzeit, Frequenz, WP-/Z2-Temperaturen,
normale AA, Cap, Wert vor Guards, finale Anforderung, HOLD und Gründe. Guard-
Übersteuerung und Schreibanforderung werden separat protokolliert. Eine in die
Queue gestellte Anforderung ist keine Bestätigung des späteren FTC-Readbacks.

Neun C++-Programme: 414 Prüfungen, davon die bisherigen 297 unverändert weiter
ausgeführt. Zusätzliche Tests betreffen Start/Boot, Z2 statt WP als Referenz,
steigenden Rücklauf, Stufen/Grenzen, niedrigere AA, HOLD 26/16 Hz, kurze
Frequenzausreißer, Raumgrenzen, Mindestdurchfluss, beide Guards, Maximaltemperatur,
Defrost/DHW/Recovery/Lockout/ODIN, ungültige/veraltete Daten, Maximaldauer auch in
HOLD, Wiederanlauf, millis-Überlauf und Übergabe ohne weitere Startrampe.

```powershell
python tests/run_return_guided_startup.py --zig <Pfad-zur-zig.exe>
esphome config ecodan-asgard-z2-en-flow-ramp.yaml
esphome compile ecodan-asgard-z2-en-flow-ramp.yaml
python tests/check_flow_ramp_build.py <ESPHome-build_path>
```

Der zweite Prüfer kontrolliert erzeugte Entity-Registrierungen, ALWAYS_OFF,
Zahlenwerte, Optimizer-Zuordnung und Byteidentität aller lokalen optimizer/ecodan-
C++-Quellen im Build. Das sind Softwareprüfungen, kein Nachweis für Modulation,
Hydraulik, COP oder Dauerlauf auf der realen Anlage. Neue Hardware-Prüfung steht aus.

## Buildquellen und OTA

`ecodan-asgard-z2-en-flow-ramp.yaml` übernimmt die zuletzt verwendete Asgard-Z2-
Konfiguration: ESP32-S3 devkitc-1, 16 MB, vorhandene Partitionierung, UART RX38/TX39,
Proxy RX8/TX7, Z1/Z2 GPIO40/41 und bisherigen WLAN-/OTA-Pakete. Quellen und Pakete
sind lokal. Die generische, unbenutzte `ecodan-esphome.yaml` bleibt unverändert.
Im speziellen Testprofil wird ausschließlich die generische Upstream-Update-
Manifest-Entität entfernt; die vorhandenen drei OTA-Methoden bleiben erhalten.
So kann ein Upstream-Release nicht versehentlich diese Fork-Variante ersetzen.
Secrets und WLAN-Zugangsdaten werden nicht in Git aufgenommen oder neu gesetzt.

Für einen späteren Remote-Build alle Packages und external_components aus
`adennefe/esphome-ecodan-hp` auf **denselben vollständigen Commit** pinnen. Nicht
`gekkekoe@main` verwenden und nicht eigene Komponenten mit fremden Packages mischen.
Das lokale Firmwarepaket enthält zusätzlich eine Remote-Build-Vorlage mit dem
nach dem Push bekannten Commit; lokale Geheimnisse bleiben separat.

OTA-Ziel wurde lesend identifiziert: `ecodan-heatpump.local` / `10.10.7.128`,
ESPHome nativ TCP3232 erreichbar. Native Geräteinformation bestätigt ESP32-S3 und
die bisher installierte `asgard-z2-en-2026-10-31.01-startup-guard-4`, Buildzeit
31.10.2026 08:51:32 +0100. Die zugehörige alte OTA-Datei ist lokal erhalten.
Bei der Vorprüfung waren Kompressor/Defrost aus und Frequenz/Durchfluss null.
Dieser Zustand muss unmittelbar vor einem freigegebenen Upload erneut geprüft werden.

**Bis zur ausdrücklichen abschließenden Freigabe kein OTA-Upload.** Nach Freigabe
aus dem neuen Firmwareordner mit der dort gebauten Datei:

```powershell
esphome upload ecodan-asgard-z2-en-flow-ramp.yaml --device 10.10.7.128 --ota-platform esphome --file ecodan-asgard-z2-en-2026-11-07.01-flow-anchored-ramp.ota.bin
```

Alternativ die OTA-Datei über den vorhandenen Web-OTA-Dialog auswählen. Nicht die
Factory-Datei für OTA verwenden. Wiederherstellung: erhaltene 31.10-OTA-Datei per
gleichem Weg, sofern OTA erreichbar; dokumentiertes Factory-Image und USB bleiben
ein manueller Rückfallweg. Es erfolgt kein automatisches USB-Flashen.

Nach Freigabe/Upload Verbindung und tatsächliche neue Version prüfen, neue
Entitäten/Schalter OFF kontrollieren, Kommunikations-/Laufzeitlogs prüfen und
normalen Z2-Betrieb beobachten. Ohne Upload gibt es keine Nachkontrolle der neuen
Firmware auf der Hardware und keine Bestätigung der HA-Entity-Anzeige.

## Buildabschluss

- ESPHome 2026.9.1, ESP-IDF 5.5.5: vollständiger Build und abschließender Build
  erfolgreich, Exit-Code 0.
- Finaler Build: 08.11.2026 10:49:02 +0100, Config-Hash `0x077e0ddf`.
- RAM 155171 / 341760 Byte (45,4 %); Flash 1454307 / 3932160 Byte (37,0 %).
- 414 C++-Prüfungen und 47 zusätzliche Build-/Registrierungs-/Quellprüfungen bestanden.
- OTA-/Factory-Datei und SHA256SUMS werden außerhalb des Git-Repositories im
  separaten Ordner `esphome-ecodan-hp-2026-11-07.01` bereitgestellt.
- OTA nicht ausgeführt. Freigabe und Kontrolle des Betriebszustands stehen aus.

## Vollständige Dateiliste dieses Entwicklungsstandes

- components/optimizer/auto_adaptive.cpp
- components/optimizer/compressor_state.cpp
- components/optimizer/events.cpp
- components/optimizer/guard_recovery.cpp
- components/optimizer/optimizer.h
- components/optimizer/optimizer_state.h
- components/optimizer/prevention.cpp
- components/optimizer/utility.cpp
- confs/auto-adaptive.yaml
- confs/ecodan-labels-ca.yaml
- confs/ecodan-labels-da.yaml
- confs/ecodan-labels-de.yaml
- confs/ecodan-labels-en.yaml
- confs/ecodan-labels-es.yaml
- confs/ecodan-labels-fi.yaml
- confs/ecodan-labels-fr.yaml
- confs/ecodan-labels-it.yaml
- confs/ecodan-labels-nl.yaml
- confs/ecodan-labels-no.yaml
- confs/ecodan-labels-pl.yaml
- confs/ecodan-labels-sk.yaml
- confs/ecodan-labels-sl.yaml
- confs/ecodan-labels-sv.yaml
- tests/run_return_guided_startup.py
- tests/startup_control_integration_test.cpp
- tests/startup_guard_integration_test.cpp
- components/optimizer/flow_anchored_startup.cpp
- components/optimizer/flow_anchored_startup.h
- docs/z2-flow-anchored-startup.md
- ecodan-asgard-z2-en-flow-ramp.yaml
- tests/check_flow_ramp_build.py
- tests/flow_anchored_startup_test.cpp
- tests/flow_ramp_integration_test.cpp
