import Foundation

extension AttributedString {
    func trimmedTrailingSpaces() -> AttributedString {
        var trimmed = self
        while let last = trimmed.last, last == " " {
            trimmed.removeLast()
        }
        return trimmed
    }
}
