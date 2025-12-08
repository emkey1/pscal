import UIKit

final class TerminalSelectionOverlay: UIView {
    weak var textView: UITextView?
    private var selectionRange: NSRange?

    var hasSelection: Bool {
        guard let range = selectionRange else { return false }
        return range.length > 0
    }

    func currentSelectionRange() -> NSRange? {
        selectionRange
    }

    func selectionBoundingRect(in view: UIView) -> CGRect? {
        guard let textView else { return nil }
        return selectionBoundingRect(for: selectionRange).flatMap {
            convert($0, from: textView)
        }
    }

    func updateSelection(start: Int, end: Int) {
        let lower = min(start, end)
        let upper = max(start, end)
        let newRange = NSRange(location: lower, length: upper - lower + 1)
        let oldRect = selectionBoundingRect(for: selectionRange)
        let newRect = selectionBoundingRect(for: newRange)

        selectionRange = newRange

        if let dirty = union(rectA: oldRect, rectB: newRect) {
            setNeedsDisplay(dirty.insetBy(dx: -2, dy: -2))
        } else {
            setNeedsDisplay()
        }
    }

    func clearSelection() {
        if let range = selectionRange {
            selectionRange = nil
            if let rect = selectionBoundingRect(for: range) {
                setNeedsDisplay(rect.insetBy(dx: -2, dy: -2))
            } else {
                setNeedsDisplay()
            }
        }
    }

    override func draw(_ rect: CGRect) {
        guard let textView = textView,
              let layoutManager = textView.layoutManager as NSLayoutManager?,
              let textContainer = textView.textContainer as NSTextContainer?,
              let selection = selectionRange,
              selection.length > 0 else {
            return
        }

        let selectionColor = UIColor.systemBlue.withAlphaComponent(0.25)
        let inset = textView.textContainerInset
        let context = UIGraphicsGetCurrentContext()
        context?.setFillColor(selectionColor.cgColor)

        var actualCharRange = NSRange()
        let glyphRange = layoutManager.glyphRange(
            forCharacterRange: selection,
            actualCharacterRange: &actualCharRange
        )

        layoutManager.enumerateLineFragments(forGlyphRange: glyphRange) {
            _, usedRect, _, glyphRangeForLine, _ in
            let intersection = NSIntersectionRange(glyphRangeForLine, glyphRange)
            if intersection.length == 0 {
                return
            }
            let highlightRect = layoutManager.boundingRect(
                forGlyphRange: intersection,
                in: textContainer
            )
            var drawRect = highlightRect
            drawRect.origin.x += inset.left - textView.contentOffset.x
            drawRect.origin.y += inset.top - textView.contentOffset.y
            drawRect = drawRect.integral.insetBy(dx: -1, dy: -1)
            context?.fill(drawRect)
        }
    }

    private func selectionBoundingRect(for range: NSRange?) -> CGRect? {
        guard let range = range,
              let textView = textView,
              let layoutManager = textView.layoutManager as NSLayoutManager?,
              let textContainer = textView.textContainer as NSTextContainer?,
              range.length > 0 else {
            return nil
        }

        let inset = textView.textContainerInset
        let glyphRange = layoutManager.glyphRange(
            forCharacterRange: range,
            actualCharacterRange: nil
        )

        var rect = layoutManager.boundingRect(
            forGlyphRange: glyphRange,
            in: textContainer
        )
        rect.origin.x += inset.left - textView.contentOffset.x
        rect.origin.y += inset.top - textView.contentOffset.y
        return rect
    }

    private func union(rectA: CGRect?, rectB: CGRect?) -> CGRect? {
        switch (rectA, rectB) {
        case let (.some(a), .some(b)): return a.union(b)
        case let (.some(a), .none):    return a
        case let (.none, .some(b)):    return b
        default:                       return nil
        }
    }
}

final class TerminalSelectionMenuView: UIView {
    var copyHandler: (() -> Void)?
    var copyAllHandler: (() -> Void)?
    var pasteHandler: (() -> Void)?

    var isPasteEnabled: Bool = true {
        didSet {
            pasteButton.isEnabled = isPasteEnabled
            pasteButton.alpha = isPasteEnabled ? 1.0 : 0.5
        }
    }

    private let stack = UIStackView()

    private let copyButton: UIButton = {
        let button = UIButton(type: .system)
        button.setTitle("Copy", for: .normal)
        button.titleLabel?.font = UIFont.systemFont(ofSize: 14, weight: .semibold)
        return button
    }()

    private let copyAllButton: UIButton = {
        let button = UIButton(type: .system)
        button.setTitle("All", for: .normal)
        button.titleLabel?.font = UIFont.systemFont(ofSize: 14, weight: .semibold)
        return button
    }()

    private let pasteButton: UIButton = {
        let button = UIButton(type: .system)
        button.setTitle("Paste", for: .normal)
        button.titleLabel?.font = UIFont.systemFont(ofSize: 14, weight: .semibold)
        return button
    }()

    override init(frame: CGRect) {
        super.init(frame: frame)

        backgroundColor = UIColor.secondarySystemBackground.withAlphaComponent(0.9)
        layer.cornerRadius = 8
        layer.masksToBounds = true

        stack.axis = .horizontal
        stack.spacing = 4
        stack.alignment = .fill
        stack.distribution = .fillEqually

        addSubview(stack)
        stack.translatesAutoresizingMaskIntoConstraints = false

        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 4),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -4),
            stack.topAnchor.constraint(equalTo: topAnchor, constant: 4),
            stack.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -4)
        ])

        copyButton.addTarget(self, action: #selector(didTapCopy), for: .touchUpInside)
        copyAllButton.addTarget(self, action: #selector(didTapCopyAll), for: .touchUpInside)
        pasteButton.addTarget(self, action: #selector(didTapPaste), for: .touchUpInside)

        stack.addArrangedSubview(copyButton)
        stack.addArrangedSubview(copyAllButton)
        stack.addArrangedSubview(pasteButton)
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func sizeToFit() {
        super.sizeToFit()
        layoutIfNeeded()
    }

    @objc private func didTapCopy() {
        copyHandler?()
    }

    @objc private func didTapCopyAll() {
        copyAllHandler?()
    }

    @objc private func didTapPaste() {
        if isPasteEnabled {
            pasteHandler?()
        }
    }
}
