import AVFoundation

enum AudioError: LocalizedError {
    case unreadable, conversion

    var errorDescription: String? {
        switch self {
        case .unreadable: return "Non riesco a leggere questo file audio."
        case .conversion: return "La conversione dell'audio non è riuscita."
        }
    }
}

/// Conversioni audio nel formato del portachiavi: PCM 16 bit, mono, 16 kHz, little-endian.
enum AudioTools {
    static let sampleRate: Double = 16000

    /// Legge qualsiasi file audio supportato da iOS e lo porta a 16 kHz mono (valori -1...1).
    static func loadMono16k(url: URL) throws -> [Float] {
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }

        let file: AVAudioFile
        do { file = try AVAudioFile(forReading: url) } catch { throw AudioError.unreadable }
        let inFormat = file.processingFormat
        let frames = AVAudioFrameCount(file.length)
        guard frames > 0, let inBuffer = AVAudioPCMBuffer(pcmFormat: inFormat, frameCapacity: frames) else {
            throw AudioError.unreadable
        }
        try file.read(into: inBuffer)

        guard let outFormat = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: sampleRate,
                                            channels: 1, interleaved: false),
              let converter = AVAudioConverter(from: inFormat, to: outFormat) else {
            throw AudioError.conversion
        }
        converter.downmix = true
        let capacity = AVAudioFrameCount(Double(frames) * sampleRate / inFormat.sampleRate) + 4096
        guard let outBuffer = AVAudioPCMBuffer(pcmFormat: outFormat, frameCapacity: capacity) else {
            throw AudioError.conversion
        }
        var fed = false
        var conversionError: NSError?
        let status = converter.convert(to: outBuffer, error: &conversionError) { _, inputStatus in
            if fed {
                inputStatus.pointee = .endOfStream
                return nil
            }
            fed = true
            inputStatus.pointee = .haveData
            return inBuffer
        }
        guard status != .error, let channel = outBuffer.floatChannelData else { throw AudioError.conversion }
        return Array(UnsafeBufferPointer(start: channel[0], count: Int(outBuffer.frameLength)))
    }

    /// Taglia, normalizza e converte in PCM 16 bit pronto per il portachiavi.
    static func render(_ samples: [Float], start: Double, duration: Double, normalize: Bool) -> Data {
        let a = max(0, min(samples.count, Int(start * sampleRate)))
        let b = max(a, min(samples.count, a + Int(duration * sampleRate)))
        guard b > a else { return Data() }
        let segment = samples[a..<b]
        let count = segment.count

        var gain: Float = 1
        if normalize {
            let peak = segment.reduce(Float(0)) { max($0, abs($1)) }
            if peak > 0.001 { gain = min(0.95 / peak, 8) }
        }
        let fadeIn = min(count, Int(sampleRate * 0.005))
        let fadeOut = min(count, Int(sampleRate * 0.02))

        var out = [Int16](repeating: 0, count: count)
        for (i, sample) in segment.enumerated() {
            var v = sample * gain
            if i < fadeIn { v *= Float(i) / Float(fadeIn) }
            if i >= count - fadeOut { v *= Float(count - i) / Float(fadeOut) }
            out[i] = Int16(max(-32768, min(32767, (v * 32767).rounded()))).littleEndian
        }
        return out.withUnsafeBufferPointer { Data(buffer: $0) }
    }

    /// Aggiunge l'intestazione WAV a un PCM 16 bit mono.
    static func wav(_ pcm: Data, sampleRate rate: Int = 16000) -> Data {
        var d = Data()
        func str(_ s: String) { d.append(s.data(using: .ascii)!) }
        func u32(_ v: UInt32) { withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) } }
        func u16(_ v: UInt16) { withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) } }
        str("RIFF"); u32(UInt32(36 + pcm.count)); str("WAVE")
        str("fmt "); u32(16); u16(1); u16(1); u32(UInt32(rate)); u32(UInt32(rate * 2)); u16(2); u16(16)
        str("data"); u32(UInt32(pcm.count))
        d.append(pcm)
        return d
    }

    /// Picchi per disegnare la forma d'onda.
    static func peaks(_ samples: [Float], bins: Int = 160) -> [Float] {
        guard !samples.isEmpty else { return [] }
        let step = max(1, samples.count / bins)
        return stride(from: 0, to: samples.count, by: step).map { start in
            samples[start..<min(samples.count, start + step)].reduce(Float(0)) { max($0, abs($1)) }
        }
    }
}

enum CRC32 {
    private static let table: [UInt32] = (0..<256).map { i in
        var c = UInt32(i)
        for _ in 0..<8 { c = (c & 1) != 0 ? (0xEDB88320 ^ (c >> 1)) : (c >> 1) }
        return c
    }

    static func checksum(_ data: Data) -> UInt32 {
        var c: UInt32 = 0xFFFFFFFF
        data.withUnsafeBytes { raw in
            for byte in raw { c = table[Int((c ^ UInt32(byte)) & 0xFF)] ^ (c >> 8) }
        }
        return c ^ 0xFFFFFFFF
    }
}
