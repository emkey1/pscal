import Foundation
import SwiftUI

// Shared aliases so runtime/editor code can refer to a single TerminalGeometryMetrics type.
// Geometry calculator lives in TerminalView.swift; these typealiases expose its nested types
// to other Swift files without duplicating the definitions.

typealias TerminalGeometryMetrics = TerminalGeometryCalculator.TerminalGeometryMetrics
typealias TerminalGridCapacity = TerminalGeometryCalculator.TerminalGridCapacity
typealias TerminalCharacterMetrics = TerminalGeometryCalculator.CharacterMetrics
