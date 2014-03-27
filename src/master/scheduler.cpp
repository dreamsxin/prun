/*
===========================================================================

This software is licensed under the Apache 2 license, quoted below.

Copyright (C) 2013 Andrey Budnik <budnik27@gmail.com>

Licensed under the Apache License, Version 2.0 (the "License"); you may not
use this file except in compliance with the License. You may obtain a copy of
the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
License for the specific language governing permissions and limitations under
the License.

===========================================================================
*/

#include "scheduler.h"
#include "common/log.h"
#include "common/error_code.h"
#include "common/service_locator.h"
#include "job_manager.h"
#include "worker_manager.h"
#include "worker_command.h"
#include "statistics.h"

// hint: avoid deadlocks. always lock jobs mutex after workers mutex

namespace master {

Scheduler::Scheduler()
{
    jobs_.SetOnRemoveCallback( this, &Scheduler::OnRemoveJob );
}

void Scheduler::OnHostAppearance( WorkerPtr &worker )
{
    {
        boost::mutex::scoped_lock scoped_lock( workersMut_ );
        nodeState_[ worker->GetIP() ].SetWorker( worker );
    }
    NotifyAll();
}

void Scheduler::DeleteWorker( const std::string &host )
{
    {
        boost::mutex::scoped_lock scoped_lock( workersMut_ );

        IPToNodeState::iterator it = nodeState_.begin();
        for( ; it != nodeState_.end(); )
        {
            const NodeState &nodeState = it->second;
            const WorkerPtr &worker = nodeState.GetWorker();

            if ( worker->GetHost() != host )
            {
                ++it;
                continue;
            }

            const WorkerJob workerJob = worker->GetJob();

            StopWorker( worker->GetIP() );

            failedWorkers_.Add( workerJob, worker->GetIP() );

            nodeState_.erase( it++ );

            // worker job should be rescheduled to any other node
            RescheduleJob( workerJob );
        }
    }
    NotifyAll();
}

void Scheduler::OnChangedWorkerState( std::vector< WorkerPtr > &workers )
{
    boost::mutex::scoped_lock scoped_lock( workersMut_ );

    std::vector< WorkerPtr >::iterator it = workers.begin();
    for( ; it != workers.end(); ++it )
    {
        WorkerPtr &worker = *it;
        WorkerState state = worker->GetState();

        if ( state == WORKER_STATE_NOT_AVAIL )
        {
            IPToNodeState::iterator it = nodeState_.find( worker->GetIP() );
            if ( it != nodeState_.end() )
            {
                NodeState &nodeState = it->second;
                if ( nodeState.GetNumBusyCPU() < 1 )
                    continue;

                const WorkerJob workerJob = worker->GetJob();

                PLOG( "Scheduler::OnChangedWorkerState: worker isn't available, while executing job"
                      "; nodeIP=" << worker->GetIP() << ", numTasks=" << workerJob.GetTotalNumTasks() );

                failedWorkers_.Add( workerJob, worker->GetIP() );
                nodeState.Reset();
                worker->ResetJob();

                if ( RescheduleJob( workerJob ) )
                {
                    scoped_lock.unlock();
                    NotifyAll();
                    scoped_lock.lock();
                }
            }
            else
            {
                PLOG( "Scheduler::OnChangedWorkerState: sheduler doesn't know about worker"
                      " with ip = " << worker->GetIP() );
            }
        }
    }
}

void Scheduler::OnNewJob()
{
    if ( CanTakeNewJob() )
        PlanJobExecution();
}

void Scheduler::PlanJobExecution()
{
    JobPtr job;

    IJobManager *jobManager = common::ServiceLocator::Instance().Get< IJobManager >();
    if ( !jobManager->PopJob( job ) )
        return;

    int numExec = GetNumPlannedExec( job );
    job->SetNumPlannedExec( numExec );

    int64_t jobId = job->GetJobId();
    {
        boost::mutex::scoped_lock scoped_lock( jobsMut_ );

        std::set< int > &tasks = tasksToSend_[ jobId ];
        for( int taskId = 0; taskId < numExec; ++taskId )
        {
            tasks.insert( taskId );
        }

        jobs_.Add( job, numExec );
    }

    NotifyAll();
}

bool Scheduler::RescheduleJob( const WorkerJob &workerJob )
{
    bool found = true;
    std::set<int64_t> jobs;
    workerJob.GetJobs( jobs );

    std::set<int64_t>::const_iterator it = jobs.begin();

    boost::mutex::scoped_lock scoped_lock( jobsMut_ );

    for( ; it != jobs.end(); ++it )
    {
        int64_t jobId = *it;
        JobPtr job;
        if ( jobs_.FindJobByJobId( jobId, job ) )
        {
            size_t failedNodesCnt = failedWorkers_.GetFailedNodesCnt( jobId );
            if ( failedNodesCnt >= (size_t)job->GetMaxFailedNodes() )
            {
                StopWorkers( jobId );
                jobs_.RemoveJob( jobId, "max failed nodes limit exceeded" );
                continue;
            }

            if ( job->IsNoReschedule() )
            {
                jobs_.DecrementJobExecution( jobId, workerJob.GetNumTasks( jobId ) );
                continue;
            }

            WorkerJob::Tasks tasks;
            workerJob.GetTasks( jobId, tasks );
            WorkerJob::Tasks::const_iterator it_task = tasks.begin();
            for( ; it_task != tasks.end(); ++it_task )
            {
                int taskId = *it_task;
                needReschedule_.push_back( WorkerTask( jobId, taskId ) );
                found = true;
            }
        }
        else
        {
            PLOG( "Scheduler::RescheduleJob: Job for jobId=" << jobId << " not found" );
        }
    }
    return found;
}

bool Scheduler::GetReschedJobForWorker( const WorkerPtr &worker, WorkerJob &plannedJob, JobPtr &job, int numFreeCPU )
{
    int64_t jobId;
    bool foundReschedJob = false;

    if ( !needReschedule_.empty() )
    {
        TaskList::iterator it = needReschedule_.begin();
        for( ; it != needReschedule_.end(); )
        {
            if ( plannedJob.GetTotalNumTasks() >= numFreeCPU )
                break;

            const WorkerTask &workerTask = *it;

            if ( !jobs_.FindJobByJobId( workerTask.GetJobId(), job ) ||
                 !CanAddTaskToWorker( worker->GetJob(), plannedJob, workerTask.GetJobId(), job ) )
            {
                ++it;
                continue;
            }

            if ( foundReschedJob )
            {
                if ( workerTask.GetJobId() == jobId )
                {
                    plannedJob.AddTask( jobId, workerTask.GetTaskId() );
                    needReschedule_.erase( it++ );
                    continue;
                }
            }
            else
            {
                jobId = workerTask.GetJobId();
                if ( !failedWorkers_.IsWorkerFailedJob( worker->GetIP(), jobId ) )
                {
                    if ( job->IsHostPermitted( worker->GetHost() ) &&
                         job->IsGroupPermitted( worker->GetGroup() ) )
                    {
                        foundReschedJob = true;
                        plannedJob.AddTask( jobId, workerTask.GetTaskId() );
                        needReschedule_.erase( it++ );
                        continue;
                    }
                }
            }
            ++it;
        }
    }

    return foundReschedJob;
}

bool Scheduler::GetJobForWorker( const WorkerPtr &worker, WorkerJob &plannedJob, JobPtr &job, int numFreeCPU )
{
    bool foundReschedJob = GetReschedJobForWorker( worker, plannedJob, job, numFreeCPU );
    int64_t jobId = plannedJob.GetJobId();

    const ScheduledJobs::JobList &jobs = jobs_.GetJobList();
    ScheduledJobs::JobList::const_iterator it = jobs.begin();
    for( ; it != jobs.end(); ++it )
    {
        const JobPtr &j = *it;

        // plannedJob tasks must belong to the only one job,
        // so jobId must be the same
        if ( foundReschedJob && jobId != j->GetJobId() )
            continue;

        if ( failedWorkers_.IsWorkerFailedJob( worker->GetIP(), j->GetJobId() ) )
            continue;

        if ( !CanAddTaskToWorker( worker->GetJob(), plannedJob, j->GetJobId(), j ) )
            continue;

        JobIdToTasks::iterator it = tasksToSend_.find( j->GetJobId() );
        if ( it == tasksToSend_.end() )
            continue;

        std::set< int > &tasks = it->second;
        if ( !tasks.empty() )
        {
            job = const_cast<JobPtr &>( j );
            if ( !j->IsHostPermitted( worker->GetHost() ) ||
                 !j->IsGroupPermitted( worker->GetGroup() ) )
                continue;

            std::set< int >::iterator it_task = tasks.begin();
            for( ; it_task != tasks.end();  )
            {
                if ( plannedJob.GetTotalNumTasks() >= numFreeCPU ||
                     !CanAddTaskToWorker( worker->GetJob(), plannedJob, j->GetJobId(), j ) )
                    break;

                int taskId = *it_task;
                plannedJob.AddTask( j->GetJobId(), taskId );

                tasks.erase( it_task++ );
                if ( tasks.empty() )
                {
                    tasksToSend_.erase( it );
                    break;
                }
            }
            break;
        }
    }

    return plannedJob.GetTotalNumTasks() > 0;
}

bool Scheduler::GetTaskToSend( WorkerJob &workerJob, std::string &hostIP, JobPtr &job )
{
    boost::mutex::scoped_lock scoped_lock_w( workersMut_ );

    workerPriority_.Sort( nodeState_.begin(), nodeState_.end(), nodeState_.size(), CompareByCPUandMemory() );

    boost::mutex::scoped_lock scoped_lock_j( jobsMut_ );

    WorkerPriority::iterator it = workerPriority_.Begin();
    for( ; it != workerPriority_.End(); ++it )
    {
        NodeState &nodeState = *(*it);
        int numFreeCPU = nodeState.GetNumFreeCPU();
        if ( numFreeCPU <= 0 )
            continue;

        WorkerPtr &w = nodeState.GetWorker();
        if ( !w->IsAvailable() )
            continue;

        if ( GetJobForWorker( w, workerJob, job, numFreeCPU ) )
        {
            w->GetJob() += workerJob;
            hostIP = w->GetIP();

            nodeState.AllocCPU( workerJob.GetTotalNumTasks() );
            return true;
        }
    }

    // if there is any worker available, but all queued jobs are
    // sended to workers, then take next job from job mgr queue
    scoped_lock_j.unlock();
    scoped_lock_w.unlock();
    PlanJobExecution();

    return false;
}

void Scheduler::OnTaskSendCompletion( bool success, const WorkerJob &workerJob, const std::string &hostIP )
{
    if ( !success )
    {
        {
            WorkerPtr w;
            IWorkerManager *workerManager = common::ServiceLocator::Instance().Get< IWorkerManager >();
            if ( !workerManager->GetWorkerByIP( hostIP, w ) )
                return;

            PLOG( "Scheduler::OnTaskSendCompletion: job sending failed."
                  " jobId=" << workerJob.GetJobId() << ", ip=" << hostIP );

            boost::mutex::scoped_lock scoped_lock( workersMut_ );
            {
                JobPtr j;
                boost::mutex::scoped_lock scoped_lock_j( jobsMut_ );
                if ( !jobs_.FindJobByJobId( workerJob.GetJobId(), j ) )
                    return;
            }

            IPToNodeState::iterator it = nodeState_.find( hostIP );
            if ( it == nodeState_.end() )
                return;

            failedWorkers_.Add( workerJob.GetJobId(), hostIP );

            // worker job should be rescheduled to any other node
            RescheduleJob( w->GetJob() );

            NodeState &nodeState = it->second;
            nodeState.FreeCPU( workerJob.GetTotalNumTasks() );
            w->ResetJob();
        }
        NotifyAll();
    }
}

void Scheduler::OnTaskCompletion( int errCode, int64_t execTime, const WorkerTask &workerTask, const std::string &hostIP )
{
    if ( !errCode )
    {
        WorkerPtr w;
        IWorkerManager *workerManager = common::ServiceLocator::Instance().Get< IWorkerManager >();
        if ( !workerManager->GetWorkerByIP( hostIP, w ) )
            return;

        boost::mutex::scoped_lock scoped_lock_w( workersMut_ );
        boost::mutex::scoped_lock scoped_lock_j( jobsMut_ );

        {
            JobPtr j;
            if ( !jobs_.FindJobByJobId( workerTask.GetJobId(), j ) )
                return;
        }

        WorkerJob &workerJob = w->GetJob();
        if ( !workerJob.DeleteTask( workerTask.GetJobId(), workerTask.GetTaskId() ) )
        {
            // task already processed.
            // it might be possible if a few threads simultaneously gets success errCode from the same task
            // or after timeout
            return;
        }

        IPToNodeState::iterator it = nodeState_.find( hostIP );
        if ( it == nodeState_.end() )
            return;

        PLOG( "Scheduler::OnTaskCompletion: jobId=" << workerTask.GetJobId() <<
              ", taskId=" << workerTask.GetTaskId() << ", execTime=" << execTime << " ms" <<
              ", ip=" << hostIP );

        NodeState &nodeState = it->second;
        nodeState.FreeCPU( 1 );

        jobs_.DecrementJobExecution( workerTask.GetJobId(), 1 );
    }
    else
    {
        if ( errCode == NODE_JOB_COMPLETION_NOT_FOUND )
            return;

        WorkerPtr w;
        IWorkerManager *workerManager = common::ServiceLocator::Instance().Get< IWorkerManager >();
        if ( !workerManager->GetWorkerByIP( hostIP, w ) )
            return;

        boost::mutex::scoped_lock scoped_lock( workersMut_ );
        {
            JobPtr j;
            boost::mutex::scoped_lock scoped_lock_j( jobsMut_ );
            if ( !jobs_.FindJobByJobId( workerTask.GetJobId(), j ) )
                return;
        }

        IPToNodeState::iterator it = nodeState_.find( hostIP );
        if ( it == nodeState_.end() )
            return;

        PLOG( "Scheduler::OnTaskCompletion: errCode=" << errCode <<
              ", jobId=" << workerTask.GetJobId() <<
              ", taskId=" << workerTask.GetTaskId() << ", ip=" << hostIP );

        failedWorkers_.Add( workerTask.GetJobId(), hostIP );

        const WorkerJob &workerJob = w->GetJob();
        // worker job should be rescheduled to any other node
        RescheduleJob( workerJob );

        NodeState &nodeState = it->second;
        nodeState.FreeCPU( workerJob.GetTotalNumTasks() );
        w->ResetJob();
    }

    NotifyAll();
}

void Scheduler::OnTaskTimeout( const WorkerTask &workerTask, const std::string &hostIP )
{
    WorkerPtr w;
    IWorkerManager *workerManager = common::ServiceLocator::Instance().Get< IWorkerManager >();
    if ( !workerManager->GetWorkerByIP( hostIP, w ) )
        return;
    const WorkerJob &workerJob = w->GetJob();

    bool hasTask = false;
    {
        boost::mutex::scoped_lock scoped_lock_w( workersMut_ );
        hasTask = workerJob.HasTask( workerTask.GetJobId(), workerTask.GetTaskId() );
    }

    if ( hasTask )
    {
        PLOG( "Scheduler::OnTaskTimeout " << workerTask.GetJobId() << ":" << workerTask.GetTaskId() << " " << hostIP );

        // send stop command to worker
        StopTaskCommand *stopCommand = new StopTaskCommand();
        stopCommand->SetParam( "job_id", workerTask.GetJobId() );
        stopCommand->SetParam( "task_id", workerTask.GetTaskId() );
        CommandPtr commandPtr( stopCommand );
        workerManager->AddCommand( commandPtr, hostIP );

        OnTaskCompletion( NODE_JOB_TIMEOUT, 0, workerTask, hostIP );
    }
}

void Scheduler::OnJobTimeout( int64_t jobId )
{
    {
        boost::mutex::scoped_lock scoped_lock_w( workersMut_ );
        boost::mutex::scoped_lock scoped_lock_j( jobsMut_ );

        {
            JobPtr j;
            if ( !jobs_.FindJobByJobId( jobId, j ) )
                return;
        }
        StopWorkers( jobId );
        jobs_.RemoveJob( jobId, "timeout" );
    }
    NotifyAll();
}

void Scheduler::StopJob( int64_t jobId )
{
    OnJobTimeout( jobId );
}

void Scheduler::StopJobGroup( int64_t groupId )
{
    std::list< JobPtr > jobs;
    {
        boost::mutex::scoped_lock scoped_lock( jobsMut_ );
        jobs_.GetJobGroup( groupId, jobs );
    }
    std::list< JobPtr >::const_iterator it = jobs.begin();
    for( ; it != jobs.end(); ++it )
    {
        const JobPtr &job = *it;
        StopJob( job->GetJobId() );
    }
}

void Scheduler::StopAllJobs()
{
    ScheduledJobs::JobList jobs;
    {
        boost::mutex::scoped_lock scoped_lock( jobsMut_ );
        jobs = jobs_.GetJobList();
    }
    ScheduledJobs::JobList::const_iterator it = jobs.begin();
    for( ; it != jobs.end(); ++it )
    {
        const JobPtr &job = *it;
        StopJob( job->GetJobId() );
    }

    // send stop all command
    {
        IWorkerManager *workerManager = common::ServiceLocator::Instance().Get< IWorkerManager >();

        boost::mutex::scoped_lock scoped_lock( workersMut_ );
        IPToNodeState::const_iterator it = nodeState_.begin();
        for( ; it != nodeState_.end(); ++it )
        {
            const NodeState &nodeState = it->second;
            const WorkerPtr &worker = nodeState.GetWorker();

            Command *stopCommand = new StopAllJobsCommand();
            CommandPtr commandPtr( stopCommand );
            workerManager->AddCommand( commandPtr, worker->GetIP() );
        }
    }
}

void Scheduler::StopPreviousJobs()
{
    IWorkerManager *workerManager = common::ServiceLocator::Instance().Get< IWorkerManager >();

    boost::mutex::scoped_lock scoped_lock( workersMut_ );
    IPToNodeState::const_iterator it = nodeState_.begin();
    for( ; it != nodeState_.end(); ++it )
    {
        const NodeState &nodeState = it->second;
        const WorkerPtr &worker = nodeState.GetWorker();

        Command *stopCommand = new StopPreviousJobsCommand();
        CommandPtr commandPtr( stopCommand );
        workerManager->AddCommand( commandPtr, worker->GetIP() );
    }
}

void Scheduler::OnRemoveJob( int64_t jobId )
{
    failedWorkers_.Delete( jobId );
}

void Scheduler::StopWorkers( int64_t jobId )
{
    {
        IWorkerManager *workerManager = common::ServiceLocator::Instance().Get< IWorkerManager >();

        IPToNodeState::iterator it = nodeState_.begin();
        for( ; it != nodeState_.end(); ++it )
        {
            NodeState &nodeState = it->second;
            WorkerPtr &worker = nodeState.GetWorker();
            WorkerJob &workerJob = worker->GetJob();

            if ( workerJob.HasJob( jobId ) )
            {
                // send stop command to worker
                WorkerJob::Tasks tasks;
                workerJob.GetTasks( jobId, tasks );
                WorkerJob::Tasks::const_iterator it_task = tasks.begin();
                for( ; it_task != tasks.end(); ++it_task )
                {
                    StopTaskCommand *stopCommand = new StopTaskCommand();
                    stopCommand->SetParam( "job_id", jobId );
                    stopCommand->SetParam( "task_id", *it_task );
                    CommandPtr commandPtr( stopCommand );
                    workerManager->AddCommand( commandPtr, worker->GetIP() );
                }

                nodeState.FreeCPU( workerJob.GetNumTasks( jobId ) );
                workerJob.DeleteJob( jobId );
            }
        }
    }

    tasksToSend_.erase( jobId );
    {
        TaskList::iterator it = needReschedule_.begin();
        for( ; it != needReschedule_.end(); )
        {
            if ( it->GetJobId() == jobId )
            {
                needReschedule_.erase( it++ );
                continue;
            }
            ++it;
        }
    }
}

void Scheduler::StopWorker( const std::string &hostIP ) const
{
    IPToNodeState::const_iterator it = nodeState_.find( hostIP );
    if ( it == nodeState_.end() )
        return;

    const NodeState &nodeState = it->second;
    const WorkerPtr &worker = nodeState.GetWorker();
    const WorkerJob &workerJob = worker->GetJob();

    IWorkerManager *workerManager = common::ServiceLocator::Instance().Get< IWorkerManager >();

    std::vector< WorkerTask > tasks;
    workerJob.GetTasks( tasks );
    std::vector< WorkerTask >::const_iterator it_task = tasks.begin();
    for( ; it_task != tasks.end(); ++it_task )
    {
        StopTaskCommand *stopCommand = new StopTaskCommand();
        stopCommand->SetParam( "job_id", it_task->GetJobId() );
        stopCommand->SetParam( "task_id", it_task->GetTaskId() );
        CommandPtr commandPtr( stopCommand );
        workerManager->AddCommand( commandPtr, worker->GetIP() );
    }
}

bool Scheduler::CanTakeNewJob() const
{
    IPToNodeState::const_iterator it = nodeState_.begin();
    for( ; it != nodeState_.end(); ++it )
    {
        const NodeState &nodeState = it->second;
        if ( nodeState.GetNumFreeCPU() > 0 )
            return true;
    }

    return false;
}

bool Scheduler::CanAddTaskToWorker( const WorkerJob &workerJob, const WorkerJob &workerPlannedJob,
                                    int64_t jobId, const JobPtr &job ) const
{
    // job exclusive case
    if ( job->IsExclusive() )
    {
        if ( workerJob.GetNumJobs() > 1 )
            return false;

        int64_t id = workerJob.GetJobId();
        if ( id != -1 && id != jobId )
            return false;
    }

    // max cpu host limit case
    int maxCPU = job->GetMaxCPU();
    if ( maxCPU < 0 )
        return true;

    int numTasks = workerJob.GetNumTasks( jobId ) + workerPlannedJob.GetNumTasks( jobId );
    return numTasks < maxCPU;
}

int Scheduler::GetNumPlannedExec( const JobPtr &job ) const
{
    if ( job->GetNumExec() > 0 )
        return job->GetNumExec();

    IWorkerManager *workerManager = common::ServiceLocator::Instance().Get< IWorkerManager >();
    int totalCPU = workerManager->GetTotalCPU();
    int maxClusterCPU = job->GetMaxClusterCPU();
    int numExec = 1; // init just to suppress compiler warning

    if ( maxClusterCPU <= 0 )
        numExec = totalCPU;
    else
        numExec = std::min( maxClusterCPU, totalCPU );

    if ( numExec < 1 )
        numExec = 1;
    return numExec;
}

void Scheduler::Accept( SchedulerVisitor *visitor )
{
    boost::mutex::scoped_lock scoped_lock_w( workersMut_ );
    boost::mutex::scoped_lock scoped_lock_j( jobsMut_ );

    visitor->Visit( *this );
}

void Scheduler::Shutdown()
{
    jobs_.Clear();
}

} // namespace master
