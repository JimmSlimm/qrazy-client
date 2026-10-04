class VSyncPreference {
  constructor(_directory, commandLine) {
    // Stock synchronized rendering is the only supported path. Ignore legacy
    // saved OFF preferences and remove unlocking flags before GPU startup.
    commandLine.removeSwitch('disable-gpu-vsync');
    commandLine.removeSwitch('disable-frame-rate-limit');
  }
  getState() {
    return { enabled: true, activeEnabled: true, restartRequired: false, canDisable: false };
  }
  setEnabled(enabled) {
    if (typeof enabled !== 'boolean') throw new TypeError('VSync preference must be a boolean');
    // Keep older game pages compatible: a request to turn it OFF reports the
    // actual locked-ON state. No preference writes or restart flow remain.
    return this.getState();
  }
}
module.exports = { VSyncPreference };
