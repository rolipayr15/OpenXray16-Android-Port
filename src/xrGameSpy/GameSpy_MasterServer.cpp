#include "stdafx.h"
#include "GameSpy_MasterServer.h"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
constexpr size_t MaxHostLength = 253;
constexpr size_t MaxResponseLength = 64 * 1024;
constexpr const char* ProtocolGame = "stalkersc";

std::array<char, MaxHostLength + 1> g_masterHost{};
unsigned short g_masterPort = 28910;

void SetSocketTimeout(SOCKET socketHandle, int milliseconds)
{
#ifdef _WIN32
    const DWORD timeout = static_cast<DWORD>(milliseconds);
    setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(socketHandle, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval timeout{};
    timeout.tv_sec = milliseconds / 1000;
    timeout.tv_usec = (milliseconds % 1000) * 1000;
    setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(socketHandle, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
}

bool ResolveMaster(sockaddr_in& address)
{
    if (!IsGameSpyMasterServerConfigured())
        return false;

    SocketStartUp();
    HOSTENT* host = gethostbyname(g_masterHost.data());
    if (!host || !host->h_addr_list || !host->h_addr_list[0])
        return false;

    address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(g_masterPort);
    std::memcpy(&address.sin_addr, host->h_addr_list[0], sizeof(address.sin_addr));
    return true;
}

SOCKET ConnectMasterTcp()
{
    sockaddr_in address{};
    if (!ResolveMaster(address))
        return INVALID_SOCKET;

    SOCKET socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socketHandle == INVALID_SOCKET)
        return INVALID_SOCKET;

    SetSocketTimeout(socketHandle, 3000);
    if (connect(socketHandle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
    {
        closesocket(socketHandle);
        return INVALID_SOCKET;
    }
    return socketHandle;
}

bool SendAll(SOCKET socketHandle, const char* data, size_t length)
{
    size_t offset = 0;
    while (offset < length)
    {
        const int sent = send(socketHandle, data + offset, static_cast<int>(length - offset), 0);
        if (sent <= 0)
            return false;
        offset += static_cast<size_t>(sent);
    }
    return true;
}

bool ReadResponse(SOCKET socketHandle, std::string& response)
{
    std::array<char, 2048> buffer{};
    while (response.size() < MaxResponseLength)
    {
        const int received = recv(socketHandle, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (received == 0)
            return true;
        if (received < 0)
            return !response.empty();
        response.append(buffer.data(), static_cast<size_t>(received));
        if (response.find("\n.\n") != std::string::npos)
            return true;
    }
    return false;
}

bool IsValidIPv4Text(const std::string& value)
{
    sockaddr_in parsed{};
    return inet_pton(AF_INET, value.c_str(), &parsed.sin_addr) == 1;
}
}

void ConfigureGameSpyMasterServer(const char* host, unsigned short port)
{
    g_masterHost.fill('\0');
    if (host && host[0])
    {
        std::strncpy(g_masterHost.data(), host, MaxHostLength);
        g_masterHost[MaxHostLength] = '\0';
    }
    g_masterPort = port ? port : 28910;
}

bool IsGameSpyMasterServerConfigured() { return g_masterHost[0] != '\0'; }
const char* GetGameSpyMasterServerHost() { return g_masterHost.data(); }
unsigned short GetGameSpyMasterServerPort() { return g_masterPort; }

bool CheckGameSpyMasterServer()
{
    SOCKET socketHandle = ConnectMasterTcp();
    if (socketHandle == INVALID_SOCKET)
        return false;

    constexpr char request[] = "PING\n";
    std::string response;
    const bool success = SendAll(socketHandle, request, sizeof(request) - 1) &&
        ReadResponse(socketHandle, response) && response.rfind("PONG\n", 0) == 0;
    closesocket(socketHandle);
    return success;
}

bool QueryGameSpyMasterServer(GameSpyMasterEndpointCallback callback, void* context)
{
    SOCKET socketHandle = ConnectMasterTcp();
    if (socketHandle == INVALID_SOCKET)
        return false;

    constexpr char request[] = "LIST stalkersc\n";
    std::string response;
    const bool received = SendAll(socketHandle, request, sizeof(request) - 1) && ReadResponse(socketHandle, response);
    closesocket(socketHandle);
    if (!received || response.rfind("XRAYMASTER 1\n", 0) != 0)
        return false;

    size_t cursor = response.find('\n') + 1;
    while (cursor < response.size())
    {
        const size_t end = response.find('\n', cursor);
        if (end == std::string::npos)
            break;
        const std::string line = response.substr(cursor, end - cursor);
        cursor = end + 1;
        if (line == ".")
            return true;

        const size_t separator = line.find(' ');
        if (separator == std::string::npos)
            return false;
        const std::string address = line.substr(0, separator);
        const std::string portText = line.substr(separator + 1);
        char* portEnd = nullptr;
        const long port = std::strtol(portText.c_str(), &portEnd, 10);
        if (!IsValidIPv4Text(address) || !portEnd || *portEnd != '\0' || port < 1 || port > 65535)
            return false;
        if (callback)
            callback(address.c_str(), static_cast<unsigned short>(port), context);
    }
    return false;
}

bool SendGameSpyMasterRegistration(unsigned short queryPort, bool unregisterServer)
{
    if (!IsGameSpyMasterServerConfigured() || queryPort == 0)
        return false;

    sockaddr_in address{};
    if (!ResolveMaster(address))
        return false;

    SOCKET socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socketHandle == INVALID_SOCKET)
        return false;
    SetSocketTimeout(socketHandle, 1000);

    char packet[96]{};
    xr_sprintf(packet, "%s %s %hu\n", unregisterServer ? "UNREGISTER" : "REGISTER", ProtocolGame, queryPort);
    const int sent = sendto(socketHandle, packet, static_cast<int>(std::strlen(packet)), 0,
        reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    closesocket(socketHandle);
    return sent == static_cast<int>(std::strlen(packet));
}
