// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// Ported by its author from iSH-AOK's Workspace (app/WorkspaceImageViewer.m at 0f75fee1c) and
// licensed to PSCAL under the Apache License 2.0.

import ImageIO
import UIKit

/// Decodes at a pixel budget, so a huge photo can never balloon into a full-resolution bitmap
/// and get the app killed for its memory. That's a correctness requirement, not an
/// optimization. The image's true size comes from its properties, without decoding it.
private enum WorkspaceImageDecoder {
    enum Outcome {
        case image(UIImage, pixelSize: CGSize)
        case failure(String)
    }

    /// Past these it isn't attempted, since a small file can claim more pixels than the app can
    /// hold. Measured on the way to a 4096-pixel thumbnail: PNG, JPEG and HEIC decode a band at
    /// a time (a 289-megapixel PNG peaked at 243 MB, a 144-megapixel HEIC at 26 MB), but a
    /// 144-megapixel BMP peaked at 1.8 GB and a TIFF at 830 MB.
    private static let maxBandedPixels = 256_000_000.0
    private static let maxOtherPixels = 64_000_000.0
    private static let bandedFormats: Set<String> = ["public.png", "public.jpeg", "public.heic", "public.heif"]

    static func decode(url: URL, maxPixelSize: Int) -> Outcome {
        let sourceOptions = [kCGImageSourceShouldCache: false] as CFDictionary
        guard let source = CGImageSourceCreateWithURL(url as CFURL, sourceOptions), CGImageSourceGetCount(source) > 0 else {
            return .failure("This doesn’t look like an image PSCAL can display.")
        }
        let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, sourceOptions) as? [CFString: Any]
        let width = (properties?[kCGImagePropertyPixelWidth] as? NSNumber)?.doubleValue ?? 0
        let height = (properties?[kCGImagePropertyPixelHeight] as? NSNumber)?.doubleValue ?? 0
        let banded = CGImageSourceGetType(source).map { bandedFormats.contains($0 as String) } ?? false
        if width * height > (banded ? maxBandedPixels : maxOtherPixels) {
            return .failure(String(format: "This image is %.0f × %.0f, which is too large to display here.", width, height))
        }
        let thumbnailOptions: [CFString: Any] = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceThumbnailMaxPixelSize: maxPixelSize,
            // Respect the EXIF orientation.
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceShouldCacheImmediately: true,
        ]
        guard let thumbnail = CGImageSourceCreateThumbnailAtIndex(source, 0, thumbnailOptions as CFDictionary) else {
            return .failure("Couldn’t decode this image.")
        }
        return .image(UIImage(cgImage: thumbnail), pixelSize: CGSize(width: width, height: height))
    }
}

/// One image at a time, with the others in its folder a step away, pinch and double-tap zoom,
/// fit or actual size, and the share sheet.
@MainActor
final class WorkspaceImageViewerAppletViewController: WorkspaceAppletViewController, WorkspaceFileOpenable, WorkspaceStatefulTool, WorkspaceFocusable, UIScrollViewDelegate {
    static let supportedExtensions: Set<String> = ["png", "jpg", "jpeg", "gif", "bmp", "tiff", "tif", "heic", "heif", "webp", "ico"]

    private static let toolbarHeight: CGFloat = 44
    private static let minPixelBudget: CGFloat = 512
    private static let maxPixelBudget: CGFloat = 4096

    private let toolbarView = UIView()
    private let prevButton = UIButton(type: .system)
    private let nextButton = UIButton(type: .system)
    private let titleLabel = UILabel()
    private let zoomToggleButton = UIButton(type: .system)
    private let shareButton = UIButton(type: .system)
    private let reloadButton = UIButton(type: .system)

    private let scrollView = UIScrollView()
    private let imageView = UIImageView()
    private let statusLabel = UILabel()
    // "Fit" pins the image view to the scroll view's frame, and the image scales down inside
    // it. "Actual size" gives it the decoded bitmap's size. Only one pair is ever active: a
    // relative and an absolute constraint can't be one constraint with a changing multiplier.
    private var imageFitWidth: NSLayoutConstraint!
    private var imageFitHeight: NSLayoutConstraint!
    private var imageActualWidth: NSLayoutConstraint!
    private var imageActualHeight: NSLayoutConstraint!

    private(set) var currentPath: String?
    /// The image paths in the current folder, for previous and next.
    private var siblingPaths: [String] = []
    /// May exceed the decoded bitmap.
    private var originalPixelSize: CGSize = .zero
    private var showingActualSize = false
    private(set) var statusMessage: String?
    var hasImageForTesting: Bool { imageView.image != nil }
    /// Discards stale work. The operations also stop it: flipping through a folder of large
    /// photos with only this guard left every skipped one decoding and every scan walking.
    private var loadGeneration = 0
    private var siblingsOperation: WorkspaceFileOperation?
    private var decodeOperation: WorkspaceFileOperation?
    // Serial, so skipping through a folder holds one decode in memory, not one per image.
    private let decodeQueue = DispatchQueue(label: "com.pscal.workspace.imageviewer", qos: .userInitiated)
    private var lastLayoutSize: CGSize = .zero

    deinit {
        siblingsOperation?.cancel()
        decodeOperation?.cancel()
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        buildToolbar()
        buildImageArea()
        statusLabel.translatesAutoresizingMaskIntoConstraints = false
        statusLabel.numberOfLines = 0
        statusLabel.textAlignment = .center
        statusLabel.font = .systemFont(ofSize: 15)
        statusLabel.accessibilityIdentifier = "workspace.imageviewer.status"
        toolContentView.addSubview(statusLabel)

        NSLayoutConstraint.activate([
            toolbarView.topAnchor.constraint(equalTo: toolContentView.topAnchor),
            toolbarView.leadingAnchor.constraint(equalTo: toolContentView.leadingAnchor),
            toolbarView.trailingAnchor.constraint(equalTo: toolContentView.trailingAnchor),
            toolbarView.heightAnchor.constraint(equalToConstant: Self.toolbarHeight),

            scrollView.topAnchor.constraint(equalTo: toolbarView.bottomAnchor),
            scrollView.leadingAnchor.constraint(equalTo: toolContentView.leadingAnchor),
            scrollView.trailingAnchor.constraint(equalTo: toolContentView.trailingAnchor),
            scrollView.bottomAnchor.constraint(equalTo: toolContentView.bottomAnchor),

            statusLabel.centerXAnchor.constraint(equalTo: scrollView.centerXAnchor),
            statusLabel.centerYAnchor.constraint(equalTo: scrollView.centerYAnchor),
            statusLabel.leadingAnchor.constraint(greaterThanOrEqualTo: toolContentView.leadingAnchor, constant: 20),
            statusLabel.trailingAnchor.constraint(lessThanOrEqualTo: toolContentView.trailingAnchor, constant: -20),
        ])
        setStatus("Open an image from the File Manager.")
    }

    /// Windows resize continuously. The fit constraints tie the image's size to the scroll
    /// view's frame, and solving them under a live zoom makes the view jump, so a real size
    /// change resets to the unzoomed fit. Zooming doesn't change these bounds, so this never
    /// fires mid-pinch.
    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        let size = view.bounds.size
        guard size != lastLayoutSize else { return }
        lastLayoutSize = size
        if scrollView.zoomScale != scrollView.minimumZoomScale {
            scrollView.setZoomScale(scrollView.minimumZoomScale, animated: false)
        }
        centerImage()
    }

    // Key commands are collected along the first-responder chain, and nothing here takes text
    // input, so this takes part directly for the arrows to work.
    override var canBecomeFirstResponder: Bool { true }

    override var keyCommands: [UIKeyCommand]? {
        let previous = UIKeyCommand(title: "Previous Image", action: #selector(navigatePrev), input: UIKeyCommand.inputLeftArrow)
        let next = UIKeyCommand(title: "Next Image", action: #selector(navigateNext), input: UIKeyCommand.inputRightArrow)
        // Without this the focus engine takes unmodified arrows before they get here.
        previous.wantsPriorityOverSystemBehavior = true
        next.wantsPriorityOverSystemBehavior = true
        return [previous, next]
    }

    // MARK: - Building

    private func buildToolbar() {
        toolbarView.translatesAutoresizingMaskIntoConstraints = false
        toolContentView.addSubview(toolbarView)

        configureIconButton(prevButton, symbol: "chevron.left", label: "Previous Image",
                            hint: "Shows the previous image in the folder", identifier: "workspace.imageviewer.previous",
                            action: #selector(navigatePrev))
        configureIconButton(nextButton, symbol: "chevron.right", label: "Next Image",
                            hint: "Shows the next image in the folder", identifier: "workspace.imageviewer.next",
                            action: #selector(navigateNext))

        titleLabel.translatesAutoresizingMaskIntoConstraints = false
        titleLabel.font = .systemFont(ofSize: 13, weight: .medium)
        titleLabel.lineBreakMode = .byTruncatingTail
        titleLabel.textAlignment = .center
        titleLabel.text = "Image"
        titleLabel.accessibilityIdentifier = "workspace.imageviewer.title"
        titleLabel.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)

        zoomToggleButton.translatesAutoresizingMaskIntoConstraints = false
        zoomToggleButton.titleLabel?.font = .systemFont(ofSize: 12, weight: .semibold)
        zoomToggleButton.addTarget(self, action: #selector(toggleZoomMode), for: .touchUpInside)
        zoomToggleButton.isEnabled = false
        zoomToggleButton.accessibilityIdentifier = "workspace.imageviewer.zoom"
        zoomToggleButton.accessibilityHint = "Switches between fitting the image to the window and its actual size"
        zoomToggleButton.setContentHuggingPriority(.required, for: .horizontal)
        updateZoomToggleTitle()

        configureIconButton(shareButton, symbol: "square.and.arrow.up", label: "Share",
                            hint: "Opens the share sheet for this image", identifier: "workspace.imageviewer.share",
                            action: #selector(shareCurrentImage))
        configureIconButton(reloadButton, symbol: "arrow.clockwise", label: "Reload",
                            hint: "Reads this image again", identifier: "workspace.imageviewer.reload",
                            action: #selector(reload))

        let stack = UIStackView(arrangedSubviews: [prevButton, nextButton, titleLabel, zoomToggleButton, shareButton, reloadButton])
        stack.translatesAutoresizingMaskIntoConstraints = false
        stack.axis = .horizontal
        stack.alignment = .center
        stack.spacing = 8
        toolbarView.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: toolbarView.leadingAnchor, constant: 10),
            stack.trailingAnchor.constraint(equalTo: toolbarView.trailingAnchor, constant: -10),
            stack.centerYAnchor.constraint(equalTo: toolbarView.centerYAnchor),
        ])
    }

    private func configureIconButton(_ button: UIButton, symbol: String, label: String, hint: String, identifier: String, action: Selector) {
        button.translatesAutoresizingMaskIntoConstraints = false
        button.setImage(UIImage(systemName: symbol), for: .normal)
        button.addTarget(self, action: action, for: .touchUpInside)
        button.widthAnchor.constraint(greaterThanOrEqualToConstant: 28).isActive = true
        button.setContentHuggingPriority(.required, for: .horizontal)
        button.accessibilityLabel = label
        button.accessibilityHint = hint
        button.accessibilityIdentifier = identifier
        button.isEnabled = false
    }

    private func buildImageArea() {
        scrollView.translatesAutoresizingMaskIntoConstraints = false
        scrollView.delegate = self
        scrollView.minimumZoomScale = 1
        scrollView.maximumZoomScale = 4
        scrollView.backgroundColor = .clear
        scrollView.accessibilityIdentifier = "workspace.imageviewer.scroll"
        toolContentView.addSubview(scrollView)

        let doubleTap = UITapGestureRecognizer(target: self, action: #selector(handleDoubleTap(_:)))
        doubleTap.numberOfTapsRequired = 2
        scrollView.addGestureRecognizer(doubleTap)

        imageView.translatesAutoresizingMaskIntoConstraints = false
        imageView.contentMode = .scaleAspectFit
        imageView.isUserInteractionEnabled = true
        imageView.accessibilityIdentifier = "workspace.imageviewer.image"
        scrollView.addSubview(imageView)

        imageFitWidth = imageView.widthAnchor.constraint(equalTo: scrollView.frameLayoutGuide.widthAnchor)
        imageFitHeight = imageView.heightAnchor.constraint(equalTo: scrollView.frameLayoutGuide.heightAnchor)
        imageActualWidth = imageView.widthAnchor.constraint(equalToConstant: 0)
        imageActualHeight = imageView.heightAnchor.constraint(equalToConstant: 0)
        NSLayoutConstraint.activate([
            imageView.topAnchor.constraint(equalTo: scrollView.contentLayoutGuide.topAnchor),
            imageView.leadingAnchor.constraint(equalTo: scrollView.contentLayoutGuide.leadingAnchor),
            imageView.trailingAnchor.constraint(equalTo: scrollView.contentLayoutGuide.trailingAnchor),
            imageView.bottomAnchor.constraint(equalTo: scrollView.contentLayoutGuide.bottomAnchor),
            imageFitWidth,
            imageFitHeight,
        ])
    }

    // MARK: - Theme

    override func applyTheme() {
        super.applyTheme()
        let colors = theme
        // The theme's text colours are only guaranteed against its card surfaces, and a
        // letterboxed image wants a neutral backdrop rather than the gradient.
        toolContentView.backgroundColor = colors.card.withAlphaComponent(0.95)
        titleLabel.textColor = colors.primary
        statusLabel.textColor = colors.secondary
        [prevButton, nextButton, shareButton, reloadButton].forEach { $0.tintColor = colors.accent }
        zoomToggleButton.setTitleColor(colors.accent, for: .normal)
    }

    // MARK: - Workspace

    func workspaceOpenFile(atPath path: String) {
        loadViewIfNeeded()
        load(path)
    }

    func workspaceToolStateForSaving() -> [String: Any]? {
        currentPath.map { ["path": $0] }
    }

    func workspaceRestoreToolState(_ state: [String: Any]) {
        // A file deleted since then shows the load error, honestly.
        if let path = state["path"] as? String, !path.isEmpty {
            workspaceOpenFile(atPath: path)
        }
    }

    /// Takes the keyboard for the arrows, and finds out if the file has gone while this window
    /// was behind another.
    func workspaceToolDidBecomeFrontmost() {
        becomeFirstResponder()
        guard let path = currentPath, imageView.image != nil else { return }
        let generation = loadGeneration
        WorkspaceFileService.shared.item(path) { [weak self] result in
            guard let self, self.loadGeneration == generation else { return }
            switch result {
            case .success(let item):
                if item.kind != .regular {
                    self.showFailure(self.message(for: WorkspaceFileError.system(ENOENT)))
                }
            case .failure(let error):
                self.showFailure(self.message(for: error))
            }
        }
    }

    // MARK: - Navigation

    @objc private func reload() {
        if let currentPath {
            load(currentPath)
        }
    }

    private var currentSiblingIndex: Int? {
        currentPath.flatMap { siblingPaths.firstIndex(of: $0) }
    }

    @objc private func navigatePrev() {
        guard let index = currentSiblingIndex, index > 0 else { return }
        load(siblingPaths[index - 1])
    }

    @objc private func navigateNext() {
        guard let index = currentSiblingIndex, index + 1 < siblingPaths.count else { return }
        load(siblingPaths[index + 1])
    }

    private func updatePrevNextButtons() {
        let index = currentSiblingIndex
        prevButton.isEnabled = index.map { $0 > 0 } ?? false
        nextButton.isEnabled = index.map { $0 + 1 < siblingPaths.count } ?? false
    }

    private func loadSiblings(of path: String, generation: Int) {
        // Guarded by the generation like the image is, or a scan cancelled by the next load
        // could land afterwards and set the folder that's just been left.
        siblingsOperation = WorkspaceFileService.shared.listDirectory(WorkspaceFileService.parent(of: path)) { [weak self] result in
            guard let self, self.loadGeneration == generation else { return }
            self.siblingsOperation = nil
            guard case .success(let items) = result else { return }
            let supported = Self.supportedExtensions
            // A folder can hold many thousands of entries.
            DispatchQueue.global(qos: .userInitiated).async { [weak self] in
                let paths = items.filter { $0.kind == .regular && supported.contains(($0.name as NSString).pathExtension.lowercased()) }
                    .map(\.path)
                    .sorted { $0.caseInsensitiveCompare($1) == .orderedAscending }
                DispatchQueue.main.async { [weak self] in
                    MainActor.assumeIsolated {
                        guard let self, self.loadGeneration == generation else { return }
                        self.siblingPaths = paths
                        self.updatePrevNextButtons()
                    }
                }
            }
        }
    }

    // MARK: - Loading

    /// The folder scan and the decode belong to one displayed image, so they stop together
    /// whenever a new one starts.
    private func cancelPendingWork() {
        siblingsOperation?.cancel()
        siblingsOperation = nil
        decodeOperation?.cancel()
        decodeOperation = nil
    }

    private func load(_ path: String) {
        cancelPendingWork()
        currentPath = path
        titleLabel.text = (path as NSString).lastPathComponent
        reloadButton.isEnabled = true
        shareButton.isEnabled = false
        zoomToggleButton.isEnabled = false
        // Before the next decode, so the two are never in memory together.
        imageView.image = nil
        originalPixelSize = .zero
        setStatus("Loading…")
        updatePrevNextButtons()

        loadGeneration += 1
        let generation = loadGeneration
        loadSiblings(of: path, generation: generation)
        // ImageIO would block forever on a FIFO, and has no good word for an empty file.
        WorkspaceFileService.shared.item(path) { [weak self] result in
            guard let self, self.loadGeneration == generation else { return }
            switch result {
            case .success(let item) where item.kind != .regular:
                self.showFailure(self.message(for: item.isBrokenSymlink ? WorkspaceFileError.system(ENOENT) : WorkspaceFileError.notRegularFile))
            case .success(let item) where item.size == 0:
                self.showFailure("This file is empty.")
            case .success:
                self.startDecode(of: path, generation: generation)
            case .failure(let error):
                self.showFailure(self.message(for: error))
            }
        }
    }

    /// About twice the window's size in screen pixels, within bounds.
    private func currentDecodePixelBudget() -> Int {
        var dimension = max(view.bounds.width, view.bounds.height)
        if dimension <= 0 {
            // Not laid out yet.
            dimension = 512
        }
        let scale = max(1, traitCollection.displayScale)
        return Int(min(max(dimension * scale * 2, Self.minPixelBudget), Self.maxPixelBudget))
    }

    private func startDecode(of path: String, generation: Int) {
        let budget = currentDecodePixelBudget()
        let operation = WorkspaceFileOperation()
        decodeOperation = operation
        decodeQueue.async { [weak self] in
            guard !operation.isCancelled else { return }
            let outcome = WorkspaceImageDecoder.decode(url: WorkspaceFileService.fileURL(path), maxPixelSize: budget)
            DispatchQueue.main.async { [weak self] in
                MainActor.assumeIsolated {
                    guard let self, self.loadGeneration == generation else { return }
                    self.decodeOperation = nil
                    switch outcome {
                    case .image(let image, let pixelSize):
                        self.originalPixelSize = pixelSize
                        self.display(image)
                    case .failure(let message):
                        self.showFailure(message)
                    }
                }
            }
        }
    }

    private func message(for error: Error) -> String {
        if case WorkspaceFileError.system(ENOENT) = error {
            return "“\(currentPath.map { ($0 as NSString).lastPathComponent } ?? "This file")” isn’t there any more. It may have been deleted or moved."
        }
        return error.localizedDescription.isEmpty ? "Couldn’t open this file." : error.localizedDescription
    }

    private func display(_ image: UIImage) {
        imageView.image = image
        imageActualWidth.constant = image.size.width
        imageActualHeight.constant = image.size.height
        showingActualSize = false
        applyZoomMode()
        updateZoomToggleTitle()
        shareButton.isEnabled = true
        zoomToggleButton.isEnabled = true
        setStatus(nil)
        updateTitleLabel()
    }

    private func showFailure(_ message: String) {
        imageView.image = nil
        originalPixelSize = .zero
        shareButton.isEnabled = false
        zoomToggleButton.isEnabled = false
        updateTitleLabel()
        setStatus(message)
    }

    private func updateTitleLabel() {
        let name = currentPath.map { ($0 as NSString).lastPathComponent } ?? ""
        if originalPixelSize.width > 0 && originalPixelSize.height > 0 {
            titleLabel.text = String(format: "%@ — %.0f × %.0f", name, originalPixelSize.width, originalPixelSize.height)
        } else {
            titleLabel.text = name
        }
    }

    private func setStatus(_ message: String?) {
        statusMessage = message
        statusLabel.isHidden = message == nil
        statusLabel.text = message
    }

    // MARK: - Zoom

    @objc private func toggleZoomMode() {
        showingActualSize.toggle()
        applyZoomMode()
        updateZoomToggleTitle()
    }

    private func updateZoomToggleTitle() {
        zoomToggleButton.setTitle(showingActualSize ? "Fit" : "100%", for: .normal)
        zoomToggleButton.accessibilityLabel = showingActualSize ? "Fit to window" : "View actual size"
    }

    private func applyZoomMode() {
        // The old pair deactivates first, or the two conflict for a moment.
        let old = showingActualSize ? [imageFitWidth!, imageFitHeight!] : [imageActualWidth!, imageActualHeight!]
        let new = showingActualSize ? [imageActualWidth!, imageActualHeight!] : [imageFitWidth!, imageFitHeight!]
        NSLayoutConstraint.deactivate(old)
        NSLayoutConstraint.activate(new)
        scrollView.setZoomScale(1, animated: false)
        toolContentView.layoutIfNeeded()
        centerImage()
    }

    @objc private func handleDoubleTap(_ gesture: UITapGestureRecognizer) {
        guard imageView.image != nil else { return }
        if scrollView.zoomScale > scrollView.minimumZoomScale {
            scrollView.setZoomScale(scrollView.minimumZoomScale, animated: true)
        } else {
            let point = gesture.location(in: imageView)
            let scale = scrollView.maximumZoomScale
            let size = CGSize(width: scrollView.bounds.width / scale, height: scrollView.bounds.height / scale)
            scrollView.zoom(to: CGRect(x: point.x - size.width / 2, y: point.y - size.height / 2, width: size.width, height: size.height), animated: true)
        }
    }

    func viewForZooming(in scrollView: UIScrollView) -> UIView? {
        imageView
    }

    func scrollViewDidZoom(_ scrollView: UIScrollView) {
        centerImage()
    }

    /// A scroll view doesn't center content smaller than its bounds on its own.
    private func centerImage() {
        let bounds = scrollView.bounds.size
        let frame = imageView.frame
        let insetX = max((bounds.width - frame.width) / 2, 0)
        let insetY = max((bounds.height - frame.height) / 2, 0)
        scrollView.contentInset = UIEdgeInsets(top: insetY, left: insetX, bottom: insetY, right: insetX)
    }

    // MARK: - Sharing

    @objc private func shareCurrentImage() {
        guard let path = currentPath else { return }
        // The file itself goes to the share sheet, so make sure it's still there.
        WorkspaceFileService.shared.item(path) { [weak self] result in
            guard let self, self.currentPath == path else { return }
            switch result {
            case .success:
                let activity = UIActivityViewController(activityItems: [WorkspaceFileService.fileURL(path)], applicationActivities: nil)
                // An iPad presents this as a popover, which must have an anchor or UIKit raises.
                activity.popoverPresentationController?.sourceView = self.shareButton
                activity.popoverPresentationController?.sourceRect = self.shareButton.bounds
                self.present(activity, animated: true)
            case .failure(let error):
                let alert = UIAlertController(title: "Can’t Share", message: self.message(for: error), preferredStyle: .alert)
                alert.addAction(UIAlertAction(title: "OK", style: .default))
                (self.workspaceHost ?? self).present(alert, animated: true)
            }
        }
    }
}
