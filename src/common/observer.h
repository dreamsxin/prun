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

#ifndef __OBSERVER_H
#define __OBSERVER_H

#include <set>
#include <map>
#include <boost/thread/locks.hpp>  
#include <boost/thread/shared_mutex.hpp> 

namespace common {

class Observer
{
public:
    virtual void NotifyObserver( int event ) = 0;
    virtual ~Observer() {}
};

class MutexLockPolicy
{
    typedef boost::shared_mutex MutexType;

public:
    class UniqueLock
    {
    public:
        UniqueLock( MutexLockPolicy *policy )
        : lock_( policy->GetLock() ), uniqueLock_( lock_ )
        {}

    private:
        boost::upgrade_lock< MutexType > lock_;
        boost::upgrade_to_unique_lock< MutexType > uniqueLock_;
    };

    class SharedLock
    {
    public:
        SharedLock( MutexLockPolicy *policy )
        : lock_( policy->GetLock() )
        {}

    private:
        boost::shared_lock< MutexType > lock_;
    };

    MutexType &GetLock() { return mut_; }

private:
    MutexType mut_;
};

class NullLockPolicy
{
public:
    class UniqueLock
    {
    public:
        UniqueLock( NullLockPolicy *policy ) {}
    };

    class SharedLock
    {
    public:
        SharedLock( NullLockPolicy *policy ) {}
    };
};

template< typename LockPolicy = NullLockPolicy >
class Observable : private LockPolicy
{
    typedef std::set<Observer *> Container;
    typedef std::map< int, Container > EventToContainer;

public:
    void Subscribe( Observer *observer, int event = 0 )
    {
        typename LockPolicy::UniqueLock lock( this );
        observers_[ event ].insert( observer );
    }

    void Unsubscribe( Observer *observer, int event = 0 )
    {
        typename LockPolicy::UniqueLock lock( this );
        EventToContainer::iterator it = observers_.find( event );
        if ( it != observers_.end() )
        {
            it->second.erase( observer );
        }
    }

    void NotifyAll( int event = 0 )
    {
        typename LockPolicy::SharedLock lock( this );
        EventToContainer::iterator it = observers_.find( event );
        if ( it == observers_.end() )
            return;

        Container::iterator it_ob = it->second.begin();
        for( ; it_ob != it->second.end(); ++it_ob )
        {
            Observer *observer = *it_ob;
            observer->NotifyObserver( event );
        }
    }

private:
    EventToContainer observers_;
};

} // namespace common

#endif
