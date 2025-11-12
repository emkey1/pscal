import Foundation

extension AttributedString {
    func trimmedTrailingSpaces() -> AttributedString {
        var trimmed = self
        var characters = Array(trimmed.characters)
        while let last = characters.last, last == " " {
            characters.removeLast()
        }
        return AttributedString(characters)
    }
}
