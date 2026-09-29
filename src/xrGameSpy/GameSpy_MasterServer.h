#pragma once

#include "xrGameSpy/xrGameSpy_API.h"

using GameSpyMasterEndpointCallback = void (*)(const char* address, unsigned short queryPort, void* context);

// An empty host keeps the original GameSpy-compatible backend path enabled.
XRGAMESPY_API void ConfigureGameSpyMasterServer(const char* host, unsigned short port);
XRGAMESPY_API bool IsGameSpyMasterServerConfigured();
XRGAMESPY_API const char* GetGameSpyMasterServerHost();
XRGAMESPY_API unsigned short GetGameSpyMasterServerPort();

XRGAMESPY_API bool CheckGameSpyMasterServer();
XRGAMESPY_API bool QueryGameSpyMasterServer(GameSpyMasterEndpointCallback callback, void* context);
XRGAMESPY_API bool SendGameSpyMasterRegistration(unsigned short queryPort, bool unregisterServer);
