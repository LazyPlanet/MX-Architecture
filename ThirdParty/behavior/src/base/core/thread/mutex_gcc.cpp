/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Tencent is pleased to support the open source community by making behaviac available.
//
// Copyright (C) 2015 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except in compliance with
// the License. You may obtain a copy of the License at http://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed under the License is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and limitations under the License.
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include "behaviac/base/core/thread/mutex.h"
//#include "behaviac/base/core/logging/log.h"
//#include <stdio.h>

#if !BEHAVIAC_COMPILER_MSVC
#include <pthread.h>
#include <errno.h>

namespace behaviac
{
#if BEHAVIAC_COMPILER_APPLE
    ////////////////////////////////////////////////////////////////////////////////
    Mutex::Mutex() : _impl(0)
    {
    }

    ////////////////////////////////////////////////////////////////////////////////
    Mutex::~Mutex()
    {
    }

    ////////////////////////////////////////////////////////////////////////////////
    void Mutex::Lock()
    {
    }

    ////////////////////////////////////////////////////////////////////////////////
    void Mutex::Unlock()
    {
    }

    ////////////////////////////////////////////////////////////////////////////////
    bool Mutex::TryLock()
    {
        return false;
    }
#else
    struct Mutex::MutexImpl
    {
        pthread_mutex_t _mutex;
    };

    ////////////////////////////////////////////////////////////////////////////////
    Mutex::Mutex() : _impl(0)
    {
        // uint32_t s = sizeof(MutexImpl);
        // printf("size of MutexImpl %d\n", s);
        // Be sure that the shadow is large enough
        BEHAVIAC_ASSERT(sizeof(m_Shadow) >= sizeof(MutexImpl));

        // Use the shadow as memory space for the platform specific implementation
        _impl = (MutexImpl*)m_Shadow;

        pthread_mutex_init(&_impl->_mutex, 0);
    }

    ////////////////////////////////////////////////////////////////////////////////
    Mutex::~Mutex()
    {
        pthread_mutex_destroy(&_impl->_mutex);
    }

    ////////////////////////////////////////////////////////////////////////////////
    void Mutex::Lock()
    {
        int rval = pthread_mutex_lock(&_impl->_mutex);
        BEHAVIAC_ASSERT(!rval, "critical_section::lock: pthread_mutex_lock failed");
        BEHAVIAC_UNUSED_VAR(rval);
    }

    ////////////////////////////////////////////////////////////////////////////////
    void Mutex::Unlock()
    {
        int rval = pthread_mutex_unlock(&_impl->_mutex);
        BEHAVIAC_UNUSED_VAR(rval);
        BEHAVIAC_ASSERT(!rval, "critical_section::unlock: pthread_mutex_unlock failed");
    }

    ////////////////////////////////////////////////////////////////////////////////
    bool Mutex::TryLock()
    {
#if !BEHAVIAC_COMPILER_GCC_LINUX
        int rval = pthread_mutex_trylock(&_impl->_mutex);
        // valid returns are 0 (locked) and [EBUSY]
        BEHAVIAC_ASSERT(rval == 0 || rval == EBUSY, "critical_section::trylock: pthread_mutex_trylock failed");
        bool gotlock = (rval == 0);

        return gotlock;
#else
        return false;
#endif
    }
#endif//BEHAVIAC_COMPILER_APPLE
}//namespace behaviac

#endif//#if !BEHAVIAC_COMPILER_MSVC
