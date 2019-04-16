/*
    Copyright (C) 2018-2019 SKALE Labs

    This file is part of skale-consensus.

    skale-consensus is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    skale-consensus is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with skale-consensus.  If not, see <http://www.gnu.org/licenses/>.

    @file BlockProposalHeader.cpp
    @author Stan Kladko
    @date 2018
*/

#pragma  once

#include "AbstractBlockRequestHeader.h"



class BlockProposalHeader : public AbstractBlockRequestHeader{



    node_id proposerNodeID;
    ptr<string> hash;

    uint64_t partialHashesCount;
    uint64_t  timeStamp = 0;

public:

    BlockProposalHeader(Schain &_sChain, ptr<BlockProposal> proposal);



    void addFields(nlohmann::basic_json<> &jsonRequest) override;

};


