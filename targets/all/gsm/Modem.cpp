/*
 * Copyright (c) 2019 triaxis s.r.o.
 * Licensed under the MIT license. See LICENSE.txt file in the repository root
 * for full license information.
 *
 * gsm/GSMModule.cpp
 */

#include <gsm/Modem.h>

#define TRACE_AT        1
#define TRACE_SOCKETS   2
#define TRACE_DATA      4

#define MODEM_TRACE     TRACE_AT | TRACE_SOCKETS

#define MYDBG(...)      DBGCL("gsm", __VA_ARGS__)

#if MODEM_TRACE
#define MYTRACE(flag, ...) if ((MODEM_TRACE) & (flag)) { MYDBG(__VA_ARGS__); }
#else
#define MYTRACE(...)
#endif

namespace gsm
{

async(Modem::WaitForPowerOn, Timeout timeout)
async_def_once()
{
    async_return(await_mask_timeout(signals, Signal::RxTaskActive, Signal::RxTaskActive, timeout));
}
async_end

async(Modem::WaitForIdle, Timeout timeout)
async_def_once()
{
    async_return(await_mask_timeout(sockets, ~0, 0, timeout));
}
async_end


async(Modem::WaitForPowerOff, Timeout timeout)
async_def_once()
{
    async_return(await_mask_not_timeout(signals, Signal::TaskActive, Signal::TaskActive, timeout));
}
async_end

Socket* Modem::CreateSocket(Span host, uint32_t port, bool tls)
{
    auto size = SocketSizeImpl();
    auto sock = (Socket*)malloc(size + host.Length() + 1);
    if (!sock)
        return NULL;

    new(sock) Socket(this, &process);
    if (size > sizeof(Socket))
    {
        memset(sock + 1, 0, size - sizeof(Socket));
    }
    sock->flags = SocketFlags::AppReference | (SocketFlags::AppSecure * tls);
    sock->port = port;
    auto pHost = (char*)sock + size;
    memcpy(pHost, host.Pointer(), host.Length());
    pHost[host.Length()] = 0;
    sock->host = pHost;

    sockets.Append(sock);
    signals |= Signal::RequireActive;

    EnsureRunning();

    MYDBG("Socket %p to %s:%d created", sock, sock->host, sock->port);
    return sock;
}

void Modem::RequestLocation()
{
    MYDBG("Requesting location");
    requireLocation = true;
    EnsureRunning();
    return;
}

int Modem::ParseLocationToInt(Span s)
{
    const char* data = s;
    const char* end = s.end();
    int base = 10;

    while (data < end && isspace(*data))
        data++;

    bool neg = false;

    if (data < end && (*data == '+' || (neg = *data == '-')))
        data++;
   
    int res = 0;
    while (data < end)
    {
        char c = *data;
        size_t digit;
        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (c == '.')
        {
            data++;
            continue;
        }
        else
            break;

        res = res * base + digit;
        data++;
    }
    return neg ? -res : res;
}

Message* Modem::SendMessage(Span recipient, Span text)
{
    auto size = MessageSizeImpl();
    auto msg = (Message*)malloc(size + recipient.Length() + text.Length());
    if (!msg)
    {
        return NULL;
    }

    new(msg) Message(this);
    if (size > sizeof(Message))
    {
        memset(msg + 1, 0, size - sizeof(Message));
    }
    auto pData = (char*)msg + size;
    msg->flags = MessageFlags::AppReference | MessageFlags::ModemWillSend;
    msg->data = pData;
    msg->lenRcpt = recipient.Length();
    msg->lenTxt = text.Length();
    memcpy(pData, recipient.Pointer(), msg->lenRcpt);
    memcpy(pData + msg->lenRcpt, text.Pointer(), msg->lenTxt);

    messages.Append(msg);
    signals |= Signal::RequireActive;

    EnsureRunning();

    MYDBG("Message %p to %b created: %b", msg, msg->Recipient(), msg->Text());
    return msg;
}

void Modem::DestroySocket(Socket* sock)
{
    ASSERT(!sock->next);
    ASSERT(!sockets.Contains(sock));
    MYDBG("Socket %p to %s:%d destroyed", sock, sock->host, sock->port);
    sock->~Socket();
    free(sock);
}

void Modem::ReleaseSocket(Socket* sock)
{
    ASSERT(sockets.Contains(sock));
    ASSERT(sock->flags & SocketFlags::AppReference);

    MYDBG("Socket %p to %s:%d released by app", sock, sock->host, sock->port);
    // mark as released and request closure
    sock->flags = (sock->flags & ~SocketFlags::AppReference) | SocketFlags::AppClose;
    // we need the task to run, at least to destroy the socket
    EnsureRunning();
}

void Modem::DestroyMessage(Message* msg)
{
    ASSERT(!msg->next);
    ASSERT(!messages.Contains(msg));
    MYDBG("Message %p to %b destroyed", msg, msg->Recipient());
    msg->~Message();
    free(msg);
}

void Modem::ReleaseMessage(Message* msg)
{
    ASSERT(messages.Contains(msg));
    ASSERT(msg->flags & MessageFlags::AppReference);

    MYDBG("Message %p to %b released by app", msg, msg->Recipient());
    msg->flags -= MessageFlags::AppReference;
    // we need the task to run, at least to destroy the message
    EnsureRunning();
}

void Modem::EnsureRunning()
{
    RequestProcessing();
    if (!(signals & Signal::TaskActive))
    {
        signals |= Signal::TaskActive;
        kernel::Task::Run(this, &Modem::Task);
    }
}

async(Modem::Task)
async_def(
    union { Socket* s; Message* m; };
    union { Socket* next; Message* mNext; };
    Buffer buff;
)
{
    // we may not need to run, preprocess sockets to find if there is an active one
    MYTRACE(TRACE_SOCKETS, "Preprocessing sockets...");
    for (auto& s: sockets)
    {
        if (!!(s.flags & SocketFlags::AppClose))
        {
            // app has requested closure of the socket in the meantime, just mark it closed
            s.Finished();
        }
        else if (s.IsNew())
        {
            // the socket is alive and needs processing
            MYTRACE(TRACE_SOCKETS, "Socket %p is alive, will power on...", &s);
            f.next = &s;
        }
        else
        {
            // should have been closed already
            ASSERT(s.IsClosed());
        }
    }

    // destroy old sockets
    for (auto& manip: sockets.Manipulate())
    {
        if (manip.Element().CanDelete())
        {
            // delete socket
            DestroySocket(&manip.Remove());
        }
    }

    // destroy old messages
    for (auto& manip: messages.Manipulate())
    {
        if (manip.Element().CanDelete())
        {
            // delete message
            DestroyMessage(&manip.Remove());
        }
    }

    if (!f.next && !messages)
    {
        MYTRACE(TRACE_SOCKETS, "No active sockets or messages to send, not starting...");
        signals &= ~Signal::TaskActive;
        async_return(false);
    }

    process = true;

    PowerDiagnostic(ModemOptions::CallbackType::PowerSend, "ON");
    if (!await(PowerOnImpl))
    {
        PowerDiagnostic(ModemOptions::CallbackType::PowerReceive, "ERR");
        ModemStatus(ModemStatus::PowerOnFailure);
        MYDBG("Power on failed. Will retry in 10 seconds.");
        async_delay_sec(10);
        if (!await(PowerOnImpl))
        {
            PowerDiagnostic(ModemOptions::CallbackType::PowerReceive, "FAIL");

            // finish all sockets
            for (f.s = sockets.First(); f.s && !rxLen; f.s = f.s->next)
            {
                f.s->Finished();
            }

            signals &= ~Signal::TaskActive;
            OnTaskStopped();
            async_return(false);
        }
    }

    PowerDiagnostic(ModemOptions::CallbackType::PowerReceive, "ON");
    options.OnPowerOn();
    MYDBG("Starting RX");
    ASSERT(!(signals & (Signal::RxTaskActive | Signal::RxTaskActive)));
    signals |= Signal::RxTaskActive;
    kernel::Task::Run(this, &Modem::RxTask);

    if (await(StartImpl))
    {
        ModemStatus(ModemStatus::Ok);

        if (await(UnlockSimImpl))
        {
            SimStatus(SimStatus::Ok);

            if (await(ConnectNetworkImpl))
            {
                GsmStatus(GsmStatus::Ok);
                signals |= Signal::NetworkActive;    // allow connections

                while (await_acquire_zero(process, 1))
                {
                    MYTRACE(TRACE_SOCKETS, "Processing...");

                    // disconnect sockets
                    for (f.s = sockets.First(); f.s && !rxLen; f.s = f.next)
                    {
                        f.next = f.s->next;
                        if (f.s->NeedsClose())
                        {
                            f.s->flags |= SocketFlags::ModemClosing;
                            MYDBG("Closing socket %p", f.s);
                            await(CloseImpl, *f.s);
                        }
                    }

                    // remove unused sockets
                    for (auto& manip: sockets.Manipulate())
                    {
                        if (manip.Element().CanDelete())
                        {
                            // delete socket
                            DestroySocket(&manip.Remove());
                        }
                    }

                    // process other operations (allocate, connect, send)
                    for (f.s = sockets.First(); f.s && !rxLen; f.s = f.s->next)
                    {
                        if (!f.s->IsAllocated())
                        {
                            TryAllocateImpl(*f.s);
                        }

                        if (f.s->NeedsConnect())
                        {
                            f.s->flags |= SocketFlags::ModemConnecting;
                            await(ConnectImpl, *f.s);
                        }

                        if (f.s->DataToSend())
                        {
                            await(SendPacketImpl, *f.s);
                            // always continue processing after send attempt
                            RequestProcessing();
                        }

                        if (f.s->DataToReceive())
                        {
                            if (f.s->CanReceive())
                            {
                                await(ReceivePacketImpl, *f.s);
                            }
                            else
                            {
                                // TODO: wait until the socket can receive data instead of polling
                                RequestProcessing();
                            }
                        }

                        if (f.s->DataToCheck() && f.s->CanReceive())
                        {
                            await(CheckIncomingImpl, *f.s);
                        }
                    }

                    // send messages
                    for (f.m = messages.First(); f.m && !rxLen; f.m = f.m->next)
                    {
                        if (f.m->ShouldSend())
                        {
                            if (!await(SendMessageImpl, *f.m))
                            {
                                f.m->SendingFailed();
                            }
                            // always continue processing after send attempt
                            RequestProcessing();
                        }
                    }

                    // remove processed messages
                    for (auto& manip: messages.Manipulate())
                    {
                        if (manip.Element().CanDelete())
                        {
                            DestroyMessage(&manip.Remove());
                        }
                    }

                    if (atResult != ATResult::OK)
                    {
                        MYDBG("AT sequence broken");
                        break;
                    }

                    if (!sockets && !messages)
                    {
                        signals -= Signal::RequireActive;
                        if (!await_mask_not_timeout(signals, Signal::RequireActive, 0, powerOffTimeout))
                        {
                            MYDBG("No activity for a while, turning off modem");
                            // further processing requests will force the modem to restart
                            process = false;
                            break;
                        }
                    }
                    else
                    {
                        async_yield();
                    }
                }

                if(requireLocation){                        
                        f.buff = lastKnownLocation;
                        await(GetLocation, f.buff);
                        Span location = Span(lastKnownLocation);
                        Span code = location.Consume(',');
                        Span lat = location.Consume(',');
                        Span lon = location.Consume(',');
                        Span acc = location.Consume(',');
                        if(code == Span("0")){
                            GsmLocation.lat = ParseLocationToInt(lat);
                            GsmLocation.lon = ParseLocationToInt(lon);
                        }
                } 

                signals = (signals & ~Signal::NetworkActive) | Signal::NetworkDisconnecting;   // disable further connections
                await(DisconnectNetworkImpl);
            }
        }

        await(StopImpl);
    }

    // finish all sockets
    for (f.s = sockets.First(); f.s && !rxLen; f.s = f.s->next)
    {
        f.s->Finished();
    }

    PowerDiagnostic(ModemOptions::CallbackType::PowerSend, "OFF");
    await(PowerOffImpl);
    tx.Close();
    options.OnPowerOff();
    PowerDiagnostic(ModemOptions::CallbackType::PowerReceive, "OFF");

    await_mask(signals, Signal::RxTaskActive, 0);

    signals &= ~Signal::TaskActive;
    OnTaskStopped();
    MYDBG("Stopped");

    if (process)
    {
        EnsureRunning();
    }
}
async_end

async(Modem::RxTask)
async_def(
    FNV1a hash;
    size_t len;
)
{
    while (await(rx.Require))
    {
        // need at least one character
        switch (rx.Peek(0))
        {
            case '>':
                rx.Advance(1);
                if (atTransmitSock)
                {
                    MYTRACE(TRACE_SOCKETS, "[%p] >> sending %d+%d=%d", atTransmitSock, atTransmitSock->OutputReader().Position(), atTransmitLen, atTransmitSock->OutputReader().Position() + atTransmitLen);
                    UNUSED size_t sent = await(atTransmitSock->OutputReader().CopyTo, tx, 0, atTransmitLen);
                    ASSERT(sent == atTransmitLen);
                    atTransmitSock = NULL;
                }
                else if (atTransmitMsg)
                {
                    MYTRACE(TRACE_SOCKETS, "[%p] >> sending message %b", atTransmitMsg, atTransmitMsg->Text());
                    UNUSED size_t sent = await(tx.Write, atTransmitMsg->Text());
                    ASSERT(sent == atTransmitMsg->Text().Length());
                    sent = await(tx.Write, BYTES(26));   // send CTRL+Z
                    ASSERT(sent);
                    atTransmitMsg = NULL;
                }
                else
                {
                    MYDBG("!! UNEXPECTED TRANSMIT PROMPT");
                }
                break;

            case '\r':
            case '\n':
            case ' ':
                // ignore whitespace characters
                rx.Advance(1);
                break;

            default:
                // EOL-terminated command
                size_t len = await(rx.RequireUntil, '\r');
                if (!len)
                {
                    if (rx.IsComplete())
                    {
                        // discard the remaining data
                        rx.Advance(rx.Available());
                    }
                    break;
                }

                lineEnd = rx.Position() + len;
#if TRACE && (MODEM_TRACE & TRACE_AT)
                DBGC("gsm", "<< ");
                for (char c: rx.Enumerate(len - 1)) _DBGCHAR(c);
                _DBGCHAR('\n');
#endif
                if (Buffer buf = options.GetDiagnosticBuffer(ModemOptions::CallbackType::CommandReceive))
                {
                    options.DiagnosticCallback(ModemOptions::CallbackType::CommandReceive, rx.Peek(buf.Left(len - 1)));
                }
                FNV1a hash;
                auto iter = rx.Enumerate(len - 1);
                bool digitsOnly = true;
                while (iter && *iter != ':')
                {
                    if (*iter == ',')
                    {
                        if (digitsOnly)
                        {
                            // calculate hash only for text after the comma
                            // for events with channel, such as "0, CONNECT OK"
                            ++iter;
                            if (iter && *iter == ' ')
                            {
                                ++iter;
                            }
                            hash = FNV1a();
                            continue;
                        }
                        else
                        {
                            // terminate at comma, include it in the event hash
                            // to disambiguate
                            hash += ',';
                            ++iter;
                            break;
                        }
                    }
                    else if (*iter < '0' || *iter > '9')
                    {
                        digitsOnly = false;
                    }

                    hash += *iter;
                    ++iter;
                }

                switch (hash)
                {
                    case fnv1a("OK"):
                        if (atResult == ATResult::Pending)
                        {
                            ATComplete();
                        }
                        else
                        {
                            MYDBG("!! Unexpected OK");
                        }
                        break;

                    case fnv1a("ERROR"):
                    case fnv1a("+CME ERROR"):
                    case fnv1a("+CMS ERROR"):
                        if (int(atResult) < 0)
                        {
                            atResult = ATResult::Error;
                            async_yield();  // let the task sending the command see the error
                        }
                        else
                            MYDBG("!! Unexpected Error");
                        break;

                    default:
                        if (*iter == ':')
                        {
                            ++iter;
                            if (iter && *iter == ' ')
                            {
                                ++iter;
                            }
                        }
                        lineFields = iter;
                        f.hash = hash;
                        if (!await(OnEvent, f.hash))
                        {
                            if (int(atResult) >= 0) // not pending
                            {
                                MYDBG("!! unexpected event");
                            }
                            else if (!atResponse)
                            {
                                MYDBG("!! unexpected AT response");
                            }
                            else
                            {
                                await(atResponse, f.hash);
                            }
                        }
                        break;
                }
                rx.AdvanceTo(lineEnd);
                if (rxLen)
                {
                    // skip '\n'
                    if (await(rx.Require))
                    {
                        if (rx.Peek(0) != '\n')
                        {
                            MYDBG("!! CRLF expected before incoming data");
                        }
                        rx.Advance(1);
                    }

                    if (rxSock)
                    {
                        MYTRACE(TRACE_SOCKETS, "[%p] << receiving %d+%d=%d", rxSock, rxSock->InputWriter().Position(), rxLen, rxSock->InputWriter().Position() + rxLen);
                    }
                    else
                    {
                        MYTRACE(TRACE_SOCKETS, "[???] << skipping %d bytes", rxLen);
                    }

                    do
                    {
                        // read at least one full segment of data
                        while (rx.Available() < rxLen && !rx.AvailableFullSegment())
                        {
                            f.len = rx.Available() + 1;
                            if (!await(rx.Require, f.len))
                            {
                                break;
                            }
                        }

                        f.len = std::min(rxLen, rx.GetSpan().Length());
                        if (!f.len)
                        {
                            break;
                        }
                        MYTRACE(TRACE_DATA, "[%p] [%d@%p] << %H", rxSock, rx.Position(), rx.GetSpan().Pointer(), rx.GetSpan().Left(f.len));
                        if (rxSock)
                        {
                            await(rx.MoveTo, rxSock->InputWriter(), f.len);
                            MYTRACE(TRACE_SOCKETS, "[%p] << received %d+%d=%d", rxSock, rxSock->InputWriter().Position() - io::PipePosition() - f.len, f.len, rxSock->InputWriter().Position());
                        }
                        else
                        {
                            // no socket, just skip
                            rx.Advance(f.len);
                            MYTRACE(TRACE_SOCKETS, "[???] << skipped %d", f.len);
                        }
                        rxLen -= f.len;
                    } while (rxLen);
                    rxLen = 0;
                    rxSock = NULL;
                    RequestProcessing();
                }
                break;
        }
    }

    MYDBG("RX Stopped");
    signals &= ~Signal::RxTaskActive;
}
async_end

async(Modem::ATLock)
async_def()
{
    if (!!(signals & Signal::ATLock))
    {
        if (atTask == &kernel::Task::Current())
        {
            async_return(false);
        }
    }

    if (modemStatus == ModemStatus::CommandError)
    {
        // we cannot continue executing commands once a command failed,
        // as the ordering in the AT protocol can be broken
        atResult = ATResult::Failure;
        async_return(true);
    }

    await_acquire(signals, Signal::ATLock);
    atTask = &kernel::Task::Current();
    atResult = ATResult::Pending;
    atRequire = 1;
    atComplete = 0;
    async_return(false);
}
async_end

async(Modem::AT, Span cmd)
async_def()
{
    if (await(ATLock))
    {
        async_return(int(ATResult::Failure));
    }

    MYTRACE(TRACE_AT, ">> AT%b", cmd);

    if (Buffer buf = options.GetDiagnosticBuffer(ModemOptions::CallbackType::CommandSend))
    {
        if (buf.Length() >= 2)
        {
            buf.Element<uint16_t>() = *(uint16_t*)"AT";
            cmd.CopyTo(buf.RemoveLeft(2));
            options.DiagnosticCallback(ModemOptions::CallbackType::CommandSend, buf.Left(cmd.Length() + 2));
        }
    }

    if (await(tx.Write, "AT") != 2 ||
        await(tx.Write, cmd) != (int)cmd.Length() ||
        await(tx.Write, "\r") != 1)
    {
        atNextTimeout = Timeout::Infinite;
        atResponse = {};
        atTask = NULL;
        signals &= ~Signal::ATLock;
        ModemStatus(ModemStatus::CommandError);
        async_return(int(atResult = ATResult::Failure));
    }

    async_return(await(ATResponse));
}
async_end

async(Modem::ATFormatV, const char* format, va_list va)
async_def()
{
    if (await(ATLock))
    {
        async_return(int(ATResult::Failure));
    }

#if TRACE && (MODEM_TRACE & TRACE_AT)
    DBGC("gsm", ">> AT");
    va_list va2;
    va_copy(va2, va);
    _DBGVA(format, va2);
    va_end(va2);
    _DBGCHAR('\n');
#endif

    if (Buffer buf = options.GetDiagnosticBuffer(ModemOptions::CallbackType::CommandSend))
    {
        if (buf.Length() >= 2)
        {
            va_list va3;
            va_copy(va3, va);
            buf.Element<uint16_t>() = *(uint16_t*)"AT";
            auto res = buf.RemoveLeft(2).FormatVA(format, va3);
            options.DiagnosticCallback(ModemOptions::CallbackType::CommandSend, Buffer(buf.Pointer(), res.end()));
        }
    }

    if (await(tx.Write, "AT") != 2 ||
        (format && format[0] && await(tx.WriteFV, Timeout::Infinite, format, va) <= 0) ||
        await(tx.Write, "\r") != 1)
    {
        atNextTimeout = Timeout::Infinite;
        atResponse = {};
        atTask = NULL;
        signals &= ~Signal::ATLock;
        ModemStatus(ModemStatus::CommandError);
        async_return(int(atResult = ATResult::Failure));
    }

    async_return(await(ATResponse));
}
async_end

async(Modem::ATResponse)
async_def(
    Timeout timeout;
)
{
    ASSERT(signals & Signal::ATLock);
    ASSERT(atTask == &kernel::Task::Current());

    f.timeout = (atNextTimeout || atTimeout).MakeAbsolute();
    atNextTimeout = Timeout::Infinite;

    if (!await_mask_not_timeout(atResult, 0x80, 0x80, f.timeout))
    {
        ModemStatus(ModemStatus::CommandError);
        atResult = ATResult::Timeout;
    }

    atResponse = {};
    atTask = NULL;
    signals &= ~Signal::ATLock;
    async_return(int(atResult));
}
async_end

unsigned Modem::InputFieldCount() const
{
    unsigned n = 0;
    bool empty = true;

    for (char c: lineFields)
    {
        empty = false;
        if (c == ',')
        {
            n++;
        }
    }

    if (!empty)
        n++;

    return n;
}

bool Modem::InputFieldNum(int& res, unsigned base)
{
    res = 0;
    auto iter = lineFields;

    bool neg = false;

    if (iter && (*iter == '+' || (neg = *iter == '-')))
        ++iter;

    bool hasDigit = false;
    bool error = false;
    while (iter)
    {
        char c = *iter;
        size_t digit;

        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (c >= 'a' && c <= 'z')
            digit = c + 10 - 'a';
        else if (c >= 'A' && c <= 'Z')
            digit = c + 10 - 'A';
        else
            break;

        if (digit >= base)
        {
            error = true;
            break;
        }

        res = res * base + digit;
        hasDigit = true;
        ++iter;
    }

    // skip the rest of the field
    while (iter)
    {
        bool eof = *iter == ',';
        ++iter;
        if (eof)
        {
            break;
        }
        else
        {
            // some other character before end of field
            error = true;
        }
    }

    lineFields = iter;
    if (neg)
    {
        res = -res;
    }
    return hasDigit && !error;
}

bool Modem::InputFieldFnv(uint32_t& res)
{
    FNV1a fnv;
    auto iter = lineFields;
    while (iter)
    {
        char c = *iter;
        ++iter;
        if (c == ',')
            break;
        fnv += c;
    }
    lineFields = iter;
    res = fnv;
    return true;
}

async(Modem::NetworkActive, Timeout timeout)
async_def()
{
    if (!(signals & Signal::TaskActive))
    {
        // we wait for network only if the task is already running
        async_return(false);
    }
    await_mask_not_timeout(signals, Signal::TaskActive | Signal::NetworkActive, Signal::TaskActive, timeout);
    async_return(!!(signals & Signal::NetworkActive));
}
async_end

void Modem::PowerDiagnostic(ModemOptions::CallbackType type, Span msg)
{
    if (Buffer buf = options.GetDiagnosticBuffer(type))
    {
        options.DiagnosticCallback(type, msg.CopyTo(buf));
    }
}

}
