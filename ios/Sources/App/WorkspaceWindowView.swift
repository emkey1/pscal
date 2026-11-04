// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// Ported by its author from iSH-AOK's Workspace (app/WorkspaceViewController.m at
// 0f75fee1c) and licensed to PSCAL under the Apache License 2.0.

import UIKit

enum WorkspaceMetrics {
    static let windowCornerRadius: CGFloat = 22
    static let titleBarHeight: CGFloat = 24
    static let buttonSize: CGFloat = 18
    static let buttonInset: CGFloat = 8
    static let titleSideInset: CGFloat = 34

    static var usesPhoneLayout: Bool {
        UIDevice.current.userInterfaceIdiom == .phone
    }

    static var preferredTerminalSize: CGSize {
        usesPhoneLayout ? CGSize(width: 372, height: 288) : CGSize(width: 900, height: 620)
    }

    static var minimumTerminalSize: CGSize {
        usesPhoneLayout ? CGSize(width: 150, height: 110) : CGSize(width: 260, height: 170)
    }
}

enum WorkspaceTheme {
    static let accent = UIColor(red: 0.20, green: 0.48, blue: 0.96, alpha: 1)

    static let desktopBackground = UIColor { traits in
        traits.userInterfaceStyle == .dark
            ? UIColor(red: 0.07, green: 0.08, blue: 0.11, alpha: 1)
            : UIColor(red: 0.92, green: 0.93, blue: 0.96, alpha: 1)
    }

    private static func luminance(_ color: UIColor) -> CGFloat {
        var red: CGFloat = 0, green: CGFloat = 0, blue: CGFloat = 0, alpha: CGFloat = 0
        color.getRed(&red, green: &green, blue: &blue, alpha: &alpha)
        func linear(_ value: CGFloat) -> CGFloat {
            value <= 0.03928 ? value / 12.92 : pow((value + 0.055) / 1.055, 2.4)
        }
        return 0.2126 * linear(red) + 0.7152 * linear(green) + 0.0722 * linear(blue)
    }

    /// Text and glyphs are measured against the fill they sit on rather than assumed
    /// white: white reads well on a teal accent and badly on a mint one.
    static func ink(on fill: UIColor) -> UIColor {
        let light = luminance(fill)
        let whiteContrast = 1.05 / (light + 0.05)
        return whiteContrast >= 4.5 ? .white : UIColor(white: 0.08, alpha: 1)
    }
}

/// How far the system's window controls reach down over the content, in an iPadOS 26
/// window. A window placed at the top of the desktop would otherwise sit with its title
/// bar, close button included, under them.
@MainActor
func workspaceWindowingControlsTopInset(_ view: UIView) -> CGFloat {
    guard #available(iOS 26.0, *), let window = view.window else { return 0 }
    // A window covering its whole screen is full screen, where the controls are in the
    // menu bar. Decided from the geometry because the corner inset can be stale after an
    // app that launched in a window goes full screen.
    if let screen = window.windowScene?.screen,
       abs(window.bounds.width - screen.bounds.width) < 1,
       abs(window.bounds.height - screen.bounds.height) < 1 {
        return 0
    }
    let region = UIView.LayoutRegion.safeArea(cornerAdaptation: .vertical)
    var windowTop = window.edgeInsets(for: region).top
    let plainTop = window.safeAreaInsets.top
    // A window whose corner adaptation made no room for its controls still has them in
    // its top-leading corner, and every window measured cleared them by 43 points. An iOS
    // app on a Mac has its chrome outside the content, so there is nothing to clear.
    let info = ProcessInfo.processInfo
    if windowTop < plainTop + 1 && !info.isiOSAppOnMac && !info.isMacCatalystApp {
        windowTop = plainTop + 43
    }
    let viewTop = view.convert(CGPoint.zero, to: window).y
    return max(0, windowTop - viewTop)
}

/// A movable, resizable window on the Workspace desktop. It knows nothing about what it
/// holds: its content is a child view controller in `contentContainerView`.
@MainActor
final class WorkspaceWindowView: UIView, UIGestureRecognizerDelegate {
    private static var nextSerial = 1

    /// A number no other window this run has had.
    let serial: Int
    let titleBarView = UIView()
    let titleLabel = UILabel()
    let closeButton = UIButton(type: .system)
    let utilityButton = UIButton(type: .system)
    let contentContainerView = UIView()
    private let panelView = UIView()
    private let resizeHandleView = UIView()
    private var tapRecognizer: UITapGestureRecognizer?

    /// The frame this window had before a layout clamp shrank or moved it to fit a smaller
    /// desktop: a rotation to portrait, or the portrait-sized pass iOS runs when the app is
    /// backgrounded. Null when no restore is pending; any deliberate frame change clears it.
    var frameBeforeBoundsClamp: CGRect = .null
    var preferredSize: CGSize = .zero {
        didSet {
            // Every deliberate size change flows through here; a stale pre-clamp frame
            // must not override it later.
            frameBeforeBoundsClamp = .null
        }
    }
    var didApplyInitialFrame = false
    var draggable = true
    var resizable = false {
        didSet {
            resizeHandleView.isHidden = !resizable
        }
    }
    var titleBarDoubleTapZoomEnabled = false
    private(set) var zoomedToFullscreen = false
    private var restoreFrameBeforeZoom: CGRect = .null
    var minimumSize = CGSize(width: 280, height: 180)
    var maximumSize: CGSize = .zero

    var closeHandler: (() -> Void)?
    var utilityHandler: (() -> Void)?
    var didBecomeFrontmostHandler: (() -> Void)?
    var frameDidChangeHandler: (() -> Void)?

    init(title: String, showsCloseButton: Bool) {
        serial = Self.nextSerial
        Self.nextSerial += 1
        super.init(frame: .zero)
        autoresizingMask = []
        backgroundColor = .clear
        layer.cornerRadius = WorkspaceMetrics.windowCornerRadius
        layer.masksToBounds = false
        layer.shadowColor = UIColor.black.cgColor
        layer.shadowOpacity = 0.18
        layer.shadowRadius = 28
        layer.shadowOffset = CGSize(width: 0, height: 16)
        buildChrome(title: title, showsCloseButton: showsCloseButton)
        installGestures()
        accessibilityIdentifier = "workspace.window.\(serial)"
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    private func buildChrome(title: String, showsCloseButton: Bool) {
        panelView.translatesAutoresizingMaskIntoConstraints = false
        panelView.layer.cornerRadius = WorkspaceMetrics.windowCornerRadius
        panelView.layer.masksToBounds = true
        panelView.layer.borderWidth = 1
        addSubview(panelView)

        titleBarView.translatesAutoresizingMaskIntoConstraints = false
        panelView.addSubview(titleBarView)

        titleLabel.translatesAutoresizingMaskIntoConstraints = false
        titleLabel.font = .preferredFont(forTextStyle: .subheadline)
        titleLabel.textAlignment = .center
        titleLabel.text = title
        titleBarView.addSubview(titleLabel)

        closeButton.translatesAutoresizingMaskIntoConstraints = false
        closeButton.setTitle("×", for: .normal)
        closeButton.titleLabel?.font = .systemFont(ofSize: 14, weight: .semibold)
        closeButton.isHidden = !showsCloseButton
        closeButton.accessibilityLabel = "Close Window"
        closeButton.accessibilityHint = "Closes this window and ends its sessions"
        closeButton.accessibilityIdentifier = "workspace.window.close"
        closeButton.addTarget(self, action: #selector(closePressed), for: .touchUpInside)
        titleBarView.addSubview(closeButton)

        utilityButton.translatesAutoresizingMaskIntoConstraints = false
        utilityButton.isHidden = true
        utilityButton.titleLabel?.font = .systemFont(ofSize: 10, weight: .semibold)
        utilityButton.accessibilityHint = "Opens the menu for this window"
        utilityButton.accessibilityIdentifier = "workspace.window.menu"
        utilityButton.addTarget(self, action: #selector(utilityPressed), for: .touchUpInside)
        titleBarView.addSubview(utilityButton)

        contentContainerView.translatesAutoresizingMaskIntoConstraints = false
        contentContainerView.clipsToBounds = true
        panelView.addSubview(contentContainerView)

        resizeHandleView.translatesAutoresizingMaskIntoConstraints = false
        resizeHandleView.isHidden = true
        resizeHandleView.layer.cornerRadius = 10
        resizeHandleView.isAccessibilityElement = true
        resizeHandleView.accessibilityLabel = "Resize Window"
        resizeHandleView.accessibilityIdentifier = "workspace.window.resize"
        panelView.addSubview(resizeHandleView)

        NSLayoutConstraint.activate([
            panelView.topAnchor.constraint(equalTo: topAnchor),
            panelView.leadingAnchor.constraint(equalTo: leadingAnchor),
            panelView.trailingAnchor.constraint(equalTo: trailingAnchor),
            panelView.bottomAnchor.constraint(equalTo: bottomAnchor),

            titleBarView.topAnchor.constraint(equalTo: panelView.topAnchor),
            titleBarView.leadingAnchor.constraint(equalTo: panelView.leadingAnchor),
            titleBarView.trailingAnchor.constraint(equalTo: panelView.trailingAnchor),
            titleBarView.heightAnchor.constraint(equalToConstant: WorkspaceMetrics.titleBarHeight),

            closeButton.leadingAnchor.constraint(equalTo: titleBarView.leadingAnchor, constant: WorkspaceMetrics.buttonInset),
            closeButton.centerYAnchor.constraint(equalTo: titleBarView.centerYAnchor),
            closeButton.widthAnchor.constraint(equalToConstant: WorkspaceMetrics.buttonSize),
            closeButton.heightAnchor.constraint(equalToConstant: WorkspaceMetrics.buttonSize),

            utilityButton.trailingAnchor.constraint(equalTo: titleBarView.trailingAnchor, constant: -WorkspaceMetrics.buttonInset),
            utilityButton.centerYAnchor.constraint(equalTo: titleBarView.centerYAnchor),
            utilityButton.widthAnchor.constraint(equalToConstant: 34),
            utilityButton.heightAnchor.constraint(equalToConstant: WorkspaceMetrics.buttonSize),

            titleLabel.leadingAnchor.constraint(equalTo: titleBarView.leadingAnchor, constant: WorkspaceMetrics.titleSideInset),
            titleLabel.trailingAnchor.constraint(equalTo: titleBarView.trailingAnchor, constant: -WorkspaceMetrics.titleSideInset),
            titleLabel.centerYAnchor.constraint(equalTo: titleBarView.centerYAnchor),

            contentContainerView.topAnchor.constraint(equalTo: titleBarView.bottomAnchor),
            contentContainerView.leadingAnchor.constraint(equalTo: panelView.leadingAnchor),
            contentContainerView.trailingAnchor.constraint(equalTo: panelView.trailingAnchor),
            contentContainerView.bottomAnchor.constraint(equalTo: panelView.bottomAnchor),

            resizeHandleView.widthAnchor.constraint(equalToConstant: 20),
            resizeHandleView.heightAnchor.constraint(equalToConstant: 20),
            resizeHandleView.trailingAnchor.constraint(equalTo: panelView.trailingAnchor, constant: -12),
            resizeHandleView.bottomAnchor.constraint(equalTo: panelView.bottomAnchor, constant: -12),
        ])
    }

    private func installGestures() {
        let pan = UIPanGestureRecognizer(target: self, action: #selector(handlePan(_:)))
        pan.delegate = self
        titleBarView.addGestureRecognizer(pan)

        let doubleTap = UITapGestureRecognizer(target: self, action: #selector(handleTitleBarDoubleTap(_:)))
        doubleTap.numberOfTapsRequired = 2
        doubleTap.cancelsTouchesInView = false
        doubleTap.delegate = self
        titleBarView.addGestureRecognizer(doubleTap)

        let resize = UIPanGestureRecognizer(target: self, action: #selector(handleResizePan(_:)))
        resizeHandleView.addGestureRecognizer(resize)

        let tap = UITapGestureRecognizer(target: self, action: #selector(bringWindowToFront))
        tap.cancelsTouchesInView = false
        tap.delegate = self
        tap.require(toFail: doubleTap)
        addGestureRecognizer(tap)
        tapRecognizer = tap
    }

    func applyChrome(active: Bool) {
        let dark = traitCollection.userInterfaceStyle == .dark
        let accent = WorkspaceTheme.accent
        let card = dark ? UIColor(red: 0.13, green: 0.14, blue: 0.17, alpha: 1) : UIColor.white
        let titleText = dark ? UIColor(white: 0.92, alpha: 1) : UIColor(red: 0.11, green: 0.13, blue: 0.18, alpha: 1)
        let hairline = dark ? UIColor(white: 1, alpha: 0.14) : UIColor(white: 0, alpha: 0.10)
        let mutedGlyph = dark ? UIColor(white: 0.62, alpha: 1) : UIColor(white: 0.42, alpha: 1)
        let ink = WorkspaceTheme.ink(on: accent)

        panelView.backgroundColor = card
        contentContainerView.backgroundColor = card
        titleBarView.backgroundColor = active ? accent : card
        titleLabel.textColor = active ? ink : titleText
        panelView.layer.borderWidth = active ? 1.5 : 1
        panelView.layer.borderColor = (active ? accent : hairline).cgColor
        closeButton.setTitleColor(active ? ink : mutedGlyph, for: .normal)
        utilityButton.setTitleColor(active ? ink : mutedGlyph, for: .normal)
        resizeHandleView.backgroundColor = active ? accent.withAlphaComponent(0.9) : hairline
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        layer.shadowPath = UIBezierPath(roundedRect: bounds, cornerRadius: WorkspaceMetrics.windowCornerRadius).cgPath
    }

    @objc func bringWindowToFront() {
        superview?.bringSubviewToFront(self)
        didBecomeFrontmostHandler?()
    }

    @objc private func closePressed() {
        closeHandler?()
    }

    @objc private func utilityPressed() {
        utilityHandler?()
    }

    func setUtilityButton(title: String?, handler: (() -> Void)?) {
        utilityHandler = handler
        let visible = !(title ?? "").isEmpty && handler != nil
        utilityButton.setTitle(title, for: .normal)
        utilityButton.accessibilityLabel = title
        utilityButton.isHidden = !visible
    }

    // Keep the window's drag, bring-to-front and double-tap-zoom gestures from intercepting
    // touches on a control. The title-bar pan cancels touches in view, so without this the
    // slightest finger movement on the small × cancels its touch and the window looks
    // unclosable.
    func gestureRecognizer(_ gestureRecognizer: UIGestureRecognizer, shouldReceive touch: UITouch) -> Bool {
        var view = touch.view
        while let current = view, current !== self {
            if current is UIControl {
                return false
            }
            view = current.superview
        }
        return true
    }

    // A terminal has tap recognizers of its own, and the deeper one would otherwise win, so
    // a tap on the body of a background terminal would never bring its window forward.
    func gestureRecognizer(_ gestureRecognizer: UIGestureRecognizer,
                           shouldRecognizeSimultaneouslyWith other: UIGestureRecognizer) -> Bool {
        gestureRecognizer === tapRecognizer
    }

    private func roundedOrigin(_ frame: CGRect) -> CGRect {
        CGRect(x: frame.origin.x.rounded(), y: frame.origin.y.rounded(), width: frame.width, height: frame.height)
    }

    @objc private func handlePan(_ recognizer: UIPanGestureRecognizer) {
        guard draggable, let superview, !zoomedToFullscreen else { return }
        if recognizer.state == .began {
            bringWindowToFront()
        }
        let translation = recognizer.translation(in: superview)
        var frame = self.frame
        frame.origin.x += translation.x
        frame.origin.y += translation.y
        // Keep a grab strip inside the desktop, so a window can reach any edge and still
        // be dragged back.
        let visibleWidth = min(frame.width, 140)
        let minX = -(frame.width - visibleWidth)
        let maxX = max(minX, superview.bounds.width - visibleWidth)
        let visibleHeight = min(frame.height, WorkspaceMetrics.titleBarHeight)
        let maxY = max(0, superview.bounds.height - visibleHeight)
        frame.origin.x = min(max(frame.origin.x, minX), maxX)
        frame.origin.y = min(max(frame.origin.y, 0), maxY)
        self.frame = roundedOrigin(frame)
        // The user chose this spot; forget any pre-clamp frame a smaller surface stashed,
        // or the next layout pass would yank the window back out from under them.
        frameBeforeBoundsClamp = .null
        frameDidChangeHandler?()
        recognizer.setTranslation(.zero, in: superview)
    }

    @objc private func handleResizePan(_ recognizer: UIPanGestureRecognizer) {
        guard resizable, let superview, !zoomedToFullscreen else { return }
        if recognizer.state == .began {
            bringWindowToFront()
        }
        let translation = recognizer.translation(in: superview)
        var frame = self.frame
        var width = max(minimumSize.width, frame.width + translation.x)
        var height = max(minimumSize.height, frame.height + translation.y)
        if maximumSize.width > 0 {
            width = min(width, maximumSize.width)
        }
        if maximumSize.height > 0 {
            height = min(height, maximumSize.height)
        }
        frame.size.width = min(width, superview.bounds.width - frame.minX)
        frame.size.height = min(height, superview.bounds.height - frame.minY)
        self.frame = frame.integral
        preferredSize = frame.size
        frameDidChangeHandler?()
        recognizer.setTranslation(.zero, in: superview)
    }

    @objc private func handleTitleBarDoubleTap(_ recognizer: UITapGestureRecognizer) {
        guard titleBarDoubleTapZoomEnabled, recognizer.state == .recognized else { return }
        toggleZoom()
    }

    func toggleZoom() {
        guard let superview else { return }
        bringWindowToFront()
        if zoomedToFullscreen {
            guard !restoreFrameBeforeZoom.isNull, !restoreFrameBeforeZoom.isEmpty else { return }
            zoomedToFullscreen = false
            let restored = roundedOrigin(restoreFrameBeforeZoom.integral)
            frame = restored
            preferredSize = restored.size
            // Only on the way back down: a handler that saves the position on every change
            // would otherwise record the full-screen frame as the window's normal spot.
            frameDidChangeHandler?()
        } else {
            restoreFrameBeforeZoom = roundedOrigin(frame.integral)
            zoomedToFullscreen = true
            var fullscreen = superview.bounds
            // In an iPadOS 26 window the system's controls sit over the top-leading corner,
            // exactly where this window's close button lands when it fills the desktop.
            let controlsTop = workspaceWindowingControlsTopInset(superview)
            if controlsTop > superview.safeAreaInsets.top {
                fullscreen.origin.y += controlsTop
                fullscreen.size.height -= controlsTop
            }
            frame = roundedOrigin(fullscreen.integral)
        }
    }

    override func traitCollectionDidChange(_ previousTraitCollection: UITraitCollection?) {
        super.traitCollectionDidChange(previousTraitCollection)
        if previousTraitCollection?.userInterfaceStyle != traitCollection.userInterfaceStyle {
            applyChrome(active: isChromeActive)
        }
    }

    var isChromeActive = false {
        didSet {
            applyChrome(active: isChromeActive)
        }
    }
}
