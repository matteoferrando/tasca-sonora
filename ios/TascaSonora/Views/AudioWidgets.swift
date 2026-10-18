import SwiftUI

/// Forma d'onda con la parte selezionata evidenziata.
struct WaveformView: View {
    let peaks: [Float]
    let from: Double
    let to: Double

    var body: some View {
        Canvas { context, size in
            guard !peaks.isEmpty else { return }
            let selection = CGRect(x: size.width * from, y: 0,
                                   width: size.width * max(0, to - from), height: size.height)
            context.fill(Path(roundedRect: selection, cornerRadius: 6), with: .color(Color.brass.opacity(0.15)))
            let barWidth = size.width / CGFloat(peaks.count)
            for (i, peak) in peaks.enumerated() {
                let x = CGFloat(i) * barWidth
                let h = max(2, CGFloat(peak) * size.height)
                let rect = CGRect(x: x, y: (size.height - h) / 2, width: max(1, barWidth - 1), height: h)
                let inside = x >= selection.minX && x <= selection.maxX
                context.fill(Path(roundedRect: rect, cornerRadius: 1),
                             with: .color(inside ? Color.brass : Color.secondary.opacity(0.35)))
            }
        }
        .accessibilityLabel("Forma d'onda")
    }
}

/// Foglio per registrare con il microfono del telefono.
struct PhoneRecorderSheet: View {
    let onFinish: (URL) -> Void

    init(onFinish: @escaping (URL) -> Void) {
        self.onFinish = onFinish
    }
    @StateObject private var recorder = PhoneRecorder()
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            VStack(spacing: 28) {
                Spacer()
                Text(Format.seconds(recorder.elapsed))
                    .font(.system(size: 56, weight: .semibold, design: .rounded))
                    .monospacedDigit()
                Text(recorder.isRecording ? "Sto registrando… (max \(Int(recorder.maxSeconds)) s)" : "Tocca il pulsante per iniziare")
                    .foregroundStyle(.secondary)
                Button {
                    if recorder.isRecording {
                        if let url = recorder.stop() {
                            onFinish(url)
                            dismiss()
                        }
                    } else {
                        Task { await recorder.start() }
                    }
                } label: {
                    ZStack {
                        Circle().stroke(Color.secondary.opacity(0.3), lineWidth: 4).frame(width: 88, height: 88)
                        if recorder.isRecording {
                            RoundedRectangle(cornerRadius: 8).fill(.red).frame(width: 34, height: 34)
                        } else {
                            Circle().fill(.red).frame(width: 70, height: 70)
                        }
                    }
                }
                .accessibilityLabel(recorder.isRecording ? "Ferma" : "Registra")
                if recorder.denied {
                    Text("Il microfono non è autorizzato. Consentilo in Impostazioni › Tasca Sonora.")
                        .font(.footnote)
                        .foregroundStyle(.red)
                        .multilineTextAlignment(.center)
                        .padding(.horizontal)
                }
                Spacer()
            }
            .navigationTitle("Registra")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Annulla") { recorder.stop(); dismiss() }
                }
            }
            .onChange(of: recorder.isRecording) { was, now in
                // Arrivato al limite di tempo: la registrazione si ferma da sola
                if was && !now && recorder.elapsed >= recorder.maxSeconds - 0.2 {
                    onFinish(recorder.url)
                    dismiss()
                }
            }
        }
        .presentationDetents([.medium])
    }
}
