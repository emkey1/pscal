import Foundation

extension AttributedString {
    func trimmedTrailingSpaces() -> AttributedString {
        var trimmed = self
        while trimmed.endIndex > trimmed.startIndex {
            let before = trimmed.index(before: trimmed.endIndex)
            let range = before..<trimmed.endIndex
            let segment = trimmed[range]
            if String(segment.characters) == " " {
                trimmed.removeSubrange(range)
            } else {
                break
            }
        }
        return trimmed
    }
}
