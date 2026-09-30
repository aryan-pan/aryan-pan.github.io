'use strict';
let currentUser=null,ready=false,viewSequence=0;
let reviewBrowsePath='/review';
function newRequestId(){
  if(typeof crypto.randomUUID==='function')return crypto.randomUUID();
  // getRandomValues also works on HTTP addresses reached through an encrypted tailnet.
  const bytes=crypto.getRandomValues(new Uint8Array(16));bytes[6]=(bytes[6]&15)|64;bytes[8]=(bytes[8]&63)|128;
  const hex=Array.from(bytes,b=>b.toString(16).padStart(2,'0')).join('');
  return [hex.slice(0,8),hex.slice(8,12),hex.slice(12,16),hex.slice(16,20),hex.slice(20)].join('-');
}
const modeNames={challenge:'Opportunity suggestion',solution:'Research approach',expertise:'Collaboration inquiry',seedling:'Research concept',team:'Connection support request',join:'Community of Interest membership',community:'Community of Interest suggestion'};
const statusNames={draft:'Draft',submitted:'Submitted',under_review:'Under review',needs_changes:'Changes requested',accepted:'Accepted',declined:'Declined',withdrawn:'Withdrawn'};
const recordSchemas={opportunities:['title','type','theme','summary','skills[]','community','stage','problem','users?','outcome?','questions[]?','evaluation?','readiness?','collaboration?','commitment?'],projects:['title','summary','theme','stage','opportunity','community','approach','next'],communities:['title','summary','theme','audience','topics[]','format','output']};
const fieldLabels={collaboration:'Collaborator roles and contributions',commitment:'Initial participation',title:'Title',type:'Opportunity type',theme:'Theme',summary:'Summary',skills:'Expertise needed',community:'Related community',stage:'Stage',problem:'Public overview',users:'Intended users',outcome:'Desired outcome',questions:'Discussion Questions',evaluation:'Public evaluation context',readiness:'Readiness and requirements',audience:'Intended participants',topics:'Discussion topics',format:'Participation Format',output:'Shared Activities and Outputs',opportunity:'Originating opportunity',approach:'Approach',next:'Next decision'};
const stamp=value=>new Date(value).toLocaleString(undefined,{dateStyle:'medium',timeStyle:'short'});
function responseStatus(status,mode){
 if(['expertise','team'].includes(mode))return {submitted:'Received',under_review:'Coordinator review',needs_changes:'Update requested',accepted:'Ready for follow-up',declined:'Closed'}[status]||statusNames[status]||status;
 return statusNames[status]||status;
}
const statusBadge=(s,mode)=>`<span class="status status-${esc(s)}">${esc(responseStatus(s,mode))}</span>`;
function sharingNote(mode){
 const visibility=draft.status==='needs_changes'?'Saved edits to this submitted response remain visible to coordinators before you resubmit.':'New drafts stay with your account until submitted.';
 if(['challenge','community'].includes(mode))return `<div class="form-sharing"><strong>Prepared for the Directory</strong>The overview and collaboration details may be published after review. Your affiliation is for coordinators. <p>${visibility}</p></div>`;
 if(['solution','seedling'].includes(mode))return `<div class="form-sharing"><strong>Response Visibility</strong>${visibility}</div>`;
 return `<div class="form-sharing"><strong>Sent to Marketplace Coordinators</strong>Your introduction is not displayed in the directory or sent directly to a project team. <p>${visibility}</p></div>`;
}
const loading=()=>`<div class="wrap section" role="status">Loading…</div>`;
const note=(message,error=false)=>`<p class="notice ${error?'error':''}" role="${error?'alert':'status'}">${esc(message)}</p>`;
async function api(path,options={}){
  const base=location.pathname.startsWith('/ai-marketplace')?'/ai-marketplace':'';
  const response=await fetch(base+'/api'+path,{...options,headers:{'Content-Type':'application/json','X-Marketplace-Request':'1',...options.headers},body:options.body===undefined?undefined:JSON.stringify(options.body)});
  const data=await response.json();if(!response.ok)throw new Error(data.error||'The request could not be completed.');return data;
}
async function refreshCatalogue(){D=await api('/catalogue');}
async function refreshPersonalActivity(){personalActivity=currentUser?await api('/activity'):{submissions:[],memberships:[]};return personalActivity;}
function nextStep(s,review=false){
 if(s.status==='draft')return 'Continue editing, then review and submit your details.';
 if(s.status==='needs_changes')return 'Read the coordinator’s feedback and update your response.';
 if(s.status==='submitted')return 'Awaiting coordinator review. Check here for feedback.';
 if(s.status==='under_review')return 'A coordinator is reviewing your response.';
 if(s.status==='accepted')return s.mode==='join'?(review?'Membership registration accepted.':personalActivity.memberships.some(g=>g.id===s.record_id)?'You have joined this community.':'This membership registration was accepted. You are no longer a member.'):s.mode==='expertise'||s.mode==='team'?'Ready for follow-up. Read the coordinator’s note under History and Feedback.':s.published_id?'Published in the marketplace.':'Accepted for the next step. Read the coordinator’s feedback.';
 if(s.status==='declined')return 'Read the coordinator’s feedback for the decision.';
 return 'This response has been withdrawn.';
}
function navigation(){
  const nav=document.querySelector('nav');
  if(currentUser?.isAdmin&&!nav.querySelector('[data-coordinator]'))nav.insertAdjacentHTML('beforeend','<a data-coordinator href="#/review">Coordinator</a>');
  if(!currentUser?.isAdmin)nav.querySelector('[data-coordinator]')?.remove();
  if(nav.querySelector('[data-account]'))nav.querySelector('[data-account]').textContent=currentUser?'Account':'Sign In';
  if(!nav.querySelector('[data-account]'))nav.insertAdjacentHTML('beforeend',`<a data-account href="#/account">${currentUser?'Account':'Sign in'}</a>`);
}
function accountPage(){
 if(currentUser)return heading('Account','Account',`Signed in as ${esc(currentUser.name)}.`)+`<div class="form-wrap"><button class="button" id="logout">Log out</button></div>`;
 return heading('Account','Sign in or create an account','Use a demo username and password to save drafts and submit responses.')+`<div class="account-grid"><form class="form-wrap account-form" id="login-form"><h2>Sign in</h2><div id="login-message"></div><div class="field"><label for="login-username">Username</label><input id="login-username" name="username" autocomplete="username" required></div><div class="field"><label for="login-password">Password</label><input id="login-password" name="password" type="password" autocomplete="current-password"></div><button class="button" type="submit">Sign in</button></form><form class="form-wrap account-form" id="register-form"><h2>Create account</h2><div id="register-message"></div><div class="field"><label for="register-username">Username</label><small>Use 2–64 letters, numbers, dots, hyphens or underscores.</small><input id="register-username" name="username" autocomplete="username" required></div><div class="field"><label for="register-name">Display name</label><input id="register-name" name="displayName" autocomplete="name"></div><div class="field"><label for="register-password">Password</label><small>A password is optional for this demo account.</small><input id="register-password" name="password" type="password" autocomplete="new-password"></div><button class="button" type="submit">Create account</button></form></div>`;
}
function bindAuth(){
 const login=document.querySelector('#login-form'),register=document.querySelector('#register-form'),logout=document.querySelector('#logout');
 if(logout)logout.onclick=async()=>{await api('/auth/logout',{method:'POST'});currentUser=null;personalActivity={submissions:[],memberships:[]};navigation();render(false);};
 for(const [form,path,target] of [[login,'/auth/login','#login-message'],[register,'/auth/register','#register-message']])if(form)form.onsubmit=e=>{e.preventDefault();withButton(form.querySelector('button'),async()=>{const result=await api(path,{method:'POST',body:Object.fromEntries(new FormData(form))});currentUser=result.user;await refreshPersonalActivity();navigation();const returnTo=getRoute().params.get('returnTo');go(returnTo?.startsWith('/participate')?returnTo:'/');},target)};
}
const originalRender=render,originalReview=reviewForm;
render=function(focus=true){
  const sequence=++viewSequence,{path,params}=getRoute();
  document.querySelectorAll('nav a').forEach(a=>{const target=a.hash.slice(1),active=target==='/'?path==='/':path===target||path.startsWith(target+'/');a.classList.toggle('active',active);if(active)a.setAttribute('aria-current','page');else a.removeAttribute('aria-current');});
  if(!ready){main.innerHTML=path==='/'?homeBase:loading();return;}
  if(path==='/activity'||path.startsWith('/activity/')||path==='/review'||path.startsWith('/review/')||path==='/manage'){
    main.innerHTML=loading();document.querySelector('nav').classList.remove('open');document.querySelector('.menu').setAttribute('aria-expanded','false');
    const work=path==='/activity'?activityPage():path.startsWith('/activity/')?submissionPage(path.split('/')[2],false):path==='/review'?reviewPage(params):path.startsWith('/review/')?submissionPage(path.split('/')[2],true):managePage(params);
    work.then(html=>{if(sequence!==viewSequence)return;main.innerHTML=html;bindWorkspace(path,params);document.title=(main.querySelector('h1')?.textContent||'Marketplace')+' | UB AI Innovation Marketplace';if(focus){window.scrollTo(0,0);main.focus();}}).catch(error=>{if(sequence===viewSequence)main.innerHTML=heading('Workspace','Unable to Load This Page')+`<div class="wrap section">${note(error.message,true)}<button class="button" id="retry-page">Try again</button></div>`;document.querySelector('#retry-page')?.addEventListener('click',()=>render(false));});
    return;
  }
  if(path==='/account'){main.innerHTML=accountPage();bindAuth();if(focus){window.scrollTo(0,0);main.focus();}return;}
  if(path==='/participate'&&params.get('draft')&&(draft.id!==params.get('draft')||!params.get('mode'))){
    main.innerHTML=loading();api('/submissions/'+encodeURIComponent(params.get('draft'))).then(saved=>{
      if(sequence!==viewSequence)return;const p=new URLSearchParams({mode:saved.mode,draft:saved.id});if(saved.record_id)p.set('id',saved.record_id);
      history.replaceState(null,'','#/participate?'+p);draft={key:saved.mode+(saved.record_id||''),values:saved.data,id:saved.id,version:saved.version,status:saved.status,feedback:saved.status==='needs_changes'?saved.events?.filter(e=>e.note).at(-1)?.note:''};render(false);
    }).catch(error=>{if(sequence===viewSequence)main.innerHTML=heading('My Activity','Unable to Load Draft')+`<div class="wrap section">${note(error.message,true)}${link('/activity','My Activity')}</div>`;});return;
  }
  originalRender(focus);
  if(path==='/participate'&&document.querySelector('#participation-form')){
    const form=document.querySelector('#participation-form');
    if(!currentUser){form.innerHTML=note('Create a demo account or sign in to save and submit your details.')+link('/account?returnTo='+encodeURIComponent(location.hash.slice(1)),'Sign in or create an account','button');return;}
    if(draft.status&&!['draft','needs_changes'].includes(draft.status)){form.innerHTML=note('This submission is no longer editable.')+link('/activity/'+draft.id,'View submission');return;}
    form.insertAdjacentHTML('afterbegin',`<p class="account-label">Submitting as ${esc(currentUser.name)}</p><div id="save-message"></div>${draft.feedback?`<div class="feedback-context"><strong>Coordinator Feedback</strong><p class="preserve">${esc(draft.feedback)}</p></div>`:''}${sharingNote(params.get('mode'))}`);
    form.querySelector('.form-actions').insertAdjacentHTML('beforeend',`<button type="button" class="button outline" id="save-draft">${draft.status==='needs_changes'?'Save changes':'Save draft'}</button>`);
    document.querySelector('#save-draft').onclick=()=>withButton(document.querySelector('#save-draft'),async()=>{draft.values=participationValues(form);await saveDraft();document.querySelector('#save-message').innerHTML=note(draft.status==='needs_changes'?'Changes saved and visible to coordinators. Review and resubmit when ready.':'Draft saved. You can return to it from My Activity.');},'#save-message');
  }
  if(path.startsWith('/projects/')){
    const project=D.projects.find(x=>x.id===path.split('/')[2]);if(project){document.querySelector('.detail-body').insertAdjacentHTML('beforeend',section('Project Updates',project.updates?.length?project.updates.map(u=>`<article class="update"><p class="meta">${esc(stamp(u.created_at))} · ${esc(u.author_name)}</p><p class="preserve">${esc(u.body)}</p></article>`).join(''):'<p>No updates have been posted.</p>'));if(currentUser?.isAdmin)document.querySelector('.aside').insertAdjacentHTML('beforeend',link('/manage?kind=projects&id='+project.id,'Manage project','button outline'));}
  }
  if(path.startsWith('/communities/')){const group=D.communities.find(x=>x.id===path.split('/')[2]);if(group)document.querySelector('.aside').insertAdjacentHTML('beforeend',`<p class="membership-count">${group.memberCount} ${group.memberCount===1?'member':'members'}</p>`);}
};
async function withButton(button,operation,target){button.disabled=true;const original=button.textContent;button.textContent='Saving…';try{await operation();}catch(error){const destination=document.querySelector(target);if(destination)destination.innerHTML=note(error.message,true);}finally{if(button.isConnected){button.disabled=false;button.textContent=original;}}}
async function saveDraft(){
  const {params}=getRoute(),mode=params.get('mode'),record_id=params.get('id')||null;
  const current=draft;
  let saved;
  if(current.id)saved=await api('/submissions/'+current.id,{method:'PATCH',body:{version:current.version,action:'save',data:current.values}});
  else{current.requestId??=newRequestId();saved=await api('/submissions',{method:'POST',body:{id:current.requestId,mode,record_id,data:current.values}});}
  current.id=saved.id;current.version=saved.version;current.status=saved.status;
  personalActivity.submissions=[saved,...personalActivity.submissions.filter(s=>s.id!==saved.id)];
  if(draft===current&&getRoute().path==='/participate'){const p=new URLSearchParams(params);p.set('draft',saved.id);history.replaceState(null,'','#/participate?'+p);}
  return saved;
}
reviewForm=function(mode,id){
  originalReview(mode,id);
  document.querySelector('#form-stage').insertAdjacentHTML('afterbegin',sharingNote(mode)+'<div id="submit-message"></div>');
  document.querySelector('#form-stage .form-actions').insertAdjacentHTML('beforeend',`<button class="button" id="submit-details">${mode==='join'?'Join community':mode==='expertise'||mode==='team'?'Send to coordinator':'Submit details'}</button>`);
  document.querySelector('#submit-details').onclick=()=>withButton(document.querySelector('#submit-details'),async()=>{
    const current=draft,sequence=viewSequence,values={...draft.values};const persisted=await saveDraft();const saved=await api('/submissions/'+persisted.id,{method:'PATCH',body:{version:persisted.version,action:'submit',data:values}});if(draft===current)draft={key:'',values:{}};await Promise.all([refreshCatalogue(),refreshPersonalActivity()]);if(sequence!==viewSequence)return;
    main.innerHTML=heading('My Activity',mode==='join'?'Community of Interest Joined':mode==='expertise'||mode==='team'?'Inquiry Received':'Submission Received',mode==='join'?'Your membership is saved. You can review discussion areas and related opportunities on the community page.':'A coordinator can review your response and record feedback here. Check My Activity for updates.')+`<div class="form-wrap">${statusBadge(saved.status,mode)}<p style="margin-top:18px">Reference: ${esc(saved.id.slice(0,8))}</p>${link(mode==='join'?'/communities/'+id:'/activity/'+saved.id,mode==='join'?'View community':'View response','button')} ${link('/activity','My Activity','button outline')}</div>`;main.focus();
  },'#submit-message');
};
function rowTitle(s){return s.data.title||s.record_title||[...D.opportunities,...D.communities].find(r=>r.id===s.record_id)?.title||modeNames[s.mode];}
function submissionRow(s,review=false){
 const editable=['draft','needs_changes'].includes(s.status),path=review?'/review/'+s.id:editable?'/participate?draft='+s.id:'/activity/'+s.id;
 return `<article class="activity-row"><div>${statusBadge(s.status,s.mode)}<h3>${link((review?'/review/':'/activity/')+s.id,titleText(rowTitle(s)),'')}</h3><p>${esc(modeNames[s.mode])}${review?' · '+esc(s.owner_name):''} · ${esc(stamp(s.updated_at))}</p>${review&&s.mode==='expertise'?`<p class="row-meta">${esc(s.data.affiliation)}</p>${s.data.selectedSkills?.length?chips(s.data.selectedSkills):''}${s.data.skills?`<p>Additional expertise: ${esc(s.data.skills)}</p>`:''}${s.data.contribution?`<p class="inquiry-excerpt">${esc(s.data.contribution.slice(0,220))}${s.data.contribution.length>220?'…':''}</p>`:''}${s.data.availability?`<p>Availability: ${esc(s.data.availability)}</p>`:''}`:''}${!review?`<p class="next-step">${esc(nextStep(s))}</p>`:''}</div>${link(path,review?'Review details':s.status==='draft'?'Continue draft':s.status==='needs_changes'?'Revise response':'View details','text-link row-action')}</article>`;
}
async function activityPage(){
 const data=await refreshPersonalActivity(),action=data.submissions.filter(s=>['draft','needs_changes'].includes(s.status)),remaining=data.submissions.filter(s=>!['draft','needs_changes'].includes(s.status));
 return heading('My Activity','My Activity','Your responses, coordinator feedback and communities.')+`<section class="wrap section workspace">${action.length?`<section aria-labelledby="attention-heading"><h2 id="attention-heading">Needs Your Attention <span class="section-count">${action.length}</span></h2>${action.map(s=>submissionRow(s)).join('')}</section>`:''}<h2>Submitted Responses</h2>${remaining.length?remaining.map(s=>submissionRow(s)).join(''):`<div class="empty"><p>No submitted responses yet.</p>${link('/opportunities','Browse opportunities','button outline')}</div>`}<h2>Your Communities of Interest</h2>${data.memberships.length?data.memberships.map(g=>`<div class="activity-row"><h3>${link('/communities/'+g.id,titleText(g.title),'')}</h3><button class="button outline" data-leave="${esc(g.id)}">Leave group</button></div>`).join(''):`<p>You have not joined a community. ${link('/communities','Browse communities')}</p>`}<div id="workspace-message"></div></section>`;
}
async function reviewPage(params){
 reviewBrowsePath='/review'+(params.size?'?'+params.toString():'');
 const q=new URLSearchParams({page:params.get('page')||'1',status:params.get('status')||'all',mode:params.get('mode')||'all',record:params.get('record')||'all'});
 const [data,records]=await Promise.all([api('/review?'+q),api('/records')]);
 const choices=records.items.filter(r=>r.kind!=='projects').sort((a,b)=>Number(a.kind==='communities')-Number(b.kind==='communities'));
 return heading('Coordinator','Review Submissions','Review contributors for a specific opportunity and record the next step.')+`<section class="wrap section workspace"><div class="workspace-toolbar">${link('/manage','Manage opportunities, projects & communities','button outline')}</div><div class="filters review-filters"><label>Status<select id="review-status">${['all','submitted','under_review','needs_changes','accepted','declined','withdrawn'].map(s=>`<option value="${s}" ${q.get('status')===s?'selected':''}>${s==='all'?'All statuses':s==='accepted'&&q.get('mode')==='all'?'Accepted / ready for follow-up':responseStatus(s,q.get('mode'))}</option>`).join('')}</select></label><label>Response type<select id="review-mode">${[['all','All options'],...Object.entries(modeNames)].map(([v,l])=>`<option value="${v}" ${q.get('mode')===v?'selected':''}>${l}</option>`).join('')}</select></label><label class="review-record-label">Opportunity or group<select id="review-record"><option value="all">All Opportunities and groups</option>${choices.map(r=>`<option value="${esc(r.id)}" ${q.get('record')===r.id?'selected':''}>${titleText(r.title)}${r.status==='archived'?' (archived)':''}</option>`).join('')}</select></label></div><p>${data.total} ${data.total===1?'response':'responses'}</p>${data.items.length?data.items.map(s=>submissionRow(s,true)).join(''):'<div class="empty">No responses match these filters.</div>'}<div class="pagination">${data.page>1?link('/review?'+new URLSearchParams({...Object.fromEntries(q),page:data.page-1}),'← Previous'):''}${data.page*25<data.total?link('/review?'+new URLSearchParams({...Object.fromEntries(q),page:data.page+1}),'Next →'):''}</div></section>`;
}
let activeSubmission=null;
async function submissionPage(id,review){
  const s=await api((review?'/review/':'/submissions/')+encodeURIComponent(id));activeSubmission=s;
  const cfg=formModes[s.mode],record=[...D.opportunities,...D.communities].find(x=>x.id===s.record_id);
  return heading(review?'Coordinator':'My Activity',titleText(rowTitle(s)))+`<div class="wrap detail-layout"><div class="detail-body">${statusBadge(s.status,s.mode)}<p class="next-step">${esc(nextStep(s,review))}</p><p class="account-label">${esc(modeNames[s.mode])} · ${esc(stamp(s.created_at))}</p>${review?`<p>${esc(s.owner_name)} · ${esc(s.owner_email)}</p>`:''}${record?`<p>Related to: ${link((s.mode==='join'?'/communities/':'/opportunities/')+record.id,titleText(record.title))}</p>`:''}<dl>${cfg.fields.map(([key,label])=>`<div class="review-row"><dt>${label}</dt><dd>${displayFieldValue(s.data[key])}</dd></div>`).join('')}</dl><h2 style="margin-top:30px">History and Feedback</h2>${s.events.length?s.events.map(e=>`<article class="update">${statusBadge(e.status,s.mode)}<p class="meta">${esc(stamp(e.created_at))} · ${esc(e.actor_name)}</p>${e.note?`<p class="preserve">${esc(e.note)}</p>`:''}</article>`).join(''):'<p>No activity recorded yet.</p>'}</div><aside class="aside"><h3>${review?'Review Actions':'Submission Actions'}</h3><div id="workspace-message"></div>${!review&&['draft','needs_changes'].includes(s.status)?link('/participate?draft='+s.id,'Edit details','button'):''}${!review&&(['draft','submitted','under_review','needs_changes'].includes(s.status)||s.status==='accepted'&&['expertise','team'].includes(s.mode))?'<button class="button outline" id="withdraw-submission">Withdraw response</button>':''}${review&&['submitted','under_review','needs_changes'].includes(s.status)?`<form id="review-form"><div class="field"><label for="decision">Decision</label><select id="decision" name="status">${(s.status==='submitted'?['under_review','needs_changes','accepted','declined']:s.status==='under_review'?['needs_changes','accepted','declined']:['under_review','accepted','declined']).map(v=>`<option value="${v}">${responseStatus(v,s.mode)}</option>`).join('')}</select></div><div class="field"><label for="feedback">Feedback and next step</label><small>Explain the decision and any action the participant should take.</small><textarea id="feedback" name="note" maxlength="3000"></textarea></div><button class="button" type="submit">Save decision</button></form>`:''}${review&&s.status==='accepted'&&!s.published_id&&['challenge','community'].includes(s.mode)?link('/manage?kind='+({challenge:'opportunities',community:'communities',solution:'projects',seedling:'projects'})[s.mode]+'&source='+s.id,'Create '+({challenge:'opportunity',community:'group',solution:'project',seedling:'project'})[s.mode],'button'):''}${s.published_id?link('/'+({challenge:'opportunities',community:'communities',solution:'projects',seedling:'projects'})[s.mode]+'/'+s.published_id,'View published record','button outline'):''}${link(review?reviewBrowsePath:'/activity',review?'Back to responses':'Back to activity','button outline')}</aside></div>`;
}
let managedRecords=[],editingRecord=null,editorSource=null,recordRequestId=null;
function editorField(def,data){
  const key=def.replace(/\[\]|\?/g,''),array=def.includes('[]'),value=data[key]??(array?[]:'');
  let choices=null;if(key==='community')choices=managedRecords.filter(r=>r.kind==='communities'&&r.status==='active').map(r=>[r.id,r.title]);if(key==='opportunity')choices=managedRecords.filter(r=>r.kind==='opportunities'&&r.status==='active').map(r=>[r.id,r.title]);if(key==='type')choices=[['challenge','Proof-of-Concept Challenge'],['seedling','AI Seedling']];if(key==='stage'&&getRoute().params.get('kind')==='projects')choices=['Scoping','Development','Evaluation planning','Evaluation','Completed','Concluded'].map(x=>[x,x]);
  const control=choices?`<select id="r-${key}" name="${key}" ${def.endsWith('?')?'':'required'}><option value="">Select…</option>${choices.map(([v,l])=>`<option value="${esc(v)}" ${v===value?'selected':''}>${esc(l)}</option>`).join('')}</select>`:array||!['title','theme','stage','audience','users'].includes(key)?`<textarea id="r-${key}" name="${key}" ${def.endsWith('?')?'':'required'} maxlength="3000">${esc(array?value.join('\n'):value)}</textarea>`:`<input id="r-${key}" name="${key}" value="${esc(value)}" ${def.endsWith('?')?'':'required'} maxlength="300">`;
  return `<div class="field"><label for="r-${key}">${fieldLabels[key]}${def.endsWith('?')?' (optional)':''}${array?' (one per line)':''}</label>${control}</div>`;
}
async function managePage(params){
  const result=await api('/records');managedRecords=result.items;const kind=recordSchemas[params.get('kind')]?params.get('kind'):'opportunities',id=params.get('id');editingRecord=managedRecords.find(r=>r.id===id)||null;editorSource=null;recordRequestId=newRequestId();
  if(params.get('source')){editorSource=await api('/review/'+encodeURIComponent(params.get('source')));if(!['challenge','community'].includes(editorSource.mode))throw new Error('Inquiries and previous proposals cannot be published as directory records. Prepare a separate public description.');}
  if(id&&!editingRecord)throw new Error('Record not found.');
  let data=editingRecord||{};
  if(editorSource){const s=editorSource.data,related=D.opportunities.find(o=>o.id===editorSource.record_id);data={title:s.title||related?.title||'',summary:s.problem||s.purpose||'',theme:related?.theme||'',skills:s.expertiseNeeded?.split('\n').map(x=>x.trim()).filter(Boolean)||[],commitment:s.commitment||'',community:related?.community||'',opportunity:related?.id||'',stage:'Scoping',type:'challenge',problem:s.problem||'',users:s.users||'',outcome:s.outcome||'',approach:'',audience:s.audience||'',format:s.purpose||''};}
  const editor=id||params.get('new')||editorSource;
  return heading('Coordinator',editor?(editingRecord?'Edit Record':'Create Record'):'Manage Records')+`<section class="wrap section workspace">${editor?`<div class="form-wrap">${editorSource?note('Review and complete these details before publishing.'):''}<div id="workspace-message"></div><form id="record-form" data-kind="${kind}">${recordSchemas[kind].map(f=>editorField(f,data)).join('')}<div class="form-actions">${link('/manage?kind='+kind,'Cancel')}<button class="button" type="submit">${editingRecord?'Save changes':'Publish record'}</button></div></form>${editingRecord?`<div class="archive-actions"><button class="button outline" id="archive-record">${editingRecord.status==='active'?'Archive':'Restore'} record</button></div>`:''}${editingRecord?.kind==='projects'?`<h2>Post a Project Update</h2><form id="update-form"><div class="field"><label for="project-update">Update</label><textarea id="project-update" required maxlength="3000"></textarea></div><button class="button" type="submit">Post update</button></form>`:''}</div>`:`<div class="workspace-toolbar">${link('/review','Review Submissions')}${link('/manage?kind='+kind+'&new=1','Create '+({opportunities:'opportunity',communities:'group',projects:'project'})[kind],'button')}</div><div class="tabs">${Object.keys(recordSchemas).map(k=>link('/manage?kind='+k,({opportunities:'Opportunities',communities:'Communities of Interest',projects:'Projects'})[k],kind===k?'selected':'')).join('')}</div>${managedRecords.filter(r=>r.kind===kind).map(r=>`<div class="activity-row"><div><span class="status">${r.status==='active'?'Published':'Archived'}</span><h3>${titleText(r.title)}</h3><p>${esc(r.theme)}</p></div>${link('/manage?kind='+kind+'&id='+r.id,'Edit →')}</div>`).join('')}`}</section>`;
}
function bindWorkspace(path,params){
  document.querySelectorAll('[data-leave]').forEach(button=>button.onclick=()=>withButton(button,async()=>{await api('/memberships/'+button.dataset.leave,{method:'DELETE'});await Promise.all([refreshCatalogue(),refreshPersonalActivity()]);render(false);},'#workspace-message'));
  ['review-status','review-mode','review-record'].forEach(id=>{const input=document.getElementById(id);if(input)input.onchange=()=>{const p=new URLSearchParams(params);p.set(id==='review-status'?'status':id==='review-mode'?'mode':'record',input.value);p.delete('page');go('/review?'+p);};});
  const withdraw=document.querySelector('#withdraw-submission');if(withdraw)withdraw.onclick=()=>withButton(withdraw,async()=>{await api('/submissions/'+activeSubmission.id,{method:'PATCH',body:{version:activeSubmission.version,action:'withdraw'}});if(draft.id===activeSubmission.id)draft={key:'',values:{}};await refreshPersonalActivity();render(false);},'#workspace-message');
  const review=document.querySelector('#review-form');if(review){const decision=review.querySelector('#decision'),feedback=review.querySelector('#feedback');const syncFeedback=()=>{feedback.required=['needs_changes','declined'].includes(decision.value)||decision.value==='accepted'&&['expertise','team'].includes(activeSubmission.mode);};decision.onchange=syncFeedback;syncFeedback();}if(review)review.onsubmit=e=>{e.preventDefault();withButton(review.querySelector('button'),async()=>{const values=Object.fromEntries(new FormData(review));await api('/review/'+activeSubmission.id,{method:'PATCH',body:{...values,version:activeSubmission.version}});await refreshPersonalActivity();render(false);},'#workspace-message');};
  const form=document.querySelector('#record-form');if(form)form.onsubmit=e=>{e.preventDefault();withButton(form.querySelector('button'),async()=>{
    const kind=form.dataset.kind,data=Object.fromEntries(new FormData(form));for(const f of recordSchemas[kind])if(f.includes('[]')){const key=f.replace(/\[\]|\?/g,'');data[key]=data[key].split('\n').map(x=>x.trim()).filter(Boolean);}
    await api(editingRecord?'/records/'+editingRecord.id:'/records',{method:editingRecord?'PATCH':'POST',body:{id:recordRequestId,kind,data,version:editingRecord?.version,source_id:editorSource?.id}});await refreshCatalogue();go('/manage?kind='+kind);
  },'#workspace-message');};
  const archive=document.querySelector('#archive-record');if(archive)archive.onclick=()=>withButton(archive,async()=>{await api('/records/'+editingRecord.id,{method:'PATCH',body:{version:editingRecord.version,status:editingRecord.status==='active'?'archived':'active'}});await refreshCatalogue();go('/manage?kind='+editingRecord.kind);},'#workspace-message');
  const update=document.querySelector('#update-form');if(update){let updateId=newRequestId();update.onsubmit=e=>{e.preventDefault();withButton(update.querySelector('button'),async()=>{await api('/projects/'+editingRecord.id+'/updates',{method:'POST',body:{id:updateId,body:document.querySelector('#project-update').value}});await refreshCatalogue();updateId=newRequestId();update.reset();document.querySelector('#workspace-message').innerHTML=note('Project update posted.');},'#workspace-message');};}
}
async function start(){if(getRoute().path!=='/')main.innerHTML=loading();try{const [catalogue,session]=await Promise.all([api('/catalogue'),api('/me')]);D=catalogue;currentUser=session.user;await refreshPersonalActivity();ready=true;navigation();render(false);}catch(error){main.innerHTML=(getRoute().path==='/'?homeBase:heading('Marketplace','Unable to connect'))+`<div class="wrap section">${note(error.message,true)}<button class="button" id="retry-start">Try again</button></div>`;document.querySelector('#retry-start').onclick=start;}}
start();
