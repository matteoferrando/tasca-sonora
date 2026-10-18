import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var ble: BLEManager
    @EnvironmentObject private var feedback: Feedback

    var body: some View {
        TabView {
            DeviceView()
                .tabItem { Label("Dispositivo", systemImage: "key.fill") }
            SoundsView()
                .tabItem { Label("Suoni", systemImage: "speaker.wave.2.fill") }
            RecordingsView()
                .tabItem { Label("Registrazioni", systemImage: "mic.fill") }
            LibraryView()
                .tabItem { Label("Archivio", systemImage: "folder.fill") }
        }
        .overlay {
            if let task = ble.task { TaskOverlay(state: task) }
        }
        .alert(feedback.alert?.title ?? "", isPresented: Binding(
            get: { feedback.alert != nil },
            set: { if !$0 { feedback.alert = nil } }
        ), presenting: feedback.alert) { _ in
            Button("OK", role: .cancel) {}
        } message: { alert in
            Text(alert.message)
        }
    }
}

/// Pannello con la barra di avanzamento durante invii, download e aggiornamenti.
struct TaskOverlay: View {
    let state: BLEManager.TaskState

    var body: some View {
        ZStack {
            Color.black.opacity(0.25).ignoresSafeArea()
            VStack(spacing: 14) {
                Text(state.title).font(.headline)
                ProgressView(value: state.progress)
                    .frame(width: 220)
                Text("\(Int(state.progress * 100))%")
                    .monospacedDigit()
                    .foregroundStyle(.secondary)
                Text("Tieni il telefono vicino al dispositivo")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
            .padding(24)
            .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 20))
        }
        .transition(.opacity)
    }
}

/// Promemoria mostrato nelle schede che richiedono il collegamento.
struct NotConnectedHint: View {
    var body: some View {
        Label {
            Text("Collega il dispositivo dalla scheda **Dispositivo** per vedere cosa c'è in memoria.")
        } icon: {
            Image(systemName: "antenna.radiowaves.left.and.right.slash")
                .foregroundStyle(.secondary)
        }
        .font(.subheadline)
    }
}

/// Avviso per i firmware precedenti alla v6.
struct OldFirmwareHint: View {
    var body: some View {
        Label("Questa funzione richiede il firmware v6. Collega il dispositivo al Mac per installarlo la prima volta.",
              systemImage: "exclamationmark.triangle")
            .font(.footnote)
    }
}

extension Catalog.Recording {
    var displayTitle: String {
        if let date {
            return date.formatted(.dateTime.day().month(.abbreviated).hour().minute())
        }
        return "Registrazione \(id)"
    }
}
