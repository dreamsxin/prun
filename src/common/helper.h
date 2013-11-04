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
#ifndef __HELPER_H
#define __HELPER_H

#include <boost/thread/condition.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/archive/iterators/base64_from_binary.hpp>
#include <boost/archive/iterators/binary_from_base64.hpp>
#include <boost/archive/iterators/transform_width.hpp>
#include "common/log.h"

namespace common {

class Semaphore
{
public:
    Semaphore( unsigned int v )
    : count_( v )
    {}

    void Notify()
    {
        boost::mutex::scoped_lock lock( mutex_ );
        ++count_;
        condition_.notify_one();
    }

    void Wait()
    {
        boost::mutex::scoped_lock lock( mutex_ );
        while( !count_ )
            condition_.wait( lock );
        --count_;
    }

private:
    boost::mutex mutex_;
    boost::condition_variable condition_;
    unsigned int count_;
};

class SyncTimer
{
public:
    void StopWaiting()
    {
        boost::mutex::scoped_lock lock( mutex_ );
        condition_.notify_one();
    }

    bool Wait( int millisec )
    {
        boost::mutex::scoped_lock lock( mutex_ );
        const boost::system_time timeout = boost::get_system_time() + boost::posix_time::milliseconds( millisec );
        return !condition_.timed_wait( lock, timeout );
    }

private:
    boost::mutex mutex_;
    boost::condition_variable condition_;
};

template< typename T >
bool EncodeBase64( const T *data, std::size_t size, std::string &out )
{
    using namespace boost::archive::iterators;
    typedef base64_from_binary< transform_width< char*, 6, 8, boost::uint8_t > > translate_out;
    try
    {
        out.append( translate_out( data ), translate_out( data + size ) );
    }
    catch( std::exception &e )
    {
        PS_LOG( "EncodeBase64: " << e.what() );
        return false;
    }
    return true;
}

template< typename Container >
bool DecodeBase64( std::string &data, Container &out )
{
    using namespace boost::archive::iterators;
    typedef transform_width< binary_from_base64< std::string::const_iterator >, 8, 6, boost::uint8_t > translate_in;

    std::size_t padding = data.size() % 4;
    data.append( padding, std::string::value_type( '=' ) );

    try
    {
        std::copy( translate_in( data.begin() ), translate_in( data.end() - padding ), std::back_inserter( out ) );
    }
    catch( std::exception &e )
    {
        // crutch for different base64 decode behaviour varying on boost version 
        try
        {
            out.clear();
            std::size_t pos = data.size();
            if ( padding )
            {
                pos = data.find_last_not_of( std::string::value_type( '=' ) );
                pos = ( pos == data.size() - 1 ) ? data.size() : pos;
            }
            std::copy( translate_in( data.begin() ), translate_in( data.begin() + pos ), std::back_inserter( out ) );
        }
        catch( std::exception &e )
        {
            PS_LOG( "DecodeBase64: " << e.what() );
            return false;
        }
    }
    return true;
}

} // namespace common

#endif
