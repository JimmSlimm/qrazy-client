// Update archives are physical files. Electron's normal fs interprets .asar
// paths as virtual directories and cannot reliably stream the archive itself.
let physical;
try { physical = require('original-fs'); }
catch (error) { if (error.code !== 'MODULE_NOT_FOUND') throw error; physical = require('node:fs'); }
module.exports = physical;
