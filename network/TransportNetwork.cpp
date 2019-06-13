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

    @file TransportNetwork.cpp
    @author Stan Kladko
    @date 2018
*/

#include "../Log.h"
#include "../SkaleCommon.h"
#include "../thirdparty/json.hpp"
#include "../abstracttcpserver/ConnectionStatus.h"
#include "../blockproposal/pusher/BlockProposalClientAgent.h"
#include "../blockproposal/received/ReceivedBlockProposalsDatabase.h"
#include "../blockproposal/server/BlockProposalWorkerThreadPool.h"
#include "../chains/Schain.h"
#include "../crypto/BLSSigShare.h"
#include "../crypto/SHAHash.h"
#include "../datastructures/BlockProposal.h"
#include "../exceptions/FatalError.h"
#include "../messages/NetworkMessage.h"
#include "../node/Node.h"
#include "../node/NodeInfo.h"
#include "../protocols/binconsensus/AUXBroadcastMessage.h"
#include "../protocols/binconsensus/BVBroadcastMessage.h"

#include "unordered_set"


#include "../exceptions/ExitRequestedException.h"
#include "../exceptions/InvalidMessageFormatException.h"
#include "../exceptions/InvalidSchainException.h"
#include "../exceptions/InvalidSourceIPException.h"
#include "../messages/Message.h"
#include "../protocols/blockconsensus/BlockConsensusAgent.h"

#include "../messages/NetworkMessageEnvelope.h"
#include "../network/Sockets.h"
#include "../network/ZMQServerSocket.h"
#include "Buffer.h"
#include "TransportNetwork.h"

TransportType TransportNetwork::transport = TransportType::ZMQ;


void TransportNetwork::addToDeferredMessageQueue( ptr< NetworkMessageEnvelope > _me ) {
    auto _blockID = _me->getMessage()->getBlockID();

    LOG( trace, "Deferring::" + to_string( _blockID ) );

    ASSERT( _me );

    ptr< vector< ptr< NetworkMessageEnvelope > > > messageList;

    {
        lock_guard< recursive_mutex > l( deferredMessageMutex );

        if ( deferredMessageQueue.count( _blockID ) == 0 ) {
            messageList = make_shared< vector< ptr< NetworkMessageEnvelope > > >();
            deferredMessageQueue[_blockID] = messageList;
        } else {
            messageList = deferredMessageQueue[_blockID];
        };

        messageList->push_back( _me );
    }
}

ptr< vector< ptr< NetworkMessageEnvelope > > > TransportNetwork::pullMessagesForBlockID(
    block_id _blockID ) {
    lock_guard< recursive_mutex > lock( deferredMessageMutex );


    auto returnList = make_shared< vector< ptr< NetworkMessageEnvelope > > >();


    for ( auto it = deferredMessageQueue.cbegin();
          it != deferredMessageQueue.cend() /* not hoisted */;
        /* no increment */ ) {
        if ( it->first <= _blockID ) {
            for ( auto&& msg : *( it->second ) ) {
                returnList->push_back( msg );
            }

            it = deferredMessageQueue.erase( it );
        } else {
            ++it;
        }
    }

    LOG( trace,
        "Pulling deferred BID::" + to_string( _blockID ) + ":" + to_string( returnList->size() ) );

    return returnList;
}

void TransportNetwork::broadcastMessage( Schain& subChain, ptr< NetworkMessage > m ) {
    if ( m->getBlockID() <= this->catchupBlocks ) {
        return;
    }

    auto ip = inet_addr( getSchain()->getThisNodeInfo()->getBaseIP()->c_str() );
    m->setIp( ip );
    node_id oldID = m->getDstNodeID();

    unordered_set< uint64_t > sent;

    while ( 3 * ( sent.size() + 1 ) < getSchain()->getNodeCount() * 2 ) {
        for ( auto const& it : subChain.getNode()->getNodeInfosByIndex() ) {
            auto index = ( uint64_t ) it.second->getSchainIndex() - 1;  // XXXX
            if ( index != ( subChain.getSchainIndex() - 1) && !sent.count( index ) ) { // XXXX
                m->setDstNodeID( it.second->getNodeID() );

                ASSERT( it.second->getSchainIndex()  != sChain->getSchainIndex() );  // XXXX

                if ( sendMessage( it.second, m ) ) {
                    sent.insert( ( uint64_t ) it.second->getSchainIndex() - 1 );  // XXXX
                }
            }
        }
    }

    if ( sent.size() + 1 < getSchain()->getNodeCount() ) {
        for ( auto const& it : subChain.getNode()->getNodeInfosByIndex() ) {
            auto index = ( uint64_t ) it.second->getSchainIndex() - 1;  /// XXXX
            if ( index != ( subChain.getSchainIndex() - 1)  && !sent.count( index ) ) { ///XXXX
                {
                    lock_guard< recursive_mutex > lock( delayedSendsLock );
                    delayedSends[index].push_back( {m, it.second} );
                    if ( delayedSends[index].size() > 256 ) {
                        delayedSends[index].pop_front();
                    }
                }
            }
        }
    }

    m->setDstNodeID( oldID );

    for ( auto const& it : subChain.getNode()->getNodeInfosByIndex() ) {
        if ( it.second->getSchainIndex()  != subChain.getSchainIndex() ) { // XXXX
            m->setDstNodeID( it.second->getNodeID() );
            confirmMessage( it.second );
        }
    }
}

void TransportNetwork::networkReadLoop() {
    setThreadName( __CLASS_NAME__ );

    waitOnGlobalStartBarrier();

    try {
        while ( !sChain->getNode()->isExitRequested() ) {
            try {
                ptr< NetworkMessageEnvelope > m = receiveMessage();

                if ( !m )
                    continue;  // check exit again

                if ( m->getMessage()->getBlockID() <= catchupBlocks ) {
                    continue;
                }

                ASSERT( sChain );

                block_id currentBlockID = sChain->getCommittedBlockID() + 1;

                postOrDefer( m, currentBlockID );
            } catch ( ExitRequestedException& ) {
                return;
            } catch ( FatalError& ) {
                throw;
            } catch ( Exception& e ) {
                if ( sChain->getNode()->isExitRequested() ) {
                    sChain->getNode()->getSockets()->consensusZMQSocket->closeReceive();
                    return;
                }
                Exception::logNested( e );
            }

        }  // while
    } catch ( FatalError& e ) {
        sChain->getNode()->exitOnFatalError( e.getMessage() );
    }

    sChain->getNode()->getSockets()->consensusZMQSocket->closeReceive();
}

void TransportNetwork::postOrDefer(
    const ptr< NetworkMessageEnvelope >& m, const block_id& currentBlockID ) {
    if ( m->getMessage()->getBlockID() > currentBlockID ) {
        addToDeferredMessageQueue( m );
    } else if ( m->getMessage()->getBlockID() <= currentBlockID ) {
        auto msg = ( NetworkMessage* ) m->getMessage().get();
        if ( msg->getRound() >
             sChain->getBlockConsensusInstance()->getRound( msg->createDestinationProtocolKey() ) +
                 1 ) {
            addToDeferredMessageQueue( m );
        } else if ( msg->getRound() == sChain->getBlockConsensusInstance()->getRound(
                                           msg->createDestinationProtocolKey() ) +
                                           1 &&
                    !sChain->getBlockConsensusInstance()->decided(
                        msg->createDestinationProtocolKey() ) ) {
            addToDeferredMessageQueue( m );
        } else {
            sChain->postMessage( m );
        }
    } else {
        sChain->postMessage( m );
    }
}

void TransportNetwork::deferredMessagesLoop() {
    setThreadName( __CLASS_NAME__ );

    waitOnGlobalStartBarrier();

    while ( !getSchain()->getNode()->isExitRequested() ) {
        ptr< vector< ptr< NetworkMessageEnvelope > > > deferredMessages;

        {
            block_id currentBlockID = sChain->getCommittedBlockID() + 1;

            deferredMessages = pullMessagesForBlockID( currentBlockID );
        }

        for ( auto message : *deferredMessages ) {
            block_id currentBlockID = sChain->getCommittedBlockID() + 1;
            postOrDefer( message, currentBlockID );
        }


        for ( int i = 0; i < getSchain()->getNodeCount(); i++ ) {
            if ( i != ( getSchain()->getSchainIndex() - 1)) {
                lock_guard< recursive_mutex > lock( delayedSendsLock );
                if ( delayedSends[i].size() > 0 ) {
                    if ( sendMessage(
                             delayedSends[i].front().second, delayedSends[i].front().first ) ) {
                        delayedSends[i].pop_front();
                    }
                }
            }
        }


        usleep( 100000 );
    }
}


void TransportNetwork::startThreads() {
    networkReadThread =
        make_shared< thread >( std::bind( &TransportNetwork::networkReadLoop, this ) );
    deferredMessageThread =
        make_shared< thread >( std::bind( &TransportNetwork::deferredMessagesLoop, this ) );

    WorkerThreadPool::addThread( networkReadThread );
    WorkerThreadPool::addThread( deferredMessageThread );
}

bool TransportNetwork::validateIpAddress( ptr< string >& ip ) {
    struct sockaddr_in sa;
    int result = inet_pton( AF_INET, ip.get()->c_str(), &( sa.sin_addr ) );
    return result != 0;
}


void TransportNetwork::waitUntilExit() {
    networkReadThread->join();
    deferredMessageThread->join();
}

ptr< string > TransportNetwork::ipToString( uint32_t _ip ) {
    char* ip = ( char* ) &_ip;
    return make_shared< string >(
        to_string( ( uint8_t ) ip[0] ) + "." + to_string( ( uint8_t ) ip[1] ) + "." +
        to_string( ( uint8_t ) ip[2] ) + "." + to_string( ( uint8_t ) ip[3] ) );
}

ptr< NetworkMessageEnvelope > TransportNetwork::receiveMessage() {
    auto buf = make_shared< Buffer >( CONSENSUS_MESSAGE_LEN );
    auto ip = readMessageFromNetwork( buf );


    if ( ip == nullptr ) {
        return nullptr;
    }

    uint64_t magicNumber;
    uint64_t sChainID;
    uint64_t blockID;
    uint64_t blockProposerIndex;
    MsgType msgType;
    uint64_t msgID;
    uint64_t srcNodeID;
    uint64_t dstNodeID;
    uint64_t round;
    uint8_t value;
    uint32_t rawIP;

    char sigShare[BLS_MAX_SIG_LEN + 1];

    memset( sigShare, 0, BLS_MAX_SIG_LEN );


    READ( buf, magicNumber );

    if ( magicNumber != MAGIC_NUMBER )
        return nullptr;

    READ( buf, sChainID );
    READ( buf, blockID );
    READ( buf, blockProposerIndex );
    READ( buf, msgType );
    READ( buf, msgID );
    READ( buf, srcNodeID );
    READ( buf, dstNodeID );
    READ( buf, round );
    READ( buf, value );
    READ( buf, rawIP );


    if ( sChain->getSchainID() != sChainID ) {
        BOOST_THROW_EXCEPTION(
            InvalidSchainException( "unknown Schain id" + to_string( sChainID ), __CLASS_NAME__ ) );
    }


    buf->read( sigShare, BLS_MAX_SIG_LEN ); /* Flawfinder: ignore */

    auto sig = make_shared< string >( sigShare );


    auto ip2 = ipToString( rawIP );
    if ( ip->size() == 0 ) {
        ip = ip2;
    } else {
        LOG( debug, ( *ip + ":" + *ip2 ).c_str() );
        ASSERT( *ip == *ip2 );
    }


    ptr< NodeInfo > realSender = sChain->getNode()->getNodeInfoByIP( ip );


    if ( realSender == nullptr ) {
        BOOST_THROW_EXCEPTION( InvalidSourceIPException( "NetworkMessage from unknown IP" + *ip ) );
    }


    ptr< NetworkMessage > mptr;

    if ( msgType == MsgType::BVB_BROADCAST ) {
        mptr = make_shared< BVBroadcastMessage >( node_id( srcNodeID ), node_id( dstNodeID ),
            block_id( blockID ), schain_index( blockProposerIndex ), bin_consensus_round( round ),
            bin_consensus_value( value ), schain_id( sChainID ), msg_id( msgID ), rawIP, sig,
            realSender->getSchainIndex()); //XXXX
    } else if ( msgType == MsgType::AUX_BROADCAST ) {
        mptr = make_shared< AUXBroadcastMessage >( node_id( srcNodeID ), node_id( dstNodeID ),
            block_id( blockID ), schain_index( blockProposerIndex ), bin_consensus_round( round ),
            bin_consensus_value( value ), schain_id( sChainID ), msg_id( msgID ), rawIP, sig,
            realSender->getSchainIndex()); //XXXX
    } else {
        ASSERT( false );
    }


    ASSERT( sChain );


    ptr< ProtocolKey > key = mptr->createDestinationProtocolKey();

    if ( key == nullptr ) {
        BOOST_THROW_EXCEPTION( InvalidMessageFormatException(
            "Network Message with corrupt protocol key", __CLASS_NAME__ ) );
    };

    return make_shared< NetworkMessageEnvelope >( mptr, realSender );
};


void TransportNetwork::setTransport( TransportType transport ) {
    TransportNetwork::transport = transport;
}

TransportType TransportNetwork::getTransport() {
    return transport;
}

uint32_t TransportNetwork::getPacketLoss() const {
    return packetLoss;
}

void TransportNetwork::setPacketLoss( uint32_t packetLoss ) {
    TransportNetwork::packetLoss = packetLoss;
}

void TransportNetwork::setCatchupBlocks( uint64_t _catchupBlocks ) {
    TransportNetwork::catchupBlocks = _catchupBlocks;
}

uint64_t TransportNetwork::getCatchupBlock() const {
    return catchupBlocks;
}


TransportNetwork::TransportNetwork( Schain& _sChain )
    : Agent( _sChain, false ), delayedSends( ( uint64_t ) _sChain.getNodeCount() ) {
    auto cfg = _sChain.getNode()->getCfg();

    if ( cfg.find( "catchupBlocks" ) != cfg.end() ) {
        uint64_t catchupBlock = cfg.at( "catchupBlocks" ).get< uint64_t >();
        setCatchupBlocks( catchupBlock );
    }

    if ( cfg.find( "packetLoss" ) != cfg.end() ) {
        uint32_t packetLoss = cfg.at( "packetLoss" ).get< uint64_t >();
        ASSERT( packetLoss <= 100 );
        setPacketLoss( packetLoss );
    }
}

void TransportNetwork::confirmMessage( const ptr< NodeInfo >& ){

};
