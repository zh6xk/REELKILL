'use strict';
/* REELKILL UI. Satu-satunya jalur ke C++ adalah Bridge (lihat docs/AGENT_GUIDE.md). */
const $=s=>document.querySelector(s);
const el=(t,c,x)=>{const e=document.createElement(t);if(c)e.className=c;if(x!=null)e.textContent=x;return e};
const p2=n=>String(n).padStart(2,'0');
const fmt=s=>{s=Math.max(0,Math.floor(s));const h=s/3600|0,m=s%3600/60|0;return (h?h+':':'')+p2(m)+':'+p2(s%60)};

/* ---------- Bridge: WebView2 asli, atau mock saat dibuka di browser ---------- */
const Bridge=(()=>{
  const real=window.chrome&&window.chrome.webview,subs=[];let timer;
  const emit=(type,payload)=>subs.forEach(f=>f({v:1,type,payload}));
  function mock(t,p){
    if(t==='startScan'){let n=0;const total=147000;clearInterval(timer);timer=setInterval(()=>{n+=2300;
      emit('progress',{percent:Math.min(100,n/total*100),frame:Math.min(n,total),total,fps:41,etaSec:Math.max(0,(total-n)/41)});
      emit('log',{line:'memindai frame '+n});
      if(n%9200<2300)emit('clipFound',{start:n/30,end:n/30+7,score:.9});
      if(n>=total){clearInterval(timer);emit('scanDone',{});}},250);}
    else if(t==='stopScan'){clearInterval(timer);emit('scanDone',{stopped:true});}
    else if(t==='pickFile'||t==='pickFolder')emit('fileSelected',{target:p.target,path:t==='pickFile'?'D:\\Rekaman\\Match_Final_2.mp4':'E:\\Videos\\OCR'});
    else if(t==='loadResults')emit('resultsLoaded',{videoUrl:'',duration:8845,clips:[[34,41],[52,61],[150,173],[213,230],[406,416],[666,678],[690,700]].map(([start,end])=>({start,end,label:'kill',score:.9}))});
    else if(t==='exportClips')emit('toast',{message:'Mock: '+p.clips.length+' klip diekspor'});
  }
  return{send(type,payload={}){const m={v:1,type,payload};real?real.postMessage(m):mock(type,payload)},
    on(f){real?real.addEventListener('message',e=>f(e.data)):subs.push(f)}};
})();

/* ---------- State ---------- */
const S={scanning:false,recent:[],clips:[],picked:new Set(),duration:0,roi:{x:40,y:60,w:380,h:90},frame:{w:1920,h:1080}};
const setStatus=t=>{$('#status').textContent=t};

/* ---------- Navigasi ---------- */
$('#nav').addEventListener('click',e=>{const b=e.target.closest('.nv');if(!b)return;
  document.querySelectorAll('.nv').forEach(n=>n.classList.toggle('on',n===b));
  document.querySelectorAll('.view').forEach(v=>v.classList.toggle('on',v.id==='view-'+b.dataset.view));});

/* ---------- Dashboard ---------- */
$('#btnVideo').onclick=()=>Bridge.send('pickFile',{target:'video'});
$('#btnOut').onclick=()=>Bridge.send('pickFolder',{target:'output'});
$('#btnStart').onclick=()=>{
  if(!$('#inVideo').value||!$('#inPlayer').value.trim()){setStatus('Isi file video dan nama player dulu');return}
  S.scanning=true;S.recent=[];$('#recent').replaceChildren();$('#log').replaceChildren();
  $('#btnStart').disabled=true;$('#btnStop').disabled=false;setStatus('Menganalisis...');
  Bridge.send('startScan',{video:$('#inVideo').value,output:$('#inOutput').value,player:$('#inPlayer').value.trim(),
    pre:+$('#inPre').value||0,post:+$('#inPost').value||0,roi:{...S.roi},roiPreset:$('#roiSelect').value});};
$('#btnStop').onclick=()=>Bridge.send('stopScan');
function addLog(t){const l=$('#log');l.append(el('div',null,t));if(l.children.length>200)l.firstChild.remove();l.scrollTop=l.scrollHeight}
function addRecent(c){S.recent.unshift(c);S.recent.length=Math.min(S.recent.length,4);
  $('#recent').replaceChildren(...S.recent.map(r=>{const d=el('div','row');d.append(el('div','th'));
    const t=el('div');t.append(el('div',null,fmt(r.start)+' – '+fmt(r.end)),el('div','mt','kill · '+(r.score??0).toFixed(2)));d.append(t);return d}))}

/* ---------- Hasil Scan ---------- */
$('#btnLoad').onclick=()=>Bridge.send('loadResults');
$('#btnAll').onclick=()=>{S.picked=new Set(S.clips.keys());drawClips()};
$('#btnNone').onclick=()=>{S.picked.clear();drawClips()};
$('#btnExport').onclick=()=>{const clips=[...S.picked].sort((a,b)=>a-b).map(i=>({start:S.clips[i].start,end:S.clips[i].end}));
  if(!clips.length){setStatus('Pilih minimal satu klip');return}Bridge.send('exportClips',{clips})};
$('#btnExplorer').onclick=()=>Bridge.send('showInExplorer');
function drawClips(){
  $('#clipList').replaceChildren(...S.clips.map((c,i)=>{const b=el('button','item');b.setAttribute('role','checkbox');
    b.setAttribute('aria-checked',S.picked.has(i));const t=el('div');t.append(el('div',null,fmt(c.start)+' – '+fmt(c.end)),el('span','tag',c.label||'kill'));
    b.append(el('span','ck'),el('div','th'),t);
    b.onclick=()=>{S.picked.has(i)?S.picked.delete(i):S.picked.add(i);drawClips()};
    b.ondblclick=()=>{$('#player').currentTime=c.start};return b}));
  const tot=[...S.picked].reduce((a,i)=>a+S.clips[i].end-S.clips[i].start,0);
  $('#resSum').textContent=S.clips.length+' klip · '+S.picked.size+' dipilih · total '+fmt(tot);}
function drawTimeline(){
  const tl=$('#tl');tl.querySelectorAll('.mk').forEach(m=>m.remove());const d=S.duration||1;
  S.clips.forEach(c=>{const m=el('div','mk');m.style.left=c.start/d*100+'%';m.style.width=Math.max(.15,(c.end-c.start)/d*100)+'%';tl.append(m)});
  $('#ticks').replaceChildren(...[0,1,2,3,4,5].map(i=>el('span',null,fmt(d*i/5))));}
$('#tl').onclick=e=>{const r=e.currentTarget.getBoundingClientRect();$('#player').currentTime=(e.clientX-r.left)/r.width*S.duration};
$('#player').ontimeupdate=e=>{const t=e.target.currentTime;$('#ph').style.left=t/(S.duration||1)*100+'%';$('#clock').textContent=fmt(t)+' / '+fmt(S.duration)};

/* ---------- ROI ---------- */
const dlg=$('#roiDlg');let roiBackup;
function drawRoi(){const r=S.roi,f=S.frame,b=$('#roiBox');
  Object.assign(b.style,{left:r.x/f.w*100+'%',top:r.y/f.h*100+'%',width:r.w/f.w*100+'%',height:r.h/f.h*100+'%'});
  ['x','y','w','h'].forEach(k=>$('#roi'+k.toUpperCase()).value=Math.round(r[k]))}
$('#btnRoi').onclick=()=>{roiBackup={...S.roi};drawRoi();dlg.showModal()};
$('#btnCancel').onclick=()=>{S.roi=roiBackup;dlg.close()};
$('#btnApply').onclick=()=>dlg.close();
$('#btnRoiDel').onclick=()=>Bridge.send('deleteRoiPreset',{name:$('#roiSelect').value});
$('#btnPreset').onclick=()=>{const n=$('#presetName').value.trim();if(!n)return;Bridge.send('saveRoiPreset',{name:n,roi:{...S.roi}});
  $('#roiSelect').append(new Option(n));$('#roiSelect').value=n};
['x','y','w','h'].forEach(k=>$('#roi'+k.toUpperCase()).onchange=e=>{S.roi[k]=+e.target.value||0;drawRoi()});
$('#roiBox').addEventListener('pointerdown',e=>{
  const box=e.currentTarget,hd=e.target.dataset.h||'move',st=$('#roiStage').getBoundingClientRect(),s0={...S.roi},x0=e.clientX,y0=e.clientY;
  box.setPointerCapture(e.pointerId);
  const mv=ev=>{const dx=(ev.clientX-x0)*S.frame.w/st.width,dy=(ev.clientY-y0)*S.frame.h/st.height;let{x,y,w,h}=s0;
    if(hd==='move'){x+=dx;y+=dy}else{if(hd.includes('e'))w+=dx;if(hd.includes('s'))h+=dy;if(hd.includes('w')){x+=dx;w-=dx}if(hd.includes('n')){y+=dy;h-=dy}}
    w=Math.max(20,w);h=Math.max(20,h);x=Math.min(Math.max(0,x),S.frame.w-w);y=Math.min(Math.max(0,y),S.frame.h-h);S.roi={x,y,w,h};drawRoi()};
  const up=()=>{box.removeEventListener('pointermove',mv);box.removeEventListener('pointerup',up)};
  box.addEventListener('pointermove',mv);box.addEventListener('pointerup',up)});

/* ---------- Pesan dari C++ (field tak dikenal diabaikan) ---------- */
Bridge.on(m=>{const p=m.payload||{};switch(m.type){
  case'progress':$('#pct').textContent=Math.round(p.percent)+'%';$('#bar').style.width=p.percent+'%';
    $('#meta').textContent='Frame '+p.frame+' / '+p.total+' · '+p.fps+' fps · sisa '+fmt(p.etaSec);break;
  case'log':addLog(p.line);break;
  case'clipFound':addRecent(p);addLog('['+fmt(p.start)+'] kill terdeteksi');break;
  case'scanDone':S.scanning=false;$('#btnStart').disabled=false;$('#btnStop').disabled=true;setStatus(p.stopped?'Dihentikan':'Selesai');break;
  case'fileSelected':{const el=$(p.target==='video'?'#inVideo':'#inOutput');if(el)el.value=p.path;break;}
  case'frameInfo':S.frame={w:p.width,h:p.height};break;
  case'resultsLoaded':S.clips=p.clips;S.picked=new Set(S.clips.keys());S.duration=p.duration;
    if(p.videoUrl){
      const pl=$('#player');
      pl.onerror=()=>{setStatus('Format rekaman video tidak didukung peramban. Periksa codec (HEVC/H.265).');};
      pl.src=p.videoUrl;
    }
    drawClips();drawTimeline();break;
  case'toast':setStatus(p.message);break;
  case'error':S.scanning=false;$('#btnStart').disabled=false;$('#btnStop').disabled=true;setStatus('Error: '+p.message);break;}});
