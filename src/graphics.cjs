function graphicsState(features = {}, platform = process.platform) {
  const webgl2 = features.webgl2 || 'unknown';
  const webgl = features.webgl || 'unknown';
  const compositing = features.gpu_compositing || 'unknown';
  const softwareRendering = [webgl, webgl2, compositing].some(value => /software/.test(value));
  const unavailable = [webgl, webgl2].some(value => /^(disabled|unavailable)_off/.test(value));
  const advice = platform === 'win32' ? 'Update your graphics driver and check Windows Graphics settings for the client’s runtime/electron.exe.' : 'Check your distribution’s graphics drivers and GPU configuration for Qrazy.';
  return { highPerformanceRequested: true, webgl, webgl2, compositing, softwareRendering,
    warning: softwareRendering ? `Qrazy is using software graphics rendering. Performance may be reduced. ${advice}` :
      unavailable ? `Hardware WebGL graphics are unavailable. ${advice}` : null };
}
module.exports = { graphicsState };
