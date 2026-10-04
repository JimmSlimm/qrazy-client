const fs = require('node:fs/promises');
const path = require('node:path');
const { spawn } = require('node:child_process');
const quote = value => "'" + value.replace(/'/g, "'\\''") + "'";
function linuxScript(plan) {
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
async function startInstaller(updater) {
  const { plan, planPath } = await updater.prepareInstall(process.pid, process.ppid);
  let executable, args;
  if (process.platform === 'win32') {
    if (updater.allUsers) {
      const broker = planPath + '.broker.ps1';
      await fs.copyFile(path.join(__dirname, 'admin-update-broker.ps1'), broker);
      const executable = path.join(process.env.SystemRoot || 'C:\\Windows', 'System32/WindowsPowerShell/v1.0/powershell.exe');
      const child = spawn(executable, ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', broker, '-PlanPath', planPath, '-Executable', path.join(updater.root, 'runtime/electron.exe')], { detached: true, stdio: ['ignore', 'pipe', 'ignore'], windowsHide: true, cwd: updater.directory });
      await new Promise((resolve, reject) => {
        let output = '', settled = false;
        const finish = error => { if (settled) return; settled = true; error ? reject(error) : resolve(); };
        child.once('error', finish);
        child.stdout.on('data', bytes => {
          output += bytes.toString();
          if (output.includes('READY\n') || output.includes('READY\r\n')) finish();
          else if (output.includes('ERROR:')) finish(new Error(output.slice(output.indexOf('ERROR:') + 6).trim().slice(0, 1000)));
        });
        child.once('exit', code => { if (!settled) finish(new Error('Update approval did not complete. Your client is still open.')); });
      });
      child.stdout.destroy(); child.unref(); return;
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
  const child = spawn(executable, args, { detached: true, stdio: 'ignore', windowsHide: true, cwd: updater.directory });
  await new Promise((resolve, reject) => { child.once('spawn', resolve); child.once('error', reject); });
  child.unref();
}
module.exports = { startInstaller, linuxScript };
