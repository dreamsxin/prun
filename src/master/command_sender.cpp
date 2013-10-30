#include <boost/bind.hpp>
#include <boost/scoped_ptr.hpp>
#include "command_sender.h"
#include "worker_manager.h"
#include "common/log.h"
#include "common/protocol.h"
#include "defines.h"

namespace master {

void CommandSender::Run()
{
    CommandPtr command;
    std::string hostIP;

    WorkerManager &workerMgr = WorkerManager::Instance();
    workerMgr.Subscribe( this, WorkerManager::eCommand );

    bool getCommand = false;
    while( !stopped_ )
    {
        if ( !getCommand )
        {
            boost::unique_lock< boost::mutex > lock( awakeMut_ );
            if ( !newCommandAvailable_ )
                awakeCond_.wait( lock );
            newCommandAvailable_ = false;
        }

        getCommand = workerMgr.GetCommand( command, hostIP );
        if ( getCommand )
        {
            PS_LOG( "Get command '" << command->GetCommand() << "' : " << hostIP );
            SendCommand( command, hostIP );
        }
    }
}

void CommandSender::Stop()
{
    stopped_ = true;
    boost::unique_lock< boost::mutex > lock( awakeMut_ );
    awakeCond_.notify_all();
}

void CommandSender::NotifyObserver( int event )
{
    boost::unique_lock< boost::mutex > lock( awakeMut_ );
    newCommandAvailable_ = true;
    awakeCond_.notify_all();
}

void CommandSender::OnSendCommand( bool success, int errCode, CommandPtr &command, const std::string &hostIP )
{
    if ( !success ) // retrieving of job result from message failed
        errCode = -1;
    command->OnExec( errCode, hostIP );
}

void CommandSenderBoost::Start()
{
    io_service_.post( boost::bind( &CommandSender::Run, this ) );
}

void CommandSenderBoost::SendCommand( CommandPtr &command, const std::string &hostIP )
{   
    cmdSenderSem_.Wait();

    RpcBoost::sender_ptr sender(
        new RpcBoost( io_service_, this, command, hostIP )
    );
    sender->SendCommand();
}

void CommandSenderBoost::OnSendCommand( bool success, int errCode, CommandPtr &command, const std::string &hostIP )
{
    cmdSenderSem_.Notify();
    CommandSender::OnSendCommand( success, errCode, command, hostIP );
}

void RpcBoost::SendCommand()
{
    tcp::endpoint nodeEndpoint(
        boost::asio::ip::address::from_string( hostIP_ ),
        NODE_PORT
    );

    socket_.async_connect( nodeEndpoint,
                           boost::bind( &RpcBoost::HandleConnect, shared_from_this(),
                                        boost::asio::placeholders::error ) );
}

void RpcBoost::HandleConnect( const boost::system::error_code &error )
{
    if ( !error )
    {
        MakeRequest();

        boost::asio::async_read( socket_,
                                 boost::asio::buffer( &buffer_, sizeof( char ) ),
                                 boost::bind( &RpcBoost::FirstRead, shared_from_this(),
                                              boost::asio::placeholders::error,
                                              boost::asio::placeholders::bytes_transferred ) );

        boost::asio::async_write( socket_,
                                  boost::asio::buffer( request_ ),
                                  boost::bind( &RpcBoost::HandleWrite, shared_from_this(),
                                               boost::asio::placeholders::error,
                                               boost::asio::placeholders::bytes_transferred ) );
    }
    else
    {
        PS_LOG( "RpcBoost::HandleConnect error=" << error.message() );
        sender_->OnSendCommand( false, 0, command_, hostIP_ );
    }
}

void RpcBoost::HandleWrite( const boost::system::error_code &error, size_t bytes_transferred )
{
    if ( error )
    {
        PS_LOG( "RpcBoost::HandleWrite error=" << error.message() );
        sender_->OnSendCommand( false, 0, command_, hostIP_ );
    }
}

void RpcBoost::FirstRead( const boost::system::error_code& error, size_t bytes_transferred )
{
    if ( !error )
    {
        if ( firstRead_ )
        {
            // skip node's read completion status byte
            firstRead_ = false;
            socket_.async_read_some( boost::asio::buffer( buffer_ ),
                                     boost::bind( &RpcBoost::FirstRead, shared_from_this(),
                                                  boost::asio::placeholders::error,
                                                  boost::asio::placeholders::bytes_transferred ) );
            return;
        }

        int ret = response_.OnFirstRead( buffer_, bytes_transferred );
        if ( ret == 0 )
        {
            socket_.async_read_some( boost::asio::buffer( buffer_ ),
                                     boost::bind( &RpcBoost::FirstRead, shared_from_this(),
                                                  boost::asio::placeholders::error,
                                                  boost::asio::placeholders::bytes_transferred ) );
            return;
        }
    }
    else
    {
        PS_LOG( "RpcBoost::FirstRead error=" << error.message() );
    }

    HandleRead( error, bytes_transferred );
}

void RpcBoost::HandleRead( const boost::system::error_code& error, size_t bytes_transferred )
{
    if ( !error )
    {
        response_.OnRead( buffer_, bytes_transferred );

        if ( !response_.IsReadCompleted() )
        {
            socket_.async_read_some( boost::asio::buffer( buffer_ ),
                                     boost::bind( &RpcBoost::HandleRead, shared_from_this(),
                                                  boost::asio::placeholders::error,
                                                  boost::asio::placeholders::bytes_transferred ) );
        }
        else
        {
            if ( !HandleResponse() )
            {
                sender_->OnSendCommand( false, 0, command_, hostIP_ );
            }
        }
    }
    else
    {
        PS_LOG( "RpcBoost::HandleRead error=" << error.message() );
        sender_->OnSendCommand( false, 0, command_, hostIP_ );
    }
}

bool RpcBoost::HandleResponse()
{
    const std::string &msg = response_.GetString();

    std::string protocol, header, body;
    int version;
    if ( !python_server::Protocol::ParseMsg( msg, protocol, version, header, body ) )
    {
        PS_LOG( "RpcBoost::HandleResponse: couldn't parse msg: " << msg );
        return false;
    }

    python_server::ProtocolCreator protocolCreator;
    boost::scoped_ptr< python_server::Protocol > parser(
        protocolCreator.Create( protocol, version )
    );
    if ( !parser )
    {
        PS_LOG( "RpcBoost::HandleResponse: appropriate parser not found for protocol: "
                << protocol << " " << version );
        return false;
    }

    std::string type;
    parser->ParseMsgType( header, type );
    if ( !parser->ParseMsgType( header, type ) )
    {
        PS_LOG( "RpcBoost::HandleResponse: couldn't parse msg type: " << header );
        return false;
    }

    if ( type == "send_command_result" )
    {
        int errCode;
        if ( parser->ParseSendCommandResult( body, errCode ) )
        {
            sender_->OnSendCommand( true, errCode, command_, hostIP_ );
            return true;
        }
    }
    else
    {
        PS_LOG( "RpcBoost::HandleResponse: unexpected msg type: " << type );
    }

    return false;
}

void RpcBoost::MakeRequest()
{
    python_server::ProtocolJson protocol;
    protocol.SendCommand( request_, command_->GetCommand(), command_->GetAllParams() );
}

} // namespace master