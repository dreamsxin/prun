/*
===========================================================================

This software is licensed under the Apache 2 license, quoted below.

Copyright (C) 2015 Andrey Budnik <budnik27@gmail.com>

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

#ifndef __SHARED_LIBRARY_H
#define __SHARED_LIBRARY_H

namespace common {

class SharedLibrary
{
public:
    SharedLibrary();
    ~SharedLibrary();

    bool Load( const char *fileName );
    void Close();

    void *GetFunction( const char *function );

private:
    void *handle_;
};

} // namespace common

#endif
