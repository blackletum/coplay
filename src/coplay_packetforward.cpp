﻿/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

//================================================
// CoaXioN Source SDK p2p networking: "CoaXioN Coplay"
// Author : Tholp / Jackson S
// File Last Modified : Mar 26 2024
//================================================

#include "cbase.h"
#include "coplay.h"

#ifdef WIN32
#include "Windows.h"
void usleep(unsigned int usec)
{
    HANDLE timer;
    LARGE_INTEGER ft;

    ft.QuadPart = -(10 * (__int64)usec);

    timer = CreateWaitableTimer(NULL, TRUE, NULL);
    SetWaitableTimer(timer, &ft, 0, NULL, NULL, 0);
    WaitForSingleObject(timer, INFINITE);
    CloseHandle(timer);
}
#endif

ConVar coplay_timeoutduration("coplay_timeoutduration", "45");

ConVar coplay_debuglog_socketspam("coplay_debuglog_socketspam", "0");


int CCoplayConnection::Run()
{
    UDPpacket **LocalInboundPackets = SDLNet_AllocPacketV(COPLAY_MAX_PACKETS + 1, 1500);//normal ethernet MTU size
    LocalInboundPackets[COPLAY_MAX_PACKETS - 1 ] = NULL;

    SteamNetworkingMessage_t *InboundSteamMessages[COPLAY_MAX_PACKETS];
    //UDPpacket *SteamPacket = SDLNet_AllocPacket(1);// we dont actually use the buffer of this one
    UDPpacket SteamPacket;

    int numSDLRecv;
    int numSteamRecv;

    int64 messageOut;
    while(!DeletionQueued)
    {
        if (LocalSocket == NULL ||  SteamConnection == 0)
        {
            Warning("[Coplay Warning] A registered Coplay socket was invalid! Deleting.\n");
            DeletionQueued = true;
            continue;
        }

        usleep(g_pCoplayConnectionHandler->usSleepTime);//dont work too hard

        //Outbound to SDR
        numSDLRecv = SDLNet_UDP_RecvV(LocalSocket, LocalInboundPackets);
        if (numSDLRecv > 0)
        {
            if(coplay_debuglog_socketspam.GetBool())
                ConColorMsg(COPLAY_DEBUG_MSG_COLOR, "[Coplay Debug] SDL %i\n", numSDLRecv);
        }
        if (numSDLRecv == -1)
            ConColorMsg(COPLAY_DEBUG_MSG_COLOR, "[Coplay Debug] SDL Error! %s\n", SDLNet_GetError());


        for (uint8 j = 0; j < numSDLRecv; j++)
        {
            // OutboundSteamMessages[j] = SteamNetworkingUtils()->AllocateMessage(LocalInboundPackets[j]->len);
            // OutboundSteamMessages[j]->m_identityPeer
            //EResult result =
            SteamNetworkingSockets()->SendMessageToConnection(SteamConnection, (const void*)LocalInboundPackets[j]->data,
                                                                            LocalInboundPackets[j]->len,
                                                                            k_nSteamNetworkingSend_UnreliableNoDelay | k_nSteamNetworkingSend_UseCurrentThread,
                                                                            &messageOut);
            //if (coplay_debuglog_socketspam.GetBool())
            //    ConColorMsg(COPLAY_DEBUG_MSG_COLOR, "[Coplay Debug] Result %i\n", result);
        }

        //Inbound from SDR

        numSteamRecv = SteamNetworkingSockets()->ReceiveMessagesOnConnection(SteamConnection, InboundSteamMessages, sizeof(InboundSteamMessages));

        if (numSteamRecv > 0 || engine->IsConnected())
        {
            if (coplay_debuglog_socketspam.GetBool())
                ConColorMsg(COPLAY_DEBUG_MSG_COLOR, "[Coplay Debug] Steam %i\n", numSteamRecv);
            LastPacketTime = gpGlobals->realtime;
        }

        for (uint8 j = 0; j < numSteamRecv; j++)
        {
            SteamPacket.data = (uint8*)InboundSteamMessages[j]->GetData();
            SteamPacket.len  = InboundSteamMessages[j]->GetSize();

            if (!SDLNet_UDP_Send(LocalSocket, 1, &SteamPacket))
                ConColorMsg(COPLAY_DEBUG_MSG_COLOR, "[Coplay Debug] Wasnt sent! %s\n", SDLNet_GetError());
        }

        for (uint8 j = 0; j < numSteamRecv; j++)
            InboundSteamMessages[j]->Release();

        if (LastPacketTime + coplay_timeoutduration.GetFloat() < gpGlobals->realtime)
            QueueForDeletion();
    }
    //Cleanup

    SDLNet_FreePacketV(LocalInboundPackets);
    return 0;
}

void CCoplayConnection::OnExit()
{
    SDLNet_UDP_Close(LocalSocket);
    SteamNetworkingSockets()->CloseConnection(SteamConnection, k_ESteamNetConnectionEnd_App_ClosedByPeer, "", false);
    g_pCoplayConnectionHandler->Connections.FindAndRemove(this);

    if (coplay_debuglog_socketcreation.GetBool())
        ConColorMsg(COPLAY_DEBUG_MSG_COLOR, "[Coplay Debug] Socket closed with port %i.\n", Port);
}

CCoplayConnection::CCoplayConnection(HSteamNetConnection hConn)
{
    SteamConnection = hConn;
    LastPacketTime = gpGlobals->realtime;

    int timeout = 50;

    while (timeout > 0)// TODO: Should probably change this..
    {
        int port = RandomInt(26000, 65535);
        UDPsocket sock = SDLNet_UDP_Open(port);
        if (sock)
        {
            LocalSocket = sock;
            Port = port;
            break;
        }
        timeout--;
    }

    if (timeout == 0)
    {
        Warning("[Coplay Error] What do you need all those ports for anyway? (Couldn't bind to a port on range 26000-65535 after 50 retries!)\n");
        //return false;
    }

    IPaddress addr;
    addr.host = 0;
    IPaddress localaddresses[16];
    int numlocal = SDLNet_GetLocalAddresses(localaddresses, sizeof(localaddresses)/sizeof(IPaddress));

    for (int i = 0; i < numlocal; i++)
    {
        if (localaddresses[i].host == 0)
            continue;
        uint8 firstoctet = ((uint8*)&localaddresses[i].host)[0];
        if (firstoctet == 127 || firstoctet == 172)//|| firstoctet == 192
            continue;
        addr.host = localaddresses[i].host;
    }
    if (addr.host == 0)
    {
        Warning("[Coplay Warning] Didn't find a suitable local address! Trying loopback..\n");
        addr.host = SwapEndian32(INADDR_LOOPBACK);
    }

    if (g_pCoplayConnectionHandler->GetRole() == eConnectionRole_CLIENT)
        addr.port = SwapEndian16(27005);//SDLNet wants these in network byte order
    else
        addr.port = SwapEndian16(27015);// default server port, check for this proper later
    SDLNet_UDP_Bind(LocalSocket, 1, &addr);// "Inbound" Channel
    SendbackAddress = addr;

    if (coplay_debuglog_socketcreation.GetBool())
    {
        ConColorMsg(COPLAY_DEBUG_MSG_COLOR, "[Coplay Debug] New socket : %u\n", Port);
        //ConColorMsg(COPLAY_DEBUG_MSG_COLOR, "[Coplay Debug] New socket : %i:%i\n", SDLNet_UDP_GetPeerAddress(tuple->LocalSocket, 0)->host, SDLNet_UDP_GetPeerAddress(tuple->LocalSocket, 0)->port);
    }
    char threadname[32];
    V_snprintf(threadname, sizeof(threadname), "coplayconnection%i", Port);
    SetName(threadname);
}

