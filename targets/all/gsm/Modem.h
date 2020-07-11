/*
 * Copyright (c) 2019 triaxis s.r.o.
 * Licensed under the MIT license. See LICENSE.txt file in the repository root
 * for full license information.
 *
 * gsm/Modem.h
 *
 * General GSM modem support
 */

#pragma once

#include <kernel/kernel.h>
#include <base/fnv1.h>
#include <io/io.h>
#include <collections/SelfLinkedList.h>

#include "Socket.h"
#include "ModemOptions.h"

namespace gsm
{

class Modem
{
public:
    enum struct ModemStatus
    {
        Ok,
        PowerOnFailure,
        AutoBaudFailure,
        CommandError,
    };

    enum struct GsmStatus
    {
        Ok,
        NoNetwork,
        Roaming,
        Searching,
    };

    enum struct SimStatus
    {
        Ok,
        NotInserted,
        Locked,
        BadPin,
    };

    enum struct TcpStatus
    {
        Ok,
        GprsError,
        TlsError,
        ConnectionError,
    };

    Modem(io::DuplexPipe pipe, ModemOptions& options)
        : rx(pipe), tx(pipe), options(options) {}

    ModemStatus ModemStatus() const { return modemStatus; }

    Timeout ATTimeout() const { return atTimeout; }
    void ATTimeout(Timeout timeout) { ASSERT(timeout.IsRelative()); atTimeout = timeout; }
    Timeout ConnectTimeout() const { return connectTimeout; }
    void ConnectTimeout(Timeout timeout) { ASSERT(timeout.IsRelative()); connectTimeout = timeout; }
    Timeout DisconnectTimeout() const { return disconnectTimeout; }
    void DisconnectTimeout(Timeout timeout) { ASSERT(timeout.IsRelative()); disconnectTimeout = timeout; }
    Timeout PowerOffTimeout() const { return powerOffTimeout; }
    void PowerOffTimeout(Timeout timeout) { ASSERT(timeout.IsRelative()); powerOffTimeout = timeout; }

    async(WaitForIdle, Timeout timeout);
    Socket* CreateSocket(Span host, uint32_t port, bool tls);

protected:
    enum struct ATResult : int8_t
    {
        OK,
        Error,
        Timeout,
        Failure,
        Pending = -1,
    };

    void EnsureRunning();

    //! Sets the timeout for the next AT call, can be called only after ATLock
    void NextATTimeout(Timeout timeout) { ASSERT(atTask == &kernel::Task::Current()); atNextTimeout = timeout; }
    //! Sets a callback for the next AT call, can be called only after ATLock
    void NextATResponse(AsyncDelegate<FNV1a> handler) { ASSERT(atTask == &kernel::Task::Current()); atResponse = handler; }
    //! Sets the socket from which data will ba transmitted during the AT command
    void NextATTransmit(Socket& sock, size_t len) { ASSERT(atTask == &kernel::Task::Current()); atTransmitSock = &sock; atTransmitLen = len; }

    async(ATLock);
    async(AT, Span cmd);
    async(ATFormat, const char* format, ...) async_def_va(ATFormatV, format, format);
    async(ATFormatV, const char* format, va_list va);

    void ReceiveForSocket(Socket* sock, size_t len) { rxSock = sock; rxLen = len; }

    async(NetworkActive, Timeout timeout = Timeout::Infinite);

    virtual bool TryAllocateImpl(Socket& sock) = 0;
    virtual async(ConnectImpl, Socket& sock) = 0;
    virtual async(SendPacketImpl, Socket& sock) = 0;
    virtual async(ReceivePacketImpl, Socket& sock) = 0;
    virtual async(CheckIncomingImpl, Socket& sock) = 0;
    virtual async(CloseImpl, Socket& sock) = 0;

    virtual async(PowerOnImpl) async_def_return(true);
    virtual async(PowerOffImpl) async_def_return(true);
    virtual async(StartImpl) async_def_return(true);
    virtual async(UnlockSimImpl) async_def_return(true);
    virtual async(ConnectNetworkImpl) async_def_return(true);
    virtual async(DisconnectNetworkImpl) async_def_return(true);
    virtual async(StopImpl) async_def_return(true);
    virtual async(OnEvent, FNV1a id) async_def_return(true);
    virtual void OnTaskStopped() {}

    void ModemStatus(enum ModemStatus status) { modemStatus = status; }
    void GsmStatus(enum GsmStatus status) { gsmStatus = status; }
    void SimStatus(enum SimStatus status) { simStatus = status; }
    void TcpStatus(enum TcpStatus status) { tcpStatus = status; }

    void RequestProcessing() { process = true; }

    io::PipeReader Input() { return rx; }
    io::PipeWriter Output() { return tx; }
    ModemOptions& Options() { return options; }
    SelfLinkedList<Socket>& Sockets() { return sockets; }
    Socket* FindSocket(uint8_t channel) { for (auto& s: sockets) { if (s.IsAllocated() && s.channel == channel) return &s; } return NULL; }
    Socket* FindSocket(uint8_t channel, bool secure) { for (auto& s: sockets) { if (s.IsAllocated() && s.IsSecure() == secure && s.channel == channel) return &s; } return NULL; }

    unsigned InputFieldCount() const;
    bool InputFieldNum(int& n, unsigned base = 10);
    bool InputFieldHex(int& n) { return InputFieldNum(n, 16); }
    bool InputFieldFnv(uint32_t& fnv);
    size_t InputLength() { return rx.LengthUntil(lineEnd); }

private:
    io::PipeReader rx;
    io::PipeWriter tx;
    ModemOptions& options;
    SelfLinkedList<Socket> sockets;

    enum struct Signal
    {
        None = 0,
        TaskActive = BIT(0),
        RxTaskActive = BIT(1),
        NetworkActive = BIT(2),
        ATLock = BIT(4),
    } signals = Signal::None;

    DECLARE_FLAG_ENUM(Signal);

    bool process = false;
    ATResult atResult = ATResult::OK;

    io::PipePosition lineEnd;
    kernel::Task* atTask = NULL;
    Timeout atNextTimeout;
    AsyncDelegate<FNV1a> atResponse;
    Socket* atTransmitSock;
    size_t atTransmitLen;
    Socket* rxSock;
    size_t rxLen = 0;

    enum ModemStatus modemStatus;
    enum GsmStatus gsmStatus;
    enum SimStatus simStatus;
    enum TcpStatus tcpStatus;

    Timeout atTimeout = Timeout::Seconds(5);
    Timeout connectTimeout = Timeout::Seconds(30);
    Timeout disconnectTimeout = Timeout::Seconds(10);
    Timeout powerOffTimeout = Timeout::Infinite;

    async(Task);
    async(RxTask);
    async(ATResponse);

    void ReleaseSocket(Socket* sock);
    void DestroySocket(Socket* sock);

    friend class Socket;
};

DEFINE_FLAG_ENUM(Modem::Signal);

}
