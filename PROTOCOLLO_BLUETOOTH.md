# Tasca Sonora — protocollo Bluetooth (firmware v6)

Riferimento per chi scrive un'app che parla con il dispositivo. Tutto quello che c'è qui è implementato
in `firmware/tasca_sonora/tasca_sonora.ino` e usato dall'app iOS (`ios/TascaSonora/BLEManager.swift`)
e dal programma di collaudo (`tools/bletest.swift`), che sono i due esempi completi da cui partire.

## Collegamento

- Il dispositivo accetta connessioni **solo in modalità Bluetooth**: si attiva tenendo premuto il tasto
  frontale per 2 secondi (sullo schermo compare "Bluetooth"). Dopo un aggiornamento OTA si riattiva da sola.
  Fuori da questa modalità le connessioni vengono chiuse subito.
- La modalità resta attiva per **2 minuti**, rinnovati da ogni lettura di INFO o CATALOG e da ogni comando.
  Un'app collegata che legge INFO ogni pochi secondi tiene il dispositivo sveglio; quando smette, dopo
  2 minuti il dispositivo chiude il Bluetooth e torna a dormire.
- Nome pubblicizzato: quello impostato dall'utente (predefinito `Tasca Sonora`), nel pacchetto di risposta
  alla scansione. Cercare per **UUID del servizio**, non per nome.
- MTU richiesto dal dispositivo: 247. Su iOS si ottengono tipicamente 185 byte.
- Nessun abbinamento/PIN: chiunque sia vicino durante i 2 minuti può collegarsi.

## Servizio e caratteristiche

Servizio: `8f1d0000-6c2a-4b8e-9d3f-2a7c5e1b4d60`

| UUID | Nome | Proprietà | Uso |
|---|---|---|---|
| `8f1d0001-…` | CTRL | write (con risposta) | comandi |
| `8f1d0002-…` | DATA | write without response | dati in arrivo (suoni, firmware) |
| `8f1d0003-…` | STATUS | read, notify | risposta ai comandi; l'ultimo valore resta leggibile |
| `8f1d0004-…` | INFO | read | stato binario (35 byte), compatibile con tutte le versioni |
| `8f1d0005-…` | DL | notify | dati in uscita (scaricamenti) |
| `8f1d0007-…` | CATALOG | read (fino a 512 byte) | stato completo in JSON (v6) |
| `8f1d0008-…` | DIAG | read (fino a 500 byte) | testo di diagnostica |

Tutti gli UUID hanno lo stesso suffisso `-6c2a-4b8e-9d3f-2a7c5e1b4d60`. Attivare le notifiche su STATUS e DL
subito dopo la scoperta delle caratteristiche. Se CATALOG non c'è, il firmware è precedente alla v6.

Tutti i numeri sono **little-endian**.

## Formato audio

PCM **16 bit con segno, mono, 16 000 Hz**, little-endian, senza intestazione (niente WAV).
Massimo 960 000 byte (30 s) per suono e per registrazione. La lunghezza deve essere pari.

## Checksum

CRC-32 standard IEEE (lo stesso di zlib / `crc32` di Python): polinomio riflesso `0xEDB88320`,
valore iniziale `0xFFFFFFFF`, XOR finale `0xFFFFFFFF`. Controllo: `crc32("123456789") = 0xCBF43926`.

## INFO (35 byte)

| Offset | Tipo | Campo |
|---|---|---|
| 0 | u16 | MTU negoziato |
| 2 | u32 | byte massimi per suono (960000) |
| 6 | u32 | frequenza di campionamento (16000) |
| 10 | u8 | volume 0-100 |
| 11 | u8 | 1 se il suono attivo esiste |
| 12 | u32 | **byte ricevuti** nel trasferimento in corso (serve al controllo di flusso) |
| 16 | u32 | byte della registrazione selezionata |
| 20 | u8 | batteria % |
| 21 | u8 | 1 se in carica |
| 22 | u32 | memoria totale (byte) |
| 26 | u32 | memoria usata (byte) |
| 30 | u32 | byte del suono attivo |
| 34 | u8 | versione del firmware |

## CATALOG (JSON, v6)

Esempio:

```json
{"fw":6,"n":"Tasca Sonora","b":84,"mv":3950,"u":0,"v":100,"a":0,"fu":184,"ft":4480,"lk":0,
 "t":1791021890,"st":[0,2,0,0,45],
 "s":[[160000,"Classe"],[0,""],[0,""],[0,""],[0,""],[0,""]],
 "r":[[7,1791020000,64000],[6,0,10880]],"sr":0}
```

| Chiave | Significato |
|---|---|
| `fw` | versione firmware |
| `n` | nome del dispositivo |
| `b`, `mv` | batteria in % e in millivolt |
| `u` | 1 se alimentato da USB |
| `v` | volume 0-100 |
| `a` | indice del suono attivo (0-5), quello del tasto frontale |
| `fu`, `ft` | memoria usata e totale in KB |
| `lk` | 1 se i tasti sono bloccati |
| `t` | ora del dispositivo (epoch UTC), 0 se non impostata |
| `st` | impostazioni: `[ripeti, audio, blocco_auto, schermo_capovolto, spegnimento_s]`; audio: 0 naturale, 1 forte, 2 massimo |
| `s` | 6 suoni: `[byte, nome]`; 0 byte = vuoto; nome vuoto = "Suono N" |
| `r` | registrazioni, **la più recente per prima**: `[id, epoch (0 = data ignota), byte]`, al massimo 6 |
| `sr` | indice della registrazione selezionata |

I nomi sono UTF-8; sul dispositivo le lettere accentate vengono mostrate senza accento.

## Comandi (scritti su CTRL)

Il primo byte è il codice del comando. Quasi tutti rispondono con una notifica su STATUS
(primo byte = codice di stato, eventuali byte seguenti = dati).

| Codice | Comando | Parametri | Risposta |
|---|---|---|---|
| 0x01 | invia suono al suono attivo | u32 dimensione | 0x10 pronto |
| 0x1C | invia suono a un suono | u8 indice, u32 dimensione | 0x10 pronto |
| 0x02 | fine invio | u32 CRC dei dati | 0x11 salvato |
| 0x05 | annulla trasferimento | — | nessuna |
| 0x03 | suona il suono attivo | — | nessuna |
| 0x19 | suona un suono (anteprima) | u8 indice | nessuna |
| 0x08 | suona la registrazione selezionata | — | nessuna |
| 0x0D | ferma | — | nessuna |
| 0x0C | fai squillare (ritrova il dispositivo) | — | nessuna |
| 0x04 | volume | u8 0-100 | nessuna |
| 0x10 | scegli il suono attivo | u8 indice | 0x15 ok, a cambio avvenuto |
| 0x11 | nome di un suono | u8 indice, UTF-8 (max 16 byte) | 0x15 |
| 0x12 | seleziona registrazione | u8 indice (0 = più recente) | 0x15 |
| 0x0B | scarica il suono attivo | — | dati su DL, poi 0x12 + u32 CRC |
| 0x1A | scarica un suono | u8 indice | dati su DL, poi 0x12 + u32 CRC |
| 0x06 | scarica la registrazione selezionata | — | dati su DL, poi 0x12 + u32 CRC |
| 0x07 | copia la registrazione selezionata nel suono attivo | — | 0x13 copiato |
| 0x1D | copia la registrazione selezionata in un suono | u8 indice | 0x13 copiato |
| 0x09 | elimina il suono attivo | — | 0x14 eliminato |
| 0x1B | elimina un suono (anche il nome) | u8 indice | 0x14 eliminato |
| 0x0A | elimina la registrazione selezionata | — | 0x14 eliminato |
| 0x13 | imposta l'ora | u32 epoch UTC, i16 fuso in minuti, [u32 prossimo cambio d'ora UTC, i16 fuso dopo il cambio] | 0x15 |
| 0x14 | impostazione | u8 chiave, u8 valore (vedi sotto) | 0x15 |
| 0x15 | nome del dispositivo | UTF-8 (max 20 byte) | 0x15 |
| 0x18 | blocco tasti | u8 0/1 | 0x15 |
| 0x16 | inizio aggiornamento firmware | u32 dimensione | 0x10 pronto |
| 0x17 | fine aggiornamento | u32 CRC | 0x16, poi riavvio |

Impostazioni (comando 0x14):

| Chiave | Impostazione | Valori |
|---|---|---|
| 1 | ripeti il suono | 0/1 |
| 2 | audio | 0 naturale, 1 forte, 2 massimo |
| 3 | blocco tasti automatico a schermo spento | 0/1 |
| 4 | schermo capovolto | 0/1 |
| 5 | spegnimento dopo | valore × 5 secondi; ammessi 20, 45, 90, 180, 300 s (cioè 4, 9, 18, 36, 60) |

Per suonare, scaricare, copiare o eliminare una registrazione diversa dalla più recente:
prima `0x12` con l'indice, poi il comando.

## Codici di stato

| Codice | Significato |
|---|---|
| 0x10 | pronto a ricevere |
| 0x11 | suono salvato |
| 0x12 | scaricamento completato (+ u32 CRC) |
| 0x13 | registrazione copiata |
| 0x14 | eliminato |
| 0x15 | ok |
| 0x16 | firmware installato (riavvio tra 1,5 s) |
| 0x20 | dimensione non valida (zero, dispari o oltre il massimo) |
| 0x21 | CRC sbagliato |
| 0x22 | errore di scrittura in memoria |
| 0x23 | stato non valido (es. "fine" senza "inizio") |
| 0x24 | occupato (sta suonando, registrando o trasferendo) |
| 0x25 | niente da scaricare / registrazione inesistente |
| 0x26 | memoria piena |
| 0x27 | aggiornamento fallito (resta il firmware precedente) |
| 0x28 | comando o parametro non valido |

## Flussi

### Inviare un suono

1. `0x1C, indice, u32 dimensione` (oppure `0x01, u32 dimensione` per il suono attivo) e aspettare `0x10`.
2. Scrivere i dati su DATA **senza risposta**, a pezzi di `min(240, MTU - 3)` byte.
3. **Controllo di flusso**: ogni ~48 pezzi leggere INFO e verificare che `byte ricevuti` (offset 12)
   sia uguale ai byte inviati. Se non coincide, alcuni pacchetti si sono persi: mandare `0x05`
   e ricominciare. Su iOS rispettare `canSendWriteWithoutResponse` / `peripheralIsReady(toSendWriteWithoutResponse:)`.
4. `0x02, u32 CRC di tutti i dati` e aspettare `0x11` (fino a ~20 s: il dispositivo scrive in memoria).

Velocità tipica da iPhone/Mac: circa 48 KB al secondo.

### Scaricare un suono o una registrazione

1. Attivare le notifiche su DL, svuotare il buffer di ricezione.
2. Mandare il comando (`0x1A, indice`, oppure `0x12, indice` e poi `0x06` per una registrazione).
3. Accumulare le notifiche DL fino alla dimensione attesa (dal CATALOG).
4. Arriva `0x12 + u32 CRC` su STATUS: confrontare con il CRC dei dati ricevuti.

Importante: in rari casi la notifica finale su STATUS può perdersi dopo la raffica di dati. Se tutti i byte
sono arrivati ma lo stato non arriva entro 1,5 s, **leggere direttamente STATUS**: contiene ancora
`0x12 + CRC`. L'app iOS fa così (`scheduleStatusFallback`).

### Aggiornare il firmware (OTA)

1. Il file è il binario dell'applicazione (`tasca_sonora.ino.bin`, inizia con il byte `0xE9`), non il file
   "merged".
2. `0x16, u32 dimensione` → `0x10`; dati su DATA come per un suono, con lo stesso controllo di flusso.
3. `0x17, u32 CRC` → `0x16`. Il dispositivo si riavvia dopo 1,5 s dall'altra partizione e riapre
   il Bluetooth da solo: ricollegarsi dopo qualche secondo.
4. Se qualcosa va storto il dispositivo resta con il firmware precedente (due partizioni).

Circa 20-30 secondi per ~900 KB.

### Ora

Mandare `0x13` a ogni connessione. Con i 6 byte finali opzionali (prossimo cambio tra ora legale e solare
e fuso successivo) il dispositivo cambia ora da solo anche senza ricollegarsi. In Swift:
`TimeZone.current.nextDaylightSavingTimeTransition(after:)` e `secondsFromGMT(for:)`.

## Suggerimenti pratici

- Serializzare i comandi: uno alla volta, aspettando la risposta su STATUS prima del successivo.
- Timeout ragionevoli: 5 s per i comandi semplici, 20 s per fine invio e fine OTA, almeno 30 s per gli scaricamenti.
- Dopo ogni modifica rileggere CATALOG per aggiornare l'interfaccia.
- Leggere INFO + CATALOG ogni ~5 s mentre l'app è in primo piano (tiene sveglio il dispositivo).
- Il JSON del CATALOG supera la MTU: la lettura avviene in più pezzi, ma il dispositivo genera il valore
  una sola volta per lettura, quindi arriva sempre coerente (iOS e Android gestiscono le letture lunghe da soli).
