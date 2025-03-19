//
//  libtakeover.cpp
//  libtakeover
//
//  Created by tihmstar on 24.02.19.
//  Copyright © 2019 tihmstar. All rights reserved.
//

#include "../include/libtakeover/libtakeover.hpp"
#include "../include/libtakeover/TKexception.hpp"
#include <stdlib.h>
#include <dlfcn.h>
#include <pthread/pthread.h>
#include <pthread/stack_np.h>
#include <stdlib.h>
#include <libgeneral/macros.h>
#include <ptrauth.h>
#include <mach-o/dyld_images.h>
#include <objc/runtime.h>
#include <unistd.h>

#ifdef DUMP_CRASH_BACKTRACE
#include <sys/utsname.h>
#include <dlfcn.h>
#endif

using namespace tihmstar;

extern "C"{
kern_return_t mach_vm_allocate(vm_map_t target, mach_vm_address_t *address, mach_vm_size_t size, int flags);
kern_return_t mach_vm_protect(vm_map_t target_task, mach_vm_address_t address, mach_vm_size_t size, boolean_t set_maximum, vm_prot_t new_protection);
kern_return_t mach_vm_read_overwrite(vm_map_t target_task, mach_vm_address_t address, mach_vm_size_t size, mach_vm_address_t data, mach_vm_size_t *outsize);
kern_return_t mach_vm_write(vm_map_t target_task, mach_vm_address_t address, vm_offset_t data, mach_msg_type_number_t dataCnt);
kern_return_t mach_vm_deallocate(mach_port_name_t target, mach_vm_address_t address, mach_vm_size_t size);
};

#pragma pack(4)
typedef struct {
    mach_msg_header_t Head;
    mach_msg_body_t msgh_body;
    mach_msg_port_descriptor_t thread;
    mach_msg_port_descriptor_t task;
    int unused1;
    exception_type_t exception;
    exception_data_type_t code;
    int unused2;
    uint64_t subcode;
    my_exception_state_t estate;
    my_thread_state_t state;
    NDR_record_t NDR;
} exception_raise_request; // the bits we need at least

typedef struct {
    mach_msg_header_t Head;
    NDR_record_t NDR;
    kern_return_t RetCode;
} exception_raise_reply;

typedef struct {
    mach_msg_header_t Head;
    NDR_record_t NDR;
    kern_return_t RetCode;
    int flavor;
    mach_msg_type_number_t new_stateCnt;
    natural_t new_state[614];
} exception_raise_state_reply;
#pragma pack()

#pragma mark helper


#define    INSTACK(a)    ((a) >= stackbot && (a) <= stacktop)
#if defined(__x86_64__)
#define    ISALIGNED(a)    ((((uintptr_t)(a)) & 0xf) == 0)
#elif defined(__i386__)
#define    ISALIGNED(a)    ((((uintptr_t)(a)) & 0xf) == 8)
#elif defined(__arm__) || defined(__arm64__)
#define    ISALIGNED(a)    ((((uintptr_t)(a)) & 0x1) == 0)
#endif

__attribute__((noinline))
static void remote_pthread_backtrace(takeover &crp, void *remote_pthread, vm_address_t *buffer, unsigned max, unsigned *nb, unsigned skip, const void *startfp_){
    const uint8_t *startfp = (const uint8_t*)startfp_;
    const uint8_t *frame = 0;
    uint8_t *next = 0;
    
    uint8_t *stacktop = (uint8_t *)crp.callfunc(dlsym(RTLD_NEXT, "pthread_get_stackaddr_np"), {(cpuword_t)remote_pthread});
    uint8_t *stackbot = stacktop - (size_t)crp.callfunc(dlsym(RTLD_NEXT, "pthread_get_stacksize_np"), {(cpuword_t)remote_pthread});

    *nb = 0;

    // Rely on the fact that our caller has an empty stackframe (no local vars)
    // to determine the minimum size of a stackframe (frame ptr & return addr)
    frame = startfp;
    next = (uint8_t *)crp.callfunc(dlsym(RTLD_NEXT, "pthread_stack_frame_decode_np"), {(cpuword_t)frame, 0});

    /* make sure return address is never out of bounds */
    stacktop -= (next - frame);

    if(!INSTACK(frame) || !ISALIGNED(frame))
        return;
    while (startfp || skip--) {
        if (startfp && startfp < next) break;
        if(!INSTACK(next) || !ISALIGNED(next) || next <= frame)
            return;
        frame = next;
        next = (uint8_t *)crp.callfunc(dlsym(RTLD_NEXT, "pthread_stack_frame_decode_np"), {(cpuword_t)frame, 0});
    }
    while (max--) {
        uintptr_t retaddr = 0;
        next = (uint8_t *)crp.callfunc(dlsym(RTLD_NEXT, "pthread_stack_frame_decode_np"), {(cpuword_t)frame, (cpuword_t)remote_pthread + sizeof(pthread_t)});
        crp.readMem((uint8_t*)remote_pthread + sizeof(pthread_t), &retaddr, sizeof(retaddr));
        buffer[*nb] = retaddr;
        (*nb)++;
        if(!INSTACK(next) || !ISALIGNED(next) || next <= frame)
            return;
        frame = next;
    }
}


#pragma mark takeover
takeover::takeover()
    /* init member vars */
    :_remoteStack(0),_marionetteThread(MACH_PORT_NULL),_exceptionHandler(MACH_PORT_NULL),_remoteSelf(MACH_PORT_NULL),_emsg({}),
    _isFakeThread(true), _remoteScratchSpace(NULL), _remoteScratchSpaceSize(0), _signptr_cb{nullptr}
,_remotePthread(NULL),_isCrashReporter(false)
{
    
}

takeover::takeover(mach_port_t target, std::function<cpuword_t(cpuword_t ptr)> signptr_cb)
    /* init member vars */
    :_target(target),_remoteStack(0),_marionetteThread(MACH_PORT_NULL),_exceptionHandler(MACH_PORT_NULL),_remoteSelf(MACH_PORT_NULL),_emsg({}),
_isFakeThread(true), _remoteScratchSpace(NULL), _remoteScratchSpaceSize(0), _signptr_cb{signptr_cb}
,_isRemotePACed(false)
,_remotePthread(NULL),_isCrashReporter(false)
{
    bool didConstructSuccessfully = false;
    cpuword_t *localStack = NULL;
    cleanup([&]{
        safeFree(localStack);
        if (!didConstructSuccessfully){
            try{
                auto err = deinit();
                if(err.first){
                    error("[takeover] deinit failed on line %d with code %d",err.first,err.second);
                }
            }catch(tihmstar::exception &e){
                e.dump();
            }
        }
    });
    kern_return_t err = 0;
    
    /* setup local variables */
    size_t stackpointer = 0;
    mach_msg_type_number_t count = MY_THREAD_STATE_COUNT;
    my_thread_state_t state = {0};
    
#if defined (__arm64__)
    _isRemotePACed = targetIsPACed(_target);
#endif
    
    /* actually construct object */
    
    //aquire send right to targer
    {
        bool didSucced = false;
        cleanup([&]{
            if (!didSucced){
                //if this step fails, make sure not to drop a send right to the target on cleanup!
                _target = MACH_PORT_NULL;
            }
        });
        kern_return_t err = 0;
        retassure(!(err = mach_port_insert_right(mach_task_self(),_target, _target, MACH_MSG_TYPE_COPY_SEND)), "Failed to insert send right");
        didSucced = true;
    }

    //allocate remote stack
    retassure(!mach_vm_allocate(_target, &_remoteStack, _remoteStackSize, VM_FLAGS_ANYWHERE), "Failed to allocate remote stack");
    retassure(!mach_vm_protect(_target, _remoteStack, _remoteStackSize, 1, VM_PROT_READ | VM_PROT_WRITE), "Failed to vm_protect remote stack");


    //setup stack
    retassure(localStack = (cpuword_t *)malloc(_remoteStackSize), "Failed to allocate local stack");
    stackpointer = ((_remoteStackSize/2) / sizeof(cpuword_t))-1; //leave enough space for stack args
#if defined (__arm64__)
    localStack[stackpointer--] = 0x4142434445464748; //magic end (x86 legacy, we don't need this for ARM64, do we?)
#elif defined (__arm__)
    localStack[stackpointer--] = 0x41424344; //magic end (x86 legacy, we don't need this for ARM64, do we?)
#endif

    
    //spawn new thread
    retassure(!thread_create(_target, &_marionetteThread), "Faied to create remote thread");

#if defined (__arm64__)
    localStack[0xf8/8] = (cpuword_t)_marionetteThread;   //thread port
    localStack[0xe0/8] = (cpuword_t)_remoteStack;        //ptr to remotePthreadBuf (lies at the beginning of _remoteStack)
    localStack[0x88/8] = (cpuword_t)0x5152535455565758;  //pc (do a soft crash)
    localStack[0x98/8] = 0x1337;                        //pthread arg1
#elif defined (__arm__)
    localStack[0x4c/4] = (cpuword_t)_remoteStack+0x100;
#endif
    
    //write localStack to remote stack
    retassure(!mach_vm_write(_target, _remoteStack, (vm_offset_t)localStack, (mach_msg_type_number_t)_remoteStackSize), "Failed to write local stack to remote stack");
    
    //setup thread state
    retassure(!thread_get_state(_marionetteThread, MY_THREAD_STATE, (thread_state_t)&state, &count), "Failed to set up thread state");

#if defined (__arm64__)
    state.__x[0] = (cpuword_t)_remoteStack;
    arm_thread_state64_set_lr_fptr(state, ptrauth_sign_unauthenticated((void*)0x71717171, ptrauth_key_function_pointer, 0));
    if (_signptr_cb) {
        state.__x[32]/*PC*/ = (cpuword_t)_signptr_cb((cpuword_t)ptrauth_strip(dlsym(RTLD_NEXT, "thread_start"), ptrauth_key_asia));
    }else{
        arm_thread_state64_set_pc_fptr(state,dlsym(RTLD_NEXT, "thread_start"));
    }
    assure(arm_thread_state64_get_pc(state));
    arm_thread_state64_set_sp(state,_remoteStack + stackpointer*sizeof(cpuword_t));
#elif defined (__arm__)
    state.__r[0] = (cpuword_t)_remoteStack;
    state.__r[1] = (cpuword_t)_marionetteThread;
    state.__r[2] = (cpuword_t)0x51515151;
    state.__r[3] = (cpuword_t)0x1337; //pthread arg1
    state.__r[4] = (cpuword_t)0x00080000; //idk
    state.__r[5] = (cpuword_t)0x080008ff; //idk
    state.__lr = 0x71717171;        //actual magic end
    assure(state.__pc = (cpuword_t)dlsym(RTLD_NEXT, "thread_start"));
    state.__sp = (cpuword_t)(_remoteStack + stackpointer*sizeof(cpuword_t));
#endif
    
    if (dlsym(RTLD_NEXT, "pthread_create_from_mach_thread")){
#if defined (__arm64__)
        if (_signptr_cb) {
            state.__x[32]/*PC*/ = (cpuword_t)_signptr_cb((cpuword_t)0x414141414141);
        }else{
            arm_thread_state64_set_pc_fptr(state,ptrauth_sign_unauthenticated((void*)0x414141414141, ptrauth_key_function_pointer, 0));
        }
#elif defined (__arm__)
        state.__pc = 0x41414141;
#endif
    }

    assure(!(err = thread_set_state(_marionetteThread, MY_THREAD_STATE, (thread_state_t)&state, MY_THREAD_STATE_COUNT)));

    //create exception port
    assure(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &_exceptionHandler));
    assure(!mach_port_insert_right(mach_task_self(),_exceptionHandler, _exceptionHandler, MACH_MSG_TYPE_MAKE_SEND));
    
    //set our new port
    assure(!thread_set_exception_ports(_marionetteThread, EXC_MASK_ALL & ~(EXC_MASK_MACH_SYSCALL | EXC_MASK_SYSCALL | EXC_MASK_RPC_ALERT), _exceptionHandler, EXCEPTION_STATE_IDENTITY | MACH_EXCEPTION_CODES, MY_THREAD_STATE));

    //initialize our remote thread
    assure(!thread_resume(_marionetteThread));
    
    //wait for exception
    assure(!mach_msg(&_emsg.head, MACH_RCV_MSG|MACH_RCV_LARGE, 0, sizeof(_emsg), _exceptionHandler, 0, MACH_PORT_NULL));
        
    //if we have pthread_create_from_mach_thread, we can do things much cleaner
    if (dlsym(RTLD_NEXT, "pthread_create_from_mach_thread")){
        kidnapThread();
    }
    didConstructSuccessfully = true;
}

takeover::takeover(takeover &&tk)
    :_target(tk._target),_remoteStack(tk._remoteStack),_marionetteThread(tk._marionetteThread),_exceptionHandler(tk._exceptionHandler),_emsg(tk._emsg),
    _isFakeThread(tk._isFakeThread), _signptr_cb{tk._signptr_cb}
    ,_remotePthread(tk._remotePthread),_isCrashReporter(tk._isCrashReporter)

{
    tk._remoteStack = 0;
    tk._target = MACH_PORT_NULL;
    tk._exceptionHandler = MACH_PORT_NULL;
    tk._marionetteThread = MACH_PORT_NULL;
    tk._remotePthread = NULL;
}

takeover takeover::takeoverWithExceptionHandler(mach_port_t exceptionHandler){
    takeover tk;
    tk._exceptionHandler = exceptionHandler;
    tk._isFakeThread = false;
    
    assure(!mach_msg(&tk._emsg.head, MACH_RCV_MSG|MACH_RCV_LARGE, 0, sizeof(tk._emsg), tk._exceptionHandler, 0, MACH_PORT_NULL));
    
    tk._marionetteThread = tk._emsg.head.msgh_remote_port;
    
    return tk;
}

void takeover::setSignptrCB(std::function<cpuword_t(cpuword_t ptr)> signptr_cb){
    _signptr_cb = signptr_cb;
}


cpuword_t takeover::callfunc(void *addr, const std::vector<cpuword_t> &x){
    exception_raise_request* req = (exception_raise_request*)&_emsg;
    my_thread_state_t *state = &req->state;
    my_exception_state_t *estate = &req->estate;
    cpuword_t lrmagic = 0x71717171;
    
//#ifdef DEBUG
//    {
//        printf("calling (0x%016llx)(",(uint64_t)addr);
//        for (auto arg : x) {
//            printf("0x%016llx, ",arg);
//        }
//        printf(")\n");
//    }
//#endif
    
#if defined (__arm64__)
    if (_signptr_cb) {
        state->__x[32]/*PC*/ = (uint64_t)_signptr_cb((cpuword_t)addr);
    }else{
        arm_thread_state64_set_pc_fptr(*state,addr);
    }
    arm_thread_state64_set_lr_fptr(*state, ptrauth_sign_unauthenticated((void*)lrmagic, ptrauth_key_function_pointer, 0));
#else
    state->__lr = (uint32_t)lrmagic;
    state->__pc = (cpuword_t)addr;
#endif
    
    
#if defined (__arm64__)
    retassure(x.size() <= 29,"only up to 29 arguments allowed");
    for (int i=0; i<29; i++) {
        state->__x[i] = (i<x.size()) ? x[i] : 0;
    }
#elif defined (__arm__)
    if (x.size() <= 4) {
        retassure(x.size() <= 12,"only up to 12 arguments allowed");
        for (int i=0; i<12; i++) {
            state->__r[i] = (i<x.size()) ? x[i] : 0;
        }
    }else{
        //wtf is this??
        state->__r[0] = x.at(0);
        state->__r[2] = x.at(1);
        //pass args by stack
        for (int i=2; i<x.size(); i++) {
            uint64_t arg = x.at(i);
            writeMem(((uint64_t*)state->__sp)+(i-2), &arg, sizeof(arg));
        }
    }
    
    state->__cpsr = (state->__cpsr & ~(1<<5)) | ((state->__pc & 1) << 5); //set ARM/THUMB mode properly
#endif
    
    exception_raise_state_reply reply = {};

    reply.Head.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(_emsg.head.msgh_bits), 0);
    reply.new_stateCnt = MY_THREAD_STATE_COUNT;
    reply.Head.msgh_remote_port = _emsg.head.msgh_remote_port;
    reply.Head.msgh_local_port = MACH_PORT_NULL;
    reply.Head.msgh_id = _emsg.head.msgh_id + 100;

    reply.NDR = req->NDR;
    reply.RetCode = KERN_SUCCESS;
    reply.flavor = MY_THREAD_STATE;
    memcpy(reply.new_state, state, sizeof(my_thread_state_t));
    
    reply.Head.msgh_size = (mach_msg_size_t)(sizeof(exception_raise_state_reply) - 2456) + (((4 * reply.new_stateCnt))); //straight from MIG
    
    //resume
    assure(!mach_msg(&reply.Head, MACH_SEND_MSG|MACH_MSG_OPTION_NONE, (mach_msg_size_t)reply.Head.msgh_size, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL));
    
    //wait until end of function
    assure(!mach_msg(&_emsg.head, MACH_RCV_MSG|MACH_RCV_LARGE, 0, sizeof(_emsg), _exceptionHandler, 0, MACH_PORT_NULL));
    
    //get result (implicit through receiving mach_msg)
    {
        bool isGoodMagic = false;
#if defined (__arm64__)
        isGoodMagic = (state->__x[32]/*PC*/ & 0xFFFFFFFF) == lrmagic;
#else
        isGoodMagic = (state->__pc & (~1)) == (lrmagic & (~1));
#endif
        
#ifdef DUMP_CRASH_BACKTRACE
        if (!isGoodMagic) {
            if (_isCrashReporter) {
                error("not printing backtrace in crash reporter mode!");
            }else{
#ifndef XCODE
                try {
#endif
                    vm_address_t *bt = NULL;
                    cleanup([&]{
                        safeFree(bt);
                    });
                    void *fp = NULL;
                    unsigned btCnt = 100;
                    
                    assure(bt = (vm_address_t*)calloc(btCnt,sizeof(vm_address_t)));
                    
        #if defined (__arm64__)
                    fp = (void *)__darwin_arm_thread_state64_get_fp(*state);
        #else
                    fp = (void *)__darwin_arm_thread_state64_get_fp(state->__r[11]);
        #endif
                    takeover crp(_target);
                    crp._isCrashReporter = true;
                    void *remote_pthread = 0;
                    crp.readMem(_remotePthread, &remote_pthread, sizeof(remote_pthread));
                    remote_pthread_backtrace(crp, remote_pthread, bt, btCnt, &btCnt, 0, fp);
                    remote_crashreporter_dump(crp, req->code, (int)req->subcode, *state, *estate, bt);
#ifndef XCODE
                } catch (tihmstar::exception &e) {
                    error("Failed to get remote backtrace:\n%s",e.dumpStr().c_str());
                }
#endif
            }
        }
#endif
        
#if defined (__arm64__)
        retcustomassure(TKexception_Bad_PC_Magic, isGoodMagic, "unexpected PC after callfunc=0x%016llx",state->__x[32]);
#else
        retcustomassure(TKexception_Bad_PC_Magic, isGoodMagic, "unexpected PC after callfunc=0x%08x",state->__pc);
#endif
    }
    
#if defined (__arm64__)
    return state->__x[0];
#elif defined (__arm__)
    return state->__r[0];
#endif
}

void takeover::kidnapThread(){
    if (!_isFakeThread) return;
    
    bool lockIsInited = false;
    void *mem_mutex           = NULL;
    void *func_mutex_destroy_pc  = NULL;
    mach_port_t kidnapped_thread = 0;
    mach_port_t newExceptionHandler = 0;
    cleanup([&]{
        if (lockIsInited) {
            try {
                callfunc(func_mutex_destroy_pc, {(cpuword_t)mem_mutex});
            } catch (tihmstar::exception &e) {
                e.dump();
            }
            lockIsInited = false;
        }
        if (mem_mutex) {
            try {
                deallocMem(mem_mutex,sizeof(pthread_mutex_t));
            } catch (tihmstar::exception &e) {
                e.dump();
            }
            mem_mutex = NULL;
        }
        safeFreeCustom(kidnapped_thread, thread_terminate);
        if (newExceptionHandler) {
            mach_port_destroy(mach_task_self(), newExceptionHandler);
            newExceptionHandler = NULL;
        }
    });
    
    void *func_mutex_init_pc        = NULL;
    void *func_mutex_lock_pc        = NULL;
    void *func_mutex_unlock_pc      = NULL;
    void *func_pthread_attr_init_pc = NULL;
    void *func_pthread_create_pc    = NULL;
    void *pt_from_mt_pc = NULL;
    cpuword_t ret = 0;
    thread_array_t threadList = 0;
    mach_msg_type_number_t threadCount = 0;
    my_thread_state_t state = {0};
    mach_msg_type_number_t count = MY_THREAD_STATE_COUNT;

    bool isThreadSuspended = false;
    
    assure(mem_mutex = allocMem(sizeof(pthread_mutex_t)));
    assure(_remotePthread = allocMem(sizeof(pthread_t)));

#define _PTHREAD_CREATE_FROM_MACH_THREAD  0x1
#define _PTHREAD_CREATE_SUSPENDED         0x2
    
    assure(func_mutex_init_pc           = dlsym(RTLD_NEXT, "pthread_mutex_init"));
    assure(func_mutex_destroy_pc        = dlsym(RTLD_NEXT, "pthread_mutex_destroy"));
    assure(func_mutex_lock_pc           = dlsym(RTLD_NEXT, "pthread_mutex_lock"));
    assure(func_mutex_unlock_pc         = dlsym(RTLD_NEXT, "pthread_mutex_unlock"));
    assure(func_pthread_attr_init_pc    = dlsym(RTLD_NEXT, "pthread_attr_init"));
    assure(func_pthread_create_pc       = dlsym(RTLD_NEXT, "pthread_create"));
                
    
    if ((pt_from_mt_pc = dlsym(RTLD_NEXT, "pthread_create_from_mach_thread"))) {
        assure(!(ret = callfunc(func_pthread_attr_init_pc, {(cpuword_t)mem_mutex})));

        pt_from_mt_pc = ptrauth_strip(pt_from_mt_pc, ptrauth_key_process_independent_code);
        pt_from_mt_pc = ((uint8_t*)pt_from_mt_pc)+4;
        pt_from_mt_pc = ptrauth_sign_unauthenticated(pt_from_mt_pc, ptrauth_key_function_pointer, 0);
        
        assure(!(ret = callfunc(pt_from_mt_pc, {(cpuword_t)_remotePthread, NULL, (cpuword_t)ptrauth_sign_unauthenticated((void*)0x71717171, ptrauth_key_function_pointer, 0), (cpuword_t)0, _PTHREAD_CREATE_FROM_MACH_THREAD | _PTHREAD_CREATE_SUSPENDED})));
        isThreadSuspended = true;
    }else{
        assure(!(ret = callfunc(func_mutex_init_pc, {(cpuword_t)mem_mutex,0})));
        lockIsInited = true;

        assure(!(ret = callfunc(func_mutex_lock_pc, {(cpuword_t)mem_mutex})));

        assure(!(ret = callfunc(func_pthread_create_pc, {(cpuword_t)_remotePthread,0,(cpuword_t)func_mutex_lock_pc,(cpuword_t)mem_mutex})));
    }

    usleep(420); //wait for new thread to spawn
    //find new thread
    assure(!task_threads(_target, &threadList, &threadCount));
    
    for (int i=0; i<threadCount; i++) {
        assure(!thread_get_state(threadList[i], MY_THREAD_STATE, (thread_state_t)&state, &count));
        

        if (isThreadSuspended) {
#if defined (__arm64__)
            if (state.__x[2] == (cpuword_t)ptrauth_sign_unauthenticated((void*)0x71717171, ptrauth_key_function_pointer, 0)) {
#elif defined (__arm__)
            if (state.__r[2] == (cpuword_t)0x71717171) {
#endif
                //found to-kidnap-thread!
                kidnapped_thread = threadList[i];
                break;
            }
        }else{
#if defined (__arm64__)
            if (state.__x[0] == (cpuword_t)mem_mutex) {
#elif defined (__arm__)
            if (state.__r[0] == (cpuword_t)mem_mutex) {
#endif
                //found to-kidnap-thread!
                kidnapped_thread = threadList[i];
                break;
            }
        }
    }
    assure(kidnapped_thread);
    
    //create exception port
    assure(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &newExceptionHandler));
    assure(!mach_port_insert_right(mach_task_self(),newExceptionHandler, newExceptionHandler, MACH_MSG_TYPE_MAKE_SEND));
    
    //set our new port
    assure(!thread_set_exception_ports(kidnapped_thread, EXC_MASK_ALL & ~(EXC_MASK_MACH_SYSCALL | EXC_MASK_SYSCALL | EXC_MASK_RPC_ALERT), newExceptionHandler, EXCEPTION_STATE_IDENTITY | MACH_EXCEPTION_CODES, MY_THREAD_STATE));
    
#if defined (__arm64__)
    state.__x[30] = 0x6161616161616161;
#elif defined (__arm__)
    state.__lr = 0x61616161; //magic end
#endif
        
    if (isThreadSuspended) {
        assure(!thread_resume(kidnapped_thread));
    }else{
        //set magic end
        assure(!thread_set_state(kidnapped_thread, MY_THREAD_STATE, (thread_state_t)&state, MY_THREAD_STATE_COUNT));

        if (pt_from_mt_pc) {
            //this new thread will finish again
            assure(!(ret = callfunc(pt_from_mt_pc, {(cpuword_t)_remoteStack, NULL, (cpuword_t)func_mutex_unlock_pc, (cpuword_t)mem_mutex})));
            
            usleep(420); //wait for new thread to spawn
        }else{
            //unlock mutex and let thread fall in magic
            assure(!(ret = callfunc(func_mutex_unlock_pc, {(cpuword_t)mem_mutex})));
        }
    }
    
    //kill marionettThread
    deinit(true);
    
    //restore globals
    _exceptionHandler = newExceptionHandler;
    newExceptionHandler = MACH_PORT_NULL;
    
    _marionetteThread = kidnapped_thread;
    kidnapped_thread = MACH_PORT_NULL;
    
    //receive exception
    assure(!mach_msg(&_emsg.head, MACH_RCV_MSG|MACH_RCV_LARGE, 0, sizeof(_emsg), _exceptionHandler, 0, MACH_PORT_NULL));
    
    _isFakeThread = false;
}

void takeover::primitiveWrite(void *remote, void *inAddr, size_t size){
    for (int i=0; i<size; i++) {
        callfunc((void*)memset,{(cpuword_t)remote+i,(cpuword_t)((uint8_t*)inAddr)[i],1});
    }
}

void takeover::primitiveRead(void *remote, void *outAddr, size_t size){
    uint8_t ab = *(uint8_t*)memcmp;
    for (int i=0; i<size; i++) {
        ((uint8_t*)outAddr)[i] = (uint8_t)callfunc((void*)memcmp,{(cpuword_t)remote+i,(cpuword_t)memcmp,1}) + ab;
    }
}



void takeover::overtakeMe(){
    if (_remoteSelf) return;
    
    mach_port_t localSenderPort = MACH_PORT_NULL;
    mach_port_t remoteListenerPort = MACH_PORT_NULL;
    cleanup([&]{
        if (localSenderPort) {
            mach_port_deallocate(mach_task_self(), localSenderPort);
            localSenderPort = MACH_PORT_NULL;
        }
        if (remoteListenerPort) {
            try {
                callfunc((void*)mach_port_deallocate,{(cpuword_t)mach_task_self_, (cpuword_t)remoteListenerPort});
            } catch (tihmstar::exception &e) {
                e.dump();
            }
            remoteListenerPort = MACH_PORT_NULL;
        }
    });
    kern_return_t ret = 0;
    mach_port_t remoteExcetionHandlerPort = MACH_PORT_NULL;
    void *curScratchSpace = NULL;
    size_t curScratchSpaceSize = 0;
    mach_port_t remoteThreadPort = MACH_PORT_NULL;
    mach_port_t *remoteListener = NULL;
    exception_mask_array_t masks = NULL;
    mach_msg_type_number_t *masksCnt = NULL;
    exception_handler_array_t old_handlers = NULL;
    exception_behavior_array_t old_behaviors = NULL;
    exception_flavor_array_t old_flavors = NULL;
    exception_raise_request msg = {};
    exception_raise_request *remoteMsg = NULL;
    
#define allocScratchSpace(size) ((void*)(curScratchSpace = (uint8_t *)curScratchSpace-size,({assure(curScratchSpaceSize>=size);}),curScratchSpaceSize-=size,(uint8_t *)curScratchSpace-size))
    
    curScratchSpaceSize = 0x1000;
    {
        //alloc mem chicken & egg problem
        //you need to have mem, to alloc mem ._.
        
        //alloc temp mem
        void *tempScratchSpace = (void*)callfunc((void*)malloc, {0x10});
        _remoteScratchSpaceSize = 0x10;
        _remoteScratchSpace = tempScratchSpace;
        
        //do real allocation
        curScratchSpace  = allocMem(curScratchSpaceSize);
        _remoteScratchSpace = curScratchSpace;
        _remoteScratchSpaceSize = curScratchSpaceSize;
        
        //free temp mem
        callfunc((void*)free, {(cpuword_t)tempScratchSpace});
    }
    allocScratchSpace(0x100);    //reserve first few bytes for possible internal stuff

    remoteListener = (mach_port_t*)allocScratchSpace(sizeof(mach_port_t));
    
    /*
     remote: create remote port with rcv right
     */
    //get remote thread port
    assure(remoteThreadPort = (mach_port_t)callfunc((void*)mach_thread_self, {}));
    
    //get remote exception handler port (which a a port we have a recv right to)
    masks = (exception_mask_array_t)allocScratchSpace(sizeof(exception_mask_array_t));
    masksCnt = (mach_msg_type_number_t *)allocScratchSpace(sizeof(mach_msg_type_number_t *));
    old_handlers = (exception_handler_array_t)allocScratchSpace(sizeof(exception_handler_array_t));
    old_behaviors = (exception_behavior_array_t)allocScratchSpace(sizeof(exception_behavior_array_t));
    old_flavors = (exception_flavor_array_t)allocScratchSpace(sizeof(exception_flavor_array_t));
    remoteMsg = (exception_raise_request*)allocScratchSpace(sizeof(exception_raise_request));
    
    //construct mach msg locally
    msg.Head.msgh_bits = MACH_MSGH_BITS_SET(MACH_MSG_TYPE_COPY_SEND, 0, 0, 0);
    msg.Head.msgh_id = 1336;
    
    msg.Head.msgh_remote_port = 0; //this one gets overwritten with exception port!
    
    msg.Head.msgh_local_port = MACH_PORT_NULL;
    msg.Head.msgh_size = sizeof(msg); //a bit larger than really neccessary, but who cares
    msg.msgh_body.msgh_descriptor_count = 1;
    
    msg.thread.name = 0; //this will be overwritten with a newly allocated port
    msg.thread.disposition = MACH_MSG_TYPE_COPY_SEND;
    msg.thread.type = MACH_MSG_PORT_DESCRIPTOR;
    
    //move over msg
    primitiveWrite(remoteMsg,&msg,sizeof(msg));
    
    //fill remote_port with remote tasks exception port
    assure(!callfunc((void*)thread_get_exception_ports,{(cpuword_t)remoteThreadPort, (cpuword_t)EXC_MASK_ALL, (cpuword_t)masks, (cpuword_t)masksCnt, (cpuword_t)&remoteMsg->Head.msgh_remote_port, (cpuword_t)old_behaviors, (cpuword_t)old_flavors}));
    
    primitiveRead(&remoteMsg->Head.msgh_remote_port, &remoteExcetionHandlerPort, sizeof(remoteExcetionHandlerPort));

    if (!remoteExcetionHandlerPort) {
        //thread exception handler not set, it's probably the task exception handler then
        //i was told mach_task_self_ is 0x103 in every process
        assure(!callfunc((void*)task_get_exception_ports,{(cpuword_t)mach_task_self_, (cpuword_t)EXC_MASK_ALL, (cpuword_t)masks, (cpuword_t)masksCnt, (cpuword_t)&remoteMsg->Head.msgh_remote_port, (cpuword_t)old_behaviors, (cpuword_t)old_flavors}));
        primitiveRead(&remoteMsg->Head.msgh_remote_port, &remoteExcetionHandlerPort, sizeof(remoteExcetionHandlerPort));
    }
    assure(remoteExcetionHandlerPort);
    
    //make sure fileds weren't overwritten by last call
    primitiveWrite(&remoteMsg->Head.msgh_local_port,&msg.Head.msgh_local_port,sizeof(msg)-(sizeof(mach_msg_bits_t)+sizeof(mach_msg_size_t)+sizeof(mach_port_t)));
    
    //i was told mach_task_self_ is 0x103 in every process
    assure(!callfunc((void*)mach_port_allocate,{(cpuword_t)mach_task_self_, (cpuword_t)MACH_PORT_RIGHT_RECEIVE, (cpuword_t)&remoteMsg->thread.name}));

    
    //send mach message!
    
    //this call will throw, because we will first receive our mach message and afterwards the exception message will be queued
    try {
        ret = (kern_return_t)callfunc((void*)mach_msg, {(cpuword_t)remoteMsg,MACH_SEND_MSG|MACH_MSG_OPTION_NONE,msg.Head.msgh_size, 0, MACH_PORT_NULL,MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL});
    } catch (TKexception &e) {
        //all good, this was expected
    }
    
    //if this is assure fails, it means we failed to send our mach message :(
    assure(_emsg.head.msgh_id == 1336);
    
    //this is the remote name of the remote port we allocated
    remoteListenerPort = ((exception_raise_request*)&_emsg)->thread.name;
    
    //fix up our call primitive by receiving the pending exception message
    assure(!mach_msg(&_emsg.head, MACH_RCV_MSG|MACH_RCV_LARGE, 0, sizeof(_emsg), _exceptionHandler, 0, MACH_PORT_NULL));

    
    /*
     remote: add send right
     */
    assure(!callfunc((void*)mach_port_insert_right,{(cpuword_t)mach_task_self_, (cpuword_t)remoteListenerPort, (cpuword_t)remoteListenerPort, (cpuword_t)MACH_MSG_TYPE_MAKE_SEND}));

    /*
     remote: send port send right here
     */
    
    //construct mach msg locally (this time we send a send right to our task)
    msg.Head.msgh_bits = MACH_MSGH_BITS_SET(MACH_MSG_TYPE_COPY_SEND, 0, 0, MACH_MSGH_BITS_COMPLEX);
    msg.Head.msgh_id = 1337;
    
    msg.Head.msgh_remote_port = remoteExcetionHandlerPort;
    
    msg.Head.msgh_local_port = MACH_PORT_NULL;
    msg.Head.msgh_size = sizeof(msg); //a bit larger than really neccessary, but who cares
    msg.msgh_body.msgh_descriptor_count = 1;
    
    msg.thread.name = remoteListenerPort; //send over a port where we can send from local and receive on remote
    msg.thread.disposition = MACH_MSG_TYPE_COPY_SEND;
    msg.thread.type = MACH_MSG_PORT_DESCRIPTOR;
    
    //move over msg
    primitiveWrite(remoteMsg,&msg,sizeof(msg));
    
    //send mach message!
    //this call will throw, because we will first receive our mach message and afterwards the exception message will be queued
    try {
        ret = (kern_return_t)callfunc((void*)mach_msg, {(cpuword_t)remoteMsg,MACH_SEND_MSG|MACH_MSG_OPTION_NONE,msg.Head.msgh_size, 0, MACH_PORT_NULL,MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL});
    } catch (TKexception &e) {
        //all good, this was expected
    }
    
    //if this is assure fails, it means we failed to send our mach message :(
    assure(_emsg.head.msgh_id == 1337);

    
    //safe the port we will send our task port to
    localSenderPort = ((exception_raise_request*)&_emsg)->thread.name;
    
    //fix up our call primitive by receiving the pending exception message
    assure(!mach_msg(&_emsg.head, MACH_RCV_MSG|MACH_RCV_LARGE, 0, sizeof(_emsg), _exceptionHandler, 0, MACH_PORT_NULL));

    /*
     //local: send over own task port
     */
    
    //construct another mach msg
    msg.Head.msgh_bits = MACH_MSGH_BITS_SET(MACH_MSG_TYPE_COPY_SEND, 0, 0, MACH_MSGH_BITS_COMPLEX);
    msg.Head.msgh_id = 1338;
    
    msg.Head.msgh_remote_port = localSenderPort; //this time we send to the remote process
    msg.Head.msgh_local_port = MACH_PORT_NULL;
    msg.Head.msgh_size = sizeof(msg); //a bit larger than really neccessary, but who cares
    msg.msgh_body.msgh_descriptor_count = 1;
    
    msg.thread.name = mach_task_self(); //let the remote process overtake me
    msg.thread.disposition = MACH_MSG_TYPE_COPY_SEND;
    msg.thread.type = MACH_MSG_PORT_DESCRIPTOR;
    
    //send over our task port
    assure(!mach_msg((mach_msg_header_t*)&msg,MACH_SEND_MSG|MACH_MSG_OPTION_NONE, sizeof(msg), 0, MACH_PORT_NULL,MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL));

    
    /*
     //remote: receive msg on port
     */
    
    //no clue what's up with mach_msg and the size field o.O
    assure(!callfunc((void*)mach_msg, {(cpuword_t)remoteMsg, MACH_RCV_MSG|MACH_RCV_LARGE, 0, sizeof(msg)+sizeof(msg.Head), remoteListenerPort, 0, MACH_PORT_NULL}));

    /*
     //remote: send back remote name for local task
     //local: store remote portname of local task locally
     */
    
    primitiveRead(&remoteMsg->thread.name, &_remoteSelf, sizeof(_remoteSelf));
        
#undef allocScratchSpace
}

void *takeover::takeover::getRemoteSym(const char *sym){
    void *ret = ptrauth_strip(dlsym(RTLD_NEXT, sym),ptrauth_key_process_independent_code);
    if (_signptr_cb) {
        ret = (void*)_signptr_cb((cpuword_t)ret);
    }else if (_isRemotePACed){
        writeMem((void*)_remoteStack, sym, strlen(sym)+1);
        return (void*)callfunc(dlsym(RTLD_NEXT, "dlsym"), {static_cast<cpuword_t>((cpuword_t)RTLD_NEXT), static_cast<cpuword_t>(_remoteStack)});
    }
    return ret;
}

void takeover::readMem(const void *remote, void *outAddr, size_t size){
    mach_vm_size_t out = size;
    if (_target) {
        assure(!mach_vm_read_overwrite(_target, (mach_vm_address_t)remote , (mach_vm_size_t)size, (mach_vm_address_t) outAddr, &out));
    }else{
        assure(_remoteSelf);
        assure(!callfunc((void*)mach_vm_write,{(cpuword_t)_remoteSelf, (cpuword_t)outAddr, (cpuword_t)remote, (cpuword_t)size}));
    }
}
void takeover::writeMem(const void *remote, const void *inAddr, size_t size){
    if (_target) {
        assure(!mach_vm_write(_target, (mach_vm_address_t)remote, (vm_offset_t)inAddr, (mach_msg_type_number_t)size));
    }else{
        assure(_remoteSelf);
        assure(_remoteScratchSpace && _remoteScratchSpaceSize > 8);
        assure(!callfunc((void*)mach_vm_read_overwrite,{(cpuword_t)_remoteSelf, (cpuword_t)inAddr, (cpuword_t)size, (cpuword_t)remote, (cpuword_t)_remoteScratchSpace}));
    }
}
void *takeover::allocMem(size_t size){
    if (_target) {
        void *ret = 0;
        assure(!mach_vm_allocate(_target, (mach_vm_address_t*)&ret, size, VM_FLAGS_ANYWHERE));
        assure(!mach_vm_protect(_target, (mach_vm_address_t)ret, size, 1, VM_PROT_READ | VM_PROT_WRITE));
        return ret;
    }else{
        //overtakeme style
        void *ret = 0;
        assure(_remoteScratchSpace && _remoteScratchSpaceSize > 8);
        
        //i was told mach_task_self_ is 0x103 in every process
        assure(!callfunc((void*)mach_vm_allocate, {(cpuword_t)mach_task_self_,(cpuword_t)_remoteScratchSpace,(cpuword_t)size,(cpuword_t)VM_FLAGS_ANYWHERE}));
        primitiveRead(_remoteScratchSpace, &ret, sizeof(ret));
        assure(!callfunc((void*)mach_vm_protect,{(cpuword_t)mach_task_self_, (cpuword_t)ret, (cpuword_t)size, (cpuword_t)1, (cpuword_t)(VM_PROT_READ | VM_PROT_WRITE)}));
        return ret;
    }
}
void takeover::deallocMem(void *remote, size_t size){
    if (_target) {
        assure(!mach_vm_deallocate(_target, (mach_vm_address_t)remote, (mach_vm_size_t)size));
    }else{
        //overtakeme style
        //i was told mach_task_self_ is 0x103 in every process
        assure(!callfunc((void*)mach_vm_deallocate,{(cpuword_t)mach_task_self_, (cpuword_t)remote, (cpuword_t)size}));
    }
}
        
std::string takeover::readString(const void *remote){
    std::string ret;
    size_t strlen = callfunc(dlsym(RTLD_NEXT, "strlen"), {(cpuword_t)remote});
    ret.resize(strlen);
    readMem(remote, (void*)ret.data(), ret.size());
    return ret;
}

//noDrop means don't drop send right if we could
std::pair<int, kern_return_t> takeover::deinit(bool noDrop){
    kern_return_t err = 0;
    std::pair<int, kern_return_t> gerr = {0,0};
    if (_remoteScratchSpace) {
        try{
            deallocMem(_remoteScratchSpace, _remoteScratchSpaceSize);
        }catch(TKexception &e){
            error("deinit: dealloc exceptio: line=%d code=%lld what=%s",__LINE__,(cpuword_t)e.code(),e.what());
        }
        _remoteScratchSpace = NULL;
        _remoteScratchSpaceSize = 0;
    }
    
    if (_marionetteThread) {
        void *func_pthread_exit = NULL;
        exception_raise_request* req = (exception_raise_request*)&_emsg;
        my_thread_state_t *state = &req->state;
        kern_return_t ret = 0;
        
        func_pthread_exit  = dlsym(RTLD_NEXT, "pthread_exit");
        
        if (func_pthread_exit && !ret && !_isFakeThread) {
#if defined (__arm64__)
            state->__x[0] = 0;
            if (_signptr_cb) {
                state->__x[32]/*PC*/ = (uint64_t)_signptr_cb((cpuword_t)func_pthread_exit);
            }else{
                arm_thread_state64_set_pc_fptr(*state,func_pthread_exit);
            }
#elif defined (__arm__)
            state->__r[0] = 0;
            state->__pc = (cpuword_t)func_pthread_exit;
#endif
            
            //clean terminate of kidnapped thread and resume
            exception_raise_state_reply reply = {};
            exception_raise_request* req = (exception_raise_request*)&_emsg;
            
            reply.Head.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(_emsg.head.msgh_bits), 0);
            reply.new_stateCnt = MY_THREAD_STATE_COUNT;
            reply.Head.msgh_remote_port = _emsg.head.msgh_remote_port;
            reply.Head.msgh_local_port = MACH_PORT_NULL;
            reply.Head.msgh_id = _emsg.head.msgh_id + 100;
            
            reply.NDR = req->NDR;
            reply.RetCode = KERN_SUCCESS;
            reply.flavor = MY_THREAD_STATE;
            memcpy(reply.new_state, state, sizeof(my_thread_state_t));
            
            reply.Head.msgh_size = (mach_msg_size_t)(sizeof(exception_raise_state_reply) - 2456) + (((4 * reply.new_stateCnt))); //straight from MIG
            
            //resume
            err = mach_msg(&reply.Head, MACH_SEND_MSG|MACH_MSG_OPTION_NONE, (mach_msg_size_t)reply.Head.msgh_size, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
            if (err) {
                error("deinit: line=%d err=%llx s=%s",__LINE__,(cpuword_t)err,mach_error_string(err));
                if (!gerr.first) {gerr.first=__LINE__; gerr.second = err;}
            }
            _marionetteThread = MACH_PORT_NULL;
        }else{
            //kill it with fire
            err = thread_terminate(_marionetteThread);
            _marionetteThread = MACH_PORT_NULL;
            if (err) {
                error("deinit: line=%d err=%llx s=%s",__LINE__,(cpuword_t)err,mach_error_string(err));
                if (!gerr.first) {gerr.first=__LINE__; gerr.second = err;}
            }
        }
    }
    
    /* DO NOT CLEAN UP _remoteStack !!!
     
     Once the thread was alive, there is no going back.
     The memory is lost forever.
     
     New threads spawned by the system will access the mapping and crash the process if
     we remove it. Thus we just leave it there.
     */
    
    
    if (_exceptionHandler) {
        err = mach_port_destroy(mach_task_self(), _exceptionHandler);
        _exceptionHandler = MACH_PORT_NULL;
        if (err) {
            error("deinit: line=%d err=%llx s=%s",__LINE__,(cpuword_t)err,mach_error_string(err));
            if (!gerr.first) {gerr.first=__LINE__; gerr.second = err;}
        }
    }
    
    if (_target && !noDrop) {
        //drop one send right
        err = mach_port_deallocate(mach_task_self(), _target);
        _target = MACH_PORT_NULL;
        if (err) {
            error("deinit: line=%d err=%llx s=%s",__LINE__,(cpuword_t)err,mach_error_string(err));
            if (!gerr.first) {gerr.first=__LINE__; gerr.second = err;}
        }
    }
    
    return gerr;
}


takeover::~takeover(){
    if (_remotePthread) {
        try {
            deallocMem(_remotePthread,sizeof(pthread_t));
        } catch (tihmstar::exception &e) {
            e.dump();
        }
        _remotePthread = NULL;
    }
    auto err = deinit();
    if(err.first){
        error("[~takeover] deinit failed on line %d with code %d",err.first,err.second);
    }
}

std::string takeover::build_commit_count(){
    return VERSION_COMMIT_COUNT;
};

std::string takeover::build_commit_sha(){
    return VERSION_COMMIT_SHA;
};

bool takeover::targetIsPACed(const mach_port_t target){
    void *(*my_objc_getClass)(const char*) = NULL;
    void *handle = NULL;
    void *someclass = NULL;
    uint64_t remoteClass = 0;
    kern_return_t err = 0;

    retassure(handle = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW), "Failed to open libojc");
    retassure(my_objc_getClass = (void*(*)(const char*))dlsym(handle, "objc_getClass"), "Failed to get _objc_getClass");
    retassure(someclass = my_objc_getClass("NSObject"), "Failed to get nsobject class");
    mach_vm_size_t out = sizeof(remoteClass);
    assure(!(err = mach_vm_read_overwrite(target, (mach_vm_address_t)someclass , (mach_vm_size_t)out, (mach_vm_address_t)&remoteClass, &out)));
    return (remoteClass >> 35);
}

#ifdef DUMP_CRASH_BACKTRACE

static const char *crashreporter_string_for_code(int code){
#define makeCode(c) case c: return #c
    switch (code){
        makeCode(EXC_BAD_ACCESS);
        makeCode(EXC_BAD_INSTRUCTION);
        makeCode(EXC_ARITHMETIC);
        makeCode(EXC_EMULATION);
        makeCode(EXC_SOFTWARE);
        makeCode(EXC_BREAKPOINT);
        makeCode(EXC_SYSCALL);
        makeCode(EXC_MACH_SYSCALL);
        makeCode(EXC_RPC_ALERT);
        makeCode(EXC_CRASH);
        makeCode(EXC_RESOURCE);
        makeCode(EXC_GUARD);
        makeCode(EXC_CORPSE_NOTIFY);
        default: return "UNKNOWN CODE";
    }
}

#if defined (__arm64__)
#define FMT_CPUWORD "%016llx"
#elif defined (__arm__)
#define FMT_CPUWORD "%08x"
#endif

        
void takeover::remote_crashreporter_dump_backtrace_line(takeover &crp, vm_address_t addr){
    Dl_info info = {};
    cpuword_t remote_into = (cpuword_t)crp._remotePthread+sizeof(pthread_t);
    crp.callfunc(dlsym(RTLD_NEXT, "dladdr"), {(cpuword_t)addr,(cpuword_t)remote_into});
    crp.readMem((void*)remote_into, &info, sizeof(info));

    const char *remote_sname = info.dli_sname;
    const char *remote_fname = info.dli_fname;
    std::string sname;
    std::string fname;

    if (!remote_sname) {
        sname = "<unexported>";
    }else{
        sname = crp.readString(remote_sname);
    }
    fname = crp.readString(remote_fname);

    printf("0x" FMT_CPUWORD ": %s \t (0x" FMT_CPUWORD " + 0x%lX) (%s(0x" FMT_CPUWORD ") + 0x%lX)\n", addr, sname.c_str(), (vm_address_t)info.dli_saddr, addr - (vm_address_t)info.dli_saddr, fname.c_str(), (vm_address_t)info.dli_fbase, addr - (vm_address_t)info.dli_fbase);
}

void takeover::remote_crashreporter_dump(takeover &crp, int code, int subcode, arm_thread_state64_t threadState, arm_exception_state64_t exceptionState, vm_address_t *bt){
    struct utsname systemInfo;
    uname(&systemInfo);

    uint64_t pc = (uint64_t)__darwin_arm_thread_state64_get_pc(threadState);

    printf("Device Model:   %s\n", systemInfo.machine);
    printf("Device Version: %s\n", systemInfo.version);
#ifdef __arm64e__
    printf("Architecture:   arm64e\n");
#else
    printf("Architecture:   arm64\n");
#endif
    printf("\n");

    printf("Exception:         %s\n", crashreporter_string_for_code(code));
    printf("Exception Subcode: %d\n", subcode);
    printf("\n");

    info("Register State:\n");
    for(int i = 0; i <= 28; i++) {
        if (i < 10) {
            printf(" ");
        }
        printf("x%d = 0x%016llX", i, threadState.__x[i]);
        if ((i+1) % (6+1) == 0) {
            printf("\n");
        }
        else {
            printf(", ");
        }
    }
    printf(" lr = 0x" FMT_CPUWORD ",  pc = 0x" FMT_CPUWORD ",  sp = 0x" FMT_CPUWORD ",  fp = 0x" FMT_CPUWORD ", cpsr=         0x%08X, far = 0x" FMT_CPUWORD "\n\n", __darwin_arm_thread_state64_get_lr(threadState), pc, __darwin_arm_thread_state64_get_sp(threadState), __darwin_arm_thread_state64_get_fp(threadState), threadState.__cpsr, exceptionState.__far);

    printf("Backtrace:\n");
    remote_crashreporter_dump_backtrace_line(crp, (vm_address_t)pc);
    int btI = 0;
    vm_address_t btAddr = bt[btI++];
    while (btAddr != 0) {
        remote_crashreporter_dump_backtrace_line(crp, btAddr);
        btAddr = bt[btI++];
    }
    printf("\n");
}

#endif
