/****************************************************************************
 * binfmt/binfmt_execmodule.c
 *
 *   Copyright (C) 2009, 2013-2014 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdint.h>
#include <stdlib.h>
#include <sched.h>
#include <debug.h>
#include <errno.h>

#include <nuttx/arch.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mm.h>
#include <nuttx/binfmt/binfmt.h>

#include "sched/sched.h"
#include "binfmt_internal.h"

#ifndef CONFIG_BINFMT_DISABLE

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/
/* If C++ constructors are used, then CONFIG_SCHED_STARTHOOK must also be
 * selected be the start hook is used to schedule execution of the
 * constructors.
 */

#if defined(CONFIG_BINFMT_CONSTRUCTORS) && !defined(CONFIG_SCHED_STARTHOOK)
#  errror "CONFIG_SCHED_STARTHOOK must be defined to use constructors"
#endif

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Private Data
 ****************************************************************************/

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: exec_ctors
 *
 * Description:
 *   Execute C++ static constructors.  This function is registered as a
 *   start hook and runs on the thread of the newly created task before
 *   the new task's main function is called.
 *
 * Input Parameters:
 *   loadinfo - Load state information
 *
 * Returned Value:
 *   0 (OK) is returned on success and a negated errno is returned on
 *   failure.
 *
 ****************************************************************************/

#ifdef CONFIG_BINFMT_CONSTRUCTORS
static void exec_ctors(FAR void *arg)
{
  FAR const struct binary_s *binp = (FAR const struct binary_s *)arg;
  binfmt_ctor_t *ctor = binp->ctors;
  int i;

  /* Execute each constructor */

  for (i = 0; i < binp->nctors; i++)
    {
      bvdbg("Calling ctor %d at %p\n", i, (FAR void *)ctor);

      (*ctor)();
      ctor++;
    }
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: exec_module
 *
 * Description:
 *   Execute a module that has been loaded into memory by load_module().
 *
 * Returned Value:
 *   This is an end-user function, so it follows the normal convention:
 *   Returns the PID of the exec'ed module.  On failure, it returns
 *   -1 (ERROR) and sets errno appropriately.
 *
 ****************************************************************************/

int exec_module(FAR const struct binary_s *binp)
{
  FAR struct task_tcb_s *tcb;
#ifdef CONFIG_ARCH_ADDRENV
  save_addrenv_t oldenv;
#endif
  FAR uint32_t *stack;
  pid_t pid;
  int err;
  int ret;

  /* Sanity checking */

#ifdef CONFIG_DEBUG
  if (!binp || !binp->entrypt || binp->stacksize <= 0)
    {
      err = EINVAL;
      goto errout;
    }
#endif

  bvdbg("Executing %s\n", binp->filename);

  /* Allocate a TCB for the new task. */

  tcb = (FAR struct task_tcb_s*)kmm_zalloc(sizeof(struct task_tcb_s));
  if (!tcb)
    {
      err = ENOMEM;
      goto errout;
    }

#ifdef CONFIG_ARCH_ADDRENV
  /* Instantiate the address environment containing the user heap */

  ret = up_addrenv_select(&binp->addrenv, &oldenv);
  if (ret < 0)
    {
      bdbg("ERROR: up_addrenv_select() failed: %d\n", ret);
      err = -ret;
      goto errout_with_tcb;
    }

  /* Initialize the user heap */

  umm_initialize((FAR void *)CONFIG_ARCH_HEAP_VBASE,
                 up_addrenv_heapsize(&binp->addrenv));
#endif

  /* Allocate the stack for the new task.
   *
   * REVISIT:  This allocation is currently always from the user heap.  That
   * will need to change if/when we want to support dynamic stack allocation.
   */

  stack = (FAR uint32_t*)kumm_malloc(binp->stacksize);
  if (!stack)
    {
      err = ENOMEM;
      goto errout_with_addrenv;
    }

  /* Initialize the task */

  ret = task_init((FAR struct tcb_s *)tcb, binp->filename, binp->priority,
                  stack, binp->stacksize, binp->entrypt, binp->argv);
  if (ret < 0)
    {
      err = get_errno();
      bdbg("task_init() failed: %d\n", err);
      goto errout_with_stack;
    }

  /* Note that tcb->flags are not modified.  0=normal task */
  /* tcb->flags |= TCB_FLAG_TTYPE_TASK; */

#ifdef CONFIG_PIC
  /* Add the D-Space address as the PIC base address.  By convention, this
   * must be the first allocated address space.
   */

  tcb->cmn.dspace = binp->alloc[0];

  /* Re-initialize the task's initial state to account for the new PIC base */

  up_initial_state(&tcb->cmn);
#endif

#ifdef CONFIG_ARCH_ADDRENV
  /* Assign the address environment to the new task group */

  ret = up_addrenv_clone(&binp->addrenv, &tcb->cmn.group->addrenv);
  if (ret < 0)
    {
      err = -ret;
      bdbg("ERROR: up_addrenv_clone() failed: %d\n", ret);
      goto errout_with_stack;
    }

  /* Mark that this group has an address environment */

  tcb->cmn.group->tg_flags |= GROUP_FLAG_ADDRENV;
#endif

#ifdef CONFIG_BINFMT_CONSTRUCTORS
  /* Setup a start hook that will execute all of the C++ static constructors
   * on the newly created thread.  The struct binary_s must persist at least
   * until the new task has been started.
   */

  task_starthook(tcb, exec_ctors, (FAR void *)binp);
#endif

  /* Get the assigned pid before we start the task */

  pid = tcb->cmn.pid;

  /* Then activate the task at the provided priority */

  ret = task_activate((FAR struct tcb_s *)tcb);
  if (ret < 0)
    {
      err = get_errno();
      bdbg("task_activate() failed: %d\n", err);
      goto errout_with_stack;
    }

#ifdef CONFIG_ARCH_ADDRENV
  /* Restore the address environment of the caller */

  ret = up_addrenv_restore(&oldenv);
  if (ret < 0)
    {
      bdbg("ERROR: up_addrenv_select() failed: %d\n", ret);
      err = -ret;
      goto errout_with_stack;
    }
#endif

  return (int)pid;

errout_with_stack:
  tcb->cmn.stack_alloc_ptr = NULL;
  sched_releasetcb(&tcb->cmn, TCB_FLAG_TTYPE_TASK);
  kumm_free(stack);
  goto errout;

errout_with_addrenv:
#ifdef CONFIG_ARCH_ADDRENV
  (void)up_addrenv_restore(&oldenv);
#endif
errout_with_tcb:
  kmm_free(tcb);
errout:
  set_errno(err);
  bdbg("returning errno: %d\n", err);
  return ERROR;
}

#endif /* CONFIG_BINFMT_DISABLE */
