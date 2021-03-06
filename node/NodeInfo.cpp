/*
    Copyright (C) 2018-2019 SKALE Labs

    This file is part of skale-consensus.

    skale-consensus is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published
    by the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    skale-consensus is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with skale-consensus.  If not, see <https://www.gnu.org/licenses/>.

    @file NodeInfo.cpp
    @author Stan Kladko
    @date 2018
*/

#include "SkaleCommon.h"
#include "Log.h"
#include "exceptions/FatalError.h"
#include "thirdparty/json.hpp"

#include "network/Sockets.h"
#include "NodeInfo.h"


using namespace std;


node_id NodeInfo::getNodeID() const {
    return nodeID;
}


schain_index NodeInfo::getSchainIndex() const {
    ASSERT(schainIndex > 0);
    return schainIndex;
}

network_port NodeInfo::getPort() const {
    ASSERT(port > 0);
    return port;
}


ptr<sockaddr_in> NodeInfo::getSocketaddr() {
    ASSERT(socketaddr);
    return socketaddr;
}

ptr<string> NodeInfo::getBaseIP() {
    CHECK_STATE( ipAddress );
    return ipAddress;
}


schain_id NodeInfo::getSchainID() const {
    return schainID;
}


NodeInfo::NodeInfo(node_id nodeID, ptr<string> &ip, network_port port, schain_id schainID, schain_index schainIndex) :
        nodeID(nodeID),
      ipAddress(ip),
        port(port),
        schainID(
                schainID),
        schainIndex(
                schainIndex) {
    ASSERT(schainIndex > 0);
    this->socketaddr = Sockets::createSocketAddress(ip, (uint16_t) port);
}
