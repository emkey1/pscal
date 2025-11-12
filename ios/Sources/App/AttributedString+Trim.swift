import Foundation

extension AttributedString {
    func trimmedTrailingSpaces() -> AttributedString {
        var copy = self
        while copy.characters.last == " " {
            copy.characters.removeLast()
        }
        return copy
    }
}
