const WAKE_MESSAGE = 'The Qrazy server is waking up (it sleeps when nobody has played for a while). This usually takes under a minute. Retrying automatically…';
const EXPIRED_MESSAGE = "Still can't reach the server. Check your internet connection or ask in the Qrazy Discord.";
const RETRY_WINDOW_MS = 300000;
const INITIAL_RETRY_INTERVAL_MS = 5000;
const BACKOFF_AFTER_MS = 60000;
const BACKOFF_RETRY_INTERVAL_MS = 10000;
function retryableConnection(description = '') {
  return /ERR_CONNECTION_|ERR_TIMED_OUT|ERR_CONNECTION_TIMED_OUT|ERR_NAME_NOT_RESOLVED|ERR_INTERNET_DISCONNECTED|ERR_NETWORK_CHANGED/.test(description);
}
class ServerRetry {
  constructor({ retry, show, expire = () => {}, now = Date.now, interval = setInterval, cancel = clearInterval }) {
    Object.assign(this, { retry, show, expire, now, interval, cancel });
    this.timer = null; this.deadline = 0; this.count = 0;
  }
  failure(diagnostic) {
    this.diagnostic = diagnostic;
    if (!this.deadline) {
      this.deadline = this.now() + RETRY_WINDOW_MS;
      this.count = 1;
      this.timer = this.interval(() => this.tick(), 1000);
    }
    const elapsed = this.now() - (this.deadline - RETRY_WINDOW_MS);
    this.next = this.now() + (elapsed < BACKOFF_AFTER_MS ? INITIAL_RETRY_INTERVAL_MS : BACKOFF_RETRY_INTERVAL_MS);
    this.pending = false;
    this.tick();
  }
  tick() {
    if (this.now() >= this.deadline) {
      this.cancel(this.timer); this.timer = null;
      this.expire();
      this.show(EXPIRED_MESSAGE, true, '', 'Retry now', this.diagnostic);
      return;
    }
    if (!this.pending && this.now() >= this.next) this.run();
    this.show(WAKE_MESSAGE, true, '', 'Retry now', this.diagnostic,
      `Attempt ${this.count} · ${this.pending ? 'Connecting…' : `Retrying in ${Math.ceil((this.next - this.now()) / 1000)} s`}`);
  }
  run() { this.pending = true; ++this.count; this.retry(); }
  stop() {
    if (this.timer !== null) this.cancel(this.timer);
    this.timer = null; this.deadline = 0; this.pending = false;
  }
}
module.exports = { ServerRetry, retryableConnection };
