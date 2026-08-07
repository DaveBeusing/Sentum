#pragma once

namespace sentum::dashboard {

inline constexpr const char* kDashboardHtml = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Sentum Research Dashboard</title>
<style>
:root{color-scheme:dark;--bg:#090d14;--panel:#111824;--panel2:#151f2e;--line:#263246;--text:#edf3ff;--muted:#8ea0ba;--good:#46d890;--warn:#f4c95d;--bad:#ff6b72;--accent:#72a7ff;--accent2:#9f7aea}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:14px/1.45 Inter,ui-sans-serif,system-ui,-apple-system,Segoe UI,sans-serif}header{height:64px;display:flex;align-items:center;gap:12px;padding:0 24px;border-bottom:1px solid var(--line);position:sticky;top:0;background:#090d14ee;backdrop-filter:blur(10px);z-index:3}.brand{font-weight:800;font-size:19px;letter-spacing:.08em}.pill{padding:5px 9px;border-radius:999px;background:var(--panel2);color:var(--muted);font-size:12px}.spacer{flex:1}main{padding:22px;max-width:1700px;margin:auto}.tabs{display:flex;gap:8px;margin-bottom:16px}.tab{border:1px solid var(--line);background:var(--panel);color:var(--muted);border-radius:8px;padding:8px 12px;cursor:pointer}.tab.active{color:var(--text);border-color:var(--accent);background:#17243a}.view{display:none}.view.active{display:block}.grid{display:grid;gap:14px}.kpis{grid-template-columns:repeat(6,minmax(120px,1fr));margin-bottom:14px}.two{grid-template-columns:1.25fr 1fr}.three{grid-template-columns:repeat(3,1fr)}.card{background:linear-gradient(180deg,var(--panel2),var(--panel));border:1px solid var(--line);border-radius:12px;padding:16px;min-width:0}.label{color:var(--muted);font-size:11px;text-transform:uppercase;letter-spacing:.07em}.value{font-size:23px;font-weight:700;margin-top:4px}.section{font-size:15px;font-weight:700;margin-bottom:12px}.sub{color:var(--muted);font-size:12px;margin-top:-7px;margin-bottom:12px}.positive{color:var(--good)}.negative{color:var(--bad)}.warn{color:var(--warn)}canvas{width:100%;height:240px;background:#0c121c;border-radius:8px}table{width:100%;border-collapse:collapse;font-size:12px}th,td{padding:8px;border-bottom:1px solid var(--line);text-align:left;white-space:nowrap}th{color:var(--muted);font-weight:600}.scroll{overflow:auto;max-height:370px}.toolbar{display:flex;gap:10px;align-items:center;flex-wrap:wrap;margin-bottom:14px}select{background:var(--panel2);border:1px solid var(--line);color:var(--text);border-radius:8px;padding:8px;max-width:430px}.run-list tr{cursor:pointer}.run-list tr:hover{background:#182235}.selected{background:#17243a}.interval{margin:9px 0}.interval-line{height:8px;background:#0b111b;border-radius:999px;position:relative;margin-top:5px}.interval-fill{height:100%;border-radius:999px;background:linear-gradient(90deg,var(--accent2),var(--accent))}.metric-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}.metric{border:1px solid var(--line);border-radius:8px;padding:10px}.regime{display:grid;grid-template-columns:130px 1fr 70px;align-items:center;gap:8px;margin:8px 0}.bar{height:8px;background:#0b111b;border-radius:999px;overflow:hidden}.bar>span{display:block;height:100%;background:var(--accent)}.heat-wrap{overflow:auto}.heat{border-collapse:separate;border-spacing:3px}.heat td{min-width:72px;text-align:center;border:0;border-radius:6px;padding:10px}.heat th{border:0}.provenance{font-family:ui-monospace,SFMono-Regular,Consolas,monospace;font-size:11px;word-break:break-all;color:var(--muted)}.footer{color:var(--muted);padding:16px 0;font-size:12px}.empty{color:var(--muted);padding:24px;text-align:center}.health{display:grid;grid-template-columns:repeat(2,1fr);gap:8px}.health div{padding:9px;border:1px solid var(--line);border-radius:8px;color:var(--muted)}.dot{width:8px;height:8px;border-radius:50%;display:inline-block;margin-right:6px;background:var(--muted)}.good .dot{background:var(--good)}.bad .dot{background:var(--bad)}
@media(max-width:1100px){.kpis{grid-template-columns:repeat(3,1fr)}.two,.three{grid-template-columns:1fr}}@media(max-width:650px){main{padding:12px}.kpis{grid-template-columns:repeat(2,1fr)}header{padding:0 12px}.metric-grid{grid-template-columns:1fr}}
</style></head>
<body>
<header><div class="brand">SENTUM</div><div id="mode" class="pill">IDLE</div><div class="pill">READ ONLY</div><div class="spacer"></div><div id="updated" class="pill">connecting…</div></header>
<main>
<div class="tabs"><button class="tab active" data-view="researchView">Research Lab</button><button class="tab" data-view="runtimeView">Runtime</button></div>

<section id="researchView" class="view active">
<div class="toolbar"><span class="label">Primary run</span><select id="runA"></select><span class="label">Compare with</span><select id="runB"><option value="">None</option></select></div>
<div class="grid kpis">
<div class="card"><div class="label">Validation Score</div><div id="validationScore" class="value">—</div></div>
<div class="card"><div class="label">Holdout Score</div><div id="holdoutScore" class="value">—</div></div>
<div class="card"><div class="label">Overfit Gap</div><div id="overfitGap" class="value">—</div></div>
<div class="card"><div class="label">Deflated Sharpe</div><div id="deflatedSharpe" class="value">—</div></div>
<div class="card"><div class="label">Stability</div><div id="stability" class="value">—</div></div>
<div class="card"><div class="label">MC Loss Probability</div><div id="lossProbability" class="value">—</div></div>
</div>

<div class="grid two">
<div class="card"><div class="section">Holdout Equity & Drawdown</div><div class="sub">Deterministic rerun of the selected holdout parameters</div><canvas id="researchCurve" width="900" height="240"></canvas></div>
<div class="card"><div class="section">Validation vs Final Holdout</div><div id="holdoutCompare" class="metric-grid"></div><div class="section" style="margin-top:18px">Robustness Intervals</div><div id="intervals"></div></div>
</div>

<div class="grid two" style="margin-top:14px">
<div class="card"><div class="section">Parameter Landscape</div><div class="sub">Best validation score for each lookback × entry-threshold cell</div><div id="heatmap" class="heat-wrap"></div></div>
<div class="card"><div class="section">Holdout Regime Performance</div><div id="regimes"></div></div>
</div>

<div class="card" style="margin-top:14px"><div class="section">Experiment Comparison</div><div id="comparison" class="scroll"></div></div>
<div class="grid two" style="margin-top:14px">
<div class="card"><div class="section">Experiment History</div><div class="scroll"><table class="run-list"><thead><tr><th>Name</th><th>Kind</th><th>Status</th><th>Started</th><th>Commit</th></tr></thead><tbody id="runRows"></tbody></table></div></div>
<div class="card"><div class="section">Run Provenance</div><div id="provenance" class="provenance">No experiment selected</div></div>
</div>
</section>

<section id="runtimeView" class="view">
<div class="grid kpis"><div class="card"><div class="label">Balance</div><div id="balance" class="value">—</div></div><div class="card"><div class="label">Net P&amp;L</div><div id="pnl" class="value">—</div></div><div class="card"><div class="label">Trades</div><div id="trades" class="value">0</div></div><div class="card"><div class="label">Win Rate</div><div id="winrate" class="value">—</div></div><div class="card"><div class="label">Symbol</div><div id="symbol" class="value">—</div></div><div class="card"><div class="label">Queue Drop</div><div id="drop" class="value">—</div></div></div>
<div class="grid two"><div class="card"><div class="section">Realized Trading Equity</div><canvas id="equity" width="900" height="240"></canvas></div><div class="card"><div class="section">System Health</div><div id="healthGrid" class="health"></div></div></div>
<div class="grid two" style="margin-top:14px"><div class="card"><div class="section">Recent Trades</div><div class="scroll"><table><thead><tr><th>Symbol</th><th>Strategy</th><th>Entry</th><th>Exit</th><th>Net P&amp;L</th><th>Reason</th></tr></thead><tbody id="tradeRows"></tbody></table></div></div><div class="card"><div class="section">Order Events</div><div class="scroll"><table><thead><tr><th>Symbol</th><th>Side</th><th>State</th><th>Executed</th><th>Fill</th><th>Source</th></tr></thead><tbody id="orderRows"></tbody></table></div></div></div>
</section>
<div class="footer">Advanced research views are read-only. No order, credential, configuration-write or kill-switch endpoints are exposed to the browser.</div>
</main>
<script>
const $=id=>document.getElementById(id),fmt=(n,d=2)=>Number.isFinite(+n)?(+n).toFixed(d):'—',pct=n=>Number.isFinite(+n)?((+n)*100).toFixed(2)+'%':'—';
const esc=s=>String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
async function get(p){try{const r=await fetch(p,{cache:'no-store'});return r.ok?await r.json():null}catch{return null}}
let runs=[],detailA=null,detailB=null,trialsA=[];

document.querySelectorAll('.tab').forEach(b=>b.onclick=()=>{document.querySelectorAll('.tab').forEach(x=>x.classList.remove('active'));document.querySelectorAll('.view').forEach(x=>x.classList.remove('active'));b.classList.add('active');$(b.dataset.view).classList.add('active')});

function lineChart(id,series){const c=$(id),x=c.getContext('2d');x.clearRect(0,0,c.width,c.height);if(!series.length||series.every(s=>!s.points?.length)){x.fillStyle='#8ea0ba';x.fillText('No curve artifact available for this run',20,30);return}const vals=series.flatMap(s=>s.points.map(p=>+p.y)).filter(Number.isFinite);const mn=Math.min(...vals),mx=Math.max(...vals),rg=Math.max(1e-9,mx-mn);const colors=['#72a7ff','#ff6b72','#9f7aea'];series.forEach((s,si)=>{if(!s.points?.length)return;x.strokeStyle=colors[si%colors.length];x.lineWidth=s.width||2;x.beginPath();s.points.forEach((p,i)=>{const px=i/Math.max(1,s.points.length-1)*(c.width-30)+15,py=c.height-15-(+p.y-mn)/rg*(c.height-30);i?x.lineTo(px,py):x.moveTo(px,py)});x.stroke()})}

function metricCard(name,v,cls=''){return `<div class="metric"><div class="label">${esc(name)}</div><div class="${cls}"><b>${esc(v)}</b></div></div>`}
function latestTrial(d){return d?.research?.leaderboard?.[0]||null}
function metric(d,key){return d?.research?.final_holdout?.[key]}

function renderResearch(){const d=detailA,t=latestTrial(d),r=d?.research||{},v=d?.visualization||{};if(!d){return}
$('validationScore').textContent=fmt(t?.validation_score,4);$('holdoutScore').textContent=fmt(r.final_holdout_score,4);$('overfitGap').textContent=fmt(t?.overfit_gap,4);$('overfitGap').className='value '+(Math.abs(+t?.overfit_gap||0)<.5?'positive':'negative');$('deflatedSharpe').textContent=fmt(t?.deflated_sharpe,3);$('stability').textContent=fmt(t?.parameter_stability_score,3);$('lossProbability').textContent=pct(r?.monte_carlo?.probability_of_loss);
const eq=(v.equity_curve||[]).map(p=>({y:+p.equity})),dd=(v.drawdown_curve||[]).map(p=>({y:-(+p.drawdown)}));lineChart('researchCurve',[{points:eq},{points:dd,width:1}]);
const keys=['net_profit','max_drawdown','sharpe','sortino','profit_factor','win_rate'];$('holdoutCompare').innerHTML=keys.map(k=>metricCard(k.replaceAll('_',' '),`${fmt(t?.validation?.[k],3)} → ${fmt(r?.final_holdout?.[k],3)}`,k==='net_profit'&&(+r?.final_holdout?.[k]||0)>=0?'positive':'' )).join('');
renderIntervals(r);renderHeatmap(trialsA);renderRegimes(r.holdout_regimes||[]);renderProvenance(d);renderComparison();}

function renderIntervals(r){const items=[['Bootstrap net profit',r?.bootstrap_net_profit],['Monte Carlo net profit',r?.monte_carlo?.net_profit],['Monte Carlo drawdown',r?.monte_carlo?.max_drawdown]];$('intervals').innerHTML=items.map(([name,v])=>{if(!v)return'';const max=Math.max(Math.abs(+v.lower||0),Math.abs(+v.upper||0),1),w=Math.min(100,Math.abs((+v.upper||0)-(+v.lower||0))/max*60+20);return `<div class="interval"><div class="label">${esc(name)} · ${fmt(v.lower,2)} / ${fmt(v.median,2)} / ${fmt(v.upper,2)}</div><div class="interval-line"><div class="interval-fill" style="width:${w}%"></div></div></div>`}).join('')||'<div class="empty">No robustness intervals</div>'}

function renderHeatmap(rows){if(!rows?.length){$('heatmap').innerHTML='<div class="empty">No trial artifact available</div>';return}const xs=[...new Set(rows.map(r=>+r.lookback))].sort((a,b)=>a-b),ys=[...new Set(rows.map(r=>+r.entry_threshold))].sort((a,b)=>a-b);const map=new Map();rows.forEach(r=>{const k=`${+r.lookback}|${+r.entry_threshold}`,v=+r.validation_score;if(!map.has(k)||v>map.get(k))map.set(k,v)});const vals=[...map.values()].filter(Number.isFinite),mn=Math.min(...vals),mx=Math.max(...vals),rg=Math.max(1e-9,mx-mn);let h='<table class="heat"><tr><th>threshold \\ lookback</th>'+xs.map(x=>`<th>${x}</th>`).join('')+'</tr>';ys.forEach(y=>{h+=`<tr><th>${y}</th>`;xs.forEach(x=>{const v=map.get(`${x}|${y}`);if(!Number.isFinite(v)){h+='<td>—</td>';return}const a=.14+.66*(v-mn)/rg;h+=`<td style="background:rgba(114,167,255,${a})"><b>${fmt(v,3)}</b></td>`});h+='</tr>'});$('heatmap').innerHTML=h+'</table>'}

function renderRegimes(rows){if(!rows.length){$('regimes').innerHTML='<div class="empty">No holdout regime data</div>';return}const mx=Math.max(...rows.map(r=>Math.abs(+r.metrics?.net_profit||0)),1);$('regimes').innerHTML=rows.map(r=>{const p=+r.metrics?.net_profit||0,w=Math.abs(p)/mx*100;return `<div class="regime"><span>${esc(r.regime)}</span><div class="bar"><span style="width:${w}%;background:${p>=0?'#46d890':'#ff6b72'}"></span></div><b class="${p>=0?'positive':'negative'}">${fmt(p,1)}</b></div>`}).join('')}

function renderProvenance(d){const sets=(d.datasets||[]).map(x=>`${esc(x.dataset_id)} · ${esc(x.symbol)}<br>SHA ${esc(x.sha256)}`).join('<br><br>');$('provenance').innerHTML=`Run: ${esc(d.run_id)}<br>Status: ${esc(d.status)}<br>Git: ${esc(d.git_commit)}<br>Config SHA: ${esc(d.config_sha256)}<br>Risk SHA: ${esc(d.risk_sha256)}<br><br>${sets}`}

function compareMetrics(d){const r=d?.research,p=d?.portfolio;return r?{name:d.name,kind:d.kind,score:r.final_holdout_score,profit:r.final_holdout?.net_profit,dd:r.final_holdout?.max_drawdown,sharpe:r.final_holdout?.sharpe,sortino:r.final_holdout?.sortino,trades:r.final_holdout?.trades}:p?{name:d.name,kind:d.kind,score:p.portfolio_filtered?.sharpe,profit:p.portfolio_filtered?.net_profit,dd:p.portfolio_filtered?.max_drawdown,sharpe:p.portfolio_filtered?.sharpe,sortino:p.portfolio_filtered?.sortino,trades:p.portfolio_filtered?.trades}:null}
function renderComparison(){const a=compareMetrics(detailA),b=compareMetrics(detailB);if(!a){$('comparison').innerHTML='<div class="empty">No comparable research metrics</div>';return}const rows=[['Score','score'],['Net Profit','profit'],['Max Drawdown','dd'],['Sharpe','sharpe'],['Sortino','sortino'],['Trades','trades']];$('comparison').innerHTML=`<table><thead><tr><th>Metric</th><th>${esc(a.name)}</th>${b?`<th>${esc(b.name)}</th><th>Δ B-A</th>`:''}</tr></thead><tbody>${rows.map(([n,k])=>`<tr><td>${n}</td><td>${fmt(a[k],3)}</td>${b?`<td>${fmt(b[k],3)}</td><td>${fmt((+b[k]||0)-(+a[k]||0),3)}</td>`:''}</tr>`).join('')}</tbody></table>`}

async function selectRun(id){if(!id)return;const [d,t]=await Promise.all([get('/api/experiment?run_id='+encodeURIComponent(id)),get('/api/experiment/trials?run_id='+encodeURIComponent(id)+'&limit=10000')]);detailA=d;trialsA=t||[];renderResearch();document.querySelectorAll('#runRows tr').forEach(x=>x.classList.toggle('selected',x.dataset.id===id))}
async function compareRun(id){detailB=id?await get('/api/experiment?run_id='+encodeURIComponent(id)):null;renderComparison()}

async function loadRuns(){runs=await get('/api/experiments?limit=200')||[];const completed=runs.filter(r=>r.status==='completed');const opts=completed.map(r=>`<option value="${esc(r.run_id)}">${esc(r.name)} · ${esc(r.kind)} · ${new Date(+r.started_at_ms).toLocaleString()}</option>`).join('');$('runA').innerHTML=opts||'<option value="">No completed experiments</option>';$('runB').innerHTML='<option value="">None</option>'+opts;$('runRows').innerHTML=runs.map(r=>`<tr data-id="${esc(r.run_id)}"><td>${esc(r.name)}</td><td>${esc(r.kind)}</td><td class="${r.status==='completed'?'positive':r.status==='failed'?'negative':'warn'}">${esc(r.status)}</td><td>${new Date(+r.started_at_ms).toLocaleString()}</td><td>${esc(String(r.git_commit||'').slice(0,10))}</td></tr>`).join('');document.querySelectorAll('#runRows tr').forEach(tr=>tr.onclick=()=>{$('runA').value=tr.dataset.id;selectRun(tr.dataset.id)});if(completed.length&&!detailA){$('runA').value=completed[0].run_id;await selectRun(completed[0].run_id)}}
$('runA').onchange=()=>selectRun($('runA').value);$('runB').onchange=()=>compareRun($('runB').value);

function healthItem(name,val){const cls=val===true?'good':val===false?'bad':'warn';return `<div class="${cls}"><span class="dot"></span>${esc(name)}: ${val===true?'OK':val===false?'DOWN':esc(String(val??'n/a'))}</div>`}
async function refreshRuntime(){const [s,t,o,e]=await Promise.all([get('/api/status'),get('/api/trades?limit=100'),get('/api/orders?limit=100'),get('/api/equity?limit=500')]);if(!s)return;$('mode').textContent=String(s.mode||'idle').toUpperCase();$('updated').textContent=new Date().toLocaleTimeString();$('balance').textContent=fmt(s.balance,2)+(s.quote_asset?' '+s.quote_asset:'');$('pnl').textContent=fmt(s.total_profit,2);$('pnl').className='value '+((+s.total_profit||0)>=0?'positive':'negative');$('trades').textContent=s.total_trades||0;$('winrate').textContent=fmt(s.win_rate,1)+'%';$('symbol').textContent=s.current_symbol||s.symbol||'—';$('drop').textContent=fmt((+s.drop_rate||0)*100,3)+'%';$('healthGrid').innerHTML=healthItem('Market data',s.market_data_connected)+healthItem('User stream',s.user_stream_connected)+healthItem('Reconciled',s.reconciliation_complete)+healthItem('Kill switch',s.kill_switch_active?false:true)+healthItem('Collector',s.collector_active)+healthItem('Scanner',s.scanner_active);$('tradeRows').innerHTML=(t||[]).map(v=>`<tr><td>${esc(v.symbol)}</td><td>${esc(v.strategy)}</td><td>${fmt(v.entry_price,4)}</td><td>${fmt(v.exit_price,4)}</td><td class="${(+v.net_profit||0)>=0?'positive':'negative'}">${fmt(v.net_profit,2)}</td><td>${esc(v.exit_reason)}</td></tr>`).join('');$('orderRows').innerHTML=(o||[]).map(v=>`<tr><td>${esc(v.symbol)}</td><td>${esc(v.side)}</td><td>${esc(v.state)}</td><td>${fmt(v.executed_quantity,6)}</td><td>${fmt(v.average_fill_price,4)}</td><td>${esc(v.source)}</td></tr>`).join('');lineChart('equity',[{points:(e||[]).map(p=>({y:+p.equity}))}])}

loadRuns();refreshRuntime();setInterval(refreshRuntime,2500);setInterval(loadRuns,15000);
</script></body></html>)HTML";

} // namespace sentum::dashboard
