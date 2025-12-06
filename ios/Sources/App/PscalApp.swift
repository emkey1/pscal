import SwiftUI
import UIKit

/// Hosts the SwiftUI terminal inside a UIKit layout that stays keyboard-safe and rotation-friendly.
/// REWRITTEN: Now uses keyboardLayoutGuide to prevent rotation race conditions.
final class TerminalRootViewController: UIViewController {
    
    // MARK: - Properties
    
    private let terminalHostingController = UIHostingController(rootView: TerminalView(showsOverlay: false))
    private let overlayBackground = UIVisualEffectView(effect: UIBlurEffect(style: .systemThinMaterialDark))
    private let buttonStack = UIStackView()
    
    private lazy var resetButton: UIButton = {
        let button = UIButton(type: .system)
        button.setTitle("Reset", for: .normal)
        button.titleLabel?.font = .systemFont(ofSize: 15, weight: .semibold)
        button.addTarget(self, action: #selector(handleReset), for: .touchUpInside)
        button.accessibilityLabel = "Reset Terminal"
        return button
    }()
    
    private lazy var settingsButton: UIButton = {
        let button = UIButton(type: .system)
        button.setImage(UIImage(systemName: "textformat.size"), for: .normal)
        button.tintColor = .label
        button.addTarget(self, action: #selector(handleSettings), for: .touchUpInside)
        button.accessibilityLabel = "Terminal Settings"
        return button
    }()

    private var overlayBottomConstraint: NSLayoutConstraint?
    private var overlayTrailingCompact: NSLayoutConstraint?
    private var overlayTrailingRegular: NSLayoutConstraint?
    
    private var lastAppliedCols: Int = 0
    private var lastAppliedRows: Int = 0
    
    // MARK: - Lifecycle
    
    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .systemBackground
        
        setupTerminalHost()
        setupOverlay()
        
        // No manual keyboard observers needed!
        // No additionalSafeAreaInsets needed!
    }
    
    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        // This is now safe because constraints (keyboardLayoutGuide)
        // have already done the hard work of resizing the view for us.
        applyTerminalGeometry(to: terminalHostingController.view)
        // refreshBackendTerminalSize()
    }
    
    override func traitCollectionDidChange(_ previousTraitCollection: UITraitCollection?) {
        super.traitCollectionDidChange(previousTraitCollection)
        if previousTraitCollection?.horizontalSizeClass != traitCollection.horizontalSizeClass {
            updateOverlayLayout(for: traitCollection)
        }
    }

    // MARK: - Setup
    
    private func setupTerminalHost() {
        addChild(terminalHostingController)
        terminalHostingController.view.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(terminalHostingController.view)
        terminalHostingController.didMove(toParent: self)

        // THE FIX: Use keyboardLayoutGuide + Safe Area combination
        // This creates a rigid "Fence" that the layout cannot break.
        NSLayoutConstraint.activate([
            // 1. Top/Sides pin to Safe Area as usual
            terminalHostingController.view.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor),
            terminalHostingController.view.leadingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.leadingAnchor),
            terminalHostingController.view.trailingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.trailingAnchor),
            
            // 2. Bottom pins to the Keyboard Top...
            terminalHostingController.view.bottomAnchor.constraint(lessThanOrEqualTo: view.keyboardLayoutGuide.topAnchor),
            
            // 3. ...BUT also respects the Safe Area bottom (for when keyboard is hidden)
            // We use 'lessThanOrEqualTo' on both, and a high priority equal to ensure it stretches
            terminalHostingController.view.bottomAnchor.constraint(lessThanOrEqualTo: view.safeAreaLayoutGuide.bottomAnchor)
        ])
        
        // This constraint pulls the view down to fill space, but stops at the barriers above
        let fillConstraint = terminalHostingController.view.bottomAnchor.constraint(equalTo: view.bottomAnchor)
        fillConstraint.priority = .defaultLow
        fillConstraint.isActive = true
    }
    
    private func setupOverlay() {
        overlayBackground.translatesAutoresizingMaskIntoConstraints = false
        overlayBackground.layer.cornerRadius = 14
        overlayBackground.clipsToBounds = true
        
        // Add Buttons
        buttonStack.axis = .horizontal
        buttonStack.alignment = .center
        buttonStack.spacing = 10
        buttonStack.translatesAutoresizingMaskIntoConstraints = false
        buttonStack.addArrangedSubview(resetButton)
        buttonStack.addArrangedSubview(settingsButton)
        
        overlayBackground.contentView.addSubview(buttonStack)
        NSLayoutConstraint.activate([
            buttonStack.leadingAnchor.constraint(equalTo: overlayBackground.contentView.leadingAnchor, constant: 12),
            buttonStack.trailingAnchor.constraint(equalTo: overlayBackground.contentView.trailingAnchor, constant: -12),
            buttonStack.topAnchor.constraint(equalTo: overlayBackground.contentView.topAnchor, constant: 8),
            buttonStack.bottomAnchor.constraint(equalTo: overlayBackground.contentView.bottomAnchor, constant: -8)
        ])
        
        view.addSubview(overlayBackground)
        
        // Pin overlay relative to the TERMINAL view, not the screen bottom.
        // This ensures the buttons ride up with the keyboard automatically.
        overlayBottomConstraint = overlayBackground.bottomAnchor.constraint(equalTo: terminalHostingController.view.bottomAnchor, constant: -12)
        
        overlayTrailingCompact = overlayBackground.trailingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.trailingAnchor, constant: -12)
        overlayTrailingRegular = overlayBackground.trailingAnchor.constraint(equalTo: view.layoutMarginsGuide.trailingAnchor, constant: -24)
        
        var constraints: [NSLayoutConstraint] = [
            overlayBottomConstraint!,
            overlayBackground.leadingAnchor.constraint(greaterThanOrEqualTo: view.layoutMarginsGuide.leadingAnchor, constant: 12),
            overlayBackground.widthAnchor.constraint(lessThanOrEqualToConstant: 320)
        ]
        
        if let compact = overlayTrailingCompact { constraints.append(compact) }
        
        NSLayoutConstraint.activate(constraints)
        
        updateOverlayLayout(for: traitCollection)
    }
    
    private func updateOverlayLayout(for traits: UITraitCollection) {
        let isRegular = traits.horizontalSizeClass == .regular
        buttonStack.axis = isRegular ? .vertical : .horizontal
        overlayTrailingCompact?.isActive = !isRegular
        overlayTrailingRegular?.isActive = isRegular
        view.layoutIfNeeded()
    }
    
    // MARK: - Logic

    /// Calculates rows/cols based on the ACTUAL view size (after auto-layout has finished).
    private func refreshBackendTerminalSize() {
        let view = terminalHostingController.view!
        let size = view.bounds.size
        
        guard size.width > 10, size.height > 10 else { return }
        
        let font = TerminalFontSettings.shared.currentFont
        let charWidth = max(1.0, ("W" as NSString).size(withAttributes: [.font: font]).width)
        let lineHeight = max(1.0, font.lineHeight)
        
        // Since we are now using Constraints, 'size.height' IS the correct usable height.
        // We don't need to manually subtract keyboardHeight anymore.
        let usableHeight = size.height
        
        let columns = max(10, min(Int(floor(size.width / charWidth)), 2000))
        let rows = max(4, min(Int(floor(usableHeight / lineHeight)), 2000))
        
        if columns == lastAppliedCols && rows == lastAppliedRows {
            return
        }
        
        // Sanity Check: If we calculated < 5 rows in Landscape, ignore it.
        // This handles the split-second during rotation where layout might be dirty.
        let isLandscape = self.view.bounds.width > self.view.bounds.height
        if isLandscape && rows < 5 {
            return
        }
        
        lastAppliedCols = columns
        lastAppliedRows = rows
        PscalRuntimeBootstrap.shared.updateTerminalSize(columns: columns, rows: rows)
    }

    @objc private func handleReset() {
        PscalRuntimeBootstrap.shared.resetTerminalState()
        NotificationCenter.default.post(name: .terminalModifierStateChanged, object: nil, userInfo: ["command": false])
    }
    
    @objc private func handleSettings() {
        let settingsHost = UIHostingController(rootView: TerminalSettingsView())
        settingsHost.modalPresentationStyle = .formSheet
        present(settingsHost, animated: true, completion: nil)
    }
}

// MARK: - SwiftUI Bridge & Entry Point

/// SwiftUI shim to host the UIKit container.
struct TerminalContainerView: UIViewControllerRepresentable {
    func makeUIViewController(context: Context) -> TerminalRootViewController {
        return TerminalRootViewController()
    }

    func updateUIViewController(_ uiViewController: TerminalRootViewController, context: Context) {
        // No-op; state is driven internally by child controllers.
    }
}

/// The App Entry Point
@main
struct PscalApp: App {
    // Ensure your AppDelegate is still hooked up if you use one
    @UIApplicationDelegateAdaptor(PscalAppDelegate.self) var appDelegate

    init() {
        PscalRuntimeBootstrap.shared.start()
    }

    var body: some Scene {
        WindowGroup {
            TerminalContainerView()
                .ignoresSafeArea(.keyboard) // Let UIKit handle the keyboard logic
        }
    }
}
