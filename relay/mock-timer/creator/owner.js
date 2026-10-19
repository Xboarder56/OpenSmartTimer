'use strict';
const $ = id => document.getElementById(id);
const secret = location.hash.slice(1) || sessionStorage.getItem('ost-owner') || '';
if (secret) sessionStorage.setItem('ost-owner', secret);
history.replaceState(null, '', location.pathname);
let pendingRequest = null;
try {
  const saved = JSON.parse(sessionStorage.getItem('ost-owner-pending'));
  if (saved && Number.isInteger(saved.ttl_seconds) && saved.ttl_seconds >= 60 && saved.ttl_seconds <= 86400
      && typeof saved.label === 'string' && typeof saved.request_id === 'string') {
    pendingRequest = saved;
    $('lifetime').value = saved.ttl_seconds / 60;
    $('label').value = saved.label;
  }
} catch (_) { sessionStorage.removeItem('ost-owner-pending'); }
let busy = false;
let canShare = false;
let lastConnectionCheck = 0;
function sharingAvailability(available) {
  canShare = available;
  $('shareControls').disabled = !available;
  $('generate').disabled = busy || !available;
  $('sharingUnavailable').textContent = available ? '' : 'Connect the timer to the network and relay to share.';
}
async function api(path, options = {}) {
  const controller = new AbortController();
  const deadline = setTimeout(() => controller.abort(), 5000);
  try {
    const response = await fetch(path, {...options, signal:controller.signal, headers: {'X-OST-Owner': secret, 'Content-Type':'application/json'}});
    const data = await response.json();
    if (!response.ok) {
      if (response.status === 503) sharingAvailability(false);
      throw new Error(data.detail || 'Request failed');
    }
    return data;
  } finally { clearTimeout(deadline); }

}
function theme(value) {
  document.documentElement.dataset.theme = value;
  $('theme').textContent = value === 'dark' ? 'Light mode' : 'Dark mode';
  localStorage.setItem('ost-relay-theme', value);
}
theme(localStorage.getItem('ost-relay-theme') || 'dark');
$('theme').onclick = () => theme(document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark');
const names = {pending:'Waiting for relay', ready:'Ready to share', revoking:'Confirming revocation', revoked:'Revoked', expired:'Expired', error:'Needs attention', offline:'Waiting for connection'};
function render(data) {
  lastConnectionCheck = Date.now();
  sharingAvailability(data.can_share === true);
  $('connection').textContent = data.connected ? 'Timer connected to relay' : 'Connect the timer to the network and relay';
  const list = $('links'); list.replaceChildren();
  if (!data.links.length) {list.textContent = 'No links yet.'; return;}
  for (const link of [...data.links].reverse()) {
    const card = document.createElement('article'); card.className = 'link-card'; card.dataset.state = link.state;
    const header = document.createElement('header');
    const title = document.createElement('strong'); title.textContent = link.label || 'Partner';
    const status = document.createElement('span'); status.className = 'link-state'; status.textContent = names[link.state] || link.state;
    header.append(title, status); card.append(header);
    const note = document.createElement('p'); note.className = 'hint';
    note.textContent = link.error || (link.remaining_seconds ? `Expires in ${Math.ceil(link.remaining_seconds / 60)} min` : 'Access has ended'); card.append(note);
    const actions = document.createElement('div'); actions.className = 'actions';
    if (canShare && link.state === 'ready' && link.url) {
      const url = document.createElement('input'); url.readOnly = true; url.value = link.url; url.setAttribute('aria-label', 'Partner share link'); card.append(url);
      const copy = document.createElement('button'); copy.className = 'btn primary'; copy.textContent = 'Copy link';
      copy.onclick = async () => {try {await navigator.clipboard.writeText(link.url); $('message').textContent = 'Link copied. Share it with your partner.';} catch {url.select(); $('message').textContent = 'Select and copy the link above.';}};
      actions.append(copy);
    }
    if (!['revoked','expired','revoking'].includes(link.state)) {
      const revoke = document.createElement('button'); revoke.className = 'btn secondary'; revoke.textContent = 'Revoke';
      revoke.onclick = async () => {revoke.disabled = true; try {await api('/api/links/' + encodeURIComponent(link.fp_hash), {method:'DELETE'}); await refresh();} catch(e) {$('message').textContent = e.message; revoke.disabled = false;}};
      actions.append(revoke);
    }
    card.append(actions); list.append(card);
  }
}
async function refresh() {
  try {render(await api('/api/links'));} catch(e) {sharingAvailability(false); $('connection').textContent = 'Creator connection unavailable'; $('message').textContent = e.message; $('links').replaceChildren();}
}
$('createForm').addEventListener('submit', async event => {
  event.preventDefault(); if (busy || !canShare) return;
  const ttl = Number($('lifetime').value) * 60, label = $('label').value;
  if (!Number.isInteger(ttl) || ttl < 60 || ttl > 86400) return;
  // Keep the request identity after an uncertain HTTP response; retrying the
  // same form never creates a second link or resets the original expiry.
  if (!pendingRequest || pendingRequest.ttl_seconds !== ttl || pendingRequest.label !== label)
    pendingRequest = {ttl_seconds:ttl, label, request_id:crypto.randomUUID()};
  sessionStorage.setItem('ost-owner-pending', JSON.stringify(pendingRequest));
  busy = true; $('generate').disabled = true;
  try {
    await api('/api/links', {method:'POST', body:JSON.stringify(pendingRequest)});
    pendingRequest = null; sessionStorage.removeItem('ost-owner-pending'); $('message').textContent = 'Link created. Waiting for relay confirmation…';
    await refresh();
  } catch(e) {$('message').textContent = e.message;}
  finally {busy = false; sharingAvailability(canShare);}
});
setInterval(() => {
  if (Date.now() - lastConnectionCheck > 5000) {
    sharingAvailability(false);
    $('links').replaceChildren();
  }
}, 1000);
async function poll() {await refresh(); setTimeout(poll, 1500);}
poll();
