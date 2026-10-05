const { ipcRenderer } = require('electron');
window.addEventListener('DOMContentLoaded', () => {
  document.getElementById('retry').addEventListener('click', () => ipcRenderer.send('client:retry'));
  document.getElementById('copy').addEventListener('click', () => ipcRenderer.send('client:copy-error'));
});
ipcRenderer.on('client:status', (_event, state) => {
  document.getElementById('message').textContent = state.message;
  document.getElementById('retry').hidden = !state.retry;
  document.getElementById('retry').textContent = state.retryLabel || 'Retry';
  document.getElementById('copy').hidden = !state.details;
  document.getElementById('diagnostic').textContent = state.diagnostic || '';
  document.getElementById('progress').textContent = state.progress || '';
});
