#ifndef __JOB_H
#define __JOB_H

#include <list>
#include <boost/thread/mutex.hpp>
#include <boost/function.hpp>
#include <boost/bind.hpp>
#include <stdint.h> // int64_t

namespace master {

class Job
{
public:
    Job( const char *script, const char *scriptLanguage, int numNodes,
		 int maxFailedNodes, int timeout )
    : script_( script ), scriptLanguage_( scriptLanguage ), numNodes_( numNodes ),
	 maxFailedNodes_( maxFailedNodes ), timeout_( timeout )
    {
        static int64_t numJobs;
        scriptLength_ = script_.size();
        id_ = numJobs++;
    }

	const std::string &GetScript() const { return script_; }
	const std::string &GetScriptLanguage() const { return scriptLanguage_; }
	unsigned int GetScriptLength() const { return scriptLength_; }

    int GetNumNodes() const { return numNodes_; }
    int GetNumPlannedExec() const { return numPlannedExec_; }
    int GetMaxFailedNodes() const { return maxFailedNodes_; }
    int GetTimeout() const { return timeout_; }
    int64_t GetJobId() const { return id_; }

    void SetNumPlannedExec( int val ) { numPlannedExec_ = val; }

    template< typename T >
    void SetCallback( T *obj, void (T::*f)( const std::string &result ) )
    {
        callback_ = boost::bind( f, obj->shared_from_this(), _1 );
    }

    void RunCallback( const std::string &result ) const
    {
        if ( callback_ )
            callback_( result );
    }

private:
    std::string script_;
    std::string scriptLanguage_;
    unsigned int scriptLength_;

    int numNodes_;
    int numPlannedExec_;
	int maxFailedNodes_;
    int timeout_;
    int64_t id_;

    boost::function< void (const std::string &) > callback_;
};

class JobQueue
{
    typedef std::map< int64_t, Job * > IdToJob;

public:
    JobQueue() : numJobs_( 0 ) {}

    void PushJob( Job *job );

    Job *PopJob();
    Job *GetTopJob();

    Job *GetJobById( int64_t jobId );

	void Clear( bool doDelete = true );

private:
    std::list< Job * > jobs_;
    IdToJob idToJob_;
    unsigned int numJobs_;
    boost::mutex jobsMut_;
};

} // namespace master

#endif
