import SwiftUI

struct SoundsView: View {
    @EnvironmentObject private var ble: BLEManager
    @EnvironmentObject private var player: PhonePlayer
    @EnvironmentObject private var library: AudioLibrary
    @EnvironmentObject private var feedback: Feedback

    @State private var selected: Catalog.Slot?
    @State private var editorSlot: EditorTarget?
    @State private var renaming: Catalog.Slot?
    @State private var newName = ""
    @State private var confirmDelete: Catalog.Slot?

    var body: some View {
        NavigationStack {
            List {
                if ble.isConnected, let cat = ble.catalog {
                    Section {
                        ForEach(cat.slots) { slot in
                            SlotRow(slot: slot, active: slot.index == cat.activeSlot)
                                .contentShape(Rectangle())
                                .onTapGesture { selected = slot }
                        }
                    } header: {
                        Text("Suoni sul dispositivo")
                    } footer: {
                        Text("Il suono attivo è quello che parte con il tasto frontale. Sul dispositivo lo cambi anche dal menu: doppio clic sul tasto laterale.")
                    }
                    Section {
                        Button {
                            let target = cat.slots.first(where: { $0.isEmpty })?.index ?? cat.activeSlot
                            editorSlot = EditorTarget(slot: target)
                        } label: {
                            Label("Nuovo suono", systemImage: "plus.circle.fill")
                        }
                    }
                } else if ble.isConnected {
                    Section { OldFirmwareHint() }
                } else {
                    Section { NotConnectedHint() }
                }
            }
            .navigationTitle("Suoni")
            .refreshable { await ble.refresh() }
            .confirmationDialog(selected?.title ?? "", isPresented: Binding(
                get: { selected != nil }, set: { if !$0 { selected = nil } }
            ), titleVisibility: .visible, presenting: selected) { slot in
                actions(for: slot)
            }
            .sheet(item: $editorSlot) { target in
                SoundEditorSheet(slot: target.slot)
            }
            .alert("Nome del suono", isPresented: Binding(
                get: { renaming != nil }, set: { if !$0 { renaming = nil } }
            ), presenting: renaming) { slot in
                TextField("Suono \(slot.index + 1)", text: $newName)
                Button("Annulla", role: .cancel) {}
                Button("Salva") {
                    let name = newName
                    feedback.run { try await ble.renameSlot(slot.index, to: name) }
                }
            } message: { _ in
                Text("Massimo 16 caratteri.")
            }
            .confirmationDialog("Eliminare questo suono dal dispositivo?", isPresented: Binding(
                get: { confirmDelete != nil }, set: { if !$0 { confirmDelete = nil } }
            ), titleVisibility: .visible, presenting: confirmDelete) { slot in
                Button("Elimina \(slot.title)", role: .destructive) {
                    feedback.run(success: "Suono eliminato.") { try await ble.deleteSlot(slot.index) }
                }
            }
        }
    }

    @ViewBuilder
    private func actions(for slot: Catalog.Slot) -> some View {
        if !slot.isEmpty {
            if slot.index != ble.catalog?.activeSlot {
                Button("Usa sul tasto frontale") {
                    feedback.run { try await ble.selectSlot(slot.index) }
                }
            }
            Button("Ascolta sul dispositivo") {
                feedback.run { try await ble.playSlot(slot.index) }
            }
            Button("Scarica e ascolta sul telefono") {
                feedback.run {
                    let pcm = try await ble.downloadSlot(slot.index)
                    let url = try library.save(pcm: pcm, prefix: slot.title)
                    player.play(url: url)
                }
            }
        }
        Button(slot.isEmpty ? "Carica un suono qui" : "Sostituisci") {
            editorSlot = EditorTarget(slot: slot.index)
        }
        Button("Rinomina") {
            newName = slot.name
            renaming = slot
        }
        if !slot.isEmpty {
            Button("Elimina", role: .destructive) { confirmDelete = slot }
        }
    }
}

struct EditorTarget: Identifiable {
    let slot: Int
    var id: Int { slot }
}

private struct SlotRow: View {
    let slot: Catalog.Slot
    let active: Bool

    var body: some View {
        HStack(spacing: 14) {
            ZStack {
                Circle()
                    .fill(active ? Color.brass : Color.secondary.opacity(0.15))
                    .frame(width: 34, height: 34)
                Text("\(slot.index + 1)")
                    .font(.headline)
                    .foregroundStyle(active ? .white : .primary)
            }
            VStack(alignment: .leading, spacing: 2) {
                Text(slot.isEmpty && slot.name.isEmpty ? "Vuoto" : slot.title)
                    .foregroundStyle(slot.isEmpty ? .secondary : .primary)
                if !slot.isEmpty {
                    Text(Format.seconds(slot.seconds) + (active ? " · sul tasto" : ""))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            Spacer()
            if active {
                Image(systemName: "checkmark.circle.fill").foregroundStyle(Color.brass)
            }
        }
        .padding(.vertical, 2)
    }
}

/// Prepara un suono (da file o dal microfono del telefono), lo taglia e lo invia a un suono del dispositivo.
struct SoundEditorSheet: View {
    let slot: Int

    @EnvironmentObject private var ble: BLEManager
    @EnvironmentObject private var player: PhonePlayer
    @EnvironmentObject private var feedback: Feedback
    @Environment(\.dismiss) private var dismiss

    @State private var importing = false
    @State private var recordingSheet = false
    @State private var loading = false
    @State private var source: [Float] = []
    @State private var sourceName = ""
    @State private var peaks: [Float] = []
    @State private var start: Double = 0
    @State private var duration: Double = 5
    @State private var normalize = true
    @State private var name = ""

    private var total: Double { Double(source.count) / AudioTools.sampleRate }
    private var maxSeconds: Double { ble.info?.maxSeconds ?? 30 }
    private var maxStart: Double { max(0.01, total - 0.3) }
    private var maxDuration: Double { max(0.3, min(maxSeconds, total - start)) }

    var body: some View {
        NavigationStack {
            List {
                Section {
                    Button {
                        importing = true
                    } label: {
                        Label("Scegli un file audio", systemImage: "doc.badge.plus")
                    }
                    Button {
                        recordingSheet = true
                    } label: {
                        Label("Registra con il telefono", systemImage: "mic.circle")
                    }
                    if loading {
                        HStack { ProgressView(); Text("Preparo l'audio…").foregroundStyle(.secondary) }
                    }
                } footer: {
                    if source.isEmpty {
                        Text("Qualsiasi file audio va bene (mp3, m4a, wav…). Poi scegli il pezzo da tenere, fino a \(Int(maxSeconds)) secondi.")
                    }
                }

                if !source.isEmpty {
                    Section {
                        VStack(alignment: .leading, spacing: 8) {
                            Text(sourceName).font(.subheadline.weight(.semibold)).lineLimit(1)
                            WaveformView(peaks: peaks,
                                         from: total > 0 ? start / total : 0,
                                         to: total > 0 ? min(1, (start + duration) / total) : 1)
                                .frame(height: 84)
                        }
                        .padding(.vertical, 4)
                        VStack(alignment: .leading) {
                            LabeledContent("Inizio", value: Format.seconds(start))
                            Slider(value: $start, in: 0...maxStart)
                        }
                        VStack(alignment: .leading) {
                            LabeledContent("Durata", value: Format.seconds(duration))
                            Slider(value: $duration, in: 0.3...max(0.31, maxDuration))
                        }
                        Toggle("Volume al massimo senza distorcere", isOn: $normalize)
                        Button {
                            if player.playingID == "anteprima" { player.stop() }
                            else { player.play(wav: AudioTools.wav(rendered()), id: "anteprima") }
                        } label: {
                            Label(player.playingID == "anteprima" ? "Ferma l'anteprima" : "Ascolta l'anteprima",
                                  systemImage: player.playingID == "anteprima" ? "stop.fill" : "play.fill")
                        }
                    } header: {
                        Text("Taglia")
                    }

                    Section {
                        TextField("Nome (facoltativo)", text: $name)
                        Button {
                            send()
                        } label: {
                            Label("Invia al suono \(slot + 1)", systemImage: "paperplane.fill")
                                .frame(maxWidth: .infinity)
                        }
                        .buttonStyle(.borderedProminent)
                        .disabled(!ble.isConnected)
                    } footer: {
                        Text("L'anteprima è esattamente quello che suonerà il dispositivo.")
                    }
                }
            }
            .navigationTitle("Suono \(slot + 1)")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Annulla") { player.stop(); dismiss() }
                }
            }
            .fileImporter(isPresented: $importing, allowedContentTypes: [.audio]) { result in
                switch result {
                case .success(let url): load(url, name: url.deletingPathExtension().lastPathComponent)
                case .failure(let error): feedback.show(error)
                }
            }
            .sheet(isPresented: $recordingSheet) {
                PhoneRecorderSheet { url in load(url, name: "Registrazione dal telefono") }
            }
            .onChange(of: start) { _, _ in duration = min(duration, maxDuration) }
            .onAppear { name = ble.catalog?.slots[safe: slot]?.name ?? "" }
        }
    }

    private func rendered() -> Data {
        AudioTools.render(source, start: start, duration: min(duration, maxDuration), normalize: normalize)
    }

    private func send() {
        let pcm = rendered()
        let newName = name.trimmingCharacters(in: .whitespacesAndNewlines)
        let currentName = ble.catalog?.slots[safe: slot]?.name ?? ""
        let target = slot
        player.stop()
        dismiss()
        feedback.run(success: "Suono \(target + 1) salvato sul dispositivo.") {
            try await ble.upload(pcm, toSlot: target)
            if newName != currentName { try await ble.renameSlot(target, to: newName) }
        }
    }

    private func load(_ url: URL, name: String) {
        loading = true
        Task {
            do {
                let samples = try await Task.detached(priority: .userInitiated) {
                    try AudioTools.loadMono16k(url: url)
                }.value
                source = samples
                sourceName = name
                peaks = AudioTools.peaks(samples)
                start = 0
                duration = min(5, max(0.3, total), maxSeconds)
                if self.name.isEmpty { self.name = String(name.prefix(16)) }
            } catch {
                feedback.show(error)
            }
            loading = false
        }
    }
}
