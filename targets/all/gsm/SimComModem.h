/*
 * Copyright (c) 2019 triaxis s.r.o.
 * Licensed under the MIT license. See LICENSE.txt file in the repository root
 * for full license information.
 *
 * gsm/SimComModem.h
 *
 * Driver for the SimCom GSM modules
 * SIM800 (2G) and SIM7600 (4G) series supported
 */

#pragma once

#include <kernel/kernel.h>

#include <hw/GPIO.h>
#include <hw/USART.h>

#include <io/USARTRxPipe.h>
#include <io/USARTTxPipe.h>

#include <nvram/nvram.h>

#include <gsm/Modem.h>

namespace gsm
{

class SimComModem : public Modem
{
private:
    struct SimComSocket : Socket
    {
        size_t incoming, outgoing, lastSent;
        bool error;
        uint8_t channel;
    };

    SimComSocket* FindSocket(uint8_t channel) { for (auto& s: Sockets()) { if (s.IsAllocated() && S(s).channel == channel) return S(&s); } return NULL; }
    SimComSocket* FindSocket(uint8_t channel, bool secure) { for (auto& s: Sockets()) { if (s.IsAllocated() && s.IsSecure() == secure && S(s).channel == channel) return (SimComSocket*)&s; } return NULL; }

    SimComSocket& S(Socket& sock) { return (SimComSocket&)sock; }
    SimComSocket* S(Socket* sock) { return (SimComSocket*)sock; }

public:
    SimComModem(ModemOptions& options, USART& usart, GPIOPin powerEnable, GPIOPin powerButton, GPIOPin status, GPIOPin dtr)
        : Modem(io::DuplexPipe(gsmRx, gsmTx), options), usartRx(usart, gsmRx), usartTx(usart, gsmTx), powerEnable(powerEnable), powerButton(powerButton), status(status), dtr(dtr)
    {
    }

    Timeout AllocateTimeout() const { return allocateTimeout; }
    void AllocateTimeout(Timeout timeout) { ASSERT(timeout.IsRelative()); allocateTimeout = timeout; }

    enum struct Model
    {
        Unknown,
        SIM800,
        SIM7600,
    };

    Model DetectedModel() const { return model; }

protected:
    virtual size_t SocketSizeImpl() const final override { return sizeof(SimComSocket); }
    virtual bool TryAllocateImpl(Socket& sock) final override;
    virtual async(ConnectImpl, Socket& sock) final override;
    virtual async(SendPacketImpl, Socket& sock) final override;
    virtual async(ReceivePacketImpl, Socket& sock) final override;
    virtual async(CheckIncomingImpl, Socket& sock) final override;
    virtual async(CloseImpl, Socket& sock) final override;
    virtual async(GetLocation, Buffer& buff) final override;

    virtual async(SendMessageImpl, Message& msg) final override;

private:
    enum struct Registration
    {
        None, Home, Searching, Denied, Unknown, Roaming,
    };

    enum
    {
        MaxPacket = 1024,
    };

    static const char* StatusName(Registration reg) { return STRINGS("NONE", "HOME", "SEARCHING", "DENIED", "UNKNOWN", "ROAMING")[int(reg)]; }

    const char* ModelName() const { return STRINGS(NULL, "SIM800", "SIM7600")[int(model)]; }
    unsigned ModelBaudRate() const { return LOOKUP_TABLE(unsigned, 115200, 460800, 3200000)[int(model)]; }

    io::Pipe gsmRx, gsmTx;
    io::USARTRxPipe usartRx;
    io::USARTTxPipe usartTx;
    GPIOPin powerEnable, powerButton, status, dtr;
    bool removePin = false;

    Model model = Model::Unknown;
    uint8_t cfun;
    struct
    {
        bool pinRequired, pinUsed, ready;
    } sim = {};
    struct RegBase
    {
        Registration status;
        bool activated;
        bool active;
        uint16_t lac, ci;
        const char* StatusName() const { return SimComModem::StatusName(status); }
    };
    struct : RegBase
    {
        int8_t rssi, ber;
        bool error;
    } net = {};
    struct : RegBase
    {
        bool attached, pdpActive;
    } gprs = {};

    char pin[9] = {0};

    Timeout allocateTimeout = Timeout::Seconds(1);
    bool running = false;

    async(PowerOnImpl) override;
    async(PowerOffImpl) override;
    async(StartImpl) override;
    async(UnlockSimImpl) override;
    async(ConnectNetworkImpl) override;
    async(DisconnectNetworkImpl) override;

    async(Initialize);
    async(StartGprs);

    async(OnEvent, FNV1a id) override;

    async(OnSendResponse800, FNV1a header);
    async(OnSendResponse7600, FNV1a header);

    async(OnReceiveId, FNV1a header);
    async(OnReceivePlainIP, FNV1a header);
    async(OnReceiveNetCch, FNV1a header);
    async(OnReceiveShutOK, FNV1a header);
    async(OnReceivePowerDown, FNV1a header);
};

}
