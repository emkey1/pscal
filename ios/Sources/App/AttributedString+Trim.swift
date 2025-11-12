import Foundation

extension AttributedString {
    func trimmedTrailingSpaces() -> AttributedString {
        var trimmed = self
        var index = trimmed.endIndex
        while index > trimmed.startIndex {
            let prev = trimmed.index(before: index)
            if trimmed[prev] == " " {
                trimmed.removeSubrange(prev..<index)
                index = prev
            } else {
                break
            }
        }
        return trimmed
    }
}
