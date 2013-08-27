#define BOOST_SPIRIT_THREADSAFE

#include <sstream>
#include <boost/property_tree/json_parser.hpp>
#include <boost/bind.hpp>
#include <boost/enable_shared_from_this.hpp>
#include "admin.h"
#include "job_manager.h"

namespace master {

void AdminCommand_Job::Execute( const std::string &command,
                                const boost::property_tree::ptree &ptree )
{
	try
	{
        std::string filePath = ptree.get<std::string>( "file" );
        // read job description from file
        std::ifstream file( filePath.c_str() );
        if ( !file.is_open() )
        {
            PS_LOG( "AdminCommand_Job::Execute: couldn't open " << filePath );
            return;
        }
        std::string job, line;
        while( std::getline( file, line ) )
            job += line;
        // add job to job queue
        JobManager::Instance().PushJob( job );
	}
	catch( std::exception &e )
	{
		PS_LOG( "AdminCommand_Job::Execute: " << e.what() );
	}
}

void AdminCommandDispatcher::Initialize()
{
    map_[ "job" ] = new AdminCommand_Job;
}

void AdminCommandDispatcher::Shutdown()
{
    std::map< std::string, AdminCommand * >::const_iterator it = map_.begin();
    for( ; it != map_.end(); ++it )
    {
        delete it->second;
    }
}

AdminCommand *AdminCommandDispatcher::Get( const std::string &command ) const
{
    std::map< std::string, AdminCommand * >::const_iterator it = map_.find( command );
    if ( it != map_.end() )
        return it->second;
    return NULL;
}


void AdminSession::Start()
{
    remoteIP_ = socket_.remote_endpoint().address().to_string();

    socket_.async_read_some( boost::asio::buffer( buffer_ ),
                             boost::bind( &AdminSession::FirstRead, shared_from_this(),
                                          boost::asio::placeholders::error,
                                          boost::asio::placeholders::bytes_transferred ) );
}

void AdminSession::FirstRead( const boost::system::error_code &error, size_t bytes_transferred )
{
    if ( !error )
    {
        int ret = request_.OnFirstRead( buffer_, bytes_transferred );
        if ( ret == 0 )
        {
            socket_.async_read_some( boost::asio::buffer( buffer_ ),
                                     boost::bind( &AdminSession::FirstRead, shared_from_this(),
                                                  boost::asio::placeholders::error,
                                                  boost::asio::placeholders::bytes_transferred ) );
            return;
        }
    }
    else
    {
        PS_LOG( "AdminSession::FirstRead error=" << error.message() );
    }

    HandleRead( error, bytes_transferred );
}

void AdminSession::HandleRead( const boost::system::error_code &error, size_t bytes_transferred )
{
    if ( !error )
    {
        request_.OnRead( buffer_, bytes_transferred );

        if ( !request_.IsReadCompleted() )
        {
            socket_.async_read_some( boost::asio::buffer( buffer_ ),
                                     boost::bind( &AdminSession::HandleRead, shared_from_this(),
                                                  boost::asio::placeholders::error,
                                                  boost::asio::placeholders::bytes_transferred ) );
        }
        else
        {
            HandleRequest();

            // read next command
            request_.Reset();
            socket_.async_read_some( boost::asio::buffer( buffer_ ),
                                     boost::bind( &AdminSession::FirstRead, shared_from_this(),
                                                  boost::asio::placeholders::error,
                                                  boost::asio::placeholders::bytes_transferred ) );
        }
    }
    else
    {
        PS_LOG( "AdminSession::HandleRead error=" << error.message() );
    }
}

void AdminSession::HandleRequest()
{
    PS_LOG( request_.GetString() );

    std::string command;
	std::istringstream ss( request_.GetString() );

	boost::property_tree::ptree ptree;
	try
	{
		boost::property_tree::read_json( ss, ptree );
        command = ptree.get<std::string>( "command" );
	}
	catch( std::exception &e )
	{
		PS_LOG( "AdminSession::HandleRequest: " << e.what() );
        return;
	}

    AdminCommand *adminCommand = AdminCommandDispatcher::Instance().Get( command );
    if ( adminCommand )
    {
        adminCommand->Execute( command, ptree );
    }
    else
    {
        PS_LOG( "AdminSession::HandleRequest: unknown command: " << command );
    }
}


void AdminConnection::StartAccept()
{
    session_ptr session( new AdminSession( io_service_ ) );
    acceptor_.async_accept( session->GetSocket(),
                            boost::bind( &AdminConnection::HandleAccept, this,
                                        session, boost::asio::placeholders::error ) );
}

void AdminConnection::HandleAccept( session_ptr session, const boost::system::error_code &error )
{
    if ( !error )
    {
        PS_LOG( "admin connection accepted..." );
        io_service_.post( boost::bind( &AdminSession::Start, session ) );
        StartAccept();
    }
    else
    {
        PS_LOG( "AdminConnection::HandleAccept: " << error.message() );
    }
}

} // namespace master
