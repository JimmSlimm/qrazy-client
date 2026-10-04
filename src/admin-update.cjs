// Runs only inside the installed application after explicit UAC approval.
// Never execute a script or trust file hashes supplied by a user's update plan.
const fs = require('./update-files.cjs').promises;
const path = require('node:path');
const { verifyManifest, newer, noLinks, hashFile } = require('./updater.cjs');
const config = require('./update-config.cjs');
const { allUsersInstallation } = require('./installation.cjs');
async function prepareAdminUpdate(planPath, root, version, options = {}) {
  const registered = options.registered || allUsersInstallation;
  if (!registered(root)) throw new Error('This is not the registered Everyone installation');
  await noLinks(root);
  if ((await fs.stat(planPath)).size > 4 * 1024 * 1024) throw new Error('Update request is too large');
  const request = JSON.parse(await fs.readFile(planPath, 'utf8'));
  if (path.resolve(request.root).toLowerCase() !== path.resolve(root).toLowerCase() || !/^[a-f0-9]{24}$/.test(request.jobToken)) throw new Error('Wrong installation or job identity');
  for (const field of ['pid', 'parentPid']) if (!Number.isSafeInteger(request[field]) || request[field] < 1 || request[field] === process.pid) throw new Error('Invalid client process');
  const release = verifyManifest(request.envelope, options.config || config, 'win32-x64');
  if (!newer(release.version, version)) throw new Error('The signed update is not newer');
  const source = path.resolve(request.stage);
  await noLinks(source);
  const parent = path.dirname(path.resolve(root));
  if (parent === root) throw new Error('Unsafe installation root');
  const disk = await fs.statfs(parent);
  if (disk.bavail * disk.bsize < release.files.reduce((sum, file) => sum + file.size, 0) + 1024 * 1024 * 1024) throw new Error('Not enough free space on the installation drive');
  const job = path.join(parent, '.qrazy-job-' + request.jobToken);
  // These directories inherit the installation parent's permissions. Refuse
  // existing names; ordinary users must never supply our privileged script/plan.
  await fs.mkdir(job);
  const stage = path.join(parent, '.qrazy-update-' + request.jobToken);
  await fs.mkdir(stage);
  for (const file of release.files) {
    const from = await noLinks(source, file.path);
    const target = path.join(stage, ...file.path.split('/'));
    if (!(await fs.stat(from)).isFile() || (await fs.stat(from)).size !== file.size) throw new Error('Staged file has the wrong size');
    await fs.mkdir(path.dirname(target), { recursive: true });
    await fs.copyFile(from, target, require('node:fs').constants.COPYFILE_EXCL);
    if ((await fs.stat(target)).size !== file.size || await hashFile(target) !== file.sha256) throw new Error('Staged file does not match the signed release');
  }
  const plan = { root: path.resolve(root), stage, backup: path.join(parent, '.qrazy-previous-' + request.jobToken),
    version: release.version, pid: request.pid, parentPid: request.parentPid, workerPid: process.pid,
    files: release.files.map(({ path, sha256 }) => ({ path, sha256 })), result: path.join(job, 'result.json'), allUsers: true, noRestart: true, helperReady: true };
  const protectedPlan = path.join(job, 'plan.json');
  const helper = path.join(job, 'helper.ps1');
  await fs.writeFile(protectedPlan, JSON.stringify(plan), { flag: 'wx' });
  await fs.writeFile(helper, await require('node:fs/promises').readFile(path.join(__dirname, 'update-helper.ps1')), { flag: 'wx' });
  return { plan, protectedPlan, helper };
}
async function runAdminUpdate(planPath, root, version) {
  const prepared = await prepareAdminUpdate(planPath, root, version);
  const executable = path.join(process.env.SystemRoot || 'C:\\Windows', 'System32/WindowsPowerShell/v1.0/powershell.exe');
  const child = await require('./update-install.cjs').startWindowsHelper(executable, ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', prepared.helper, '-PlanPath', prepared.protectedPlan], path.dirname(prepared.helper));
  child.unref();
}
module.exports = { prepareAdminUpdate, runAdminUpdate };
