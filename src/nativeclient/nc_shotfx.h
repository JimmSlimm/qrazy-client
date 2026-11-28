// Weapon effects of one shot event, for the player's own shots (the sim's events) and other players' (nc_match.h): the core decides which beam, marks and sparks it
// leaves (qc_ses_fx: the marks setting, the surfaces, the runway walls, the traces); the client draws the commands. The blast itself is the client's (World::fxBlast).
#pragma once
#include "nc_world.h"
#include "qrazy_core_api.h"

namespace nc {

// kind QC_SESFX_*; mode 0 PQL / 1 VQL; from / pos / normal may be null. Draws the core's commands in order. No allocation.
inline void fxShot(World& w, int kind, int weapon, int mode, bool runway, const double* from, const double* pos, const double* normal, int surface = 0, double fraction = 0) {
  QcFxIn in{}; in.kind = kind; in.weapon = weapon; in.mode = mode; in.flags = (normal ? QC_SESFX_HAS_NORMAL : 0) | (runway ? QC_SESFX_RUNWAY : 0); in.surface = surface; in.fraction = fraction;
  for (int k = 0; k < 3; ++k) { in.from[k] = from ? from[k] : 0; in.pos[k] = pos ? pos[k] : 0; in.normal[k] = normal ? normal[k] : 0; }
  QcFxCmd cmd[QC_FXCMD_MAX]; const int n = qc_ses_fx(&in, cmd, QC_FXCMD_MAX);
  for (int i = 0; i < n; ++i) { const QcFxCmd& c = cmd[i];
    if (c.kind == QC_FXCMD_BEAM) w.fxBeam(c.pos, c.normal);
    else if (c.kind == QC_FXCMD_MARK) w.fxMark(c.style, c.pos, c.normal, c.radius, runway);
    else if (c.kind == QC_FXCMD_SPARKS) w.fxSparks(c.pos, c.normal);
  }
}

// The blast's splash radius as the core sizes it (qc_asm_fx_pose: a blast's scale at the end of its life is 0.6 of the radius; the plasma splash ignores the radius).
inline double blastSplash(int weapon, int mode) {
  QcAsmFxIn in{}; in.kind = QC_FX_BLAST; in.weapon = weapon; in.mode = mode; in.age = 1; in.camQuat[3] = 1; QcAsmNode node;
  return qc_asm_fx_pose(&in, &node, 1) == 1 ? node.scale[0] / 0.6 : 0;
}

}  // namespace nc
