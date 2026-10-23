// The web agent's site-tool scripts (agent_tools::BuildToolListScript,
// BuildToolListPollScript, BuildToolCallScript, BuildToolCallPollScript in
// src/chrome/browser/molt_ai/agents/agent_tools.cc), run in jsdom against a
// stand-in for Chromium 152's document.modelContext (WebMCP), which jsdom
// doesn't have.
//
// The scripts are printf templates in C++; this reads them from the source
// and fills in their %s the way the C++ does (JSON strings and literals),
// so the test can't drift from the code. In the browser they run in the
// agent's isolated world, where the page can't patch what they call; here
// they share the page's world, so the test only covers their logic.
//
// Run: npm ci && node agent_tools_test.js   (from tools/webui_checks)
'use strict';
const fs = require('fs');
const path = require('path');
const assert = require('assert');
const {JSDOM} = require('jsdom');

const SRC = fs.readFileSync(path.join(
    __dirname, '../../src/chrome/browser/molt_ai/agents/agent_tools.cc'), 'utf8');

// The R"JS(...)JS" template inside the C++ function |name|.
function template(name) {
  const at = SRC.indexOf('std::string ' + name + '(');
  assert(at >= 0, name + ' not found');
  const m = /R"JS\(([\s\S]*?)\)JS"/.exec(SRC.slice(at));
  assert(m, name + ': no R"JS( template');
  return m[1];
}

const FORM_HELPERS = (() => {
  const m = /kFormHelpers\[\] = R"JS\(([\s\S]*?)\)JS"/.exec(SRC);
  assert(m, 'kFormHelpers not found');
  return m[1];
})();

// printf's %s, in order.
function fill(tpl, args) {
  let i = 0;
  const out = tpl.replace(/%s/g, () => args[i++]);
  assert.strictEqual(i, args.length, 'argument count');
  return out;
}

const ORIGIN = 'https://shop.example';
// The document mark the browser passes (an unguessable token there).
const MARK = '3F2A9C0B7D1E4F6A8B5C2D9E0F1A3B4C';
const listScript = (id, mark = MARK) =>
    fill(template('BuildToolListScript'),
         [JSON.stringify(id), JSON.stringify(mark), FORM_HELPERS]);
const listPoll = (id) =>
    fill(template('BuildToolListPollScript'), [JSON.stringify(id)]);
// |tool| as SiteTool keeps it: the mark of the document that listed it
// (|tool.doc|, MARK unless given), listed description, schema (or null),
// the read-only hint, the form's fields (or null); |args| the JSON the
// browser wrote.
const callScript = (id, origin, tool, args) =>
    fill(template('BuildToolCallScript'), [
      JSON.stringify(id),
      JSON.stringify(tool.doc === undefined ? MARK : tool.doc),
      JSON.stringify(origin), JSON.stringify(tool.name),
      JSON.stringify(tool.description),
      tool.schema === null ? 'null' : JSON.stringify(tool.schema),
      tool.readOnly ? 'true' : 'false',
      tool.form === null ? 'null' : JSON.stringify(tool.form),
      JSON.stringify(args), FORM_HELPERS]);
const callPoll = (id) =>
    fill(template('BuildToolCallPollScript'), [JSON.stringify(id)]);

// A page at |url| whose document.modelContext lists |tools| ({name,
// description, inputSchema, annotations}, owned by this window and
// origin unless they say otherwise) and records executeTool() calls.
function page(url, tools, options = {}) {
  const dom = new JSDOM(options.html ||
      '<form toolname="subscribe" tooldescription="Subscribe">' +
      '<input name="email" type="email" autocomplete="email">' +
      '<button type="submit">Go</button></form>',
      {url, runScripts: 'outside-only'});
  const w = dom.window;
  const calls = [];
  const answer = options.answer || ((t, args) => Promise.resolve('ok:' + args));
  const mc = {
    getTools() {
      if (options.listFails) return Promise.reject(new Error('no'));
      return Promise.resolve(tools.map((t) => Object.assign({
        window: t.otherWindow ? {} : w,
        origin: t.origin || new URL(url).origin,
      }, t)));
    },
    executeTool(tool, args) {
      calls.push({tool, args});
      return answer(tool, args);
    },
  };
  if (!options.noModelContext) {
    Object.defineProperty(w.document, 'modelContext', {value: mc});
  }
  // A document the agent already listed carries its mark (in the browser,
  // on the isolated world's global; here the page's).
  if (options.mark !== null) w.__moltToolDocument = options.mark || MARK;
  if (options.insecure) {
    Object.defineProperty(w, 'isSecureContext', {value: false});
  } else if (w.isSecureContext !== true) {
    Object.defineProperty(w, 'isSecureContext', {value: true});
  }
  return {
    w, calls,
    // As the browser gets it: plain JSON-like data (and out of jsdom's
    // realm, so deepStrictEqual compares values).
    run: (js) => JSON.parse(JSON.stringify(w.eval(js))),
  };
}

const settle = () => new Promise((resolve) => setImmediate(resolve));

const SEARCH = {
  name: 'search', description: 'Search the shop.',
  inputSchema: '{"type":"object","properties":{"q":{"type":"string"}}}',
  annotations: {readOnlyHint: true, untrustedContentHint: false},
};

(async () => {
  // ---- Listing ------------------------------------------------------------

  // No WebMCP, or not a secure context: nothing to list, nothing started.
  let p = page(ORIGIN, [SEARCH], {noModelContext: true});
  assert.deepStrictEqual(p.run(listScript('l1')), {state: 'unavailable'});
  assert.deepStrictEqual(p.run(listPoll('l1')), {state: 'gone'});
  p = page('http://shop.example/', [SEARCH], {insecure: true});
  assert.deepStrictEqual(p.run(listScript('l1')), {state: 'unavailable'});

  // The document's own tools, with their fields cut one character over the
  // C++ caps, and a <form toolname>'s fields.
  const tools = [
    SEARCH,
    {name: 'subscribe', description: 'Subscribe.',
     inputSchema: '{"type":"object"}'},
    {name: 'in_frame', description: 'x', otherWindow: true},
    {name: 'other_origin', description: 'x', origin: 'https://ads.example'},
    {name: 'n'.repeat(500), description: 'd'.repeat(9000),
     inputSchema: 's'.repeat(20000)},
  ];
  p = page(ORIGIN, tools);
  assert.deepStrictEqual(p.run(listScript('l2')), {state: 'pending'});
  assert.deepStrictEqual(p.run(listPoll('l2')), {state: 'pending'});
  assert.deepStrictEqual(p.run(listPoll('other')), {state: 'gone'});
  await settle();
  let r = p.run(listPoll('l2'));
  assert.strictEqual(r.state, 'done');
  assert.strictEqual(r.origin, ORIGIN);
  assert.deepStrictEqual(r.tools.map((t) => t.name.length),
                         [6, 9, 129]);
  assert.deepStrictEqual(r.tools[0], {
    name: 'search', description: 'Search the shop.',
    schema: SEARCH.inputSchema, readOnly: true, untrusted: false,
    form: null,
  });
  assert.deepStrictEqual(r.tools[1].form,
                         [['email', 'email'], ['submit', '']]);
  assert.strictEqual(r.tools[1].readOnly, false);
  assert.strictEqual(r.tools[2].description.length, 4097);
  assert.strictEqual(r.tools[2].schema.length, 8193);
  // A tool without a schema lists null.
  p = page(ORIGIN, [{name: 'ping', description: 'Ping.'}]);
  p.run(listScript('l3'));
  await settle();
  assert.strictEqual(p.run(listPoll('l3')).tools[0].schema, null);
  // At most 32, and a failure is a failure.
  p = page(ORIGIN, Array.from({length: 50}, (_, i) => (
      {name: 't' + i, description: 'd'})));
  p.run(listScript('l4'));
  await settle();
  assert.strictEqual(p.run(listPoll('l4')).tools.length, 32);
  p = page(ORIGIN, [SEARCH], {listFails: true});
  p.run(listScript('l5'));
  await settle();
  assert.deepStrictEqual(p.run(listPoll('l5')), {state: 'failed'});
  // A form with more controls than the cap reports one more.
  p = page(ORIGIN, [{name: 'big', description: 'd'}], {html:
      '<form toolname="big">' + '<input>'.repeat(80) + '</form>'});
  p.run(listScript('l6'));
  await settle();
  assert.strictEqual(p.run(listPoll('l6')).tools[0].form.length, 65);
  // In Chromium a form's named controls override its members
  // ([LegacyOverrideBuiltIns]; jsdom doesn't, so the test defines them on
  // the form): the fields read are still the form's own controls.
  {
    const q = page(ORIGIN, [{name: 'login', description: 'Log in.'}], {html:
        '<form toolname="login"><input type="password" name="pw">' +
        '<select name="elements"><option>a</option></select>' +
        '<input name="getAttribute"></form>'});
    const form = q.w.document.querySelector('form');
    Object.defineProperty(form, 'elements',
                          {value: form.querySelector('select')});
    Object.defineProperty(form, 'getAttribute',
                          {value: form.querySelector('[name=getAttribute]')});
    q.run(listScript('l7'));
    await settle();
    assert.deepStrictEqual(q.run(listPoll('l7')).tools[0].form,
                           [['password', ''], ['select-one', ''], ['text', '']]);
  }

  // The document's mark: set by the first listing, kept by the next ones
  // (the chat's, another run's), reported with the tools.
  p = page(ORIGIN, [SEARCH], {mark: null});
  assert.strictEqual(p.w.__moltToolDocument, undefined);
  p.run(listScript('m1', 'FIRST'));
  await settle();
  assert.strictEqual(p.run(listPoll('m1')).document, 'FIRST');
  p.run(listScript('m2', 'SECOND'));
  await settle();
  r = p.run(listPoll('m2'));
  assert.strictEqual(r.state, 'done');
  assert.strictEqual(r.document, 'FIRST');
  assert.strictEqual(p.w.__moltToolDocument, 'FIRST');
  // Without WebMCP nothing is marked.
  p = page(ORIGIN, [SEARCH], {mark: null, noModelContext: true});
  p.run(listScript('m3', 'X'));
  assert.strictEqual(p.w.__moltToolDocument, undefined);

  // ---- Calling ------------------------------------------------------------

  const listed = {name: 'search', description: SEARCH.description,
                  schema: SEARCH.inputSchema, readOnly: true, form: null};
  // A call runs the tool the page still lists, with the browser's JSON.
  p = page(ORIGIN, [SEARCH]);
  assert.strictEqual(p.run(callScript('c1', ORIGIN, listed, '{"q":"shoes"}')),
                     'started');
  assert.deepStrictEqual(p.run(callPoll('c1')), {state: 'pending', result: null});
  await settle();
  assert.deepStrictEqual(p.run(callPoll('c1')),
                         {state: 'done', result: 'ok:{"q":"shoes"}'});
  assert.strictEqual(p.calls.length, 1);
  assert.strictEqual(p.calls[0].tool.name, 'search');
  assert.strictEqual(p.calls[0].args, '{"q":"shoes"}');
  assert.deepStrictEqual(p.run(callPoll('c0')), {state: 'gone'});

  // Not on another origin, nor without WebMCP; nothing runs.
  p = page(ORIGIN, [SEARCH]);
  assert.strictEqual(p.run(callScript('c2', 'https://evil.example', listed, '{}')),
                     'origin_changed');
  p = page(ORIGIN, [SEARCH], {noModelContext: true});
  assert.strictEqual(p.run(callScript('c2', ORIGIN, listed, '{}')), 'unavailable');

  // A tool that is gone, or that the page changed since it was listed
  // (re-registered under the same name), doesn't run.
  const changed = async (tool, expected) => {
    const q = page(ORIGIN, [tool]);
    assert.strictEqual(q.run(callScript('c3', ORIGIN, listed, '{}')), 'started');
    await settle();
    assert.deepStrictEqual(q.run(callPoll('c3')), {state: expected, result: null});
    assert.strictEqual(q.calls.length, 0);
  };
  await changed(Object.assign({}, SEARCH, {name: 'other'}), 'not_found');
  await changed(Object.assign({}, SEARCH, {otherWindow: true}), 'not_found');
  await changed(Object.assign({}, SEARCH, {origin: 'https://ads.example'}),
                'not_found');
  await changed(Object.assign({}, SEARCH, {description: 'Delete everything.'}),
                'changed');
  await changed(Object.assign({}, SEARCH, {inputSchema: '{}'}), 'changed');
  await changed(Object.assign({}, SEARCH, {inputSchema: undefined}), 'changed');
  await changed(Object.assign({}, SEARCH, {annotations: {readOnlyHint: false}}),
                'changed');
  await changed(Object.assign({}, SEARCH, {annotations: undefined}), 'changed');
  // A same-named form: listed without one, called with one.
  {
    const q = page(ORIGIN, [SEARCH], {html:
        '<form toolname="search"><input type="password" name="q"></form>'});
    q.run(callScript('c4', ORIGIN, listed, '{}'));
    await settle();
    assert.strictEqual(q.run(callPoll('c4')).state, 'changed');
    assert.strictEqual(q.calls.length, 0);
  }
  // A form tool whose form gained a password field since it was listed.
  {
    const formTool = {name: 'subscribe', description: 'Subscribe.',
                      inputSchema: '{"type":"object"}'};
    const formListed = {name: 'subscribe', description: 'Subscribe.',
                        schema: '{"type":"object"}', readOnly: false,
                        form: [['email', 'email'], ['submit', '']]};
    let q = page(ORIGIN, [formTool]);
    q.run(callScript('c5', ORIGIN, formListed, '{"email":"a@b.c"}'));
    await settle();
    assert.strictEqual(q.run(callPoll('c5')).state, 'done');
    q = page(ORIGIN, [formTool], {html:
        '<form toolname="subscribe"><input name="email" type="email" ' +
        'autocomplete="email"><input type="password" name="pw">' +
        '<button type="submit">Go</button></form>'});
    q.run(callScript('c6', ORIGIN, formListed, '{"email":"a@b.c"}'));
    await settle();
    assert.strictEqual(q.run(callPoll('c6')).state, 'changed');
    assert.strictEqual(q.calls.length, 0);
    // ... at the end of the form, or a field that became a card field.
    for (const html of [
      '<form toolname="subscribe"><input name="email" type="email" ' +
          'autocomplete="email"><button type="submit">Go</button>' +
          '<input type="password" name="pw"></form>',
      '<form toolname="subscribe"><input name="email" type="email" ' +
          'autocomplete="cc-number"><button type="submit">Go</button></form>',
    ]) {
      q = page(ORIGIN, [formTool], {html});
      q.run(callScript('c6', ORIGIN, formListed, '{"email":"a@b.c"}'));
      await settle();
      assert.strictEqual(q.run(callPoll('c6')).state, 'changed', html);
      assert.strictEqual(q.calls.length, 0);
    }
  }

  // Only in the document that listed the tool: another document (one a
  // frame navigated to, a new document in a reused frame host) has no mark
  // or another one, whatever tools it lists; nothing runs.
  for (const mark of [null, 'OTHER']) {
    const q = page(ORIGIN, [SEARCH], {mark});
    assert.strictEqual(q.run(callScript('c13', ORIGIN, listed, '{}')),
                       'document_changed');
    assert.deepStrictEqual(q.run(callPoll('c13')), {state: 'gone'});
    await settle();
    assert.strictEqual(q.calls.length, 0);
  }
  // A tool the browser holds without a mark never runs.
  {
    const q = page(ORIGIN, [SEARCH]);
    assert.strictEqual(
        q.run(callScript('c14', ORIGIN, Object.assign({}, listed, {doc: ''}),
                         '{}')),
        'document_changed');
    await settle();
    assert.strictEqual(q.calls.length, 0);
  }

  // Answers: any value as text (cut), nothing, a failure.
  p = page(ORIGIN, [SEARCH], {answer: () => Promise.resolve(null)});
  p.run(callScript('c7', ORIGIN, listed, '{}'));
  await settle();
  assert.deepStrictEqual(p.run(callPoll('c7')), {state: 'done', result: null});
  p = page(ORIGIN, [SEARCH], {answer: () => Promise.resolve('x'.repeat(10000))});
  p.run(callScript('c8', ORIGIN, listed, '{}'));
  await settle();
  assert.strictEqual(p.run(callPoll('c8')).result.length, 8192);
  p = page(ORIGIN, [SEARCH], {answer: () => Promise.reject(new Error('boom'))});
  p.run(callScript('c9', ORIGIN, listed, '{}'));
  await settle();
  assert.deepStrictEqual(p.run(callPoll('c9')), {state: 'failed', result: null});
  p = page(ORIGIN, [SEARCH], {answer: () => { throw new TypeError('bad'); }});
  p.run(callScript('c10', ORIGIN, listed, '{}'));
  await settle();
  assert.deepStrictEqual(p.run(callPoll('c10')), {state: 'failed', result: null});
  p = page(ORIGIN, [SEARCH], {listFails: true});
  p.run(callScript('c11', ORIGIN, listed, '{}'));
  await settle();
  assert.deepStrictEqual(p.run(callPoll('c11')), {state: 'failed', result: null});

  // Hostile names, descriptions and arguments stay data.
  const hostile = '"); alert(1); ("</script> ';
  const hostileTool = {name: 'h', description: hostile,
                       inputSchema: hostile, annotations: {readOnlyHint: true}};
  p = page(ORIGIN, [hostileTool]);
  assert.strictEqual(p.run(callScript('c12', ORIGIN,
      {name: 'h', description: hostile, schema: hostile, readOnly: true,
       form: null}, JSON.stringify({q: hostile}))), 'started');
  await settle();
  assert.strictEqual(p.run(callPoll('c12')).state, 'done');
  assert.strictEqual(p.calls[0].args, JSON.stringify({q: hostile}));

  console.log('agent site-tool scripts: listing (own tools only, caps, ' +
              'forms), calling (origin, unchanged tool, answers) ok');
})().catch((e) => {
  console.error(e);
  process.exit(1);
});
