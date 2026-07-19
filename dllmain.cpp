#include "pch.h"
#include <windows.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cctype>
#include <cstdlib>

#include "eiface.h"
#include "engine/iserverplugin.h"
#include "cdll_int.h"
#include "icliententitylist.h"
#include "icliententity.h"
#include "iclientunknown.h"
#include "client_class.h"
#include "tier1/KeyValues.h"
#include "filesystem.h"
#include "materialsystem/imaterialsystem.h"
#include "materialsystem/imaterial.h"
#include "materialsystem/imaterialvar.h"
#include "materialsystem/imaterialproxy.h"
#include "materialsystem/imaterialproxyfactory.h"

// === SEH-guarded KeyValues reads ===
// A C++ try/catch does NOT catch access violations unless the project has
// /EHa enabled - it only catches genuine C++ exceptions by default (/EHsc).
// Windows SEH (__try/__except) catches access violations regardless of that
// compiler setting, which is what we actually need here: pKeyValues has
// crashed GetString() even though it looked like a legitimate, non-null
// pointer when logged right before the call. These are isolated in their own
// plain functions because __try/__except cannot safely surround C++ objects
// that have destructors in the same function.
static const char* SafeGetKeyValuesString(KeyValues* pKV, const char* pKeyName, const char* pDefault)
{
	__try
	{
		return pKV->GetString(pKeyName, pDefault);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		OutputDebugStringA("[2DSpeedProxy] SEH: access violation reading KeyValues string - using default\n");
		return pDefault;
	}
}

static float SafeGetKeyValuesFloat(KeyValues* pKV, const char* pKeyName, float flDefault)
{
	__try
	{
		return pKV->GetFloat(pKeyName, flDefault);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		OutputDebugStringA("[2DSpeedProxy] SEH: access violation reading KeyValues float - using default\n");
		return flDefault;
	}
}

// Logging
static void LogLine(const char* fmt, ...)
{
	char buf[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	strcat_s(buf, "\n");
	OutputDebugStringA(buf);
}

// Tiny local struct for velocity reading (3 raw floats at known offset)
struct Vector3D
{
	float x, y, z;
	float Length2D() const { return std::sqrt(x * x + y * y); }
	float Length3D() const { return std::sqrt(x * x + y * y + z * z); }
};

// Globals
static IMaterialSystem* g_pMatSys = nullptr;
static IMaterialProxyFactory* g_pOldFactory = nullptr;
static IFileSystem* g_pFileSystem = nullptr;
static IVEngineClient* g_pEngine = nullptr;
static IClientEntityList* g_pEntityList = nullptr;
static IBaseClientDLL* g_pClientDLL = nullptr;
static int g_iVelocityOffset = -1;
static int g_iObserverModeOffset = -1;
static int g_iObserverTargetOffset = -1;
static int g_iResolveAttempts = 0;
static const int MAX_RESOLVE_ATTEMPTS = 3;

// === Offset Resolution Helper Functions ===
// Recursively search RecvTable for m_vecVelocity prop, summing offsets
static bool FindRecvPropOffset(RecvTable* pTable, const char* propName, int baseOffset, int& outOffset)
{
	if (!pTable) return false;

	for (int i = 0; i < pTable->GetNumProps(); i++)
	{
		RecvProp* pProp = pTable->GetProp(i);
		if (!pProp) continue;

		if (pProp->GetDataTable())
		{
			if (FindRecvPropOffset(pProp->GetDataTable(), propName, baseOffset + pProp->GetOffset(), outOffset))
				return true;
		}
		else if (pProp->GetName() && strcmp(pProp->GetName(), propName) == 0)
		{
			outOffset = baseOffset + pProp->GetOffset();
			return true;
		}
	}
	return false;
}

// Resolve m_vecVelocity, m_iObserverMode, and m_hObserverTarget offsets at runtime
// by walking client RecvTable. Observer offsets are best-effort (not fatal if missing).
// SEH-guarded wrapper: on pre-TLS builds, pClientDLL->GetAllClasses() and the
// subsequent RecvTable walk are virtual calls resolved against the vtable
// layout our SDK headers (compiled for post-TLS L4D2) expect. That layout can
// differ from the actual pre-TLS binary - it's the same class of ABI mismatch
// that hit KeyValues, just on a different interface. GetAllClasses() has been
// observed to return outright garbage (e.g. 0x160) instead of a real pointer
// on such builds. Guarding the whole walk means a mismatch here degrades to
// the existing hardcoded fallback offset instead of crashing the game.
static bool ResolveVelocityOffsetImpl(IBaseClientDLL* pClientDLL)
{
	ClientClass* pClass = pClientDLL->GetAllClasses();

	if (!pClass)
	{
		//LogLine("[2DSpeedProxy] GetAllClasses() returned NULL pointer");
		return false;
	}

	while (pClass)
	{
		if (pClass->m_pNetworkName && strcmp(pClass->m_pNetworkName, "CTerrorPlayer") == 0)
		{
			int offset = 0;
			bool bGotVelocity = false;

			if (FindRecvPropOffset(pClass->m_pRecvTable, "m_vecVelocity", 0, offset))
			{
				if (offset >= 0 && offset <= 65536)
				{
					g_iVelocityOffset = offset;
					//LogLine("[2DSpeedProxy] Resolved m_vecVelocity offset = %d (client-side RecvTable)", offset);
					bGotVelocity = true;
				}
				else
				{
					//LogLine("[2DSpeedProxy] Resolved m_vecVelocity to an implausible offset = %d -- rejecting it", offset);
				}
			}
			else
			{
				//LogLine("[2DSpeedProxy] Found CTerrorPlayer but couldn't find m_vecVelocity in its RecvTable");
			}

			// Observer mode/target - needed to detect noclip while spectating another player
			int obsOffset = 0;
			if (FindRecvPropOffset(pClass->m_pRecvTable, "m_iObserverMode", 0, obsOffset))
			{
				g_iObserverModeOffset = obsOffset;
				//LogLine("[2DSpeedProxy] Resolved m_iObserverMode offset = %d", obsOffset);
			}
			else
			{
				//LogLine("[2DSpeedProxy] Could not find m_iObserverMode in RecvTable");
			}

			int targetOffset = 0;
			if (FindRecvPropOffset(pClass->m_pRecvTable, "m_hObserverTarget", 0, targetOffset))
			{
				g_iObserverTargetOffset = targetOffset;
				//LogLine("[2DSpeedProxy] Resolved m_hObserverTarget offset = %d", targetOffset);
			}
			else
			{
				//LogLine("[2DSpeedProxy] Could not find m_hObserverTarget in RecvTable");
			}

			return bGotVelocity;
		}
		pClass = pClass->m_pNext;
	}

	// CTerrorPlayer not found, dump what we got
	int iCount = 0;
	pClass = pClientDLL->GetAllClasses();
	while (pClass)
	{
		//LogLine("[2DSpeedProxy]   class[%d] = \"%s\"", iCount,
		//	pClass->m_pNetworkName ? pClass->m_pNetworkName : "(null name)");
		pClass = pClass->m_pNext;
		iCount++;
	}
	//LogLine("[2DSpeedProxy] Could not find CTerrorPlayer client class -- dumped %d classes above", iCount);
	return false;
}

// Public entry point - same name/signature the rest of the file already calls.
// Wraps the actual walk in SEH so a vtable/ABI mismatch (garbage pointer from
// GetAllClasses(), bad RecvTable pointers, etc.) can't take the process down;
// it just fails this resolution attempt and the caller's existing hardcoded
// fallback (g_iVelocityOffset = 0x100) takes over instead.
static bool ResolveVelocityOffset(IBaseClientDLL* pClientDLL)
{
	if (!pClientDLL)
	{
		//LogLine("[2DSpeedProxy] ResolveVelocityOffset called with NULL pClientDLL!");
		return false;
	}

	__try
	{
		return ResolveVelocityOffsetImpl(pClientDLL);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		//LogLine("[2DSpeedProxy] SEH: access violation walking client classes/RecvTable "
		//	"(GetAllClasses ABI mismatch on this build) - falling back to hardcoded offset");
		return false;
	}
}

// === Movement State Classification ===
// Both the existing 2D-mode speed calc AND the label-state proxy read this -
// single source of truth, no duplication.
enum class EMovementState
{
	Normal, // default - 2D/UPS, idle AND near-zero jumps included
	Ladder, // MOVETYPE_LADDER
};

// MOVETYPE_LADDER = 9 (same m_MoveType field already used for WALK=2/NOCLIP=8
// at 0x144). Real engine state instead of a velocity guess - no more stutter,
// matches sw1ft's TAS Kit MOVETYPE_LADDER check.
static bool IsPlayerOnLadderMoveType(C_BaseEntity* pEntity)
{
	if (!pEntity) return false;
	try {
		const char* pBase = reinterpret_cast<const char*>(pEntity);
		int iMoveType = *reinterpret_cast<const int*>(pBase + 0x144);
		return iMoveType == 9; // MOVETYPE_LADDER
	}
	catch (...) {
		return false;
	}
}


static EMovementState ClassifyMovementState(C_BaseEntity* pEntity)
{
	if (IsPlayerOnLadderMoveType(pEntity))
		return EMovementState::Ladder;
	return EMovementState::Normal;
}

// Only Ladder forces full 3D in the 2D speed calc below.
static bool IsOnLadder(C_BaseEntity* pEntity)
{
	return IsPlayerOnLadderMoveType(pEntity);
}

// If spectating (observer mode != 0) with a valid target, returns the target entity.
// Otherwise (playing normally, or free-roam with no target) returns pLocalEntity itself.
// This makes noclip/velocity reads follow whoever you're actually spectating.
static C_BaseEntity* ResolveEffectiveEntity(C_BaseEntity* pLocalEntity)
{
	if (!pLocalEntity || g_iObserverModeOffset < 0 || g_iObserverTargetOffset < 0 || !g_pEntityList)
		return pLocalEntity;

	const char* pBase = reinterpret_cast<const char*>(pLocalEntity);
	int iObsMode = *reinterpret_cast<const int*>(pBase + g_iObserverModeOffset);

	// OBS_MODE_NONE = 0 -> not spectating, use self
	if (iObsMode == 0)
		return pLocalEntity;

	int iHandleRaw = *reinterpret_cast<const int*>(pBase + g_iObserverTargetOffset);
	if (iHandleRaw == 0 || iHandleRaw == 0xFFFFFFFF)
		return pLocalEntity; // no valid target (e.g. free-roam), fall back to self

	CBaseHandle hTarget(static_cast<unsigned long>(iHandleRaw));
	IClientEntity* pTargetEnt = g_pEntityList->GetClientEntityFromHandle(hTarget);
	if (!pTargetEnt)
		return pLocalEntity;

	C_BaseEntity* pTargetBase = pTargetEnt->GetBaseEntity();
	return pTargetBase ? pTargetBase : pLocalEntity;
}

// MoveType 0x144 confirmed: 2 = MOVETYPE_WALK, 8 = MOVETYPE_NOCLIP. Logs only on change now, no more per-frame spam.
static bool IsInNoclip(C_BaseEntity* pEntity)
{
	if (!pEntity) return false;

	try {
		int iOffset = 0x144; // confirmed via cheat engine address scanning: 2 = MOVETYPE_WALK, 8 = MOVETYPE_NOCLIP
		const char* pBase = reinterpret_cast<const char*>(pEntity);
		int* pMoveType = reinterpret_cast<int*>((void*)(pBase + iOffset));

		// Only log when the value actually changes
		static int iLastMoveType = -1;
		if (*pMoveType != iLastMoveType)
		{
			iLastMoveType = *pMoveType;
			if (*pMoveType == 2)
				LogLine("[2DSpeedProxy] MOVETYPE = WALK");
			else if (*pMoveType == 8)
				LogLine("[2DSpeedProxy] MOVETYPE = NOCLIP");
		}

		// MOVETYPE_NOCLIP = 8
		return (*pMoveType == 8);
	}
	catch (...) {
		return false;
	}
}

static float ComputeSmoothedLocalPlayerSpeed2D(bool bForce3D = false)
{
	const int FILTER_SAMPLES = 1;
	static float flHistory[FILTER_SAMPLES] = { 0.0f };
	static int   iIndex = 0;
	static bool  bFilled = false;
	static bool  bWasOnLadder = false;

	float flRaw = 0.0f;

	try {
		if (g_pEngine && g_pEntityList && g_iVelocityOffset >= 0)
		{
			int iLocalPlayer = g_pEngine->GetLocalPlayer();
			if (iLocalPlayer > 0)
			{
				IClientEntity* pClientEnt = g_pEntityList->GetClientEntity(iLocalPlayer);
				if (pClientEnt)
				{
					// Get the actual C_BaseEntity, not the IClientEntity wrapper
					C_BaseEntity* pEntity = pClientEnt->GetBaseEntity();

					// If spectating/free-roaming, follow the target being observed instead of self
					pEntity = ResolveEffectiveEntity(pEntity);

					if (pEntity)
					{
						// Cast to char* for pointer arithmetic, then cast to Vector3D
						const char* pRaw = reinterpret_cast<const char*>(pEntity);
						const Vector3D* pVel = reinterpret_cast<const Vector3D*>(pRaw + g_iVelocityOffset);

						// If in noclip, return raw 3D velocity, bypass ladder/2D logic entirely
						if (IsInNoclip(pEntity))
						{
							return pVel->Length3D();
						}

						// Always use 2D for bhop, 3D only on ladders - unless the
						// calling material explicitly opted into "mode 3d", in which
						// case always report full 3D magnitude (still smoothed the
						// same way $speed/$2dvel are).
						bool bOnLadder = IsOnLadder(pEntity);
						if (bForce3D || bOnLadder)
							flRaw = pVel->Length3D();  // Full 3D velocity (forced, or on ladder)
						else
							flRaw = pVel->Length2D();  // Only horizontal velocity for bhop (2D mode)

						bWasOnLadder = bOnLadder;
					}
				}
			}
		}
	}
	catch (...) {
		// Silently ignore any errors accessing entity data during demos
		flRaw = 0.0f;
	}

	flHistory[iIndex] = flRaw;
	iIndex = (iIndex + 1) % FILTER_SAMPLES;
	if (iIndex == 0) bFilled = true;

	int iCount = bFilled ? FILTER_SAMPLES : iIndex;
	float flSum = 0.0f;
	for (int i = 0; i < iCount; i++) flSum += flHistory[i];

	return iCount > 0 ? (flSum / iCount) : 0.0f;
}

// === Real VMT-text auto-detection (replaces the old hardcoded whitelist) ===
// We no longer guess which shader var a material uses, or hardcode its mode.
// Instead we open the material's own .vmt off disk (via IFileSystem - not
// KeyValues, which is what crashes on this pre-TLS build) and read the
// "PlayerSpeed" proxy block's actual "resultVar", "scale" and "mode" keys.
//
// Rule (per spec): if the vmt's PlayerSpeed block has no "mode" key at all,
// treat it as raw 3D - i.e. identical to cl_showpos 1 velocity, unsmoothed,
// jump/bhop spikes and all. Only an explicit mode "2d" turns on the
// smoothed/hysteresis horizontal-only path. This makes every material in the
// addon (and anything the user adds) work automatically, with zero editing
// of this file required when someone edits a .vmt.
struct VmtProxyConfig
{
	char  szResultVar[64] = { 0 };
	float flScale = 1.0f;
	bool  bMode2D = false; // false = raw 3D (default when "mode" key absent)
	bool  bValid = false;
};

// Find needle in haystack, case-insensitive. Minimal, no CRT locale nonsense.
static const char* FindCaseInsensitive(const char* pHaystack, const char* pNeedle)
{
	if (!pHaystack || !pNeedle || !*pNeedle) return nullptr;
	size_t needleLen = strlen(pNeedle);
	for (const char* p = pHaystack; *p; p++)
	{
		size_t i = 0;
		for (; i < needleLen; i++)
		{
			if (!p[i]) break;
			if (std::tolower((unsigned char)p[i]) != std::tolower((unsigned char)pNeedle[i])) break;
		}
		if (i == needleLen) return p;
	}
	return nullptr;
}

// Extract the token following pszKey inside [pBlockStart, pBlockEnd) into
// pOutBuf. Plain pointer scanning only - no STL, no heap allocation, so this
// can't clash with the host process's CRT/heap the way std::string can.
static bool ExtractKeyValue(const char* pBlockStart, const char* pBlockEnd, const char* pszKey,
	char* pOutBuf, size_t iOutBufSize)
{
	if (!pBlockStart || !pBlockEnd || pBlockEnd <= pBlockStart || iOutBufSize == 0)
		return false;

	size_t iKeyLen = strlen(pszKey);
	const char* pKey = nullptr;

	for (const char* p = pBlockStart; p + iKeyLen <= pBlockEnd; p++)
	{
		size_t i = 0;
		for (; i < iKeyLen; i++)
		{
			if (std::tolower((unsigned char)p[i]) != std::tolower((unsigned char)pszKey[i])) break;
		}
		if (i == iKeyLen) { pKey = p; break; }
	}
	if (!pKey) return false;

	const char* p = pKey + iKeyLen;
	// must not be a match against a longer key sharing this prefix
	if (p < pBlockEnd && (std::isalnum((unsigned char)*p) || *p == '_'))
		return false;

	bool bWasQuoted = false;
	while (p < pBlockEnd && (*p == '"' || *p == ' ' || *p == '\t'))
	{
		if (*p == '"') bWasQuoted = true;
		p++;
	}

	size_t iOut = 0;
	if (bWasQuoted)
	{
		while (p < pBlockEnd && *p != '"' && iOut + 1 < iOutBufSize)
			pOutBuf[iOut++] = *p++;
	}
	else
	{
		while (p < pBlockEnd && !std::isspace((unsigned char)*p) && *p != '{' && *p != '}' && *p != '"'
			&& iOut + 1 < iOutBufSize)
			pOutBuf[iOut++] = *p++;
	}
	pOutBuf[iOut] = '\0';
	return iOut > 0;
}

// Locate a Proxies{} sub-block by exact key name (e.g. "PlayerSpeed") with
// real word-boundary checks - so searching for "PlayerSpeed" can never match
// inside "PlayerSpeedState" by accident (plain substring search would).
static bool FindProxyBlock(const char* pText, const char* pTextEnd, const char* pszProxyName,
	const char** ppBlockStart, const char** ppBlockEnd)
{
	size_t iNameLen = strlen(pszProxyName);
	for (const char* p = pText; p + iNameLen <= pTextEnd; p++)
	{
		size_t i = 0;
		for (; i < iNameLen; i++)
		{
			if (std::tolower((unsigned char)p[i]) != std::tolower((unsigned char)pszProxyName[i])) break;
		}
		if (i != iNameLen) continue;

		// char before the match (if any) must not be alnum/underscore
		if (p > pText && (std::isalnum((unsigned char)p[-1]) || p[-1] == '_'))
			continue;
		// char after the match must not be alnum/underscore either - this is
		// the check that stops "PlayerSpeed" from matching "PlayerSpeedState"
		const char* pAfter = p + iNameLen;
		if (pAfter < pTextEnd && (std::isalnum((unsigned char)*pAfter) || *pAfter == '_'))
			continue;

		const char* pOpen = pAfter;
		while (pOpen < pTextEnd && *pOpen != '{') pOpen++;
		if (pOpen >= pTextEnd) continue;

		int iDepth = 0;
		const char* pClose = nullptr;
		for (const char* q = pOpen; q < pTextEnd; q++)
		{
			if (*q == '{') iDepth++;
			else if (*q == '}')
			{
				iDepth--;
				if (iDepth == 0) { pClose = q; break; }
			}
		}
		if (!pClose) continue;

		*ppBlockStart = pOpen;
		*ppBlockEnd = pClose + 1;
		return true;
	}
	return false;
}

// Read materials/<pMaterial->GetName()>.vmt directly off disk, locate the
// Proxies { PlayerSpeed { ... } } block, and pull out its real settings.
// Uses a fixed local buffer (no std::string/new) - VMTs are a few KB at most.
static bool ReadPlayerSpeedProxyConfig(const char* pszMaterialName, VmtProxyConfig& outCfg)
{
	if (!g_pFileSystem || !pszMaterialName || !*pszMaterialName)
		return false;

	char szPath[512];
	_snprintf_s(szPath, sizeof(szPath), _TRUNCATE, "materials/%s.vmt", pszMaterialName);

	FileHandle_t hFile = g_pFileSystem->Open(szPath, "rt", "GAME");
	if (!hFile)
	{
		//LogLine("[2DSpeedProxy] ReadPlayerSpeedProxyConfig: could not open \"%s\"", szPath);
		return false;
	}

	const unsigned int kMaxVmtSize = 16384;
	unsigned int uSize = g_pFileSystem->Size(hFile);
	if (uSize == 0 || uSize > kMaxVmtSize)
	{
		//LogLine("[2DSpeedProxy] ReadPlayerSpeedProxyConfig: \"%s\" bad size %u", szPath, uSize);
		g_pFileSystem->Close(hFile);
		return false;
	}

	char szText[kMaxVmtSize + 1];
	int iRead = g_pFileSystem->Read(szText, (int)uSize, hFile);
	g_pFileSystem->Close(hFile);

	if (iRead <= 0)
		return false;
	szText[iRead] = '\0';
	const char* pTextEnd = szText + iRead;

	const char* pOpen = nullptr;
	const char* pBlockEnd = nullptr;
	if (!FindProxyBlock(szText, pTextEnd, "PlayerSpeed", &pOpen, &pBlockEnd))
	{
		//LogLine("[2DSpeedProxy] \"%s\" has no PlayerSpeed proxy - skipping", szPath);
		return false;
	}
	const char* pClose = pBlockEnd - 1;

	char szScale[32] = { 0 };
	char szMode[32] = { 0 };

	bool bHasResultVar = ExtractKeyValue(pOpen, pClose + 1, "resultVar", outCfg.szResultVar, sizeof(outCfg.szResultVar));
	bool bHasScale = ExtractKeyValue(pOpen, pClose + 1, "scale", szScale, sizeof(szScale));
	bool bHasMode = ExtractKeyValue(pOpen, pClose + 1, "mode", szMode, sizeof(szMode));

	if (!bHasResultVar)
	{
		//LogLine("[2DSpeedProxy] \"%s\" PlayerSpeed block has no resultVar - skipping", szPath);
		return false;
	}

	outCfg.flScale = bHasScale ? (float)atof(szScale) : 1.0f;

	// THE RULE: no "mode" key at all -> raw 3D (cl_showpos-style), same as
	// numbers.vmt today. Only an explicit mode "2d" enables smoothing.
	outCfg.bMode2D = bHasMode && (FindCaseInsensitive(szMode, "2d") != nullptr);
	outCfg.bValid = true;

	LogLine("[2DSpeedProxy] \"%s\" -> resultVar=%s scale=%.2f mode=%s (read from vmt on disk)",
		szPath, outCfg.szResultVar, outCfg.flScale, outCfg.bMode2D ? "2d" : "3d (default)");

	return true;
}

// === PlayerSpeedState: drives $frame on the speedometer label texture ===
// Frame layout baked into the atlas: 0=UPS (default/normal), 1=VEL
// (near-zero-velocity launch), 2=LADDER (real ladder climb), 3=NOCLIP.
// resultVar defaults to "$frame" - only reads the vmt at all to allow an
// override, same philosophy as PlayerSpeed's resultVar.
struct VmtStateProxyConfig
{
	char szResultVar[64] = "$frame";
	bool bValid = false;
};

static bool ReadPlayerSpeedStateProxyConfig(const char* pszMaterialName, VmtStateProxyConfig& outCfg)
{
	if (!g_pFileSystem || !pszMaterialName || !*pszMaterialName)
		return false;

	char szPath[512];
	_snprintf_s(szPath, sizeof(szPath), _TRUNCATE, "materials/%s.vmt", pszMaterialName);

	FileHandle_t hFile = g_pFileSystem->Open(szPath, "rt", "GAME");
	if (!hFile)
		return false;

	const unsigned int kMaxVmtSize = 16384;
	unsigned int uSize = g_pFileSystem->Size(hFile);
	if (uSize == 0 || uSize > kMaxVmtSize)
	{
		g_pFileSystem->Close(hFile);
		return false;
	}

	char szText[kMaxVmtSize + 1];
	int iRead = g_pFileSystem->Read(szText, (int)uSize, hFile);
	g_pFileSystem->Close(hFile);
	if (iRead <= 0)
		return false;
	szText[iRead] = '\0';
	const char* pTextEnd = szText + iRead;

	const char* pOpen = nullptr;
	const char* pBlockEnd = nullptr;
	if (!FindProxyBlock(szText, pTextEnd, "PlayerSpeedState", &pOpen, &pBlockEnd))
		return false; // block not present -> keep default "$frame", not an error

	char szVar[64];
	if (ExtractKeyValue(pOpen, pBlockEnd, "resultVar", szVar, sizeof(szVar)))
		strncpy_s(outCfg.szResultVar, szVar, _TRUNCATE);

	outCfg.bValid = true;
	return true;
}

// PlayerSpeed material proxy
class CSpeedProxy : public IMaterialProxy
{
private:
	IMaterialVar* m_pSpeedVar = nullptr;
	float m_flScale = 1.0f;
	bool m_bForce3D = false;
	static float flLastDisplayed;

public:
	bool Init(IMaterial* pMaterial, KeyValues* pKeyValues) override
	{
		(void)pKeyValues; // deliberately never touched - see comment above

		const char* pszMatName = pMaterial ? pMaterial->GetName() : nullptr;
		//LogLine("[2DSpeedProxy] PlayerSpeed::Init material = \"%s\"", pszMatName ? pszMatName : "(null)");

		if (!pMaterial)
		{
			m_flScale = 1.0f;
			m_bForce3D = true;
			return false;
		}

		VmtProxyConfig cfg;
		if (!ReadPlayerSpeedProxyConfig(pszMatName, cfg))
		{
			//LogLine("[2DSpeedProxy] PlayerSpeed: could not read a valid config for \"%s\" from its vmt - skipping (not our material)",
			//	pszMatName ? pszMatName : "(null)");
			m_flScale = 1.0f;
			m_bForce3D = true;
			return false;
		}

		bool bFound = false;
		m_pSpeedVar = pMaterial->FindVar(cfg.szResultVar, &bFound);
		if (!bFound || !m_pSpeedVar)
		{
			//LogLine("[2DSpeedProxy] PlayerSpeed: vmt for \"%s\" names resultVar \"%s\" but FindVar failed - skipping",
			//	pszMatName ? pszMatName : "(null)", cfg.szResultVar);
			m_flScale = 1.0f;
			m_bForce3D = true;
			return false;
		}

		m_flScale = cfg.flScale;
		m_bForce3D = !cfg.bMode2D; // no "mode" key, or anything other than "2d", = raw 3D

		//LogLine("[2DSpeedProxy] PlayerSpeed: material \"%s\" -> resultVar=%s scale=%.2f mode=%s (auto-detected from vmt on disk)",
		//	pszMatName ? pszMatName : "(null)", cfg.szResultVar, m_flScale, m_bForce3D ? "3d" : "2d");

		return true;
	}

	void OnBind(void* pRenderable) override
	{
		if (!m_pSpeedVar) return;

		if (m_bForce3D)
		{
			///////////////////////////////////////////////////////////////////////
			// mode "3d" (default): raw, no smoothing/rounding/hysteresis - just  //
			// * scale. With scale 1 this mirrors cl_showpos exactly, in every    //
			// state (walking, bhopping, ladders, noclip).                       //
			///////////////////////////////////////////////////////////////////////
			m_pSpeedVar->SetFloatValue(ComputeSmoothedLocalPlayerSpeed2D(true) * m_flScale);
			return;
		}

		///////////////////////////////////////////////////////////////////////////
		// mode "2d": horizontal-only speed, WITH smoothing/hysteresis applied -  //
		// this used to be the separate $2dvel/PlayerSpeed2D proxy, now merged    //
		// in here so "mode" alone decides raw-vs-clean instead of needing a      //
		// second proxy name. Still forces full 3D during noclip (bypassing      //
		// hysteresis) since noclip speed changes are intentional/instant, not    //
		// jitter to smooth out. Ladder gets the same bypass, for accuracy: the   //
		// std::round() below would show 199.99 as 200, and climbing wants the    //
		// exact value, not the rounded/hysteresis-smoothed one.                  //
		///////////////////////////////////////////////////////////////////////////
		bool bNoclip = false;
		bool bOnLadder = false;
		if (g_pEngine && g_pEntityList)
		{
			int iLocalPlayer = g_pEngine->GetLocalPlayer();
			if (iLocalPlayer > 0)
			{
				IClientEntity* pClientEnt = g_pEntityList->GetClientEntity(iLocalPlayer);
				if (pClientEnt)
				{
					C_BaseEntity* pEntity = pClientEnt->GetBaseEntity();
					pEntity = ResolveEffectiveEntity(pEntity);
					bNoclip = pEntity && IsInNoclip(pEntity);
					bOnLadder = pEntity && IsOnLadder(pEntity);
				}
			}
		}

		if (bNoclip || bOnLadder)
		{
			m_pSpeedVar->SetFloatValue(ComputeSmoothedLocalPlayerSpeed2D() * m_flScale);
			return;
		}

		float flSmoothed = ComputeSmoothedLocalPlayerSpeed2D();
		float flRounded = std::round(flSmoothed);

		if (flRounded <= 2.0f)
		{
			flLastDisplayed = 0.0f;
		}
		else if (flRounded > flLastDisplayed)
		{
			flLastDisplayed = flRounded;
		}
		else if (flRounded < flLastDisplayed)
		{
			float flDelta = flLastDisplayed - flRounded;
			if (flDelta >= 3.0f)
			{
				flLastDisplayed = flRounded;
			}
		}

		m_pSpeedVar->SetFloatValue(flLastDisplayed * m_flScale);
	}

	void Release() override
	{
		delete this;
	}

	IMaterial* GetMaterial() override
	{
		return nullptr;
	}
};

// Static member initialization
float CSpeedProxy::flLastDisplayed = 0.0f;

// === CSpeedStateProxy: drives $frame on the speedometer label atlas ===
// Frame mapping (must match the order frames were baked into the .vtf):
//   0 = VEL     - unused by this code 
//   1 = UPS     - default/normal 2D-speed-tracking (also what shows if the
//                 plugin isn't loaded, since $frame just stays at its
//                 built-in default)
//   2 = CLIMB   - genuinely climbing a ladder (real MOVETYPE_LADDER)
//   3 = NOCLIP  - noclip active = Shows 3d Vel when it's active
//   4 = BOOST   - unused, same as frame 0 (present in the .vtf, not wired up)
//				   (boost detection was rolled back -
//                 too unreliable on laggy clients), but still present in
//                 the.vtf atlas, just never selected
class CSpeedStateProxy : public IMaterialProxy
{
private:
	IMaterialVar* m_pStateVar = nullptr;

public:
	bool Init(IMaterial* pMaterial, KeyValues* pKeyValues) override
	{
		(void)pKeyValues;

		const char* pszMatName = pMaterial ? pMaterial->GetName() : nullptr;
		//LogLine("[2DSpeedProxy] PlayerSpeedState::Init material = \"%s\"", pszMatName ? pszMatName : "(null)");

		if (!pMaterial)
			return false;

		VmtStateProxyConfig cfg;
		ReadPlayerSpeedStateProxyConfig(pszMatName, cfg); // ok if this returns false - szResultVar still defaults to "$frame"

		bool bFound = false;
		m_pStateVar = pMaterial->FindVar(cfg.szResultVar, &bFound);
		if (!bFound || !m_pStateVar)
		{
			//LogLine("[2DSpeedProxy] PlayerSpeedState: material \"%s\" has no \"%s\" var - skipping",
			//	pszMatName ? pszMatName : "(null)", cfg.szResultVar);
			return false;
		}

		//LogLine("[2DSpeedProxy] PlayerSpeedState: material \"%s\" -> resultVar=%s (auto-detected from vmt on disk)",
		//	pszMatName ? pszMatName : "(null)", cfg.szResultVar);

		return true;
	}

	void OnBind(void* pRenderable) override
	{
		if (!m_pStateVar) return;

		int iFrame = 1; // default: UPS

		if (g_pEngine && g_pEntityList && g_iVelocityOffset >= 0)
		{
			try
			{
				int iLocalPlayer = g_pEngine->GetLocalPlayer();
				if (iLocalPlayer > 0)
				{
					IClientEntity* pClientEnt = g_pEntityList->GetClientEntity(iLocalPlayer);
					if (pClientEnt)
					{
						C_BaseEntity* pEntity = pClientEnt->GetBaseEntity();
						pEntity = ResolveEffectiveEntity(pEntity);
						if (pEntity)
						{
							if (IsInNoclip(pEntity))
							{
								iFrame = 3; // NOCLIP
							}
							else
							{
								switch (ClassifyMovementState(pEntity))
								{
								case EMovementState::Ladder: iFrame = 2; break; // CLIMB
								default:                      iFrame = 1; break; // UPS
								}
							}
						}
					}
				}
			}
			catch (...)
			{
				iFrame = 1; // UPS
			}
		}

		m_pStateVar->SetIntValue(iFrame);
	}

	void Release() override
	{
		delete this;
	}

	IMaterial* GetMaterial() override
	{
		return nullptr;
	}
};

// Proxy factory
class CProxyFactory : public IMaterialProxyFactory
{
public:
	void SetOriginalFactory(IMaterialProxyFactory* pOriginal)
	{
		if (pOriginal == this)
		{
			//LogLine("[2DSpeedProxy] WARNING: GetMaterialProxyFactory() returned ourselves -- not chaining");
			m_pOldFactory = nullptr;
			return;
		}
		m_pOldFactory = pOriginal;
	}

	IMaterialProxy* CreateProxy(const char* name) override
	{
		//LogLine("[2DSpeedProxy] CreateProxy called: %s", name);
		if (name && strcmp(name, "PlayerSpeed") == 0)
		{
			LogLine("[2DSpeedProxy] Creating PlayerSpeed proxy");
			return new CSpeedProxy();
		}
		if (name && strcmp(name, "PlayerSpeedState") == 0)
		{
			LogLine("[2DSpeedProxy] Creating PlayerSpeedState proxy");
			return new CSpeedStateProxy();
		}
		// Pass through to old factory for other proxies
		if (!m_pOldFactory)
			return nullptr;

		if (m_iRecursionDepth > 8)
		{
			//LogLine("[2DSpeedProxy] WARNING: CreateProxy recursion depth exceeded for \"%s\"", name ? name : "(null)");
			return nullptr;
		}

		m_iRecursionDepth++;
		IMaterialProxy* pResult = m_pOldFactory->CreateProxy(name);
		m_iRecursionDepth--;
		return pResult;
	}

	void DeleteProxy(IMaterialProxy* proxy) override
	{
		if (proxy) proxy->Release();
	}

private:
	IMaterialProxyFactory* m_pOldFactory = nullptr;
	int m_iRecursionDepth = 0;
};

static CProxyFactory* g_pProxyFactory = nullptr;

// Plugin
class CPlugin : public IServerPluginCallbacks
{
public:
	bool Load(CreateInterfaceFn engine, CreateInterfaceFn gameServer) override
	{
		LogLine("[2DSpeedProxy] Load() called");

		// Get engine client interface (for GetLocalPlayer)
		g_pEngine = (IVEngineClient*)engine(VENGINE_CLIENT_INTERFACE_VERSION, nullptr);
		if (!g_pEngine)
		{
			LogLine("[2DSpeedProxy] Failed to get engine client");
			return true;
		}
		LogLine("[2DSpeedProxy] Got engine: %p", g_pEngine);

		// Get client.dll interfaces (IBaseClientDLL for GetAllClasses, IClientEntityList for entities)
		HMODULE hClientDll = GetModuleHandleA("client.dll");
		if (!hClientDll)
		{
			LogLine("[2DSpeedProxy] FAILED to get client.dll module handle");
			return true;
		}

		auto clientFactory = reinterpret_cast<CreateInterfaceFn>(
			GetProcAddress(hClientDll, "CreateInterface"));

		if (!clientFactory)
		{
			LogLine("[2DSpeedProxy] FAILED to get client.dll's CreateInterface");
			return true;
		}

		// Try multiple versions since I don't know exactly which one is active xd
		const char* clientVersions[] = { "VClient016", "VClient015", "VClient014", "VClient013" };
		int iClientReturnCode = -999;

		for (int vi = 0; vi < 4; vi++)
		{
			const char* pszClientVersion = clientVersions[vi];
			g_pClientDLL = static_cast<IBaseClientDLL*>(
				clientFactory(pszClientVersion, &iClientReturnCode));

			if (g_pClientDLL)
			{
				LogLine("[2DSpeedProxy] Got IBaseClientDLL with version \"%s\" (return code %d)",
					pszClientVersion, iClientReturnCode);
				ResolveVelocityOffset(g_pClientDLL);
				break;
			}
			else
			{
				LogLine("[2DSpeedProxy] Version \"%s\" not available (code %d)", pszClientVersion, iClientReturnCode);
			}
		}

		if (!g_pClientDLL)
			LogLine("[2DSpeedProxy] FAILED to get IBaseClientDLL from any version!");
		else
		{
			if (!ResolveVelocityOffset(g_pClientDLL))
			{
				// Fallback to hardcoded offset if dynamic resolution fails at Load() time
				// L4D2 CTerrorPlayer m_vecVelocity is at offset 0x100 (confirmed via runtime scan)
				g_iVelocityOffset = 0x100;
				LogLine("[2DSpeedProxy] Dynamic resolution failed at Load(), using fallback offset: 0x%X", g_iVelocityOffset);
			}
		}

		// Get IClientEntityList
		int iEntListReturnCode = -999;
		g_pEntityList = static_cast<IClientEntityList*>(
			clientFactory(VCLIENTENTITYLIST_INTERFACE_VERSION, &iEntListReturnCode));

		if (!g_pEntityList)
			LogLine("[2DSpeedProxy] FAILED to get IClientEntityList (version \"%s\", code %d)",
				VCLIENTENTITYLIST_INTERFACE_VERSION, iEntListReturnCode);

		// Get filesystem interface (from filesystem_stdio.dll) so we can read
		// .vmt files directly off disk for PlayerSpeed's real mode/scale/resultVar.
		HMODULE hFileSys = GetModuleHandleA("filesystem_stdio.dll");
		if (!hFileSys)
		{
			LogLine("[2DSpeedProxy] FAILED to get filesystem_stdio.dll module handle - vmt auto-detect will not work");
		}
		else
		{
			auto fileSysFactory = reinterpret_cast<CreateInterfaceFn>(
				GetProcAddress(hFileSys, "CreateInterface"));

			if (!fileSysFactory)
			{
				LogLine("[2DSpeedProxy] FAILED to get filesystem_stdio.dll's CreateInterface");
			}
			else
			{
				int iFileSysReturnCode = -999;
				g_pFileSystem = static_cast<IFileSystem*>(
					fileSysFactory(FILESYSTEM_INTERFACE_VERSION, &iFileSysReturnCode));

				if (!g_pFileSystem)
					LogLine("[2DSpeedProxy] FAILED to get IFileSystem (version \"%s\", code %d)",
						FILESYSTEM_INTERFACE_VERSION, iFileSysReturnCode);
				else
					LogLine("[2DSpeedProxy] Got filesystem: %p", g_pFileSystem);
			}
		}

		// Get material system (from materialsystem.dll)
		HMODULE hMatSys = GetModuleHandleA("materialsystem.dll");
		if (!hMatSys)
		{
			LogLine("[2DSpeedProxy] FAILED to get materialsystem.dll module handle");
			return true;
		}

		auto matSysFactory = reinterpret_cast<CreateInterfaceFn>(
			GetProcAddress(hMatSys, "CreateInterface"));

		if (!matSysFactory)
		{
			LogLine("[2DSpeedProxy] FAILED to get materialsystem.dll's CreateInterface");
			return true;
		}

		// VMaterialSystem080 is the real version in L4D2 (SDK header says VMaterialSystem079 ¯\_(ツ)_/¯)
		const char* pszMatSysVersion = "VMaterialSystem080";

		int iMatSysReturnCode = -999;
		g_pMatSys = static_cast<IMaterialSystem*>(
			matSysFactory(pszMatSysVersion, &iMatSysReturnCode));

		if (!g_pMatSys)
		{
			LogLine("[2DSpeedProxy] FAILED to get IMaterialSystem (version \"%s\", code %d)",
				pszMatSysVersion, iMatSysReturnCode);
			return true;
		}

		LogLine("[2DSpeedProxy] Got material system: %p", g_pMatSys);

		// Install proxy factory (only once)
		if (!g_pProxyFactory)
		{
			g_pOldFactory = g_pMatSys->GetMaterialProxyFactory();
			LogLine("[2DSpeedProxy] Old factory: %p", g_pOldFactory);

			g_pProxyFactory = new CProxyFactory();
			g_pProxyFactory->SetOriginalFactory(g_pOldFactory);
			g_pMatSys->SetMaterialProxyFactory(g_pProxyFactory);
			LogLine("[2DSpeedProxy] Installed proxy factory: %p", g_pProxyFactory);

			// IMPORTANT: materials that were already precached (menu, level load,
			// HUD sprites loaded early) had their proxy list built against the OLD
			// factory before we ever got a chance to install ours - swapping the
			// factory pointer alone does NOT retroactively rebuild proxies on
			// already-loaded IMaterial instances.
			//
			// We do NOT attempt a forced reload here:
			//   - g_pMatSys->ReloadMaterials() crashes (ESP/RTC #0) because our
			//     local imaterialsystem.h is versioned "VMaterialSystem079" while
			//     L4D2's actual factory exposes "VMaterialSystem080" - a
			//     different, incompatible vtable layout.
			//   - The console command fallback ("mat_reloadmaterials") doesn't
			//     exist in this build ("Unknown command") - likely compiled out
			//     as dev-only in retail L4D2.
			//
			// Instead: make sure this plugin is loaded (plugin_load) BEFORE
			// joining/loading any map, ideally right at the main menu with
			// nothing connected. That way every HUD material (your gauge,
			// nOaimbot's velometer, the quake-speedo, etc.) gets created and
			// precached for the FIRST time already pointed at our factory - no
			// retroactive rebind required.
		}

		ConColorMsg({ 0, 255, 255, 255 }, "[2DSpeedProxy] Successfully loaded\n");
		return true;
	}

	void Unload() override
	{
		LogLine("[2DSpeedProxy] Unload() called");
		if (g_pMatSys && g_pOldFactory)
		{
			g_pMatSys->SetMaterialProxyFactory(g_pOldFactory);
			LogLine("[2DSpeedProxy] Restored old factory");
		}
		if (g_pProxyFactory)
		{
			delete g_pProxyFactory;
			g_pProxyFactory = nullptr;
		}
	}

	void Pause() override {}
	void UnPause() override {}
	const char* GetPluginDescription() override { return "L4D2_2DVel [SpeedProxy] v1.1 by DXSamXD"; }
	void LevelInit(char const* map) override {}
	void ServerActivate(edict_t* edicts, int count, int max) override {}

	void GameFrame(bool sim) override
	{
		if (g_iVelocityOffset < 0 && g_pClientDLL && g_iResolveAttempts < MAX_RESOLVE_ATTEMPTS)
		{
			g_iResolveAttempts++;
			LogLine("[2DSpeedProxy] Retrying offset resolution from GameFrame (attempt %d/%d)",
				g_iResolveAttempts, MAX_RESOLVE_ATTEMPTS);

			if (ResolveVelocityOffset(g_pClientDLL))
			{
				LogLine("[2DSpeedProxy] Successfully resolved offset in GameFrame!");
			}
			else if (g_iResolveAttempts >= MAX_RESOLVE_ATTEMPTS)
			{
				// L4D2 CTerrorPlayer m_vecVelocity is at offset 0x100 (confirmed via runtime scan)
				g_iVelocityOffset = 0x100;
				LogLine("[2DSpeedProxy] Gave up on dynamic resolution, using hardcoded fallback offset: 0x%X", g_iVelocityOffset);
			}
		}
	}

	void LevelShutdown() override {}
	void ClientActive(edict_t* ent) override {}
	void ClientDisconnect(edict_t* ent) override {}
	void ClientPutInServer(edict_t* ent, char const* name) override {}
	void SetCommandClient(int idx) override {}
	void ClientSettingsChanged(edict_t* ent) override {}
	PLUGIN_RESULT ClientConnect(bool* allow, edict_t* ent, const char* name, const char* addr, char* reject, int maxlen) override { return PLUGIN_CONTINUE; }
	PLUGIN_RESULT ClientCommand(edict_t* ent, const CCommand& args) override { return PLUGIN_CONTINUE; }
	PLUGIN_RESULT NetworkIDValidated(const char* name, const char* id) override { return PLUGIN_CONTINUE; }
	void OnQueryCvarValueFinished(QueryCvarCookie_t id, edict_t* ent, EQueryCvarValueStatus status, const char* name, const char* val) override {}
};

static CPlugin g_plugin;

extern "C" __declspec(dllexport) void* __cdecl CreateInterface(const char* name, int* code)
{
	LogLine("[2DSpeedProxy] CreateInterface: %s", name);
	if (strcmp(name, INTERFACEVERSION_ISERVERPLUGINCALLBACKS) == 0)
	{
		if (code) *code = 0;
		return &g_plugin;
	}
	if (code) *code = 1;
	return nullptr;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		LogLine("[2DSpeedProxy] DLL_PROCESS_ATTACH");
	}
	else if (reason == DLL_PROCESS_DETACH)
	{
		// Per MSDN: if lpvReserved (here: `reserved`) is non-NULL, the process
		// is terminating (not a clean FreeLibrary call) - other DLLs, including
		// tier0.dll, may already be unloaded/torn down at this point, so it is
		// unsafe to touch them (or do any cleanup at all). Just return
		// immediately in that case; the OS reclaims everything anyway.
		if (reserved != nullptr)
		{
			return TRUE;
		}
		LogLine("[2DSpeedProxy] DLL_PROCESS_DETACH (clean unload)");
	}
	return TRUE;
}