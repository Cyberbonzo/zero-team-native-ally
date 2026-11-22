// The native title has its own frontend, ROM identity and fixed video layout.
// FBNeo's optional patch, cheat and bezel services are not part of this board.
#include "burnint.h"
RomDataInfo *pRDI = nullptr;
BurnRomInfo *pDataRomDesc = nullptr;
bool bDoIpsPatch = false;
UINT32 nIpsMemExpLen[SND2_ROM+1] = {};
void IpsApplyPatches(UINT8 *, char *, UINT32, bool) {}
void CpuCheatRegister(INT32, cpu_core_config *) {}
void HiscoreReset(INT32) {}
INT32 BurnArtworkGetDisplaySize(INT32 *, INT32 *) { return 0; }
INT32 BurnArtworkGetAspect(INT32 *, INT32 *) { return 0; }
void BurnArtworkFrameBegin() {}
void BurnArtworkFrameEnd() {}

// The board keeps its own save directory; FBNeo's EEPROM path is unused but
// referenced by the shared driver exit path.
TCHAR szAppEEPROMPath[MAX_PATH] = _T("config/games/");
