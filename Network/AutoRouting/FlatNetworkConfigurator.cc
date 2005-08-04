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

#ifdef _MSC_VER
#pragma warning(disable:4786)
#endif

#include <algorithm>
#include "RoutingTable.h"
#include "InterfaceTable.h"
#include "IPAddressResolver.h"
#include "FlatNetworkConfigurator.h"
#include "IPv4InterfaceData.h"


Define_Module(FlatNetworkConfigurator);


void FlatNetworkConfigurator::initialize(int stage)
{
    if (stage!=2) return;

    cTopology topo("topo");

    StringVector types = cStringTokenizer(par("moduleTypes"), " ").asVector();
    StringVector nonIPTypes = cStringTokenizer(par("nonIPModuleTypes"), " ").asVector();
    int i;
    for (i=0; i<nonIPTypes.size(); i++)
        types.push_back(nonIPTypes[i]);

    // extract topology
    topo.extractByModuleType(types);
    ev << "cTopology found " << topo.nodes() << " nodes\n";


    // assign IP addresses
    uint32 networkAddress = IPAddress(par("networkAddress").stringValue()).getInt();
    uint32 netmask = IPAddress(par("netmask").stringValue()).getInt();
    int maxNodes = (~netmask)-1;  // 0 and ffff have special meaning and cannot be used
    if (topo.nodes()>maxNodes)
        error("netmask too large, not enough addresses for all %d nodes", topo.nodes());

    // we'll store node addresses here
    std::vector<uint32> nodeAddresses;
    nodeAddresses.resize(topo.nodes());

    int numIPNodes = 0;
    for (i=0; i<topo.nodes(); i++)
    {
        // skip bus types
        if (isNonIPType(topo.node(i), nonIPTypes))
            continue;

        uint32 addr = networkAddress | uint32(++numIPNodes);
        nodeAddresses[i] = addr;

        // find interface table and assign address to all (non-loopback) interfaces
        cModule *mod = topo.node(i)->module();
        InterfaceTable *ift = IPAddressResolver().interfaceTableOf(mod);

        for (int k=0; k<ift->numInterfaces(); k++)
        {
            InterfaceEntry *ie = ift->interfaceAt(k);
            if (!ie->isLoopback())
            {
                ie->ipv4()->setInetAddress(IPAddress(addr));
                ie->ipv4()->setNetmask(IPAddress::ALLONES_ADDRESS); // full address must match for local delivery
            }
        }
    }

    // add default route to nodes with exactly one (non-loopback) interface
    std::vector<bool> usesDefaultRoute;
    usesDefaultRoute.resize(topo.nodes());
    for (i=0; i<topo.nodes(); i++)
    {
        cTopology::Node *node = topo.node(i);

        // skip bus types
        if (isNonIPType(topo.node(i), nonIPTypes))
            continue;

        InterfaceTable *ift = IPAddressResolver().interfaceTableOf(node->module());
        RoutingTable *rt = IPAddressResolver().routingTableOf(node->module());

        // count non-loopback interfaces
        int numIntf = 0;
        InterfaceEntry *ie = NULL;
        for (int k=0; k<ift->numInterfaces(); k++)
            if (!ift->interfaceAt(k)->isLoopback())
                {ie = ift->interfaceAt(k); numIntf++;}

        usesDefaultRoute[i] = (numIntf==1);
        if (numIntf!=1)
            continue; // only deal with nodes with one interface plus loopback

        ev << "  " << node->module()->fullName() << "=" << IPAddress(nodeAddresses[i])
           << " has only one (non-loopback) interface, adding default route\n";

        // add route
        RoutingEntry *e = new RoutingEntry();
        e->host = IPAddress();
        e->netmask = IPAddress();
        e->interfaceName = ie->name();
        e->interfacePtr = ie;
        e->type = RoutingEntry::REMOTE;
        e->source = RoutingEntry::MANUAL;
        //e->metric() = 1;
        rt->addRoutingEntry(e);
    }

    // fill in routing tables
    for (i=0; i<topo.nodes(); i++)
    {
        cTopology::Node *destNode = topo.node(i);

        // skip bus types
        if (isNonIPType(destNode, nonIPTypes))
            continue;

        uint32 destAddr = nodeAddresses[i];
        std::string destModName = destNode->module()->fullName();

        // calculate shortest paths from everywhere towards destNode
        topo.unweightedSingleShortestPathsTo(destNode);

        // add route (with host=destNode) to every routing table in the network
        // (excepting nodes with only one interface -- there we'll set up a default route)
        for (int j=0; j<topo.nodes(); j++)
        {
            if (i==j) continue;
            if (isNonIPType(topo.node(j), nonIPTypes))
                continue;

            cTopology::Node *atNode = topo.node(j);
            if (atNode->paths()==0)
                continue; // not connected
            if (usesDefaultRoute[j])
                continue; // already added default route here

            uint32 atAddr = nodeAddresses[j];

            int outputPort = atNode->path(0)->localGate()->index();
            ev << "  from " << atNode->module()->fullName() << "=" << IPAddress(atAddr);
            ev << " towards " << destModName << "=" << IPAddress(destAddr) << " outputPort=" << outputPort << endl;

            // add route
            InterfaceTable *ift = IPAddressResolver().interfaceTableOf(atNode->module());
            RoutingTable *rt = IPAddressResolver().routingTableOf(atNode->module());
            InterfaceEntry *ie = ift->interfaceByPortNo(outputPort);
            if (!ie)
                error("%s has no entry for interface %d", ift->fullPath().c_str(), outputPort);

            RoutingEntry *e = new RoutingEntry();
            e->host = IPAddress(destAddr);
            e->netmask = IPAddress(255,255,255,255); // full match needed
            e->interfaceName = ie->name();
            e->interfacePtr = ie;
            e->type = RoutingEntry::DIRECT;
            e->source = RoutingEntry::MANUAL;
            //e->metric() = 1;
            rt->addRoutingEntry(e);
        }
    }

    // update display string
    setDisplayString(numIPNodes, topo.nodes()-numIPNodes);
}

void FlatNetworkConfigurator::handleMessage(cMessage *msg)
{
    error("this module doesn't handle messages, it runs only in initialize()");
}

void FlatNetworkConfigurator::setDisplayString(int numIPNodes, int numNonIPNodes)
{
    // update display string
    char buf[80];
    sprintf(buf, "%d IP nodes\n%d non-IP nodes", numIPNodes, numNonIPNodes);
    displayString().setTagArg("t",0,buf);
}

bool FlatNetworkConfigurator::isNonIPType(cTopology::Node *node, StringVector& nonIPTypes)
{
    return std::find(nonIPTypes.begin(), nonIPTypes.end(), node->module()->className())!=nonIPTypes.end();
}

