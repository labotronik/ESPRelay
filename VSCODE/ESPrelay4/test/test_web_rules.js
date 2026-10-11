// Execute the actual UI JavaScript with a small DOM and HTTP stub.
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const html = fs.readFileSync(require('node:path').join(__dirname, '../data/index.html'), 'utf8');
const script = html.split('<script>')[1].split('</script>')[0].replace(/\ninit\(\);\s*$/, '\n');
const elements = new Map();
const node = () => ({disabled: false, style: {}, dataset: {}, value: '', textContent: '',
  classList: {add(){}, remove(){}, toggle(){}, contains(){return false;}}, addEventListener(){}});
let fail = false, conflict = false, writes = 0;
const rules = {version: 3, _revision: 'revision', relays: Array.from({length:4}, () => ({expr:{op:'NONE'}})), shutters:[]};
const context = vm.createContext({console, URL, AbortController, setTimeout, clearTimeout,
  setInterval(){}, window:{}, localStorage:{getItem(){return '';},setItem(){},removeItem(){}},
  document:{getElementById(id){if(!elements.has(id)) elements.set(id,node());return elements.get(id);},
    querySelectorAll(){return [];},querySelector(){return node();}},
  fetch:async(path, options) => {
    const write = options?.method === 'PUT';
    if (write) ++writes;
    if (fail && !write) throw new Error('connection lost');
    const status = write && conflict ? 409 : 200;
    return {ok:status===200,status,statusText:'',headers:{get(){return 'application/json';}},
      text:async()=>JSON.stringify(write ? {ok: status===200} : rules)};
  }});
vm.runInContext(script, context);
vm.runInContext('isAuthed=true; lastState={}; originalRenderState=renderState; renderState=()=>{}; renderAll=()=>{}; renderShutterCards=()=>{}; toast=()=>{};', context);
(async()=>{
  await vm.runInContext('loadRules()',context);
  assert.equal(vm.runInContext('rulesLoaded',context), true);
  await vm.runInContext('putRules(rules)',context);
  assert.equal(writes,1);
  fail = true;
  await vm.runInContext('loadRules()',context);
  assert.equal(vm.runInContext('rulesLoaded',context),false);
  assert.equal(vm.runInContext('rules._revision',context),'revision'); // keep last confirmed config
  await vm.runInContext('saveRules()',context);
  await vm.runInContext('window.applyOne(0)',context);
  await vm.runInContext('applyShutter(0)',context);
  await vm.runInContext('removeShutter(0)',context);
  await assert.rejects(vm.runInContext('putRules(rules)',context));
  assert.equal(writes,1); // no defaults sent after a failed GET
  fail = false;
  await vm.runInContext('loadRules()',context);
  conflict = true;
  await assert.rejects(vm.runInContext('putRules(rules)',context));
  assert.equal(vm.runInContext('rulesLoaded',context),false);
  assert.equal(vm.runInContext('escapeHtml(\'<img src=x onerror="alert(1)">\')',context), '&lt;img src=x onerror=&quot;alert(1)&quot;&gt;');
  assert.ok(html.includes('${escapeHtml(sh.name'));
  vm.runInContext('originalRenderState({inputs:[0,0,0,0],relays:[0,0,0,0],override:[-1,-1,-1,-1],modules_output_ok:[0]})',context);
  assert.ok(elements.get('inputsView').innerHTML.includes('Outputs not confirmed'));
  vm.runInContext('originalRenderState({inputs:[0,0,0,0],relays:[0,0,0,0],override:[-1,-1,-1,-1],modules_output_ok:[1]})',context);
  assert.ok(!elements.get('inputsView').innerHTML.includes('Outputs not confirmed'));
  console.log('Web regression checks passed (failed reads, revision conflicts, escaped shutter names).');
})().catch(e=>{console.error(e);process.exitCode=1;});
