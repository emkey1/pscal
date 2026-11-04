// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// The grid rules of the iOS terminal page, section 13 of its spec: term.js run against the
// real sums in hterm_all.js (hterm.Terminal.prototype.onResize_, realizeSize_ and the
// ScrollPort's getScreenSize). Everything that needs a browser's layout is a stand-in, so this
// says nothing about drawing. That still takes a real WebKit.
//
// Usage: node Tests/ios_terminalweb_page_test.js ios/Sources/App/TerminalWeb

'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const webDir = process.argv[2];
if (!webDir) {
  console.error('usage: ios_terminalweb_page_test.js <TerminalWeb directory>');
  process.exit(2);
}

// Anything hterm_all.js touches while it defines itself. It has no browser to talk to here,
// and nothing below calls what it would have got from one.
function inertValue() {
  return new Proxy(function () {}, {
    get: (target, name) => (name === Symbol.toPrimitive ? () => '' : inertValue()),
    apply: () => inertValue(),
    construct: () => inertValue(),
  });
}

function loadHterm(context) {
  vm.runInContext(fs.readFileSync(path.join(webDir, 'hterm_all.js'), 'utf8'), context,
                  {filename: 'hterm_all.js'});
  // Its declarations stay script-scoped, so they have to be fetched from inside.
  return vm.runInContext('({lib, hterm})', context);
}

const CELL = {width: 8, height: 16};
const PADDING = 4;
const SCROLLBAR = 16;

// A page brought up as term.js brings it up, on a stand-in terminal with hterm's real sums.
function bringUpPage() {
  const messages = [];
  const listeners = {};
  const handlers = {};
  for (const name of ['load', 'syncFocus', 'focus', 'sendInput', 'resize', 'propUpdate',
                      'newScrollHeight', 'newScrollTop', 'openLink', 'selectionChanged',
                      'log']) {
    handlers[name] = {postMessage: (body) => messages.push({name, body})};
  }

  const sandbox = {
    console,
    Promise,
    setTimeout: () => 0,
    clearTimeout: () => {},
    requestAnimationFrame: () => 0,
    atob: (text) => Buffer.from(text, 'base64').toString('latin1'),
    navigator: inertValue(),
    document: inertValue(),
    Blob: class {},
    TextEncoder,
    TextDecoder,
  };
  sandbox.window = sandbox;
  sandbox.webkit = {messageHandlers: handlers};
  sandbox.addEventListener = (name, listener) => {
    listeners[name] = listener;
  };
  const context = vm.createContext(sandbox);
  const {lib, hterm} = loadHterm(context);

  const eventTarget = {addEventListener() {}, setAttribute() {}, contains: () => true};
  // What layout would say: the web view's enormous starting frame, as in R16.
  const layout = {width: 10000, height: 10000};
  const port = Object.create(hterm.ScrollPort.prototype);
  Object.assign(port, {
    screenPaddingSize: PADDING,
    currentScrollbarWidthPx: SCROLLBAR,
    characterSize: {width: CELL.width, height: CELL.height},
    isScrolledEnd: false,
    scrollArea_: {style: {height: '0px'}},
    screen_: Object.assign({
      scrollTop: 0,
      getBoundingClientRect: () => ({width: layout.width, height: layout.height}),
    }, eventTarget),
    getScreenNode() { return this.screen_; },
    getDocument: () => Object.assign({
      defaultView: {getComputedStyle: () => ({})},
      activeElement: null,
    }, eventTarget),
    syncCharacterSize() { port.characterSyncs = (port.characterSyncs || 0) + 1; },
    syncScrollHeight() {},
    scheduleInvalidate() {},
    scheduleRedraw() {},
    scrollRowToBottom() {},
  });

  const io = Object.create(hterm.Terminal.IO.prototype);
  const term = Object.create(hterm.Terminal.prototype);
  Object.assign(term, {
    scrollPort_: port,
    screenSize: {width: 80, height: 24},
    keyboard: {applicationCursor: false},
    accessibilityReader_: {},
    scrollbackRows_: [],
    prefs_: {get: () => false},
    io: {
      push() {
        term.io = io;
        return io;
      },
    },
    reset() {},
    setScrollbackLimit() {},
    focus() {},
    blur() {},
    getPrefs: () => ({set() {}, get: () => false}),
    realizeWidth_(columns) { this.screenSize.width = columns; },
    realizeHeight_(rows) { this.screenSize.height = rows; },
    updateCssCharsize_() {},
    restyleCursor_() {},
    scheduleSyncCursorPosition_() {},
    overlaySize() {},
    scrollEnd() {},
    getRowCount: () => 24,
    decorate() { this.onTerminalReady(); },
  });

  // What term.js will build for itself.
  hterm.Terminal = function () { return term; };
  lib.init = () => Promise.resolve();
  lib.Storage = {Memory: function () {}};
  sandbox.document = {getElementById: () => ({}), getSelection: () => null,
                      addEventListener() {}};

  vm.runInContext(fs.readFileSync(path.join(webDir, 'term.js'), 'utf8'), context,
                  {filename: 'term.js'});
  return {sandbox, term, port, messages, listeners, layout};
}

const lastResize = (page) => {
  const reports = page.messages.filter((message) => message.name === 'resize');
  return reports.length ? reports[reports.length - 1].body : null;
};

const asGrid = (value) => Array.from(value);

// What hterm's formula gives for a view of this size.
const gridFor = (width, height) => [
  Math.floor((width - PADDING - Math.max(PADDING, SCROLLBAR)) / CELL.width),
  Math.floor((height - 2 * PADDING) / CELL.height),
];

const tests = [];
const test = (name, body) => tests.push({name, body});

test('a measurement is worked out from the view, not from layout', (page) => {
  const grid = page.sandbox.exports.measureGrid(400, 300);
  assert.deepStrictEqual(asGrid(grid), gridFor(400, 300));
  assert.deepStrictEqual(asGrid(page.sandbox.exports.getSize()), gridFor(400, 300));
  assert.deepStrictEqual(lastResize(page).columns, gridFor(400, 300)[0]);
});

test('layout catching up late changes nothing', (page) => {
  page.sandbox.exports.measureGrid(400, 300);
  page.layout.width = 123;
  page.layout.height = 77;
  // What hterm does when the iframe finally gets its resize event.
  page.term.onResize_();
  assert.deepStrictEqual(asGrid(page.sandbox.exports.getSize()), gridFor(400, 300));
});

test('until a view has a size, layout is all there is', (page) => {
  page.sandbox.exports.reflowAndGetSize();
  assert.deepStrictEqual(asGrid(page.sandbox.exports.getSize()), gridFor(10000, 10000));
});

test('a size that holds no cell measures nothing and changes nothing', (page) => {
  page.sandbox.exports.measureGrid(400, 300);
  page.messages.length = 0;
  for (const [width, height] of [[0, 0], [1, 1], [400, 0], [0, 300], [20, 300], [400, 12],
                                 [NaN, 300], [400, Infinity], ['x', null]]) {
    assert.deepStrictEqual(asGrid(page.sandbox.exports.measureGrid(width, height)), [0, 0]);
  }
  assert.deepStrictEqual(asGrid(page.sandbox.exports.getSize()), gridFor(400, 300));
  assert.strictEqual(page.messages.filter((message) => message.name === 'resize').length, 0);
  // And it isn't remembered as the view's size either.
  page.term.onResize_();
  assert.deepStrictEqual(asGrid(page.sandbox.exports.getSize()), gridFor(400, 300));
});

test('a grid set from outside does not outlast the next measurement', (page) => {
  page.sandbox.exports.measureGrid(400, 300);
  page.sandbox.exports.setGridSize(152, 4);
  assert.deepStrictEqual(asGrid(page.sandbox.exports.getSize()), [152, 4]);
  assert.strictEqual(lastResize(page).columns, 152);
  assert.strictEqual(lastResize(page).rows, 4);
  page.sandbox.exports.reflowAndGetSize();
  assert.deepStrictEqual(asGrid(page.sandbox.exports.getSize()), gridFor(400, 300));
  assert.deepStrictEqual(lastResize(page).columns, gridFor(400, 300)[0]);
});

test('a reset re-measures, from native or from a program', (page) => {
  page.sandbox.exports.measureGrid(400, 300);
  page.sandbox.exports.setGridSize(152, 4);
  page.sandbox.exports.resetForEpoch(7);
  assert.deepStrictEqual(asGrid(page.sandbox.exports.getSize()), gridFor(400, 300));

  page.sandbox.exports.setGridSize(120, 50);
  // hterm's handler for ESC c calls this.
  page.term.reset();
  assert.deepStrictEqual(asGrid(page.sandbox.exports.getSize()), gridFor(400, 300));
});

test('a rotation is one measurement away', (page) => {
  page.sandbox.exports.measureGrid(390, 844);
  page.sandbox.exports.measureGrid(844, 390);
  assert.deepStrictEqual(asGrid(page.sandbox.exports.getSize()), gridFor(844, 390));
  assert.deepStrictEqual(lastResize(page).columns, gridFor(844, 390)[0]);
  assert.deepStrictEqual(lastResize(page).rows, gridFor(844, 390)[1]);
});

test('the same measurement again leaves the grid alone', (page) => {
  page.sandbox.exports.measureGrid(400, 300);
  const before = page.term.screenSize.width;
  let realized = 0;
  const realizeWidth = page.term.realizeWidth_;
  page.term.realizeWidth_ = function (columns) {
    realized += 1;
    realizeWidth.call(this, columns);
  };
  page.sandbox.exports.measureGrid(400, 300);
  assert.strictEqual(realized, 0);
  assert.strictEqual(page.term.screenSize.width, before);
});

test('every function native may call is there before load is posted', (page) => {
  const load = page.messages.findIndex((message) => message.name === 'load');
  assert.notStrictEqual(load, -1);
  for (const name of ['writeB64ForEpoch', 'resetForEpoch', 'measureGrid', 'reflowAndGetSize',
                      'getSize', 'setGridSize', 'updateStyle', 'setFocused', 'newScrollTop',
                      'scrollToBottom', 'setUserGesture', 'copy', 'releaseMemory']) {
    assert.strictEqual(typeof page.sandbox.exports[name], 'function', name);
  }
});

(async () => {
  let failures = 0;
  for (const {name, body} of tests) {
    const page = bringUpPage();
    page.listeners.load();
    // Let lib.init()'s promise run term.js's start-up.
    await new Promise((resolve) => setImmediate(resolve));
    try {
      body(page);
      console.log(`ok: ${name}`);
    } catch (error) {
      failures += 1;
      console.error(`FAIL: ${name}\n${error.stack || error}`);
    }
  }
  process.exit(failures === 0 ? 0 : 1);
})();
