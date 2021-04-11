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
#include "Message.h"
#include "ModemOptions.h"

namespace gsm
{

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

class NetworkInfo
{
    union
    {
        struct
        {
            unsigned mcc : 10, mnc : 10, mncDigits : 4;
        };
        unsigned raw;
    };

public:
    constexpr NetworkInfo(unsigned mcc, unsigned mnc, unsigned mncDigits)
        : mcc(mcc), mnc(mnc), mncDigits(mncDigits) {}
    constexpr NetworkInfo()
        : raw(0) {}

    unsigned Mcc() const { return mcc; }
    unsigned Mnc() const { return mnc; }
    unsigned MncDigits() const { return mncDigits; }
};

class Modem
{
public:

    Modem(io::DuplexPipe pipe, ModemOptions& options)
        : rx(pipe), tx(pipe), options(options) {}

    enum ModemStatus ModemStatus() const { return modemStatus; }
    enum GsmStatus GsmStatus() const { return gsmStatus; }
    enum SimStatus SimStatus() const { return simStatus; }
    enum TcpStatus TcpStatus() const { return tcpStatus; }

    Span GetApn() const { return options.GetApn(); }
    Span GetApnUser() const { return options.GetApnUser(); }
    Span GetApnPassword() const { return options.GetApnPassword(); }

    const class NetworkInfo& NetworkInfo() const { return netInfo; }

    bool IsActive() const { return !!(signals & Signal::TaskActive); }
    bool IsDisconnecting() const { return !!(signals & Signal::NetworkDisconnecting); }

    int Rssi() const { return rssi; }

    Timeout ATTimeout() const { return atTimeout; }
    void ATTimeout(Timeout timeout) { ASSERT(timeout.IsRelative()); atTimeout = timeout; }
    Timeout ConnectTimeout() const { return connectTimeout; }
    void ConnectTimeout(Timeout timeout) { ASSERT(timeout.IsRelative()); connectTimeout = timeout; }
    Timeout DisconnectTimeout() const { return disconnectTimeout; }
    void DisconnectTimeout(Timeout timeout) { ASSERT(timeout.IsRelative()); disconnectTimeout = timeout; }
    Timeout PowerOffTimeout() const { return powerOffTimeout; }
    void PowerOffTimeout(Timeout timeout) { ASSERT(timeout.IsRelative()); powerOffTimeout = timeout; }

    async(WaitForIdle, Timeout timeout);
    async(WaitForPowerOn, Timeout timeout);
    async(WaitForPowerOff, Timeout timeout);
    Socket* CreateSocket(Span host, uint32_t port, bool tls);
    Message* SendMessage(Span recipient, Span text);

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
    //! @returns false so it can be easily chained between ATLock and ATXxx
    bool NextATTimeout(Timeout timeout) { ASSERT(atTask == &kernel::Task::Current()); atNextTimeout = timeout; return false; }
    //! Sets a callback for the next AT call, can be called only after ATLock
    //! @returns false so it can be easily chained between ATLock and ATXxx
    bool NextATResponse(AsyncDelegate<FNV1a> handler, uint8_t mask = 1) { ASSERT(atTask == &kernel::Task::Current()); atResponse = handler; atRequire = mask; return false; }
    //! Sets the socket from which data will be transmitted during the AT command
    //! @returns false so it can be easily chained between ATLock and ATXxx
    bool NextATTransmit(Socket& sock, size_t len) { ASSERT(atTask == &kernel::Task::Current()); atTransmitSock = &sock; atTransmitLen = len; return false; }
    //! Sets the message which will be transmitted during the AT command
    //! @returns false so it can be easily chained between ATLock and ATXxx
    bool NextATTransmit(Message& msg) { ASSERT(atTask == &kernel::Task::Current()); atTransmitMsg = &msg; return false; }
    //! Sets the requirements mask for next AT command. ATComplete must be called with all mask bits before the command is considered complete.
    //! @returns false so it can be easily chained between ATLock and ATXxx
    //! Mark the specified requirement mask as complete
    void ATComplete(uint8_t mask = 1) { ASSERT(atResult == ATResult::Pending); if ((atComplete |= mask) == atRequire) { atResult = ATResult::OK; } }

    //! Gets the lock for executing an AT command with response
    //! @returns non-zero if the lock cannot be obtained
    async(ATLock);
    //! Executes a simple AT command
    //! @returns an ATResult indicating the result of the command execution
    async(AT, Span cmd);
    //! Executes a simple AT command
    //! @returns an ATResult indicating the result of the command execution
    async(ATFormat, const char* format, ...) async_def_va(ATFormatV, format, format);
    //! Executes a simple AT command
    //! @returns an ATResult indicating the result of the command execution
    async(ATFormatV, const char* format, va_list va);

    void ReceiveForSocket(Socket* sock, size_t len) { rxSock = sock; rxLen = len; }

    async(NetworkActive, Timeout timeout = Timeout::Infinite);

    virtual size_t SocketSizeImpl() const { return sizeof(Socket); }
    virtual size_t MessageSizeImpl() const { return sizeof(Message); }
    virtual bool TryAllocateImpl(Socket& sock) = 0;
    virtual async(ConnectImpl, Socket& sock) = 0;
    virtual async(SendPacketImpl, Socket& sock) = 0;
    virtual async(ReceivePacketImpl, Socket& sock) = 0;
    virtual async(CheckIncomingImpl, Socket& sock) = 0;
    virtual async(CloseImpl, Socket& sock) = 0;

    virtual async(SendMessageImpl, Message& msg) async_def_return(false);

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
    void NetworkInfo(const class NetworkInfo& info) { netInfo = info; }
    void Rssi(int8_t value) { rssi = value; }

    void RequestProcessing() { process = true; }

    io::PipeReader Input() { return rx; }
    size_t InputLength() const { return rx.LengthUntil(lineEnd); }
    io::PipeWriter Output() { return tx; }
    ModemOptions& Options() { return options; }
    SelfLinkedList<Socket>& Sockets() { return sockets; }

    io::Pipe::Iterator& InputField() { return lineFields; }
    unsigned InputFieldCount() const;
    bool InputFieldNum(int& n, unsigned base = 10);
    bool InputFieldHex(int& n) { return InputFieldNum(n, 16); }
    bool InputFieldFnv(uint32_t& fnv);

    void PowerDiagnostic(ModemOptions::CallbackType type, Span msg);

private:
    io::PipeReader rx;
    io::PipeWriter tx;
    ModemOptions& options;
    SelfLinkedList<Socket> sockets;
    SelfLinkedList<Message> messages;

    enum struct Signal
    {
        None = 0,
        TaskActive = BIT(0),
        RxTaskActive = BIT(1),
        NetworkActive = BIT(2),
        NetworkDisconnecting = BIT(3),
        ATLock = BIT(4),
        RequireActive = BIT(5), // set if there are active sockets or messsages
    } signals = Signal::None;

    DECLARE_FLAG_ENUM(Signal);

    bool process = false;
    ATResult atResult = ATResult::OK;
    uint8_t atComplete, atRequire;

    io::PipePosition lineEnd;
    io::Pipe::Iterator lineFields;
    kernel::Task* atTask = NULL;
    Timeout atNextTimeout;
    AsyncDelegate<FNV1a> atResponse;
    Socket* atTransmitSock;
    Message* atTransmitMsg;
    size_t atTransmitLen;
    Socket* rxSock;
    size_t rxLen = 0;

    enum ModemStatus modemStatus = ModemStatus::Ok;
    enum GsmStatus gsmStatus = GsmStatus::Ok;
    enum SimStatus simStatus = SimStatus::Ok;
    enum TcpStatus tcpStatus = TcpStatus::Ok;
    class NetworkInfo netInfo = {};
    int8_t rssi = 0;

    Timeout atTimeout = Timeout::Seconds(5);
    Timeout connectTimeout = Timeout::Seconds(30);
    Timeout disconnectTimeout = Timeout::Seconds(10);
    Timeout powerOffTimeout = Timeout::Infinite;

    async(Task);
    async(RxTask);
    async(ATResponse);

    void ReleaseSocket(Socket* sock);
    void DestroySocket(Socket* sock);

    void ReleaseMessage(Message* msg);
    void DestroyMessage(Message* msg);

    friend class Socket;
    friend class Message;
};

DEFINE_FLAG_ENUM(Modem::Signal);

}
