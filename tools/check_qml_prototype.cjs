/* Offline DOM checks for the design prototype, never for the native plugin.
 * Setup: npm --prefix build/qml-prototype-check install --ignore-scripts --no-audit --no-fund jsdom@26.1.0
 * Run: node tools/check_qml_prototype.cjs
 * jsdom does not render layout; dialogs and timers below are controlled doubles.
 */
"use strict";
const fs=require("node:fs"),path=require("node:path"),assert=require("node:assert/strict");
const root=path.resolve(__dirname,"..");
const {JSDOM,VirtualConsole}=require(path.join(root,"build/qml-prototype-check/node_modules/jsdom"));
const html=fs.readFileSync(path.join(root,"docs/qml/prototype/index.html"),"utf8");
const source=fs.readFileSync(path.join(root,"docs/qml/prototype/prototype.js"),"utf8");
let assertions=0;const results=[];
function equal(actual,expected){assertions++;assert.deepEqual(actual,expected);}
function ok(value,message){assertions++;assert.ok(value,message);}
function setup(){
  const errors=[],queue=[],virtualConsole=new VirtualConsole();virtualConsole.on("jsdomError",error=>errors.push(error.message));
  const dom=new JSDOM(html,{url:"https://envhosts-prototype.invalid/",runScripts:"outside-only",virtualConsole});
  const w=dom.window;w.structuredClone=structuredClone;w.setTimeout=fn=>{queue.push(fn);return queue.length;};
  w.HTMLDialogElement.prototype.showModal=function(){this.open=true;};w.HTMLDialogElement.prototype.close=function(){this.open=false;};
  w.eval(source+'\nwindow.__snapshot=(k,which)=>JSON.stringify(canonical(sessions[k][which],k));');
  const q=selector=>{const el=w.document.querySelector(selector);assert.ok(el,`Missing ${selector}`);return el;};
  return {w,q,errors,dom,click:selector=>q(selector).click(),key:(selector,key,extra={})=>q(selector).dispatchEvent(new w.KeyboardEvent("keydown",{key,bubbles:true,cancelable:true,...extra})),
    fill(selector,value){const el=q(selector);el.focus();el.value=value;el.dispatchEvent(new w.Event("input",{bubbles:true}));},
    change(selector){q(selector).dispatchEvent(new w.Event("change",{bubbles:true}));},
    disk:k=>JSON.parse(w.__snapshot(k,"disk")),
    work:k=>JSON.parse(w.__snapshot(k,"work")),
    flush(){while(queue.length)queue.shift()();},
    addPath(){this.click('[data-command="add-part"]');const el=[...w.document.querySelectorAll('[data-part-input]')].at(-1);el.value="C:\\SDK\\bin";el.dispatchEvent(new w.KeyboardEvent("keydown",{key:"Enter",bubbles:true,cancelable:true}));}
  };
}
function run(id,name,fn){const h=setup();try{fn(h);equal(h.errors,[]);results.push({id,name,status:"PASS"});}catch(error){results.push({id,name,status:"FAIL",error:error.message});}finally{h.dom.window.close();}}
run("R01","Initial state and prototype isolation",h=>{
  equal(h.q("#dirty-badge").textContent,"已同步");ok(h.q("#save").disabled);ok(!h.q("#content").textContent.includes("应用到变量"));
  equal(h.disk("env").find(r=>r.id==="u-path").value,"%USERPROFILE%\\bin;C:\\Tools\\bin");
  ok(h.q('meta[http-equiv="Content-Security-Policy"]').content.includes("connect-src 'none'"));
});
run("R02","PATH staging, review cancellation, and explicit save",h=>{
  const before=h.disk("env");h.addPath();ok(h.work("env")[0].value.endsWith(";C:\\SDK\\bin"));equal(h.disk("env"),before);
  h.click("#save");ok(h.q("#modal").open);ok(h.q("#modal-body").textContent.includes("C:\\SDK\\bin"));h.click('[data-dialog="cancel"]');equal(h.disk("env"),before);ok(!h.q("#save").disabled);
  h.click("#save");h.click('[data-dialog="save"]');ok(h.q("#save").disabled);equal(h.disk("env"),before);h.flush();
  ok(h.disk("env")[0].value.endsWith(";C:\\SDK\\bin"));equal(h.q("#dirty-badge").textContent,"已同步");ok(h.q("#task-path").textContent.includes("已保存"));
});
run("R03","Staged item deletion survives row and page changes",h=>{
  h.click('[aria-label="删除第 2 项"]');equal(h.work("env")[0].value,"%USERPROFILE%\\bin");ok(h.q(".part.deleted"));
  h.click('[data-row="u-java"]');h.click('[data-page="backup"]');h.click('[data-page="env"]');h.click('[data-row="u-path"]');
  equal(h.q(".part.deleted input").value,"C:\\Tools\\bin");ok(!h.q("#modal").open);h.click('[data-part-action="restore"]');equal(h.q("#dirty-badge").textContent,"已同步");
});
run("R04","Item identity, move, delete, undo, and redo",h=>{
  h.addPath();h.click('[aria-label="上移第 3 项"]');equal(h.work("env")[0].value,"%USERPROFILE%\\bin;C:\\SDK\\bin;C:\\Tools\\bin");
  h.click('[aria-label="删除第 2 项"]');equal(h.w.document.querySelectorAll(".part").length,2);
  h.click("#undo");equal(h.w.document.querySelectorAll(".part").length,3);h.click("#redo");equal(h.w.document.querySelectorAll(".part").length,2);
  equal(h.disk("env")[0].value,"%USERPROFILE%\\bin;C:\\Tools\\bin");
});
run("R05","hosts toggle, undo, redo and save",h=>{
  h.click('[data-task="hosts"]');h.click('[data-toggle-host="h-api"]');equal(h.q('[data-toggle-host="h-api"]').getAttribute("aria-checked"),"false");ok(h.disk("hosts").find(r=>r.id==="h-api").enabled);
  h.click("#undo");equal(h.q('[data-toggle-host="h-api"]').getAttribute("aria-checked"),"true");h.click("#redo");h.click("#save");h.click('[data-dialog="save"]');h.flush();
  equal(h.disk("hosts").find(r=>r.id==="h-api").enabled,false);ok(h.disk("hosts").find(r=>r.id==="h-local").enabled);ok(h.q("#task-hosts").textContent.includes("已保存"));
});
run("R06","Backup preview, reversible restore and scoped save",h=>{
  const system=h.disk("env").find(r=>r.scope==="system");h.click('[data-task="restore"]');ok(h.q("#content").textContent.includes("OLD_SDK"));h.click("#restore-backup");
  ok(h.work("env").some(r=>r.id==="u-old"));ok(!h.disk("env").some(r=>r.id==="u-old"));h.click("#undo");ok(!h.work("env").some(r=>r.id==="u-old"));
  h.click("#redo");h.click("#save");ok(h.q("#modal-body").textContent.includes("将删除：DEV_MODE"));h.click('[data-dialog="save"]');h.flush();
  ok(h.disk("env").some(r=>r.id==="u-old"));equal(h.disk("env").find(r=>r.scope==="system"),system);ok(h.q("#task-restore").textContent.includes("已保存"));
});
run("R07","External conflict keeps draft and requires a fresh review",h=>{
  h.addPath();const mine=h.work("env")[0].value;h.click("#external");ok(!h.q("#banner").hidden);equal(h.work("env")[0].value,mine);h.click("#save");
  equal(h.q("#modal-title").textContent,"目标已被外部修改");h.click('[data-dialog="cancel"]');ok(!h.q("#banner").hidden);
  h.click('[data-command="conflict"]');h.click('[data-dialog="overwrite"]');ok(h.q("#modal-title").textContent.startsWith("保存"));h.click('[data-dialog="save"]');h.flush();equal(h.disk("env")[0].value,mine);
});
run("R08","Save failure preserves edits and retry succeeds",h=>{
  const before=h.disk("env");h.addPath();h.click("#fail-next");h.click("#save");h.click('[data-dialog="save"]');h.flush();
  equal(h.disk("env"),before);ok(h.q("#banner").textContent.includes("保存失败"));ok(!h.q("#save").disabled);ok(h.work("env")[0].value.endsWith(";C:\\SDK\\bin"));
  h.click('[data-command="retry"]');h.click('[data-dialog="save"]');h.flush();equal(h.q("#dirty-badge").textContent,"已同步");
});
run("R09","Read-only, filtering and theme changes keep edits",h=>{
  h.addPath();const mine=h.work("env");h.click("#readonly");ok(h.q("#save").disabled);ok(h.q('[data-command="add-part"]').disabled);h.click("#theme");ok(h.w.document.body.classList.contains("dark"));
  h.fill("#filter","JAVA");ok(h.q("#content").textContent.includes("JAVA_HOME"));equal(h.work("env"),mine);h.fill("#filter","no match");ok(h.q("#content").textContent.includes("没有匹配"));h.click('[data-command="clear-filter"]');h.click("#readonly");ok(!h.q("#save").disabled);
});
run("R10","Esc cancels active text, Enter stages, and row delete restores",h=>{
  const first=h.q('[aria-label="分隔项 1"]');h.fill('[aria-label="分隔项 1"]',"changed");h.key('[aria-label="分隔项 1"]',"Escape");equal(first.value,"%USERPROFILE%\\bin");equal(h.q("#dirty-badge").textContent,"已同步");
  h.fill('[aria-label="分隔项 1"]',"D:\\bin");h.key('[aria-label="分隔项 1"]',"Enter");equal(h.work("env")[0].value,"D:\\bin;C:\\Tools\\bin");
  h.click('[data-command="delete-row"]');ok(!h.work("env").some(r=>r.id==="u-path"));h.click('[data-command="delete-row"]');equal(h.work("env")[0].value,"D:\\bin;C:\\Tools\\bin");
});
run("R11","Reload cancellation keeps edits; confirmed discard restores target",h=>{
  const before=h.disk("env");h.addPath();h.click("#reload");h.click('[data-dialog="cancel"]');ok(h.work("env")[0].value.endsWith(";C:\\SDK\\bin"));
  h.click("#reload");h.click('[data-dialog="reload"]');equal(h.work("env"),before);equal(h.q("#dirty-badge").textContent,"已同步");
});
run("R12","Clean external change reloads and keyboard save opens review",h=>{
  h.click("#external");equal(h.work("env"),h.disk("env"));equal(h.q("#dirty-badge").textContent,"已同步");h.addPath();h.key("#content","s",{ctrlKey:true});ok(h.q("#modal").open);ok(h.q("#modal-title").textContent.startsWith("保存"));
});
run("R13","Pending input marks dirty before commit and Esc clears it",h=>{
  const before=h.disk("env");h.fill('[aria-label="分隔项 1"]',"D:\\pending");
  equal(h.q("#dirty-badge").textContent,"1 项未保存");ok(!h.q("#save").disabled);equal(h.work("env"),before);
  h.key('[aria-label="分隔项 1"]',"Escape");equal(h.q("#dirty-badge").textContent,"已同步");ok(h.q("#save").disabled);equal(h.disk("env"),before);
});
const report={kind:"Offline jsdom prototype checks; not browser or QML tests",assertions,passed:results.filter(r=>r.status==="PASS").length,total:results.length,results};
const output=path.join(root,"build/qml-prototype-check/results.json");fs.writeFileSync(output,JSON.stringify(report,null,2)+"\n");
console.log(JSON.stringify(report,null,2));if(report.passed!==report.total)process.exitCode=1;
