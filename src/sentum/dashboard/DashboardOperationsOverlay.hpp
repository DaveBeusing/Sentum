#pragma once

#include <string>
#include <string_view>

namespace sentum::dashboard {

inline constexpr std::string_view kOperationsDashboardOverlay = R"HTML(
<style id="sentum-operations-overlay-style">
.ops-grid{display:grid;grid-template-columns:repeat(4,minmax(160px,1fr));gap:12px;margin-bottom:14px}.ops-card{background:linear-gradient(180deg,var(--panel2),var(--panel));border:1px solid var(--line);border-radius:12px;padding:14px}.ops-card .value{font-size:18px}.ops-wide{grid-column:1/-1}.ops-list{display:grid;gap:8px}.ops-row{border:1px solid var(--line);border-radius:8px;padding:10px;background:#0c121c}.ops-row.blocked{border-color:var(--bad)}.ops-row.warn{border-color:var(--warn)}.ops-muted{color:var(--muted)}@media(max-width:900px){.ops-grid{grid-template-columns:repeat(2,1fr)}}@media(max-width:560px){.ops-grid{grid-template-columns:1fr}}
</style>
<script id="sentum-operations-overlay-script">
(()=>{
const q=id=>document.getElementById(id),safe=v=>String(v??'—').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const OPERATIONS_SCHEMA_VERSION=1,OPERATIONS_CONTRACT='sentum.operations.v1',OPERATIONS_AUTHORITY='READ_ONLY_PRESENTATION';
function badgeClass(v){const s=String(v||'').toUpperCase();return s.includes('CRITICAL')||s.includes('BLOCKED')||s.includes('FORBIDDEN')?'negative':s.includes('WARNING')||s.includes('ATTENTION')||s.includes('STALE')||s.includes('APPROVAL')?'warn':'positive'}
function validContract(d){return d&&d.schema_version===OPERATIONS_SCHEMA_VERSION&&d.contract===OPERATIONS_CONTRACT&&d.authority===OPERATIONS_AUTHORITY}
function install(){
 if(q('operationsView')) return;
 const tabs=document.querySelector('.tabs');
 if(!tabs) return;
 const button=document.createElement('button');button.className='tab';button.dataset.view='operationsView';button.textContent='Operations';tabs.appendChild(button);
 const main=document.querySelector('main');if(!main)return;
 const section=document.createElement('section');section.id='operationsView';section.className='view';section.innerHTML=`<div class="ops-grid"><div class="ops-card"><div class="label">Runtime</div><div id="opsRuntime" class="value">—</div></div><div class="ops-card"><div class="label">Governance</div><div id="opsGovernance" class="value">—</div></div><div class="ops-card"><div class="label">Evidence</div><div id="opsEvidence" class="value">—</div></div><div class="ops-card"><div class="label">Pending approvals</div><div id="opsApprovals" class="value">0</div></div><div class="ops-card"><div class="label">Maintenance</div><div id="opsMaintenance" class="value">—</div></div><div class="ops-card"><div class="label">Incident</div><div id="opsIncident" class="value">—</div></div><div class="ops-card"><div class="label">Recovery</div><div id="opsRecovery" class="value">—</div></div><div class="ops-card"><div class="label">Authority</div><div id="opsAuthority" class="value">READ ONLY</div></div><div class="ops-card ops-wide"><div class="section">Operational Workflows</div><div id="opsWorkflows" class="ops-list"></div></div><div class="ops-card ops-wide"><div class="section">Approval Queue</div><div id="opsApprovalList" class="ops-list"></div></div><div class="ops-card ops-wide"><div class="section">Audit Timeline</div><div id="opsAuditList" class="ops-list"></div></div></div>`;
 const footer=document.querySelector('.footer');main.insertBefore(section,footer||null);
 button.onclick=()=>{document.querySelectorAll('.tab').forEach(x=>x.classList.remove('active'));document.querySelectorAll('.view').forEach(x=>x.classList.remove('active'));button.classList.add('active');section.classList.add('active');refreshOperations()};
}
function workflowRow(w){if(!w)return '<div class="ops-row ops-muted">Unavailable</div>';const cls=w.blocked?' blocked':w.approval_required?' warn':'';return `<div class="ops-row${cls}"><b>${safe(w.title)}</b> · ${safe(w.state)} · ${safe(w.action||'no action')} · ${safe(w.classification)}${w.request_id?` · request ${safe(w.request_id)}`:''}${w.actor?` · actor ${safe(w.actor)}`:''}</div>`}
function approvalRow(a){const cls=String(a.status||'').includes('BLOCKED')?' blocked':' warn';return `<div class="ops-row${cls}"><b>${safe(a.request_id)}</b> · ${safe(a.action)} · ${safe(a.classification)} · ${safe(a.status)} · actor ${safe(a.actor)} · ${safe(a.reason)}</div>`}
function auditRow(a){return `<div class="ops-row"><b>${safe(a.timestamp_utc)}</b> · ${safe(a.request_id)} · ${safe(a.action)} · actor ${safe(a.actor)} · ${safe(a.outcome)} · ${safe(a.reason)}</div>`}
function renderUnavailable(message){['opsRuntime','opsGovernance','opsEvidence','opsAuthority'].forEach(id=>{const el=q(id);if(el){el.textContent='UNAVAILABLE';el.className='value negative'}});const el=q('opsWorkflows');if(el)el.innerHTML=`<div class="ops-row blocked">${safe(message)}</div>`}
async function refreshOperations(){
 try{const r=await fetch('/api/operations',{cache:'no-store'});if(!r.ok)throw new Error('HTTP '+r.status);const d=await r.json();if(!validContract(d)){renderUnavailable('Operations contract mismatch - fail closed');return}const g=d.governance||{},rt=d.runtime||{};
 [['opsRuntime',rt.label],['opsGovernance',g.state],['opsEvidence',g.evidence_state],['opsApprovals',g.pending_approvals],['opsMaintenance',g.maintenance_state],['opsIncident',g.incident_state],['opsRecovery',g.recovery_state],['opsAuthority',d.authority]].forEach(([id,v])=>{const el=q(id);if(el){el.textContent=v??'—';el.className='value '+badgeClass(v)}});
 const w=d.workflows||{};q('opsWorkflows').innerHTML=[w.maintenance,w.incident,w.recovery].map(workflowRow).join('');
 const approvals=d.approval_queue?.items||[];q('opsApprovalList').innerHTML=approvals.length?approvals.map(approvalRow).join(''):'<div class="ops-row ops-muted">No approval evidence available</div>';
 const audit=d.audit_timeline?.items||[];q('opsAuditList').innerHTML=audit.length?audit.map(auditRow).join(''):'<div class="ops-row ops-muted">No audit evidence available</div>';
 }catch(e){renderUnavailable('Operations evidence unavailable')}
}
window.sentumRefreshOperations=refreshOperations;install();setInterval(()=>{if(q('operationsView')?.classList.contains('active'))refreshOperations()},2000);
})();
</script>
)HTML";

inline std::string dashboard_html_with_operations(std::string_view base_html) {
	std::string html(base_html);
	if (html.find("sentum-operations-overlay-script") != std::string::npos) return html;
	const auto marker = html.rfind("</body>");
	if (marker == std::string::npos) {
		html.append(kOperationsDashboardOverlay);
		return html;
	}
	html.insert(marker, kOperationsDashboardOverlay);
	return html;
}

} // namespace sentum::dashboard
