import SwiftUI
import UniformTypeIdentifiers

struct DeviceView: View {
    @EnvironmentObject private var ble: BLEManager
    @EnvironmentObject private var feedback: Feedback
    @State private var volume: Double = 70
    @State private var renaming = false
    @State private var newName = ""
    @State private var firmwareSheet = false

    private static let sleepChoices = [20, 45, 90, 180, 300]

    var body: some View {
        NavigationStack {
            List {
                Section { ConnectionCard() }

                if ble.isConnected {
                    if let cat = ble.catalog {
                        statusSection(cat)
                        volumeSection
                        settingsSection(cat)
                        actionsSection(cat)
                    } else if let info = ble.info {
                        legacyStatus(info)
                        volumeSection
                        Section { OldFirmwareHint() }
                    }
                    firmwareSection
                }
            }
            .navigationTitle(ble.catalog?.name ?? "Tasca Sonora")
            .refreshable { await ble.refresh() }
            .onChange(of: ble.catalog?.volume ?? ble.info?.volume) { _, newValue in
                if let newValue { volume = Double(newValue) }
            }
            .alert("Nome del dispositivo", isPresented: $renaming) {
                TextField("Tasca Sonora", text: $newName)
                Button("Annulla", role: .cancel) {}
                Button("Salva") {
                    let name = newName
                    feedback.run(success: "Nome aggiornato. Lo vedrai così la prossima volta che colleghi il dispositivo.") {
                        try await ble.renameDevice(name)
                    }
                }
            } message: {
                Text("Massimo 20 caratteri. Compare sullo schermo e nella ricerca Bluetooth.")
            }
            .sheet(isPresented: $firmwareSheet) { FirmwareSheet() }
        }
    }

    // MARK: Sezioni

    private func statusSection(_ cat: Catalog) -> some View {
        Section("Stato") {
            Button {
                newName = cat.name
                renaming = true
            } label: {
                LabeledContent("Nome") {
                    HStack(spacing: 6) {
                        Text(cat.name)
                        Image(systemName: "pencil").font(.caption)
                    }
                }
            }
            .foregroundStyle(.primary)
            LabeledContent("Batteria") {
                Label("\(cat.battery)%" + (cat.usb ? (cat.battery < 100 ? " · in carica" : " · USB") : ""),
                      systemImage: batteryIcon(cat.battery, charging: cat.usb && cat.battery < 100))
                    .foregroundStyle(cat.battery <= 10 && !cat.usb ? .red : .secondary)
            }
            VStack(alignment: .leading, spacing: 6) {
                LabeledContent("Memoria", value: "\(Format.bytes(cat.memoryUsedKB * 1024)) di \(Format.bytes(cat.memoryTotalKB * 1024))")
                ProgressView(value: Double(cat.memoryUsedKB), total: Double(max(cat.memoryTotalKB, 1)))
            }
            let filled = cat.slots.filter { !$0.isEmpty }.count
            LabeledContent("Suoni", value: "\(filled) di \(cat.slots.count)")
            LabeledContent("Registrazioni", value: "\(cat.recordings.count)")
            LabeledContent("Firmware", value: "v\(cat.fw)")
            if cat.locked {
                Label("Tasti bloccati", systemImage: "lock.fill").foregroundStyle(Color.brass)
            }
        }
    }

    private func legacyStatus(_ info: DeviceInfo) -> some View {
        Section("Stato") {
            if let battery = info.battery {
                LabeledContent("Batteria", value: "\(battery)%")
            }
            LabeledContent("Firmware", value: "v\(info.firmware)")
        }
    }

    private var volumeSection: some View {
        Section("Volume") {
            Slider(value: $volume, in: 0...100, step: 10) {
                Text("Volume")
            } minimumValueLabel: {
                Image(systemName: "speaker.fill")
            } maximumValueLabel: {
                Image(systemName: "speaker.wave.3.fill")
            } onEditingChanged: { editing in
                guard !editing else { return }
                let value = Int(volume)
                feedback.run { try await ble.setVolume(value) }
            }
        }
    }

    private func settingsSection(_ cat: Catalog) -> some View {
        Section {
            Toggle("Ripeti il suono", isOn: binding(cat.repeatOn, .repeatPlayback))
            Picker("Audio", selection: Binding(
                get: { cat.audioMode },
                set: { value in feedback.run { try await ble.setSetting(.punch, value) } }
            )) {
                Text("Naturale").tag(0)
                Text("Forte").tag(1)
                Text("Massimo").tag(2)
            }
            Toggle("Blocco tasti automatico", isOn: binding(cat.autoLockOn, .autoLock))
            Toggle("Schermo capovolto", isOn: binding(cat.flipOn, .flip))
            Picker("Spegnimento dopo", selection: Binding(
                get: { cat.sleepSeconds },
                set: { value in feedback.run { try await ble.setSetting(.sleep, value / 5) } }
            )) {
                ForEach(Self.sleepChoices, id: \.self) { s in
                    Text(s < 60 ? "\(s) secondi" : "\(s / 60) minut\(s / 60 == 1 ? "o" : "i")").tag(s)
                }
            }
        } header: {
            Text("Impostazioni")
        } footer: {
            Text("«Forte» e «Massimo» tolgono i bassi che l'altoparlante non riesce a riprodurre e alzano le parti deboli: il suono è molto più forte, «Massimo» a costo di un po' di saturazione. I suoni salvati restano originali. Il blocco automatico blocca i tasti quando lo schermo si spegne: si sblocca con il tasto di accensione o tenendo premuti insieme i due tasti.")
        }
    }

    private func actionsSection(_ cat: Catalog) -> some View {
        Section("Azioni") {
            Button {
                feedback.run { try await ble.find() }
            } label: {
                Label("Fai squillare il dispositivo", systemImage: "bell.and.waves.left.and.right")
            }
            Button {
                feedback.run { try await ble.playSound() }
            } label: {
                Label("Suona il suono attivo", systemImage: "play.fill")
            }
            Button {
                feedback.run { try await ble.stopPlayback() }
            } label: {
                Label("Ferma la riproduzione", systemImage: "stop.fill")
            }
            Button {
                let lock = !cat.locked
                feedback.run { try await ble.setLocked(lock) }
            } label: {
                Label(cat.locked ? "Sblocca i tasti" : "Blocca i tasti", systemImage: cat.locked ? "lock.open" : "lock")
            }
        }
    }

    private var firmwareSection: some View {
        Section("Firmware") {
            Button {
                firmwareSheet = true
            } label: {
                LabeledContent {
                    if ble.firmwareVersion < BundledFirmware.version {
                        Text("v\(BundledFirmware.version) disponibile").foregroundStyle(Color.brass)
                    }
                } label: {
                    Label("Aggiorna il firmware", systemImage: "arrow.down.circle")
                }
            }
            if ble.supportsCatalog {
                NavigationLink {
                    DiagnosticsView()
                } label: {
                    Label("Diagnostica", systemImage: "stethoscope")
                }
            }
        }
    }

    // MARK: Utilità

    private func binding(_ value: Bool, _ setting: DeviceSetting) -> Binding<Bool> {
        Binding(get: { value }, set: { on in feedback.run { try await ble.setSetting(setting, on ? 1 : 0) } })
    }

    private func batteryIcon(_ level: Int, charging: Bool) -> String {
        if charging { return "battery.100.bolt" }
        switch level {
        case ..<13: return "battery.0"
        case ..<38: return "battery.25"
        case ..<63: return "battery.50"
        case ..<88: return "battery.75"
        default: return "battery.100"
        }
    }
}

struct ConnectionCard: View {
    @EnvironmentObject private var ble: BLEManager

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(alignment: .top, spacing: 14) {
                Image(systemName: icon)
                    .font(.title2)
                    .foregroundStyle(ble.isConnected ? Color.brass : .secondary)
                    .frame(width: 34)
                    .symbolEffect(.pulse, isActive: ble.phase == .scanning || ble.phase == .connecting)
                VStack(alignment: .leading, spacing: 4) {
                    Text(title).font(.headline)
                    Text(subtitle).font(.subheadline).foregroundStyle(.secondary)
                }
            }
            switch ble.phase {
            case .idle:
                Button("Collega") { ble.scan() }
                    .buttonStyle(.borderedProminent)
            case .scanning:
                Button("Annulla") { ble.cancelScan() }
                    .buttonStyle(.bordered)
            case .connected:
                Button("Scollega", role: .destructive) { ble.disconnect() }
                    .buttonStyle(.bordered)
            default:
                EmptyView()
            }
        }
        .padding(.vertical, 6)
    }

    private var icon: String {
        switch ble.phase {
        case .connected: return "key.radiowaves.forward.fill"
        case .bluetoothOff, .unauthorized: return "exclamationmark.triangle"
        default: return "key"
        }
    }

    private var title: String {
        switch ble.phase {
        case .bluetoothOff: return "Bluetooth spento"
        case .unauthorized: return "Permesso Bluetooth negato"
        case .idle: return ble.scanFailed ? "Non l'ho trovato" : "Non collegato"
        case .scanning: return "Cerco il dispositivo…"
        case .connecting: return "Mi collego…"
        case .connected: return ble.deviceName
        }
    }

    private var subtitle: String {
        switch ble.phase {
        case .bluetoothOff: return "Accendi il Bluetooth dal Centro di Controllo."
        case .unauthorized: return "Consenti l'accesso in Impostazioni › Privacy e sicurezza › Bluetooth."
        case .idle:
            return ble.scanFailed
                ? "Tieni premuto il tasto frontale per 2 secondi finché compare «Bluetooth attivo», poi riprova."
                : "Tieni premuto il tasto frontale per 2 secondi finché compare «Bluetooth attivo», poi tocca Collega."
        case .scanning, .connecting: return "Tieni il telefono vicino al dispositivo."
        case .connected: return "Collegato. Resta collegato finché l'app è aperta."
        }
    }
}

/// Aggiornamento del firmware via Bluetooth.
struct FirmwareSheet: View {
    @EnvironmentObject private var ble: BLEManager
    @EnvironmentObject private var feedback: Feedback
    @Environment(\.dismiss) private var dismiss
    @State private var importing = false

    var body: some View {
        NavigationStack {
            List {
                Section {
                    LabeledContent("Sul dispositivo", value: ble.firmwareVersion > 0 ? "v\(ble.firmwareVersion)" : "—")
                    LabeledContent("Incluso nell'app", value: "v\(BundledFirmware.version)")
                }
                if ble.firmwareVersion >= 6 {
                    Section {
                        Button {
                            guard let url = BundledFirmware.url, let data = try? Data(contentsOf: url) else {
                                feedback.show(BLEError.badFirmwareFile)
                                return
                            }
                            install(data)
                        } label: {
                            Label("Installa la v\(BundledFirmware.version)", systemImage: "arrow.down.circle.fill")
                        }
                        Button {
                            importing = true
                        } label: {
                            Label("Scegli un file .bin…", systemImage: "doc")
                        }
                    } footer: {
                        Text("L'aggiornamento dura circa un minuto. Se si interrompe, il dispositivo resta con il firmware di prima. Alla fine si riavvia e riapre il Bluetooth da solo: tocca di nuovo Collega.")
                    }
                } else {
                    Section { OldFirmwareHint() }
                }
            }
            .navigationTitle("Firmware")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) { Button("Fine") { dismiss() } }
            }
            .fileImporter(isPresented: $importing, allowedContentTypes: [.data]) { result in
                switch result {
                case .success(let url):
                    let scoped = url.startAccessingSecurityScopedResource()
                    defer { if scoped { url.stopAccessingSecurityScopedResource() } }
                    guard let data = try? Data(contentsOf: url) else { feedback.show(BLEError.badFirmwareFile); return }
                    install(data)
                case .failure(let error):
                    feedback.show(error)
                }
            }
        }
    }

    private func install(_ data: Data) {
        dismiss()
        feedback.run(success: "Firmware installato. Il dispositivo si sta riavviando e riapre il Bluetooth: tocca Collega tra qualche secondo.") {
            try await ble.updateFirmware(data)
        }
    }
}

/// Registro degli avvii e stato interno del dispositivo.
struct DiagnosticsView: View {
    @EnvironmentObject private var ble: BLEManager
    @State private var text = "Leggo…"

    var body: some View {
        ScrollView {
            Text(text)
                .font(.system(.footnote, design: .monospaced))
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding()
        }
        .navigationTitle("Diagnostica")
        .toolbar {
            ToolbarItem {
                ShareLink(item: text) { Image(systemName: "square.and.arrow.up") }
            }
        }
        .task { await load() }
        .refreshable { await load() }
    }

    private func load() async {
        do { text = try await ble.readDiagnostics() }
        catch { text = error.localizedDescription }
    }
}
