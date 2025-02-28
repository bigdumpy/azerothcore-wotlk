/*
 * Copyright (C) 2016+     AzerothCore <www.azerothcore.org>, released under GNU GPL v2 license, you may redistribute it and/or modify it under version 2 of the License, or (at your option), any later version.
 * Copyright (C) 2008-2016 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
 */

#include "WorldSocket.h"
#include "AccountMgr.h"
#include "BigNumber.h"
#include "CryptoHash.h"
#include "CryptoRandom.h"
#include "DatabaseEnv.h"
#include "IPLocation.h"
#include "Opcodes.h"
#include "PacketLog.h"
#include "Random.h"
#include "Realm.h"
#include "ScriptMgr.h"
#include "World.h"
#include "WorldSession.h"
#include <memory>

using boost::asio::ip::tcp;

WorldSocket::WorldSocket(tcp::socket&& socket)
    : Socket(std::move(socket)), _OverSpeedPings(0), _worldSession(nullptr), _authed(false), _sendBufferSize(4096)
{
    Acore::Crypto::GetRandomBytes(_authSeed);
    _headerBuffer.Resize(sizeof(ClientPktHeader));
}

WorldSocket::~WorldSocket() = default;

void WorldSocket::Start()
{
    std::string ip_address = GetRemoteIpAddress().to_string();

    LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_SEL_IP_INFO);
    stmt->setString(0, ip_address);

    _queryProcessor.AddCallback(LoginDatabase.AsyncQuery(stmt).WithPreparedCallback(std::bind(&WorldSocket::CheckIpCallback, this, std::placeholders::_1)));
}

void WorldSocket::CheckIpCallback(PreparedQueryResult result)
{
    if (result)
    {
        bool banned = false;
        do
        {
            Field* fields = result->Fetch();
            if (fields[0].GetUInt64() != 0)
                banned = true;

        } while (result->NextRow());

        if (banned)
        {
            SendAuthResponseError(AUTH_REJECT);
            LOG_ERROR("network", "WorldSocket::CheckIpCallback: Sent Auth Response (IP %s banned).", GetRemoteIpAddress().to_string().c_str());
            DelayedCloseSocket();
            return;
        }
    }

    AsyncRead();
    HandleSendAuthSession();
}

bool WorldSocket::Update()
{
    EncryptablePacket* queued;
    MessageBuffer buffer(_sendBufferSize);
    while (_bufferQueue.Dequeue(queued))
    {
        ServerPktHeader header(queued->size() + 2, queued->GetOpcode());
        if (queued->NeedsEncryption())
            _authCrypt.EncryptSend(header.header, header.getHeaderLength());

        if (buffer.GetRemainingSpace() < queued->size() + header.getHeaderLength())
        {
            QueuePacket(std::move(buffer));
            buffer.Resize(_sendBufferSize);
        }

        if (buffer.GetRemainingSpace() >= queued->size() + header.getHeaderLength())
        {
            buffer.Write(header.header, header.getHeaderLength());
            if (!queued->empty())
                buffer.Write(queued->contents(), queued->size());
        }
        else    // single packet larger than 4096 bytes
        {
            MessageBuffer packetBuffer(queued->size() + header.getHeaderLength());
            packetBuffer.Write(header.header, header.getHeaderLength());
            if (!queued->empty())
                packetBuffer.Write(queued->contents(), queued->size());

            QueuePacket(std::move(packetBuffer));
        }

        delete queued;
    }

    if (buffer.GetActiveSize() > 0)
        QueuePacket(std::move(buffer));

    if (!BaseSocket::Update())
        return false;

    _queryProcessor.ProcessReadyCallbacks();

    return true;
}

void WorldSocket::HandleSendAuthSession()
{
    WorldPacket packet(SMSG_AUTH_CHALLENGE, 40);
    packet << uint32(1);                                    // 1...31
    packet.append(_authSeed);

    packet.append(Acore::Crypto::GetRandomBytes<32>());               // new encryption seeds

    SendPacketAndLogOpcode(packet);
}

void WorldSocket::OnClose()
{
    {
        std::lock_guard<std::mutex> sessionGuard(_worldSessionLock);
        _worldSession = nullptr;
    }
}

void WorldSocket::ReadHandler()
{
    if (!IsOpen())
        return;

    MessageBuffer& packet = GetReadBuffer();
    while (packet.GetActiveSize() > 0)
    {
        if (_headerBuffer.GetRemainingSpace() > 0)
        {
            // need to receive the header
            std::size_t readHeaderSize = std::min(packet.GetActiveSize(), _headerBuffer.GetRemainingSpace());
            _headerBuffer.Write(packet.GetReadPointer(), readHeaderSize);
            packet.ReadCompleted(readHeaderSize);

            if (_headerBuffer.GetRemainingSpace() > 0)
            {
                // Couldn't receive the whole header this time.
                ASSERT(packet.GetActiveSize() == 0);
                break;
            }

            // We just received nice new header
            if (!ReadHeaderHandler())
            {
                CloseSocket();
                return;
            }
        }

        // We have full read header, now check the data payload
        if (_packetBuffer.GetRemainingSpace() > 0)
        {
            // need more data in the payload
            std::size_t readDataSize = std::min(packet.GetActiveSize(), _packetBuffer.GetRemainingSpace());
            _packetBuffer.Write(packet.GetReadPointer(), readDataSize);
            packet.ReadCompleted(readDataSize);

            if (_packetBuffer.GetRemainingSpace() > 0)
            {
                // Couldn't receive the whole data this time.
                ASSERT(packet.GetActiveSize() == 0);
                break;
            }
        }

        // just received fresh new payload
        ReadDataHandlerResult result = ReadDataHandler();
        _headerBuffer.Reset();

        if (result != ReadDataHandlerResult::Ok)
        {
            if (result != ReadDataHandlerResult::WaitingForQuery)
            {
                CloseSocket();
            }

            return;
        }
    }

    AsyncRead();
}

bool WorldSocket::ReadHeaderHandler()
{
    ASSERT(_headerBuffer.GetActiveSize() == sizeof(ClientPktHeader));

    if (_authCrypt.IsInitialized())
    {
        _authCrypt.DecryptRecv(_headerBuffer.GetReadPointer(), sizeof(ClientPktHeader));
    }

    ClientPktHeader* header = reinterpret_cast<ClientPktHeader*>(_headerBuffer.GetReadPointer());
    EndianConvertReverse(header->size);
    EndianConvert(header->cmd);

    if (!header->IsValidSize() || !header->IsValidOpcode())
    {
        LOG_ERROR("network", "WorldSocket::ReadHeaderHandler(): client %s sent malformed packet (size: %hu, cmd: %u)",
            GetRemoteIpAddress().to_string().c_str(), header->size, header->cmd);

        return false;
    }

    header->size -= sizeof(header->cmd);
    _packetBuffer.Resize(header->size);

    return true;
}

struct AuthSession
{
    uint32 BattlegroupID = 0;
    uint32 LoginServerType = 0;
    uint32 RealmID = 0;
    uint32 Build = 0;
    std::array<uint8, 4> LocalChallenge = {};
    uint32 LoginServerID = 0;
    uint32 RegionID = 0;
    uint64 DosResponse = 0;
    Acore::Crypto::SHA1::Digest Digest = {};
    std::string Account;
    ByteBuffer AddonInfo;
};

struct AccountInfo
{
    uint32 Id;
    ::SessionKey SessionKey;
    std::string LastIP;
    bool IsLockedToIP;
    std::string LockCountry;
    uint8 Expansion;
    int64 MuteTime;
    LocaleConstant Locale;
    uint32 Recruiter;
    std::string OS;
    bool IsRectuiter;
    AccountTypes Security;
    bool IsBanned;
    uint32 TotalTime;

    explicit AccountInfo(Field* fields)
    {
        //           0             1          2         3               4            5           6         7            8     9           10          11
        // SELECT a.id, a.sessionkey, a.last_ip, a.locked, a.lock_country, a.expansion, a.mutetime, a.locale, a.recruiter, a.os, a.totaltime, aa.gmLevel,
        //                                                           12    13
        // ab.unbandate > UNIX_TIMESTAMP() OR ab.unbandate = ab.bandate, r.id
        // FROM account a
        // LEFT JOIN account_access aa ON a.id = aa.AccountID AND aa.RealmID IN (-1, ?)
        // LEFT JOIN account_banned ab ON a.id = ab.id
        // LEFT JOIN account r ON a.id = r.recruiter
        // WHERE a.username = ? ORDER BY aa.RealmID DESC LIMIT 1
        Id = fields[0].GetUInt32();
        SessionKey = fields[1].GetBinary<SESSION_KEY_LENGTH>();
        LastIP = fields[2].GetString();
        IsLockedToIP = fields[3].GetBool();
        LockCountry = fields[4].GetString();
        Expansion = fields[5].GetUInt8();
        MuteTime = fields[6].GetInt64();
        Locale = LocaleConstant(fields[7].GetUInt8());
        Recruiter = fields[8].GetUInt32();
        OS = fields[9].GetString();
        TotalTime = fields[10].GetUInt32();
        Security = AccountTypes(fields[11].GetUInt8());
        IsBanned = fields[12].GetUInt64() != 0;
        IsRectuiter = fields[13].GetUInt32() != 0;

        uint32 world_expansion = sWorld->getIntConfig(CONFIG_EXPANSION);
        if (Expansion > world_expansion)
            Expansion = world_expansion;

        if (Locale >= TOTAL_LOCALES)
            Locale = LOCALE_enUS;
    }
};

WorldSocket::ReadDataHandlerResult WorldSocket::ReadDataHandler()
{
    ClientPktHeader* header = reinterpret_cast<ClientPktHeader*>(_headerBuffer.GetReadPointer());
    OpcodeClient opcode = static_cast<OpcodeClient>(header->cmd);

    WorldPacket packet(opcode, std::move(_packetBuffer));
    WorldPacket* packetToQueue;

    if (sPacketLog->CanLogPacket())
        sPacketLog->LogPacket(packet, CLIENT_TO_SERVER, GetRemoteIpAddress(), GetRemotePort());

    std::unique_lock<std::mutex> sessionGuard(_worldSessionLock, std::defer_lock);

    switch (opcode)
    {
        case CMSG_PING:
        {
            LogOpcodeText(opcode, sessionGuard);
            try
            {
                return HandlePing(packet) ? ReadDataHandlerResult::Ok : ReadDataHandlerResult::Error;
            }
            catch (ByteBufferException const&)
            {
            }
            LOG_ERROR("network", "WorldSocket::ReadDataHandler(): client %s sent malformed CMSG_PING", GetRemoteIpAddress().to_string().c_str());
            return ReadDataHandlerResult::Error;
        }
        case CMSG_AUTH_SESSION:
        {
            LogOpcodeText(opcode, sessionGuard);
            if (_authed)
            {
                // locking just to safely log offending user is probably overkill but we are disconnecting him anyway
                if (sessionGuard.try_lock())
                    LOG_ERROR("network", "WorldSocket::ProcessIncoming: received duplicate CMSG_AUTH_SESSION from %s", _worldSession->GetPlayerInfo().c_str());
                return ReadDataHandlerResult::Error;
            }

            try
            {
                HandleAuthSession(packet);
                return ReadDataHandlerResult::WaitingForQuery;
            }
            catch (ByteBufferException const&) { }

            LOG_ERROR("network", "WorldSocket::ReadDataHandler(): client %s sent malformed CMSG_AUTH_SESSION", GetRemoteIpAddress().to_string().c_str());
            return ReadDataHandlerResult::Error;
        }
        case CMSG_KEEP_ALIVE: // todo: handle this packet in the same way of CMSG_TIME_SYNC_RESP
            sessionGuard.lock();
            LogOpcodeText(opcode, sessionGuard);
            if (_worldSession)
                _worldSession->ResetTimeOutTime(true);
            return ReadDataHandlerResult::Ok;
        case CMSG_TIME_SYNC_RESP:
            packetToQueue = new WorldPacket(std::move(packet), std::chrono::steady_clock::now());
            break;
        default:
            packetToQueue = new WorldPacket(std::move(packet));
            break;
    }

    sessionGuard.lock();

    LogOpcodeText(opcode, sessionGuard);

    if (!_worldSession)
    {
        LOG_ERROR("network.opcode", "ProcessIncoming: Client not authed opcode = %u", uint32(opcode));
        delete packetToQueue;
        return ReadDataHandlerResult::Error;
    }

    OpcodeHandler const* handler = opcodeTable[opcode];
    if (!handler)
    {
        LOG_ERROR("network.opcode", "No defined handler for opcode %s sent by %s", GetOpcodeNameForLogging(static_cast<OpcodeClient>(packet.GetOpcode())).c_str(), _worldSession->GetPlayerInfo().c_str());
        delete packetToQueue;
        return ReadDataHandlerResult::Error;
    }

    // Our Idle timer will reset on any non PING opcodes on login screen, allowing us to catch people idling.
    _worldSession->ResetTimeOutTime(false);

    // Copy the packet to the heap before enqueuing
    _worldSession->QueuePacket(packetToQueue);

    return ReadDataHandlerResult::Ok;
}

void WorldSocket::LogOpcodeText(OpcodeClient opcode, std::unique_lock<std::mutex> const& guard) const
{
    if (!guard)
    {
        LOG_TRACE("network.opcode", "C->S: %s %s", GetRemoteIpAddress().to_string().c_str(), GetOpcodeNameForLogging(opcode).c_str());
    }
    else
    {
        LOG_TRACE("network.opcode", "C->S: %s %s", (_worldSession ? _worldSession->GetPlayerInfo() : GetRemoteIpAddress().to_string()).c_str(),
            GetOpcodeNameForLogging(opcode).c_str());
    }
}

void WorldSocket::SendPacketAndLogOpcode(WorldPacket const& packet)
{
    LOG_TRACE("network.opcode", "S->C: %s %s", GetRemoteIpAddress().to_string().c_str(), GetOpcodeNameForLogging(static_cast<OpcodeServer>(packet.GetOpcode())).c_str());
    SendPacket(packet);
}

void WorldSocket::SendPacket(WorldPacket const& packet)
{
    if (!IsOpen())
        return;

    if (sPacketLog->CanLogPacket())
        sPacketLog->LogPacket(packet, SERVER_TO_CLIENT, GetRemoteIpAddress(), GetRemotePort());

    _bufferQueue.Enqueue(new EncryptablePacket(packet, _authCrypt.IsInitialized()));
}

void WorldSocket::HandleAuthSession(WorldPacket & recvPacket)
{
    std::shared_ptr<AuthSession> authSession = std::make_shared<AuthSession>();

    // Read the content of the packet
    recvPacket >> authSession->Build;
    recvPacket >> authSession->LoginServerID;
    recvPacket >> authSession->Account;
    recvPacket >> authSession->LoginServerType;
    recvPacket.read(authSession->LocalChallenge);
    recvPacket >> authSession->RegionID;
    recvPacket >> authSession->BattlegroupID;
    recvPacket >> authSession->RealmID;               // realmId from auth_database.realmlist table
    recvPacket >> authSession->DosResponse;
    recvPacket.read(authSession->Digest);
    authSession->AddonInfo.resize(recvPacket.size() - recvPacket.rpos());
    recvPacket.read(authSession->AddonInfo.contents(), authSession->AddonInfo.size()); // .contents will throw if empty, thats what we want

    // Get the account information from the auth database
    LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_SEL_ACCOUNT_INFO_BY_NAME);
    stmt->setInt32(0, int32(realm.Id.Realm));
    stmt->setString(1, authSession->Account);

    _queryProcessor.AddCallback(LoginDatabase.AsyncQuery(stmt).WithPreparedCallback(std::bind(&WorldSocket::HandleAuthSessionCallback, this, authSession, std::placeholders::_1)));
}

void WorldSocket::HandleAuthSessionCallback(std::shared_ptr<AuthSession> authSession, PreparedQueryResult result)
{
    // Stop if the account is not found
    if (!result)
    {
        // We can not log here, as we do not know the account. Thus, no accountId.
        SendAuthResponseError(AUTH_UNKNOWN_ACCOUNT);
        LOG_ERROR("network", "WorldSocket::HandleAuthSession: Sent Auth Response (unknown account).");
        DelayedCloseSocket();
        return;
    }

    AccountInfo account(result->Fetch());

    // For hook purposes, we get Remoteaddress at this point.
    std::string address = GetRemoteIpAddress().to_string();

    // As we don't know if attempted login process by ip works, we update last_attempt_ip right away
    LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_UPD_LAST_ATTEMPT_IP);
    stmt->setString(0, address);
    stmt->setString(1, authSession->Account);
    LoginDatabase.Execute(stmt);
    // This also allows to check for possible "hack" attempts on account

    // even if auth credentials are bad, try using the session key we have - client cannot read auth response error without it
    _authCrypt.Init(account.SessionKey);

    // First reject the connection if packet contains invalid data or realm state doesn't allow logging in
    if (sWorld->IsClosed())
    {
        SendAuthResponseError(AUTH_REJECT);
        LOG_ERROR("network", "WorldSocket::HandleAuthSession: World closed, denying client (%s).", GetRemoteIpAddress().to_string().c_str());
        DelayedCloseSocket();
        return;
    }

    if (authSession->RealmID != realm.Id.Realm)
    {
        SendAuthResponseError(REALM_LIST_REALM_NOT_FOUND);
        LOG_ERROR("network", "WorldSocket::HandleAuthSession: Client %s requested connecting with realm id %u but this realm has id %u set in config.",
            GetRemoteIpAddress().to_string().c_str(), authSession->RealmID, realm.Id.Realm);
        DelayedCloseSocket();
        return;
    }

    // Must be done before WorldSession is created
    bool wardenActive = sWorld->getBoolConfig(CONFIG_WARDEN_ENABLED);
    if (wardenActive && account.OS != "Win" && account.OS != "OSX")
    {
        SendAuthResponseError(AUTH_REJECT);
        LOG_ERROR("network", "WorldSocket::HandleAuthSession: Client %s attempted to log in using invalid client OS (%s).", address.c_str(), account.OS.c_str());
        DelayedCloseSocket();
        return;
    }

    // Check that Key and account name are the same on client and server
    uint8 t[4] = { 0x00,0x00,0x00,0x00 };

    Acore::Crypto::SHA1 sha;
    sha.UpdateData(authSession->Account);
    sha.UpdateData(t);
    sha.UpdateData(authSession->LocalChallenge);
    sha.UpdateData(_authSeed);
    sha.UpdateData(account.SessionKey);
    sha.Finalize();

    if (sha.GetDigest() != authSession->Digest)
    {
        SendAuthResponseError(AUTH_FAILED);
        LOG_ERROR("network", "WorldSocket::HandleAuthSession: Authentication failed for account: %u ('%s') address: %s", account.Id, authSession->Account.c_str(), address.c_str());
        DelayedCloseSocket();
        return;
    }

    if (IpLocationRecord const* location = sIPLocation->GetLocationRecord(address))
        _ipCountry = location->CountryCode;

    ///- Re-check ip locking (same check as in auth).
    if (account.IsLockedToIP)
    {
        if (account.LastIP != address)
        {
            SendAuthResponseError(AUTH_FAILED);
            LOG_DEBUG("network", "WorldSocket::HandleAuthSession: Sent Auth Response (Account IP differs. Original IP: %s, new IP: %s).", account.LastIP.c_str(), address.c_str());
            // We could log on hook only instead of an additional db log, however action logger is config based. Better keep DB logging as well
            sScriptMgr->OnFailedAccountLogin(account.Id);
            DelayedCloseSocket();
            return;
        }
    }
    else if (!account.LockCountry.empty() && account.LockCountry != "00" && !_ipCountry.empty())
    {
        if (account.LockCountry != _ipCountry)
        {
            SendAuthResponseError(AUTH_FAILED);
            LOG_DEBUG("network", "WorldSocket::HandleAuthSession: Sent Auth Response (Account country differs. Original country: %s, new country: %s).", account.LockCountry.c_str(), _ipCountry.c_str());
            // We could log on hook only instead of an additional db log, however action logger is config based. Better keep DB logging as well
            sScriptMgr->OnFailedAccountLogin(account.Id);
            DelayedCloseSocket();
            return;
        }
    }

    if (account.IsBanned)
    {
        SendAuthResponseError(AUTH_BANNED);
        LOG_ERROR("network", "WorldSocket::HandleAuthSession: Sent Auth Response (Account banned).");
        sScriptMgr->OnFailedAccountLogin(account.Id);
        DelayedCloseSocket();
        return;
    }

    // Check locked state for server
    AccountTypes allowedAccountType = sWorld->GetPlayerSecurityLimit();
    LOG_DEBUG("network", "Allowed Level: %u Player Level %u", allowedAccountType, account.Security);
    if (allowedAccountType > SEC_PLAYER && account.Security < allowedAccountType)
    {
        SendAuthResponseError(AUTH_UNAVAILABLE);
        LOG_DEBUG("network", "WorldSocket::HandleAuthSession: User tries to login but his security level is not enough");
        sScriptMgr->OnFailedAccountLogin(account.Id);
        DelayedCloseSocket();
        return;
    }

    LOG_DEBUG("network", "WorldSocket::HandleAuthSession: Client '%s' authenticated successfully from %s.", authSession->Account.c_str(), address.c_str());

    // Update the last_ip in the database as it was successful for login
    stmt = LoginDatabase.GetPreparedStatement(LOGIN_UPD_LAST_IP);
    stmt->setString(0, address);
    stmt->setString(1, authSession->Account);

    LoginDatabase.Execute(stmt);

    // At this point, we can safely hook a successful login
    sScriptMgr->OnAccountLogin(account.Id);

    _authed = true;

    _worldSession = new WorldSession(account.Id, std::move(authSession->Account), shared_from_this(), account.Security,
        account.Expansion, account.MuteTime, account.Locale, account.Recruiter, account.IsRectuiter, account.Security ? true : false, account.TotalTime);

    _worldSession->ReadAddonsInfo(authSession->AddonInfo);

    // Initialize Warden system only if it is enabled by config
    if (wardenActive)
    {
        _worldSession->InitWarden(account.SessionKey, account.OS);
    }

    sWorld->AddSession(_worldSession);

    AsyncRead();
}

void WorldSocket::SendAuthResponseError(uint8 code)
{
    WorldPacket packet(SMSG_AUTH_RESPONSE, 1);
    packet << uint8(code);

    SendPacketAndLogOpcode(packet);
}

bool WorldSocket::HandlePing(WorldPacket& recvPacket)
{
    using namespace std::chrono;

    uint32 ping;
    uint32 latency;

    // Get the ping packet content
    recvPacket >> ping;
    recvPacket >> latency;

    if (_LastPingTime == steady_clock::time_point())
    {
        _LastPingTime = steady_clock::now();
    }
    else
    {
        steady_clock::time_point now = steady_clock::now();
        steady_clock::duration diff = now - _LastPingTime;

        _LastPingTime = now;

        if (diff < seconds(27))
        {
            ++_OverSpeedPings;

            uint32 maxAllowed = sWorld->getIntConfig(CONFIG_MAX_OVERSPEED_PINGS);

            if (maxAllowed && _OverSpeedPings > maxAllowed)
            {
                std::unique_lock<std::mutex> sessionGuard(_worldSessionLock);

                if (_worldSession && AccountMgr::IsPlayerAccount(_worldSession->GetSecurity()))
                {
                    LOG_ERROR("network", "WorldSocket::HandlePing: %s kicked for over-speed pings (address: %s)",
                        _worldSession->GetPlayerInfo().c_str(), GetRemoteIpAddress().to_string().c_str());

                    return false;
                }
            }
        }
        else
        {
            _OverSpeedPings = 0;
        }
    }

    {
        std::lock_guard<std::mutex> sessionGuard(_worldSessionLock);

        if (_worldSession)
            _worldSession->SetLatency(latency);
        else
        {
            LOG_ERROR("network", "WorldSocket::HandlePing: peer sent CMSG_PING, but is not authenticated or got recently kicked, address = %s", GetRemoteIpAddress().to_string().c_str());
            return false;
        }
    }

    WorldPacket packet(SMSG_PONG, 4);
    packet << ping;
    SendPacketAndLogOpcode(packet);

    return true;
}
