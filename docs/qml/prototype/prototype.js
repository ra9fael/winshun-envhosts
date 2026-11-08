/* Offline interaction prototype. All targets and backups exist only in memory. */
"use strict";
const $ = (selector) => document.querySelector(selector);
const clone = (value) => structuredClone(value);
const escapeHtml = (value) => String(value ?? "").replace(/[&<>"']/g, (c) => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[c]));
let nextId = 10;
function parts(values) { return values.map(value => ({id:`p${nextId++}`,value,isNew:false,deleted:false})); }
const initialEnv = [
  {id:"u-path",name:"Path",scope:"user",type:"REG_EXPAND_SZ",parts:parts(["%USERPROFILE%\\bin","C:\\Tools\\bin"]),deleted:false},
  {id:"u-java",name:"JAVA_HOME",scope:"user",type:"REG_SZ",value:"C:\\SDK\\jdk-21",deleted:false},
  {id:"u-mode",name:"DEV_MODE",scope:"user",type:"REG_SZ",value:"local",deleted:false},
  {id:"s-path",name:"Path",scope:"system",type:"REG_EXPAND_SZ",parts:parts(["%SystemRoot%\\System32"]),deleted:false}
];
const initialHosts = [
  {id:"h-local",ip:"127.0.0.1",name:"localhost",comment:"",enabled:true,deleted:false},
  {id:"h-api",ip:"127.0.0.1",name:"api.test auth.test",comment:"local gateway",enabled:true,deleted:false},
  {id:"h-preview",ip:"10.0.0.24",name:"preview.test",comment:"preview",enabled:false,deleted:false}
];
const backupEnv = clone(initialEnv).filter(row => row.id !== "u-mode");
backupEnv[0].parts = parts(["%USERPROFILE%\\bin","C:\\SDK\\bin"]);
backupEnv.push({id:"u-old",name:"OLD_SDK",scope:"user",type:"REG_SZ",value:"C:\\OldSDK",deleted:false});
function session(rows) { return {base:clone(rows),disk:clone(rows),work:clone(rows),undo:[],redo:[],revision:0,readOnly:false,failNext:false,error:"",backups:[]}; }
let sessions = {env:session(initialEnv),hosts:session(initialHosts)};
let page="env",selected="u-path",mode="parts",filter="",scope="all",task="path",message="已与演示目标同步",writing=false,dialogAction=null,returnFocus=null;
const kind = () => page === "hosts" ? "hosts" : "env";
const current = () => sessions[kind()];
const rowValue = (row) => row.parts ? row.parts.filter(p=>!p.deleted).map(p=>p.value).join(";") : row.value ?? "";
function canonical(rows,k) {
  return rows.filter(row=>!row.deleted).map(row=>k==="env"
    ? {id:row.id,name:row.name,scope:row.scope,type:row.type,value:rowValue(row)}
    : {id:row.id,name:row.name,ip:row.ip,comment:row.comment,enabled:row.enabled});
}
function signature(rows,k) { return JSON.stringify(canonical(rows,k).sort((a,b)=>a.id.localeCompare(b.id))); }
function diff(before,after,k) {
  const a=new Map(canonical(before,k).map(row=>[row.id,row]));
  const b=new Map(canonical(after,k).map(row=>[row.id,row]));
  return [...new Set([...a.keys(),...b.keys()])].flatMap(id=>{
    const old=a.get(id),value=b.get(id);
    return JSON.stringify(old)===JSON.stringify(value)?[]:[{id,old,value,operation:!old?"新增":!value?"删除":"修改"}];
  });
}
function changes(k=kind()) { return diff(sessions[k].base,sessions[k].work,k); }
function conflict(k=kind()) { const s=sessions[k]; return signature(s.base,k)!==signature(s.disk,k); }
function rowWritable(row) { return !current().readOnly && !writing && row && row.scope!=="system" && !row.deleted; }
function mutate(fn,notice="修改已暂存，尚未保存到系统",repaint=true) {
  const s=current(); if(s.readOnly||writing) return;
  const before=clone(s.work); fn(s.work);
  if(JSON.stringify(before)===JSON.stringify(s.work)) return;
  s.undo.push(before); if(s.undo.length>200)s.undo.shift(); s.redo=[];s.revision++;s.error="";message=notice;
  if(repaint)render();else renderChrome();
}
function history(redo=false) {
  if(writing||current().readOnly)return;
  const s=current(),from=redo?s.redo:s.undo,to=redo?s.undo:s.redo;
  if(!from.length)return;to.push(clone(s.work));s.work=from.pop();s.revision++;s.error="";message=redo?"已重做，系统值未改变":"已撤销，系统值未改变";render();
}
function fieldValue(row,k) { return k==="env"?row.value:`${row.enabled?"":"# "}${row.ip} ${row.name}${row.comment?" # "+row.comment:""}`; }
function diffHtml(items,k) {
  return items.map(item=>`<div class="diff-row"><span class="badge ${item.operation==="新增"?"clean":""}">${escapeHtml(item.operation)}</span><div><strong>${escapeHtml(item.value?.name??item.old.name)}</strong>${item.old?`<p>原：${escapeHtml(fieldValue(item.old,k))}</p>`:""}${item.value?`<p>新：${escapeHtml(fieldValue(item.value,k))}</p>`:""}</div></div>`).join("");
}
function renderChrome() {
  const s=current(),active=document.activeElement;
  const pending=active?.matches('.part-value,#raw-value')&&!active.readOnly&&active.value!==active.dataset.original;
  const dirtyIds=new Set(changes().map(change=>change.id));if(pending)dirtyIds.add(selected);
  const n=dirtyIds.size;
  $("#target-title").textContent=kind()==="env"?"环境变量":"hosts 文件";
  $("#dirty-badge").textContent=writing?"正在保存…":n?`${n} 项未保存`:"已同步";
  $("#dirty-badge").className=`badge ${n||writing?"":"clean"}`;
  $("#undo").disabled=!s.undo.length||writing||s.readOnly;
  $("#redo").disabled=!s.redo.length||writing||s.readOnly;
  $("#save").disabled=!n||writing||s.readOnly;
  $("#reload").disabled=writing;
  $("#status").textContent=message;
  $("#capability").textContent=s.readOnly?"当前目标只读 · 可以查看与搜索":kind()==="env"?"用户范围可写 · 系统范围只读":"演示文件可写 · 保留注释";
  document.querySelectorAll("[data-page]").forEach(button=>{button.disabled=writing;button.hidden=(kind()==="hosts")!==(button.dataset.page==="hosts");button.setAttribute("aria-current",button.dataset.page===page?"page":"false");});
  document.querySelectorAll("[data-task]").forEach(button=>{button.disabled=writing;button.classList.toggle("active",button.dataset.task===task);});
  ["#reset","#external","#fail-next","#readonly"].forEach(id=>$(id).disabled=writing);
  const banner=$("#banner");banner.className=`banner ${s.error?"error":""}`;
  if(s.error){banner.hidden=false;banner.innerHTML=`<span>${escapeHtml(s.error)}</span><button data-command="retry">再次审阅保存</button>`;}
  else if(conflict()&&n){banner.hidden=false;banner.innerHTML='<span>目标已被外部修改。你的编辑仍然保留。</span><button data-command="conflict">查看差异</button>';}
  else if(s.readOnly){banner.hidden=false;banner.textContent="当前目标只读；修改功能不可用，已有工作副本仍然保留。";}
  else banner.hidden=true;
  const env=canonical(sessions.env.disk,"env"),hosts=canonical(sessions.hosts.disk,"hosts");
  const checks={path:env.find(r=>r.id==="u-path")?.value.split(";").includes("C:\\SDK\\bin"),hosts:hosts.find(r=>r.id==="h-api")?.enabled===false,restore:!!env.find(r=>r.id==="u-old")};
  Object.entries(checks).forEach(([name,done])=>{const el=$("#task-"+name);el.textContent=done?"✓ 已保存到演示目标":"等待完成";el.classList.toggle("complete",done);});
}
function render() {
  renderChrome();
  if(page==="backup")renderBackup();else if(page==="hosts")renderHosts();else renderEnv();
}
function filtered(rows) { return rows.filter(row=>(scope==="all"||row.scope===scope)&&`${row.name} ${rowValue(row)}`.toLowerCase().includes(filter.toLowerCase())); }
function renderEnv() {
  const s=current(),rows=filtered(s.work),changed=new Map(changes().map(change=>[change.id,change]));
  if(!rows.some(row=>row.id===selected))selected=rows[0]?.id??"";
  $("#content").innerHTML=`<div class="toolbar"><input id="filter" type="search" aria-label="筛选名称或值" placeholder="筛选名称或值 · Ctrl+F" value="${escapeHtml(filter)}"><label>范围 <select id="scope"><option value="all">全部</option><option value="user">用户</option><option value="system">系统</option></select></label></div>
    <div class="list-scroll"><table aria-label="环境变量"><colgroup><col style="width:23%"><col style="width:12%"><col class="hide-small" style="width:22%"><col></colgroup><thead><tr><th>名称 / 状态</th><th>范围</th><th class="hide-small">类型</th><th>值</th></tr></thead><tbody>${rows.map(row=>`<tr data-row="${row.id}" tabindex="0" aria-selected="${row.id===selected}" class="${row.id===selected?"selected ":""}${row.deleted?"deleted":""}"><td><span class="state-symbol">${row.deleted?"−":changed.has(row.id)?"●":row.scope==="system"?"锁":""}</span><strong class="row-name">${escapeHtml(row.name)}</strong></td><td class="scope">${row.scope==="user"?"用户":"系统"}</td><td class="hide-small scope">${escapeHtml(row.type)}</td><td class="mono" data-value-for="${row.id}" title="${escapeHtml(rowValue(row))}">${escapeHtml(rowValue(row))}</td></tr>`).join("")}</tbody></table>${rows.length?"":'<div class="empty">没有匹配的变量<br><button data-command="clear-filter">清除筛选</button></div>'}</div><div id="detail"></div>`;
  $("#scope").value=scope;
  const row=s.work.find(r=>r.id===selected);if(row)renderDetail(row);
}
function renderDetail(row) {
  const can=rowWritable(row),hasParts=!!row.parts;
  $("#detail").innerHTML=`<section class="detail"><div class="detail-heading"><div class="detail-title"><h3>${escapeHtml(row.name)}</h3><small>${row.scope==="user"?"用户":"系统"} · ${escapeHtml(row.type)}</small></div><button data-command="delete-row" class="quiet danger" ${current().readOnly||row.scope==="system"?"disabled":""}>${row.deleted?"恢复变量":"删除整个变量"}</button></div>
    ${row.deleted?'<div class="readonly-note">此变量已标记删除，保存前可以恢复。</div>':!can?'<div class="readonly-note">系统范围只读。原型不提供提权或系统写入。</div>':""}
    ${hasParts?`<div class="segmented" aria-label="值编辑方式"><button data-mode="parts" class="${mode==="parts"?"active":""}">分隔项</button><button data-mode="raw" class="${mode==="raw"?"active":""}">原值</button></div>`:""}
    ${hasParts&&mode==="parts"&&!row.deleted?partsHtml(row,can):`<label class="block" for="raw-value">原始值</label><textarea id="raw-value" data-original="${escapeHtml(rowValue(row))}" ${can?"":"readonly"}>${escapeHtml(rowValue(row))}</textarea>`}
    <p class="hint">编辑会自动暂存；切换视图不需要提交。只有“保存到系统”确认后才写入。</p>
    ${hasParts?'<details class="advanced"><summary>高级：分隔规则与完整值</summary><p>此任务按分号拆分。正式版将提供引号保护、自定义分隔符和无损往返；当前原型只验证任务流程。</p></details>':""}</section>`;
}
function partsHtml(row,can) {
  return `<div class="part-toolbar"><span class="muted">分隔符：分号 ;</span><button data-command="add-part" ${can?"":"disabled"}>＋ 新增项</button><span id="part-count" class="count">${countText(row)}</span></div><div class="parts" role="group" aria-label="Path 分隔项">${row.parts.map((p,index)=>`<div class="part ${p.deleted?"deleted":""}" data-part="${p.id}"><span class="state-symbol ${p.isNew?"added":""}">${p.deleted?"−":p.isNew?"+":""}</span><input class="part-value" aria-label="分隔项 ${index+1}" data-part-input="${p.id}" data-original="${escapeHtml(p.value)}" value="${escapeHtml(p.value)}" ${can&&!p.deleted?"":"readonly"}><div class="part-buttons">${p.deleted?`<button data-part-action="restore" data-id="${p.id}" ${can?"":"disabled"}>恢复此项</button>`:`<button data-part-action="up" data-id="${p.id}" aria-label="上移第 ${index+1} 项" ${can&&index>0?"":"disabled"}>↑</button><button data-part-action="down" data-id="${p.id}" aria-label="下移第 ${index+1} 项" ${can&&index<row.parts.length-1?"":"disabled"}>↓</button><button class="delete-part" data-part-action="delete" data-id="${p.id}" aria-label="删除第 ${index+1} 项" ${can?"":"disabled"}>删除</button>`}</div></div>`).join("")}</div>`;
}
function countText(row) { return `有效 ${row.parts.filter(p=>!p.deleted).length} 项 · 待删除 ${row.parts.filter(p=>p.deleted).length} 项 · ${rowValue(row).length.toLocaleString()} / 32,767`; }
function renderHosts() {
  const s=current();
  const rows=s.work.filter(row=>`${row.ip} ${row.name} ${row.comment}`.toLowerCase().includes(filter.toLowerCase()));
  $("#content").innerHTML=`<div class="toolbar"><input id="filter" type="search" aria-label="筛选 IP、主机名或注释" placeholder="筛选 IP、主机名或注释 · Ctrl+F" value="${escapeHtml(filter)}"><span class="muted">C:\\Fixtures\\hosts</span></div><div class="list-scroll"><table aria-label="hosts 条目"><colgroup><col style="width:16%"><col style="width:23%"><col><col class="hide-small" style="width:24%"></colgroup><thead><tr><th>启用</th><th>IP 地址</th><th>主机名</th><th class="hide-small">注释</th></tr></thead><tbody>${rows.map(row=>`<tr><td><button class="switch" role="switch" aria-label="${escapeHtml(row.name)} 启用" aria-checked="${row.enabled}" data-toggle-host="${row.id}" ${s.readOnly?"disabled":""}>${row.enabled?"开":"关"}</button></td><td class="mono">${row.ip}</td><td class="mono">${escapeHtml(row.name)}</td><td class="hide-small muted">${escapeHtml(row.comment)}</td></tr>`).join("")}</tbody></table>${rows.length?"":'<div class="empty">没有匹配的映射<br><button data-command="clear-filter">清除筛选</button></div>'}</div><section class="detail"><div class="detail-heading"><h3>工作副本预览</h3><span class="badge">尚未写入文件</span></div><pre class="mono raw-preview">${escapeHtml(s.work.map(row=>fieldValue(row,"hosts")).join("\n"))}</pre><p class="hint">关闭开关只在工作副本加上注释。误操作可以直接撤销，最后统一审阅保存。</p></section>`;
}
function restorePlan() { return diff(current().work,backupEnv,"env").filter(item=>(item.value??item.old).scope==="user"); }
function renderBackup() {
  const plan=restorePlan();
  $("#content").innerHTML=`<div class="backup-layout"><div><div class="backup-card"><strong>2026-11-08 14:32</strong><p>用户范围 · 3 个变量</p><span class="badge clean">可恢复</span></div><p class="hint">演示快照：Path、JAVA_HOME、OLD_SDK。</p></div><div class="backup-preview"><h3>恢复之前，先看变化</h3><p class="muted">与当前工作副本比较；系统范围保持不变。</p>${plan.length?diffHtml(plan,"env"):'<div class="empty">备份与当前用户工作副本相同</div>'}<button class="primary" id="restore-backup" ${current().readOnly||!plan.length?"disabled":""}>恢复到工作副本</button><p class="hint">恢复可撤销，暂不写系统。之后点击“保存到系统”核对实际写入。</p></div></div>`;
}
function commitInput(input) {
  if(!input||input.readOnly||input.value===input.dataset.original)return;
  const row=current().work.find(r=>r.id===selected);if(!rowWritable(row))return;
  const value=input.value;
  if(input.dataset.partInput){
    const id=input.dataset.partInput;
    mutate(rows=>{rows.find(r=>r.id===selected).parts.find(p=>p.id===id).value=value;},"分隔项已自动暂存，尚未写入系统",false);
  }else if(input.id==="raw-value"){
    mutate(rows=>{const target=rows.find(r=>r.id===selected);if(target.parts)target.parts=parts(value.split(";"));else target.value=value;},"原值已自动暂存，尚未写入系统",false);
  }else return;
  input.dataset.original=value;
  const cell=document.querySelector(`[data-value-for="${selected}"]`);if(cell){cell.textContent=rowValue(current().work.find(r=>r.id===selected));cell.title=cell.textContent;}
  const rowEl=document.querySelector(`[data-row="${selected}"] .state-symbol`);if(rowEl)rowEl.textContent=changes().some(c=>c.id===selected)?"●":"";
  if($("#part-count"))$("#part-count").textContent=countText(current().work.find(r=>r.id===selected));
  renderChrome();
}
function commitActive() { commitInput(document.activeElement); }
function focusPart(id) { document.querySelector(`[data-part-input="${id}"]`)?.focus({preventScroll:true}); }
function partAction(action,id) {
  const row=current().work.find(r=>r.id===selected);if(!rowWritable(row))return;
  mutate(rows=>{const ps=rows.find(r=>r.id===selected).parts,index=ps.findIndex(p=>p.id===id);if(index<0)return;
    if(action==="delete"){if(ps[index].isNew)ps.splice(index,1);else ps[index].deleted=true;}
    if(action==="restore")ps[index].deleted=false;
    const dest=index+(action==="up"?-1:action==="down"?1:0);
    if(dest!==index&&dest>=0&&dest<ps.length)[ps[index],ps[dest]]=[ps[dest],ps[index]];
  });focusPart(id);
}
function showModal(title,body,buttons,action) {
  const modal=$("#modal");if(!modal.open)returnFocus=document.activeElement;
  $("#modal-body").innerHTML=`<h2 id="modal-title">${escapeHtml(title)}</h2>${body}`;
  $("#modal-actions").innerHTML=buttons.map(button=>`<button data-dialog="${button.id}" class="${button.style??""}">${button.label}</button>`).join("");
  dialogAction=action;if(!modal.open)modal.showModal();$("#modal-actions button")?.focus();
}
function closeModal() { if(writing)return;$("#modal").close();dialogAction=null;if(returnFocus?.isConnected)returnFocus.focus();else $("#save").focus(); }
function requestSave(force=false) {
  commitActive();const k=kind(),s=current(),plan=changes(k);if(!plan.length||s.readOnly||writing)return;
  if(conflict(k)&&!force){showConflict();return;}
  const revision=s.revision,diskSignature=signature(s.disk,k),deletions=plan.filter(c=>c.operation==="删除");
  showModal(`保存 ${plan.length} 项${k==="env"?"环境变量":"hosts"}变更`,`<p class="muted">${force?"已经审阅外部差异。":""}确认后先备份，再写入并读回。本原型仅更新演示目标。</p>${diffHtml(plan,k)}${deletions.length?`<div class="warning">将删除：${escapeHtml(deletions.map(c=>c.old.name).join("、"))}</div>`:""}`,[{id:"cancel",label:"返回编辑"},{id:"save",label:`确认保存 ${plan.length} 项`,style:"primary"}],action=>{
    if(action==="cancel"){closeModal();return;}
    if(revision!==s.revision||diskSignature!==signature(s.disk,k)){closeModal();showConflict();return;}
    writing=true;renderChrome();$("#modal-body").innerHTML='<h2 id="modal-title">正在保存…</h2><p>正在备份、写入和核对演示数据。</p><progress class="progress" aria-label="保存进度"></progress>';$("#modal-actions").innerHTML="";
    setTimeout(()=>{
      if(s.failNext){s.failNext=false;s.error="保存失败：演示写入被拒绝。工作副本与恢复信息已保留。";message="未保存成功，修改仍然保留";}
      else{
        s.backups.push(clone(s.disk));
        for(const change of plan){const index=s.disk.findIndex(r=>r.id===change.id);if(change.operation==="删除"){if(index>=0)s.disk.splice(index,1);}else{const value=clone(s.work.find(r=>r.id===change.id));value.deleted=false;if(value.parts)value.parts=value.parts.filter(p=>!p.deleted).map(p=>({...p,isNew:false}));if(index<0)s.disk.push(value);else s.disk[index]=value;}}
        s.work=clone(s.disk);s.base=clone(s.disk);s.undo=[];s.redo=[];s.error="";message="已保存到演示目标，并完成读回核对";
      }
      writing=false;closeModal();render();
    },450);
  });
}
function showConflict() {
  const k=kind(),s=current(),external=diff(s.base,s.disk,k);
  if(!external.length){requestSave();return;}
  const mine=new Map(canonical(s.work,k).map(row=>[row.id,row]));
  const body=`<p class="muted">编辑仍然保留。对比基线、你的修改和当前演示目标。</p>${external.map(item=>`<h3>${escapeHtml((item.value??item.old).name)}</h3>${[["基线",item.old],["我的修改",mine.get(item.id)],["当前目标",item.value]].map(([label,row])=>`<div class="conflict-value"><span>${label}</span><code>${escapeHtml(row?fieldValue(row,k):"（不存在）")}</code></div>`).join("")}`).join("")}<p class="muted">覆盖仅应用你修改的项目；保存前仍需核对和备份。</p>`;
  showModal("目标已被外部修改",body,[{id:"cancel",label:"暂不处理"},{id:"reload",label:"重新载入…"},{id:"overwrite",label:"用我的修改覆盖…",style:"danger"}],action=>{closeModal();if(action==="reload")requestReload();if(action==="overwrite")requestSave(true);});
}
function requestReload() {
  if(writing)return;
  const run=()=>{const s=current();s.work=clone(s.disk);s.base=clone(s.disk);s.undo=[];s.redo=[];s.error="";s.revision++;message="已重新载入演示目标";closeModal();render();};
  if(!changes().length){run();return;}
  showModal("放弃未保存修改？",'<p class="muted">重新载入将丢弃当前目标的工作副本修改。</p>',[{id:"cancel",label:"继续编辑"},{id:"reload",label:"放弃并重新载入",style:"danger"}],action=>{if(action==="reload")run();else closeModal();});
}
function navigate(nextPage) { commitActive();page=nextPage;filter="";scope="all";message=changes().length?"修改已暂存，切换页面不会写入系统":"已与演示目标同步";render(); }
$("#content").addEventListener("input",event=>{
  if(event.target.id!=="filter"){if(event.target.matches('.part-value,#raw-value'))renderChrome();return;}
  const input=event.target,start=input.selectionStart;filter=input.value;
  if(page==="env")renderEnv();else renderHosts();$("#filter").focus();$("#filter").setSelectionRange?.(start,start);
});
$("#content").addEventListener("change",event=>{if(event.target.id==="scope"){scope=event.target.value;renderEnv();}else commitInput(event.target);});
$("#content").addEventListener("click",event=>{
  const button=event.target.closest("button");
  if(button?.disabled)return;
  if(button?.dataset.mode){commitActive();mode=button.dataset.mode;render();return;}
  if(button?.dataset.partAction){partAction(button.dataset.partAction,button.dataset.id);return;}
  if(button?.dataset.toggleHost){mutate(rows=>{const row=rows.find(r=>r.id===button.dataset.toggleHost);row.enabled=!row.enabled;});return;}
  if(button?.id==="restore-backup"){
    mutate(rows=>{const system=rows.filter(r=>r.scope==="system");rows.splice(0,rows.length,...clone(backupEnv.filter(r=>r.scope==="user")),...system);},"备份已恢复到工作副本，可以撤销；尚未写入系统");return;
  }
  if(button?.dataset.command==="add-part"){
    const id=`p${nextId++}`;mutate(rows=>rows.find(r=>r.id===selected).parts.push({id,value:"",isNew:true,deleted:false}));focusPart(id);return;
  }
  if(button?.dataset.command==="delete-row"){
    const row=current().work.find(r=>r.id===selected);if(row.scope==="system")return;
    mutate(rows=>{const target=rows.find(r=>r.id===selected);target.deleted=!target.deleted;},row.deleted?"变量已恢复":"已标记删除整个变量，保存前仍可恢复");return;
  }
  if(button?.dataset.command==="clear-filter"){filter="";render();$("#filter").focus();return;}
  const row=event.target.closest("[data-row]");if(row){commitActive();selected=row.dataset.row;render();}
});
document.querySelectorAll("[data-page]").forEach(button=>button.addEventListener("click",()=>navigate(button.dataset.page)));
document.querySelectorAll("[data-task]").forEach(button=>button.addEventListener("click",()=>{task=button.dataset.task;selected="u-path";mode="parts";navigate(task==="path"?"env":task==="hosts"?"hosts":"backup");}));
$("#save").addEventListener("click",()=>requestSave());$("#undo").addEventListener("click",()=>history());$("#redo").addEventListener("click",()=>history(true));$("#reload").addEventListener("click",requestReload);
$("#modal-actions").addEventListener("click",event=>{const button=event.target.closest("[data-dialog]");if(button)dialogAction?.(button.dataset.dialog);});
$("#modal").addEventListener("cancel",event=>{event.preventDefault();closeModal();});
$("#banner").addEventListener("click",event=>{if(event.target.dataset.command==="conflict")showConflict();if(event.target.dataset.command==="retry")requestSave();});
$("#theme").addEventListener("click",()=>{document.body.classList.toggle("dark");$("#theme").textContent=document.body.classList.contains("dark")?"切换浅色":"切换暗色";});
$("#reset").addEventListener("click",()=>{sessions={env:session(initialEnv),hosts:session(initialHosts)};page="env";selected="u-path";mode="parts";filter="";scope="all";task="path";message="演示已重置";render();});
$("#external").addEventListener("click",()=>{commitActive();const s=current();if(kind()==="env")s.disk.find(r=>r.id==="u-path").parts.push(...parts([`C:\\External${nextId}\\bin`]));else s.disk.find(r=>r.id==="h-api").comment="changed outside";if(!changes().length){s.base=clone(s.disk);s.work=clone(s.disk);message="无未保存修改，已自动载入外部变化";}else message="检测到外部改动，你的修改仍然保留";render();});
$("#fail-next").addEventListener("click",()=>{current().failNext=!current().failNext;message=current().failNext?"演示情境已启用：下一次保存失败":"保存失败情境已取消";renderChrome();});
$("#readonly").addEventListener("click",()=>{commitActive();current().readOnly=!current().readOnly;message=current().readOnly?"目标切为只读，工作副本保留":"目标恢复可写";render();});
document.addEventListener("keydown",event=>{
  if(event.isComposing||writing)return;
  if($("#modal").open)return;
  const input=event.target.closest("input,textarea,select"),key=event.key.toLowerCase();
  if(event.ctrlKey&&key==="s"){event.preventDefault();commitActive();requestSave();return;}
  if(event.ctrlKey&&key==="f"){event.preventDefault();$("#filter")?.focus();return;}
  if(event.key==="Escape"&&input){if(input.dataset.original!==undefined)input.value=input.dataset.original;input.blur();renderChrome();return;}
  if(event.key==="Enter"&&input&&input.tagName!=="TEXTAREA"){event.preventDefault();commitInput(input);input.blur();return;}
  if(event.altKey&&["ArrowUp","ArrowDown"].includes(event.key)&&input?.dataset.partInput){event.preventDefault();commitInput(input);partAction(event.key==="ArrowUp"?"up":"down",input.dataset.partInput);return;}
  if(input)return;
  if(event.ctrlKey&&key==="z"){event.preventDefault();history(event.shiftKey);return;}
  if(event.ctrlKey&&key==="y"){event.preventDefault();history(true);return;}
  if(event.key==="Enter"&&event.target.dataset.row){event.preventDefault();selected=event.target.dataset.row;render();document.querySelector(".part-value,#raw-value")?.focus();}
});
render();
