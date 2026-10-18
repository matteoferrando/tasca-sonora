import SwiftUI

@main
struct TascaSonoraApp: App {
    @StateObject private var ble = BLEManager()
    @StateObject private var player = PhonePlayer()
    @StateObject private var library = AudioLibrary()
    @StateObject private var feedback = Feedback()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(ble)
                .environmentObject(player)
                .environmentObject(library)
                .environmentObject(feedback)
                .tint(.brass)
        }
    }
}

extension Color {
    static let brass = Color(red: 0.72, green: 0.52, blue: 0.16)
}

/// Esegue un'operazione asincrona e mostra un avviso con l'esito.
struct AlertInfo: Identifiable {
    let id = UUID()
    let title: String
    let message: String
}

@MainActor
final class Feedback: ObservableObject {
    @Published var alert: AlertInfo?

    func run(success: String? = nil, _ operation: @escaping () async throws -> Void) {
        Task {
            do {
                try await operation()
                if let success { alert = AlertInfo(title: "Fatto", message: success) }
            } catch {
                alert = AlertInfo(title: "Qualcosa non ha funzionato", message: error.localizedDescription)
            }
        }
    }

    func show(_ error: Error) {
        alert = AlertInfo(title: "Qualcosa non ha funzionato", message: error.localizedDescription)
    }
}

enum Format {
    static func seconds(_ s: Double) -> String {
        s.formatted(.number.precision(.fractionLength(1))) + " s"
    }
    static func bytes(_ b: Int) -> String {
        ByteCountFormatter.string(fromByteCount: Int64(b), countStyle: .file)
    }
}
