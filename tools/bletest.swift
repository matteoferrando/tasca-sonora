// Collaudo automatico del protocollo Bluetooth di Tasca Sonora v6, da riga di comando sul Mac.
// Uso: bletest [percorso firmware .bin per provare anche l'OTA]
import CoreBluetooth
import Foundation
setvbuf(stdout, nil, _IOLBF, 0)

let SVC = CBUUID(string: "8f1d0000-6c2a-4b8e-9d3f-2a7c5e1b4d60")
func U(_ n: Int) -> CBUUID { CBUUID(string: String(format: "8f1d%04x-6c2a-4b8e-9d3f-2a7c5e1b4d60", n)) }
let CTRL = U(1), DATA = U(2), STATUS = U(3), INFO = U(4), DL = U(5), CATALOG = U(7), DIAG = U(8)

func le32(_ v: Int) -> [UInt8] { let x = UInt32(truncatingIfNeeded: v); return [UInt8(x & 255), UInt8(x >> 8 & 255), UInt8(x >> 16 & 255), UInt8(x >> 24)] }
func le16(_ v: Int) -> [UInt8] { let u = UInt16(bitPattern: Int16(clamping: v)); return [UInt8(u & 255), UInt8(u >> 8)] }
/// Fuso del Mac adesso e dopo il prossimo cambio d'ora, come fa l'app
func tzPayload() -> [UInt8] {
    let z = TimeZone.current, now = Date()
    var p = le16(z.secondsFromGMT(for: now) / 60)
    if let n = z.nextDaylightSavingTimeTransition(after: now) { p += le32(Int(n.timeIntervalSince1970)) + le16(z.secondsFromGMT(for: n) / 60) }
    return p
}
func crc32(_ d: Data) -> UInt32 {
    var c: UInt32 = 0xFFFFFFFF
    for b in d { c ^= UInt32(b); for _ in 0..<8 { c = (c & 1) != 0 ? (c >> 1) ^ 0xEDB88320 : c >> 1 } }
    return c ^ 0xFFFFFFFF
}
let t0 = Date()
func log(_ s: String) { print(String(format: "[%6.1f] ", Date().timeIntervalSince(t0)) + s) }
var failures = 0
var fallbacks = 0
func check(_ ok: Bool, _ what: String) { log((ok ? "OK    " : "ERRORE ") + what); if !ok { failures += 1 } }

final class Tester: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    var cm: CBCentralManager!
    var p: CBPeripheral?
    var ch: [CBUUID: CBCharacteristic] = [:]
    var sem = DispatchSemaphore(value: 0)
    var lastStatus = Data(), lastRead = Data(), dlBuf = Data()
    let q = DispatchQueue(label: "ble")
    var connected = false

    override init() { super.init(); cm = CBCentralManager(delegate: self, queue: q) }

    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        if c.state == .poweredOn { log("cerco il dispositivo (tieni premuto il frontale 2 s)…"); c.scanForPeripherals(withServices: [SVC]) }
    }
    func centralManager(_ c: CBCentralManager, didDiscover per: CBPeripheral, advertisementData: [String: Any], rssi: NSNumber) {
        log("trovato \(per.name ?? "?") (\(rssi) dBm)"); c.stopScan(); p = per; per.delegate = self; c.connect(per)
    }
    func centralManager(_ c: CBCentralManager, didConnect per: CBPeripheral) { connected = true; per.discoverServices([SVC]) }
    func centralManager(_ c: CBCentralManager, didDisconnectPeripheral per: CBPeripheral, error: Error?) { connected = false; log("scollegato"); sem.signal() }
    func peripheral(_ per: CBPeripheral, didDiscoverServices e: Error?) { per.discoverCharacteristics(nil, for: per.services!.first!) }
    func peripheral(_ per: CBPeripheral, didDiscoverCharacteristicsFor s: CBService, error: Error?) {
        for c in s.characteristics ?? [] { ch[c.uuid] = c }
        per.setNotifyValue(true, for: ch[STATUS]!); per.setNotifyValue(true, for: ch[DL]!)
        q.asyncAfter(deadline: .now() + 1) { self.sem.signal() }
    }
    func peripheral(_ per: CBPeripheral, didUpdateValueFor c: CBCharacteristic, error: Error?) {
        guard let v = c.value else { return }
        if c.uuid == STATUS { log(String(format: "   <- stato %02x (%d byte) dl=%d", v.first ?? 0, v.count, dlBuf.count)); lastStatus = v; sem.signal() }
        else if c.uuid == DL { dlBuf.append(v) }
        else { lastRead = v; sem.signal() }
    }
    func peripheral(_ per: CBPeripheral, didWriteValueFor c: CBCharacteristic, error: Error?) {}

    func wait(_ s: Double) -> Bool { sem.wait(timeout: .now() + s) == .success }
    func read(_ u: CBUUID) -> Data { lastRead = Data(); p!.readValue(for: ch[u]!); _ = wait(5); return lastRead }
    func cmd(_ b: [UInt8], timeout: Double = 6) -> UInt8? {
        lastStatus = Data(); p!.writeValue(Data(b), for: ch[CTRL]!, type: .withResponse)
        return wait(timeout) ? lastStatus.first : nil
    }
    func catalog() -> [String: Any] { (try? JSONSerialization.jsonObject(with: read(CATALOG))) as? [String: Any] ?? [:] }
    func received() -> Int { let d = [UInt8](read(INFO)); return d.count >= 16 ? Int(d[12]) | Int(d[13]) << 8 | Int(d[14]) << 16 | Int(d[15]) << 24 : -1 }

    func stream(_ payload: Data, start: [UInt8], end: UInt8) -> UInt8? {
        guard cmd(start) == 0x10 else { return nil }
        let chunk = min(p!.maximumWriteValueLength(for: .withoutResponse), 240)
        var off = 0, n = 0
        while off < payload.count {
            while !p!.canSendWriteWithoutResponse { usleep(2000) }
            let e = min(off + chunk, payload.count)
            p!.writeValue(payload.subdata(in: off..<e), for: ch[DATA]!, type: .withoutResponse)
            off = e; n += 1
            if n % 48 == 0 { let r = received(); if r != off { log("persi pacchetti: \(r) != \(off)"); return nil } }
        }
        _ = received()
        return cmd([end] + le32(Int(crc32(payload))), timeout: 25)
    }

    func run(firmware: Data?) {
        guard wait(60) else { log("dispositivo non trovato"); exit(2) }
        let info = [UInt8](read(INFO))
        check(info.count == 35 && info[34] >= 6, "INFO 35 byte, firmware v\(info.count >= 35 ? Int(info[34]) : 0)")
        var cat = catalog()
        check(cat["fw"] as? Int ?? 0 >= 6, "catalogo JSON leggibile (\(read(CATALOG).count) byte)")
        check(cmd([0x13] + le32(Int(Date().timeIntervalSince1970)) + tzPayload()) == 0x15, "imposta ora e fuso (\(TimeZone.current.secondsFromGMT() / 60) min)")
        cat = catalog(); check(abs((cat["t"] as? Int ?? 0) - Int(Date().timeIntervalSince1970)) < 5, "ora riletta corretta")

        // suono di prova: 1,5 s di sweep a 16 kHz nel suono 4
        var pcm = Data()
        for i in 0..<24000 { let s = Int16(9000 * sin(2 * Double.pi * (300 + Double(i) / 30) * Double(i) / 16000)); withUnsafeBytes(of: s.littleEndian) { pcm.append(contentsOf: $0) } }
        let t = Date()
        check(stream(pcm, start: [0x1C, 3] + le32(pcm.count), end: 0x02) == 0x11, String(format: "invio 48 KB al suono 4 in %.1f s", Date().timeIntervalSince(t)))
        cat = catalog()
        let slots = cat["s"] as? [[Any]] ?? []
        check(slots.count == 6 && (slots[safe: 3]?.first as? Int) == pcm.count, "catalogo: suono 4 = \(pcm.count) byte")
        check(cmd([0x11, 3] + Array("Prova àè".utf8)) == 0x15, "rinomina suono 4")
        check(((catalog()["s"] as? [[Any]])?[safe: 3]?[safe: 1] as? String) == "Prova àè", "nome riletto con accenti")
        check(cmd([0x10, 3]) == 0x15 && (catalog()["a"] as? Int) == 3, "suono 4 attivo")
        dlBuf = Data()
        var st = cmd([0x1A, 3], timeout: 6)
        if st == nil && dlBuf.count == pcm.count {
            fallbacks += 1; log("   notifica di fine persa: rileggo lo stato")
            lastStatus = Data(); p!.readValue(for: ch[STATUS]!); st = wait(5) ? lastStatus.first : nil
        }
        check(st == 0x12 && dlBuf == pcm && lastStatus.count >= 5, "download suono 4 identico (\(dlBuf.count) byte)")
        check(cmd([0x14, 1, 1]) == 0x15 && ((catalog()["st"] as? [Int])?.first) == 1, "impostazione ripeti")
        check(cmd([0x14, 1, 0]) == 0x15, "ripeti spento")
        check(cmd([0x14, 5, 9]) == 0x15 && ((catalog()["st"] as? [Int])?[safe: 4]) == 45, "spegnimento 45 s")
        check(cmd([0x14, 5, 7]) == 0x28, "valore non valido rifiutato")
        check(cmd([0x1B, 3], timeout: 8) == 0x14 && ((catalog()["s"] as? [[Any]])?[safe: 3]?.first as? Int) == 0, "elimina suono 4")
        check(cmd([0x1C, 9] + le32(100)) == 0x28, "suono inesistente rifiutato")
        check(cmd([0x77]) == 0x28, "comando sconosciuto rifiutato")
        let diag = String(decoding: read(DIAG), as: UTF8.self)
        check(diag.contains("Tasca Sonora v"), "diagnostica: \(diag.split(separator: "\n").first ?? "")")
        if let fw = firmware {
            let t2 = Date()
            let r = stream(fw, start: [0x16] + le32(fw.count), end: 0x17)
            check(r == 0x16, String(format: "OTA %d KB in %.0f s", fw.count / 1024, Date().timeIntervalSince(t2)))
            _ = wait(10)
        }
        _ = cmd([0x10, 0])                       // rimette attivo il primo suono
        log("riletture dello stato: \(fallbacks)")
        log(failures == 0 ? "TUTTO OK" : "\(failures) ERRORI")
        exit(failures == 0 ? 0 : 1)
    }
}

extension Array { subscript(safe i: Int) -> Element? { indices.contains(i) ? self[i] : nil } }

let fw = CommandLine.arguments.count > 1 ? try? Data(contentsOf: URL(fileURLWithPath: CommandLine.arguments[1])) : nil
let tester = Tester()
DispatchQueue.global().async { tester.run(firmware: fw) }
RunLoop.main.run()
