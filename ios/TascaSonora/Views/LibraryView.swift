import SwiftUI

struct LibraryView: View {
    @EnvironmentObject private var ble: BLEManager
    @EnvironmentObject private var player: PhonePlayer
    @EnvironmentObject private var library: AudioLibrary
    @EnvironmentObject private var feedback: Feedback
    @State private var sending: LibraryItem?

    var body: some View {
        NavigationStack {
            Group {
                if library.items.isEmpty {
                    ContentUnavailableView("Archivio vuoto", systemImage: "folder",
                        description: Text("Le registrazioni e i suoni scaricati dal dispositivo finiscono qui. Li trovi anche nell'app File, nella cartella Tasca Sonora."))
                } else {
                    List {
                        ForEach(library.items) { item in
                            row(item)
                                .swipeActions(edge: .trailing) {
                                    Button(role: .destructive) {
                                        if player.playingID == item.url.absoluteString { player.stop() }
                                        library.delete(item)
                                    } label: { Label("Elimina", systemImage: "trash") }
                                }
                                .swipeActions(edge: .leading) {
                                    Button { askSlot(item) } label: {
                                        Label("Al dispositivo", systemImage: "paperplane")
                                    }
                                    .tint(.brass)
                                }
                                .contextMenu {
                                    Button { askSlot(item) } label: {
                                        Label("Manda al dispositivo", systemImage: "paperplane")
                                    }
                                    ShareLink(item: item.url)
                                    Button(role: .destructive) { library.delete(item) } label: {
                                        Label("Elimina", systemImage: "trash")
                                    }
                                }
                        }
                    }
                }
            }
            .navigationTitle("Archivio")
            .onAppear { library.reload() }
            .refreshable { library.reload() }
            .confirmationDialog("In quale suono lo mando?", isPresented: Binding(
                get: { sending != nil }, set: { if !$0 { sending = nil } }
            ), titleVisibility: .visible, presenting: sending) { item in
                if let slots = ble.catalog?.slots {
                    ForEach(slots) { slot in
                        Button("\(slot.index + 1). \(slot.isEmpty ? "vuoto" : slot.title)") { send(item, slot: slot.index) }
                    }
                } else {
                    Button("Suono del tasto") { send(item, slot: nil) }
                }
            } message: { _ in
                Text("Se il suono è già occupato viene sostituito. Viene tenuto al massimo l'inizio di 30 secondi.")
            }
        }
    }

    private func row(_ item: LibraryItem) -> some View {
        let playing = player.playingID == item.url.absoluteString
        return HStack(spacing: 12) {
            Button {
                player.toggle(url: item.url)
            } label: {
                Image(systemName: playing ? "stop.circle.fill" : "play.circle.fill")
                    .font(.title)
                    .foregroundStyle(Color.brass)
            }
            .buttonStyle(.borderless)
            VStack(alignment: .leading, spacing: 2) {
                Text(item.name).lineLimit(1)
                Text("\(item.date.formatted(date: .abbreviated, time: .shortened)) · \(Format.seconds(item.duration))")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            ShareLink(item: item.url) { Image(systemName: "square.and.arrow.up") }
                .buttonStyle(.borderless)
        }
    }

    private func askSlot(_ item: LibraryItem) {
        guard ble.isConnected else {
            feedback.show(BLEError.notConnected)
            return
        }
        sending = item
    }

    private func send(_ item: LibraryItem, slot: Int?) {
        let maxSeconds = ble.info?.maxSeconds ?? 30
        let title = String(item.name.prefix(16))
        let message = slot.map { "Mandato al suono \($0 + 1)." } ?? "Suono salvato sul dispositivo."
        feedback.run(success: message) {
            let samples = try await Task.detached { try AudioTools.loadMono16k(url: item.url) }.value
            let pcm = AudioTools.render(samples, start: 0, duration: maxSeconds, normalize: true)
            try await ble.upload(pcm, toSlot: slot)
            if let slot, ble.supportsCatalog { try await ble.renameSlot(slot, to: title) }
        }
    }
}
