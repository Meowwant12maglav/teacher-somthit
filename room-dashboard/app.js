const ROOM_CAPACITY = 20;
const state = { occupancy: 0, entries: 0, exits: 0, events: [], endpoint: localStorage.getItem('esp32Endpoint') || '', polling: null };

const el = (id) => document.getElementById(id);
const ui = { count: el('occupancyCount'), entries: el('entriesCount'), exits: el('exitsCount'), events: el('eventsCount'), fill: el('capacityFill'), percent: el('capacityPercent'), updated: el('updatedAt'), timeline: el('timeline'), callout: el('eventCallout'), door: el('doorVisual'), status: el('connectionStatus'), url: el('esp32Url'), message: el('formMessage') };

function timeNow() { return new Intl.DateTimeFormat([], { hour: '2-digit', minute: '2-digit' }).format(new Date()); }
function updateUi() {
  ui.count.textContent = state.occupancy;
  ui.entries.textContent = state.entries;
  ui.exits.textContent = state.exits;
  ui.events.textContent = state.entries + state.exits;
  const percent = Math.min(100, Math.round((state.occupancy / ROOM_CAPACITY) * 100));
  ui.fill.style.width = `${percent}%`;
  ui.percent.textContent = `${percent}%`;
}
function renderTimeline() {
  if (!state.events.length) { ui.timeline.innerHTML = '<li class="empty-state">No entries yet. The first sensor event will appear here.</li>'; return; }
  ui.timeline.innerHTML = state.events.slice(0, 6).map(event => `<li><span class="timeline-mark ${event.type}">${event.type === 'entry' ? '→' : '←'}</span><span><strong>${event.type === 'entry' ? 'Entered Room 1' : 'Exited Room 1'}</strong><br><small>${event.source}</small></span><time class="timeline-time">${event.time}</time></li>`).join('');
}
function addEvent(type, source = 'Two sensors triggered') {
  if (type === 'entry') { state.occupancy++; state.entries++; }
  if (type === 'exit') { state.occupancy = Math.max(0, state.occupancy - 1); state.exits++; }
  state.events.unshift({ type, source, time: timeNow() });
  ui.updated.textContent = `Last activity ${timeNow()}`;
  ui.door.className = `door-visual ${type}`;
  ui.callout.className = `event-callout ${type}`;
  ui.callout.innerHTML = type === 'entry' ? '<span class="event-icon">→</span><div><strong>Someone entered Room 1</strong><p>Detected from left to right.</p></div>' : '<span class="event-icon">←</span><div><strong>Someone exited Room 1</strong><p>Detected from right to left.</p></div>';
  updateUi(); renderTimeline();
  window.setTimeout(() => { ui.door.className = 'door-visual'; }, 1200);
}
function setConnection(connected, text) {
  ui.status.classList.toggle('connected', connected);
  ui.status.lastElementChild.textContent = text;
}
async function pollEsp32() {
  if (!state.endpoint) return;
  try {
    const response = await fetch(`${state.endpoint}/api/status`, { cache: 'no-store' });
    if (!response.ok) throw new Error('ESP32 did not respond');
    const data = await response.json();
    setConnection(true, 'ESP32 connected');
    // Expected event values: "entry" (left → right) or "exit" (right → left).
    if (data.event === 'entry' || data.event === 'exit') addEvent(data.event, 'ESP32 sensor event');
    if (Number.isInteger(data.occupancy)) { state.occupancy = Math.max(0, data.occupancy); updateUi(); }
  } catch (error) { setConnection(false, 'ESP32 unreachable'); ui.message.textContent = 'Cannot reach the ESP32 yet. Check its IP address and Wi-Fi connection.'; }
}
function connect(url) {
  state.endpoint = url.replace(/\/$/, '');
  localStorage.setItem('esp32Endpoint', state.endpoint);
  clearInterval(state.polling);
  if (!state.endpoint) { setConnection(false, 'Demo mode'); ui.message.textContent = 'Leave blank to stay in demo mode.'; return; }
  ui.message.textContent = 'Connecting…'; pollEsp32(); state.polling = setInterval(pollEsp32, 1000);
}

el('connectionForm').addEventListener('submit', (event) => { event.preventDefault(); connect(ui.url.value.trim()); });
el('resetButton').addEventListener('click', () => { state.occupancy = 0; state.entries = 0; state.exits = 0; state.events = []; ui.updated.textContent = 'Today’s count was reset'; ui.callout.className = 'event-callout neutral'; ui.callout.innerHTML = '<span class="event-icon">↔</span><div><strong>Ready to detect</strong><p>Waiting for a two-sensor sequence.</p></div>'; updateUi(); renderTimeline(); });
el('demoButton').addEventListener('click', () => { addEvent('entry', 'Demo event'); window.setTimeout(() => addEvent('exit', 'Demo event'), 1500); });
ui.url.value = state.endpoint;
updateUi(); renderTimeline();
if (state.endpoint) connect(state.endpoint);
