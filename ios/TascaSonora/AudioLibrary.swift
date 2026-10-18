import AVFoundation

struct LibraryItem: Identifiable, Hashable {
    let url: URL
    let date: Date
    let duration: Double
    var id: URL { url }
    var name: String { url.deletingPathExtension().lastPathComponent }
}

/// Archivio locale: i file stanno nella cartella Documenti, visibile anche dall'app File.
@MainActor
final class AudioLibrary: ObservableObject {
    @Published private(set) var items: [LibraryItem] = []

    let folder = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]

    init() { reload() }

    func reload() {
        let keys: [URLResourceKey] = [.creationDateKey]
        let urls = (try? FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: keys)) ?? []
        items = urls
            .filter { ["wav", "m4a", "mp3", "aif", "aiff", "caf"].contains($0.pathExtension.lowercased()) }
            .map { url in
                let date = (try? url.resourceValues(forKeys: [.creationDateKey]).creationDate) ?? .distantPast
                var duration = 0.0
                if let f = try? AVAudioFile(forReading: url) {
                    duration = Double(f.length) / f.processingFormat.sampleRate
                }
                return LibraryItem(url: url, date: date, duration: duration)
            }
            .sorted { $0.date > $1.date }
    }

    /// Salva un PCM del portachiavi come WAV e restituisce il file.
    @discardableResult
    func save(pcm: Data, prefix rawPrefix: String) throws -> URL {
        let prefix = rawPrefix.components(separatedBy: CharacterSet(charactersIn: "/:\\?%*|\"<>")).joined(separator: "-")
        let stamp = Date().formatted(.dateTime.day(.twoDigits).month(.twoDigits).hour(.twoDigits(amPM: .omitted)).minute(.twoDigits))
            .replacingOccurrences(of: "/", with: "-")
            .replacingOccurrences(of: ":", with: ".")
            .replacingOccurrences(of: ",", with: "")
        var url = folder.appendingPathComponent("\(prefix) \(stamp).wav")
        var n = 2
        while FileManager.default.fileExists(atPath: url.path) {
            url = folder.appendingPathComponent("\(prefix) \(stamp) (\(n)).wav")
            n += 1
        }
        try AudioTools.wav(pcm).write(to: url)
        reload()
        return url
    }

    func delete(_ item: LibraryItem) {
        try? FileManager.default.removeItem(at: item.url)
        reload()
    }
}
