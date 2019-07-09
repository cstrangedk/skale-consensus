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

    @file CommittedBlock.cpp
    @author Stan Kladko
    @date 2018
*/

#include <boost/iostreams/device/array.hpp>
#include <boost/iostreams/stream.hpp>

#include "../SkaleCommon.h"
#include "../Log.h"
#include "../exceptions/FatalError.h"


#include "../thirdparty/json.hpp"
#include "../crypto/SHAHash.h"
#include "../abstracttcpserver/ConnectionStatus.h"
#include "../headers/CommittedBlockHeader.h"
#include "../exceptions/NetworkProtocolException.h"
#include "../exceptions/ParsingException.h"
#include "../exceptions/InvalidArgumentException.h"


#include "../datastructures/Transaction.h"
#include "TransactionList.h"
#include "../network/Buffer.h"
#include "CommittedBlock.h"

CommittedBlock::CommittedBlock(Schain &_sChain, ptr<BlockProposal> _p) : BlockProposal(_sChain,
                                                                                       _p->getBlockID(),
                                                                                       _p->getProposerIndex(),
                                                                                       _p->getTransactionList(),
                                                                                       _p->getTimeStamp(),
                                                                                       _p->getTimeStampMs()) {
}


ptr<vector<uint8_t>> CommittedBlock::serialize() {



    auto items = transactionList->getItems();

    auto header = make_shared<CommittedBlockHeader>(*this);

    auto buf = header->toBuffer();

    ASSERT(buf->getBuf()->at(sizeof(uint64_t)) == '{');
    ASSERT(buf->getBuf()->at(buf->getCounter() - 1) == '}');


    uint64_t binSize = 0;

    for (auto &&tx: *items) {
        binSize += tx->getSerializedSize();
    }

    auto block = make_shared<vector<uint8_t>>();

    block->insert(block->end(), buf->getBuf()->begin(), buf->getBuf()->begin() + buf->getCounter());

    for (auto &&tx: *items) {
        tx->serializeInto(block);
    }

    ASSERT(block->at(sizeof(uint64_t)) == '{');

    return block;

}


CommittedBlock::CommittedBlock(ptr<vector<uint8_t>> _serializedBlock) : BlockProposal(0,0) {


    ASSERT(_serializedBlock != nullptr);

    auto size = _serializedBlock->size();

    if (size < sizeof(headerSize) + 2) {
        BOOST_THROW_EXCEPTION(InvalidArgumentException("Serialized block too small:" + to_string(size),
                                                       __CLASS_NAME__));
    }


    using boost::iostreams::array_source;
    using boost::iostreams::stream;

    array_source src((char*)_serializedBlock->data(), _serializedBlock->size());

    stream<array_source>  in(src);

    in.read((char*)&headerSize, sizeof(headerSize)); /* Flawfinder: ignore */


    if (headerSize < 2 || headerSize + sizeof(headerSize) > _serializedBlock->size()) {
        BOOST_THROW_EXCEPTION(InvalidArgumentException("Invalid header size" + to_string(headerSize), __CLASS_NAME__));
    }

    if (headerSize  > MAX_BUFFER_SIZE) {
        BOOST_THROW_EXCEPTION(InvalidArgumentException("Header size too large", __CLASS_NAME__));
    }

    auto header = make_shared<string>(headerSize, ' ');

    in.read((char*)header->c_str(), headerSize); /* Flawfinder: ignore */

    ptr<vector<size_t>> transactionSizes;

    try {
        transactionSizes =  parseBlockHeader(header);
    } catch (...) {
        throw_with_nested(ParsingException("Could not parse committed block header: \n" + *header, __CLASS_NAME__));
    }

    transactionList = TransactionList::deserialize(transactionSizes, _serializedBlock, headerSize);

    calculateHash();

}

ptr<vector<size_t>> CommittedBlock::parseBlockHeader(
        const shared_ptr<string> &header) {

    CHECK_ARGUMENT(header != nullptr);

    CHECK_ARGUMENT(header->size() > 2);

    if (header->at(0) != '{') {
        BOOST_THROW_EXCEPTION(InvalidArgumentException("Block header does not start with {", __CLASS_NAME__));
    }

    if (header->at(header->size() - 1 ) != '}') {
        BOOST_THROW_EXCEPTION(InvalidArgumentException("Block header does not end with }", __CLASS_NAME__));
    }

    auto transactionSizes = make_shared<vector<size_t>>();

    size_t totalSize = 0;

    auto js = nlohmann::json::parse(*header);

    proposerIndex = schain_index(Header::getUint64(js, "proposerIndex"));
    proposerNodeID = node_id(Header::getUint64(js, "proposerNodeID"));
    blockID = block_id(Header::getUint64(js, "blockID"));
    schainID = schain_id(Header::getUint64(js, "schainID"));
    timeStamp = Header::getUint64(js, "timeStamp");
    timeStampMs = Header::getUint32(js, "timeStampMs");

    transactionCount = js["sizes"].size();
    hash = SHAHash::fromHex(Header::getString(js, "hash"));


    Header::nullCheck(js, "sizes");
    nlohmann::json jsonTransactionSizes = js["sizes"];
    transactionCount = jsonTransactionSizes.size();

    for (auto &&jsize : jsonTransactionSizes) {
        transactionSizes->push_back(jsize);
        totalSize += (size_t) jsize;
    }

    return transactionSizes;

}
ptr< CommittedBlock > CommittedBlock::deserialize( ptr< vector< uint8_t > > _serializedBlock ) {
    return ptr< CommittedBlock >(new CommittedBlock(_serializedBlock));
};
