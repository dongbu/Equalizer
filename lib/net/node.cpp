
/* Copyright (c) 2005-2008, Stefan Eilemann <eile@equalizergraphics.com> 
   All rights reserved. */

#include "node.h"

#include "command.h"
#include "connectionSet.h"
#include "global.h"
#include "pipeConnection.h"
#include "session.h"
#include "socketConnection.h"

#include <eq/base/base.h>
#include <eq/base/launcher.h>
#include <eq/base/rng.h>

#include <errno.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef WIN32_API
#  include <direct.h>  // for chdir
#  define chdir _chdir
#endif

using namespace eqBase;
using namespace std;

namespace eqNet
{
//----------------------------------------------------------------------
// State management
//----------------------------------------------------------------------
Node::Node()
        : _requestHandler( true )
        , _autoLaunch( false )
        , _id( true )
        , _state( STATE_STOPPED )
        , _receivedCommand( 0 )
        , _launchID( EQ_ID_INVALID )
        , _programName( Global::getProgramName( ))
        , _workDir( Global::getWorkDir( ))
{
    _receiverThread = new ReceiverThread( this );
    _commandThread  = new CommandThread( this );

    registerCommand( CMD_NODE_STOP, 
                     CommandFunc<Node>( this, &Node::_cmdStop ),
                     &_commandThreadQueue );
    registerCommand( CMD_NODE_MAP_SESSION, 
                     CommandFunc<Node>( this, &Node::_cmdMapSession ),
                     &_commandThreadQueue );
    registerCommand( CMD_NODE_MAP_SESSION_REPLY,
                     CommandFunc<Node>( this, &Node::_cmdMapSessionReply ),
                     &_commandThreadQueue );
    registerCommand( CMD_NODE_UNMAP_SESSION, 
                     CommandFunc<Node>( this, &Node::_cmdUnmapSession ),
                     &_commandThreadQueue );
    registerCommand( CMD_NODE_UNMAP_SESSION_REPLY,
                     CommandFunc<Node>( this, &Node::_cmdUnmapSessionReply ),
                     &_commandThreadQueue );
    registerCommand( CMD_NODE_CONNECT,
                     CommandFunc<Node>( this, &Node::_cmdConnect ), 0 );
    registerCommand( CMD_NODE_CONNECT_REPLY,
                     CommandFunc<Node>( this, &Node::_cmdConnectReply ), 0 );
    registerCommand( CMD_NODE_DISCONNECT,
                     CommandFunc<Node>( this, &Node::_cmdDisconnect ), 0 );
    registerCommand( CMD_NODE_GET_NODE_DATA,
                     CommandFunc<Node>( this, &Node::_cmdGetNodeData),
                     &_commandThreadQueue );
    registerCommand( CMD_NODE_GET_NODE_DATA_REPLY,
                     CommandFunc<Node>( this, &Node::_cmdGetNodeDataReply ),
                     &_commandThreadQueue );

    EQINFO << "New Node @" << (void*)this << " " << _id << endl;
}

Node::~Node()
{
    EQINFO << "Delete Node @" << (void*)this << " " << _id << endl;

    delete _commandThread;
    _commandThread = 0;

    delete _receiverThread;
    _receiverThread = 0;
}

bool Node::operator == ( const Node* node ) const
{ 
    EQASSERTINFO( _id != node->_id || this == node,
                  "Two node instances with the same ID found "
                  << (void*)this << " and " << (void*)node );

    return ( this == node );
}

bool Node::initLocal( const int argc, char** argv )
{
#ifndef NDEBUG
    EQINFO << disableFlush << "args: ";
    for( int i=0; i<argc; i++ )
         EQINFO << argv[i] << ", ";
    EQINFO << endl << enableFlush;
#endif

    // We do not use getopt_long because it really does not work due to the
    // following aspects:
    // - reordering of arguments
    // - different behaviour of GNU and BSD implementations
    // - incomplete man pages
    bool   isClient   = false;
    bool   isResident = false;
    string clientOpts;

    for( int i=1; i<argc; ++i )
    {
        if( string( "--eq-listen" ) == argv[i] )
        {
            if( i<argc && argv[i+1][0] != '-' )
            {
                string                        data = argv[++i];
                ConnectionDescriptionPtr desc = new ConnectionDescription;
                desc->TCPIP.port = Global::getDefaultPort();

                if( desc->fromString( data ))
                {
                    addConnectionDescription( desc );
                    EQASSERTINFO( data.empty(), data );
                }
                else
                    EQWARN << "Ignoring listen option: " << argv[i] << endl;
            }
        }
        else if( string( "--eq-client" ) == argv[i] )
        {
            isClient = true;
            if( i<argc && argv[i+1][0] != '-' ) // server-started client
            {
                clientOpts = argv[++i];

                if( !deserialize( clientOpts ))
                    EQWARN << "Failed to parse client listen port parameters"
                           << endl;
                EQASSERT( !clientOpts.empty( ));
            }
            else // resident render client
                isResident = true;
        }
    }
    
    if( _connectionDescriptions.empty( )) // add default listener
    {
        ConnectionDescriptionPtr connDesc = new ConnectionDescription;
        connDesc->type       = CONNECTIONTYPE_TCPIP;
        connDesc->TCPIP.port = Global::getDefaultPort();
        addConnectionDescription( connDesc );
    }

    EQINFO << "Listener data: " << serialize() << endl;

    if( !listen( ))
    {
        EQWARN << "Can't setup listener(s)" << endl; 
        return false;
    }
    
    if( isClient )
    {
        EQINFO << "Client node started from command line with option " 
               << clientOpts << endl;

        bool ret = (isResident ? clientLoop() : runClient( clientOpts ));

        EQINFO << "Exit node process " << getRefCount() << endl;
        ret &= exitClient();
        ::exit( ret ? EXIT_SUCCESS : EXIT_FAILURE );
    }

    return true;
}

bool Node::listen()
{
    if( _state != STATE_STOPPED )
        return false;

    if( !_listenToSelf( ))
        return false;

    for( vector< ConnectionDescriptionPtr >::const_iterator i = 
             _connectionDescriptions.begin();
         i != _connectionDescriptions.end(); ++i )
    {
        ConnectionDescriptionPtr desc = *i;
        ConnectionPtr      connection = Connection::create( desc->type );
        connection->setDescription( desc );
        if( !connection->listen( ))
        {
            EQWARN << "Can't create listener connection: " << desc << endl;
            return false;
        }

        connection.ref();
        _connectionNodes[ connection.get() ] = this;
        _connectionSet.addConnection( connection );
    }

    _state = STATE_LISTENING;

    EQVERB << typeid(*this).name() << " starting command and receiver thread "
           << endl;
    _commandThread->start();
    _receiverThread->start();

    EQINFO << this << " listening." << endl;
    return true;
}

bool Node::stopListening()
{
    if( _state != STATE_LISTENING )
        return false;

    NodeStopPacket packet;
    send( packet );

    const bool recvJoined = _receiverThread->join();
    EQASSERT( recvJoined );

    const bool cmdJoined = _commandThread->join();
    EQASSERT( cmdJoined );

    _cleanup();

    EQINFO << _connectionSet.size() << " connections open after stopListening"
           << endl;
#ifndef NDEBUG
    const ConnectionVector& connections = _connectionSet.getConnections();
    for( ConnectionVector::const_iterator i = connections.begin();
         i != connections.end(); ++i )

        EQINFO << "    " << *i << endl;
#endif
    return true;
}

void Node::_cleanup()
{
    EQINFO << "Clean up stopped node" << endl;
    EQASSERTINFO( _state == STATE_STOPPED, _state );
    EQASSERT( _connection.isValid( ));

    _connectionSet.removeConnection( _connection );
    _connectionNodes.erase( _connection.get( ));
    _connection.unref();
    _connection = 0;

    const ConnectionVector& connections = _connectionSet.getConnections();
    for( ConnectionVector::const_iterator i = connections.begin(); 
         i != connections.end(); ++i )
    {
        ConnectionPtr connection = *i;
        NodePtr       node       = _connectionNodes[ connection.get() ];

        node->_state      = STATE_STOPPED;
        node->_connection = 0;
        connection.unref();
    }

    _connectionSet.clear();
    _connectionNodes.clear();
    _nodes.clear();
}

bool Node::_listenToSelf()
{
    // setup local connection to myself
    _connection = new PipeConnection;
    if( !_connection->connect())
    {
        EQERROR << "Could not create local connection to receiver thread."
                << endl;
        _connection = 0;
        return false;
    }

    // add to connection set
    EQASSERT( _connection->getDescription().isValid( )); 
    EQASSERT( _connectionNodes.find(_connection.get())==_connectionNodes.end());

    _connection.ref();
    _connectionNodes[ _connection.get() ] = this;
    _connectionSet.addConnection( _connection );
    _nodes[ _id ] = this;
    return true;
}

bool Node::connect( NodePtr node, ConnectionPtr connection )
{
    EQASSERT( connection.isValid( ));

    if( !node.isValid() || _state != STATE_LISTENING ||
        connection->getState() != Connection::STATE_CONNECTED ||
        node->_state != STATE_STOPPED )
        return false;

    // send connect packet to peer
    eqNet::NodeConnectPacket packet;

    packet.requestID = _requestHandler.registerRequest( node.get( ));
    packet.nodeID    = _id;
    packet.nodeID.convertToNetwork();

    packet.type      = getType();
    packet.launchID  = node->_launchID;
    node->_launchID  = EQ_ID_INVALID;

    _connectionSet.addConnection( connection );
    connection->send( packet, serialize( ));

    bool connected = false;
    _requestHandler.waitRequest( packet.requestID, connected );
    if( !connected )
        return false;

    EQASSERT( node->_id != NodeID::ZERO );
    EQASSERTINFO( node->_id != _id, _id );
    EQINFO << node << " connected to " << this << endl;
    return true;
}

NodePtr Node::getNode( const NodeID& id ) const
{ 
    NodeIDHash< NodePtr >::const_iterator iter = _nodes.find( id );
    if( iter == _nodes.end( ))
        return 0;
    return iter->second;
}

bool Node::disconnect( NodePtr node )
{
    if( !node || _state != STATE_LISTENING || 
        node->_state != STATE_CONNECTED || !node->_connection )
        return false;
    EQASSERT( !inCommandThread( ));

    NodeDisconnectPacket packet;
    packet.requestID = _requestHandler.registerRequest( node.get( ));
    send( packet );

    _requestHandler.waitRequest( packet.requestID );
    return true;
}

void Node::addConnectionDescription( ConnectionDescriptionPtr cd )
{
    _connectionDescriptions.push_back( cd ); 
}

//----------------------------------------------------------------------
// Node functionality
//----------------------------------------------------------------------
void Node::addSession( Session* session, NodePtr server, 
                       const uint32_t sessionID, const string& name )
{
    CHECK_THREAD( _thread );

    session->setLocalNode( this );
    session->_server    = server;
    session->_id        = sessionID;
    session->_name      = name;
    session->_isMaster  = ( server==this && isLocal( ));

    _sessions[sessionID] = session;

    EQINFO << (session->_isMaster ? "master" : "client") << " session, id "
         << sessionID << ", name " << name << ", served by " << server.get() 
         << ", managed by " << this << endl;
}

void Node::removeSession( Session* session )
{
    CHECK_THREAD( _thread );
    _sessions.erase( session->getID( ));

    session->setLocalNode( 0 );
    session->_server    = 0;
    session->_id        = EQ_ID_INVALID;
    session->_name      = "";
    session->_isMaster  = false;
}

bool Node::mapSession( NodePtr server, Session* session, 
                       const string& name )
{
    EQASSERT( isLocal( ));
    EQASSERT( !inCommandThread( ));

    NodeMapSessionPacket packet;
    packet.requestID  = _requestHandler.registerRequest( session );
    server->send( packet, name );

    bool ret = false;
    _requestHandler.waitRequest( packet.requestID, ret );
    return ret;
}

bool Node::mapSession( NodePtr server, Session* session, const uint32_t id)
{
    EQASSERT( isLocal( ));
    EQASSERT( id != EQ_ID_INVALID );

    NodeMapSessionPacket packet;
    packet.requestID = _requestHandler.registerRequest( session );
    packet.sessionID =  id;
    server->send( packet );

    bool ret = false;
    _requestHandler.waitRequest( packet.requestID, ret );
    return ret;
}

bool Node::unmapSession( Session* session )
{
    EQASSERT( isLocal( ));

    NodeUnmapSessionPacket packet;
    packet.requestID = _requestHandler.registerRequest( session );
    packet.sessionID =  session->getID();
    session->getServer()->send( packet );

    bool ret = false;
    _requestHandler.waitRequest( packet.requestID, ret );
    return ret;
}

uint32_t Node::_generateSessionID()
{
    CHECK_THREAD( _thread );
    RNG      rng;
    uint32_t id  = rng.get<uint32_t>();

    while( id == EQ_ID_INVALID || _sessions.find( id ) != _sessions.end( ))
        id = rng.get<uint32_t>();

    return id;  
}

#define SEPARATOR '#'

std::string Node::serialize() const
{
    ostringstream data;
    data << _id << SEPARATOR << _connectionDescriptions.size() << SEPARATOR;

    for( vector< ConnectionDescriptionPtr >::const_iterator i = 
             _connectionDescriptions.begin(); 
         i != _connectionDescriptions.end(); ++i )
    {
        ConnectionDescriptionPtr desc = *i;
        desc->serialize( data );
    }
    
    return data.str();
}
 
bool Node::deserialize( std::string& data )
{
    EQASSERT( getState() == STATE_STOPPED || getState() == STATE_LAUNCHED );

    EQINFO << "Node data: " << data << endl;
    if( !_connectionDescriptions.empty( ))
        EQWARN << "Node already holds data while deserializing it" << endl;

    // node id
    size_t nextPos = data.find( SEPARATOR );
    if( nextPos == string::npos || nextPos == 0 )
    {
        EQERROR << "Could not parse node data" << endl;
        return false;
    }

    _id = data.substr( 0, nextPos );
    data = data.substr( nextPos + 1 );

    // num connection descriptions
    nextPos = data.find( SEPARATOR );
    if( nextPos == string::npos || nextPos == 0 )
    {
        EQERROR << "Could not parse node data" << endl;
        return false;
    }

    const string sizeStr = data.substr( 0, nextPos );
    if( !isdigit( sizeStr[0] ))
    {
        EQERROR << "Could not parse node data" << endl;
        return false;
    }

    const size_t nDesc = atoi( sizeStr.c_str( ));
    data = data.substr( nextPos + 1 );

    // connection descriptions
    for( size_t i = 0; i<nDesc; ++i )
    {
        ConnectionDescriptionPtr desc = new ConnectionDescription;
        if( !desc->fromString( data ))
        {
            EQERROR << "Error during node connection data parsing" << endl;
            return false;
        }
        addConnectionDescription( desc );
    }

    return true;
}

//----------------------------------------------------------------------
// receiver thread functions
//----------------------------------------------------------------------
void* Node::_runReceiverThread()
{
    EQINFO << "Entered receiver thread" << endl;

    _receivedCommand = new Command;

    int nErrors = 0;
    while( _state == STATE_LISTENING )
    {
        const ConnectionSet::Event result = _connectionSet.select();
        switch( result )
        {
            case ConnectionSet::EVENT_CONNECT:
                _handleConnect();
                break;

            case ConnectionSet::EVENT_DATA:      
                _handleData();
                break;

            case ConnectionSet::EVENT_DISCONNECT:
            case ConnectionSet::EVENT_INVALID_HANDLE:
            {
                _handleDisconnect();
                EQVERB << &_connectionSet << endl;
                break;
            } 

            case ConnectionSet::EVENT_TIMEOUT:   
                EQINFO << "select timeout" << endl;
                break;

            case ConnectionSet::EVENT_ERROR:      
                ++nErrors;
                EQWARN << "Connection signalled error during select" << endl;
                if( nErrors > 100 )
                {
                    EQWARN << "Too many errors in a row, capping connection" 
                           << endl;
                    _handleDisconnect();
                }
                break;

            case ConnectionSet::EVENT_SELECT_ERROR:      
                EQWARN << "Error during select" << endl;
                ++nErrors;
                if( nErrors > 10 )
                {
                    EQWARN << "Too many errors in a row" << endl;
                    EQUNIMPLEMENTED;
                }
                break;

            case ConnectionSet::EVENT_INTERRUPT:
                _redispatchCommands();
                break;

            default:
                EQUNIMPLEMENTED;
        }
        if( result != ConnectionSet::EVENT_ERROR && 
            result != ConnectionSet::EVENT_SELECT_ERROR )

            nErrors = 0;
    }

    if( !_pendingCommands.empty( ))
        EQWARN << _pendingCommands.size() 
               << " commands pending while leaving command thread" << endl;

    for( list<Command*>::const_iterator i = _pendingCommands.begin();
         i != _pendingCommands.end(); ++i )
            
        _commandCache.release( *i );

    _pendingCommands.clear();
    _commandCache.flush();

    delete _receivedCommand;
    _receivedCommand = 0;

    EQINFO << "Leaving receiver thread" << endl;
    return EXIT_SUCCESS;
}

void Node::_handleConnect()
{
    ConnectionPtr connection = _connectionSet.getConnection();
    ConnectionPtr newConn    = connection->accept();

    if( !newConn )
    {
        EQINFO << "Received connect event, but accept() failed" << endl;
        return;
    }

    _connectionSet.addConnection( newConn );
    // Node will be created when receiving NodeConnectPacket from other side
}

void Node::_handleDisconnect()
{
    while( _handleData( )) ; // read remaining data off connection

    ConnectionPtr connection = _connectionSet.getConnection();
    NodePtr       node;
    if( _connectionNodes.find( connection.get( )) != _connectionNodes.end( ))
        node = _connectionNodes[ connection.get() ];

    EQASSERT( !node || node->_connection == connection );

    if( node.isValid( ))
    {
        node->_state      = STATE_STOPPED;
        node->_connection = 0;
        _connectionNodes.erase( connection.get( ));
        _nodes.erase( node->_id );
        connection.unref();
    }

    _connectionSet.removeConnection( connection );

    EQINFO << node << " disconnected from " << this << " connection used " 
           << connection->getRefCount() << endl;
    //connection->close();
}

bool Node::_handleData()
{
    ConnectionPtr connection = _connectionSet.getConnection();
    NodePtr       node;

    if( _connectionNodes.find( connection.get( )) != _connectionNodes.end( ))
        node = _connectionNodes[ connection.get() ];

    EQASSERT( connection.isValid( ));
    EQASSERTINFO( !node || node->_connection == connection, 
                  typeid( *node.get( )).name( ));
    EQVERB << "Handle data from " << node << endl;

    uint64_t size;
    const bool gotSize = connection->recv( &size, sizeof( size ));
    if( !gotSize ) // Some systems signal data on dead connections.
        return false;

    EQASSERT( size );
    _receivedCommand->allocate( node, this, size );
    size -= sizeof( size );

    char* ptr = reinterpret_cast< char* >(_receivedCommand->getPacket()) + 
                    sizeof( size );
    const bool gotData = connection->recv( ptr, size );

    EQASSERT( gotData );
    EQASSERT( _receivedCommand->isValid( ));

    if( !gotData )
    {
        EQERROR << "Incomplete packet read: " << *_receivedCommand << endl;
        return false;
    }

    // This is one of the initial packets during the connection handshake, at
    // this point the remote node is not yet available.
    EQASSERTINFO( node.isValid() ||
                  (*_receivedCommand)->datatype == DATATYPE_EQNET_NODE &&
                  ( (*_receivedCommand)->command == CMD_NODE_CONNECT  || 
                    (*_receivedCommand)->command == CMD_NODE_CONNECT_REPLY),
                  *_receivedCommand << " connection " << connection );

    _dispatchCommand( *_receivedCommand );

#ifndef NDEBUG
    // Unref nodes in command to keep node ref counts easier for debugging.
    // Release builds will unref the nodes at receiver thread exit.
    _receivedCommand->allocate( 0, 0, 1 );
#endif
    return true;
}

void Node::_dispatchCommand( Command& command )
{
    EQASSERT( command.isValid( ));

    const bool dispatched = dispatchCommand( command );
 
    _redispatchCommands();
  
    if( !dispatched )
    {
        Command* dispCommand = _commandCache.alloc( command );
        _pendingCommands.push_back( dispCommand );

#ifndef NDEBUG
        if( (_pendingCommands.size() % 10) == 9 )
            EQINFO << _pendingCommands.size() << " commands pending" <<endl;
#endif
    }
}

bool Node::dispatchCommand( Command& command )
{
    EQVERB << "dispatch " << command << " by " << _id << endl;
    EQASSERT( command.isValid( ));

    const uint32_t datatype = command->datatype;
    switch( datatype )
    {
        case DATATYPE_EQNET_NODE:
            return Base::dispatchCommand( command );

        case DATATYPE_EQNET_SESSION:
        case DATATYPE_EQNET_OBJECT:
        {
            const SessionPacket* sessionPacket = 
                static_cast<SessionPacket*>( command.getPacket( ));
            const uint32_t       id            = sessionPacket->sessionID;
            Session*             session       = _sessions[id];
            EQASSERTINFO( session, "Can't find session for " << sessionPacket );
            
            return session->dispatchCommand( command );
        }

        default:
            EQASSERTINFO( 0, "Unknown datatype " << datatype << " for "
                          << command );
            return true;
    }
}

void Node::_redispatchCommands()
{
    bool changes = true;
    while( changes && !_pendingCommands.empty( ))
    {
        changes = false;

        for( list<Command*>::iterator i = _pendingCommands.begin();
             i != _pendingCommands.end(); ++i )
        {
            Command* command = (*i);
            EQASSERT( command->isValid( ));

            if( dispatchCommand( *command ))
            {
                _pendingCommands.erase( i );
                _commandCache.release( command );
                changes = true;
                break;
            }
        }
    }

#ifndef NDEBUG
    if( !_pendingCommands.empty( ))
        EQINFO << _pendingCommands.size() << " undispatched commands" << endl;
#endif
}

//----------------------------------------------------------------------
// command thread functions
//----------------------------------------------------------------------
void* Node::_runCommandThread()
{
    EQINFO << "Entered command thread" << endl;

    while( _state == STATE_LISTENING )
    {
        Command*            command = _commandThreadQueue.pop();
        EQASSERT( command->isValid( ));

        const CommandResult result  = invokeCommand( *command );
        switch( result )
        {
            case COMMAND_ERROR:
                EQASSERTINFO( 0, "Error handling " << *command );
                break;

            case COMMAND_HANDLED:
            case COMMAND_DISCARD:
                break;

            default:
                EQUNIMPLEMENTED;
        }

        _commandThreadQueue.release( command );
    }
 
    _commandThreadQueue.flush();
    EQINFO << "Leaving command thread" << endl;
    return EXIT_SUCCESS;
}

CommandResult Node::invokeCommand( Command& command )
{
    EQVERB << "dispatch " << command << " by " << _id << endl;
    EQASSERT( command.isValid( ));

    const uint32_t datatype = command->datatype;
    switch( datatype )
    {
        case DATATYPE_EQNET_NODE:
            return Base::invokeCommand( command );

        case DATATYPE_EQNET_SESSION:
        case DATATYPE_EQNET_OBJECT:
        {
            const SessionPacket* sessionPacket = 
                static_cast<SessionPacket*>( command.getPacket( ));
            const uint32_t       id            = sessionPacket->sessionID;
            Session*             session       = _sessions[id];
            EQASSERTINFO( session, "Can't find session for " << sessionPacket );
            
            return session->invokeCommand( command );
        }

        default:
            EQASSERTINFO( 0, "Unknown datatype " << datatype << " for "
                          << command );
            return COMMAND_ERROR;
    }
}

CommandResult Node::_cmdStop( Command& command )
{
    EQINFO << "Cmd stop " << this << endl;
    EQASSERT( _state == STATE_LISTENING );

    _state = STATE_STOPPED;
    _connectionSet.interrupt();

    return COMMAND_HANDLED;
}

CommandResult Node::_cmdMapSession( Command& command )
{
    EQASSERT( getState() == STATE_LISTENING );

    const NodeMapSessionPacket* packet = 
        command.getPacket<NodeMapSessionPacket>();
    EQINFO << "Cmd map session: " << packet << endl;
    CHECK_THREAD( _thread );
    
    Session*       session   = 0;
    const uint32_t sessionID = packet->sessionID;
    string         sessionName;
    NodePtr   node      = command.getNode();

    if( node == this ) // local mapping
    {
        if( sessionID == EQ_ID_INVALID ) // mapped by name
        {
            sessionName = packet->name;
            if( !_findSession( sessionName )) // not yet mapped 
            {
                session = static_cast< Session* >( 
                    _requestHandler.getRequestData( packet->requestID ));
                const uint32_t newSessionID = _generateSessionID();
                addSession( session, this, newSessionID, sessionName );
            }
        }
        // else mapped by identifier: not possible since we are the master
    }
    else // remote mapping
    {
        if( sessionID == EQ_ID_INVALID ) // mapped by name
        {
            sessionName = packet->name;
            session     = _findSession( sessionName );
        
#if 0
            if( !session ) // session does not exist, wait until master maps it
                return COMMAND_REDISPATCH;
#endif

        }
        else // mapped by identifier, session has to exist already
        {
            session = _sessions[sessionID];
            if( session )
                sessionName = session->getName();
        }
    }

    NodeMapSessionReplyPacket reply( packet );
    reply.sessionID  = session ? session->getID() : EQ_ID_INVALID;
                
    node->send( reply, sessionName );
    return COMMAND_HANDLED;
}

CommandResult Node::_cmdMapSessionReply( Command& command)
{
    const NodeMapSessionReplyPacket* packet = 
        command.getPacket<NodeMapSessionReplyPacket>();
    EQINFO << "Cmd map session reply: " << packet << endl;
    CHECK_THREAD( _thread );

    const uint32_t requestID = packet->requestID;
    if( packet->sessionID == EQ_ID_INVALID )
    {
        _requestHandler.serveRequest( requestID, (void*)false );
        return COMMAND_HANDLED;
    }        
    
    NodePtr   node      = command.getNode();
    if( node == this ) // local mapping, was performed in _cmdMapSession
    {
        _requestHandler.serveRequest( requestID, (void*)true );
        return COMMAND_HANDLED;
    }

    Session* session = (Session*)_requestHandler.getRequestData( requestID );
    EQASSERT( session );

    addSession( session, node, packet->sessionID, packet->name );
    _requestHandler.serveRequest( requestID, (void*)true );
    return COMMAND_HANDLED;
}

CommandResult Node::_cmdUnmapSession( Command& command )
{
    const NodeUnmapSessionPacket* packet =
        command.getPacket<NodeUnmapSessionPacket>();
    EQINFO << "Cmd unmap session: " << packet << endl;
    CHECK_THREAD( _thread );
    
    const uint32_t sessionID = packet->sessionID;
    Session*       session   = _sessions[sessionID];

    NodeUnmapSessionReplyPacket reply( packet );

    NodePtr   node      = command.getNode();
    if( !session )
    {
        reply.result = false;
        node->send( reply );
        return COMMAND_HANDLED;
    }

    if( session->_server == this )
        removeSession( session );
        // TODO: unmap all session instances.

    reply.result = true;
    node->send( reply );
    return COMMAND_HANDLED;
}

CommandResult Node::_cmdUnmapSessionReply( Command& command)
{
    const NodeUnmapSessionReplyPacket* packet = 
        command.getPacket<NodeUnmapSessionReplyPacket>();
    EQINFO << "Cmd unmap session reply: " << packet << endl;
    CHECK_THREAD( _thread );

    const uint32_t requestID = packet->requestID;

    if( !packet->result )
    {
        _requestHandler.serveRequest( requestID, (void*)false );
        return COMMAND_HANDLED;
    }        
    
    Session* session = (Session*)_requestHandler.getRequestData( requestID );
    EQASSERT( session );

    NodePtr   node      = command.getNode();
    if( node != this ) // client instance unmapping
        removeSession( session );

    _requestHandler.serveRequest( requestID, (void*)true );
    return COMMAND_HANDLED;
}

CommandResult Node::_cmdConnect( Command& command )
{
    EQASSERT( !command.getNode().isValid( ));
    EQASSERT( inReceiverThread( ));

    const NodeConnectPacket* packet = command.getPacket<NodeConnectPacket>();
    ConnectionPtr   connection = _connectionSet.getConnection();

    NodeID nodeID = packet->nodeID;
    nodeID.convertToHost();

    EQINFO << "handle connect " << packet << endl;
    EQASSERT( _connectionNodes.find( connection.get( )) == 
              _connectionNodes.end( ));

    if( _nodes.find( nodeID ) != _nodes.end( ))
    {   // Node exists, probably simultaneous connect from peer
        EQASSERT( packet->launchID == EQ_ID_INVALID );
        EQINFO << "Already got node " << nodeID << ", refusing connect"
               << endl;

        // refuse connection
        NodeConnectReplyPacket reply( packet );
        connection->send( reply, serialize( ));

        // close connection
        _connectionSet.removeConnection( connection );
        return eqNet::COMMAND_HANDLED;
    }

    // create and add connected node
    NodePtr remoteNode;
    if( packet->launchID != EQ_ID_INVALID )
    {
        void* ptr = _requestHandler.getRequestData( packet->launchID );
        EQASSERT( dynamic_cast< Node* >( (Base*)ptr ));
        remoteNode = static_cast< Node* >( ptr );
        remoteNode->_connectionDescriptions.clear(); //use actual data from peer
    }
    else
        remoteNode = createNode( packet->type );

    string data = packet->nodeData;
    if( !remoteNode->deserialize( data ))
        EQWARN << "Error during node initialization" << endl;
    EQASSERT( data.empty( ));
    EQASSERTINFO( remoteNode->_id == nodeID,
                  remoteNode->_id << "!=" << nodeID );

    remoteNode->_connection = connection;
    remoteNode->_state      = STATE_CONNECTED;
    
    connection.ref();
    _connectionNodes[ connection.get() ] = remoteNode;
    _nodes[ remoteNode->_id ]            = remoteNode;

    // send our information as reply
    NodeConnectReplyPacket reply( packet );
    reply.nodeID    = _id;
    reply.nodeID.convertToNetwork();

    reply.type      = getType();

    connection->send( reply, serialize( ));

    if( packet->launchID != EQ_ID_INVALID )
        _requestHandler.serveRequest( packet->launchID );
    
    return COMMAND_HANDLED;
}

CommandResult Node::_cmdConnectReply( Command& command )
{
    EQASSERT( !command.getNode().isValid( ));
    EQASSERT( inReceiverThread( ));

    const NodeConnectReplyPacket* packet = 
        command.getPacket<NodeConnectReplyPacket>();
    ConnectionPtr connection = _connectionSet.getConnection();

    NodeID nodeID = packet->nodeID;
    nodeID.convertToHost();

    EQINFO << "handle connect reply " << packet << endl;
    EQASSERT( _connectionNodes.find( connection.get( )) == 
              _connectionNodes.end( ));

    if( nodeID == NodeID::ZERO || // connection refused
        _nodes.find( nodeID ) != _nodes.end( )) // Node exists, probably
                                                // simultaneous connect
    {
        _connectionSet.removeConnection( connection );
        connection->close();
        
        if( packet->requestID != EQ_ID_INVALID )
            _requestHandler.serveRequest( packet->requestID, false );
        
        return eqNet::COMMAND_HANDLED;
    }

    // create and add node
    NodePtr remoteNode;
    if( packet->requestID != EQ_ID_INVALID )
    {
        void* ptr = _requestHandler.getRequestData( packet->requestID );
        EQASSERT( dynamic_cast< Node* >( (Base*)ptr ));
        remoteNode = static_cast< Node* >( ptr );
        remoteNode->_connectionDescriptions.clear(); //get actual data from peer
    }

    if( !remoteNode.isValid( ))
        remoteNode = createNode( packet->type );

    EQASSERT( remoteNode->getType() == packet->type );
    EQASSERT( remoteNode->getState() == STATE_STOPPED );

    string data = packet->nodeData;
    if( !remoteNode->deserialize( data ))
        EQWARN << "Error during node initialization" << endl;
    EQASSERT( data.empty( ));
    EQASSERT( remoteNode->_id == nodeID );

    remoteNode->_connection = connection;
    remoteNode->_state      = STATE_CONNECTED;
    
    connection.ref();
    _connectionNodes[ connection.get() ] = remoteNode;
    _nodes[ remoteNode->_id ]            = remoteNode;

    if( packet->requestID != EQ_ID_INVALID )
        _requestHandler.serveRequest( packet->requestID, true );
    return COMMAND_HANDLED;
}

CommandResult Node::_cmdDisconnect( Command& command )
{
    EQASSERT( inReceiverThread( ));

    const NodeDisconnectPacket* packet = 
        command.getPacket<NodeDisconnectPacket>();

    NodePtr node = static_cast<Node*>( 
        _requestHandler.getRequestData( packet->requestID ));
    EQASSERT( node.isValid( ));

    ConnectionPtr connection = node->_connection;
    if( connection.isValid( ))
    {
        node->_state      = STATE_STOPPED;
        node->_connection = 0;

        const bool removed = _connectionSet.removeConnection( connection );
        EQASSERTINFO( removed, connection );

        EQINFO << node << " disconnecting from " << this << endl;
        EQASSERT( _connectionNodes.find( connection.get( )) !=
                  _connectionNodes.end( ));

        _connectionNodes.erase( connection.get( ));
        _nodes.erase( node->_id );
        connection.unref();
    }

    EQASSERT( node->_state == STATE_STOPPED );
    _requestHandler.serveRequest( packet->requestID );
    return COMMAND_HANDLED;
}

CommandResult Node::_cmdGetNodeData( Command& command)
{
    const NodeGetNodeDataPacket* packet = 
        command.getPacket<NodeGetNodeDataPacket>();
    EQINFO << "cmd get node data: " << packet << endl;

    NodeID nodeID = packet->nodeID;
    nodeID.convertToHost();

    NodePtr descNode = getNode( nodeID );
    
    NodeGetNodeDataReplyPacket reply( packet );

    string nodeData;
    if( descNode.isValid( ))
    {
        reply.type = descNode->getType();
        nodeData   = descNode->serialize();
    }
    else
    {
        EQINFO << "Node " << nodeID << " unknown" << endl;
        reply.type = TYPE_EQNET_INVALID;
    }

    NodePtr node = command.getNode();
    node->send( reply, nodeData );
    EQINFO << "Sent node data " << nodeData << " to " << node << endl;
    return COMMAND_HANDLED;
}

CommandResult Node::_cmdGetNodeDataReply( Command& command )
{
    NodeGetNodeDataReplyPacket* packet = 
        command.getPacket<NodeGetNodeDataReplyPacket>();
    EQINFO << "cmd get node data reply: " << packet << endl;

    const uint32_t requestID = packet->requestID;

    NodeID nodeID = packet->nodeID;
    nodeID.convertToHost();

    if( _nodes.find( nodeID ) != _nodes.end( ))
    {   
        // Requested node connected to us in the meantime
        NodePtr node = _nodes[ nodeID ];
        EQASSERT( node->isConnected( ));

        node.ref();
        _requestHandler.serveRequest( requestID, node.get( ));
        return COMMAND_HANDLED;
    }

    if( packet->type == TYPE_EQNET_INVALID )
    {
        _requestHandler.serveRequest( requestID, (void*)0 );
        return COMMAND_HANDLED;
    }
        
    NodePtr node = createNode( packet->type );
    EQASSERT( node.isValid( ));

    string data = packet->nodeData;
    if( !node->deserialize( data ))
        EQWARN << "Failed do initialize node data" << endl;
    EQASSERT( data.empty( ));

    node->setAutoLaunch( false );
    node.ref();
    _requestHandler.serveRequest( requestID, node.get( ));
    return COMMAND_HANDLED;
}

//----------------------------------------------------------------------
// utility functions
//----------------------------------------------------------------------
Session* Node::_findSession( const std::string& name ) const
{
    CHECK_THREAD( _thread );
    for( IDHash<Session*>::const_iterator iter = _sessions.begin();
         iter != _sessions.end(); iter++ )
    {
        Session* session = (*iter).second;
        if( session->getName() == name )
            return session;
    }
    return 0;
}

//----------------------------------------------------------------------
// Connecting and launching a node
//----------------------------------------------------------------------
bool Node::connect( NodePtr node )
{
    if( node->getState() == STATE_CONNECTED ||
        node->getState() == STATE_LISTENING )

        return true;

    if( !initConnect( node ))
    {
        EQERROR << "Connection initialisation to " << node << " failed." 
                << endl;
        return false;
    }

    return syncConnect( node );
}

bool Node::initConnect( NodePtr node )
{
    EQASSERT( _state == STATE_LISTENING );
    if( node->getState() == STATE_CONNECTED ||
        node->getState() == STATE_LISTENING )

        return true;

    EQASSERT( node->getState() == STATE_STOPPED );

    // try connecting first
    const ConnectionDescriptionVector& cds = node->getConnectionDescriptions();
    for( ConnectionDescriptionVector::const_iterator i = cds.begin();
        i != cds.end(); ++i )
    {
        ConnectionDescriptionPtr description = *i;
        ConnectionPtr connection = Connection::create( description->type );
        connection->setDescription( description );

        if( !connection->connect( ))
            continue;

        if( !connect( node, connection ))
            return false;

        return true;
    }

    EQINFO << "Node could not be connected." << endl;
    if( !node->_autoLaunch )
        return false;
    
    EQINFO << "Attempting to launch node." << endl;
    for( ConnectionDescriptionVector::const_iterator i = cds.begin();
        i != cds.end(); ++i )
    {
        ConnectionDescriptionPtr description = *i;

        if( _launch( node, description ))
            return true;
    }

    return false;
}

bool Node::syncConnect( NodePtr node )
{
    if( node->_launchID == EQ_ID_INVALID )
        return ( node->getState() == STATE_CONNECTED );

    void*          ret;
    const float    time    = -( node->_launchTimeout.getTimef( )); 
    const uint32_t timeout = static_cast< uint32_t >((time > 0) ? time : 0);

    if( _requestHandler.waitRequest( node->_launchID, ret, timeout ))
    {
        EQASSERT( node->getState() == STATE_CONNECTED );
        node->_launchID = EQ_ID_INVALID;
        return true;
    }

    node->_state = STATE_STOPPED;
    _requestHandler.unregisterRequest( node->_launchID );
    node->_launchID = EQ_ID_INVALID;
    return false;
}

NodePtr Node::connect( const NodeID& nodeID, NodePtr server )
{
    EQASSERT( nodeID != NodeID::ZERO );

    NodeIDHash< RefPtr< Node > >::const_iterator iter = _nodes.find( nodeID );
    if( iter != _nodes.end( ))
        return iter->second;

    // Make sure that only one connection request based on the node identifier
    // is pending at a given time. Otherwise a node with the same id might be
    // instantiated twice in _cmdGetNodeDataReply(). The alternative to this
    // mutex is to register connecting nodes with this local node, and handle
    // all cases correctly, which is far more complex. Node connection only
    // happens a lot during initialization, and are therefore not time-critical.
    ScopedMutex< Lock > mutex( _connectMutex );
    EQINFO << "Connecting node " << nodeID << endl;

    iter = _nodes.find( nodeID );
    if( iter != _nodes.end( ))
        return iter->second;
 
    EQASSERT( _id != nodeID );
    NodeGetNodeDataPacket packet;
    packet.requestID = _requestHandler.registerRequest();
    packet.nodeID    = nodeID;
    packet.nodeID.convertToNetwork();

    server->send( packet );

    void* result = 0;
    _requestHandler.waitRequest( packet.requestID, result );

    if( !result )
    {
        EQWARN << "Node not found on server" << endl;
        return 0;
    }

    EQASSERT( dynamic_cast< Node* >( (Base*)result ));
    NodePtr node = static_cast< Node* >( result );
    node.unref(); // ref'd before serveRequest()
    
    if( node->isConnected( ))
        return node;

    if( !connect( node ))
    {
        // connect failed - maybe simultaneous connect from peer?
        iter = _nodes.find( nodeID );
        if( iter != _nodes.end( ))
            return iter->second;
        
        EQWARN << "Node connection failed" << endl;
        return 0;
    }

    return node;
}

bool Node::_launch( NodePtr node,
                    ConnectionDescriptionPtr description )
{
    EQASSERT( node->getState() == STATE_STOPPED );

    node->_launchID = _requestHandler.registerRequest( node.get() );
    node->_launchTimeout.setAlarm( description->launchTimeout );

    const string launchCommand = _createLaunchCommand( node, description );

    if( !eqBase::Launcher::run( launchCommand ))
    {
        EQWARN << "Could not launch node using '" << launchCommand << "'" 
               << endl;
        _requestHandler.unregisterRequest( node->_launchID );
        node->_launchID = EQ_ID_INVALID;
        return false;
    }
    
    node->_state = STATE_LAUNCHED;
    return true;
}

string Node::_createLaunchCommand( NodePtr node,
                                   ConnectionDescriptionPtr description )
{
    const string& launchCommand    = description->getLaunchCommand();
    const size_t  launchCommandLen = launchCommand.size();
    const char    quote            = description->launchCommandQuote;

    bool          commandFound     = false;
    size_t        lastPos          = 0;
    string        result;

    for( size_t percentPos = launchCommand.find( '%' );
         percentPos != string::npos; 
         percentPos = launchCommand.find( '%', percentPos+1 ))
    {
        ostringstream replacement;
        switch( launchCommand[percentPos+1] )
        {
            case 'c':
            {
                replacement << _createRemoteCommand( node, quote );
                commandFound = true;
                break;
            }
            case 'h':
                replacement << description->getHostname();
                break;

            case 'n':
                replacement << node->getNodeID();
                break;

            default:
                EQWARN << "Unknown token " << launchCommand[percentPos+1] 
                       << endl;
        }

        result += launchCommand.substr( lastPos, percentPos-lastPos );
        if( !replacement.str().empty( ))
            result += replacement.str();

        lastPos  = percentPos+2;
    }

    result += launchCommand.substr( lastPos, launchCommandLen-lastPos );

    if( !commandFound )
        result += " " + _createRemoteCommand( node, quote );

    EQINFO << "Launch command: " << result << endl;
    return result;
}

string Node::_createRemoteCommand( NodePtr node, const char quote )
{
    if( getState() != STATE_LISTENING )
    {
        EQERROR << "Node is not listening, can't launch " << this << endl;
        return "";
    }

    ostringstream stringStream;

    //----- environment
#ifndef WIN32
#  ifdef Darwin
    const char libPath[] = "DYLD_LIBRARY_PATH";
#  else
    const char libPath[] = "LD_LIBRARY_PATH";
#  endif

    stringStream << "env "; // XXX
    char* env = getenv( libPath );
    if( env )
        stringStream << libPath << "=" << env << " ";

    for( int i=0; environ[i] != 0; i++ )
        if( strlen( environ[i] ) > 2 && strncmp( environ[i], "EQ_", 3 ) == 0 )
            stringStream << environ[i] << " ";

    stringStream << "EQ_LOG_LEVEL=" << eqBase::Log::getLogLevelString() << " ";
    if( eqBase::Log::topics != 0 )
        stringStream << "EQ_LOG_TOPICS=" << eqBase::Log::topics << " ";
#endif // WIN32

    //----- program + args
    string program = node->_programName;
#ifdef WIN32
    EQASSERT( program.length() > 2 );
    if( !( program[1] == ':' && (program[2] == '/' || program[2] == '\\' )) &&
        // !( drive letter and full path present )
        !( program[0] == '/' || program[0] == '\\' ))
        // !full path without drive letter

        program = node->_workDir + '/' + program; // add _workDir to rel. path
#else
    if( program[0] != '/' )
        program = node->_workDir + '/' + program;
#endif

    const string ownData    = serialize();
    const string remoteData = node->serialize();

    stringStream
        << quote << program << quote << " -- --eq-client " << quote
        << remoteData << node->_launchID << SEPARATOR << node->_workDir 
        << SEPARATOR << node->_id << SEPARATOR << getType() << SEPARATOR
        << ownData << quote;

    return stringStream.str();
}

bool Node::runClient( const string& clientArgs )
{
    EQASSERT( _state == STATE_LISTENING );

    size_t nextPos = clientArgs.find( SEPARATOR );
    if( nextPos == string::npos )
    {
        EQERROR << "Could not parse request identifier: " << clientArgs << endl;
        return false;
    }

    const string   request     = clientArgs.substr( 0, nextPos );
    string         description = clientArgs.substr( nextPos + 1 );
    const uint32_t launchID    = strtoul( request.c_str(), 0, 10 );

    nextPos = description.find( SEPARATOR );
    if( nextPos == string::npos )
    {
        EQERROR << "Could not parse working directory: " << description 
                << " is left from " << clientArgs << endl;
        return false;
    }

    const string workDir = description.substr( 0, nextPos );
    description          = description.substr( nextPos + 1 );

    Global::setWorkDir( workDir );
    if( !workDir.empty() && chdir( workDir.c_str( )) == -1 )
        EQWARN << "Can't change working directory to " << workDir << ": "
               << strerror( errno ) << endl;
    
    EQINFO << "Launching node with launch ID=" << launchID << ", cwd="
           << workDir << endl;

    nextPos = description.find( SEPARATOR );
    if( nextPos == string::npos )
    {
        EQERROR << "Could not parse node identifier: " << description
                << " is left from " << clientArgs << endl;
        return false;
    }
    _id         = description.substr( 0, nextPos );
    description = description.substr( nextPos + 1 );

    nextPos = description.find( SEPARATOR );
    if( nextPos == string::npos )
    {
        EQERROR << "Could not parse server node type: " << description
                << " is left from " << clientArgs << endl;
        return false;
    }
    const string nodeType = description.substr( 0, nextPos );
    description           = description.substr( nextPos + 1 );
    const uint32_t   type = atoi( nodeType.c_str( ));

    RefPtr< Node >   node = createNode( type );
    if( !node )
    {
        EQERROR << "Can't create server node" << endl;
        return false;
    }
    
    node->setAutoLaunch( false );
    node->_launchID = launchID;

    if( !node->deserialize( description ))
        EQWARN << "Can't parse node data" << endl;

    if( !connect( node ))
    {
        EQERROR << "Can't connect node" << endl;
        return false;
    }

    return clientLoop();
}


EQ_EXPORT std::ostream& operator << ( std::ostream& os, const Node::State state)
{
    os << ( state == Node::STATE_STOPPED ? "stopped" :
            state == Node::STATE_LAUNCHED ? "launched" :
            state == Node::STATE_CONNECTED ? "connected" :
            state == Node::STATE_LISTENING ? "listening" : "ERROR" );
    return os;
}

}
