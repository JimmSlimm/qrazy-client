const fs = require('node:fs/promises');
const path = require('node:path');
const { spawn } = require('node:child_process');
const quote = value => "'" + value.replace(/'/g, "'\\''") + "'";
function linuxScript(plan) {
  if (plan.appImage) return appImageScript(plan);
  const checks = plan.files.map(file => `test "$(sha256sum -- ${quote(path.posix.join(plan.stage, file.path))} | cut -d ' ' -f 1)" = ${quote(file.sha256)} || fail 'Staged update failed verification'`).join('\n');
  return `#!/bin/sh
set -eu
root=${quote(plan.root)}
stage=${quote(plan.stage)}
backup=${quote(plan.backup)}
result=${quote(plan.result)}
moved=0
installed=0
report() { printf '%s\\n' "$1" > "$result"; }
fail() {
  if [ "$moved" = 1 ] && [ "$installed" = 0 ]; then
    if ! mv -- "$backup" "$root"; then report '{"phase":"error","message":"Update recovery needs manual attention. The previous folder is retained beside the installation."}'; exit 1; fi
  fi
  report '{"phase":"error","message":"Update failed. The previous client and your profile have been kept."}'
  if [ "$installed" = 0 ]; then "$root/Qrazy" >/dev/null 2>&1 & fi
  exit 1
}

count=0
while kill -0 ${plan.pid} 2>/dev/null; do
  count=$((count + 1)); [ "$count" -lt 120 ] || fail 'Client did not close'; sleep 1
done
command -v sha256sum >/dev/null || fail 'sha256sum is unavailable'
[ ! -e "$backup" ] || fail 'Recovery folder exists'
[ -d "$root" ] && [ -d "$stage" ] || fail 'Update folder missing'
[ -z "$(find "$stage" -type l -print -quit)" ] || fail 'Staged links are unsafe'
${checks}
mv -- "$root" "$backup" || fail 'Could not retain old client'
moved=1
mv -- "$stage" "$root" || fail 'Could not install update'
installed=1
report '{"phase":"installed","message":"Client updated. The previous folder is retained for recovery."}'
"$root/Qrazy" >/dev/null 2>&1 &
`;
}
function appImageScript(plan) {
  if (plan.files.length !== 1 || plan.files[0].path !== 'Qrazy-linux-x64.AppImage') throw new Error('Invalid AppImage install inventory');
  return `#!/bin/sh
set -eu
image=${quote(plan.appImage)}
stage=${quote(plan.stage)}
next=${quote(path.posix.join(plan.stage, plan.files[0].path))}
backup=${quote(plan.backup)}
result=${quote(plan.result)}
moved=0
report() { printf '%s\\n' "$1" > "$result"; }
fail() {
  if [ "$moved" = 1 ]; then
    if ! mv -- "$backup" "$image"; then report '{"phase":"error","message":"AppImage recovery needs manual attention. The previous image is retained beside the download."}'; exit 1; fi
  fi
  report '{"phase":"error","message":"AppImage update failed. Your previous client and profile have been kept."}'
  "$image" >/dev/null 2>&1 &
  exit 1
}
count=0
while kill -0 ${plan.pid} 2>/dev/null; do
  count=$((count + 1)); [ "$count" -lt 120 ] || fail; sleep 1
done
command -v sha256sum >/dev/null || fail
[ -f "$image" ] && [ ! -L "$image" ] && [ -f "$next" ] && [ ! -L "$next" ] || fail
[ ! -e "$backup" ] && [ ! -L "$backup" ] || fail
[ -z "$(find "$stage" -type l -print -quit)" ] || fail
[ "$(sha256sum -- "$next" | cut -d ' ' -f 1)" = ${quote(plan.files[0].sha256)} ] || fail
chmod 755 -- "$next" || fail
mv -- "$image" "$backup" || fail
moved=1
mv -- "$next" "$image" || fail
moved=0
rmdir -- "$stage" || true
report '{"phase":"installed","message":"Client updated. The previous AppImage is retained beside it for recovery."}'
unset APPIMAGE APPDIR ARGV0 OWD
"$image" >/dev/null 2>&1 &
`;
}
async function startWindowsHelper(executable, args, directory, timeout = 15000) {
  // PowerShell needs its own console; DETACHED_PROCESS exits silently on Windows.
  // Start-Process creates that console hidden and lets the helper outlive Electron.
  const token = require('node:crypto').randomUUID();
  const outputPath = path.join(directory, 'helper-' + token + '.out');
  const errorPath = path.join(directory, 'helper-' + token + '.err');
  const psQuote = value => "'" + value.replace(/'/g, "''") + "'";
  const command = ` $ErrorActionPreference='Stop'; Start-Process -FilePath ${psQuote(executable)} -ArgumentList ${psQuote(args.map(arg => '"' + arg + '"').join(' '))} -WorkingDirectory ${psQuote(directory)} -WindowStyle Hidden -RedirectStandardOutput ${psQuote(outputPath)} -RedirectStandardError ${psQuote(errorPath)} | Out-Null`;
  const child = spawn(executable, ['-NoProfile', '-NonInteractive', '-EncodedCommand', Buffer.from(command, 'utf16le').toString('base64')], {
    windowsHide: true, stdio: ['ignore', 'ignore', 'pipe'], cwd: directory,
    env: { ...process.env, PSModulePath: path.join(process.env.SystemRoot || 'C:\\Windows', 'System32/WindowsPowerShell/v1.0/Modules') }
  });
  await new Promise((resolve, reject) => {
    let errors = '';
    child.stderr.on('data', bytes => { errors = (errors + bytes.toString()).slice(-2000); });
    child.once('error', reject);
    child.once('exit', code => code === 0 ? resolve() : reject(new Error('Update helper could not start: ' + errors.trim())));
  });
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    const output = await fs.readFile(outputPath, 'utf8').catch(() => '');
    const errors = await fs.readFile(errorPath, 'utf8').catch(() => '');
    if (/(^|[\r\n])READY[\r\n]/.test(output)) return child;
    if (output.includes('ERROR:') || errors.trim()) throw new Error((errors.trim() || output.slice(output.indexOf('ERROR:') + 6).trim()).slice(0, 1000));
    await new Promise(resolve => setTimeout(resolve, 100));
  }
  throw new Error('Update helper did not become ready. Your client is still open.');
}
async function startInstaller(updater) {
  const { plan, planPath } = await updater.prepareInstall(process.pid, process.ppid);
  let executable, args;
  if (process.platform === 'win32') {
    if (updater.allUsers) {
      const broker = planPath + '.broker.ps1';
      await fs.copyFile(path.join(__dirname, 'admin-update-broker.ps1'), broker);
      const executable = path.join(process.env.SystemRoot || 'C:\\Windows', 'System32/WindowsPowerShell/v1.0/powershell.exe');
      const child = await startWindowsHelper(executable, ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', broker, '-PlanPath', planPath, '-Executable', path.join(updater.root, 'runtime/electron.exe')], updater.directory, 180000);
      child.unref(); return;
    }
    const script = planPath + '.ps1';
    await fs.copyFile(path.join(__dirname, 'update-helper.ps1'), script);
    executable = path.join(process.env.SystemRoot || 'C:\\Windows', 'System32/WindowsPowerShell/v1.0/powershell.exe');
    args = ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', script, '-PlanPath', planPath];
  } else {
    const script = planPath + '.sh';
    await fs.writeFile(script, linuxScript(plan), { mode: 0o700 });
    executable = '/bin/sh'; args = [script];
  }
  if (process.platform === 'win32') {
    const child = await startWindowsHelper(executable, args, updater.directory);
    child.unref(); return;
  }
  const helperEnv = { ...process.env };
  if (plan.appImage) for (const name of ['APPIMAGE', 'APPDIR', 'ARGV0', 'OWD']) delete helperEnv[name];
  const child = spawn(executable, args, { detached: true, stdio: 'ignore', windowsHide: true, cwd: updater.directory, env: helperEnv });
  await new Promise((resolve, reject) => { child.once('spawn', resolve); child.once('error', reject); });
  child.unref();
}
module.exports = { startInstaller, startWindowsHelper, linuxScript, appImageScript };

