//
// Copyright (C) 2004 Andras Varga
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
//


#include <string.h>
#include "TCPMain.h"
#include "TCPConnection.h"
#include "TCPSegment_m.h"
#include "TCPCommand_m.h"
#include "IPControlInfo_m.h"
#include "TCPSendQueue.h"
#include "TCPReceiveQueue.h"
#include "TCPAlgorithm.h"


TCPStateVariables::TCPStateVariables()
{
    // set everything to 0 -- real init values will be set manually
    bool active = false;
    snd_mss = 512;
    snd_una = 0;
    snd_nxt = 0;
    snd_wnd = 0;
    snd_up = 0;
    snd_wl1 = 0;
    snd_wl2 = 0;
    iss = 0;
    rcv_nxt = 0;
    //rcv_wnd = 65536;
    rcv_wnd = 2048; // FIXME for testing only
    rcv_up = 0;
    irs = 0;

    fin_ack_rcvd = false;
    send_fin = false;
    snd_fin_seq = 0;
    fin_rcvd = false;
    rcv_fin_seq = 0;

}

std::string TCPStateVariables::detailedInfo() const
{
    std::stringstream out;
    out << "active = " << active << "\n";
    out << "snd_mss = " << snd_mss << "\n";
    out << "snd_una = " << snd_una << "\n";
    out << "snd_nxt = " << snd_nxt << "\n";
    out << "snd_wnd = " << snd_wnd << "\n";
    out << "snd_up = " << snd_up << "\n";
    out << "snd_wl1 = " << snd_wl1 << "\n";
    out << "snd_wl2 = " << snd_wl2 << "\n";
    out << "iss = " << iss << "\n";
    out << "rcv_nxt = " << rcv_nxt << "\n";
    out << "rcv_wnd = " << rcv_wnd << "\n";
    out << "rcv_up = " << rcv_up << "\n";
    out << "irs = " << irs << "\n";
    out << "fin_ack_rcvd = " << fin_ack_rcvd << "\n";
    return out.str();
}


//
// FSM framework, TCP FSM
//

TCPConnection::TCPConnection(TCPMain *_mod, int _appGateIndex, int _connId)
{
    tcpMain = _mod;
    appGateIndex = _appGateIndex;
    connId = _connId;

    localPort = remotePort = -1;

    char fsmname[24];
    sprintf(fsmname, "fsm-%d", connId);
    fsm.setName(fsmname);
    fsm.setState(TCP_S_INIT);


    // queues and algorithm will be created on active or passive open
    sendQueue = NULL;
    receiveQueue = NULL;
    tcpAlgorithm = NULL;
    state = NULL;

    the2MSLTimer = new cMessage("2MSL");
    connEstabTimer = new cMessage("CONN-ESTAB");
    finWait2Timer = new cMessage("FIN-WAIT-2");

    the2MSLTimer->setContextPointer(this);
    connEstabTimer->setContextPointer(this);
    finWait2Timer->setContextPointer(this);
}

TCPConnection::~TCPConnection()
{
    delete sendQueue;
    delete receiveQueue;

    delete tcpAlgorithm;
    delete state;

    delete tcpMain->cancelEvent(the2MSLTimer);
    delete tcpMain->cancelEvent(connEstabTimer);
    delete tcpMain->cancelEvent(finWait2Timer);
}

bool TCPConnection::processTimer(cMessage *msg)
{
    // first do actions
    TCPEventCode event;
    if (msg==the2MSLTimer)
    {
        event = TCP_E_TIMEOUT_2MSL;
        process_TIMEOUT_2MSL();
    }
    else if (msg==connEstabTimer)
    {
        event = TCP_E_TIMEOUT_CONN_ESTAB;
        process_TIMEOUT_CONN_ESTAB();
    }
    else if (msg==finWait2Timer)
    {
        event = TCP_E_TIMEOUT_FIN_WAIT_2;
        process_TIMEOUT_FIN_WAIT_2();
    }
    else
    {
        event = TCP_E_IGNORE;
        tcpAlgorithm->processTimer(msg, event);
    }

    // then state transitions
    return performStateTransition(event);
}

bool TCPConnection::processTCPSegment(TCPSegment *tcpseg, IPAddress segSrcAddr, IPAddress segDestAddr)
{
    if (!localAddr.isNull())
    {
        ASSERT(localAddr==segDestAddr);
        ASSERT(localPort==tcpseg->destPort());
    }
    if (!remoteAddr.isNull())
    {
        ASSERT(remoteAddr==segSrcAddr);
        ASSERT(remotePort==tcpseg->srcPort());
    }

    if (tryFastRoute(tcpseg))
        return true;

    // first do actions
    TCPEventCode event = process_RCV_SEGMENT(tcpseg, segSrcAddr, segDestAddr);

    // then state transitions
    return performStateTransition(event);
}

bool TCPConnection::processAppCommand(cMessage *msg)
{
    // first do actions
    TCPCommand *tcpCommand = (TCPCommand *)(msg->removeControlInfo());
    TCPEventCode event = preanalyseAppCommandEvent(msg->kind());
    ev << "App command: " << eventName(event) << "\n";
    switch (event)
    {
        case TCP_E_OPEN_ACTIVE: process_OPEN_ACTIVE(event, tcpCommand, msg); break;
        case TCP_E_OPEN_PASSIVE: process_OPEN_PASSIVE(event, tcpCommand, msg); break;
        case TCP_E_SEND: process_SEND(event, tcpCommand, msg); break;
        case TCP_E_CLOSE: process_CLOSE(event, tcpCommand, msg); break;
        case TCP_E_ABORT: process_ABORT(event, tcpCommand, msg); break;
        case TCP_E_STATUS: process_STATUS(event, tcpCommand, msg); break;
        default: opp_error("wrong event code");
    }

    // then state transitions
    return performStateTransition(event);
}


TCPEventCode TCPConnection::preanalyseAppCommandEvent(int commandCode)
{
    switch (commandCode)
    {
        case TCP_C_OPEN_ACTIVE:  return TCP_E_OPEN_ACTIVE;
        case TCP_C_OPEN_PASSIVE: return TCP_E_OPEN_PASSIVE;
        case TCP_C_SEND:         return TCP_E_SEND;
        case TCP_C_CLOSE:        return TCP_E_CLOSE;
        case TCP_C_ABORT:        return TCP_E_ABORT;
        case TCP_C_STATUS:       return TCP_E_STATUS;
        default: opp_error("Unknown message kind in app command");
                 return (TCPEventCode)0; // to satisfy compiler
    }
}

bool TCPConnection::performStateTransition(const TCPEventCode& event)
{
    ASSERT(fsm.state()!=TCP_S_CLOSED); // closed connections should be deleted immediately

    if (event==TCP_E_IGNORE)  // e.g. discarded segment
    {
        ev << "Staying in state: " << stateName(fsm.state()) << " (no FSM event)\n";
        return true;
    }

    // state machine
    // FIXME add handling of connection timeout event (keepalive), with transition to closed (?)
    int oldState = fsm.state();
    switch (fsm.state())
    {
        case TCP_S_INIT:
            switch (event)
            {
                case TCP_E_OPEN_PASSIVE:FSM_Goto(fsm, TCP_S_LISTEN); break;
                case TCP_E_OPEN_ACTIVE: FSM_Goto(fsm, TCP_S_SYN_SENT); break;
            }
            break;

        case TCP_S_LISTEN:
            switch (event)
            {
                case TCP_E_OPEN_ACTIVE: FSM_Goto(fsm, TCP_S_SYN_SENT); break;
                case TCP_E_SEND:        FSM_Goto(fsm, TCP_S_SYN_SENT); break;
                case TCP_E_CLOSE:       FSM_Goto(fsm, TCP_S_CLOSED); break;
                case TCP_E_ABORT:       FSM_Goto(fsm, TCP_S_CLOSED); break;
                case TCP_E_RCV_SYN:     FSM_Goto(fsm, TCP_S_SYN_RCVD);break;
            }
            break;

        case TCP_S_SYN_RCVD:
            switch (event)
            {
                case TCP_E_CLOSE:       FSM_Goto(fsm, TCP_S_FIN_WAIT_1); break;
                case TCP_E_ABORT:       FSM_Goto(fsm, TCP_S_CLOSED); break;
                case TCP_E_TIMEOUT_CONN_ESTAB: FSM_Goto(fsm, TCP_S_CLOSED); break;
                case TCP_E_RCV_RST:     FSM_Goto(fsm, state->active ? TCP_S_CLOSED : TCP_S_LISTEN); break;
                case TCP_E_RCV_ACK:     FSM_Goto(fsm, TCP_S_ESTABLISHED); break;
                case TCP_E_RCV_FIN:     FSM_Goto(fsm, TCP_S_CLOSE_WAIT); break;
                case TCP_E_RCV_UNEXP_SYN: FSM_Goto(fsm, TCP_S_CLOSED); break;
            }
            break;

        case TCP_S_SYN_SENT:
            switch (event)
            {
                case TCP_E_CLOSE:       FSM_Goto(fsm, TCP_S_CLOSED); break;
                case TCP_E_ABORT:       FSM_Goto(fsm, TCP_S_CLOSED); break;
                case TCP_E_TIMEOUT_CONN_ESTAB: FSM_Goto(fsm, TCP_S_CLOSED); break;
                case TCP_E_RCV_RST:     FSM_Goto(fsm, TCP_S_CLOSED); break;
                case TCP_E_RCV_SYN_ACK: FSM_Goto(fsm, TCP_S_ESTABLISHED); break;
                case TCP_E_RCV_SYN:     FSM_Goto(fsm, TCP_S_SYN_RCVD); break;
            }
            break;

        case TCP_S_ESTABLISHED:
            switch (event)
            {
                case TCP_E_CLOSE:       FSM_Goto(fsm, TCP_S_FIN_WAIT_1); break;
                case TCP_E_ABORT:       FSM_Goto(fsm, TCP_S_CLOSED); break;
                case TCP_E_RCV_FIN:     FSM_Goto(fsm, TCP_S_CLOSE_WAIT); break;
                case TCP_E_RCV_RST:     FSM_Goto(fsm, TCP_S_CLOSED); break;
                case TCP_E_RCV_UNEXP_SYN: FSM_Goto(fsm, TCP_S_CLOSED); break;
            }
            break;

        case TCP_S_CLOSE_WAIT:
            switch (event)
            {
                case TCP_E_CLOSE:       FSM_Goto(fsm, TCP_S_LAST_ACK); break;
                case TCP_E_ABORT:       FSM_Goto(fsm, TCP_S_CLOSED); break;
                case TCP_E_RCV_RST:     FSM_Goto(fsm, TCP_S_CLOSED); break;
                case TCP_E_RCV_UNEXP_SYN: FSM_Goto(fsm, TCP_S_CLOSED); break;
            }
            break;

        case TCP_S_LAST_ACK:
            switch (event)
            {
                case TCP_E_ABORT:       FSM_Goto(fsm, TCP_S_CLOSED); break;
                case TCP_E_RCV_ACK:     FSM_Goto(fsm, TCP_S_CLOSED); break;
                case TCP_E_RCV_RST:     FSM_Goto(fsm, TCP_S_CLOSED); break;
                case TCP_E_RCV_UNEXP_SYN: FSM_Goto(fsm, TCP_S_CLOSED); break;
            }
            break;

        case TCP_S_FIN_WAIT_1:
            switch (event)
            {
                case TCP_E_ABORT:       FSM_Goto(fsm, TCP_S_CLOSED); break;
                case TCP_E_RCV_FIN:     FSM_Goto(fsm, TCP_S_CLOSING); break;
                case TCP_E_RCV_ACK:     FSM_Goto(fsm, TCP_S_FIN_WAIT_2); break;
                case TCP_E_RCV_FIN_ACK: FSM_Goto(fsm, TCP_S_TIME_WAIT); break;
                case TCP_E_RCV_RST:     FSM_Goto(fsm, TCP_S_CLOSED); break;
                case TCP_E_RCV_UNEXP_SYN: FSM_Goto(fsm, TCP_S_CLOSED); break;
            }
            break;

        case TCP_S_FIN_WAIT_2:
            switch (event)
            {
                case TCP_E_ABORT:       FSM_Goto(fsm, TCP_S_CLOSED); break;
                case TCP_E_RCV_FIN:     FSM_Goto(fsm, TCP_S_TIME_WAIT); break;
                case TCP_E_TIMEOUT_FIN_WAIT_2: FSM_Goto(fsm, TCP_S_CLOSED); break;
                case TCP_E_RCV_RST:     FSM_Goto(fsm, TCP_S_CLOSED); break;
                case TCP_E_RCV_UNEXP_SYN: FSM_Goto(fsm, TCP_S_CLOSED); break;
            }
            break;

        case TCP_S_CLOSING:
            switch (event)
            {
                case TCP_E_ABORT:       FSM_Goto(fsm, TCP_S_CLOSED); break;
                case TCP_E_RCV_ACK:     FSM_Goto(fsm, TCP_S_TIME_WAIT); break;
                case TCP_E_RCV_RST:     FSM_Goto(fsm, TCP_S_CLOSED); break;
                case TCP_E_RCV_UNEXP_SYN: FSM_Goto(fsm, TCP_S_CLOSED); break;
            }
            break;

        case TCP_S_TIME_WAIT:
            switch (event)
            {
                case TCP_E_ABORT:       FSM_Goto(fsm, TCP_S_CLOSED); break;
                case TCP_E_TIMEOUT_2MSL: FSM_Goto(fsm, TCP_S_CLOSED); break;
                case TCP_E_RCV_RST:     FSM_Goto(fsm, TCP_S_CLOSED); break;
                case TCP_E_RCV_UNEXP_SYN: FSM_Goto(fsm, TCP_S_CLOSED); break;
            }
            break;

        case TCP_S_CLOSED:
            break;
    }

    if (oldState!=fsm.state())
        ev << "Transition: " << stateName(oldState) << " --> " << stateName(fsm.state()) << "  (event was: " << eventName(event) << ")\n";
    else
        ev << "Staying in state: " << stateName(fsm.state()) << " (event was: " << eventName(event) << ")\n";

    return fsm.state()!=TCP_S_CLOSED;
}


