/*
 * Copyright (c) 2019 triaxis s.r.o.
 * Licensed under the MIT license. See LICENSE.txt file in the repository root
 * for full license information.
 *
 * gsm/SimComModem.cpp
 */

#include <gsm/SimComModem.h>

//#define SIMCOM_TRACE  1

#define MYDBG(...)  DBGCL("SimCom", __VA_ARGS__)

#if SIMCOM_TRACE
#define MYTRACE MYDBG
#else
#define MYTRACE(...)
#endif

namespace gsm
{

bool SimComModem::TryAllocateImpl(Socket& sock)
{
    switch (model)
    {
        case Model::SIM800:
        {
            // allocate any of the 6 sockets
            uint32_t avail = MASK(6);
            for (auto& other: Sockets())
            {
                if (other.IsAllocated())
                    RESBIT(avail, other.channel);
            }
            if (avail)
            {
                sock.Allocate(__builtin_ctz(avail));
                MYDBG("%s channel %d bound to socket %p", "TLS/TCP", sock.channel, &sock);
                return true;
            }
            break;
        }

        case Model::SIM7600:
        {
            // there are two TLS and 10 regular sockets
            uint32_t avail = MASK(sock.IsSecure() ? 2 : 10);
            for (auto& other: Sockets())
            {
                if (other.IsSecure() == sock.IsSecure() && other.IsAllocated())
                    RESBIT(avail, other.channel);
            }
            if (avail)
            {
                sock.Allocate(__builtin_ctz(avail));
                MYDBG("%s channel %d bound to socket %p", sock.IsSecure() ? "TLS" : "TCP", sock.channel, &sock);
                return true;
            }
            break;
        }

        default:
            MYDBG("Unsupported modem");
            break;
    }

    return false;
}

async(SimComModem::ConnectImpl, Socket& sock)
async_def()
{
    switch (model)
    {
        case Model::SIM800:
            if (await(ATFormat, "+CIPSSL=%d", sock.IsSecure()))
            {
                TcpStatus(TcpStatus::TlsError);
            }
            else if (await(ATFormat, "+CIPSTART=%d,\"TCP\",\"%s\",\"%d\"", sock.channel, sock.host, sock.port))
            {
                sock.Disconnected();
                TcpStatus(TcpStatus::ConnectionError);
            }
            else
            {
                sock.Bound();
                async_return(true);
            }
            break;

        case Model::SIM7600:
            if (sock.IsSecure())
            {
                if (!await(ATFormat, "+CCHOPEN=%d,\"%s\",%d,2", sock.channel, sock.host, sock.port))
                {
                    sock.Bound();
                    async_return(true);
                }
            }
            else
            {
                if (!await(ATFormat, "+CIPOPEN=%d,\"TCP\",\"%s\",%d", sock.channel, sock.host, sock.port))
                {
                    sock.Bound();
                    async_return(true);
                }
            }
            sock.Disconnected();
            TcpStatus(TcpStatus::ConnectionError);
            break;

        default:
            MYDBG("Unsupported modem");
            break;
    }
    async_return(false);
}
async_end

async(SimComModem::SendPacketImpl, Socket& sock)
async_def(
    size_t len
)
{
    f.len = std::min(size_t(MaxPacket), sock.OutputReader().Available());

    if (!f.len)
    {
        async_return(0);
    }

    await(ATLock);
    sock.Sending();
    NextATTransmit(sock, f.len);
    if (await(ATFormat, "+CCHSEND=%d,%d", sock.channel, f.len))
    {
        sock.SendingComplete();
        async_return(false);
    }
    async_return(true);
}
async_end

async(SimComModem::ReceivePacketImpl, Socket& sock)
async_def()
{
    sock.IncomingRequested();
    async_return(!await(ATFormat, "+CCHRECV=%d,%d", sock.channel, MaxPacket));
}
async_end

async(SimComModem::CheckIncomingImpl, Socket& sock)
async_def()
{
    sock.IncomingRequested();
    async_return(!await(AT, "+CCHRECV?"));
}
async_end

async(SimComModem::CloseImpl, Socket& sock)
async_def()
{
    switch (model)
    {
        case Model::SIM800:
            if (!await(ATFormat, "+CIPCLOSE=%d", sock.channel))
            {
                async_return(true);
            }
            break;

        case Model::SIM7600:
            if (sock.IsSecure())
            {
                if (!await(ATFormat, "+CCHCLOSE=%d", sock.channel))
                {
                    async_return(true);
                }
            }
            else
            {
                if (!await(ATFormat, "+CIPCLOSE=%d", sock.channel))
                {
                    async_return(true);
                }
            }
            break;

        default:
            MYDBG("Unsupported modem");
            break;
    }
    async_return(false);
}
async_end

async(SimComModem::PowerOnImpl)
async_def()
{
    powerEnable.Set();

    if (status)
    {
        MYDBG("Already powered on.");
    }
    else
    {
        // power on
        MYDBG("Powering on...");
        powerButton.Res();
        bool success = await(status.WaitFor, true, Timeout::Seconds(20));
        powerButton.Set();
        if (!success)
        {
            MYDBG("Power on failed...");
            powerEnable.Res();
            async_return(false);
        }
        MYDBG("Powered on.");
    }

    dtr.Set();
    async_delay_ms(50);

    gsmRx.Reset();
    gsmTx.Reset();

    await_multiple_init();
    await_multiple_add(usartRx, &io::USARTRxPipe::Start);
    await_multiple_add(usartTx, &io::USARTTxPipe::Start);
    await_multiple();

    async_return(true);
}
async_end

async(SimComModem::PowerOffImpl)
async_def()
{
    // TODO: soft power off

    await_multiple_init();
    await_multiple_add(usartRx, &io::USARTRxPipe::Stop, Timeout::Infinite);
    await_multiple_add(usartTx, &io::USARTTxPipe::Stop, Timeout::Infinite);
    await_multiple();

    // power off
    MYDBG("Powering off...");
    powerEnable.Res();
    async_delay_ms(100);
}
async_end

async(SimComModem::StartImpl)
async_def(
    unsigned i;
)
{
    MYDBG("Autobauding...");

    usartRx.GetUSART().BaudRate(115200);
    usartRx.GetUSART().FlowControlDisable();

    for (f.i = 0; f.i < 10; f.i++)
    {
        await(ATLock);
        NextATTimeout(Timeout::Milliseconds(100));
        if (!await(AT, Span()))
        {
            async_return(await(Initialize));
        }
    }

    MYDBG("Autobauding failed");
    ModemStatus(ModemStatus::AutoBaudFailure);
    async_return(false);
}
async_end

async(SimComModem::Initialize)
async_def()
{
    model = Model::Unknown;
    sim = {};
    net = {};
    gprs = {};

    if (await(AT, "E0"))    // turn off command echo
    {
        async_return(false);
    }

    // request modem identification
    await(ATLock);
    NextATResponse(GetDelegate(this, &SimComModem::OnReceiveId));
    if (await(AT, "I"))
    {
        async_return(false);
    }

    if (model == Model::Unknown)
    {
        MYDBG("Failed to determine model");
        async_return(false);
    }
    else
    {
        MYDBG("%s detected", ModelName());
    }

    if (Options().UseFlowControl())
    {
        MYDBG("Enabling handshaking");
        if (!await(ATFormat, "+IFC=2,2"))
        {
            usartRx.GetUSART().FlowControlEnable();
        }
    }

    MYDBG("Switching to %d baud", ModelBaudRate());
    if (!await(ATFormat, "+IPR=%d", ModelBaudRate()))
    {
        async_delay_ms(100);         // must wait, communicating too quickly confuses the module
        usartRx.GetUSART().BaudRate(ModelBaudRate());
    }

    if (model == Model::SIM800)
    {
        // additional identification
        await(ATLock);
        NextATResponse(GetDelegate(this, &SimComModem::OnReceiveId));
        if (await(AT, "+GSV"))    // additional identification
        {
            async_return(false);
        }
    }

    if (await(AT, "+CMEE=2") ||     // extended error reporting
        (model == Model::SIM800 && await(AT, "+CSDT=0")) ||     // SIM card detection off
        await(AT, "+CREG=2") ||     // extended network registration notifications
        await(AT, "+CGREG=2") ||    // extended GPRS network registration notifications
        // network timestamp notifications
        (model == Model::SIM800 && await(AT, "+CLTS=1")) ||
        (model == Model::SIM7600 && await(AT, "+CTZR=1")) ||
        // signal strength and error rate
        (model == Model::SIM800 && await(AT, "+EXUNSOL=\"SQ\",1")) ||
        (model == Model::SIM7600 && await(AT, "+AUTOCSQ=1,1")) ||
        // network info
        (model == Model::SIM800 && await(AT, "+CIEV=1")) ||
        (model == Model::SIM7600 && await(AT, "+CPSI=10")) ||
        false)
    {
        async_return(false);
    }

    async_return(true);
}
async_end

async(SimComModem::OnReceiveId, FNV1a header)
async_def_sync()
{
    if (header == "Model" &&
        Input().Matches("SIMCOM_") &&
        Input().Matches("SIM7600", 7))
    {
        model = Model::SIM7600;
    }
}
async_end

async(SimComModem::UnlockSimImpl)
async_def()
{
    if (!await(AT, "+CPIN?"))
    {
        if (sim.pinRequired && pin[0])
        {
            sim.pinUsed = !await(ATFormat, "+CPIN=\"%s\"", pin);
        }

        await_signal_sec(sim.ready, 5);

        if (sim.pinUsed)
        {
            if (removePin)
            {
                // remove the PIN from the card
                await(ATFormat, "+CLCK=\"SC\",0,\"%s\"", pin);
            }

            if (!sim.ready || removePin)
            {
                // forget PIN, if it has been removed successfully
                // or  used unsuccessfully to avoid locking the SIM
                pin[0] = 0;
            }
        }

        if (sim.ready)
        {
            async_return(true);
        }
        else if (sim.pinRequired)
        {
            SimStatus(sim.pinUsed ? SimStatus::BadPin : SimStatus::Locked);
        }
        else
        {
            SimStatus(SimStatus::NotInserted);
        }
    }
    else if (Input().Matches("+CME ERROR: "))
    {
        if (Input().Matches("SIM not inserted", 12))
        {
            SimStatus(SimStatus::NotInserted);
        }
    }

    async_return(false);
}
async_end

async(SimComModem::ConnectNetworkImpl)
async_def(
    Timeout timeout;
)
{
    if (await(AT, "+CREG?") ||
        await(AT, "+CGREG?") ||
        await(AT, "+COPS?") ||
        await(AT, "+CSQ"))
    {
        async_return(false);
    }

    MYDBG("Waiting for network...");
    if (!await_signal_sec(net.active, 120))
    {
        GsmStatus(GsmStatus::NoNetwork);
        async_return(false);
    }

    MYDBG("Waiting for GPRS...");
    if (!await(StartGprs))
    {
        TcpStatus(TcpStatus::GprsError);
        async_return(false);
    }

    async_return(true);
}
async_end

async(SimComModem::StartGprs)
async_def()
{
    if (!await_signal_sec(gprs.active, 5))
    {
        // attach GPRS if it's reported as not attached and voice is already registered
        MYDBG("Attaching GPRS...");
        if (await(AT, "+CGATT=1") || !await_signal_sec(gprs.active, 5))
        {
            async_return(false);
        }
    }

    MYDBG("Connecting GPRS...");
    if (model == Model::SIM800)
    {
        // enable socket multiplexing
        if (await(AT, "+CIPMUX=1") || await(AT, "+CIPQSEND=1"))
        {
            async_return(false);
        }
    }

    // define PDP context
    MYDBG("Connecting to APN: %b", Options().GetApn());
    if (await(ATFormat, "+CGDCONT=1,\"IP\",\"%b\"", Options().GetApn()))
    {
        async_return(false);
    }

    // activate PDP context
    await(ATLock);
    NextATTimeout(Timeout::Seconds(60));
    if (await(AT, "+CGACT=1,1"))
    {
        async_return(false);
    }

    if (model == Model::SIM800)
    {
        // start the data transfer task
        if (await(ATFormat, "+CSTT=\"%b\",\"%b\",\"%b\"", Options().GetApn(), Options().GetApnUser(), Options().GetApnPassword()))
        {
            async_return(false);
        }
        // activate GPRS
        await(ATLock);
        NextATTimeout(Timeout::Seconds(60));
        if (await(AT, "+CIICR"))
        {
            async_return(false);
        }

        // get local IP (doesn't send an OK reply, just one line with the address)
        if (await(AT, "+CIFSR"))
        {
            async_return(false);
        }
    }
    else
    {
        // configure GPRS auth
        if (Options().GetApnUser().Length() || Options().GetApnPassword().Length())
        {
            if (await(ATFormat, "+CGAUTH=1,3,\"%b\",\"%b\"", Options().GetApnUser(), Options().GetApnPassword()))
            {
                async_return(false);
            }
        }

        // activate TCP and TLS
        await(ATLock);
        NextATTimeout(Timeout::Seconds(60));
        if (await(AT, "+NETOPEN") || await(AT, "+CCHSET=1,0") || await(AT, "+CCHSTART"))
        {
            async_return(false);
        }

        // get local IP
        if (await(AT, "+IPADDR"))
        {
            async_return(false);
        }
    }

    async_return(true);
}
async_end

async(SimComModem::OnEvent, FNV1a hash)
async_def_sync()
{
    switch (hash)
    {
        case fnv1a("+CSQ"):
        {
            int rssi, ber;
            if (InputFieldNum(rssi) && InputFieldNum(ber))
            {
                net.rssi = rssi <= 31 ? -113 + rssi * 2 :
                    rssi >= 100 && rssi <= 191 ? -116 + rssi :
                    0;
                net.ber = ber <= 7 ? ber + 1 : 0;
                Rssi(net.rssi);
                MYDBG("RSSI: %d, BER: %s", net.rssi, STRINGS("UNK", "<0.01%", "<0.1%", "<0.5%", "<1%", "<2%", "<4%", "<8%", ">=8%")[net.ber]);
            }
            async_return(true);
        }

        case fnv1a("+CREG"):
        case fnv1a("+CGREG"):
        {
            int stat, lac, ci;
            bool isGprs = hash == fnv1a("+CGREG");
            RegBase& reg = *(isGprs ? (RegBase*)&gprs : &net);

            switch (InputFieldCount())
            {
                case 4:
                case 2:
                    // response to +CREG?, first field is mode
                    InputFieldNum(stat);
                    break;
            }

            if (InputFieldNum(stat))
            {
                reg.status = (Registration)stat;
                reg.active = reg.status == Registration::Home || reg.status == Registration::Roaming;

                GsmStatus(reg.status == Registration::Home ? GsmStatus::Ok :
                    reg.status == Registration::Roaming ? GsmStatus::Roaming :
                    GsmStatus::Searching);

                if (InputFieldHex(lac) && InputFieldHex(ci))
                {
                    reg.lac = lac;
                    reg.ci = ci;
                    MYDBG("%s: %s, LAC: %04X, CI: %04X", isGprs ? "GPRS" : "GSM", net.StatusName(), lac, ci);
                }
                else
                {
                    MYDBG("%s: %s", isGprs ? "GPRS" : "GSM", net.StatusName());
                }
            }
            async_return(true);
        }

        case fnv1a("+CPIN"):
            if (Input().Matches("READY"))
            {
                sim.ready = true;
            }
            async_return(true);

        case fnv1a("+CCHOPEN"):
        {
            int ch, status;
            if (InputFieldNum(ch) && InputFieldNum(status))
            {
                Socket* s = FindSocket(ch, true);
                if (!s)
                {
                    MYDBG("Status arrived for unallocated TLS socket %d", ch);
                }
                else
                {
                    if (!status)
                    {
                        MYDBG("%p connected", s);
                        s->Connected();
                    }
                    else
                    {
                        MYDBG("%p connection failed: %d", s, status);
                        s->Disconnected();
                    }
                    RequestProcessing();
                }
            }
            async_return(true);
        }

        case fnv1a("+CCHCLOSE"):
        case fnv1a("+CCH_PEER_CLOSED"):
        {
            int ch, status;
            if (InputFieldNum(ch) && (hash == fnv1a("+CCH_PEER_CLOSED") || InputFieldNum(status)))
            {
                Socket* s = FindSocket(ch, true);
                if (!s)
                {
                    MYDBG("Status arrived for unallocated TLS socket %d", ch);
                }
                else
                {
                    MYDBG("%p disconnected", s);
                    s->Disconnected();
                    RequestProcessing();
                }
            }
            async_return(true);
        }

        case fnv1a("+CCHRECV"):
        {
            uint32_t type;
            int ch, len, err;
            if (InputFieldCount() == 2 && InputFieldNum(ch) && InputFieldNum(err))
            {
                // end of receive
                Socket* s = FindSocket(ch, true);
                if (!s)
                {
                    MYDBG("End of receive arrived for unallocated TLS socket %d", ch);
                }
                else if (err)
                {
                    MYDBG("%p disconnected", s);
                    s->Disconnected();
                    RequestProcessing();
                }
                else
                {
                    // look for more data
                    s->MaybeIncoming();
                    RequestProcessing();
                }
            }
            else if (InputFieldFnv(type))
            {
                switch (type)
                {
                case fnv1a("DATA"):
                    if (InputFieldNum(ch) && InputFieldNum(len))
                    {
                        // data received for channel
                        Socket* s = FindSocket(ch, true);
                        if (!s)
                        {
                            MYDBG("Incoming %d bytes of data for unallocated TLS socket %d", len, ch);
                        }
                        else
                        {
                            MYTRACE("Incoming %d bytes of data for socket %p", len, s);
                        }
                        s->MaybeIncoming();
                        RequestProcessing();
                        ReceiveForSocket(s, len);
                    }
                    break;

                case fnv1a("LEN"):
                    for (ch = 0; InputFieldNum(len); ch++)
                    {
                        if (len)
                        {
                            Socket* s = FindSocket(ch, true);
                            if (!s)
                            {
                                MYDBG("Unallocated TLS socket %d has %d data in the buffer", ch, len);
                            }
                            else
                            {
                                MYTRACE("%d bytes of data in socket %p buffer", len, s);
                                s->Incoming();
                                RequestProcessing();
                            }
                        }
                    }
                    break;
                }
            }
            async_return(true);
        }

        case fnv1a("+CCHEVENT"):
        {
            int ch;
            uint32_t type;
            if (InputFieldNum(ch) && InputFieldFnv(type) && type == fnv1a("RECV EVENT"))
            {
                // buffered data received for channel
                Socket* s = FindSocket(ch, true);
                if (!s)
                {
                    MYDBG("Indicated incoming data for unallocated TLS socket %d", ch);
                }
                else
                {
                    MYTRACE("Indicated data for socket %p", s);
                }
                s->Incoming();
                RequestProcessing();
            }
            async_return(true);
        }

        case fnv1a("+CCHSEND"):
        {
            int ch, err;
            if (InputFieldNum(ch) && InputFieldNum(err))
            {
                // data sent
                Socket* s = FindSocket(ch, true);
                if (!s)
                {
                    MYDBG("Send confirmation (%d) for unallocaed TLS socket %d", err, ch);
                }
                else if (err)
                {
                    MYDBG("Sending failed (%d) for socket %p", err, s);
                }
                else
                {
                    MYTRACE("Packet sent for socket %p", s);
                    s->SendingComplete();
                    RequestProcessing();
                }
            }
            async_return(true);
        }

        case fnv1a("+CPSI"):
        {
            uint32_t tmp;
            InputFieldFnv(tmp);    // network type
            InputFieldFnv(tmp);
            struct N { unsigned value = 0, digits = 0; } mcc, mnc;
            N* n = &mcc;
            for (auto ch: Input())
            {
                if (ch >= '0' && ch <= '9')
                {
                    n->value = n->value * 10 + ch - '0';
                    n->digits++;
                }
                else if (ch == '-' && n == &mcc)
                {
                    n = &mnc;
                }
                else
                {
                    break;
                }
            }
            if (!mcc.digits || !(mnc.digits == 2 || mnc.digits == 3))
            {
                MYTRACE("Invalid MCC/MNC value");
            }
            NetworkInfo(gsm::NetworkInfo(mcc.value, mnc.value, mnc.digits));
            async_return(true);
        }
        case fnv1a("+CTZV"):
        case fnv1a("+CCHSTART"):
        case fnv1a("+COPS"):
        case fnv1a("+IPADDR"):
        case fnv1a("+NETOPEN"):
            // events we don't want to handle
            async_return(true);
    }

    async_return(false);
}
async_end

}
