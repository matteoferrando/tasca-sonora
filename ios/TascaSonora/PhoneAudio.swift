import AVFoundation

/// Riproduce audio sul telefono.
@MainActor
final class PhonePlayer: NSObject, ObservableObject, AVAudioPlayerDelegate {
    @Published private(set) var playingID: String?
    private var player: AVAudioPlayer?

    func play(wav: Data, id: String) {
        start(try? AVAudioPlayer(data: wav), id: id)
    }

    func play(url: URL, id: String? = nil) {
        start(try? AVAudioPlayer(contentsOf: url), id: id ?? url.absoluteString)
    }

    func toggle(url: URL) {
        if playingID == url.absoluteString { stop() } else { play(url: url) }
    }

    func stop() {
        player?.stop()
        playingID = nil
    }

    private func start(_ newPlayer: AVAudioPlayer?, id: String) {
        stop()
        try? AVAudioSession.sharedInstance().setCategory(.playback)
        try? AVAudioSession.sharedInstance().setActive(true)
        guard let newPlayer else { return }
        newPlayer.delegate = self
        newPlayer.play()
        player = newPlayer
        playingID = id
    }

    nonisolated func audioPlayerDidFinishPlaying(_ player: AVAudioPlayer, successfully flag: Bool) {
        Task { @MainActor in self.playingID = nil }
    }
}

/// Registra con il microfono del telefono (per creare un suono da mandare al portachiavi).
@MainActor
final class PhoneRecorder: ObservableObject {
    @Published private(set) var isRecording = false
    @Published private(set) var elapsed: Double = 0
    @Published private(set) var denied = false

    let maxSeconds: Double = 30
    let url = FileManager.default.temporaryDirectory.appendingPathComponent("registrazione-telefono.m4a")
    private var recorder: AVAudioRecorder?
    private var timer: Timer?

    func start() async {
        let granted = await withCheckedContinuation { cont in
            AVAudioApplication.requestRecordPermission { cont.resume(returning: $0) }
        }
        guard granted else { denied = true; return }
        let session = AVAudioSession.sharedInstance()
        try? session.setCategory(.playAndRecord, options: [.defaultToSpeaker])
        try? session.setActive(true)
        let settings: [String: Any] = [
            AVFormatIDKey: kAudioFormatMPEG4AAC,
            AVSampleRateKey: 44100,
            AVNumberOfChannelsKey: 1,
            AVEncoderAudioQualityKey: AVAudioQuality.high.rawValue
        ]
        guard let rec = try? AVAudioRecorder(url: url, settings: settings) else { return }
        rec.record(forDuration: maxSeconds)
        recorder = rec
        isRecording = true
        elapsed = 0
        timer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self, let r = self.recorder else { return }
                self.elapsed = r.currentTime
                if !r.isRecording { self.finish() }
            }
        }
    }

    @discardableResult
    func stop() -> URL? {
        recorder?.stop()
        return finish()
    }

    @discardableResult
    private func finish() -> URL? {
        timer?.invalidate()
        timer = nil
        let wasRecording = isRecording
        isRecording = false
        recorder = nil
        return wasRecording ? url : nil
    }
}
