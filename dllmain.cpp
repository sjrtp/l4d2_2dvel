#include "pch.h"
#include <windows.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdarg>

#include "eiface.h"
#include "engine/iserverplugin.h"
#include "cdll_int.h"
#include "icliententitylist.h"
#include "icliententity.h"
#include "iclientunknown.h"
#include "client_class.h"
#include "materialsystem/imaterialsystem.h"
#include "materialsystem/imaterial.h"
#include "materialsystem/imaterialvar.h"
#include "materialsystem/imaterialproxy.h"
#include "materialsystem/imaterialproxyfactory.h"

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
static IVEngineClient* g_pEngine = nullptr;
static IClientEntityList* g_pEntityList = nullptr;
static IBaseClientDLL* g_pClientDLL = nullptr;
static int g_iVelocityOffset = -1;
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

// Resolve m_vecVelocity offset at runtime by walking client RecvTable
static bool ResolveVelocityOffset(IBaseClientDLL* pClientDLL)
{
	if (!pClientDLL)
	{
		LogLine("[2DSpeedProxy] ResolveVelocityOffset called with NULL pClientDLL!");
		return false;
	}

	ClientClass* pClass = pClientDLL->GetAllClasses();

	if (!pClass)
	{
		LogLine("[2DSpeedProxy] GetAllClasses() returned NULL pointer");
		return false;
	}

	while (pClass)
	{
		if (pClass->m_pNetworkName && strcmp(pClass->m_pNetworkName, "CTerrorPlayer") == 0)
		{
			int offset = 0;
			if (FindRecvPropOffset(pClass->m_pRecvTable, "m_vecVelocity", 0, offset))
			{
				if (offset < 0 || offset > 65536)
				{
					LogLine("[2DSpeedProxy] Resolved m_vecVelocity to an implausible offset = %d -- rejecting it", offset);
					return false;
				}
				g_iVelocityOffset = offset;
				LogLine("[2DSpeedProxy] Resolved m_vecVelocity offset = %d (client-side RecvTable)", offset);
				return true;
			}
			LogLine("[2DSpeedProxy] Found CTerrorPlayer but couldn't find m_vecVelocity in its RecvTable");
			return false;
		}
		pClass = pClass->m_pNext;
	}

	// CTerrorPlayer not found, dump what we got
	int iCount = 0;
	pClass = pClientDLL->GetAllClasses();
	while (pClass)
	{
		LogLine("[2DSpeedProxy]   class[%d] = \"%s\"", iCount,
			pClass->m_pNetworkName ? pClass->m_pNetworkName : "(null name)");
		pClass = pClass->m_pNext;
		iCount++;
	}
	LogLine("[2DSpeedProxy] Could not find CTerrorPlayer client class -- dumped %d classes above", iCount);
	return false;
}

// === Velocity Calculation with Smoothing ===
// Helper: detect if player is on a ladder (check if significant Z velocity with low XY velocity)
static bool IsOnLadder(const Vector3D* pVel)
{
	if (!pVel) return false;
	float flHorizontal = std::sqrt(pVel->x * pVel->x + pVel->y * pVel->y);
	float flVertical = std::abs(pVel->z);

	// On ladder: EITHER very high vertical velocity (350+) with low horizontal (<50)
	// OR standing still with any upward movement (horizontal <25 and vertical >10)
	// This prevents normal jump/bhop from triggering 3D calculation
	return (flVertical > 350.0f && flHorizontal < 50.0f) || (flVertical > 10.0f && flHorizontal < 25.0f);
}

static float ComputeSmoothedLocalPlayerSpeed2D()
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
					if (pEntity)
					{
						// Cast to char* for pointer arithmetic, then cast to Vector3D
						const char* pRaw = reinterpret_cast<const char*>(pEntity);
						const Vector3D* pVel = reinterpret_cast<const Vector3D*>(pRaw + g_iVelocityOffset);

						// Always use 2D for bhop, 3D only on ladders
						bool bOnLadder = IsOnLadder(pVel);
						if (!bOnLadder)
							flRaw = pVel->Length2D();  // Only horizontal velocity for bhop (2D mode)
						else
							flRaw = pVel->Length3D();  // Full 3D velocity on ladder

						bWasOnLadder = bOnLadder;
					}
				}
			}
		}
	} catch (...) {
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

// PlayerSpeed material proxy
class CSpeedProxy : public IMaterialProxy
{
private:
	IMaterialVar* m_pSpeedVar = nullptr;
	static float flLastDisplayed;
	static int iFramesAtLow;
	static const int FRAMES_UNTIL_DECREASE = 2;  // Only show decrease if it lasts 2+ frames

public:
	bool Init(IMaterial* pMaterial, KeyValues* pKeyValues) override
	{
		m_pSpeedVar = pMaterial->FindVar("$speed", nullptr);
		return m_pSpeedVar != nullptr;
	}

	void OnBind(void* pRenderable) override
	{
		if (!m_pSpeedVar) return;
		float flSmoothed = ComputeSmoothedLocalPlayerSpeed2D();
		float flRounded = std::round(flSmoothed);

		///////////////////////////////////////////////////////////////////////////////
		//                 Jump-apex dip suppression (display smoothing)             //
		// Filters out the fake 2-3 unit speed drop that happens at the peak of a    //
		// jump, so the displayed number doesn't flicker down when the player        //
		// hasn't actually slowed down.                                              //
		///////////////////////////////////////////////////////////////////////////////

		// Force very low velocities to 0 (idle/animation detection)
		// When velocity is sitting at 1-2 units, player is effectively stationary
		if (flRounded <= 2.0f)
		{
			flLastDisplayed = 0.0f;
			iFramesAtLow = 0;
		}
		// Hysteresis: only allow decreases if they persist AND are significant (5+ units)
		else if (flRounded > flLastDisplayed)
		{
			// Velocity increasing - update immediately
			flLastDisplayed = flRounded;
			iFramesAtLow = 0;
		}
		else if (flRounded < flLastDisplayed)
		{
			// Velocity decreasing - only update if decrease is significant (>= 3 units)
			float flDelta = flLastDisplayed - flRounded;
			if (flDelta >= 3.0f)
			{
				// Significant decrease - show it
				flLastDisplayed = flRounded;
				iFramesAtLow = 0;
			}
			else
			{
				// Minor dip (< 3 units) - ignore it
				iFramesAtLow++;
			}
		}
		else
		{
			// Same velocity - reset counter
			iFramesAtLow = 0;
		}

		m_pSpeedVar->SetFloatValue(flLastDisplayed);
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
int CSpeedProxy::iFramesAtLow = 0;

// Proxy factory
class CProxyFactory : public IMaterialProxyFactory
{
public:
	void SetOriginalFactory(IMaterialProxyFactory* pOriginal)
	{
		if (pOriginal == this)
		{
			LogLine("[2DSpeedProxy] WARNING: GetMaterialProxyFactory() returned ourselves -- not chaining");
			m_pOldFactory = nullptr;
			return;
		}
		m_pOldFactory = pOriginal;
	}

	IMaterialProxy* CreateProxy(const char* name) override
	{
		LogLine("[2DSpeedProxy] CreateProxy called: %s", name);
		if (name && strcmp(name, "PlayerSpeed") == 0)
		{
			LogLine("[2DSpeedProxy] Creating PlayerSpeed proxy");
			return new CSpeedProxy();
		}
		// Pass through to old factory for other proxies
		if (!m_pOldFactory)
			return nullptr;

		if (m_iRecursionDepth > 8)
		{
			LogLine("[2DSpeedProxy] WARNING: CreateProxy recursion depth exceeded for \"%s\"", name ? name : "(null)");
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
	const char* GetPluginDescription() override { return "L4D2_2DVel [SpeedProxy] v1.0 by DXSamXD"; }
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
	return TRUE;
}
