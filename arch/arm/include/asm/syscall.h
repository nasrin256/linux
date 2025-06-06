/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Access to user system call parameters and results
 *
 * See asm-generic/syscall.h for descriptions of what we must do here.
 */

#ifndef _ASM_ARM_SYSCALL_H
#define _ASM_ARM_SYSCALL_H

#include <uapi/linux/audit.h> /* for AUDIT_ARCH_* */
#include <linux/elf.h> /* for ELF_EM */
#include <linux/err.h>
#include <linux/sched.h>

#include <asm/unistd.h>

#define NR_syscalls (__NR_syscalls)

extern const unsigned long sys_call_table[];

static inline int syscall_get_nr(struct task_struct *task,
				 struct pt_regs *regs)
{
	if (IS_ENABLED(CONFIG_AEABI) && !IS_ENABLED(CONFIG_OABI_COMPAT))
		return task_thread_info(task)->abi_syscall;

	if (task_thread_info(task)->abi_syscall == -1)
		return -1;

	return task_thread_info(task)->abi_syscall & __NR_SYSCALL_MASK;
}

static inline bool __in_oabi_syscall(struct task_struct *task)
{
	return IS_ENABLED(CONFIG_OABI_COMPAT) &&
		(task_thread_info(task)->abi_syscall & __NR_OABI_SYSCALL_BASE);
}

static inline bool in_oabi_syscall(void)
{
	return __in_oabi_syscall(current);
}

static inline void syscall_rollback(struct task_struct *task,
				    struct pt_regs *regs)
{
	regs->ARM_r0 = regs->ARM_ORIG_r0;
}

static inline long syscall_get_error(struct task_struct *task,
				     struct pt_regs *regs)
{
	unsigned long error = regs->ARM_r0;
	return IS_ERR_VALUE(error) ? error : 0;
}

static inline long syscall_get_return_value(struct task_struct *task,
					    struct pt_regs *regs)
{
	return regs->ARM_r0;
}

static inline void syscall_set_return_value(struct task_struct *task,
					    struct pt_regs *regs,
					    int error, long val)
{
	regs->ARM_r0 = (long) error ? error : val;
}

static inline void syscall_set_nr(struct task_struct *task,
				  struct pt_regs *regs,
				  int nr)
{
	if (nr == -1) {
		task_thread_info(task)->abi_syscall = -1;
		/*
		 * When the syscall number is set to -1, the syscall will be
		 * skipped.  In this case the syscall return value has to be
		 * set explicitly, otherwise the first syscall argument is
		 * returned as the syscall return value.
		 */
		syscall_set_return_value(task, regs, -ENOSYS, 0);
		return;
	}
	if ((IS_ENABLED(CONFIG_AEABI) && !IS_ENABLED(CONFIG_OABI_COMPAT))) {
		task_thread_info(task)->abi_syscall = nr;
		return;
	}
	task_thread_info(task)->abi_syscall =
		(task_thread_info(task)->abi_syscall & ~__NR_SYSCALL_MASK) |
		(nr & __NR_SYSCALL_MASK);
}

#define SYSCALL_MAX_ARGS 7

static inline void syscall_get_arguments(struct task_struct *task,
					 struct pt_regs *regs,
					 unsigned long *args)
{
	args[0] = regs->ARM_ORIG_r0;
	args++;

	memcpy(args, &regs->ARM_r0 + 1, 5 * sizeof(args[0]));
}

static inline void syscall_set_arguments(struct task_struct *task,
					 struct pt_regs *regs,
					 const unsigned long *args)
{
	memcpy(&regs->ARM_r0, args, 6 * sizeof(args[0]));
	/*
	 * Also copy the first argument into ARM_ORIG_r0
	 * so that syscall_get_arguments() would return it
	 * instead of the previous value.
	 */
	regs->ARM_ORIG_r0 = regs->ARM_r0;
}

static inline int syscall_get_arch(struct task_struct *task)
{
	/* ARM tasks don't change audit architectures on the fly. */
	return AUDIT_ARCH_ARM;
}

#endif /* _ASM_ARM_SYSCALL_H */
