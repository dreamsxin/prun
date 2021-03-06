#include "master/command.h"
#include "master/timeout_manager.h"
#include "master/job_history.h"

using namespace std;

namespace master {

struct MockCommand : Command
{
    MockCommand() : Command( "mock" ) {}
    virtual int GetRepeatDelay() const { return 0; }
    virtual void OnCompletion( int errCode, const std::string &hostIP ) {}
};

struct MockTimeoutManager : ITimeoutManager
{
    virtual void PushJobQueue( int64_t jobId, int queueTimeout ) {}
    virtual void PushJob( int64_t jobId, int jobTimeout ) {}
    virtual void PushTask( const WorkerTask &task, const std::string &hostIP, int timeout ) {}
    virtual void PushCommand( CommandPtr &command, const std::string &hostIP, int delay ) {}
};

struct MockJobHistory : IJobEventReceiver
{
    virtual void OnJobAdd( const std::string &jobId, const std::string &jobDescr ) {}
    virtual void OnJobDelete( int64_t jobId ) {}
    virtual void OnJobDelete( const std::string &jobName ) {}
};

} // namespace master
