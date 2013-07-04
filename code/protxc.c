/* protxc.c: PROTECTION EXCPETION HANDLER FOR OS X MACH
 *
 * $Id$
 * Copyright (c) 2013 Ravenbrook Limited.  See end of file for license.
 *
 * This is the protection exception handling code for Mac OS X using the
 * Mach interface (not pthreads).
 *
 * In Mach, a thread that hits protected memory is suspended, and a message
 * is sent to a separate handler thread.
 * The handler thread can fix things up and continue the suspended thread by
 * sending back a "success" reply.  It can forward the message to another
 * handler of the same kind, or it can forward the message to another handler
 * at the next level out (the levels are thread, task, host) by sending a
 * "fail" reply.
 *
 * In Mac OS X, pthreads are implemented by Mach threads.  (The implementation
 * is part of the XNU source code at opensource.apple.com.  [copy to import?])
 * So we can use some pthread interfaces for convenience in setting up threads.
 *
 * This module sets up an exception handling thread for the EXC_BAD_ACCESS
 * exceptions that will be caused by the MPS shield (read/write barriers).
 * That thread calls the MPS to resolve the condition and allow the mutator
 * thread to progress.
 *
 * That part is fairly simple.  Most of the code in this module is concerned
 * with decoding Mach messages and re-encoding them in order to forward them
 * on to other exception handlers.
 *
 *
 * SOURCES
 * .source.man: <http://felinemenace.org/~nemo/mach/manpages/>
 *              <http://web.mit.edu/darwin/src/modules/xnu/osfmk/man/>
 *
 *
 * REFERENCES
 * [Fuller_2013] "Mach Exception Handlers"; Landon Fuller;
 *               <http://www.mikeash.com/pyblog/friday-qa-2013-01-11-mach-exception-handlers.html>
 *
 *
 * TRANSGRESSIONS
 *
 * .trans.stdlib: It's OK to use the C library from here because we know
 * we're on OS X and not freestanding.  In particular, we use memcpy.
 */

#include "mpm.h"
#include "prmcxc.h"

#include <stdlib.h> /* see .trans.stdlib */

#include <pthread.h>

#include <mach/mach_port.h>
#include <mach/mach_init.h>
#include <mach/task.h>
#include <mach/thread_act.h>
#include <mach/thread_status.h>
#include <mach/mach_error.h>
#include <mach/i386/thread_status.h>
#include <mach/exc.h>

#if !defined(MPS_OS_XC)
#error "protxc.c is OS X specific"
#endif
#ifndef PROTECTION
#error "protxc.c implements protection, but PROTECTION is not set"
#endif

SRCID(protxc, "$Id$");


/* The Mach headers in /usr/include/mach and the code generated by the Mach
   Interface Generator (mig) program are not the truth.  In particular, if
   you specify your exception behaviour with the MACH_EXCEPTION_CODES flag
   then you get a different layout with 64-bit wide exception code and
   subcode fields.  We want those (so we can get the faulting address on
   x86_64) but also we must cope with passing them onwards to other
   exception handlers in the debugger or whatever.  So we're forced to
   define our own copies of these structures.  Makes a bit of a nonsense of
   having them in /usr/include or mig at all, really.  We definitely don't
   want to complicate the MPS build process with the mig. */

#define REQUEST_RAISE_STRUCT(name, code_type)  \
  struct name { \
    mach_msg_header_t Head; \
    mach_msg_body_t msgh_body; \
    mach_msg_port_descriptor_t thread; \
    mach_msg_port_descriptor_t task; \
    NDR_record_t NDR; \
    exception_type_t exception; \
    mach_msg_type_number_t codeCnt; \
    code_type code[2]; \
  }

#define REQUEST_RAISE_STATE_STRUCT(name, code_type) \
  struct name { \
    mach_msg_header_t Head; \
    NDR_record_t NDR; \
    exception_type_t exception; \
    mach_msg_type_number_t codeCnt; \
    code_type code[2]; \
    int flavor; \
    mach_msg_type_number_t old_stateCnt; \
    natural_t old_state[224]; \
  }

#define REQUEST_RAISE_STATE_IDENTITY_STRUCT(name, code_type) \
  struct name { \
    mach_msg_header_t Head; \
    mach_msg_body_t msgh_body; \
    mach_msg_port_descriptor_t thread; \
    mach_msg_port_descriptor_t task; \
    NDR_record_t NDR; \
    exception_type_t exception; \
    mach_msg_type_number_t codeCnt; \
    code_type code[2]; \
    int flavor; \
    mach_msg_type_number_t old_stateCnt; \
    natural_t old_state[224]; \
  }

/* Structure packing really makes a difference because the code field is
   not naturally aligned on 64-bit. */

#ifdef  __MigPackStructs
#pragma pack(4)
#endif

typedef REQUEST_RAISE_STRUCT(request_32_s, __int32_t) request_32_s;
typedef REQUEST_RAISE_STRUCT(request_64_s, __int64_t) request_64_s;
typedef REQUEST_RAISE_STATE_STRUCT(request_s32_s, __int32_t) request_s32_s;
typedef REQUEST_RAISE_STATE_STRUCT(request_s64_s, __int64_t) request_s64_s;
typedef REQUEST_RAISE_STATE_IDENTITY_STRUCT(request_si32_s, __int32_t) request_si32_s;
typedef REQUEST_RAISE_STATE_IDENTITY_STRUCT(request_si64_s, __int64_t) request_si64_s;

typedef __Reply__exception_raise_state_identity_t reply_si_s;

#ifdef  __MigPackStructs
#pragma pack()
#endif


/* These are the message IDs that appear in request messages, determined
   by experimentation.  They can also be extracted by running mig on
   /usr/include/mach/exc.defs and /usr/include/mach/mach_exc.defs.
   The replies to these messages are these + 100. */

#define MSG_ID_REQUEST_32 2401
#define MSG_ID_REQUEST_STATE_32 2402
#define MSG_ID_REQUEST_STATE_IDENTITY_32 2403

#define MSG_ID_REQUEST_64 2405
#define MSG_ID_REQUEST_STATE_64 2406
#define MSG_ID_REQUEST_STATE_IDENTITY_64 2407


/* Convert request messages between 32- and 64-bit code code layouts.
   May truncate 64-bit codes, of course, but that appears to be what
   Mac OS X does. */

#define COPY_COMMON(dst, src, id, code_type) \
  BEGIN \
    (dst)->Head = (src)->Head; \
    (dst)->Head.msgh_id = (id); \
    (dst)->Head.msgh_size = sizeof(*dst); \
    (dst)->NDR = (src)->NDR; \
    (dst)->exception = (src)->exception; \
    (dst)->codeCnt = (src)->codeCnt; \
    (dst)->code[0] = (code_type)(src)->code[0]; \
    (dst)->code[1] = (code_type)(src)->code[1]; \
  END

#define COPY_IDENTITY(dst, src) \
  BEGIN \
    (dst)->thread = (src)->thread; \
    (dst)->task = (src)->task; \
  END

#define COPY_STATE(dst, src, state_flavor, state, state_count) \
  BEGIN \
    mach_msg_size_t _s; \
    (dst)->flavor = (state_flavor); \
    (dst)->old_stateCnt = (state_count); \
    _s = (dst)->old_stateCnt * sizeof(natural_t); \
    AVER(_s <= sizeof((dst)->old_state)); \
    memcpy((dst)->old_state, (state), _s); \
    (dst)->Head.msgh_size = \
      offsetof(request_s32_s, old_state) + _s; \
  END

#define COPY_REQUEST(dst, src, width) \
  BEGIN \
    COPY_COMMON(dst, src, MSG_ID_REQUEST_##width, __int##width##_t); \
    COPY_IDENTITY(dst, src); \
  END;

#define COPY_REQUEST_STATE(dst, src, width, state_flavor, state, state_count) \
  BEGIN \
    COPY_COMMON(dst, src, MSG_ID_REQUEST_STATE_##width, __int##width##_t); \
    COPY_STATE(dst, src, state_flavor, state, state_count); \
  END

#define COPY_REQUEST_STATE_IDENTITY(dst, src, width, state_flavor, state, state_count) \
  BEGIN \
    COPY_COMMON(dst, src, MSG_ID_REQUEST_STATE_IDENTITY_##width, __int##width##_t); \
    COPY_IDENTITY(dst, src); \
    COPY_STATE(dst, src, state_flavor, state, state_count); \
  END


/* Guard used to ensure we only set up the exception handler once. */
static pthread_once_t prot_setup_once = PTHREAD_ONCE_INIT;

/* The exception handling thread. */
static pthread_t exc_thread;

/* This will be the port that will receive messages for our
   exception handler. */
static mach_port_name_t exc_port;

/* This is a record of the previously-installed exception handler.  The
   behaviour says which format of messages it wants to receive, and the
   flavour says which kind of thread state should be attached to those
   messages.
   
   When we forward exceptions that we don't want to handle, we have to cope
   with translating our message for the other handler.  That's a real pain.
   
   Note that the other handler might well be an attached debugger or the
   BSD subsystem of Mac OS X.  The latter does the work of translating
   Mach exceptions into Unix signals.
   
   TODO: Determine what happens if you attach a debugger to a process that
   has already set up the exception handler. */

#if 0
static exception_handler_t old_port;
static exception_behavior_t old_behaviour;
static thread_state_flavor_t old_flavor;
#endif


/* This corresponds to the portable part of the MPS that would resolve
   the access, doing some scanning, lowering the shield, or single-stepping
   the mutator in the case of a weak access. */

/* TODO: This probably ought to be in a separate worker thread to the
   exception message receiver, so that if there's ever a bug in the MPS
   that causes an exception there's no deadlock.  We could also spread
   scanning work across cores. */

static Bool catch_bad_access(void *address, THREAD_STATE_T *state)
{
  MutatorFaultContextStruct mfcStruct;
  Bool handled;

  /* fprintf(stderr, "catch_bad_access(address = %p)\n", address); */

  mfcStruct.address = (Addr)address;
  AVER(sizeof(mfcStruct.thread_state) == sizeof(THREAD_STATE_T));
  memcpy(&mfcStruct.thread_state, state, sizeof(mfcStruct.thread_state));
  
  handled = ArenaAccess(mfcStruct.address,
                        AccessREAD | AccessWRITE,
                        &mfcStruct);
  
  return handled;
}


/* Build a reply message based on a request. */

static void build_reply(reply_si_s *reply,
                        request_si64_s *request,
                        kern_return_t ret_code)
{
  mach_msg_size_t state_size;
  reply->Head.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(request->Head.msgh_bits), 0);
  reply->Head.msgh_remote_port = request->Head.msgh_remote_port;
  reply->Head.msgh_local_port = MACH_PORT_NULL;
  reply->Head.msgh_reserved = 0;
  reply->Head.msgh_id = request->Head.msgh_id + 100;
  reply->NDR = request->NDR;
  reply->RetCode = ret_code;
  reply->flavor = request->flavor;
  reply->new_stateCnt = request->old_stateCnt;
  state_size = reply->new_stateCnt * sizeof(natural_t);
  AVER(sizeof(reply->new_state) >= state_size);
  memcpy(reply->new_state, request->old_state, state_size);
  /* If you use sizeof(reply) for reply->Head.msgh_size then the state
     gets ignored. */
  reply->Head.msgh_size = offsetof(reply_si_s, new_state) + state_size;
}


static void must_send(mach_msg_header_t *head)
{
  kern_return_t kr;
  kr = mach_msg(head,
          MACH_SEND_MSG,
          head->msgh_size,
          /* recv_size */ 0,
          MACH_PORT_NULL,
          MACH_MSG_TIMEOUT_NONE,
          MACH_PORT_NULL);
  if (kr != KERN_SUCCESS) {
    mach_error("mach_msg send", kr);
    exit(1);
  }
}


/* Handle (or forward) one EXC_BAD_ACCESS exception message. */
/* Derived from reverse-engineering and running "mig /usr/include/exc.defs" */

/* Mac OS X provides a function exc_server (in
   /usr/lib/system/libsystem_kernel.dylib) that's documented in the XNU
   sources and generated by the Mach Interface Generator (mig).  It unpacks
   an exception message structure and calls one of several handler functions.
   We can't use it because:
 
     1. It's hard-wired to call certain functions by name.  The MPS can't
        steal those names in case the client program is using them too.
  
     2. It fails anyway in Xcode's default "Release" build with hidden
        symbols, because it uses dlsym to find those handler functins, and
        dlsym can't find them.

    So instead this function duplicates the work of exc_server, and is shorter
    because it's specialised for protection exceptions of a single behaviour
    and flavour.  It is also more flexible and can dispatch to any function
    we want.  The downside is that it depends on various unpublished stuff
    like the code numbers for certain messages. */

static void handle_one(void)
{
  request_si64_s request;
  mach_msg_return_t mr;
#if 0
  union {
    mach_msg_header_t Head;
    request_32_s r32;
    request_64_s r64;
    request_s32_s rs32;
    request_s64_s rs64;
    request_si32_s rsi32;
    request_si64_s rsi64;
  } forward;
  kern_return_t kr;
  thread_state_data_t thread_state;
  mach_msg_type_number_t thread_state_count;
#endif
  reply_si_s reply;

  /* TODO: This timeout loop probably isn't necessary, but it might help
     with killing or interrupting the process. */
  do {
    mr = mach_msg(&request.Head,
                  MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                  /* send_size */ 0,
                  /* receive_size */ sizeof(request),
                  exc_port,
                  /* timeout */ 10000,
                  /* notify */ MACH_PORT_NULL);
  } while (mr == MACH_RCV_TIMED_OUT);
  if (mr != MACH_MSG_SUCCESS) {
    mach_error("mach_msg recv\n", mr);
    exit(1);
  }

  AVER(request.Head.msgh_id == MSG_ID_REQUEST_STATE_IDENTITY_64);
  AVER(request.Head.msgh_local_port == exc_port);
  AVER(request.task.name == mach_task_self());
  AVER(request.exception == EXC_BAD_ACCESS);
  AVER(request.codeCnt == 2);
  AVER(request.old_stateCnt == THREAD_STATE_COUNT);
  AVER(request.flavor == THREAD_STATE_FLAVOR);
  
  if (request.code[0] == KERN_PROTECTION_FAILURE)
    if (catch_bad_access((void *)request.code[1],
                         (THREAD_STATE_T *)request.old_state)) {
      /* Send a reply that will cause the thread to continue. */
      build_reply(&reply, &request, KERN_SUCCESS);
      must_send(&reply.Head);
      return;
    }

  /* We didn't handle the exception -- it wasn't one of ours. */
  
  /* If there was no previously installed exception port, send a reply
     that will cause the system to pass the message on to the next kind
     of handler -- presumably the host handler.  The BSD subsystem of OS X
     has a host handler that turns exception into Unix signals [ref?] */

  fprintf(stderr,
          "unhandled message from port %u, thread %u, exception %u, code %llu\n",
          request.Head.msgh_remote_port,
          request.thread.name,
          request.exception,
          request.code[0]);

/*  if (old_port == MACH_PORT_NULL) { */
    build_reply(&reply, &request, KERN_FAILURE);
    must_send(&reply.Head);
    return;
/*  } */
  
#if 0
  /* Forward the exception message to the previously installed exception
     port.  This is a common situation when running under a debugger, when
     the port will be the debugger's handler for this process' exceptions.
     Unfortunately, that port might've been expecting a different behavior
     and flavour, so we have to cope.  Note that we leave the reply port
     intact, so we don't have to handle and forward replies. */
  
  switch (old_behaviour) {
  case EXCEPTION_DEFAULT:
    COPY_REQUEST(&forward.r32, &request, 32);
    break;

  case EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES:
    COPY_REQUEST(&forward.r64, &request, 64);
    break;

  default:
    /* Get the flavour of thread state that the old handler expects. */
    thread_state_count = THREAD_STATE_MAX;
    kr = thread_get_state(request.thread.name,
                          old_flavor,
                          thread_state,
                          &thread_state_count);
    if (kr != KERN_SUCCESS) {
      mach_error("thread_get_state", kr);
      exit(1);
    }
    
    switch (old_behaviour) {
    case EXCEPTION_STATE:
      COPY_REQUEST_STATE(&forward.rs32,
                         &request,
                         32,
                         old_flavor,
                         thread_state,
                         thread_state_count);
      break;

    case EXCEPTION_STATE | MACH_EXCEPTION_CODES:
      COPY_REQUEST_STATE(&forward.rs64,
                         &request,
                         64,
                         old_flavor,
                         thread_state,
                         thread_state_count);
      break;

    case EXCEPTION_STATE_IDENTITY:
      COPY_REQUEST_STATE_IDENTITY(&forward.rsi32,
                                  &request,
                                  32,
                                  old_flavor,
                                  thread_state,
                                  thread_state_count);
      break;

    case EXCEPTION_STATE_IDENTITY | MACH_EXCEPTION_CODES:
      COPY_REQUEST_STATE_IDENTITY(&forward.rsi64,
                                  &request,
                                  64,
                                  old_flavor,
                                  thread_state,
                                  thread_state_count);
      break;

    default:
      fprintf(stderr, "unknown old behaviour %x\n", old_behaviour);
      NOTREACHED;
      abort();
    }
    break;
  }
  
  fprintf(stderr, "forwarding to %u\n", old_port);

  /* Forward the message to the old port. */
  forward.Head.msgh_local_port = old_port;
  mr = mach_msg(&forward.Head,
                MACH_SEND_MSG,
                forward.Head.msgh_size,
                /* rcv_size */ 0,
                /* rcv_name */ MACH_PORT_NULL,
                MACH_MSG_TIMEOUT_NONE,
                /* notify */ MACH_PORT_NULL);
  if (mr != MACH_MSG_SUCCESS) {
    mach_error("mach_msg forward", mr);
    /* Fall back to sending a reply.  This might bypass a
       handler but may avoid exiting the process entirely. */
    build_reply(&reply, &request, KERN_FAILURE);
    must_send(&reply.Head);
  }
#endif
}


/* The exception handler thread loop. */

static void *handler(void *p) {
  UNUSED(p);
  for (;;)
    handle_one();
  return NULL;
}


extern void protThreadRegister(Bool setup);

extern void protThreadRegister(Bool setup)
{
  kern_return_t kr;
  mach_msg_type_number_t old_cnt;
  exception_mask_t old_mask;
  exception_behavior_t behaviour;
  mach_port_t old_port;
  exception_behavior_t old_behaviour;
  thread_state_flavor_t old_flavor;
  mach_port_t self;
  static mach_port_t setup_thread = MACH_PORT_NULL;

  self = mach_thread_self();
  
  /* Avoid setting up the exception handler for the setup thread twice,
     in the case where the mutator registers that thread twice. */
  if (setup) {
    AVER(setup_thread == MACH_PORT_NULL);
    setup_thread = self;
  } else {
    AVER(setup_thread != MACH_PORT_NULL);
    if (self == setup_thread)
      return;
  }
  
  /* Ask to receive EXC_BAD_ACCESS exceptions on our new port, complete
     with thread state and identity information in the message.
     The MACH_EXCEPTION_CODES flag causes the code fields to be
     passed 64-bits wide, matching request_si64_s. */
  behaviour = (exception_behavior_t)(EXCEPTION_STATE_IDENTITY | MACH_EXCEPTION_CODES);
  kr = thread_swap_exception_ports(self,
                                   EXC_MASK_BAD_ACCESS,
                                   exc_port,
                                   behaviour,
                                   THREAD_STATE_FLAVOR,
                                   &old_mask,
                                   &old_cnt,
                                   &old_port,
                                   &old_behaviour,
                                   &old_flavor);
  if (kr != KERN_SUCCESS) {
    mach_error("thread_swap_exception_ports", kr);
    exit(1);
  }
  AVER(old_mask == EXC_MASK_BAD_ACCESS);
  AVER(old_cnt == 1);
  
  fprintf(stderr,
          "thread = %u, old_port = %u, old_behaviour = %x, old_flavour = %d\n",
          self, old_port, old_behaviour, old_flavor);

  AVER(old_port == MACH_PORT_NULL);
}


static void protSetup(void)
{
  kern_return_t kr;
#if 0
  mach_msg_type_number_t old_cnt;
  exception_mask_t old_mask;
  exception_behavior_t behaviour;
#endif
  int pr;

  /* Create a port right to send and receive exceptions */
  kr = mach_port_allocate(mach_task_self(),
                          MACH_PORT_RIGHT_RECEIVE,
                          &exc_port);
  if (kr != KERN_SUCCESS) {
    mach_error("mach_port_allocate", kr);
    exit(1);
  }
  
  /* Allow me to send exceptions on this port right. */
  /* FIXME: Why? */
  kr = mach_port_insert_right(mach_task_self(),
                              exc_port, exc_port,
                              MACH_MSG_TYPE_MAKE_SEND);
  if (kr != KERN_SUCCESS) {
    mach_error("mach_port_insert_right", kr);
    exit(1);
  }
  
#if 0
  /* Ask to receive EXC_BAD_ACCESS exceptions on our new port, complete
     with thread state and identity information in the message.
     The MACH_EXCEPTION_CODES flag causes the code fields to be
     passed 64-bits wide, matching request_si64_s. */
  behaviour = (exception_behavior_t)(EXCEPTION_STATE_IDENTITY | MACH_EXCEPTION_CODES);
  kr = task_swap_exception_ports(mach_task_self(),
                                 EXC_MASK_BAD_ACCESS,
                                 exc_port,
                                 behaviour,
                                 THREAD_STATE_FLAVOR,
                                 &old_mask,
                                 &old_cnt,
                                 &old_port,
                                 &old_behaviour,
                                 &old_flavor);
  if (kr != KERN_SUCCESS) {
    mach_error("task_swap_exception_ports", kr);
    exit(1);
  }
  AVER(old_mask == EXC_MASK_BAD_ACCESS);
  AVER(old_cnt == 1);

  fprintf(stderr,
          "old_port = %u, old_behaviour = %x, old_flavour = %d\n",
          old_port, old_behaviour, old_flavor);

#endif

  protThreadRegister(TRUE);

  /* Launch the exception handling thread. */
  pr = pthread_create(&exc_thread, NULL, handler, NULL);
  if (pr != 0) {
    fprintf(stderr, "pthread_create: %d\n", pr);
    exit(1);
  }
}

void ProtSetup(void)
{
  pthread_once(&prot_setup_once, protSetup);
}


#if 0

#include <sys/mman.h>
#include <unistd.h>

int main(int argc, const char * argv[])
{
  ProtSetup();
  
  protected_block = mmap(0, getpagesize(), PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
  if (protected_block == MAP_FAILED) {
    perror("mmap");
    exit(1);
  }
  
  *protected_block = 'x';
  
  puts("Hello, world!");

  *(volatile char *)0xDEADBEEFC0DE = 'x';
  
  puts("Goodbye, moon!");

  return 0;
}

#endif


/* C. COPYRIGHT AND LICENSE
 *
 * Copyright (C) 2013 Ravenbrook Limited <http://www.ravenbrook.com/>.
 * All rights reserved.  This is an open source license.  Contact
 * Ravenbrook for commercial licensing options.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 
 * 3. Redistributions in any form must be accompanied by information on how
 * to obtain complete source code for this software and any accompanying
 * software that uses this software.  The source code must either be
 * included in the distribution or be available for no more than the cost
 * of distribution plus a nominal fee, and must be freely redistributable
 * under reasonable conditions.  For an executable file, complete source
 * code means the source code for all modules it contains. It does not
 * include source code for modules or files that typically accompany the
 * major components of the operating system on which the executable file
 * runs.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 * PURPOSE, OR NON-INFRINGEMENT, ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS AND CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */