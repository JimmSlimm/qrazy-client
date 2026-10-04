// Select the destination before building. Double-clicking Qrazy.exe uses this setting.
// 'live' = https://qrazy-game.onrender.com/; 'dev' = http://localhost:5173/
const CLIENT_MODE = 'live';
if (!['live', 'dev'].includes(CLIENT_MODE)) throw new Error('CLIENT_MODE must be live or dev');
module.exports = { DEV_MODE: CLIENT_MODE === 'dev' };
