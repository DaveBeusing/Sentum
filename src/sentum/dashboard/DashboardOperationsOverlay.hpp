#pragma once

#include <string>
#include <string_view>

namespace sentum::dashboard {

inline constexpr std::string_view kOperationsDashboardOverlay = R"HTML(
<style id="sentum-operations-overlay-style">
.ops-grid{display:grid;grid-template-columns:repeat(4,minmax(160px,1fr));gap:12px;margin-bottom:14px}.ops-card{background:linear-gradient(180deg,var(--panel2),var(--panel));border:1px solid var(--line);border-radius:12px;padding:14px}.ops-card .value{font-size:18px}.ops-wide{grid-column:1/-1}.ops-list{display:grid;gap:8px}.ops-row{border:1px solid var(--line);border-radius:8px;padding:10px;background:#0c121c}.ops-row.blocked{border-color:var(--bad)}.ops-row.warn{border-color:var(--warn)}.ops-row.critical{border-color:var(--bad)}.ops-muted{color:var(--muted)}@media(max-width:900px){.ops-grid{grid-template-columns:repeat(2,1fr)}}@media(max-width:560px){.ops-grid{grid-template-columns:1fr}}
</style>
<script id="sentum-operations-overlay-script">
(()=>{
const q=id=>document.getElementById(id),safe=v=>String(v??'—').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const OPERATIONS_SCHEMA_VERSION=1,OPERATIONS_CONTRACT='sentum.operations.v1',OPERATIONS_AUTHORITY='READ_ONLY_PRESENTATION';
const OPERATIONS_REFRESH_MS=2000,OPERATIONS_MAX_BACKOFF_MS=30000;
let operationsFailures=0,operationsTimer=null,operationsInFlight=false,operationsLastSuccess=0;
function badgeClass(v){const s=String(v||'').toUpperCase();return s.includes('CRITICAL')||s.includes('BLOCKED')||s.includes('FORBIDDEN')||s.includes('UNAVAILABLE')?'negative':s.includes('WARNING')||s.includes('ATTENTION')||s.includes('STALE')||s.includes('APPROVAL')?'warn':'positive'}
function validContract(d){return d&&d.schema_version===OPERATIONS_SCHEMA_VERSION&&d.contract===OPERATIONS_CONTRACT&&d.authority===OPERATIONS_AUTHORITY}
function operationsActive(){return q('operationsView')?.classList.contains('active')===true}
function nextOperationsDelay(){return Math.min(OPERATIONS_REFRESH_MS*Math.pow(2,Math.min(operationsFailures,4)),OPERATIONS_MAX_BACKOFF_MS)}
function scheduleOperations(delay){if(operationsTimer!==null)clearTimeout(operationsTimer);operationsTimer=null;if(!operationsActive())return;operationsTimer=setTimeout(()=>{operationsTimer=null;refreshOperations()},Math.max(0,delay))}
function renderTransport(state,detail=''){const el=q('opsTransport');if(!el)return;el.textContent=detail?`${state} · ${detail}`:state;el.className='value '+badgeClass(state)}
function install(){
 if(q('operationsView')) return;
 const tabs=document.querySelector('.tabs');if(!tabs)return;
 const button=document.createElement('button');button.className='tab';button.dataset.view='operationsView';button.textContent='Operations';tabs.appendChild(button);
 const main=document.querySelector('main');if(!main)return;
 const section=document.createElement('section');section.id='operationsView';section.className='view';section.innerHTML=`<div class="ops-grid"><div class="ops-card"><div class="label">Runtime</div><div id="opsRuntime" class="value">—</div></div><div class="ops-card"><div class="label">Governance</div><div id="opsGovernance" class="value">—</div></div><div class="ops-card"><div class="label">Evidence</div><div id="opsEvidence" class="value">—</div></div><div class="ops-card"><div class="label">Transport</div><div id="opsTransport" class="value">CONNECTING</div></div><div class="ops-card"><div class="label">Alerts active</div><div id="opsAlertActive" class="value">0</div></div><div class="ops-card"><div class="label">Alerts acknowledged</div><div id="opsAlertAck" class="value">0</div></div><div class="ops-card"><div class="label">Alerts cleared</div><div id="opsAlertCleared" class="value">0</div></div><div class="ops-card"><div class="label">Critical / Warning</div><div id="opsAlertSeverity" class="value">0 / 0</div></div><div class="ops-card"><div class="label">Pending approvals</div><div id="opsApprovals" class="value">0</div></div><div class="ops-card"><div class="label">Maintenance</div><div id="opsMaintenance" class="value">—</div></div><div class="ops-card"><div class="label">Incident</div><div id="opsIncident" class="value">—</div></div><div class="ops-card"><div class="label">Recovery</div><div id="opsRecovery" class="value">—</div></div><div class="ops-card"><div class="label">Authority</div><div id="opsAuthority" class="value">READ ONLY</div></div><div class="ops-card ops-wide"><div class="section">Alert Center</div><div id="opsAlertList" class="ops-list"></div></div><div class="ops-card ops-wide"><div class="section">Operational Workflows</div><div id="opsWorkflows" class="ops-list"></div></div><div class="ops-card ops-wide"><div class="section">Approval Queue</div><div id="opsApprovalList" class="ops-list"></div></div><div class="ops-card ops-wide"><div class="section">Audit Timeline</div><div id="opsAuditList" class="ops-list"></div></div></div>`;
 const footer=document.querySelector('.footer');main.insertBefore(section,footer||null);
 button.onclick=()=>{document.querySelectorAll('.tab').forEach(x=>x.classList.remove('active'));document.querySelectorAll('.view').forEach(x=>x.classList.remove('active'));button.classList.add('active');section.classList.add('active');scheduleOperations(0)};
 document.querySelectorAll('.tab').forEach(tab=>{if(tab===button)return;tab.addEventListener('click',()=>{if(operationsTimer!==null)clearTimeout(operationsTimer);operationsTimer=null})});
}
function workflowRow(w){if(!w)return '<div class="ops-row ops-muted">Unavailable</div>';const cls=w.blocked?' blocked':w.approval_required?' warn':'';return `<div class="ops-row${cls}"><b>${safe(w.title)}</b> · ${safe(w.state)} · ${safe(w.action||'no action')} · ${safe(w.classification)}${w.request_id?` · request ${safe(w.request_id)}`:''}${w.actor?` · actor ${safe(w.actor)}`:''}</div>`}
function approvalRow(a){const cls=String(a.status||'').includes('BLOCKED')?' blocked':' warn';return `<div class="ops-row${cls}"><b>${safe(a.request_id)}</b> · ${safe(a.action)} · ${safe(a.classification)} · ${safe(a.status)} · actor ${safe(a.actor)} · ${safe(a.reason)}</div>`}
function auditRow(a){return `<div class="ops-row"><b>${safe(a.timestamp_utc)}</b> · ${safe(a.request_id)} · ${safe(a.action)} · actor ${safe(a.actor)} · ${safe(a.outcome)} · ${safe(a.reason)}</div>`}
function alertRow(a){const sev=String(a.severity||'').toUpperCase(),cls=sev==='CRITICAL'?' critical':sev==='WARNING'?' warn':'';return `<div class="ops-row${cls}"><b>${safe(a.severity)} · ${safe(a.state)}</b> · ${safe(a.id)} · ${safe(a.title)} · gen ${safe(a.generation)}${a.acknowledged_by?` · ack ${safe(a.acknowledged_by)}`:''}</div>`}
function renderUnavailable(message){['opsRuntime','opsGovernance','opsEvidence','opsAuthority'].forEach(id=>{const el=q(id);if(el){el.textContent='UNAVAILABLE';el.className='value negative'}});const el=q('opsWorkflows');if(el)el.innerHTML=`<div class="ops-row blocked">${safe(message)}</div>`}
function renderOperations(d){const g=d.governance||{},rt=d.runtime||{},alerts=d.alerts||{};[['opsRuntime',rt.label],['opsGovernance',g.state],['opsEvidence',g.evidence_state],['opsApprovals',g.pending_approvals],['opsMaintenance',g.maintenance_state],['opsIncident',g.incident_state],['opsRecovery',g.recovery_state],['opsAuthority',d.authority],['opsAlertActive',alerts.active??0],['opsAlertAck',alerts.acknowledged??0],['opsAlertCleared',alerts.cleared??0]].forEach(([id,v])=>{const el=q(id);if(el){el.textContent=v??'—';el.className='value '+badgeClass(v)}});const sev=q('opsAlertSeverity');if(sev){sev.textContent=`${alerts.critical??0} / ${alerts.warning??0}`;sev.className='value '+((alerts.critical??0)>0?'negative':(alerts.warning??0)>0?'warn':'positive')}const alertItems=alerts.items||[];q('opsAlertList').innerHTML=alertItems.length?alertItems.map(alertRow).join(''):'<div class="ops-row ops-muted">No operator alerts</div>';const w=d.workflows||{};q('opsWorkflows').innerHTML=[w.maintenance,w.incident,w.recovery].map(workflowRow).join('');const approvals=d.approval_queue?.items||[];q('opsApprovalList').innerHTML=approvals.length?approvals.map(approvalRow).join(''):'<div class="ops-row ops-muted">No approval evidence available</div>';const audit=d.audit_timeline?.items||[];q('opsAuditList').innerHTML=audit.length?audit.map(auditRow).join(''):'<div class="ops-row ops-muted">No audit evidence available</div>'}
async function refreshOperations(){if(!operationsActive()||operationsInFlight)return;operationsInFlight=true;try{const r=await fetch('/api/operations',{cache:'no-store'});if(!r.ok)throw new Error('HTTP '+r.status);const d=await r.json();if(!validContract(d)){operationsFailures++;operationsLastSuccess=0;renderUnavailable('Operations contract mismatch - fail closed');renderTransport('UNAVAILABLE','contract mismatch');scheduleOperations(nextOperationsDelay());return}renderOperations(d);operationsFailures=0;operationsLastSuccess=Date.now();renderTransport('LIVE');scheduleOperations(OPERATIONS_REFRESH_MS)}catch(e){operationsFailures++;const delay=nextOperationsDelay();if(operationsLastSuccess>0){const age=Math.max(0,Math.floor((Date.now()-operationsLastSuccess)/1000));renderTransport('STALE',`${age}s since last success · retry ${Math.round(delay/1000)}s`)}else{renderUnavailable('Operations evidence unavailable');renderTransport('UNAVAILABLE',`retry ${Math.round(delay/1000)}s`)}scheduleOperations(delay)}finally{operationsInFlight=false}}
window.sentumRefreshOperations=refreshOperations;install();
})();
</script>
)HTML";

inline std::string dashboard_html_with_operations(std::string_view base_html) {
	std::string html(base_html);
	if (html.find("sentum-operations-overlay-script") != std::string::npos) return html;
	const auto marker = html.rfind("</body>");
	if (marker == std::string::npos) { html.append(kOperationsDashboardOverlay); return html; }
	html.insert(marker, kOperationsDashboardOverlay);
	return html;
}

} // namespace sentum::dashboard
