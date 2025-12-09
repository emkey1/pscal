import UIKit
import Combine

class NativeTerminalView: UITextView {

    var onSettingsChanged: ((CGFloat, UIColor) -> Void)?
    var onInput: ((String) -> Void)?
    var onResize: (() -> Void)?

    private var currentFontSize: CGFloat = 14
    private var currentColor: UIColor = .green
    private var isSettingsOpen = false

    private var containerView: TerminalAccessoryView!
    private var settingsContainer: UIView!

    // UI Controls
    private let sizeSlider = UISlider()
    private let colorSegmentedControl = UISegmentedControl(items: ["Green", "White", "Amber", "Cyan"])

    private var lastKeyboardFrame: CGRect = .zero

    private var pendingUpdate: (text: NSAttributedString, cursor: TerminalCursorInfo?)?

    // Custom cursor layer
    private let terminalCursorLayer: CALayer = {
        let layer = CALayer()
        layer.backgroundColor = UIColor.systemOrange.cgColor
        layer.opacity = 0
        return layer
    }()
    private var blinkAnimationAdded = false

    override init(frame: CGRect, textContainer: NSTextContainer?) {
        super.init(frame: frame, textContainer: textContainer)
        commonInit()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        commonInit()
    }

    private func commonInit() {
        setupAccessoryView()
        setupKeyboardObservers()

        // Visual Styling
        backgroundColor = .black
        autocapitalizationType = .none
        keyboardAppearance = .dark

        // Layout priority
        setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        setContentCompressionResistancePriority(.defaultLow, for: .vertical)

        // Disable smart features
        keyboardType = .asciiCapable
        autocorrectionType = .no
        spellCheckingType = .no
        smartDashesType = .no
        smartQuotesType = .no
        smartInsertDeleteType = .no

        // Ensure assistant items are empty (removes the default bar if possible)
        let item = inputAssistantItem
        item.leadingBarButtonGroups = []
        item.trailingBarButtonGroups = []

        // Add cursor layer
        layer.addSublayer(terminalCursorLayer)
    }

    deinit { NotificationCenter.default.removeObserver(self) }

    func updateAppearance(color: UIColor, fontSize: CGFloat) {
        if self.currentColor != color || self.currentFontSize != fontSize {
            self.currentColor = color
            self.currentFontSize = fontSize
            self.textColor = color
            self.font = UIFont.monospacedSystemFont(ofSize: fontSize, weight: .medium)
            self.terminalCursorLayer.backgroundColor = color.cgColor

            // Update settings controls to match external state
            sizeSlider.value = Float(fontSize)
            switch color {
            case .white: colorSegmentedControl.selectedSegmentIndex = 1
            case .yellow: colorSegmentedControl.selectedSegmentIndex = 2
            case .cyan: colorSegmentedControl.selectedSegmentIndex = 3
            default: colorSegmentedControl.selectedSegmentIndex = 0
            }
        }
    }

    func updateContent(text: NSAttributedString, cursor: TerminalCursorInfo?) {
        // If we are not on main thread, dispatch
        if !Thread.isMainThread {
            DispatchQueue.main.async {
                self.updateContent(text: text, cursor: cursor)
            }
            return
        }

        self.attributedText = text
        self.font = UIFont.monospacedSystemFont(ofSize: currentFontSize, weight: .medium) // Ensure font persists
        self.textColor = currentColor
        self.backgroundColor = TerminalFontSettings.shared.backgroundColor

        updateCursor(info: cursor)

        // Scroll to cursor or bottom
        if let cursor = cursor {
             let len = text.length
             let safeOffset = max(0, min(cursor.textOffset, len))
             let range = NSRange(location: safeOffset, length: 0)
             self.scrollRangeToVisible(range)
        } else {
             let len = text.length
             if len > 0 {
                 let bottom = NSMakeRange(len - 1, 1)
                 self.scrollRangeToVisible(bottom)
             }
        }
    }

    private func updateCursor(info: TerminalCursorInfo?) {
        guard let info = info else {
            terminalCursorLayer.opacity = 0
            return
        }

        // Calculate cursor rect
        let len = attributedText.length
        let safeOffset = max(0, min(info.textOffset, len))

        if let pos = position(from: beginningOfDocument, offset: safeOffset) {
            let caret = caretRect(for: pos)
            var rect = caret
            rect.origin.x -= contentOffset.x
            rect.origin.y -= contentOffset.y
            // Ensure min width for visibility (block cursor style)
            let charWidth = TerminalGeometryCalculator.characterMetrics(for: font ?? UIFont.monospacedSystemFont(ofSize: 14, weight: .medium)).width
            rect.size.width = max(charWidth, rect.size.width)

            CATransaction.begin()
            CATransaction.setDisableActions(true)
            terminalCursorLayer.frame = rect
            CATransaction.commit()

            if !blinkAnimationAdded {
                let animation = CABasicAnimation(keyPath: "opacity")
                animation.fromValue = 1
                animation.toValue = 0
                animation.duration = 0.8
                animation.autoreverses = true
                animation.repeatCount = .infinity
                terminalCursorLayer.add(animation, forKey: "blink")
                blinkAnimationAdded = true
            }
            terminalCursorLayer.opacity = 1
        } else {
            terminalCursorLayer.opacity = 0
        }
    }

    // MARK: - Accessory View Construction
    private func setupAccessoryView() {
        // Use the custom self-sizing view
        containerView = TerminalAccessoryView(frame: CGRect(x: 0, y: 0, width: UIScreen.main.bounds.width, height: 44))
        containerView.backgroundColor = UIColor(white: 0.15, alpha: 1.0)
        containerView.autoresizingMask = .flexibleHeight

        // 1. Toolbar Keys
        let keysStack = UIStackView()
        keysStack.axis = .horizontal
        keysStack.distribution = .fillEqually
        keysStack.spacing = 6
        keysStack.translatesAutoresizingMaskIntoConstraints = false

        let keys = ["Esc", "Tab", "Ctrl", "/", "-", "R", "⚙️"]
        for key in keys {
            let btn = UIButton(type: .system)
            btn.setTitle(key, for: .normal)
            btn.setTitleColor(.white, for: .normal)
            btn.backgroundColor = UIColor(white: 0.3, alpha: 1.0)
            btn.layer.cornerRadius = 6
            btn.addTarget(self, action: #selector(keyTapped(_:)), for: .touchUpInside)
            keysStack.addArrangedSubview(btn)
        }

        containerView.addSubview(keysStack)

        // 2. Settings Drawer
        settingsContainer = UIView()
        settingsContainer.translatesAutoresizingMaskIntoConstraints = false
        settingsContainer.isHidden = true
        settingsContainer.alpha = 0
        containerView.addSubview(settingsContainer)

        // Settings UI
        let sizeLabel = UILabel()
        sizeLabel.text = "Font Size"
        sizeLabel.textColor = .lightGray
        sizeLabel.font = .systemFont(ofSize: 12)
        sizeLabel.translatesAutoresizingMaskIntoConstraints = false

        sizeSlider.minimumValue = 10
        sizeSlider.maximumValue = 24
        sizeSlider.tintColor = .gray
        sizeSlider.translatesAutoresizingMaskIntoConstraints = false
        sizeSlider.addTarget(self, action: #selector(settingsChanged), for: .valueChanged)

        colorSegmentedControl.selectedSegmentIndex = 0
        colorSegmentedControl.translatesAutoresizingMaskIntoConstraints = false
        colorSegmentedControl.addTarget(self, action: #selector(settingsChanged), for: .valueChanged)

        settingsContainer.addSubview(sizeLabel)
        settingsContainer.addSubview(sizeSlider)
        settingsContainer.addSubview(colorSegmentedControl)

        // 3. Layout Constraints
        NSLayoutConstraint.activate([
            // Keys: Top of container
            keysStack.topAnchor.constraint(equalTo: containerView.topAnchor, constant: 5),
            keysStack.leadingAnchor.constraint(equalTo: containerView.leadingAnchor, constant: 10),
            keysStack.trailingAnchor.constraint(equalTo: containerView.trailingAnchor, constant: -10),
            keysStack.heightAnchor.constraint(equalToConstant: 34),

            // Settings: Below keys, FIXED height
            settingsContainer.topAnchor.constraint(equalTo: keysStack.bottomAnchor, constant: 10),
            settingsContainer.leadingAnchor.constraint(equalTo: containerView.leadingAnchor, constant: 20),
            settingsContainer.trailingAnchor.constraint(equalTo: containerView.trailingAnchor, constant: -20),
            settingsContainer.heightAnchor.constraint(equalToConstant: 80),

            // Internal Settings Controls
            sizeLabel.topAnchor.constraint(equalTo: settingsContainer.topAnchor),
            sizeLabel.leadingAnchor.constraint(equalTo: settingsContainer.leadingAnchor),

            sizeSlider.centerYAnchor.constraint(equalTo: sizeLabel.centerYAnchor),
            sizeSlider.leadingAnchor.constraint(equalTo: sizeLabel.trailingAnchor, constant: 15),
            sizeSlider.trailingAnchor.constraint(equalTo: settingsContainer.trailingAnchor),

            colorSegmentedControl.topAnchor.constraint(equalTo: sizeLabel.bottomAnchor, constant: 15),
            colorSegmentedControl.leadingAnchor.constraint(equalTo: settingsContainer.leadingAnchor),
            colorSegmentedControl.trailingAnchor.constraint(equalTo: settingsContainer.trailingAnchor)
        ])

        self.inputAccessoryView = containerView
    }

    @objc func keyTapped(_ sender: UIButton) {
        guard let title = sender.title(for: .normal) else { return }
        if title == "⚙️" {
            toggleSettings()
        } else if title == "R" {
            // Reset Terminal
            PscalRuntimeBootstrap.shared.resetTerminalState()
        } else if title == "Tab" {
            onInput?("\t")
        } else if title == "Esc" {
            onInput?("\u{1B}")
        } else if title == "Ctrl" {
            // Toggle Ctrl mode or send next char as ctrl?
            // Prototype just has it as a key. For now let's just log or implement if needed.
            // TerminalInputBridge had a toggle. Let's assume toggle behavior or single shot?
            // For now, I'll implementing a simple latch visual feedback if I could, but let's stick to basic input.
            // Wait, standard Ctrl key usually means "Control".
            // In the prototype code provided, it didn't have implementation for Ctrl action other than just a key.
            // I'll implement it as sending a Ctrl modifier signal or just ignore for now if not specified.
            // Actually, TerminalInputBridge had logic for Ctrl.
            // Let's implement a simple latch.
             sender.isSelected.toggle()
             sender.backgroundColor = sender.isSelected ? UIColor.systemBlue : UIColor(white: 0.3, alpha: 1.0)
        } else {
            // Regular keys
            if let ctrlBtn = (containerView.subviews.first?.subviews.first(where: { ($0 as? UIButton)?.title(for: .normal) == "Ctrl" }) as? UIButton), ctrlBtn.isSelected {
                // Handle Control char
                 if let first = title.first, let scalar = UnicodeScalar(String(first)) {
                     let value = scalar.value
                     if value >= 0x40 && value <= 0x7F {
                         let ctrlChar = UnicodeScalar(value & 0x1F)!
                         onInput?(String(ctrlChar))
                         // Reset Ctrl
                         ctrlBtn.isSelected = false
                         ctrlBtn.backgroundColor = UIColor(white: 0.3, alpha: 1.0)
                         return
                     }
                 }
            }
            onInput?(title)
        }
    }

    func toggleSettings() {
        isSettingsOpen.toggle()

        // 1. Update the target height on our custom class
        let newHeight: CGFloat = isSettingsOpen ? 140 : 44
        containerView.targetHeight = newHeight

        // 2. Animate visibility
        UIView.animate(withDuration: 0.3) {
            self.settingsContainer.isHidden = !self.isSettingsOpen
            self.settingsContainer.alpha = self.isSettingsOpen ? 1 : 0
        }

        // 3. Tell the input system to refresh layout
        self.reloadInputViews()
    }

    @objc func settingsChanged() {
        let newSize = CGFloat(sizeSlider.value)
        var newColor: UIColor = .green
        switch colorSegmentedControl.selectedSegmentIndex {
        case 0: newColor = .green
        case 1: newColor = .white
        case 2: newColor = .yellow
        case 3: newColor = .cyan
        default: break
        }
        onSettingsChanged?(newSize, newColor)
    }

    // MARK: - Layout Boilerplate
    override func layoutSubviews() {
        super.layoutSubviews()
        updateBottomInset()
        // Notify of resize?
        // In the original, grid calculation happens in SwiftUI.
    }

    private func setupKeyboardObservers() {
        NotificationCenter.default.addObserver(self, selector: #selector(kbFrameChanged), name: UIResponder.keyboardWillChangeFrameNotification, object: nil)
        NotificationCenter.default.addObserver(self, selector: #selector(kbWillHide), name: UIResponder.keyboardWillHideNotification, object: nil)
    }

    @objc private func kbWillHide(n: Notification) {
        lastKeyboardFrame = .zero
        updateBottomInset()
    }

    @objc private func kbFrameChanged(n: Notification) {
        guard let val = n.userInfo?[UIResponder.keyboardFrameEndUserInfoKey] as? NSValue else { return }
        lastKeyboardFrame = val.cgRectValue
        setNeedsLayout()
        updateBottomInset()
    }

    private func updateBottomInset() {
        if lastKeyboardFrame == .zero {
            if contentInset.bottom != 0 {
                contentInset = .zero; scrollIndicatorInsets = .zero
            }
            return
        }
        let kbFrame = convert(lastKeyboardFrame, from: nil)
        let overlap = bounds.intersection(kbFrame).height
        if contentInset.bottom != overlap {
            contentInset = UIEdgeInsets(top: 0, left: 0, bottom: overlap, right: 0)
            scrollIndicatorInsets = contentInset
        }
    }

    override var canBecomeFirstResponder: Bool { true }
}
