// House of the Dead VR for Apple Vision Pro: the app (docs/platforms.md, P5).
//
// It opens straight into a full-immersion space, as the Quest app opens into VR (Info.plist: the scene role
// CPSceneSessionRoleImmersiveSpaceApplication). The space's CompositorLayer hands its layer renderer to vos_render.m,
// which runs the frames and the game from there.
//
// The one window is the document picker's, for your ROM set: opened only when the game asks (vosPickerOpen),
// beside the space. (GoldenEye VR's visionOS app found this shape: port/visionos/VosApp.swift.)
//
// Not called main.swift: Swift treats that name as top-level code, which clashes with @main.

import SwiftUI
import UIKit
import CompositorServices
import UniformTypeIdentifiers

struct VosLayerConfiguration: CompositorLayerConfiguration {
    func makeConfiguration(capabilities: LayerRenderer.Capabilities, configuration: inout LayerRenderer.Configuration) {
        // sRGB: the eyes' pictures (sRGB too) are copied in unchanged (vos_render.m)
        configuration.colorFormat = .bgra8Unorm_srgb
        configuration.depthFormat = .depth32Float
        configuration.isFoveationEnabled = false   // (each view drawn whole: one GL pass an eye)
        let layouts = capabilities.supportedLayouts(options: [])
        configuration.layout = layouts.contains(.dedicated) ? .dedicated : .layered
    }
}

// MARK: the document picker

final class VosPicker: ObservableObject {
    static let shared = VosPicker()
    @Published var title = "Choose your ROM set"
    @Published var picking = false
    var pending = false      // the game waits for an answer
    var windowOpen = false

    func finish(_ status: Int32, _ path: String?) {
        guard pending else { return }
        pending = false
        if let path { path.withCString { vosPickerDone(status, $0) } } else { vosPickerDone(status, nil) }
    }
}

@_cdecl("vosPickerOpen")
func vosPickerOpen(_ title: UnsafePointer<CChar>?) {
    let t = title.map { String(cString: $0) } ?? "Choose your ROM set"
    DispatchQueue.main.async {
        let p = VosPicker.shared
        p.title = t
        p.pending = true
        if p.windowOpen { p.picking = true; return }
        UIApplication.shared.activateSceneSession(for: UISceneSessionActivationRequest(role: .windowApplication)) { error in
            NSLog("hotdvr: the picker's window would not open: \(error)")
            DispatchQueue.main.async { VosPicker.shared.finish(-1, nil) }
        }
    }
}

struct VosPickerView: View {
    @ObservedObject private var picker = VosPicker.shared
    @Environment(\.dismissWindow) private var dismissWindow

    var body: some View {
        VStack(spacing: 18) {
            Text(picker.title).font(.title2)
            Text("MAME's hotd set, Revision A. House of the Dead VR keeps a copy.").foregroundStyle(.secondary)
            HStack {
                Button("Show the picker") { picker.picking = true }
                Button("Cancel") { close(0, nil) }
            }
        }
        .padding(30)
        .fileImporter(isPresented: $picker.picking, allowedContentTypes: [.zip, UTType(filenameExtension: "7z") ?? .data, .data]) { result in
            switch result {
            case .success(let url): close(1, copyIn(url))
            case .failure(let error):
                NSLog("hotdvr: picker: \(error)")
                close(0, nil)
            }
        }
        .onChange(of: picker.picking) { _, now in
            if !now {   // closed without a choice: no callback comes
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
                    if picker.pending && !picker.picking { close(0, nil) }
                }
            }
        }
        .onAppear {
            picker.windowOpen = true
            if picker.pending { picker.picking = true }
        }
        .onDisappear {
            picker.windowOpen = false
            picker.finish(0, nil)   // the window was closed: cancelled
        }
    }

    /// the chosen file, copied where the app can read it (vos_render.m moves it into Documents/arcade)
    private func copyIn(_ url: URL) -> String? {
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        let dest = FileManager.default.temporaryDirectory.appendingPathComponent(url.lastPathComponent)
        try? FileManager.default.removeItem(at: dest)
        do {
            try FileManager.default.copyItem(at: url, to: dest)
            return dest.path
        } catch {
            NSLog("hotdvr: picker: could not copy \(url.lastPathComponent): \(error)")
            return nil
        }
    }

    private func close(_ status: Int32, _ path: String?) {
        picker.finish(status == 1 && path == nil ? -1 : status, path)
        dismissWindow(id: "picker")
    }
}

@main
struct HouseOfTheDeadVRApp: App {
    init() { vosAppStart() }

    var body: some Scene {
        // no id: the space the app launches into is the body's unnamed one
        ImmersiveSpace {
            CompositorLayer(configuration: VosLayerConfiguration()) { layerRenderer in
                vosLayerReady(Unmanaged.passUnretained(layerRenderer).toOpaque())
            }
        }
        .immersionStyle(selection: .constant(.full), in: .full)
        .persistentSystemOverlays(.hidden)
        // your own hands not drawn over the game's (it draws its hands holding the guns)
        .upperLimbVisibility(.hidden)

        WindowGroup(id: "picker") { VosPickerView() }
            .defaultSize(width: 620, height: 260)
            .windowResizability(.contentSize)
            .defaultLaunchBehavior(.suppressed)   // only when the game asks
            .restorationBehavior(.disabled)
    }
}
