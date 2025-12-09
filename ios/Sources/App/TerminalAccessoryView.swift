import UIKit

// MARK: - Helper Class for Self-Sizing Accessory View
class TerminalAccessoryView: UIView {
    // This property drives the height
    var targetHeight: CGFloat = 44 {
        didSet {
            // Tell the system the size has changed so it animates layout
            invalidateIntrinsicContentSize()
        }
    }

    override var intrinsicContentSize: CGSize {
        return CGSize(width: UIView.noIntrinsicMetric, height: targetHeight)
    }
}
