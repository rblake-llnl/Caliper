// Copyright (c) 2019, Lawrence Livermore National Security, LLC.
// See top-level LICENSE file for details.

// PthreadService.cpp
// Service for pthreads-based threading runtimes

#include "caliper/CaliperService.h"

#include "../util/ChannelList.hpp"

#include "caliper/Caliper.h"

#include "caliper/common/Log.h"

#include <pthread.h>

#include <gotcha/gotcha.h>

#include <algorithm>
#include <cstdlib>
#include <vector>

using namespace cali;
using util::ChannelList;

namespace
{

gotcha_wrappee_handle_t  orig_pthread_create_handle = 0x0;

Attribute id_attr = Attribute::invalid;
Attribute master_attr = Attribute::invalid;

ChannelList* pthread_channels = nullptr;

bool is_wrapped = false;

struct wrapper_args {
    void* (*fn)(void*);
    void* arg;
};

// Wrapper for the user-provided thread start function.
// We wrap the original thread start function to create Caliper thread scope
// on the new child thread.
void*
thread_wrapper(void *arg)
{
    uint64_t id = static_cast<uint64_t>(pthread_self());
    Caliper  c;

    for (ChannelList* p = pthread_channels; p; p = p->next)
        if (p->channel->is_active()) {
            c.set(master_attr, Variant(false));
            c.set(p->channel, id_attr, Variant(cali_make_variant_from_uint(id)));
        }

    wrapper_args* wrap = static_cast<wrapper_args*>(arg);
    void* ret = (*(wrap->fn))(wrap->arg);

    delete wrap;
    return ret;
}

// Wrapper for pthread_create()
int
cali_pthread_create_wrapper(pthread_t *thread, const pthread_attr_t *attr,
                            void *(*fn)(void*), void* arg)
{
    decltype(&pthread_create) orig_pthread_create =
        reinterpret_cast<decltype(&pthread_create)>(gotcha_get_wrappee(orig_pthread_create_handle));
    
    return (*orig_pthread_create)(thread, attr, thread_wrapper, new wrapper_args({ fn, arg }));
}

void
post_init_cb(Caliper* c, Channel* chn)
{
    uint64_t id = static_cast<uint64_t>(pthread_self());
    
    c->set(chn, master_attr, Variant(true));
    c->set(chn, id_attr, Variant(cali_make_variant_from_uint(id)));
}

// Initialization routine.
void 
pthreadservice_initialize(Caliper* c, Channel* chn)
{
    id_attr =
        c->create_attribute("pthread.id", CALI_TYPE_UINT,
                            CALI_ATTR_SCOPE_THREAD);
    master_attr =
        c->create_attribute("pthread.is_master", CALI_TYPE_BOOL,
                            CALI_ATTR_SCOPE_THREAD |
                            CALI_ATTR_SKIP_EVENTS);

    if (!is_wrapped) {
        struct gotcha_binding_t pthread_binding[] = { 
            { "pthread_create", (void*) cali_pthread_create_wrapper, &orig_pthread_create_handle }
        };

        gotcha_wrap(pthread_binding, sizeof(pthread_binding)/sizeof(struct gotcha_binding_t),
                    "caliper/pthread");

        is_wrapped = true;
    }

    ChannelList::add(&pthread_channels, chn);

    chn->events().post_init_evt.connect(
        [](Caliper* c, Channel* chn){
            post_init_cb(c, chn);
        });
    chn->events().finish_evt.connect(
        [](Caliper* c, Channel* chn){
            ChannelList::remove(&pthread_channels, chn);
        });

    Log(1).stream() << chn->name() << ": Registered pthread service" << std::endl;
}

} // namespace [anonymous]

namespace cali
{

CaliperService pthread_service { "pthread", ::pthreadservice_initialize };

}
