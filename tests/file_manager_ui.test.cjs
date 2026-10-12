// Lightweight DOM/network harness: no npm packages or device writes required.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
class Element {
  constructor() { this.children=[]; this.value=''; this.hidden=false; this.dataset={}; this.textContent=''; this.classList={toggle(){},add(){},remove(){}}; }
  append(...children) { this.children.push(...children); }
  replaceChildren(...children) { this.children=children; }
  get firstElementChild() { return this.children[0]; }
  setAttribute() {} addEventListener() {} focus() {} select() {} showModal() {} close() {} click() {} remove() {}
}
const elements=new Map();
const get=id=>{if(!elements.has(id))elements.set(id,new Element());return elements.get(id)};
get('empty').append(new Element());
const places=['/','/apps','/music'].map(folder=>{const element=new Element();element.dataset.place=folder;return element});
const calls=[],uploads=[];
const folderMap=new Map([
  ['/',[{name:'apps',directory:true,size:0},{name:'music',directory:true,size:0},{name:'<img src=x>.txt',directory:false,size:3}]],
  ['/apps',[]], ['/music',[{name:'song.mp3',directory:false,size:5}]], ['/music/Albums',[]]
]);
let failAction=null;
async function fetchMock(url,options){const parsed=new URL(url,'http://device');const action=parsed.pathname.split('/').pop(),params=Object.fromEntries(parsed.searchParams);calls.push({action,...params,method:options.method});if(failAction===action)return {ok:false,text:async()=>'Simulated failure'};if(action==='list'){if(!folderMap.has(params.path))return {ok:false,text:async()=>'Folder not found'};return {ok:true,json:async()=>folderMap.get(params.path)}}return {ok:true,text:async()=>'OK'}}
class XHR { constructor(){this.upload={}} open(method,url){this.url=url} send(file){uploads.push({url:this.url,name:file.name});this.status=200;this.responseText='Uploaded';this.upload.onprogress?.({loaded:file.size,total:file.size});this.onload()} }
const context=vm.createContext({document:{getElementById:get,querySelectorAll:()=>places,createElement:()=>new Element(),body:new Element()},window:{addEventListener(){}},fetch:fetchMock,XMLHttpRequest:XHR,URL,URLSearchParams,Blob,console,setTimeout});
const html=fs.readFileSync(path.join(__dirname,'../components/file_server/web/index.html'),'utf8');
vm.runInContext(html.match(/<script>([\s\S]*?)<\/script>/)[1],context);
const run=source=>vm.runInContext(source,context);
const tick=()=>new Promise(resolve=>setImmediate(resolve));
(async()=>{
  await tick();
  assert.equal(run('current'),'/');
  assert.equal(get('upload').disabled,false);
  assert.equal(get('entries').children.length,3);
  // Names become text, never markup.
  assert.equal(get('entries').children[2].children[1].children[0].children[1].textContent,'<img src=x>.txt');
  await run("navigate('/music')");
  assert.equal(run('current'),'/music');
  assert.equal(run('parent(current)'),'/');
  await run("uploadFiles([{name:'a + b.txt',size:17}])");
  const upload=new URL(uploads[0].url,'http://device');
  assert.equal(upload.searchParams.get('path'),'/music/a + b.txt');
  assert.equal(run('busy'),false);
  assert.equal(get('transfer-title').textContent,'Upload complete');
  run("selected.add('song.mp3')");get('cut').onclick();
  await run("navigate('/music/Albums')");
  get('paste').onclick();await tick();
  assert.ok(calls.some(call=>call.action==='move'&&call.path==='/music/song.mp3'&&call.to==='/music/Albums/song.mp3'));
  assert.equal(run('clipboard.length'),0);
  // Failed moves retain the clipboard, so users can retry.
  run("clipboard=['/music/song.mp3']");failAction='move';get('paste').onclick();await tick();
  assert.equal(run('clipboard.length'),1);
  assert.equal(get('notice').hidden,false);
  assert.match(get('notice').textContent,/Simulated failure/);
  failAction=null;
  await run("navigate('/does-not-exist')");
  assert.equal(run('current'),'/music/Albums');
  assert.equal(run('busy'),false);
  for(const name of ['../escape','.wifi-creds.bin','bad/name','bad\\name','trailing.','trailing ','bad:stream','']){
    context.testName=name;assert.equal(run('validName(testName)'),false,name);
  }
  for(const name of ['hello world.txt','a+b.txt','music..mp3','Türkçe.mp3']){
    context.testName=name;assert.equal(run('validName(testName)'),true,name);
  }
  // Search and selection do not change the destination directory.
  await run("navigate('/')");get('search').value='apps';get('search').oninput();
  assert.equal(get('entries').children.length,1);
  get('select-all').checked=true;get('select-all').onchange({target:get('select-all')});
  assert.equal(run('selected.size'),1);
  assert.equal(get('rename').disabled,false);
  assert.equal(run('current'),'/');
  console.log('PASS: navigation, nested uploads, move/paste, retry state, safe names, search, selection, and script syntax.');
})().catch(error=>{console.error(error);process.exitCode=1});
