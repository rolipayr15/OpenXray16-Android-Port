#include "stdafx.h"
#include "GameSpy_Available.h"
#include "GameSpy_MasterServer.h"

bool CGameSpy_Available::CheckAvailableServices(shared_str& resultstr)
{
    if (IsGameSpyMasterServerConfigured())
    {
        // ServerBrowserNew/GP/QR2 are local SDK objects, but the legacy SDK refuses
        // to construct them until its global availability flag is set.  Our custom
        // master does its own availability check, so the retired GameSpy probe is
        // intentionally bypassed here.
        __GSIACResult = GSIACAvailable;

        if (CheckGameSpyMasterServer())
        {
            resultstr = "Success";
            return true;
        }

        // Keep the browser alive so a later refresh can retry and report
        // MasterUnreachable instead of leaving a half-created GameSpy facade.
        resultstr = "! Configured OpenXRay master server is currently unreachable; refresh will retry.";
        return true;
    }

    GSIACResult result;
    GSIStartAvailableCheckA(GAMESPY_GAMENAME_SOC);

    while ((result = GSIAvailableCheckThink()) == GSIACWaiting)
        msleep(5);

    if (result != GSIACAvailable)
    {
        switch (result)
        {
        case GSIACUnavailable: { resultstr = "! Online Services for STALKER are no longer available.";
        }
        break;
        case GSIACTemporarilyUnavailable:
        {
            resultstr = "! Online Services for STALKER are temporarily down for maintenance.";
        }
        break;
        }
        return false;
    }
    else
    {
        resultstr = "Success";
    };
    return true;
};
