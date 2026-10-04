function validCommit(value) { return typeof value === 'string' && /^[a-f0-9]{7,40}$/.test(value); }
class WebsiteUpdates {
  constructor({ url, fetch, notify }) { Object.assign(this, { url, fetch, notify }); this.loaded = null; this.outdated = false; this.busy = false; }
  getState() { return { outdated: this.outdated }; }
  reset() { this.loaded = null; this.outdated = false; this.notify(this.getState()); }
  async reportLoadedBuild(commit) {
    if (!validCommit(commit)) return this.getState();
    // Pin what the page actually loaded, never the latest server response.
    if (!this.loaded) this.loaded = commit;
    await this.check(); return this.getState();
  }
  async check() {
    if (!this.loaded || this.busy || this.outdated) return this.getState();
    this.busy = true; const loaded = this.loaded;
    try {
      const response = await this.fetch(new URL('/build.json', this.url).href, { cache: 'no-store', credentials: 'omit', signal: AbortSignal.timeout(10000) });
      if (!response.ok) return this.getState();
      const text = await require('./updater.cjs').readBounded(response, 4096);
      const build = JSON.parse(text);
      if (this.loaded === loaded && validCommit(build.commit) && build.commit !== loaded) {
        this.outdated = true; this.notify(this.getState());
      }
    } catch { /* Offline checks never interrupt gameplay or invent a deployment. */ }
    finally { this.busy = false; }
    return this.getState();
  }
}
module.exports = { WebsiteUpdates, validCommit };
