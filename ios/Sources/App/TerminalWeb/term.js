// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// The terminal page of PSCAL's iOS app: one hterm, driven by HtermTerminalView.swift.
// Native calls the functions on window.exports and hears back through
// window.webkit.messageHandlers. Typing and pasting never come through here. A hidden
// native text view takes both and hands them straight to the runtime, so this page
// mostly draws, and reports what native has to mirror: the grid, the scroll position
// and the cursor key mode.

'use strict';

(function () {
  const debugging = window.PSCALI_HERM_DEBUG === true;
  const WRITES_TO_TRACE = 5;

  // Rules for hterm's iframe, which term.css can't reach. hterm sets the background and
  // overflow of x-screen inline, hence !important. The background has to stay clear
  // whatever colour is set: the colour behind the text is the native view behind this
  // transparent web view. Overflow is hidden because the scrollbar the user sees is
  // native too; scrollTop still works from script.
  const IFRAME_CSS = `
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
}`;

  const STARTUP_PREFERENCES = {
    // Nothing should show until native's style arrives.
    'background-color': 'transparent',
    'foreground-color': 'transparent',
    'cursor-color': 'transparent',
    // Curses programs switch character sets to draw lines, which UTF-8 mode ignores.
    'terminal-encoding': 'iso-2022',
    'enable-resize-status': false,
    // Selecting must not overwrite the clipboard: copying happens only when asked.
    'copy-on-select': false,
    'enable-clipboard-notice': false,
    'user-css-text': IFRAME_CSS,
    // Native's grid calculation counts on this padding.
    'screen-padding-size': 4,
    // On iOS 16 WebKit can freeze setting up the bell's sound, most readily on a
    // simulator that has just been reset.
    'audible-bell-sound': '',
  };

  function post(name, payload) {
    // postMessage insists on an argument, so a message without data carries null.
    const body = payload === undefined ? null : payload;
    window.webkit.messageHandlers[name].postMessage(body);
  }

  function debugLog(text) {
    if (debugging) {
      post('log', text);
    }
  }

  function bytesFromBase64(encoded) {
    let binary;
    try {
      binary = atob(encoded);
    } catch (error) {
      return null;
    }
    const bytes = new Uint8Array(binary.length);
    for (let i = 0; i < binary.length; i++) {
      bytes[i] = binary.charCodeAt(i);
    }
    return bytes;
  }

  function bytesFromString(text) {
    const bytes = new Uint8Array(text.length);
    for (let i = 0; i < text.length; i++) {
      bytes[i] = text.charCodeAt(i);
    }
    return bytes;
  }

  function finiteOrZero(value) {
    return Number.isFinite(value) ? value : 0;
  }

  // hterm answers an OSC 12 query by reading the cursor colour back exactly as it was set,
  // and it can only report the rgb() form. Native sends #RRGGBB, which would leave the query
  // unanswered and a program waiting for the reply hung. It draws the same either way.
  function inRgbForm(color) {
    if (typeof color !== 'string') {
      return color;
    }
    return lib.colors.normalizeCSS(color) || color;
  }

  // Everything below needs a terminal that has finished starting.
  function terminalIsReady(term) {
    const port = term.scrollPort_;
    const screen = port.getScreenNode();
    const iframeDocument = port.getDocument();

    // hterm buffers what's written to an IO that isn't in front, so the overrides go on
    // one that is.
    const io = term.io.push();

    function reportGrid(columns, rows) {
      // A zero grid would send native straight back here to ask again, for ever.
      if (columns > 0 && rows > 0) {
        post('resize', {columns: columns, rows: rows});
      }
    }

    function currentGrid() {
      return [finiteOrZero(term.screenSize.width), finiteOrZero(term.screenSize.height)];
    }

    // The size of the native view, which is what the grid comes from. hterm would otherwise
    // read its own screen element, and that only catches up with the view once WebKit has
    // laid out again, and means nothing at all while the web view is out of the hierarchy.
    // Until native has a size, the screen element is all there is.
    let hostSize = null;
    const screenSizeFromLayout = port.getScreenSize;

    // hterm's own sums, on native's numbers.
    function screenSizeInside(host) {
      const rightPadding = Math.max(port.screenPaddingSize, port.currentScrollbarWidthPx);
      return {
        height: host.height - (2 * port.screenPaddingSize),
        width: host.width - port.screenPaddingSize - rightPadding,
      };
    }

    port.getScreenSize = function () {
      return hostSize ? screenSizeInside(hostSize) : screenSizeFromLayout.call(this);
    };

    function holdsACell(host) {
      const usable = screenSizeInside(host);
      const cell = port.characterSize;
      return cell.width > 0 && cell.height > 0 &&
             usable.width >= cell.width && usable.height >= cell.height;
    }

    function reflow() {
      // This also recomputes the viewport. hterm only works out the grid from that a moment
      // later on a timer, too late for the answer here, so it's asked now. When its timer
      // comes round it finds nothing left to change.
      port.syncCharacterSize();
      term.onResize_();
      const grid = currentGrid();
      reportGrid(grid[0], grid[1]);
      return grid;
    }

    function reportCursorKeyMode() {
      // Sent every time rather than on a change: native only hears this while a view is
      // bound to the terminal, so a hidden tab would miss a one-off, and a new page has
      // to overwrite what native remembers from the last one.
      post('propUpdate', ['applicationCursor', term.keyboard.applicationCursor]);
    }

    let lastScrollHeight = null;
    let lastScrollTop = null;

    function reportScrollState() {
      // hterm gives its hidden scroll area the height of every row. The element's
      // scrollHeight wouldn't do: it depends on layout that may not have happened yet.
      const height = parseFloat(port.scrollArea_.style.height);
      if (Number.isFinite(height) && height !== lastScrollHeight) {
        lastScrollHeight = height;
        post('newScrollHeight', height);
      }
      const top = screen.scrollTop;
      if (Number.isFinite(top) && top !== lastScrollTop) {
        lastScrollTop = top;
        post('newScrollTop', top);
      }
    }

    let repaintQueued = false;

    function queueRepaint() {
      if (repaintQueued) {
        return;
      }
      repaintQueued = true;
      requestAnimationFrame(() => {
        repaintQueued = false;
        // A big burst right after launch can otherwise leave rows unpainted; dropping the
        // row cache as well as redrawing is what puts them back.
        port.scheduleInvalidate();
        port.scheduleRedraw();
        reportScrollState();
      });
    }

    let writesTraced = 0;

    function traceWrite(bytes) {
      if (writesTraced === 0) {
        const style = iframeDocument.defaultView.getComputedStyle(screen);
        debugLog(`screen style: color=${style.color} background=${style.backgroundColor} ` +
                 `font=${style.fontFamily} ${style.fontSize}`);
      }
      writesTraced += 1;
      const head = Array.from(bytes.subarray(0, 48));
      const text = head.map((b) => (b >= 0x20 && b < 0x7f) ? String.fromCharCode(b) : '.');
      const hex = head.map((b) => b.toString(16).padStart(2, '0'));
      debugLog(`write ${writesTraced}: ${bytes.length} bytes, rows=${term.getRowCount()}, ` +
               `cursor=${term.getCursorRow()},${term.getCursorColumn()}, ` +
               `text="${text.join('')}" hex=${hex.join(' ')}`);
    }

    function writeBytes(bytes) {
      const followingOutput = port.isScrolledEnd;
      // Not decoded here: this keeps decoder state between calls, and native cuts its
      // chunks at arbitrary bytes.
      io.writeUTF8(bytes);
      if (followingOutput) {
        port.scrollRowToBottom(term.getRowCount());
      }
      reportCursorKeyMode();
      queueRepaint();
      if (debugging && writesTraced < WRITES_TO_TRACE) {
        traceWrite(bytes);
      }
    }

    function writeBase64(encoded) {
      if (typeof encoded !== 'string' || encoded === '') {
        return;
      }
      const bytes = bytesFromBase64(encoded);
      if (bytes) {
        writeBytes(bytes);
      }
    }

    // Native stamps each write with its epoch and raises the epoch when a session's view
    // is rebuilt, so a write left over from before the rebuild can't land on the snapshot
    // that replaces it. Native sends its epoch at every load, so this start value is
    // never what the check runs against.
    let epoch = 1;

    window.exports = {
      writeB64ForEpoch(writeEpoch, encoded) {
        if (Number.isFinite(writeEpoch) && writeEpoch === epoch) {
          writeBase64(encoded);
        }
      },

      resetForEpoch(newEpoch) {
        epoch = Number.isFinite(newEpoch) ? newEpoch : epoch + 1;
        term.reset();
        reportCursorKeyMode();
        queueRepaint();
        reportScrollState();
      },

      // The grid's one source: native's measurement of its view, in points. A size that won't
      // hold a single cell measures nothing and changes nothing, and [0, 0] says so.
      measureGrid(width, height) {
        if (!Number.isFinite(width) || !Number.isFinite(height) ||
            !holdsACell({width: width, height: height})) {
          return [0, 0];
        }
        hostSize = {width: width, height: height};
        if (debugging) {
          const laidOut = screen.getBoundingClientRect();
          debugLog(`measure: view ${width}x${height}, screen ${laidOut.width}x${laidOut.height}`);
        }
        return reflow();
      },

      reflowAndGetSize() {
        return reflow();
      },

      getSize() {
        return currentGrid();
      },

      // Native has stopped calling this, because the grid comes from the view. What it sets
      // lasts until the next measurement.
      setGridSize(columns, rows) {
        if (!Number.isFinite(columns) || !Number.isFinite(rows)) {
          return;
        }
        const appliedColumns = Math.max(1, Math.floor(columns));
        const appliedRows = Math.max(1, Math.floor(rows));
        term.realizeSize_(appliedColumns, appliedRows);
        reportCursorKeyMode();
        reportScrollState();
        // Always, even if nothing changed: native takes this as the acknowledgement.
        reportGrid(appliedColumns, appliedRows);
      },

      updateStyle(style) {
        if (!style || typeof style !== 'object') {
          return;
        }
        const preferences = term.getPrefs();
        const setIfGiven = (name, value) => {
          if (value !== undefined) {
            preferences.set(name, value);
          }
        };
        setIfGiven('background-color', style.backgroundColor);
        setIfGiven('foreground-color', style.foregroundColor);
        // There is no separate cursor colour to set.
        setIfGiven('cursor-color', inRgbForm(style.foregroundColor));
        setIfGiven('font-family', style.fontFamily);
        setIfGiven('font-size', style.fontSize);
        setIfGiven('cursor-blink', style.blinkCursor);
        setIfGiven('cursor-shape', style.cursorShape);
        preferences.set('color-palette-overrides', style.colorPaletteOverrides || null);
        debugLog(`style: ${JSON.stringify(style)}`);

        // Font metrics take a moment to settle, and an early measurement can be stale.
        reflow();
        requestAnimationFrame(() => {
          reflow();
          setTimeout(reflow, 24);
        });
      },

      setFocused(focused) {
        if (focused) {
          term.focus();
        } else {
          term.blur();
        }
      },

      newScrollTop(top) {
        if (!Number.isFinite(top)) {
          return;
        }
        screen.scrollTop = top;
        // The browser may round or clamp, so record what it kept. The scroll event this
        // causes then finds nothing new to tell native.
        lastScrollTop = screen.scrollTop;
      },

      scrollToBottom() {
        term.scrollEnd();
      },

      setUserGesture() {
        // Native input never produces the key events hterm watches for this.
        term.accessibilityReader_.hasUserGesture = true;
      },

      copy() {
        term.copySelectionToClipboard();
      },

      releaseMemory(keepRows) {
        const keep = (typeof keepRows === 'number' && keepRows > 0) ? Math.floor(keepRows) : 0;
        const before = term.scrollbackRows_.length;
        if (keep === 0) {
          // Not setScrollbackLimit(0), which would mean no limit at all.
          term.clearScrollback();
        } else {
          // Setting a limit trims to it at once. Putting the old one back leaves the rows
          // gone, with room to grow again.
          const limit = term.scrollbackLimit_;
          term.setScrollbackLimit(keep);
          term.setScrollbackLimit(limit);
        }
        return [before, term.scrollbackRows_.length];
      },

      write(data) {
        let text;
        try {
          text = (data === null || data === undefined) ? '' : String(data);
        } catch (error) {
          return;
        }
        writeBytes(bytesFromString(text));
      },

      writeB64(encoded) {
        writeBase64(encoded);
      },

      getCharacterSize() {
        return [port.characterSize.width, port.characterSize.height];
      },

      clearScrollback() {
        term.clearScrollback();
      },

      setScrollbackLimit(rows) {
        try {
          term.setScrollbackLimit(rows);
        } catch (error) {
          // hterm converts the argument with Number(), which can throw.
        }
      },

      scrollbackRowCount() {
        return term.scrollbackRows_.length;
      },
    };

    io.sendString = (text) => {
      post('sendInput', String(text));
    };

    io.onTerminalResize = (width, height) => {
      const columns = Number(width);
      const rows = Number(height);
      if (Number.isFinite(columns) && Number.isFinite(rows) && columns > 0 && rows > 0) {
        reportGrid(columns, rows);
      } else {
        const grid = currentGrid();
        reportGrid(grid[0], grid[1]);
      }
    };

    // hterm's keyboard handler is deliberately not installed, because typing reaches the
    // session through the native key view alone. hterm therefore never detects a keystroke,
    // and io.onVTKeystroke has nothing to do.

    // Start blank, then bound the scrollback: every row kept past the top of the screen
    // stays alive as DOM nodes, and without a cap the system eventually kills the app.
    term.reset();
    term.setScrollbackLimit(5000);

    // A reset re-measures, whether it came from native or from a program's ESC c.
    const resetTerminal = term.reset;
    term.reset = function () {
      resetTerminal.apply(this, arguments);
      reflow();
    };

    // hterm offers no callback for this and trimming scrollback goes through it too, so
    // wrapping it sees every change to the scroll area.
    const syncScrollHeight = port.syncScrollHeight;
    port.syncScrollHeight = function () {
      syncScrollHeight.apply(this, arguments);
      reportScrollState();
    };
    screen.addEventListener('scroll', reportScrollState);

    // Dragging belongs to the native scroll view. hterm skips its own touch scrolling when
    // an event reads as default-prevented, but really preventing it would also stop the
    // browser's long-press text selection. So the event only claims to be.
    port.onTouch = (event) => {
      Object.defineProperty(event, 'defaultPrevented', {value: true});
    };

    // hterm makes the screen editable so that browsers show a virtual keyboard, and the
    // keyboard here is native. The blur and focus put the new setting in force.
    screen.setAttribute('contenteditable', 'false');
    term.blur();
    term.focus();

    // Native keeps first responder on its key view almost all the time, and each time the
    // web view loses it the screen sees a blur while staying the active element of its
    // document. That isn't the terminal losing focus, and hterm would draw a hollow cursor
    // under someone who is typing. A real blur changes the active element first, so it
    // gets through. This listens on the document because blur doesn't bubble, and capture
    // is the only way to reach it before hterm's listener on the screen.
    iframeDocument.addEventListener('blur', (event) => {
      if (event.target === screen && iframeDocument.activeElement === screen) {
        event.stopPropagation();
      }
    }, true);

    // While the web view holds first responder Tab would otherwise move focus away from
    // the terminal, so on the screen it goes to the program instead. It's the only key the
    // page forwards. Tab with a modifier stays a system shortcut. Capturing at the document
    // runs this ahead of anything on the screen itself, and the browser's default.
    iframeDocument.addEventListener('keydown', (event) => {
      if (event.key !== 'Tab' || event.metaKey || event.ctrlKey || event.altKey ||
          !screen.contains(event.target)) {
        return;
      }
      event.preventDefault();
      event.stopPropagation();
      post('sendInput', event.shiftKey ? '\x1b[Z' : '\t');
    }, true);

    if (debugging) {
      // hterm sets outline: none !important inline, so this has to replace that.
      screen.style.setProperty('outline', '2px solid magenta', 'important');
      debugLog(`ready, grid ${currentGrid().join('x')}`);
    }

    post('load');
    post('syncFocus');

    // Added after load: the focus above is this page's own start-up, and native has to
    // hear about load first.
    screen.addEventListener('focus', () => {
      post('syncFocus');
    });

    // The next two deliberately look at the top-level document. hterm's selection lives in
    // the iframe's document, so as a rule they see nothing, and native's edit menu path
    // stays quiet. Pointing them at the iframe would switch that path on and change what
    // the user sees, which is a decision of its own.
    screen.addEventListener('mousedown', () => {
      const selection = document.getSelection();
      if (!selection || selection.isCollapsed) {
        post('focus');
      }
    });

    let selectionRectSent = false;

    document.addEventListener('selectionchange', () => {
      const selection = document.getSelection();
      let rect = null;
      if (selection && !selection.isCollapsed && selection.rangeCount > 0) {
        const bounds = selection.getRangeAt(0).getBoundingClientRect();
        if (bounds.width !== 0 || bounds.height !== 0) {
          rect = {x: bounds.left, y: bounds.top, width: bounds.width, height: bounds.height};
        }
      }
      if (rect) {
        selectionRectSent = true;
        post('selectionChanged', rect);
      } else if (selectionRectSent) {
        selectionRectSent = false;
        post('selectionChanged', null);
      }
    });
  }

  window.addEventListener('load', () => {
    // hterm's default opens a browser window, which does nothing useful here.
    hterm.openUrl = (url) => {
      post('openLink', String(url));
    };

    // Native sends the style again at every load, and each tab has its own: preferences
    // must not outlive the page or leak between tabs. hterm picks persistent storage for
    // itself during lib.init() unless this is already set.
    hterm.defaultStorage = new lib.Storage.Memory();

    lib.init().then(() => {
      const term = new hterm.Terminal();
      window.term = term;
      term.onTerminalReady = () => {
        terminalIsReady(term);
      };
      const preferences = term.getPrefs();
      Object.keys(STARTUP_PREFERENCES).forEach((name) => {
        preferences.set(name, STARTUP_PREFERENCES[name]);
      });
      term.decorate(document.getElementById('terminal'));
    });
  });
})();
