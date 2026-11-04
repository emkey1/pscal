// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// Ported by its author from iSH-AOK's Workspace (app/WorkspaceViewController.m at
// 0f75fee1c) and licensed to PSCAL under the Apache License 2.0.

import UIKit

/// Picks the theme, edits palettes and saves them, and sets density, gauges and wallpaper.
///
/// Theme names, the prose and the colour readouts follow the text size. The thumbnails, the
/// wallpaper, the swatches, the sliders and the Rings and Bars control are pictures and
/// controls, and stay as they are. The text size is this window's alone, so nothing here goes
/// near the theme notification when it changes.
@MainActor
final class WorkspaceThemesAppletViewController: WorkspaceAppletViewController, WorkspaceTextScalable {
    private var contentStack: UIStackView!
    private var themeListStack: UIStackView!
    private var activeThemeLabel: UILabel!
    private var editorThemeLabel: UILabel!
    private let previewSurface = UIView()
    private let previewGradient = CAGradientLayer()
    private let previewTitle = UILabel()
    private let previewBody = UILabel()
    private let previewProgress = UIProgressView(progressViewStyle: .default)
    private let wallpaperPreview = UIImageView()
    private var densityValueLabel: UILabel!
    private let densitySlider = UISlider()
    private let gaugeControl = UISegmentedControl(items: ["Rings", "Bars"])
    private var swatches: [String: UIView] = [:]
    private var channelSliders: [String: [String: UISlider]] = [:]
    private var channelValues: [String: [String: UILabel]] = [:]
    private var themeRows: [(button: UIButton, identifier: String, preview: UIImageView, title: UILabel, detail: UILabel)] = []
    private var actionButtons: [UIButton] = []
    private var editingIdentifier = WorkspaceTheme.currentIdentifier
    private var unscaledLabels: [(label: UILabel, font: UIFont)] = []
    private var unscaledColumns: [(constraint: NSLayoutConstraint, width: CGFloat)] = []

    private static func title(forColorKey key: String) -> String {
        switch key {
        case "backgroundTop": return "Background Top"
        case "backgroundBottom": return "Background Bottom"
        case "card": return "Card Surface"
        case "primary": return "Primary Text"
        case "secondary": return "Secondary Text"
        case "accent": return "Accent"
        case "accentAlt": return "Accent Alt"
        default: return key
        }
    }

    private func recordLabel(_ label: UILabel) {
        if let font = label.font {
            unscaledLabels.append((label, font))
        }
    }

    private var actionButtonFont: UIFont {
        .systemFont(ofSize: toolScaledFontSize(WorkspaceTheme.fontSize(.caption1)), weight: .semibold)
    }

    override func applyTextScale() {
        super.applyTextScale()
        guard contentStack != nil else { return }
        for entry in unscaledLabels {
            let size = toolScaledFontSize(entry.font.pointSize)
            entry.label.font = size == entry.font.pointSize ? entry.font : entry.font.withSize(size)
        }
        // These line the sliders up across the cards, and widen with their digits so "255"
        // isn't clipped.
        for entry in unscaledColumns {
            entry.constraint.constant = (entry.width * workspaceTextScale).rounded(.up)
        }
        actionButtons.forEach { $0.titleLabel?.font = actionButtonFont }
        // Rebuilt rather than re-fonted: its rows build their fonts at the current scale.
        refreshThemeRows()
    }

    private func draftPalette() -> WorkspacePalette {
        var palette: WorkspacePalette = [:]
        for key in WorkspaceTheme.editableColorKeys {
            guard let channels = channelSliders[key] else { continue }
            palette[key] = [
                "red": Double((channels["red"]?.value ?? 0).rounded()),
                "green": Double((channels["green"]?.value ?? 0).rounded()),
                "blue": Double((channels["blue"]?.value ?? 0).rounded()),
            ]
        }
        return palette
    }

    private func actionButton(title: String, action: Selector) -> UIButton {
        let button = UIButton(type: .system)
        button.translatesAutoresizingMaskIntoConstraints = false
        button.layer.cornerRadius = 10
        button.layer.borderWidth = 1
        button.contentEdgeInsets = UIEdgeInsets(top: 7, left: 10, bottom: 7, right: 10)
        button.titleLabel?.font = actionButtonFont
        button.setTitle(title, for: .normal)
        button.addTarget(self, action: action, for: .touchUpInside)
        actionButtons.append(button)
        return button
    }

    private func addThemeRow(for record: WorkspaceThemeRecord, applied: Bool) {
        let button = UIButton(type: .custom)
        button.translatesAutoresizingMaskIntoConstraints = false
        button.layer.cornerRadius = 12
        button.layer.borderWidth = 1
        button.clipsToBounds = false
        button.heightAnchor.constraint(greaterThanOrEqualToConstant: 68).isActive = true
        button.accessibilityIdentifier = "workspace.theme.\(record.identifier)"
        // A custom button has no title for VoiceOver, and the labels inside it aren't its own.
        button.accessibilityLabel = record.title
        button.tag = themeRows.count
        button.addTarget(self, action: #selector(themeRowTapped(_:)), for: .touchUpInside)

        let row = UIStackView()
        row.translatesAutoresizingMaskIntoConstraints = false
        row.axis = .horizontal
        row.spacing = 8
        row.alignment = .center
        row.isUserInteractionEnabled = false
        button.addSubview(row)

        let preview = UIImageView(image: WorkspaceTheme.artworkImage(identifier: record.identifier, palette: record.palette,
                                                                     size: CGSize(width: 92, height: 52), wallpaper: false))
        preview.translatesAutoresizingMaskIntoConstraints = false
        preview.contentMode = .scaleAspectFill
        preview.clipsToBounds = true
        preview.layer.cornerRadius = 8
        preview.layer.borderWidth = 1
        preview.widthAnchor.constraint(equalToConstant: 92).isActive = true
        preview.heightAnchor.constraint(equalToConstant: 52).isActive = true
        row.addArrangedSubview(preview)

        let text = UIStackView()
        text.axis = .vertical
        text.spacing = 2
        text.alignment = .leading
        text.isUserInteractionEnabled = false
        let title = themePrimaryLabel(style: .body)
        title.font = .systemFont(ofSize: toolScaledFontSize(WorkspaceTheme.fontSize(.subheadline)), weight: .semibold)
        title.numberOfLines = 1
        title.text = record.title
        let detail = themeSecondaryLabel(style: .footnote)
        detail.font = .systemFont(ofSize: toolScaledFontSize(WorkspaceTheme.fontSize(.footnote)))
        detail.numberOfLines = 2
        let kind = record.builtIn ? "Built-in theme" : "Saved custom theme"
        detail.text = applied ? "Applied • \(kind)" : kind
        button.accessibilityValue = kind
        text.addArrangedSubview(title)
        text.addArrangedSubview(detail)
        row.addArrangedSubview(text)

        NSLayoutConstraint.activate([
            row.topAnchor.constraint(equalTo: button.topAnchor, constant: 7),
            row.leadingAnchor.constraint(equalTo: button.leadingAnchor, constant: 8),
            row.trailingAnchor.constraint(equalTo: button.trailingAnchor, constant: -8),
            row.bottomAnchor.constraint(equalTo: button.bottomAnchor, constant: -7),
        ])
        themeRows.append((button, record.identifier, preview, title, detail))
        themeListStack.addArrangedSubview(button)
    }

    private func sliderCard(key: String) -> UIView {
        let card = stackCard(spacing: 3, inset: 10)

        let header = UIStackView()
        header.axis = .horizontal
        header.spacing = 8
        header.alignment = .center
        let title = themePrimaryLabel(style: .subheadline)
        title.text = Self.title(forColorKey: key)
        recordLabel(title)
        let swatch = UIView()
        swatch.translatesAutoresizingMaskIntoConstraints = false
        swatch.layer.cornerRadius = 8
        swatch.layer.borderWidth = 1
        swatches[key] = swatch
        swatch.widthAnchor.constraint(equalToConstant: 34).isActive = true
        swatch.heightAnchor.constraint(equalToConstant: 18).isActive = true
        header.addArrangedSubview(title)
        header.addArrangedSubview(swatch)
        title.setContentHuggingPriority(.defaultLow, for: .horizontal)
        swatch.setContentHuggingPriority(.required, for: .horizontal)
        card.stack.addArrangedSubview(header)

        var sliders: [String: UISlider] = [:]
        var values: [String: UILabel] = [:]
        for (name, channel) in [("R", "red"), ("G", "green"), ("B", "blue")] {
            let row = UIStackView()
            row.axis = .horizontal
            row.spacing = 6
            row.alignment = .center

            let channelLabel = themeSecondaryLabel(style: .caption1, monospaced: true)
            channelLabel.text = name
            channelLabel.textAlignment = .center
            let channelWidth = channelLabel.widthAnchor.constraint(equalToConstant: 16)
            channelWidth.isActive = true
            recordLabel(channelLabel)
            unscaledColumns.append((channelWidth, 16))

            let slider = UISlider()
            slider.minimumValue = 0
            slider.maximumValue = 255
            slider.accessibilityIdentifier = "\(key):\(channel)"
            slider.addTarget(self, action: #selector(channelSliderChanged(_:)), for: .valueChanged)

            let valueLabel = themeAccentLabel(style: .caption1, monospaced: true)
            valueLabel.textAlignment = .right
            let valueWidth = valueLabel.widthAnchor.constraint(equalToConstant: 28)
            valueWidth.isActive = true
            recordLabel(valueLabel)
            unscaledColumns.append((valueWidth, 28))

            row.addArrangedSubview(channelLabel)
            row.addArrangedSubview(slider)
            row.addArrangedSubview(valueLabel)
            channelLabel.setContentHuggingPriority(.required, for: .horizontal)
            valueLabel.setContentHuggingPriority(.required, for: .horizontal)
            card.stack.addArrangedSubview(row)
            sliders[channel] = slider
            values[channel] = valueLabel
        }
        channelSliders[key] = sliders
        channelValues[key] = values
        return card.card
    }

    private func updateSliderValueLabels() {
        for key in WorkspaceTheme.editableColorKeys {
            for (channel, slider) in channelSliders[key] ?? [:] {
                channelValues[key]?[channel]?.text = String(Int(slider.value.rounded()))
            }
        }
    }

    private func loadThemeIntoEditor(_ identifier: String) {
        editingIdentifier = identifier
        let palette = WorkspaceTheme.palette(forIdentifier: identifier)
        let fallback = WorkspaceTheme.palette(forIdentifier: WorkspaceTheme.defaultIdentifier)
        for key in WorkspaceTheme.editableColorKeys {
            let descriptor = palette[key] ?? fallback[key] ?? [:]
            for channel in ["red", "green", "blue"] {
                channelSliders[key]?[channel]?.value = Float(descriptor[channel] ?? 0)
            }
        }
        updateSliderValueLabels()
        updateDraftPreview()
        refreshThemeRows()
    }

    private func refreshThemeRows() {
        clearArrangedSubviews(of: themeListStack)
        themeRows.removeAll()
        let current = WorkspaceTheme.currentIdentifier
        for record in WorkspaceTheme.choices {
            addThemeRow(for: record, applied: record.identifier == current)
        }
        applyTheme()
    }

    private func updateDraftPreview() {
        let palette = draftPalette()
        let preview = WorkspaceTheme.colors(from: palette)
        previewGradient.colors = [preview.backgroundTop.cgColor, preview.backgroundBottom.cgColor]
        previewSurface.backgroundColor = preview.card.withAlphaComponent(0.96)
        previewSurface.layer.borderColor = preview.stroke.cgColor
        previewTitle.textColor = preview.accent
        previewBody.textColor = preview.primary
        previewProgress.trackTintColor = preview.accentAlt.withAlphaComponent(0.18)
        previewProgress.progressTintColor = preview.accentAlt
        previewProgress.progress = 0.72
        wallpaperPreview.image = WorkspaceTheme.artworkImage(identifier: editingIdentifier, palette: palette,
                                                             size: CGSize(width: 1200, height: 675), wallpaper: true)
        wallpaperPreview.layer.borderColor = preview.stroke.cgColor
        densityValueLabel.text = WorkspaceTheme.densityTitle
        for key in WorkspaceTheme.editableColorKeys {
            swatches[key]?.backgroundColor = WorkspaceTheme.uiColor(palette[key])
            swatches[key]?.layer.borderColor = preview.stroke.withAlphaComponent(0.9).cgColor
        }
        let title = WorkspaceTheme.record(forIdentifier: editingIdentifier)?.title ?? "Draft Theme"
        editorThemeLabel.text = "Editing: \(title)"
    }

    @objc private func channelSliderChanged(_ sender: UISlider) {
        sender.value = sender.value.rounded()
        updateSliderValueLabels()
        updateDraftPreview()
    }

    @objc private func themeRowTapped(_ sender: UIButton) {
        guard themeRows.indices.contains(sender.tag) else { return }
        let identifier = themeRows[sender.tag].identifier
        WorkspaceTheme.setCurrentIdentifier(identifier)
        loadThemeIntoEditor(identifier)
    }

    @objc private func saveAsNew() {
        let alert = UIAlertController(title: "Save Theme", message: "Save the current editor palette as a custom Workspace theme.", preferredStyle: .alert)
        alert.addTextField { field in
            field.placeholder = "Theme name"
            field.text = "Custom Theme"
        }
        alert.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        alert.addAction(UIAlertAction(title: "Save", style: .default) { [weak self, weak alert] _ in
            guard let self else { return }
            let typed = alert?.textFields?.first?.text?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
            let identifier = WorkspaceTheme.makeCustomIdentifier()
            WorkspaceTheme.saveCustomTheme(identifier: identifier, title: typed.isEmpty ? "Custom Theme" : typed, palette: self.draftPalette())
            WorkspaceTheme.setCurrentIdentifier(identifier)
            self.loadThemeIntoEditor(identifier)
        })
        (workspaceHost ?? self).present(alert, animated: true)
    }

    @objc private func updateSelected() {
        guard !WorkspaceTheme.isBuiltIn(editingIdentifier) else { return }
        let title = WorkspaceTheme.record(forIdentifier: editingIdentifier)?.title ?? "Custom Theme"
        WorkspaceTheme.saveCustomTheme(identifier: editingIdentifier, title: title, palette: draftPalette())
        WorkspaceTheme.setCurrentIdentifier(editingIdentifier)
        loadThemeIntoEditor(editingIdentifier)
    }

    @objc private func deleteSelected() {
        guard !WorkspaceTheme.isBuiltIn(editingIdentifier) else { return }
        let title = WorkspaceTheme.record(forIdentifier: editingIdentifier)?.title ?? "this theme"
        let alert = UIAlertController(title: "Delete Theme?", message: "Remove \(title) from saved custom themes.", preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        alert.addAction(UIAlertAction(title: "Delete", style: .destructive) { [weak self] _ in
            guard let self else { return }
            WorkspaceTheme.deleteCustomTheme(identifier: self.editingIdentifier)
            WorkspaceTheme.setCurrentIdentifier(WorkspaceTheme.defaultIdentifier)
            self.loadThemeIntoEditor(WorkspaceTheme.currentIdentifier)
        })
        (workspaceHost ?? self).present(alert, animated: true)
    }

    @objc private func densityChanged(_ sender: UISlider) {
        sender.value = (sender.value * 100).rounded() / 100
        WorkspaceTheme.density = CGFloat(sender.value)
        densityValueLabel.text = WorkspaceTheme.densityTitle
    }

    @objc private func gaugeStyleChanged(_ sender: UISegmentedControl) {
        WorkspaceTheme.usesRingGauges = sender.selectedSegmentIndex == 0
    }

    @objc private func applyWallpaper() {
        workspaceHost?.applyWallpaper(identifier: editingIdentifier, palette: draftPalette())
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        contentStack = installScrollingStack(spacing: 12, inset: 14).stack

        let header = stackCard(spacing: 6, inset: 14)
        let eyebrow = themeSecondaryLabel(style: .caption1)
        eyebrow.text = "WORKSPACE THEMES"
        eyebrow.font = .systemFont(ofSize: 11, weight: .semibold)
        activeThemeLabel = themeAccentLabel(style: .title2)
        let headerBody = themePrimaryLabel(style: .body)
        headerBody.text = "Change themes from one native utility, then fine-tune and save custom palettes for every Workspace app."
        [eyebrow, activeThemeLabel, headerBody].forEach(header.stack.addArrangedSubview)

        let library = stackCard(spacing: 8, inset: 14)
        let libraryTitle = themePrimaryLabel(style: .headline)
        libraryTitle.text = "Theme Library"
        let librarySubtitle = themeSecondaryLabel(style: .footnote)
        librarySubtitle.text = "Tap any theme to apply it everywhere and load it into the editor below."
        themeListStack = UIStackView()
        themeListStack.axis = .vertical
        themeListStack.spacing = 6
        [libraryTitle, librarySubtitle, themeListStack].forEach(library.stack.addArrangedSubview)

        let editor = stackCard(spacing: 10, inset: 14)
        let editorTitle = themePrimaryLabel(style: .headline)
        editorTitle.text = "Palette Editor"
        editorThemeLabel = themeSecondaryLabel(style: .footnote)

        previewSurface.translatesAutoresizingMaskIntoConstraints = false
        previewSurface.layer.cornerRadius = 18
        previewSurface.layer.borderWidth = 1
        previewSurface.layer.masksToBounds = true
        previewGradient.startPoint = CGPoint(x: 0.1, y: 0)
        previewGradient.endPoint = CGPoint(x: 0.9, y: 1)
        previewSurface.layer.insertSublayer(previewGradient, at: 0)
        let previewStack = UIStackView()
        previewStack.translatesAutoresizingMaskIntoConstraints = false
        previewStack.axis = .vertical
        previewStack.spacing = 6
        previewSurface.addSubview(previewStack)
        previewTitle.font = .preferredFont(forTextStyle: .title3)
        previewTitle.text = "Preview"
        previewBody.numberOfLines = 0
        previewBody.font = .preferredFont(forTextStyle: .body)
        previewBody.text = "Buttons, cards, text, and monitor bars will all update when you apply this theme."
        previewProgress.translatesAutoresizingMaskIntoConstraints = false
        previewProgress.transform = CGAffineTransform(scaleX: 1, y: 1.4)
        [previewTitle, previewBody, previewProgress].forEach(previewStack.addArrangedSubview)
        NSLayoutConstraint.activate([
            previewSurface.heightAnchor.constraint(greaterThanOrEqualToConstant: 126),
            previewStack.topAnchor.constraint(equalTo: previewSurface.topAnchor, constant: 14),
            previewStack.leadingAnchor.constraint(equalTo: previewSurface.leadingAnchor, constant: 14),
            previewStack.trailingAnchor.constraint(equalTo: previewSurface.trailingAnchor, constant: -14),
            previewStack.bottomAnchor.constraint(equalTo: previewSurface.bottomAnchor, constant: -14),
        ])

        let wallpaper = stackCard(spacing: 6, inset: 14)
        let wallpaperTitle = themePrimaryLabel(style: .subheadline)
        wallpaperTitle.text = "Wallpaper Preview"
        let wallpaperSubtitle = themeSecondaryLabel(style: .caption1)
        wallpaperSubtitle.text = "Generated from the current palette and sized for the current screen."
        wallpaperPreview.translatesAutoresizingMaskIntoConstraints = false
        wallpaperPreview.contentMode = .scaleAspectFill
        wallpaperPreview.clipsToBounds = true
        wallpaperPreview.layer.cornerRadius = 12
        wallpaperPreview.layer.borderWidth = 1
        [wallpaperTitle, wallpaperSubtitle, wallpaperPreview].forEach(wallpaper.stack.addArrangedSubview)
        wallpaperPreview.heightAnchor.constraint(equalToConstant: 124).isActive = true

        let density = stackCard(spacing: 6, inset: 14)
        let densityTitle = themePrimaryLabel(style: .subheadline)
        densityTitle.text = "Utility Density"
        densityValueLabel = themeAccentLabel(style: .subheadline)
        densitySlider.minimumValue = 0
        densitySlider.maximumValue = 1
        densitySlider.value = Float(WorkspaceTheme.density)
        densitySlider.addTarget(self, action: #selector(densityChanged(_:)), for: .valueChanged)
        let densitySubtitle = themeSecondaryLabel(style: .caption1)
        densitySubtitle.text = "Lower values pack the utility cards tighter for smaller screens."
        [densityTitle, densityValueLabel, densitySlider, densitySubtitle].forEach(density.stack.addArrangedSubview)

        let gauge = stackCard(spacing: 6, inset: 14)
        let gaugeTitle = themePrimaryLabel(style: .subheadline)
        gaugeTitle.text = "Gauge Style"
        gaugeControl.selectedSegmentIndex = WorkspaceTheme.usesRingGauges ? 0 : 1
        gaugeControl.addTarget(self, action: #selector(gaugeStyleChanged(_:)), for: .valueChanged)
        let gaugeSubtitle = themeSecondaryLabel(style: .caption1)
        gaugeSubtitle.text = "How the status applets draw CPU, memory, battery, and storage."
        [gaugeTitle, gaugeControl, gaugeSubtitle].forEach(gauge.stack.addArrangedSubview)

        let actionStack = UIStackView()
        actionStack.axis = .vertical
        actionStack.spacing = 8
        func actionRow(_ buttons: [UIButton]) -> UIStackView {
            let row = UIStackView()
            row.axis = .horizontal
            row.spacing = 8
            row.distribution = .fillEqually
            buttons.forEach(row.addArrangedSubview)
            return row
        }
        actionStack.addArrangedSubview(actionRow([
            actionButton(title: "Save As New", action: #selector(saveAsNew)),
            actionButton(title: "Update Selected", action: #selector(updateSelected)),
        ]))
        actionStack.addArrangedSubview(actionRow([
            actionButton(title: "Delete Selected", action: #selector(deleteSelected)),
            actionButton(title: "Apply Wallpaper", action: #selector(applyWallpaper)),
        ]))

        [editorTitle, editorThemeLabel, previewSurface, density.card, gauge.card, actionStack].forEach(editor.stack.addArrangedSubview)
        for key in WorkspaceTheme.editableColorKeys {
            editor.stack.addArrangedSubview(sliderCard(key: key))
        }

        [header.card, wallpaper.card, library.card, editor.card].forEach(contentStack.addArrangedSubview)

        // After every literal that replaced a factory font, so the text scale multiplies the
        // font each label really has.
        let scaled: [UILabel] = [eyebrow, activeThemeLabel, headerBody, libraryTitle, librarySubtitle,
                                 editorTitle, editorThemeLabel, previewTitle, previewBody,
                                 wallpaperTitle, wallpaperSubtitle, densityTitle, densityValueLabel, densitySubtitle,
                                 gaugeTitle, gaugeSubtitle]
        scaled.forEach(recordLabel)

        loadThemeIntoEditor(WorkspaceTheme.currentIdentifier)
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        previewGradient.frame = previewSurface.bounds
    }

    override func themeDidChange() {
        guard contentStack != nil else {
            super.themeDidChange()
            return
        }
        if WorkspaceTheme.record(forIdentifier: editingIdentifier) == nil {
            loadThemeIntoEditor(WorkspaceTheme.currentIdentifier)
        } else {
            refreshThemeRows()
        }
    }

    override func applyTheme() {
        super.applyTheme()
        guard contentStack != nil else { return }
        let colors = theme
        activeThemeLabel.text = "Active Theme: \(WorkspaceTheme.currentTitle)"

        // A selected row is an accent wash over the card, so its text is measured against
        // that composite and not against the card.
        let selectedFill = WorkspaceTheme.wash(colors.accent, alpha: 0.34, over: colors.card)
        let selectedInk = WorkspaceTheme.onSurface(selectedFill)
        let current = WorkspaceTheme.currentIdentifier
        for row in themeRows {
            let selected = row.identifier == current
            if selected {
                row.button.accessibilityTraits.insert(.selected)
            } else {
                row.button.accessibilityTraits.remove(.selected)
            }
            row.button.backgroundColor = selected ? colors.accent.withAlphaComponent(0.34) : colors.cardAlt.withAlphaComponent(0.92)
            row.button.layer.borderWidth = selected ? 2 : 1
            row.button.layer.borderColor = (selected ? colors.accentAlt : colors.stroke).cgColor
            row.button.layer.shadowColor = selected ? colors.accent.cgColor : UIColor.clear.cgColor
            row.button.layer.shadowOpacity = selected ? 0.35 : 0
            row.button.layer.shadowRadius = selected ? 10 : 0
            row.button.layer.shadowOffset = .zero
            row.preview.layer.borderColor = (selected ? colors.accentAlt : colors.stroke).cgColor
            row.title.textColor = selected ? selectedInk : colors.primary
            row.detail.textColor = selected ? WorkspaceTheme.mutedOnSurface(selectedInk, fill: selectedFill) : colors.secondary
        }

        let editingCustom = !WorkspaceTheme.isBuiltIn(editingIdentifier)
        for (index, button) in actionButtons.enumerated() {
            button.backgroundColor = colors.cardAlt.withAlphaComponent(0.92)
            button.layer.borderColor = colors.stroke.cgColor
            button.setTitleColor(colors.accent, for: .normal)
            if index == 1 || index == 2 {
                button.isEnabled = editingCustom
                button.alpha = editingCustom ? 1 : 0.45
            }
        }
        wallpaperPreview.layer.borderColor = colors.stroke.cgColor
        densitySlider.minimumTrackTintColor = colors.accent
        densitySlider.maximumTrackTintColor = colors.accentAlt.withAlphaComponent(0.24)
        densityValueLabel.text = WorkspaceTheme.densityTitle
        gaugeControl.selectedSegmentTintColor = colors.accent
        gaugeControl.setTitleTextAttributes([.foregroundColor: colors.secondary], for: .normal)
        gaugeControl.setTitleTextAttributes([.foregroundColor: WorkspaceTheme.onSurface(colors.accent)], for: .selected)
    }
}
