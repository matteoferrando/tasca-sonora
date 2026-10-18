import SwiftUI

struct RecordingsView: View {
    @EnvironmentObject private var ble: BLEManager
    @EnvironmentObject private var player: PhonePlayer
    @EnvironmentObject private var library: AudioLibrary
    @EnvironmentObject private var feedback: Feedback

    @State private var selected: Catalog.Recording?
    @State private var copying: Catalog.Recording?
    @State private var confirmDelete: Catalog.Recording?
    @State private var lastDownloaded: URL?

    var body: some View {
        NavigationStack {
            List {
                if ble.isConnected, let cat = ble.catalog {
                    Section {
                        if cat.recordings.isEmpty {
                            Text("Nessuna registrazione sul dispositivo.")
                                .foregroundStyle(.secondary)
                        }
                        ForEach(cat.recordings) { rec in
                            RecordingRow(rec: rec)
                                .contentShape(Rectangle())
                                .onTapGesture { selected = rec }
                        }
                    } header: {
                        Text("Sul dispositivo")
                    } footer: {
                        if !cat.recordings.isEmpty {
                            Text("Il dispositivo tiene le ultime 6: quando ne fai una nuova, la più vecchia si cancella. Scarica quelle che vuoi tenere.")
                        }
                    }
                } else if ble.isConnected {
                    Section { OldFirmwareHint() }
                } else {
                    Section { NotConnectedHint() }
                }

                if let url = lastDownloaded {
                    Section("Appena scaricata") {
                        HStack {
                            Button {
                                player.toggle(url: url)
                            } label: {
                                Image(systemName: player.playingID == url.absoluteString ? "stop.circle.fill" : "play.circle.fill")
                                    .font(.title)
                                    .foregroundStyle(Color.brass)
                            }
                            .buttonStyle(.borderless)
                            Text(url.deletingPathExtension().lastPathComponent).lineLimit(1)
                            Spacer()
                            ShareLink(item: url) { Image(systemName: "square.and.arrow.up") }
                                .buttonStyle(.borderless)
                        }
                        Text("È salvata anche nell'Archivio.")
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                    }
                }

                Section("Come si registra") {
                    Label("Tieni premuto il tasto laterale: registra finché lo tieni, fino a 30 secondi.", systemImage: "hand.tap")
                    Label("Una pressione breve del laterale riascolta l'ultima registrazione.", systemImage: "play.circle")
                    Label("Il doppio bip conferma che è stata salvata.", systemImage: "checkmark.circle")
                }
                .font(.subheadline)
            }
            .navigationTitle("Registrazioni")
            .refreshable { await ble.refresh() }
            .confirmationDialog(selected?.displayTitle ?? "", isPresented: Binding(
                get: { selected != nil }, set: { if !$0 { selected = nil } }
            ), titleVisibility: .visible, presenting: selected) { rec in
                Button("Scarica e ascolta sul telefono") {
                    feedback.run {
                        let pcm = try await ble.downloadRecording(rec.index)
                        let url = try library.save(pcm: pcm, prefix: "Registrazione")
                        lastDownloaded = url
                        player.play(url: url)
                    }
                }
                Button("Ascolta sul dispositivo") {
                    feedback.run { try await ble.playRecording(rec.index) }
                }
                Button("Copia in un suono…") { copying = rec }
                Button("Elimina", role: .destructive) { confirmDelete = rec }
            }
            .confirmationDialog("In quale suono la copio?", isPresented: Binding(
                get: { copying != nil }, set: { if !$0 { copying = nil } }
            ), titleVisibility: .visible, presenting: copying) { rec in
                ForEach(ble.catalog?.slots ?? []) { slot in
                    Button("\(slot.index + 1). \(slot.isEmpty ? "vuoto" : slot.title)") {
                        feedback.run(success: "Registrazione copiata nel suono \(slot.index + 1).") {
                            try await ble.copyRecording(rec.index, toSlot: slot.index)
                        }
                    }
                }
            } message: { _ in
                Text("Se il suono è già occupato viene sostituito.")
            }
            .confirmationDialog("Eliminare questa registrazione dal dispositivo?", isPresented: Binding(
                get: { confirmDelete != nil }, set: { if !$0 { confirmDelete = nil } }
            ), titleVisibility: .visible, presenting: confirmDelete) { rec in
                Button("Elimina", role: .destructive) {
                    feedback.run(success: "Registrazione eliminata.") { try await ble.deleteRecording(rec.index) }
                }
            } message: { _ in
                Text("Se non l'hai scaricata, la perdi.")
            }
        }
    }
}

private struct RecordingRow: View {
    let rec: Catalog.Recording

    var body: some View {
        HStack(spacing: 14) {
            Image(systemName: "waveform.circle.fill")
                .font(.title2)
                .foregroundStyle(rec.index == 0 ? Color.red : Color.secondary)
            VStack(alignment: .leading, spacing: 2) {
                Text(rec.displayTitle)
                Text(Format.seconds(rec.seconds) + (rec.index == 0 ? " · la più recente" : ""))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            Image(systemName: "ellipsis.circle").foregroundStyle(.secondary)
        }
        .padding(.vertical, 2)
    }
}
