import CoreBluetooth
import Foundation

enum TascaSonoraUUID {
    static let service  = CBUUID(string: "8f1d0000-6c2a-4b8e-9d3f-2a7c5e1b4d60")
    static let control  = CBUUID(string: "8f1d0001-6c2a-4b8e-9d3f-2a7c5e1b4d60")
    static let data     = CBUUID(string: "8f1d0002-6c2a-4b8e-9d3f-2a7c5e1b4d60")
    static let status   = CBUUID(string: "8f1d0003-6c2a-4b8e-9d3f-2a7c5e1b4d60")
    static let info     = CBUUID(string: "8f1d0004-6c2a-4b8e-9d3f-2a7c5e1b4d60")
    static let download = CBUUID(string: "8f1d0005-6c2a-4b8e-9d3f-2a7c5e1b4d60")
    static let catalog  = CBUUID(string: "8f1d0007-6c2a-4b8e-9d3f-2a7c5e1b4d60")
    static let diag     = CBUUID(string: "8f1d0008-6c2a-4b8e-9d3f-2a7c5e1b4d60")
}

/// Versione del firmware inclusa nell'app (file tasca_sonora.bin nelle risorse).
enum BundledFirmware {
    static let version = 6
    static var url: URL? { Bundle.main.url(forResource: "tasca_sonora", withExtension: "bin") }
}

/// Caratteristica INFO (formato compatibile con tutte le versioni del firmware).
struct DeviceInfo: Equatable {
    var mtu = 23, maxBytes = 0, sampleRate = 16000, volume = 70
    var hasSound = false, received = 0, recBytes = 0
    var battery: Int?, charging = false
    var fsTotal = 0, fsUsed = 0, soundBytes = 0, firmware = 1

    init?(_ raw: Data) {
        let d = [UInt8](raw)
        guard d.count >= 16 else { return nil }
        func u16(_ o: Int) -> Int { Int(d[o]) | Int(d[o + 1]) << 8 }
        func u32(_ o: Int) -> Int { u16(o) | u16(o + 2) << 16 }
        mtu = u16(0); maxBytes = u32(2); sampleRate = u32(6); volume = Int(d[10])
        hasSound = d[11] == 1; received = u32(12)
        if d.count >= 20 { recBytes = u32(16); firmware = 2 }
        if d.count >= 35 {
            battery = Int(d[20]); charging = d[21] == 1
            fsTotal = u32(22); fsUsed = u32(26); soundBytes = u32(30); firmware = Int(d[34])
        }
    }

    var maxSeconds: Double { Double(maxBytes) / 2 / Double(sampleRate) }
}

/// Catalogo completo del dispositivo (firmware v6 e successivi), letto come JSON.
struct Catalog: Decodable, Equatable {
    struct Slot: Decodable, Equatable, Identifiable {
        var index = 0
        let bytes: Int
        let name: String
        var id: Int { index }
        var isEmpty: Bool { bytes == 0 }
        var seconds: Double { Double(bytes) / 32000 }
        var title: String { name.isEmpty ? "Suono \(index + 1)" : name }

        init(from decoder: Decoder) throws {
            var c = try decoder.unkeyedContainer()
            bytes = try c.decode(Int.self)
            name = try c.decode(String.self)
        }
    }

    struct Recording: Decodable, Equatable, Identifiable {
        var index = 0
        let id: Int
        let epoch: Int
        let bytes: Int
        var seconds: Double { Double(bytes) / 32000 }
        var date: Date? { epoch > 0 ? Date(timeIntervalSince1970: TimeInterval(epoch)) : nil }

        init(from decoder: Decoder) throws {
            var c = try decoder.unkeyedContainer()
            id = try c.decode(Int.self)
            epoch = try c.decode(Int.self)
            bytes = try c.decode(Int.self)
        }
    }

    let fw: Int
    let n: String
    let b: Int
    let mv: Int
    let u: Int
    let v: Int
    let a: Int
    let fu: Int
    let ft: Int
    let lk: Int
    let t: Int
    let st: [Int]
    private let s: [Slot]
    private let r: [Recording]
    let sr: Int

    enum CodingKeys: String, CodingKey { case fw, n, b, mv, u, v, a, fu, ft, lk, t, st, s, r, sr }

    var name: String { n }
    var battery: Int { b }
    var millivolts: Int { mv }
    var usb: Bool { u == 1 }
    var volume: Int { v }
    var activeSlot: Int { a }
    var locked: Bool { lk == 1 }
    var memoryUsedKB: Int { fu }
    var memoryTotalKB: Int { ft }
    var slots: [Slot] { s.enumerated().map { var x = $0.element; x.index = $0.offset; return x } }
    var recordings: [Recording] { r.enumerated().map { var x = $0.element; x.index = $0.offset; return x } }
    var repeatOn: Bool { st.count > 0 && st[0] == 1 }
    var punchOn: Bool { audioMode > 0 }
    /// 0 naturale, 1 forte, 2 massimo
    var audioMode: Int { st.count > 1 ? st[1] : 0 }
    var autoLockOn: Bool { st.count > 2 && st[2] == 1 }
    var flipOn: Bool { st.count > 3 && st[3] == 1 }
    var sleepSeconds: Int { st.count > 4 ? st[4] : 45 }
}

enum DeviceSetting: UInt8 { case repeatPlayback = 1, punch = 2, autoLock = 3, flip = 4, sleep = 5 }

enum BLEError: LocalizedError {
    case notConnected, timeout, lostPackets, corrupted, tooLong, disconnected, oldFirmware, badFirmwareFile
    case device(UInt8)

    var errorDescription: String? {
        switch self {
        case .notConnected: return "Il dispositivo non è collegato."
        case .timeout: return "Il dispositivo non risponde. Avvicina il telefono e riprova."
        case .lostPackets: return "Alcuni pacchetti si sono persi per strada. Avvicina il telefono e riprova."
        case .corrupted: return "I dati sono arrivati incompleti. Riprova."
        case .tooLong: return "Il suono è troppo lungo per il dispositivo."
        case .disconnected: return "Il dispositivo si è scollegato."
        case .oldFirmware: return "Questa funzione richiede un firmware più recente sul dispositivo."
        case .badFirmwareFile: return "Questo file non sembra un firmware valido (.bin) per Tasca Sonora."
        case .device(let code):
            switch code {
            case 0x20: return "Dimensione non valida."
            case 0x21: return "I dati sono arrivati corrotti. Riprova."
            case 0x22: return "Errore di scrittura nella memoria del dispositivo."
            case 0x23: return "Il dispositivo non era pronto. Riprova."
            case 0x24: return "Il dispositivo è occupato (suona, registra o sta trasferendo). Riprova tra poco."
            case 0x25: return "Non c'è niente da scaricare."
            case 0x26: return "Memoria piena sul dispositivo: elimina qualche suono o registrazione."
            case 0x27: return "Aggiornamento non riuscito. Il dispositivo resta con il firmware di prima."
            case 0x28: return "Richiesta non valida."
            default: return "Il dispositivo ha risposto con l'errore \(code)."
            }
        }
    }
}

@MainActor
final class BLEManager: NSObject, ObservableObject {
    enum Phase: Equatable { case bluetoothOff, unauthorized, idle, scanning, connecting, connected }

    struct TaskState: Equatable {
        var title: String
        var progress: Double
    }

    @Published private(set) var phase: Phase = .idle
    @Published private(set) var info: DeviceInfo?
    @Published private(set) var catalog: Catalog?
    @Published private(set) var deviceName = "Tasca Sonora"
    @Published private(set) var task: TaskState?
    @Published private(set) var scanFailed = false

    var isConnected: Bool { phase == .connected }
    var supportsCatalog: Bool { chars[TascaSonoraUUID.catalog] != nil }
    var firmwareVersion: Int { catalog?.fw ?? info?.firmware ?? 0 }

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var chars: [CBUUID: CBCharacteristic] = [:]
    private var pendingNotifications = 0

    private var readConts: [CBUUID: CheckedContinuation<Data, Error>] = [:]
    private var writeCont: CheckedContinuation<Void, Error>?
    private var statusCont: CheckedContinuation<Data, Error>?
    private var statusToken = 0
    private var sendReadyCont: CheckedContinuation<Void, Never>?
    private var downloadBuffer = Data()
    private var downloadExpected = 0
    private var pollTimer: Timer?
    private var scanTimer: Timer?
    private var busy = false

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: .main)
    }

    // MARK: - Connessione

    func scan() {
        guard central.state == .poweredOn, phase == .idle else { return }
        scanFailed = false
        phase = .scanning
        central.scanForPeripherals(withServices: [TascaSonoraUUID.service])
        scanTimer?.invalidate()
        scanTimer = Timer.scheduledTimer(withTimeInterval: 20, repeats: false) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self, self.phase == .scanning else { return }
                self.central.stopScan()
                self.phase = .idle
                self.scanFailed = true
            }
        }
    }

    func cancelScan() {
        central.stopScan()
        scanTimer?.invalidate()
        if phase == .scanning { phase = .idle }
    }

    func disconnect() {
        if let peripheral { central.cancelPeripheralConnection(peripheral) }
    }

    // MARK: - Lettura stato

    @discardableResult
    func readInfo() async throws -> DeviceInfo {
        let raw = try await read(TascaSonoraUUID.info)
        guard let parsed = DeviceInfo(raw) else { throw BLEError.corrupted }
        info = parsed
        return parsed
    }

    @discardableResult
    func readCatalog() async throws -> Catalog {
        let raw = try await read(TascaSonoraUUID.catalog)
        do {
            let parsed = try JSONDecoder().decode(Catalog.self, from: raw)
            catalog = parsed
            deviceName = parsed.name
            return parsed
        } catch {
            throw BLEError.corrupted
        }
    }

    func readDiagnostics() async throws -> String {
        let raw = try await read(TascaSonoraUUID.diag)
        return String(decoding: raw, as: UTF8.self)
    }

    /// Aggiorna tutto quello che l'interfaccia mostra.
    func refresh() async {
        _ = try? await readInfo()
        if supportsCatalog { _ = try? await readCatalog() }
    }

    // MARK: - Comandi semplici

    func playSound() async throws { try await write([0x03]) }
    func stopPlayback() async throws { try requireFirmware(3); try await write([0x0D]) }
    func find() async throws { try requireFirmware(3); try await write([0x0C]) }

    func setVolume(_ value: Int) async throws {
        try await write([0x04, UInt8(max(0, min(100, value)))])
        info?.volume = value
    }

    /// Ora, fuso attuale e prossimo cambio tra ora legale e solare: il dispositivo cambia ora da solo.
    func syncTime() async throws {
        let now = Date()
        let zone = TimeZone.current
        var payload: [UInt8] = [0x13] + le32(Int(now.timeIntervalSince1970)) + le16(zone.secondsFromGMT(for: now) / 60)
        if let next = zone.nextDaylightSavingTimeTransition(after: now) {
            payload += le32(Int(next.timeIntervalSince1970)) + le16(zone.secondsFromGMT(for: next) / 60)
        }
        try check(try await send(payload, timeout: 5), expect: 0x15)
    }

    func selectSlot(_ slot: Int) async throws {
        try check(try await send([0x10, UInt8(slot)], timeout: 5), expect: 0x15)
        await refresh()
    }

    func playSlot(_ slot: Int) async throws { try await write([0x19, UInt8(slot)]) }

    func renameSlot(_ slot: Int, to name: String) async throws {
        try check(try await send([0x11, UInt8(slot)] + utf8Prefix(name, maxBytes: 16), timeout: 5), expect: 0x15)
        await refresh()
    }

    func deleteSlot(_ slot: Int) async throws {
        try check(try await send([0x1B, UInt8(slot)], timeout: 8), expect: 0x14)
        await refresh()
    }

    func renameDevice(_ name: String) async throws {
        try check(try await send([0x15] + utf8Prefix(name, maxBytes: 20), timeout: 5), expect: 0x15)
        await refresh()
    }

    func setSetting(_ setting: DeviceSetting, _ value: Int) async throws {
        try check(try await send([0x14, setting.rawValue, UInt8(clamping: value)], timeout: 5), expect: 0x15)
        await refresh()
    }

    func setLocked(_ locked: Bool) async throws {
        try check(try await send([0x18, locked ? 1 : 0], timeout: 5), expect: 0x15)
        await refresh()
    }

    private func selectRecording(_ index: Int) async throws {
        try check(try await send([0x12, UInt8(index)], timeout: 5), expect: 0x15)
    }

    func playRecording(_ index: Int = 0) async throws {
        if supportsCatalog { try await selectRecording(index) }
        try await write([0x08])
    }

    func deleteRecording(_ index: Int = 0) async throws {
        try requireFirmware(3)
        if supportsCatalog { try await selectRecording(index) }
        try check(try await send([0x0A], timeout: 8), expect: 0x14)
        await refresh()
    }

    func copyRecording(_ index: Int, toSlot slot: Int) async throws {
        if supportsCatalog {
            try await selectRecording(index)
            try check(try await send([0x1D, UInt8(slot)], timeout: 12), expect: 0x13)
        } else {
            try check(try await send([0x07], timeout: 12), expect: 0x13)
        }
        await refresh()
    }

    // MARK: - Invio di un suono

    func upload(_ input: Data, toSlot slot: Int? = nil) async throws {
        let pcm = Data(input)
        let current = try await readInfo()
        guard pcm.count <= current.maxBytes else { throw BLEError.tooLong }
        let start: [UInt8]
        if let slot, supportsCatalog { start = [0x1C, UInt8(slot)] + le32(pcm.count) }
        else { start = [0x01] + le32(pcm.count) }
        let title = slot.map { "Invio al suono \($0 + 1)" } ?? "Invio del suono"
        try await runTask(title) {
            try await self.stream(pcm, start: start, end: 0x02, okCode: 0x11)
        }
        await refresh()
    }

    // MARK: - Aggiornamento del firmware

    func updateFirmware(_ image: Data) async throws {
        try requireFirmware(6)
        // un'immagine ESP32 inizia con il byte magico 0xE9
        guard image.count > 100_000, image.first == 0xE9 else { throw BLEError.badFirmwareFile }
        try await runTask("Aggiornamento del firmware") {
            try await self.stream(Data(image), start: [0x16] + le32(image.count), end: 0x17, okCode: 0x16)
        }
    }

    /// Trasferimento generico: comando di inizio, dati senza risposta con verifica periodica, comando di fine con CRC.
    private func stream(_ payload: Data, start: [UInt8], end: UInt8, okCode: UInt8) async throws {
        guard let p = peripheral, let dataChar = chars[TascaSonoraUUID.data] else { throw BLEError.notConnected }
        do {
            try check(try await send(start, timeout: 8), expect: 0x10)
            let chunk = min(p.maximumWriteValueLength(for: .withoutResponse), 240)
            var offset = 0, packets = 0
            while offset < payload.count {
                await waitUntilCanSend(p)
                let endIndex = min(offset + chunk, payload.count)
                p.writeValue(payload.subdata(in: offset..<endIndex), for: dataChar, type: .withoutResponse)
                offset = endIndex
                packets += 1
                if packets % 48 == 0 || offset == payload.count {
                    guard try await readInfo().received == offset else { throw BLEError.lostPackets }
                    task?.progress = Double(offset) / Double(payload.count)
                }
            }
            let crc = Int(CRC32.checksum(payload))
            try check(try await send([end] + le32(crc), timeout: 20), expect: okCode)
        } catch {
            try? await write([0x05])
            throw error
        }
    }

    // MARK: - Download

    func downloadSlot(_ slot: Int) async throws -> Data {
        if supportsCatalog {
            let bytes = catalog?.slots[safe: slot]?.bytes ?? 0
            return try await download(command: [0x1A, UInt8(slot)], bytes: bytes, title: "Scarico il suono \(slot + 1)")
        }
        try requireFirmware(3)
        let bytes = try await readInfo().soundBytes
        return try await download(command: [0x0B], bytes: bytes, title: "Scarico il suono")
    }

    func downloadRecording(_ index: Int = 0) async throws -> Data {
        let bytes: Int
        if supportsCatalog {
            try await selectRecording(index)
            bytes = catalog?.recordings[safe: index]?.bytes ?? 0
        } else {
            bytes = try await readInfo().recBytes
        }
        return try await download(command: [0x06], bytes: bytes, title: "Scarico la registrazione")
    }

    private func download(command: [UInt8], bytes: Int, title: String) async throws -> Data {
        guard chars[TascaSonoraUUID.download] != nil else { throw BLEError.oldFirmware }
        guard bytes > 0 else { throw BLEError.device(0x25) }
        return try await runTask(title) {
            self.downloadBuffer = Data(capacity: bytes)
            self.downloadExpected = bytes
            defer { self.downloadExpected = 0 }
            let reply = try await self.send(command, timeout: max(30, Double(bytes) / 3000))
            try self.check(reply, expect: 0x12)
            let r = [UInt8](reply)
            guard r.count >= 5 else { throw BLEError.corrupted }
            let crc = UInt32(r[1]) | UInt32(r[2]) << 8 | UInt32(r[3]) << 16 | UInt32(r[4]) << 24
            guard self.downloadBuffer.count == bytes, CRC32.checksum(self.downloadBuffer) == crc else {
                throw BLEError.corrupted
            }
            return self.downloadBuffer
        }
    }

    // MARK: - Meccanica interna

    private func requireFirmware(_ version: Int) throws {
        guard firmwareVersion >= version else { throw BLEError.oldFirmware }
    }

    private func runTask<T>(_ title: String, _ body: () async throws -> T) async throws -> T {
        busy = true
        task = TaskState(title: title, progress: 0)
        defer { busy = false; task = nil }
        return try await body()
    }

    private func check(_ reply: Data, expect: UInt8) throws {
        guard let code = reply.first else { throw BLEError.corrupted }
        guard code == expect else { throw BLEError.device(code) }
    }

    private func read(_ uuid: CBUUID) async throws -> Data {
        guard let p = peripheral, let c = chars[uuid] else { throw BLEError.notConnected }
        while readConts[uuid] != nil { try await Task.sleep(nanoseconds: 30_000_000) }
        return try await withCheckedThrowingContinuation { cont in
            readConts[uuid] = cont
            p.readValue(for: c)
        }
    }

    private func write(_ bytes: [UInt8]) async throws {
        guard let p = peripheral, let c = chars[TascaSonoraUUID.control] else { throw BLEError.notConnected }
        while writeCont != nil { try await Task.sleep(nanoseconds: 20_000_000) }
        try await withCheckedThrowingContinuation { (cont: CheckedContinuation<Void, Error>) in
            writeCont = cont
            p.writeValue(Data(bytes), for: c, type: .withResponse)
        }
    }

    /// Manda un comando e aspetta la notifica di stato che il dispositivo invia in risposta.
    private func send(_ bytes: [UInt8], timeout: Double) async throws -> Data {
        while statusCont != nil { try await Task.sleep(nanoseconds: 20_000_000) }
        statusToken += 1
        let token = statusToken
        return try await withCheckedThrowingContinuation { cont in
            statusCont = cont
            Timer.scheduledTimer(withTimeInterval: timeout, repeats: false) { [weak self] _ in
                MainActor.assumeIsolated {
                    guard let self, self.statusToken == token, let c = self.statusCont else { return }
                    self.statusCont = nil
                    c.resume(throwing: BLEError.timeout)
                }
            }
            Task { @MainActor in
                do { try await self.write(bytes) } catch {
                    guard self.statusToken == token, let c = self.statusCont else { return }
                    self.statusCont = nil
                    c.resume(throwing: error)
                }
            }
        }
    }

    private func waitUntilCanSend(_ p: CBPeripheral) async {
        if p.canSendWriteWithoutResponse { return }
        await withCheckedContinuation { cont in sendReadyCont = cont }
    }

    private func finishConnecting() {
        guard let p = peripheral else { return }
        phase = .connected
        deviceName = p.name ?? "Tasca Sonora"
        Task {
            if self.supportsCatalog { try? await self.syncTime() }
            await self.refresh()
        }
        pollTimer?.invalidate()
        pollTimer = Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self, self.isConnected, !self.busy, self.readConts.isEmpty, self.statusCont == nil else { return }
                Task { await self.refresh() }
            }
        }
    }

    private func tearDown(_ error: Error) {
        pollTimer?.invalidate()
        for (_, c) in readConts { c.resume(throwing: error) }
        readConts = [:]
        writeCont?.resume(throwing: error); writeCont = nil
        statusCont?.resume(throwing: error); statusCont = nil
        sendReadyCont?.resume(); sendReadyCont = nil
        peripheral = nil
        chars = [:]
        info = nil
        catalog = nil
        downloadExpected = 0
        if central.state == .poweredOn { phase = .idle }
    }

    fileprivate func handleState(_ state: CBManagerState) {
        switch state {
        case .poweredOn:
            if phase == .bluetoothOff || phase == .unauthorized { phase = .idle }
        case .unauthorized:
            phase = .unauthorized
        default:
            tearDown(BLEError.disconnected)
            phase = .bluetoothOff
        }
    }

    fileprivate func handleDiscovered(_ p: CBPeripheral) {
        guard phase == .scanning else { return }
        central.stopScan()
        scanTimer?.invalidate()
        peripheral = p
        p.delegate = self
        phase = .connecting
        central.connect(p)
    }

    fileprivate func handleCharacteristics(_ list: [CBCharacteristic], of p: CBPeripheral) {
        for c in list { chars[c.uuid] = c }
        pendingNotifications = 0
        for uuid in [TascaSonoraUUID.status, TascaSonoraUUID.download] {
            if let c = chars[uuid] {
                pendingNotifications += 1
                p.setNotifyValue(true, for: c)
            }
        }
        if pendingNotifications == 0 { finishConnecting() }
    }

    fileprivate func handleNotificationEnabled() {
        pendingNotifications -= 1
        if pendingNotifications == 0 { finishConnecting() }
    }

    fileprivate func handleValue(_ uuid: CBUUID, _ value: Data?, _ error: Error?) {
        switch uuid {
        case TascaSonoraUUID.status:
            guard let value, let c = statusCont else { return }
            statusCont = nil
            c.resume(returning: value)
        case TascaSonoraUUID.download:
            guard let value, downloadExpected > 0 else { return }
            downloadBuffer.append(value)
            task?.progress = min(1, Double(downloadBuffer.count) / Double(downloadExpected))
            if downloadBuffer.count == downloadExpected { scheduleStatusFallback() }
        default:
            guard let c = readConts.removeValue(forKey: uuid) else { return }
            if let value, error == nil { c.resume(returning: value) } else { c.resume(throwing: error ?? BLEError.corrupted) }
        }
    }

    /// Se la notifica di fine download si perde per strada, la conferma resta leggibile sul dispositivo:
    /// dopo 1,5 s la si va a leggere (la risposta passa da handleValue come se fosse una notifica).
    private func scheduleStatusFallback() {
        let token = statusToken
        Timer.scheduledTimer(withTimeInterval: 1.5, repeats: false) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self, self.statusToken == token, self.statusCont != nil,
                      let p = self.peripheral, let c = self.chars[TascaSonoraUUID.status] else { return }
                p.readValue(for: c)
            }
        }
    }

    fileprivate func handleWrite(_ error: Error?) {
        guard let c = writeCont else { return }
        writeCont = nil
        if let error { c.resume(throwing: error) } else { c.resume() }
    }

    fileprivate func handleReadyToSend() {
        sendReadyCont?.resume()
        sendReadyCont = nil
    }
}

// MARK: - Delegati CoreBluetooth (chiamati sulla coda principale)

extension BLEManager: CBCentralManagerDelegate, CBPeripheralDelegate {
    nonisolated func centralManagerDidUpdateState(_ central: CBCentralManager) {
        let state = central.state
        MainActor.assumeIsolated { handleState(state) }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                                    advertisementData: [String: Any], rssi RSSI: NSNumber) {
        MainActor.assumeIsolated { handleDiscovered(peripheral) }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        peripheral.discoverServices([TascaSonoraUUID.service])
    }

    nonisolated func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        MainActor.assumeIsolated { tearDown(BLEError.disconnected) }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        MainActor.assumeIsolated { tearDown(BLEError.disconnected) }
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let service = peripheral.services?.first(where: { $0.uuid == TascaSonoraUUID.service }) else {
            MainActor.assumeIsolated { disconnect() }
            return
        }
        peripheral.discoverCharacteristics(nil, for: service)
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        let list = service.characteristics ?? []
        MainActor.assumeIsolated { handleCharacteristics(list, of: peripheral) }
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        MainActor.assumeIsolated { handleNotificationEnabled() }
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        let uuid = characteristic.uuid, value = characteristic.value
        MainActor.assumeIsolated { handleValue(uuid, value, error) }
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        MainActor.assumeIsolated { handleWrite(error) }
    }

    nonisolated func peripheralIsReady(toSendWriteWithoutResponse peripheral: CBPeripheral) {
        MainActor.assumeIsolated { handleReadyToSend() }
    }
}

// MARK: - Utilità

private func le16(_ value: Int) -> [UInt8] {
    let v = UInt16(bitPattern: Int16(clamping: value))
    return [UInt8(v & 0xFF), UInt8(v >> 8)]
}

private func le32(_ value: Int) -> [UInt8] {
    let v = UInt32(truncatingIfNeeded: value)
    return [UInt8(v & 0xFF), UInt8((v >> 8) & 0xFF), UInt8((v >> 16) & 0xFF), UInt8(v >> 24)]
}

/// Primi `maxBytes` byte UTF-8 di un testo, senza spezzare i caratteri.
private func utf8Prefix(_ text: String, maxBytes: Int) -> [UInt8] {
    var out: [UInt8] = []
    for ch in text.trimmingCharacters(in: .whitespacesAndNewlines) {
        let bytes = Array(String(ch).utf8)
        if out.count + bytes.count > maxBytes { break }
        out += bytes
    }
    return out
}

extension Array {
    subscript(safe index: Int) -> Element? { indices.contains(index) ? self[index] : nil }
}
