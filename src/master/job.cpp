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

#include "job.h"
#include "common/log.h"

namespace master {

void JobGroup::OnJobCompletion( const JobVertex &vertex )
{
    using namespace boost;

    JobGraph::out_edge_iterator i, i_end;
    for( tie( i, i_end ) = out_edges( vertex, graph_ ); i != i_end; ++i )
    {
        JobVertex out = target( *i, graph_ );
        Job *job = indexToJob_[ out ];

        int numDeps = job->GetNumDepends();
        job->SetNumDepends( numDeps - 1 );
    }
}

void Job::ReleaseJobGroup()
{
    if ( jobGroup_ )
        jobGroup_->OnJobCompletion( graphVertex_ );
}

bool Job::IsHostPermitted( const std::string &host ) const
{
    if ( !hosts_.size() )
        return true;

    std::set< std::string >::const_iterator it = hosts_.find( host );
    return it != hosts_.end();
}

bool Job::IsGroupPermitted( const std::string &group ) const
{
    if ( !groups_.size() )
        return true;

    std::set< std::string >::const_iterator it = groups_.find( group );
    return it != groups_.end();
}

void JobQueue::PushJob( Job *job, int64_t groupId )
{
    boost::mutex::scoped_lock scoped_lock( jobsMut_ );
    job->SetGroupId( groupId );
    JobPtr j( job );
    jobs_.push_back( j );
    idToJob_[ job->GetJobId() ] = j;
    ++numJobs_;
}

void JobQueue::PushJobs( std::list< Job * > &jobs, int64_t groupId )
{
    boost::mutex::scoped_lock scoped_lock( jobsMut_ );
    std::list< Job * >::const_iterator it = jobs.begin();
    for( ; it != jobs.end(); ++it )
    {
        Job *job = *it;
        job->SetGroupId( groupId );
        JobPtr j( job );
        jobs_.push_back( j );
        idToJob_[ job->GetJobId() ] = j;
        ++numJobs_;
    }
}

bool JobQueue::GetJobById( int64_t jobId, JobPtr &job )
{
    boost::mutex::scoped_lock scoped_lock( jobsMut_ );
    IdToJob::const_iterator it = idToJob_.find( jobId );
    if ( it != idToJob_.end() )
    {
        job = it->second;
        return true;
    }
    return false;
}

bool JobQueue::DeleteJob( int64_t jobId )
{
    boost::mutex::scoped_lock scoped_lock( jobsMut_ );

    {
        IdToJob::iterator it = idToJob_.find( jobId );
        if ( it == idToJob_.end() )
            return false;
        idToJob_.erase( it );
    }

    JobList::iterator it = jobs_.begin();
    for( ; it != jobs_.end(); ++it )
    {
        JobPtr &job = *it;
        if ( job->GetJobId() != jobId )
            continue;

        std::ostringstream ss;
        ss << "================" << std::endl <<
            "Job deleted from job queue, jobId = " << job->GetJobId() << std::endl <<
            "completion status: failed" << std::endl <<
            "================";

        PLOG( ss.str() );

        boost::property_tree::ptree params;
        params.put( "job_id", job->GetJobId() );
        params.put( "user_msg", ss.str() );

        job->RunCallback( "on_job_deletion", params );
        job->ReleaseJobGroup();

        jobs_.erase( it );
        --numJobs_;
        return true;
    }
    return false;
}

bool JobQueue::DeleteJobGroup( int64_t groupId )
{
    JobList jobs;
    bool deleted = false;
    {
        boost::mutex::scoped_lock scoped_lock( jobsMut_ );
        JobList::iterator it = jobs_.begin();
        for( ; it != jobs_.end(); ++it )
        {
            JobPtr &job = *it;
            if ( job->GetGroupId() == groupId )
                jobs.push_back( job );
        }
    }

    JobList::const_iterator it = jobs.begin();
    for( ; it != jobs.end(); ++it )
    {
        const JobPtr &job = *it;
        if ( job->GetGroupId() == groupId )
            deleted = DeleteJob( job->GetJobId() );
    }

    return deleted;
}

bool JobQueue::PopJob( JobPtr &job )
{
    boost::mutex::scoped_lock scoped_lock( jobsMut_ );
    if ( numJobs_ )
    {
        JobList jobs;
        Sort( jobs );

        JobList::iterator it = jobs.begin();
        for( ; it != jobs.end(); ++it )
        {
            JobPtr &j = *it;
            if ( j->GetNumDepends() > 0 )
                continue;

            JobList::iterator i = jobs_.begin();
            for( ; i != jobs_.end(); ++i )
            {
                if ( j == *i )
                {
                    jobs_.erase( i );
                    break;
                }
            }

            idToJob_.erase( j->GetJobId() );
            --numJobs_;
            job = j;
            return true;
        }
    }
    return false;
}

void JobQueue::Clear()
{
    boost::mutex::scoped_lock scoped_lock( jobsMut_ );
    jobs_.clear();
    idToJob_.clear();
    numJobs_ = 0;
}

struct JobComparatorPriority
{
    bool operator() ( const JobPtr &a, const JobPtr &b ) const
    {
        if ( a->GetPriority() < b->GetPriority() )
            return true;
        if ( a->GetPriority() == b->GetPriority() )
        {
            if ( a->GetGroupId() < b->GetGroupId() )
                return true;
        }
        return false;
    }
};

void JobQueue::Sort( JobList &jobs )
{
    JobList::iterator it = jobs_.begin();
    for( ; it != jobs_.end(); ++it )
    {
        JobPtr &job = *it;
        if ( job->GetNumDepends() == 0 )
        {
            jobs.push_back( job );
        }
    }

    // sort jobs by priority, saving group order
    jobs.sort( JobComparatorPriority() );
    //PrintJobs( jobs );
}

void JobQueue::PrintJobs( const JobList &jobs ) const
{
    std::ostringstream ss;
    ss << std::endl;
    JobList::const_iterator it = jobs.begin();
    for( ; it != jobs.end(); ++it )
    {
        if ( it != jobs.begin() )
            ss << "," << std::endl;
        ss << "(priority=" << (*it)->GetPriority() <<
            ", groupid=" << (*it)->GetGroupId() << ")";
    }
    PLOG( ss.str() );
}

} // namespace master
