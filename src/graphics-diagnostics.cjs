const path = require('node:path');
const { execFile } = require('node:child_process');

function windowsPreference(executable, platform = process.platform) {
  if (platform !== 'win32') return Promise.resolve('Not applicable');
  return new Promise(resolve => {
    try {
    execFile(path.join(process.env.SystemRoot || 'C:\\Windows', 'System32', 'reg.exe'),
      ['query', 'HKCU\\Software\\Microsoft\\DirectX\\UserGpuPreferences', '/v', executable, '/reg:64'],
      { windowsHide: true, timeout: 3000, maxBuffer: 65536 }, (error, stdout) => {
        const value = stdout?.match(/REG_SZ\s+([^\r\n]*)/)?.[1];
        if (value !== undefined) {
          const preference = value.match(/(?:^|;)GpuPreference=(\d+)(?:;|$)/)?.[1];
          resolve(`${({ 0: 'Windows decides', 1: 'Power saving', 2: 'High performance' })[preference] || 'Unspecified/unknown'} (${value})`);
        } else resolve(error ? 'No readable entry (missing or query failed)' : 'No GPU preference');
      });
    } catch { resolve('Windows preference query could not start'); }
  });
}

async function graphicsReport(app, webglRenderer) {
  const executable = process.execPath;
  const launcher = process.platform === 'win32' ? path.join(path.dirname(path.dirname(executable)), 'Qrazy.exe') : null;
  const [runtimePreference, launcherPreference, gpu] = await Promise.all([
    windowsPreference(executable), launcher ? windowsPreference(launcher) : Promise.resolve('Not applicable'),
    app.getGPUInfo('complete').catch(() => null)
  ]);
  return [
    'Qrazy graphics diagnostic',
    `Client version: ${app.getVersion()}`,
    `Electron / Chromium: ${process.versions.electron} / ${process.versions.chrome}`,
    `Platform: ${process.platform} ${process.arch}`,
    `Runtime executable: ${executable}`,
    `Runtime Windows preference: ${runtimePreference}`,
    `Launcher executable: ${launcher || 'Not applicable'}`,
    `Launcher Windows preference: ${launcherPreference}`,
    `Launcher route marker: ${process.env.QRAZY_CLIENT_RUNTIME || 'Not present'}`,
    `NVIDIA launch hint: ${process.env.SHIM_MCCOMPAT || 'Not present'}`,
    `Active GPU(s): ${gpu?.gpuDevice?.filter(device => device.active).map(device => device.deviceString || `${device.vendorId}:${device.deviceId}`).join(', ') || 'Unavailable'}`,
    `Chromium renderer: ${gpu?.auxAttributes?.glRenderer || 'Unavailable'}`,
    `WebGL probe renderer: ${typeof webglRenderer === 'string' ? webglRenderer.slice(0, 1024) : 'Unavailable'}`,
    `GPU features: ${JSON.stringify(app.getGPUFeatureStatus())}`,
    'WebGL probe uses a separate context; this is not an FPS measurement.',
    'Local report only. Executable paths may contain your Windows username.'
  ].join('\n');
}
module.exports = { windowsPreference, graphicsReport };
