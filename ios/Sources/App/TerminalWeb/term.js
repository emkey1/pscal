hterm.defaultStorage = new lib.Storage.Memory();
window.onload = async function() {
    await lib.init();
    window.term = new hterm.Terminal();

    // make everything invisible so as to not be embarrassing
    term.getPrefs().set('background-color', 'transparent');
    term.getPrefs().set('foreground-color', 'transparent');
    term.getPrefs().set('cursor-color', 'transparent');

    term.getPrefs().set('terminal-encoding', 'iso-2022');
    term.getPrefs().set('enable-resize-status', false);
    term.getPrefs().set('copy-on-select', false);
    term.getPrefs().set('enable-clipboard-notice', false);
    term.getPrefs().set('user-css-text', termCss);
    term.getPrefs().set('screen-padding-size', 4);
    // Creating and preloading the <audio> element for this sometimes hangs WebKit on iOS 16 for some reason. Can be most easily reproduced by resetting a simulator and starting the app. System logs show Fig hanging while trying to do work.
    term.getPrefs().set('audible-bell-sound', '');

    term.onTerminalReady = onTerminalReady;
    term.decorate(document.getElementById('terminal'));
};

var termCss = `
x-screen {
    background: transparent !important;
    overflow: hidden !important;
    -webkit-tap-highlight-color: transparent;
}
x-row {
  text-rendering: optimizeLegibility;
  font-variant-ligatures: normal;
}
.uri-node {
  text-decoration: underline;
}
`;

function onTerminalReady() {

// Shorthand for JS -> native IPC
const native = new Proxy({}, {
    get(obj, prop) {
        return (...args) => {
            if (args.length == 0)
                args = null;
            else if (args.length == 1)
                args = args[0];
            webkit.messageHandlers[prop].postMessage(args);
        };
    },
});

const debugEnabled = window.PSCALI_HERM_DEBUG === true;
const debugLog = (message) => {
    if (debugEnabled) {
        native.log(message);
    }
};

// Functions for native -> JS
window.exports = {};

term.io.push();
term.reset();

let oldProps = {};
function syncProp(name, value) {
    if (oldProps[name] !== value)
        native.propUpdate(name, value);
}
let decoder = new TextDecoder();
let didLogFirstWrite = false;
let writeLogCount = 0;
exports.write = (data) => {
    const payload = data == null ? '' : String(data);
    const bytes = lib.codec.stringToCodeUnitArray(payload);
    term.io.writeUTF16(decoder.decode(bytes));
    syncProp('applicationCursor', term.keyboard.applicationCursor);
    if (debugEnabled && (!didLogFirstWrite || writeLogCount < 3)) {
        didLogFirstWrite = true;
        writeLogCount += 1;
        const rowCount = term.getRowCount();
        let rowText = '';
        const cursor = term.screen_ && term.screen_.cursorPosition ? term.screen_.cursorPosition : null;
        const cursorInfo = cursor ? ('cursor=' + cursor.row + ',' + cursor.column) : 'cursor=?';
        const cursorRow = cursor ? cursor.row : 0;
        if (rowCount > 0) {
            try {
                rowText = term.getRowText(cursorRow) || '';
            } catch (err) {
                rowText = '<rowText error>';
            }
        }
        const safeText = rowText.replace(/\s+/g, ' ').slice(0, 120);
        const maxDump = Math.min(bytes.length, 64);
        const hex = [];
        const ascii = [];
        for (let i = 0; i < maxDump; i++) {
            const value = bytes[i];
            hex.push(value.toString(16).padStart(2, '0'));
            ascii.push(value >= 0x20 && value <= 0x7E ? String.fromCharCode(value) : '.');
        }
        const active = (term.io && term.io.terminal_ && term.io.terminal_.io === term.io);
        debugLog('writeBytes len=' + bytes.length + ' rows=' + rowCount + ' ' + cursorInfo +
                 ' activeIo=' + active +
                 ' text="' + safeText + '"' +
                 ' hex=' + hex.join('') +
                 ' ascii="' + ascii.join('') + '"');
        if (writeLogCount == 1) {
            const screen = term.scrollPort_ && term.scrollPort_.screen_ ? term.scrollPort_.screen_ : null;
            if (screen && window.getComputedStyle) {
                const style = window.getComputedStyle(screen);
                debugLog('screen style visibility=' + style.visibility +
                         ' opacity=' + style.opacity +
                         ' color=' + style.color +
                         ' fontSize=' + style.fontSize +
                         ' height=' + style.height +
                         ' width=' + style.width);
            }
        }
    }
};
term.io.sendString = term.io.onVTKeyStroke = (data) => {
    native.sendInput(data);
};

// hterm size updates native size
term.io.onTerminalResize = () => native.resize();
exports.getSize = () => [term.screenSize.width, term.screenSize.height];

// selection, copying
term.scrollPort_.screen_.contentEditable = false;
term.blur();
term.focus();
exports.copy = () => term.copySelectionToClipboard();

let lastSelectionRect = null;
function getSelectionRect() {
    const sel = document.getSelection();
    if (!sel || sel.rangeCount === 0 || sel.isCollapsed) return null;
    const rect = sel.getRangeAt(0).getBoundingClientRect();
    if (!rect || (rect.width === 0 && rect.height === 0)) return null;
    return {x: rect.left, y: rect.top, width: rect.width, height: rect.height};
}
document.addEventListener('selectionchange', () => {
    const rect = getSelectionRect();
    if (rect) {
        native.selectionChanged(rect);
        lastSelectionRect = rect;
    } else if (lastSelectionRect) {
        native.selectionChanged(null);
        lastSelectionRect = null;
    }
});

// focus
// This listener blocks blur events that come in because the webview has lost first responder
term.scrollPort_.screen_.addEventListener('blur', (e) => {
    if (e.target.ownerDocument.activeElement == e.target) {
        e.stopPropagation();
    }
}, {capture: true});
term.scrollPort_.screen_.addEventListener('mousedown', (e) => {
    // Taps while there is a selection should be left to the selection view
    if ((document.getSelection().rangeCount != 0) &&
        (!document.getSelection().isCollapsed)) return;
    native.focus();
});
exports.setFocused = (focus) => {
    if (focus)
        term.focus();
    else
        term.blur();
};
term.scrollPort_.screen_.addEventListener('focus', (e) => native.syncFocus());

// scrolling
// Disable hterm builtin touch scrolling
term.scrollPort_.onTouch = (e) => {
    // Convince hterm that we called preventDefault() and that it shouldn't do more handling, but don't actually call it because that would break text selection
    Object.defineProperty(e, 'defaultPrevented', {value: true});
};
// Scroll to bottom wrapper
exports.scrollToBottom = () => term.scrollEnd();
// Set scroll position
exports.newScrollTop = (y) => {
    // two lines instead of one because the value you read out of scrollTop can be different from the value you write into it
    term.scrollPort_.screen_.scrollTop = y;
    lastScrollTop = term.scrollPort_.screen_.scrollTop;
};

// Send scroll height and position to native code
let lastScrollHeight, lastScrollTop;
function syncScroll() {
    const scrollHeight = parseFloat(term.scrollPort_.scrollArea_.style.height);
    if (scrollHeight != lastScrollHeight)
        native.newScrollHeight(scrollHeight);
    lastScrollHeight = scrollHeight;

    const scrollTop = term.scrollPort_.screen_.scrollTop;
    if (scrollTop != lastScrollTop)
        native.newScrollTop(scrollTop);
    lastScrollTop = scrollTop;
}

const realSyncScrollHeight = hterm.ScrollPort.prototype.syncScrollHeight;
hterm.ScrollPort.prototype.syncScrollHeight = function() {
    realSyncScrollHeight.call(this);
    syncScroll();
};
term.scrollPort_.screen_.addEventListener('scroll', syncScroll);

exports.updateStyle = ({foregroundColor, backgroundColor, fontFamily, fontSize, colorPaletteOverrides, blinkCursor, cursorShape}) => {
    term.getPrefs().set('background-color', backgroundColor);
    term.getPrefs().set('foreground-color', foregroundColor);
    term.getPrefs().set('cursor-color', foregroundColor);
    term.getPrefs().set('font-family', fontFamily);
    term.getPrefs().set('font-size', fontSize);
    term.getPrefs().set('color-palette-overrides', colorPaletteOverrides);
    term.getPrefs().set('cursor-blink', blinkCursor);
    term.getPrefs().set('cursor-shape', cursorShape);
    if (debugEnabled) {
        debugLog('updateStyle fg=' + foregroundColor + ' bg=' + backgroundColor);
    }
};

exports.getCharacterSize = () => {
    return [term.scrollPort_.characterSize.width, term.scrollPort_.characterSize.height];
};

exports.clearScrollback = () => term.clearScrollback();
exports.setUserGesture = () => term.accessibilityReader_.hasUserGesture = true;

hterm.openUrl = (url) => native.openLink(url);

native.load();
native.syncFocus();
if (debugEnabled) {
    debugLog('terminalReady');
    if (term.scrollPort_ && term.scrollPort_.screen_) {
        term.scrollPort_.screen_.style.outline = '1px solid #FF0000';
    }
}

}
