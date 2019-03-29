/**
 * MaNGOS is a full featured server for World of Warcraft, supporting
 * the following clients: 1.12.x, 2.4.3, 3.3.5a, 4.3.4a and 5.4.8
 *
 * Copyright (C) 2005-2019  MaNGOS project <https://getmangos.eu>
 * Copyright (C) 2008-2015 TrinityCore <http://www.trinitycore.org/>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * World of Warcraft, and all World of Warcraft or Warcraft art, images,
 * and lore are copyrighted by Blizzard Entertainment, Inc.
 */

#include "Common.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Log.h"
#include "Opcodes.h"
#include "Player.h"
#include "ByteBuffer.h"
#include <openssl/sha.h>
#include "World.h"
#include "Util.h"
#include "Warden.h"
#include "AccountMgr.h"

Warden::Warden() : _session(NULL), _inputCrypto(16), _outputCrypto(16), _checkTimer(10000/*10 sec*/), _clientResponseTimer(0),
                   _dataSent(false), _previousTimestamp(0), _module(NULL), _initialized(false)
{
    memset(_inputKey, 0, sizeof(_inputKey));
    memset(_outputKey, 0, sizeof(_outputKey));
    memset(_seed, 0, sizeof(_seed));
}

Warden::~Warden()
{
    delete[] _module->CompressedData;
    delete _module;
    _module = NULL;
    _initialized = false;
}

void Warden::RequestHash()
{
    sLog.outWarden("Request hash");

    // Create packet structure
    WardenHashRequest Request;
    Request.Command = WARDEN_SMSG_HASH_REQUEST;
    memcpy(Request.Seed, _seed, 16);

    // Encrypt with warden RC4 key.
    EncryptData((uint8*)&Request, sizeof(WardenHashRequest));

    WorldPacket pkt(SMSG_WARDEN_DATA, sizeof(WardenHashRequest));
    pkt.append((uint8*)&Request, sizeof(WardenHashRequest));
    _session->SendPacket(&pkt);
}

void Warden::SendModuleToClient()
{
    sLog.outWarden("Send module to client");

    // Create packet structure
    WardenModuleTransfer packet;

    uint32 sizeLeft = _module->CompressedSize;
    uint32 pos = 0;
    uint16 burstSize;
    while (sizeLeft > 0)
    {
        burstSize = sizeLeft < 500 ? sizeLeft : 500;
        packet.Command = WARDEN_SMSG_MODULE_CACHE;
        packet.DataSize = burstSize;
        memcpy(packet.Data, &_module->CompressedData[pos], burstSize);
        sizeLeft -= burstSize;
        pos += burstSize;

        EncryptData((uint8*)&packet, burstSize + 3);
        WorldPacket pkt1(SMSG_WARDEN_DATA, burstSize + 3);
        pkt1.append((uint8*)&packet, burstSize + 3);
        _session->SendPacket(&pkt1);
    }
}

void Warden::RequestModule()
{
    sLog.outWarden("Request module");

    // Create packet structure
    WardenModuleUse request;
    request.Command = WARDEN_SMSG_MODULE_USE;

    memcpy(request.ModuleId, _module->Id, 16);
    memcpy(request.ModuleKey, _module->Key, 16);
    request.Size = _module->CompressedSize;

    // Encrypt with warden RC4 key.
    EncryptData((uint8*)&request, sizeof(WardenModuleUse));

    WorldPacket pkt(SMSG_WARDEN_DATA, sizeof(WardenModuleUse));
    pkt.append((uint8*)&request, sizeof(WardenModuleUse));
    _session->SendPacket(&pkt);
}

void Warden::Update()
{
    if (_initialized)
    {
        uint32 currentTimestamp = WorldTimer::getMSTime();
        uint32 diff = currentTimestamp - _previousTimestamp;
        _previousTimestamp = currentTimestamp;

        if (_dataSent)
        {
            uint32 maxClientResponseDelay = sWorld.getConfig(CONFIG_UINT32_WARDEN_CLIENT_RESPONSE_DELAY);

            if (maxClientResponseDelay > 0)
            {
                // Kick player if client response delays more than set in config
                if (_clientResponseTimer > maxClientResponseDelay * IN_MILLISECONDS)
                {
                    sLog.outWarden("%s (latency: %u, IP: %s) exceeded Warden module response delay for more than %s - disconnecting client",
                        _session->GetPlayerName(), _session->GetLatency(), _session->GetRemoteAddress().c_str(), secsToTimeString(maxClientResponseDelay, true).c_str());
                    _session->KickPlayer();
                }
                else
                    _clientResponseTimer += diff;
            }
        }
        else
        {
            if (diff >= _checkTimer)
            {
                RequestData();
            }
            else
                _checkTimer -= diff;
        }
    }
}

void Warden::DecryptData(uint8* buffer, uint32 length)
{
    _inputCrypto.UpdateData(length, buffer);
}

void Warden::EncryptData(uint8* buffer, uint32 length)
{
    _outputCrypto.UpdateData(length, buffer);
}

bool Warden::IsValidCheckSum(uint32 checksum, const uint8* data, const uint16 length)
{
    uint32 newChecksum = BuildChecksum(data, length);

    if (checksum != newChecksum)
    {
        sLog.outWarden("CHECKSUM IS NOT VALID");
        return false;
    }
    else
    {
        sLog.outWarden("CHECKSUM IS VALID");
        return true;
    }
}

struct keyData {
    union
    {
        struct
        {
            uint8 bytes[20];
        } bytes;

        struct
        {
            uint32 ints[5];
        } ints;
    };
};

uint32 Warden::BuildChecksum(const uint8* data, uint32 length)
{
    keyData hash;
    SHA1(data, length, hash.bytes.bytes);
    uint32 checkSum = 0;
    for (uint8 i = 0; i < 5; ++i)
        checkSum = checkSum ^ hash.ints.ints[i];

    return checkSum;
}

std::string Warden::Penalty(WardenCheck* check /*= NULL*/)
{
    WardenActions action;

    if (check)
        action = check->Action;
    else
        action = WardenActions(sWorld.getConfig(CONFIG_UINT32_WARDEN_CLIENT_FAIL_ACTION));

    switch (action)
    {
    case WARDEN_ACTION_LOG:
        return "None";
        break;
    case WARDEN_ACTION_KICK:
        _session->KickPlayer();
        return "Kick";
        break;
    case WARDEN_ACTION_BAN:
        {
            std::stringstream duration;
            std::string accountName;
            sAccountMgr.GetName(_session->GetAccountId(), accountName);
            std::stringstream banReason;
            banReason << "Warden Anticheat Violation";
            // Check can be NULL, for example if the client sent a wrong signature in the warden packet (CHECKSUM FAIL)
            if (check)
                banReason << ": " << check->Comment << " (CheckId: " << check->CheckId << ")";

            sWorld.BanAccount(BAN_ACCOUNT, accountName, sWorld.getConfig(CONFIG_UINT32_WARDEN_CLIENT_BAN_DURATION), banReason.str(), "Warden");

            return "Ban";
        }
    default:
        break;
    }
    return "Undefined";
}

void WorldSession::HandleWardenDataOpcode(WorldPacket& recvData)
{
    if (!_warden || recvData.empty())
        return;

    _warden->DecryptData(const_cast<uint8*>(recvData.contents()), recvData.size());
    uint8 opcode;
    recvData >> opcode;
    sLog.outWarden("Got packet, opcode %02X, size %u", opcode, uint32(recvData.size()));
    recvData.hexlike();

    switch (opcode)
    {
        case WARDEN_CMSG_MODULE_MISSING:
            _warden->SendModuleToClient();
            break;
        case WARDEN_CMSG_MODULE_OK:
            _warden->RequestHash();
            break;
        case WARDEN_CMSG_CHEAT_CHECKS_RESULT:
            _warden->HandleData(recvData);
            break;
        case WARDEN_CMSG_MEM_CHECKS_RESULT:
            sLog.outWarden("NYI WARDEN_CMSG_MEM_CHECKS_RESULT received!");
            break;
        case WARDEN_CMSG_HASH_RESULT:
            _warden->HandleHashResult(recvData);
            _warden->InitializeModule();
            break;
        case WARDEN_CMSG_MODULE_FAILED:
            sLog.outWarden("NYI WARDEN_CMSG_MODULE_FAILED received!");
            break;
        default:
            sLog.outWarden("Got unknown warden opcode %02X of size %u.", opcode, uint32(recvData.size() - 1));
            break;
    }
}

void Warden::HandleData(ByteBuffer& /*buff*/)
{
    // Set hold off timer, minimum timer should at least be 1 second
    uint32 holdOff = sWorld.getConfig(CONFIG_UINT32_WARDEN_CLIENT_CHECK_HOLDOFF);
    _checkTimer = (holdOff < 1 ? 1 : holdOff) * IN_MILLISECONDS;
}

void Warden::LogPositiveToDB(WardenCheck* check)
{
    if (!check || !_session)
        return;

    if (uint32(check->Action) < sWorld.getConfig(CONFIG_UINT32_WARDEN_DB_LOGLEVEL))
        return;

    static SqlStatementID insWardenPositive;

    SqlStatement stmt = LoginDatabase.CreateStatement(insWardenPositive, "INSERT INTO warden_log (`check`, `action`, `account`, `guid`, `map`, `position_x`, `position_y`, `position_z`) VALUES (?, ?, ?, ?, ?, ?, ?, ?)");

    stmt.addUInt16(check->CheckId);
    stmt.addInt8(check->Action);
    stmt.addUInt32(_session->GetAccountId());
    if (Player* pl = _session->GetPlayer())
    {
        stmt.addUInt64(pl->GetObjectGuid().GetRawValue());
        stmt.addUInt32(pl->GetMapId());
        stmt.addFloat(pl->GetPositionX());
        stmt.addFloat(pl->GetPositionY());
        stmt.addFloat(pl->GetPositionZ());
    }
    else
    {
        stmt.addUInt64(0);
        stmt.addUInt32(0xFFFFFFFF);
        stmt.addFloat(0.0f);
        stmt.addFloat(0.0f);
        stmt.addFloat(0.0f);
    }
    stmt.Execute();
}
