#ifndef __NODE_JOB_H
#define __NODE_JOB_H

#define BOOST_SPIRIT_THREADSAFE

#include <boost/property_tree/json_parser.hpp>
#include "common/protocol.h"
#include "common/helper.h"
#include "common/request.h"
#include "job_completion_table.h"

namespace python_server {

class Job
{
public:
	template< typename T >
	bool ParseRequest( Request<T> &request )
	{
        const std::string &req = request.GetString();

        std::string protocol, header, body;
        int version;
        if ( !Protocol::ParseMsg( req, protocol, version, header, body ) )
        {
            PS_LOG( "Job::ParseRequest: couldn't parse request: " << req );
            return false;
        }

        ProtocolCreator protocolCreator;
	    boost::scoped_ptr< Protocol > parser(
		    protocolCreator.Create( protocol, version )
		);
        if ( !parser )
        {
            PS_LOG( "Job::ParseRequest: appropriate parser not found for protocol: "
                    << protocol << " " << version );
            return false;
        }

        parser->ParseMsgType( header, taskType_ );

        return ParseRequestBody( body, parser.get() );
	}

	void GetResponse( std::string &response ) const
	{
        if ( taskType_ == "get_result" )
        {
			JobDescriptor descr;
			JobCompletionStat stat;
			ProtocolJson protocol;

			descr.jobId = GetJobId();
			descr.taskId = GetTaskId();
			if ( JobCompletionTable::Instance().Get( descr, stat ) )
			{
				JobCompletionTable::Instance().Erase( descr );
				protocol.SendJobResult( response, stat.errCode );
			}
			else
			{
                PS_LOG( "Job::GetResponse: job not found in completion table: "
                        "jobId=" << GetJobId() << ", taskId=" << GetTaskId() );
				protocol.SendJobResult( response, -1 );
			}
        }
	}

	void OnError( int err )
	{
		errCode_ = err;
	}

	bool NeedPingMaster() const
	{
		return taskType_ == "exec";
	}

    void SetMasterIP( const std::string &ip ) { masterIP_ = ip; }

	unsigned int GetScriptLength() const { return scriptLength_; }
	const std::string &GetScriptLanguage() const { return language_; }
	const std::string &GetScript() const { return script_; }
    int64_t GetJobId() const { return jobId_; }
    int GetTaskId() const { return taskId_; }
    int GetErrorCode() const { return errCode_; }
	const std::string &GetTaskType() const { return taskType_; }
	const std::string &GetMasterIP() const { return masterIP_; }

private:
    bool ParseRequestBody( const std::string &body, Protocol *parser )
    {
        if ( taskType_ == "exec" )
        {
            std::string script64;
            parser->ParseSendScript( body, language_, script64, jobId_, taskId_ );
            DecodeBase64( script64, script_ );

            scriptLength_ = script_.size();
            return true;
        }
        if ( taskType_ == "get_result" )
        {
            return parser->ParseGetJobResult( body, jobId_, taskId_ );
        }
        return false;
    }

private:
	unsigned int scriptLength_;
	std::string language_;
	std::string script_;
    int64_t jobId_;
    int taskId_;
	int errCode_;
	std::string taskType_;
    std::string masterIP_;
};

} // namespace python_server

#endif
