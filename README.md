# Tasca Sonora

Portachiavi sonoro basato su **M5Stack StickS3** (ESP32-S3, schermo 240×135, altoparlante 1 W,
microfono, batteria 250 mAh): premi un tasto e suona un suono caricato dal telefono via Bluetooth,
tieni premuto l'altro e registra.

Questo repository contiene il firmware (v6), l'app iOS (v2) e gli strumenti di collaudo.

## Contenuto

```
firmware/tasca_sonora/        sorgente Arduino (tasca_sonora.ino + partitions.csv)
firmware/bin/                 binari già compilati, pronti da installare
ios/                          app iOS (SwiftUI, iOS 17+), progetto XcodeGen
tools/bletest.swift           collaudo automatico del protocollo da riga di comando (macOS)
web/tasca_sonora.html         pagina web di controllo (Web Bluetooth, funzioni di base)
PROTOCOLLO_BLUETOOTH.md       il protocollo completo: servizi, comandi, formati, flussi
```

**Se stai scrivendo un'app tua, parti da `PROTOCOLLO_BLUETOOTH.md`**, poi guarda
`ios/TascaSonora/BLEManager.swift` (implementazione completa) e `tools/bletest.swift` (la più compatta).

## Cosa fa il dispositivo

| | Tasto frontale | Tasto laterale |
|---|---|---|
| Pressione breve | suona il suono attivo (di nuovo = ferma) | riascolta l'ultima registrazione |
| Tieni premuto | 2 s: accende/spegne il Bluetooth | registra finché lo tieni (max 30 s) |
| Doppio clic | — | apre il menu (laterale = pagina, frontale = cambia, tieni frontale 1 s = esci) |

- **Blocco tasti**: clic sul tasto di accensione, oppure frontale + laterale insieme per 1 s.
- **Tasto di accensione**: premi = accende, doppio clic = spegne.
- **6 suoni con nome**; **ultime 6 registrazioni** con data e ora.
- **Menu**: suono attivo, volume, ripeti, audio (naturale / forte / massimo), blocco automatico,
  schermo capovolto, tempo di spegnimento, informazioni.
- **Audio "forte/massimo"**: passa-alto, rinforzo di presenza, livellatore e saturazione morbida
  calcolati al momento di suonare (i file restano originali). Circa +11 / +14 dB percepiti.
- **Risparmio energetico**: deep sleep dopo il tempo impostato (a batteria); prima di dormire
  spegne uscita 5 V, codec audio e sensore di movimento.
- **Protezione dai blackout a batteria**: il tetto del volume si adatta alla tensione della batteria
  durante la riproduzione.
- **Aggiornamento firmware via Bluetooth** (due partizioni: se fallisce resta la versione precedente).
- Orologio con cambio automatico ora legale/solare, diario degli avvii per la diagnostica.

## Firmware

### Compilare

- Arduino IDE 2.x o arduino-cli, core **esp32 by Espressif 3.3.12**
- Librerie: **M5Unified 0.2.23**, **M5GFX 0.2.30**, **M5PM1 1.0.7**, **NimBLE-Arduino 2.5.1**
- Scheda: **ESP32S3 Dev Module**, Flash 8MB, PSRAM **OPI**, USB CDC On Boot **Enabled**
- Il file `partitions.csv` nella cartella dello sketch viene usato automaticamente:
  due partizioni app da 1,75 MB (OTA) e ~4,4 MB LittleFS per i suoni.

```
arduino-cli compile --fqbn "esp32:esp32:esp32s3:FlashSize=8M,PSRAM=opi,PartitionScheme=default_8MB,CDCOnBoot=cdc" firmware/tasca_sonora
arduino-cli upload -p /dev/cu.usbmodemXXXX --fqbn "esp32:esp32:esp32s3:FlashSize=8M,PSRAM=opi,PartitionScheme=default_8MB,CDCOnBoot=cdc" firmware/tasca_sonora
```

### Installare i binari già compilati (senza Arduino)

Con esptool (`pip install esptool`), dispositivo collegato via USB:

```
esptool --chip esp32s3 -p /dev/cu.usbmodemXXXX -b 921600 write-flash \
  0x0 firmware/bin/tasca_sonora.ino.bootloader.bin \
  0x8000 firmware/bin/tasca_sonora.ino.partitions.bin \
  0xe000 firmware/bin/boot_app0.bin \
  0x10000 firmware/bin/tasca_sonora.ino.bin
```

Oppure tutto in un colpo (cancella anche i suoni salvati):
`esptool --chip esp32s3 -p … write-flash 0x0 firmware/bin/tasca_sonora.ino.merged.bin`

Attenzione: la **prima installazione** su un dispositivo con firmware di fabbrica o con una versione
precedente alla v6 cambia la tabella delle partizioni, quindi la memoria dei suoni viene formattata.
Da lì in poi gli aggiornamenti (USB o Bluetooth) conservano suoni e impostazioni.

`tasca_sonora.ino.bin` è anche il file da usare per l'aggiornamento via Bluetooth.

### Console USB (115200 baud)

Utile per collaudo e diagnostica senza toccare il dispositivo. Ogni carattere è un comando,
**quindi non inviare testo qualsiasi**: `h` elenco comandi, `i` stato, `c` catalogo JSON, `d` diagnostica,
`P` registri di alimentazione, `V` tetto del volume a batteria, `1`/`2` clic simulati sui tasti,
`B` tieni il frontale, `R` registra, `L` blocco, `b` Bluetooth, `m`/`n`/`v` menu, `p` suona, `s` ferma,
`+`/`-` volume, `z` deep sleep.

## App iOS

Richiede Xcode 16+ e [XcodeGen](https://github.com/yonaskolb/XcodeGen) (`brew install xcodegen`).

1. In `ios/project.yml` imposta `DEVELOPMENT_TEAM` con il tuo Team ID e cambia
   `bundleIdPrefix` / `PRODUCT_BUNDLE_IDENTIFIER` con un identificativo tuo.
2. `cd ios && xcodegen generate && open TascaSonora.xcodeproj`
3. Esegui su un iPhone vero (il Bluetooth non funziona nel simulatore).

L'app include `ios/Firmware/tasca_sonora.bin` per l'aggiornamento via Bluetooth
(Dispositivo › Aggiorna il firmware). Se ricompili il firmware, sostituisci quel file.

Schede: **Dispositivo** (stato, nome, volume, impostazioni, aggiornamento, diagnostica), **Suoni** (i 6 suoni:
carica da file o dal microfono con taglio e anteprima, rinomina, scarica, elimina), **Registrazioni**
(scarica, ascolta, copia in un suono, elimina), **Archivio** (file salvati sul telefono, anche nell'app File).

## Strumenti

- `tools/bletest.swift`: collaudo automatico di tutto il protocollo dal Mac (invio, scaricamento, nomi,
  impostazioni, errori, opzionalmente OTA). Mettere prima il dispositivo in modalità Bluetooth.
  Attenzione: la prova usa il suono 4 (lo carica, lo rinomina e lo elimina) e alla fine rimette attivo il suono 1.
  ```
  cd tools
  swiftc -O bletest.swift -o bletest -Xlinker -sectcreate -Xlinker __TEXT -Xlinker __info_plist -Xlinker Info.plist
  codesign -s - -f bletest
  ./bletest                                   # solo protocollo
  ./bletest ../firmware/bin/tasca_sonora.ino.bin   # anche aggiornamento OTA
  ```
- `web/tasca_sonora.html`: pagina per Chrome desktop o Android (Web Bluetooth). Scritta per le versioni
  precedenti: con la v6 funzionano le funzioni di base sul suono attivo, non i 6 suoni né le impostazioni.

## Note sull'hardware imparate strada facendo

- Il chip di alimentazione **M5PM1** ha un watchdog lasciato attivo dal firmware di fabbrica:
  va spento all'avvio (registro 0x0A = 0), altrimenti riavvia il dispositivo.
- Dopo ogni reset il PM1 riaccende l'uscita 5 V (`BOOST_EN`, registro 0x06 bit 3) che alimenta
  l'amplificatore: va spenta esplicitamente, altrimenti consuma anche durante il deep sleep.
- La libreria spegne il codec ES8311 (I2C 0x18) solo per il microfono: per l'altoparlante il DAC resta
  alimentato, il firmware lo spegne a mano prima di dormire.
- La percentuale batteria della libreria è lineare (3,30-4,15 V) e molto ottimista sotto i 3,8 V:
  il firmware usa una curva LiPo reale.
- A volume pieno la batteria da 250 mAh può non reggere i picchi dell'altoparlante: da qui la protezione
  adattiva del volume.
- Tasti: frontale GPIO 11, laterale GPIO 12 (attivi bassi); risveglio dal deep sleep con EXT1.

## Licenza

Rilasciato con licenza [MIT](LICENSE): puoi usarlo, modificarlo e ridistribuirlo liberamente,
mantenendo l'avviso di copyright.
