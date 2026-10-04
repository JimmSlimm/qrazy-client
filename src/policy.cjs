// One build-time choice; never fall back to localhost after a live-site failure.
const { DEV_MODE } = require('./client-mode.cjs');
const GAME_URL = DEV_MODE ? 'http://localhost:5173/' : 'https://qrazy-game.onrender.com/';
function isGameURL(value) {
  try { const url = new URL(value); return url.origin === new URL(GAME_URL).origin && !url.username && !url.password; }
  catch { return false; }
}
function permissionAllowed(permission, url) {
  return isGameURL(url) && ['pointerLock', 'fullscreen', 'keyboardLock'].includes(permission);
}
function isFullscreenShortcut(input) {
  return input.key === 'Enter' && input.alt === true && !input.control && !input.meta && !input.shift;
}
module.exports = { DEV_MODE, GAME_URL, isGameURL, permissionAllowed, isFullscreenShortcut };
