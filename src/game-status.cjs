function validateGameStatus(value) {
  if (!value || typeof value !== 'object' || !['loading','ready','warning','error'].includes(value.phase)) throw new TypeError('Invalid game status phase');
  const text = (name, limit, optional = false) => {
    const v = value[name]; if (optional && v == null) return '';
    if (typeof v !== 'string' || !v.trim() || v.length > limit || /[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/.test(v)) throw new TypeError(`Invalid game status ${name}`);
    return v.trim();
  };
  const stage = value.stage || 'game';
  if (!['server','map','assets','graphics','game'].includes(stage)) throw new TypeError('Invalid game status stage');
  const recovery = value.recovery || 'none';
  if (!['none','reload'].includes(recovery) || (recovery === 'reload' && value.phase !== 'error')) throw new TypeError('Invalid game recovery');
  return {phase:value.phase,stage,message:value.phase==='ready'?'':text('message',500),code:text('code',80,true),details:text('details',2000,true),recovery};
}
module.exports = { validateGameStatus };
