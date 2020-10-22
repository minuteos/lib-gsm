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

async(Modem::WaitForIdle, Timeout timeout)
async_def()
{
    await_mask_timeout(sockets, ~0, 0, timeout);
}
async_end

Socket* Modem::CreateSocket(Span host, uint32_t port, bool tls)
{
    auto sock = (Socket*)malloc(sizeof(Socket) + host.Length() + 1);
    if (!sock)
        return NULL;

    new(sock) Socket(this, &process);
    sock->flags = SocketFlags::AppReference | (SocketFlags::AppSecure * tls);
    sock->port = port;
    memcpy(sock->host, host.Pointer(), host.Length());
    sock->host[host.Length()] = 0;

    sockets.Append(sock);

    EnsureRunning();

    MYDBG("Socket %p to %s:%d created", sock, sock->host, sock->port);
    return sock;
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

    // mark as released and request closure
    sock->flags = (sock->flags & ~SocketFlags::AppReference) | SocketFlags::AppClose;
    RequestProcessing();
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
    Socket* s;
    Socket* next;
)
{
    PowerDiagnostic(ModemOptions::CallbackType::CommandSend, "ON");
    while (!await(PowerOnImpl))
    {
        PowerDiagnostic(ModemOptions::CallbackType::CommandReceive, "ERR");
        ModemStatus(ModemStatus::PowerOnFailure);
        MYDBG("Power on failed. Will retry in 30 seconds.");
        async_delay_sec(30);
    }

    PowerDiagnostic(ModemOptions::CallbackType::CommandReceive, "ON");
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
                    MYTRACE(TRACE_SOCKETS, "Processing sockets...");

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

                    if (!sockets)
                    {
                        if (!await_mask_not_timeout(sockets, ~0u, 0, powerOffTimeout))
                        {
                            MYDBG("No sockets for a while, turning off modem");
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

                signals &= ~Signal::NetworkActive;   // disable further connections
                await(DisconnectNetworkImpl);
            }
        }

        await(StopImpl);
    }

    tx.Close();

    PowerDiagnostic(ModemOptions::CallbackType::CommandSend, "OFF");
    await(PowerOffImpl);
    PowerDiagnostic(ModemOptions::CallbackType::CommandReceive, "OFF");

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
                if (!atTransmitSock)
                {
                    MYDBG("!! UNEXPECTED TRANSMIT PROMPT");
                }
                else
                {
                    MYTRACE(TRACE_SOCKETS, "[%p] >> sending %d+%d=%d", atTransmitSock, atTransmitSock->OutputReader().Position(), atTransmitLen, atTransmitSock->OutputReader().Position() + atTransmitLen);
                    UNUSED size_t sent = await(atTransmitSock->OutputReader().MoveTo, tx, atTransmitLen);
                    MYTRACE(TRACE_SOCKETS, "[%p] >> sent up to %d", atTransmitSock, atTransmitSock->OutputReader().Position());
                    ASSERT(sent == atTransmitLen);
                    atTransmitSock = NULL;
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
                            atResult = ATResult::OK;
                        else
                            MYDBG("!! Unexpected OK");
                        break;

                    case fnv1a("ERROR"):
                    case fnv1a("+CME ERROR"):
                    case fnv1a("+CMS ERROR"):
                        if (atResult == ATResult::Pending)
                        {
                            atResult = ATResult::Error;
                            async_yield();  // let the task sendint the command see the error
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
                            if (atResult != ATResult::Pending)
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
            async_return(true);
        }
    }

    await_acquire(signals, Signal::ATLock);
    atTask = &kernel::Task::Current();
    async_return(true);
}
async_end

async(Modem::AT, Span cmd)
async_def()
{
    await(ATLock);

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

    atResult = ATResult::Pending;

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
    await(ATLock);

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

    atResult = ATResult::Pending;

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

    if (!await_mask_not_timeout(atResult, MASK(sizeof(atResult) * 8), ATResult::Pending, f.timeout))
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
        auto hdr = Span("!PWR:");
        if (buf.Length() >= hdr.Length())
        {
            memcpy(buf.Pointer(), hdr, hdr.Length());
            memcpy(buf.Pointer() + hdr.Length(), msg, std::min(buf.Length() - hdr.Length(), msg.Length()));
            options.DiagnosticCallback(type, buf.Left(hdr.Length() + msg.Length()));
        }
    }
}

}
