const fs = require('node:fs');
const path = require('node:path');
const DEFAULT_MAX_FPS = 250;
const MIN_MAX_FPS = 30;
function validateMaxFps(value) {
  if (!Number.isSafeInteger(value) || value < MIN_MAX_FPS)
    throw new TypeError('com_maxfps must be a whole number of at least 30');
  return value;
}
class MaxFpsPreference {
  constructor(directory) {
    this.file = path.join(directory, 'max-fps.json');
    this.value = DEFAULT_MAX_FPS;
    try {
      const data = JSON.parse(fs.readFileSync(this.file, 'utf8'));
      try { this.value = validateMaxFps(data?.com_maxfps); } catch {}
    } catch (error) {
      if (error.code !== 'ENOENT' && !(error instanceof SyntaxError)) throw error;
    }
  }
  getState() { return { value: this.value, minimum: MIN_MAX_FPS, defaultValue: DEFAULT_MAX_FPS }; }
  setValue(value) {
    validateMaxFps(value);
    fs.mkdirSync(path.dirname(this.file), { recursive: true });
    const temporary = this.file + '.tmp';
    fs.writeFileSync(temporary, JSON.stringify({ com_maxfps: value }) + '\n', { encoding: 'utf8', mode: 0o600 });
    fs.renameSync(temporary, this.file);
    this.value = value;
    return this.getState();
  }
}
module.exports = { MaxFpsPreference, validateMaxFps };
