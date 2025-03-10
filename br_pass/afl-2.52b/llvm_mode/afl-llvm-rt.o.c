/*
   american fuzzy lop - LLVM instrumentation bootstrap
   ---------------------------------------------------

   Written by Laszlo Szekeres <lszekeres@google.com> and
              Michal Zalewski <lcamtuf@google.com>

   LLVM integration design comes from Laszlo Szekeres.

   Copyright 2015, 2016 Google Inc. All rights reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at:

     http://www.apache.org/licenses/LICENSE-2.0

   This code is the rewrite of afl-as.h's main_payload.

*/

#include "../config.h"
#include "../types.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>

#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <sys/types.h>

/* This is a somewhat ugly hack for the experimental 'trace-pc-guard' mode.
   Basically, we need to make sure that the forkserver is initialized after
   the LLVM-generated runtime initialization pass, not before. */

#ifdef USE_TRACE_PC
#  define CONST_PRIO 5
#else
#  define CONST_PRIO 0
#endif /* ^USE_TRACE_PC */


/* Globals needed by the injected instrumentation. The __afl_area_initial region
   is used for instrumentation output before __afl_map_shm() has a chance to run.
   It will end up as .comm, so it shouldn't be too wasteful. */

u8  __afl_area_initial[MAP_SIZE];
u8* __afl_area_ptr = __afl_area_initial;

__thread u32 __afl_prev_loc;


/* Running in persistent mode? */

static u8 is_persistent;


/* SHM setup. */

static void __afl_map_shm(void) {

  u8 *id_str = getenv(SHM_ENV_VAR);

  /* If we're running under AFL, attach to the appropriate region, replacing the
     early-stage __afl_area_initial region that is needed to allow some really
     hacky .init code to work correctly in projects such as OpenSSL. */

  if (id_str) {

    u32 shm_id = atoi(id_str);

    __afl_area_ptr = shmat(shm_id, NULL, 0);

    /* Whooooops. */

    if (__afl_area_ptr == (void *)-1) _exit(1);

    /* Write something into the bitmap so that even with low AFL_INST_RATIO,
       our parent doesn't give up on us. */

    __afl_area_ptr[0] = 1;

  }

}


/* Fork server logic. */

static void __afl_start_forkserver(void) {

  static u8 tmp[4];
  s32 child_pid;

  u8  child_stopped = 0;

  /* Phone home and tell the parent that we're OK. If parent isn't there,
     assume we're not running in forkserver mode and just execute program. */

  if (write(FORKSRV_FD + 1, tmp, 4) != 4) return;

  while (1) {

    u32 was_killed;
    int status;

    /* Wait for parent by reading from the pipe. Abort if read fails. */

    if (read(FORKSRV_FD, &was_killed, 4) != 4) _exit(1);

    /* If we stopped the child in persistent mode, but there was a race
       condition and afl-fuzz already issued SIGKILL, write off the old
       process. */

    if (child_stopped && was_killed) {
      child_stopped = 0;
      if (waitpid(child_pid, &status, 0) < 0) _exit(1);
    }

    if (!child_stopped) {

      /* Once woken up, create a clone of our process. */

      child_pid = fork();
      if (child_pid < 0) _exit(1);

      /* In child process: close fds, resume execution. */

      if (!child_pid) {

        close(FORKSRV_FD);
        close(FORKSRV_FD + 1);
        return;
  
      }

    } else {

      /* Special handling for persistent mode: if the child is alive but
         currently stopped, simply restart it with SIGCONT. */

      kill(child_pid, SIGCONT);
      child_stopped = 0;

    }

    /* In parent process: write PID to pipe, then wait for child. */

    if (write(FORKSRV_FD + 1, &child_pid, 4) != 4) _exit(1);

    if (waitpid(child_pid, &status, is_persistent ? WUNTRACED : 0) < 0)
      _exit(1);

    /* In persistent mode, the child stops itself with SIGSTOP to indicate
       a successful run. In this case, we want to wake it up without forking
       again. */

    if (WIFSTOPPED(status)) child_stopped = 1;

    /* Relay wait status to pipe, then loop back. */

    if (write(FORKSRV_FD + 1, &status, 4) != 4) _exit(1);

  }

}


/* A simplified persistent mode handler, used as explained in README.llvm. */

int __afl_persistent_loop(unsigned int max_cnt) {

  static u8  first_pass = 1;
  static u32 cycle_cnt;

  if (first_pass) {

    /* Make sure that every iteration of __AFL_LOOP() starts with a clean slate.
       On subsequent calls, the parent will take care of that, but on the first
       iteration, it's our job to erase any trace of whatever happened
       before the loop. */

    if (is_persistent) {

      memset(__afl_area_ptr, 0, MAP_SIZE);
      __afl_area_ptr[0] = 1;
      __afl_prev_loc = 0;
    }

    cycle_cnt  = max_cnt;
    first_pass = 0;
    return 1;

  }

  if (is_persistent) {

    if (--cycle_cnt) {

      raise(SIGSTOP);

      __afl_area_ptr[0] = 1;
      __afl_prev_loc = 0;

      return 1;

    } else {

      /* When exiting __AFL_LOOP(), make sure that the subsequent code that
         follows the loop is not traced. We do that by pivoting back to the
         dummy output region. */

      __afl_area_ptr = __afl_area_initial;

    }

  }

  return 0;

}


/* This one can be called from user code when deferred forkserver mode
    is enabled. */

void __afl_manual_init(void) {

  static u8 init_done;

  if (!init_done) {

    __afl_map_shm();
    __afl_start_forkserver();
    init_done = 1;

  }

}


/* Proper initialization routine. */

__attribute__((constructor)) void __afl_auto_init(void) {

  __afl_manual_init();

}


/* The following stuff deals with supporting -fsanitize-coverage=trace-pc-guard.
   It remains non-operational in the traditional, plugin-backed LLVM mode.
   For more info about 'trace-pc-guard', see README.llvm.

   The first function (__sanitizer_cov_trace_pc_guard) is called back on every
   edge (as opposed to every basic block). */

void __sanitizer_cov_trace_pc_guard(uint32_t* guard) {
  __afl_area_ptr[*guard]++;
}


/* Init callback. Populates instrumentation IDs. Note that we're using
   ID of 0 as a special value to indicate non-instrumented bits. That may
   still touch the bitmap, but in a fairly harmless way. */

void __sanitizer_cov_trace_pc_guard_init(uint32_t* start, uint32_t* stop) {

  u32 inst_ratio = 100;
  u8* x;

  if (start == stop || *start) return;

  x = getenv("AFL_INST_RATIO");
  if (x) inst_ratio = atoi(x);

  if (!inst_ratio || inst_ratio > 100) {
    fprintf(stderr, "[-] ERROR: Invalid AFL_INST_RATIO (must be 1-100).\n");
    abort();
  }

  /* Make sure that the first element in the range is always set - we use that
     to avoid duplicate calls (which can happen as an artifact of the underlying
     implementation in LLVM). */

  *(start++) = R(MAP_SIZE - 1) + 1;

  while (start < stop) {

    if (R(100) < inst_ratio) *start = R(MAP_SIZE - 1) + 1;
    else *start = 0;

    start++;

  }

}

/*
void check_br8(int br_id, char op1, char op2, int constant_loc){
    int target_br_id = ((int *)__afl_area_ptr)[0];
    if (br_id == target_br_id)
    {
        if(constant_loc == 2)
            ((int *)__afl_area_ptr)[1] = (int)op1;
        if(constant_loc == 1)
            ((int *)__afl_area_ptr)[1] = (int)op2;
        ((int *)__afl_area_ptr)[2] = 12;
        exit(0);
    }
    else
        return;
}*/


void check_br8(int br_id, char op1, char op2, int constant_loc){
    int target_br_id = ((int *)__afl_area_ptr)[0];
    if (br_id == target_br_id)
    {
            ((int *)__afl_area_ptr)[1] = (int)op1;
            ((int *)__afl_area_ptr)[2] = (int)op2;
        ((int *)__afl_area_ptr)[3] = 12;
        exit(0);
    }
    else
        return;
}

void check_br16(int br_id, short op1, short op2, int constant_loc){
    int target_br_id = ((int *)__afl_area_ptr)[0];
    if (br_id == target_br_id)
    {
            ((int *)__afl_area_ptr)[1] = (int)op1;
            ((int *)__afl_area_ptr)[2] = (int)op2;
        ((int *)__afl_area_ptr)[3] = 12;
        exit(0);
    }
    else
        return;
}

void check_br32(int br_id, int op1, int op2, int constant_loc){
    int target_br_id = ((int *)__afl_area_ptr)[0];
    if (br_id == target_br_id)
    {
            ((int *)__afl_area_ptr)[1] = (int)op1;
            ((int *)__afl_area_ptr)[2] = (int)op2;
        ((int *)__afl_area_ptr)[3] = 12;
        exit(0);
    }
    else
        return;
}

void check_br64(int br_id, long long int op1, long long int op2, int constant_loc){
    int target_br_id = ((int *)__afl_area_ptr)[0];
    if (br_id == target_br_id)
    {
            ((int *)__afl_area_ptr)[1] = (int)op1;
            ((int *)__afl_area_ptr)[2] = (int)op2;
        ((int *)__afl_area_ptr)[3] = 12;
        exit(0);
    }
    else
        return;
}

void check_strcmp(int br_id, int type, char *op1, char* op2,int ret, int constant_loc){
    int target_br_id = ((int *)__afl_area_ptr)[0];
    if (br_id == target_br_id)
    {
            ((int *)__afl_area_ptr)[1] = (int)(op1[0]);
            ((int *)__afl_area_ptr)[2] = (int)(op2[0]);
        ((int *)__afl_area_ptr)[3] = 12;
        exit(0);
    }
    else
        return;
}

void check_strncmp(int br_id, int type, char *op1, char* op2,int len, int ret, int constant_loc){
    int target_br_id = ((int *)__afl_area_ptr)[0];
    if (br_id == target_br_id)
    {
        ((int *)__afl_area_ptr)[1] = (int)(op1[0]);
        ((int *)__afl_area_ptr)[2] = (int)(op2[0]);
        ((int *)__afl_area_ptr)[3] = 12;
        exit(0);
    }
    else
        return;
}


void log_br8(int br_id, int type, char op1, char op2, int constant_loc){
    int br_dist = op1 - op2;
    char val = ((char *)__afl_area_ptr)[br_id];
    if (val==3)
        return;
    switch(type){
        case 0:
        case 1:
            if (br_dist > 0)
            {
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 1;
                else if( val == 2)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            else if(br_dist <= 0)
            {
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 2;
                else if(val == 1)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            break;
        case 2:
        case 7:
        case 11:
            if (br_dist == 0)
            {
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 1;
                else if( val == 2)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            else if(br_dist != 0)
            {
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 2;
                else if(val == 1)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            break;
        case 3:
        case 4:
            if (br_dist >= 0)
            {
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 1;
                else if( val == 2)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            else if(br_dist < 0)
            {
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 2;
                else if(val == 1)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            break;
        case 5:
        case 6:
            if (br_dist < 0)
            {
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 1;
                else if( val == 2)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            else if(br_dist >= 0)
            {    
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 2;
                else if(val == 1)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            break;
        case 8:
        case 9:
            if (br_dist <= 0)
            {    
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 1;
                else if( val == 2)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            else if(br_dist > 0)
            {
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 2;
                else if(val == 1)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            break;
        default:
            break;
    }
    return;
}


void log_br16(int br_id, int type, short op1, short op2, int constant_loc){
    int br_dist = op1 - op2;
    char val = ((char *)__afl_area_ptr)[br_id];
    if (val==3)
        return;
    switch(type){
        case 0:
        case 1:
            if (br_dist > 0)
            {
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 1;
                else if( val == 2)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            else if(br_dist <= 0)
            {
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 2;
                else if(val == 1)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            break;
        case 2:
        case 7:
        case 11:
            if (br_dist == 0)
            {
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 1;
                else if( val == 2)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            else if(br_dist != 0)
            {
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 2;
                else if(val == 1)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            break;
        case 3:
        case 4:
            if (br_dist >= 0)
            {
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 1;
                else if( val == 2)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            else if(br_dist < 0)
            {
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 2;
                else if(val == 1)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            break;
        case 5:
        case 6:
            if (br_dist < 0)
            {
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 1;
                else if( val == 2)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            else if(br_dist >= 0)
            {    
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 2;
                else if(val == 1)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            break;
        case 8:
        case 9:
            if (br_dist <= 0)
            {    
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 1;
                else if( val == 2)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            else if(br_dist > 0)
            {
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 2;
                else if(val == 1)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            break;
        default:
            break;
    }
    return;
}

void log_br32(int br_id, int type, int op1, int op2, int constant_loc){
    int br_dist = op1 - op2;
    char val = ((char *)__afl_area_ptr)[br_id];
    if (val==3)
        return;
    switch(type){
        case 0:
        case 1:
            if (br_dist > 0)
            {
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 1;
                else if( val == 2)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            else if(br_dist <= 0)
            {
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 2;
                else if(val == 1)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            break;
        case 2:
        case 7:
        case 11:
            if (br_dist == 0)
            {
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 1;
                else if( val == 2)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            else if(br_dist != 0)
            {
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 2;
                else if(val == 1)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            break;
        case 3:
        case 4:
            if (br_dist >= 0)
            {
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 1;
                else if( val == 2)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            else if(br_dist < 0)
            {
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 2;
                else if(val == 1)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            break;
        case 5:
        case 6:
            if (br_dist < 0)
            {
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 1;
                else if( val == 2)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            else if(br_dist >= 0)
            {    
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 2;
                else if(val == 1)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            break;
        case 8:
        case 9:
            if (br_dist <= 0)
            {    
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 1;
                else if( val == 2)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            else if(br_dist > 0)
            {
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 2;
                else if(val == 1)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            break;
        default:
            break;
    }
    return;
}

void log_br64(int br_id, int type, long long op1, long long op2, int constant_loc){
    long long br_dist = op1 - op2;
    char val = ((char *)__afl_area_ptr)[br_id];
    if(val==3)
        return;
    switch(type){
        case 0:
        case 1:
            if (br_dist > 0)
            {
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 1;
                else if( val == 2)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            else if(br_dist <= 0)
            {
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 2;
                else if(val == 1)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            break;
        case 2:
        case 7:
        case 11:
            if (br_dist == 0)
            {
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 1;
                else if( val == 2)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            else if(br_dist != 0)
            {
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 2;
                else if(val == 1)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            break;
        case 3:
        case 4:
            if (br_dist >= 0)
            {
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 1;
                else if( val == 2)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            else if(br_dist < 0)
            {
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 2;
                else if(val == 1)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            break;
        case 5:
        case 6:
            if (br_dist < 0)
            {
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 1;
                else if( val == 2)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            else if(br_dist >= 0)
            {    
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 2;
                else if(val == 1)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            break;
        case 8:
        case 9:
            if (br_dist <= 0)
            {    
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 1;
                else if( val == 2)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            else if(br_dist > 0)
            {
                if(val == 0)
                    ((char *)__afl_area_ptr)[br_id] = 2;
                else if(val == 1)
                    ((char *)__afl_area_ptr)[br_id] = 3;
            }
            break;
        default:
            break;
    }
    return;
}

void log_strcmp(int br_id, int type, int ret, int constant_loc){
    int br_dist = ret;
    char val = ((char *)__afl_area_ptr)[br_id];
    if(val==3)
        return;
    if (br_dist == 0)
    {
        if(val == 0)
            ((char *)__afl_area_ptr)[br_id] = 1;
        else if( val == 2)
            ((char *)__afl_area_ptr)[br_id] = 3;
    }
    else if(br_dist != 0)
    {
        if(val == 0)
            ((char *)__afl_area_ptr)[br_id] = 2;
        else if(val == 1)
            ((char *)__afl_area_ptr)[br_id] = 3;
    }
    return;
}

void log_strncmp(int br_id, int type,int len, int ret, int constant_loc){
    int br_dist = ret;
    char val = ((char *)__afl_area_ptr)[br_id];
    // use last 2 bits to save val
    val = val & 0x3;
    // use first 6 bits to save len
    char saved_len = (val & 0xfc) >> 2;
    if(val==3)
        return;
    if (br_dist == 0)
    {
        if(val == 0)
            ((char *)__afl_area_ptr)[br_id] =  1 + (len << 2);
        else if( val == 2)
            ((char *)__afl_area_ptr)[br_id] = 3 + (len << 2);
    }
    else if(br_dist != 0)
    {
        if(val == 0)
            ((char *)__afl_area_ptr)[br_id] = 2 + (len << 2);
        else if(val == 1)
            ((char *)__afl_area_ptr)[br_id] = 3 + (len << 2);
    }
    return;
}
/*
void log_br8(int br_id, int type, char op1, char op2, int constant_loc){
    fprintf(stderr, "###$$$ branch ID %d type %d op1 %d op2 %d len %d constant_loc %d\n", br_id, type, (int)op1, (int)op2, 8, constant_loc);
}

void log_br16(int br_id, int type, short op1, short op2, int constant_loc){
    fprintf(stderr, "###$$$ branch ID %d type %d op1 %d op2 %d len %d constant_loc %d\n", br_id, type, (int)op1, (int)op2,16, constant_loc);
}

void log_br32(int br_id, int type, int op1, int op2, int constant_loc){
    fprintf(stderr, "###$$$ branch ID %d type %d op1 %d op2 %d len %d constant_loc %d\n", br_id, type, op1, op2,32, constant_loc);
}

void log_br64(int br_id, int type, long long int op1, long long int op2, int constant_loc){
    fprintf(stderr, "###$$$ branch ID %d type %d op1 %lld op2 %lld len %d constant_loc %d\n", br_id, type, op1, op2, 64, constant_loc);
}
*/

void string2hexString(char* input, char* output)
{
    int loop;
    int i; 
    
    i=0;
    loop=0;
    
    while(input[loop] != '\0')
    {
        sprintf((char*)(output+i),"%02X", input[loop]);
        loop+=1;
        i+=2;
    }
    //insert NULL at the end of the output string
    output[i++] = '\0';
}

void string2hexStringn(char* input, char* output, int len)
{
    int loop;
    int i; 
    i=0;
    loop=0;
    
    while((input[loop] != '\0')&& loop<len)
    {
        sprintf((char*)(output+i),"%02X", input[loop]);
        loop+=1;
        i+=2;
    }
    //insert NULL at the end of the output string
    output[i++] = '\0';
}
/*
void log_strcmp(int br_id, int type, char *op1, char* op2,int ret, int constant_loc){
    char tmp_op1[256];
    char tmp_op2[256];
    string2hexString(op1, tmp_op1);
    string2hexString(op2, tmp_op2);
    fprintf(stderr, "###$$$ branch ID %d type %d op1 %s op2 %s ret %d constant_loc %d\n", br_id, type, tmp_op1, tmp_op2, ret, constant_loc);
}

void log_strncmp(int br_id, int type, char *op1, char* op2, int len, int ret, int constant_loc){
    char tmp_op1[256];
    char tmp_op2[256];
    string2hexStringn(op1, tmp_op1, len);
    string2hexStringn(op2, tmp_op2, len);
    fprintf(stderr, "###$$$ branch ID %d type %d op1 %s op2 %s len %d ret %d constant_loc %d\n", br_id, type,tmp_op1,tmp_op2, len, ret, constant_loc);
}
*/
