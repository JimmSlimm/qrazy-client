const fs = require('node:fs');
const path = require('node:path');
const data = require('./changelog.json');
const categories = ['added', 'changed', 'fixed'];
function releaseNotes(entry) {
  return categories.filter(key => entry?.[key]?.length).map(key =>
    `${key[0].toUpperCase() + key.slice(1)}\n${entry[key].map(text => `• ${text}`).join('\n')}`).join('\n\n');
}
class Changelog {
  constructor(directory, installedVersion) {
    this.file = path.join(directory, 'changelog-read.json');
    this.installedVersion = installedVersion;
    this.readVersion = null;
    try { this.readVersion = JSON.parse(fs.readFileSync(this.file, 'utf8')).version; } catch {}
  }
  getState() {
    const installed = data.releases.find(entry => entry.version === this.installedVersion);
    return { ...data, installedVersion: this.installedVersion,
      unread: !!installed && this.readVersion !== this.installedVersion };
  }
  markRead() {
    fs.mkdirSync(path.dirname(this.file), { recursive: true });
    const temporary = this.file + '.tmp';
    fs.writeFileSync(temporary, JSON.stringify({ version: this.installedVersion }));
    fs.renameSync(temporary, this.file);
    this.readVersion = this.installedVersion;
    return this.getState();
  }
}
module.exports = { Changelog, releaseNotes };
