import UIKit

// MARK: - Helper Class for Self-Sizing Accessory View

final class TerminalAccessoryView: UIView {
    var targetHeight: CGFloat = 44 {
        didSet { invalidateIntrinsicContentSize() }
    }

    override var intrinsicContentSize: CGSize {
        CGSize(width: UIView.noIntrinsicMetric, height: targetHeight)
    }
}

// MARK: - Custom Native View with Expandable Settings Drawer

final class NativeTerminalView: UITextView {
    var onSettingsChanged: ((CGFloat, UIColor) -> Void)?
    var onInput: ((String) -> Void)?
    private var softKeyboardVisible: Bool = false
    private let amberColor = UIColor(red: 1.0, green: 0.74, blue: 0.23, alpha: 1.0)

    private var currentFontSize: CGFloat = 14
    private var currentColor: UIColor = .green
    private var isSettingsOpen = false

    private var containerView: TerminalAccessoryView!
    private var settingsContainer: UIView!

    private let sizeSlider = UISlider()
    private let colorSegmentedControl = UISegmentedControl(items: ["Green", "White", "Amber", "Cyan"])

    private var lastKeyboardFrame: CGRect = .zero

    override init(frame: CGRect, textContainer: NSTextContainer?) {
        super.init(frame: frame, textContainer: textContainer)
        configureInputTraits()
        setupAccessoryView()
        setupKeyboardObservers()
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        super.init(coder: coder)
        configureInputTraits()
        setupAccessoryView()
        setupKeyboardObservers()
    }

    deinit { NotificationCenter.default.removeObserver(self) }

    func updateAppearance(color: UIColor, fontSize: CGFloat) {
        textColor = color
        font = UIFont.monospacedSystemFont(ofSize: fontSize, weight: .medium)
        currentColor = color
        currentFontSize = fontSize

        sizeSlider.value = Float(fontSize)
        switch color {
        case .white: colorSegmentedControl.selectedSegmentIndex = 1
        case _ where color.isEqual(amberColor): colorSegmentedControl.selectedSegmentIndex = 2
        case .cyan: colorSegmentedControl.selectedSegmentIndex = 3
        default: colorSegmentedControl.selectedSegmentIndex = 0
        }
    }

    private func configureInputTraits() {
        keyboardType = .asciiCapable
        autocorrectionType = .no
        autocapitalizationType = .none
        spellCheckingType = .no
        smartDashesType = .no
        smartQuotesType = .no
        smartInsertDeleteType = .no
        keyboardAppearance = .dark
        inputAssistantItem.leadingBarButtonGroups = []
        inputAssistantItem.trailingBarButtonGroups = []
        setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        setContentCompressionResistancePriority(.defaultLow, for: .vertical)
    }

    // MARK: Accessory View Construction

    private func setupAccessoryView() {
        containerView = TerminalAccessoryView(frame: CGRect(x: 0, y: 0, width: UIScreen.main.bounds.width, height: 44))
        containerView.backgroundColor = UIColor(white: 0.15, alpha: 1.0)
        containerView.autoresizingMask = .flexibleHeight

        let keysStack = UIStackView()
        keysStack.axis = .horizontal
        keysStack.distribution = .fillEqually
        keysStack.spacing = 6
        keysStack.translatesAutoresizingMaskIntoConstraints = false

        let keys = ["Esc", "Tab", "Ctrl", "/", "-", "⚙️"]
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

        settingsContainer = UIView()
        settingsContainer.translatesAutoresizingMaskIntoConstraints = false
        settingsContainer.isHidden = true
        settingsContainer.alpha = 0
        containerView.addSubview(settingsContainer)

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

        NSLayoutConstraint.activate([
            keysStack.topAnchor.constraint(equalTo: containerView.topAnchor, constant: 5),
            keysStack.leadingAnchor.constraint(equalTo: containerView.leadingAnchor, constant: 10),
            keysStack.trailingAnchor.constraint(equalTo: containerView.trailingAnchor, constant: -10),
            keysStack.heightAnchor.constraint(equalToConstant: 34),

            settingsContainer.topAnchor.constraint(equalTo: keysStack.bottomAnchor, constant: 10),
            settingsContainer.leadingAnchor.constraint(equalTo: containerView.leadingAnchor, constant: 20),
            settingsContainer.trailingAnchor.constraint(equalTo: containerView.trailingAnchor, constant: -20),
            settingsContainer.heightAnchor.constraint(equalToConstant: 80),

            sizeLabel.topAnchor.constraint(equalTo: settingsContainer.topAnchor),
            sizeLabel.leadingAnchor.constraint(equalTo: settingsContainer.leadingAnchor),

            sizeSlider.centerYAnchor.constraint(equalTo: sizeLabel.centerYAnchor),
            sizeSlider.leadingAnchor.constraint(equalTo: sizeLabel.trailingAnchor, constant: 15),
            sizeSlider.trailingAnchor.constraint(equalTo: settingsContainer.trailingAnchor),

            colorSegmentedControl.topAnchor.constraint(equalTo: sizeLabel.bottomAnchor, constant: 15),
            colorSegmentedControl.leadingAnchor.constraint(equalTo: settingsContainer.leadingAnchor),
            colorSegmentedControl.trailingAnchor.constraint(equalTo: settingsContainer.trailingAnchor)
        ])

        inputAccessoryView = containerView
    }

    // MARK: Actions

    @objc private func keyTapped(_ sender: UIButton) {
        guard let title = sender.title(for: .normal) else { return }
        if title == "⚙️" {
            toggleSettings()
        } else if title == "Tab" {
            insertText("    ")
        } else if title == "Esc" {
            // Placeholder for escape action.
        } else if title == "Ctrl" {
            // Control handling can be added if needed.
        } else {
            insertText(title)
        }
    }

    private func toggleSettings() {
        isSettingsOpen.toggle()
        let newHeight: CGFloat = isSettingsOpen ? 140 : 44
        containerView.targetHeight = newHeight

        UIView.animate(withDuration: 0.3) {
            self.settingsContainer.isHidden = !self.isSettingsOpen
            self.settingsContainer.alpha = self.isSettingsOpen ? 1 : 0
        }
        reloadInputViews()
    }

    @objc private func settingsChanged() {
        let newSize = CGFloat(sizeSlider.value)
        var newColor: UIColor = .green
        switch colorSegmentedControl.selectedSegmentIndex {
        case 0: newColor = .green
        case 1: newColor = .white
        case 2: newColor = amberColor
        case 3: newColor = .cyan
        default: break
        }
        onSettingsChanged?(newSize, newColor)
    }

    // MARK: Layout / Keyboard

    override func layoutSubviews() {
        super.layoutSubviews()
        updateBottomInset()
    }

    private func setupKeyboardObservers() {
        NotificationCenter.default.addObserver(self,
                                               selector: #selector(kbWillShow),
                                               name: UIResponder.keyboardWillShowNotification,
                                               object: nil)
        NotificationCenter.default.addObserver(self,
                                               selector: #selector(kbFrameChanged),
                                               name: UIResponder.keyboardWillChangeFrameNotification,
                                               object: nil)
        NotificationCenter.default.addObserver(self,
                                               selector: #selector(kbWillHide),
                                               name: UIResponder.keyboardWillHideNotification,
                                               object: nil)
    }

    @objc private func kbWillShow(n: Notification) {
        setSoftKeyboardVisible(true)
    }

    @objc private func kbWillHide(n: Notification) {
        lastKeyboardFrame = .zero
        setSoftKeyboardVisible(false)
        updateBottomInset()
    }

    @objc private func kbFrameChanged(n: Notification) {
        guard let val = n.userInfo?[UIResponder.keyboardFrameEndUserInfoKey] as? NSValue else { return }
        lastKeyboardFrame = val.cgRectValue
        setNeedsLayout()
        updateBottomInset()
        setSoftKeyboardVisible(true)
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

    override var inputAccessoryView: UIView? {
        get { softKeyboardVisible ? containerView : nil }
        set { /* ignore external setters */ }
    }

    private func setSoftKeyboardVisible(_ isVisible: Bool) {
        guard softKeyboardVisible != isVisible else { return }
        softKeyboardVisible = isVisible
        reloadInputViews()
    }

    // MARK: Hardware key handling

    override var keyCommands: [UIKeyCommand]? {
        let up = UIKeyCommand(input: UIKeyCommand.inputUpArrow, modifierFlags: [], action: #selector(handleUp))
        let down = UIKeyCommand(input: UIKeyCommand.inputDownArrow, modifierFlags: [], action: #selector(handleDown))
        let left = UIKeyCommand(input: UIKeyCommand.inputLeftArrow, modifierFlags: [], action: #selector(handleLeft))
        let right = UIKeyCommand(input: UIKeyCommand.inputRightArrow, modifierFlags: [], action: #selector(handleRight))
        let esc = UIKeyCommand(input: UIKeyCommand.inputEscape, modifierFlags: [], action: #selector(handleEscape))
        [up, down, left, right, esc].forEach { command in
            if #available(iOS 15.0, *) { command.wantsPriorityOverSystemBehavior = true }
        }
        return [up, down, left, right, esc]
    }

    @objc private func handleUp() {
        onInput?("\u{1B}[A")
        moveCaretToEnd()
    }

    @objc private func handleDown() {
        onInput?("\u{1B}[B")
        moveCaretToEnd()
    }

    @objc private func handleLeft() {
        onInput?("\u{1B}[D")
        moveCaretToEnd()
    }

    @objc private func handleRight() {
        onInput?("\u{1B}[C")
        moveCaretToEnd()
    }

    @objc private func handleEscape() {
        onInput?("\u{1B}")
        moveCaretToEnd()
    }

    private func moveCaretToEnd() {
        let length = text?.count ?? 0
        selectedRange = NSRange(location: length, length: 0)
        scrollRangeToVisible(selectedRange)
    }
}
