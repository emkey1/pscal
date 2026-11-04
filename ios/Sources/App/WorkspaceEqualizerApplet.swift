// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// Ported by its author from iSH-AOK's Workspace (app/WorkspaceViewController.m at 0f75fee1c)
// and licensed to PSCAL under the Apache License 2.0.

import UIKit

/// The Music player's 10-band equalizer: an on switch, a preset menu and an upright slider per
/// band, ±12 dB. Changes apply as the sliders move, to whatever's playing. The settings are
/// `WorkspaceAudioEqualizer`'s, so Music needn't be open.
@MainActor
final class WorkspaceEqualizerAppletViewController: WorkspaceAppletViewController {
    private let enabledSwitch = UISwitch()
    private let presetButton = UIButton(type: .system)
    private var sliders: [UISlider] = []
    private var gainLabels: [UILabel] = []
    private var observer: NSObjectProtocol?

    private var equalizer: WorkspaceAudioEqualizer { .shared }

    var isEnabledForTesting: Bool { equalizer.isEnabled }
    var gainsForTesting: [Float] { equalizer.gains }
    var presetNameForTesting: String { equalizer.currentPresetName }

    deinit {
        if let observer {
            NotificationCenter.default.removeObserver(observer)
        }
    }

    private static func frequencyTitle(_ hertz: Float) -> String {
        hertz >= 1000 ? String(format: "%gk", hertz / 1000) : String(format: "%g", hertz)
    }

    override func viewDidLoad() {
        super.viewDidLoad()

        let card = themeCardView()
        toolContentView.addSubview(card)

        let onLabel = themePrimaryLabel(style: .subheadline)
        onLabel.text = "Equalizer"
        enabledSwitch.accessibilityLabel = "Equalizer"
        enabledSwitch.accessibilityIdentifier = "workspace.equalizer.enabled"
        enabledSwitch.addTarget(self, action: #selector(enabledChanged), for: .valueChanged)
        presetButton.titleLabel?.font = .systemFont(ofSize: WorkspaceTheme.fontSize(.subheadline))
        presetButton.accessibilityIdentifier = "workspace.equalizer.preset"
        presetButton.addTarget(self, action: #selector(presetTapped), for: .touchUpInside)
        presetButton.setContentHuggingPriority(.required, for: .horizontal)
        let topRow = UIStackView(arrangedSubviews: [onLabel, enabledSwitch, UIView(), presetButton])
        topRow.axis = .horizontal
        topRow.spacing = 10
        topRow.alignment = .center
        // The bands take what height there is.
        topRow.setContentHuggingPriority(.required, for: .vertical)

        let bands = UIStackView()
        bands.axis = .horizontal
        bands.distribution = .fillEqually
        bands.spacing = 2
        for (index, frequency) in WorkspaceAudioEqualizer.frequencies.enumerated() {
            let gain = themeSecondaryLabel(style: .caption2, monospaced: true)
            gain.textAlignment = .center
            gain.numberOfLines = 1
            gain.adjustsFontSizeToFitWidth = true
            gain.minimumScaleFactor = 0.6
            gain.setContentHuggingPriority(.required, for: .vertical)
            gain.accessibilityIdentifier = "workspace.equalizer.gain.\(index)"
            let title = themeSecondaryLabel(style: .caption2)
            title.textAlignment = .center
            title.numberOfLines = 1
            title.adjustsFontSizeToFitWidth = true
            title.minimumScaleFactor = 0.6
            title.setContentHuggingPriority(.required, for: .vertical)
            title.text = Self.frequencyTitle(frequency)

            // A slider turned upright, laid out across its container's height.
            let track = UIView()
            let slider = UISlider()
            slider.minimumValue = -12
            slider.maximumValue = 12
            slider.tag = index
            slider.translatesAutoresizingMaskIntoConstraints = false
            slider.transform = CGAffineTransform(rotationAngle: -.pi / 2)
            slider.accessibilityLabel = "\(Self.frequencyTitle(frequency)) hertz"
            slider.accessibilityIdentifier = "workspace.equalizer.band.\(index)"
            slider.addTarget(self, action: #selector(sliderChanged(_:)), for: .valueChanged)
            track.addSubview(slider)
            NSLayoutConstraint.activate([
                slider.centerXAnchor.constraint(equalTo: track.centerXAnchor),
                slider.centerYAnchor.constraint(equalTo: track.centerYAnchor),
                slider.widthAnchor.constraint(equalTo: track.heightAnchor),
            ])

            let column = UIStackView(arrangedSubviews: [gain, track, title])
            column.axis = .vertical
            column.spacing = 4
            bands.addArrangedSubview(column)
            sliders.append(slider)
            gainLabels.append(gain)
        }

        let stack = UIStackView(arrangedSubviews: [topRow, bands])
        stack.axis = .vertical
        stack.spacing = 10
        stack.translatesAutoresizingMaskIntoConstraints = false
        card.addSubview(stack)
        NSLayoutConstraint.activate([
            card.topAnchor.constraint(equalTo: toolContentView.topAnchor, constant: 8),
            card.leadingAnchor.constraint(equalTo: toolContentView.leadingAnchor, constant: 8),
            card.trailingAnchor.constraint(equalTo: toolContentView.trailingAnchor, constant: -8),
            card.bottomAnchor.constraint(equalTo: toolContentView.bottomAnchor, constant: -8),
            stack.topAnchor.constraint(equalTo: card.topAnchor, constant: 10),
            stack.leadingAnchor.constraint(equalTo: card.leadingAnchor, constant: 12),
            stack.trailingAnchor.constraint(equalTo: card.trailingAnchor, constant: -12),
            stack.bottomAnchor.constraint(equalTo: card.bottomAnchor, constant: -10),
        ])

        observer = NotificationCenter.default.addObserver(forName: .workspaceEqualizerDidChange, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                self?.refresh()
            }
        }
        applyTheme()
        refresh()
    }

    override func applyTheme() {
        super.applyTheme()
        let colors = theme
        enabledSwitch.onTintColor = colors.accent
        presetButton.setTitleColor(colors.accentAlt, for: .normal)
        for slider in sliders {
            slider.minimumTrackTintColor = colors.accent
            slider.maximumTrackTintColor = colors.stroke
        }
    }

    private func refresh() {
        enabledSwitch.isOn = equalizer.isEnabled
        presetButton.setTitle(equalizer.currentPresetName + " \u{25BE}", for: .normal)
        presetButton.accessibilityLabel = "Preset: \(equalizer.currentPresetName)"
        for (index, slider) in sliders.enumerated() {
            let gain = equalizer.gain(forBand: index)
            if !slider.isTracking {
                slider.value = gain
            }
            let text = String(format: "%+.0f", gain)
            gainLabels[index].text = text
            slider.accessibilityValue = "\(text) decibels"
            slider.isEnabled = equalizer.isEnabled
        }
    }

    @objc private func enabledChanged() {
        equalizer.isEnabled = enabledSwitch.isOn
    }

    /// Whole decibels: finer than a listener can place by thumb, and the labels stay honest.
    @objc private func sliderChanged(_ slider: UISlider) {
        equalizer.setGain(slider.value.rounded(), forBand: slider.tag)
    }

    @objc private func presetTapped() {
        let sheet = UIAlertController(title: "Preset", message: nil, preferredStyle: .actionSheet)
        for name in WorkspaceAudioEqualizer.presetNames {
            sheet.addAction(UIAlertAction(title: name, style: .default) { [weak self] _ in
                guard let self else { return }
                self.equalizer.applyPreset(name)
                if !self.equalizer.isEnabled {
                    self.equalizer.isEnabled = true
                }
            })
        }
        sheet.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        // An iPad presents this as a popover, which must have an anchor or UIKit raises.
        sheet.popoverPresentationController?.sourceView = presetButton
        sheet.popoverPresentationController?.sourceRect = presetButton.bounds
        (workspaceHost ?? self).present(sheet, animated: true)
    }
}
