import AppKit
import Foundation

class AppDelegate: NSObject, NSApplicationDelegate {
    var filesOpened = false

    // ドロップ・ダブルクリック時はこちらが「先」または「同時期」に呼ばれる
    func application(_ sender: NSApplication, openFiles filenames: [String]) {
        filesOpened = true
        launchBackend(with: filenames)
        // 処理が終わったら即終了
        NSApp.terminate(nil)
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        // 次のランループ（イベント配送後）に実行を予約
        DispatchQueue.main.async {
            if !self.filesOpened {
                // openFilesが呼ばれていなければ、単体起動とみなす
                self.launchBackend(with: [])
                NSApp.terminate(nil)
            }
        }
    }

    func launchBackend(with files: [String]) {
        guard let binDir = Bundle.main.executableURL?.deletingLastPathComponent() else { return }
        let backendURL = binDir.appendingPathComponent("backend")
        
        let process = Process()
        process.executableURL = backendURL
        process.arguments = files
        try? process.run()
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.run()