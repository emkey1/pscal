import Foundation

extension AttributedString {
    func trimmedTrailingSpaces() -> AttributedString {
        var trimmed = self
        var index = trimmed.endIndex
        while index > trimmed.startIndex {
            let previous = trimmed.index(before: index)
            if trimmed[previous] == " " {
                trimmed.removeSubrange(previous..<index)
                index = previous
            } else {
                break
            }
        }
        return trimmed
    }
}
